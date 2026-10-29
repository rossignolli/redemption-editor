#include "clip_pack.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include "../external/lzma/LzmaDec.h"
#include "../external/lzma/LzmaEnc.h"
#include "../external/zstd/zstd.h"

namespace replay::clippack
{
    namespace
    {
        // ---------------------------------------------------------------
        // Small helpers
        // ---------------------------------------------------------------
        constexpr std::size_t kClipHeaderBytes = 20;   // magic, format version, duration
        constexpr std::size_t kSectionHeaderBytes = 14; // tag, version, length
        constexpr std::size_t kSectionHashBytes = 8;
        constexpr std::size_t kFileHeaderBytes = 32;
        constexpr std::size_t kCutBytes = 16;
        constexpr std::size_t kBlockEntryBytes = 40;
        constexpr std::uint32_t kMaxBlocks = 1u << 16;
        constexpr std::uint32_t kMaxCuts = 2;

        // POSE records (pose_stream.cpp): u32 size, u32 time ms, u16 flags,
        // u16 entry count, 16 u32 world matrix, then per entry u16 channel,
        // u8 mask and the fields of the mask's bits in bit order.
        constexpr std::size_t kRecordHeader = 76;
        constexpr std::size_t kTrackHead = 32; // record id, bones, config
        constexpr std::size_t kTrackTail = 68; // current layout, stats
        constexpr int kFieldBits = 6;
        constexpr std::size_t kFieldBytes[kFieldBits] = {4, 8, 6, 12, 4, 16};
        constexpr std::uint8_t kKnownMask = 0x3F;
        constexpr std::size_t kChannels = 65536;

        constexpr std::uint32_t Fourcc(const char (&text)[5])
        {
            return static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[0])) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[1])) << 8) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[2])) << 16) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[3])) << 24);
        }
        constexpr std::uint32_t kPoseTag = Fourcc("POSE");
        constexpr std::uint32_t kRopeTag = Fourcc("ROPE");

        template <typename T> T Load(const std::uint8_t* at)
        {
            T value;
            std::memcpy(&value, at, sizeof(T));
            return value;
        }
        template <typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }
        template <typename T> void Put(std::vector<std::uint8_t>& out, T value)
        {
            const std::size_t at = out.size();
            out.resize(at + sizeof(T));
            std::memcpy(out.data() + at, &value, sizeof(T));
        }
        void PutBytes(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t size)
        {
            if (size > 0)
                out.insert(out.end(), data, data + size);
        }

        double MsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        bool Cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_acquire); }

        // 64-bit hash of a block's bytes, eight bytes per step.
        std::uint64_t Hash(const std::uint8_t* data, std::size_t size)
        {
            std::uint64_t hash = 0xCBF29CE484222325ull ^ size;
            std::size_t at = 0;
            for (; at + 8 <= size; at += 8)
            {
                hash = (hash ^ Load<std::uint64_t>(data + at)) * 0x100000001B3ull;
                hash ^= hash >> 29;
            }
            for (; at < size; ++at)
                hash = (hash ^ data[at]) * 0x100000001B3ull;
            return hash ^ (hash >> 32);
        }

        // Bounds checked reader: any read past the end clears ok() and
        // returns zeros.
        class Reader
        {
        public:
            Reader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}
            bool Ok() const { return m_ok; }
            std::size_t At() const { return m_at; }
            std::size_t Left() const { return m_size - m_at; }
            bool AtEnd() const { return m_ok && m_at == m_size; }
            void Fail() { m_ok = false; }
            template <typename T> T Get()
            {
                if (!m_ok || sizeof(T) > Left())
                {
                    m_ok = false;
                    return T{};
                }
                T value = Load<T>(m_data + m_at);
                m_at += sizeof(T);
                return value;
            }
            const std::uint8_t* Take(std::uint64_t bytes)
            {
                if (!m_ok || bytes > Left())
                {
                    m_ok = false;
                    return nullptr;
                }
                const std::uint8_t* at = m_data + m_at;
                m_at += static_cast<std::size_t>(bytes);
                return at;
            }
            // A count of elements of `elementBytes` each that must fit in
            // what is left (elementBytes 0: no check).
            std::uint64_t Count(std::size_t elementBytes)
            {
                const std::uint64_t count = Get<std::uint64_t>();
                if (m_ok && elementBytes > 0 && count > Left() / elementBytes)
                    m_ok = false;
                return m_ok ? count : 0;
            }

        private:
            const std::uint8_t* m_data;
            std::size_t m_size;
            std::size_t m_at = 0;
            bool m_ok = true;
        };

        // A for loop over [0, count) on up to `threads` threads (the
        // caller's included). `body` must not throw.
        template <typename Body> void ParallelFor(std::size_t count, unsigned threads, Body&& body)
        {
            const std::size_t workers = std::min<std::size_t>(threads == 0 ? 1 : threads, count);
            if (workers <= 1)
            {
                for (std::size_t index = 0; index < count; ++index)
                    body(index);
                return;
            }
            std::atomic<std::size_t> next{0};
            const auto work = [&]() {
                for (;;)
                {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= count)
                        break;
                    body(index);
                }
            };
            std::vector<std::thread> pool;
            try
            {
                pool.reserve(workers - 1);
                for (std::size_t n = 1; n < workers; ++n)
                    pool.emplace_back(work);
            }
            catch (...)
            {
                // Fewer threads: the ones started and this one do the work.
            }
            work();
            for (std::thread& thread : pool)
                thread.join();
        }

        unsigned DefaultThreads(unsigned asked)
        {
            if (asked > 0)
                return std::min(asked, 64u);
            const unsigned hardware = std::thread::hardware_concurrency();
            return std::clamp(hardware == 0 ? 1u : hardware, 1u, 12u);
        }

        // The first error of several threads.
        class ErrorSlot
        {
        public:
            void Set(const std::string& error)
            {
                std::lock_guard<std::mutex> guard(m_lock);
                if (!m_failed.load())
                    m_error = error;
                m_failed.store(true);
            }
            bool Failed() const { return m_failed.load(); }
            const std::string& Error() const { return m_error; }

        private:
            std::mutex m_lock;
            std::atomic<bool> m_failed{false};
            std::string m_error;
        };

        // ---------------------------------------------------------------
        // POSE transform
        // ---------------------------------------------------------------
        // The streams of one mask bit, the elements of each and their size.
        struct FieldStream
        {
            std::uint32_t stream;
            std::size_t bytes; // per entry with the bit
        };
        struct FieldBit
        {
            int count;
            FieldStream streams[2];
        };
        constexpr FieldBit kFieldStreams[kFieldBits] = {
            {2, {{kPoseRot32Big, 1}, {kPoseRot32Fields, 6}}},
            {2, {{kPoseRot64Big, 1}, {kPoseRot64Fields, 12}}},
            {1, {{kPoseTrans16, 6}, {0, 0}}},
            {1, {{kPoseTransFloat, 12}, {0, 0}}},
            {1, {{kPoseTransW, 4}, {0, 0}}},
            {1, {{kPoseScale, 16}, {0, 0}}},
        };

        // The previous value of every channel of a track.
        struct ChannelState
        {
            std::vector<std::uint32_t> rot32 = std::vector<std::uint32_t>(kChannels);
            std::vector<std::uint64_t> rot64 = std::vector<std::uint64_t>(kChannels);
            std::vector<std::uint16_t> trans16 = std::vector<std::uint16_t>(kChannels * 3);
            std::vector<std::uint32_t> transFloat = std::vector<std::uint32_t>(kChannels * 3);
            std::vector<std::uint32_t> transW = std::vector<std::uint32_t>(kChannels);
            std::vector<std::uint32_t> scale = std::vector<std::uint32_t>(kChannels * 4);
            std::uint32_t world[16]{};
            std::size_t maxChannel = 0;
            bool any = false;

            void Touch(std::size_t channel)
            {
                maxChannel = std::max(maxChannel, channel);
                any = true;
            }
            void Reset()
            {
                std::memset(world, 0, sizeof(world));
                if (!any)
                    return;
                const std::size_t n = maxChannel + 1;
                std::fill_n(rot32.begin(), n, 0u);
                std::fill_n(rot64.begin(), n, 0ull);
                std::fill_n(trans16.begin(), n * 3, static_cast<std::uint16_t>(0));
                std::fill_n(transFloat.begin(), n * 3, 0u);
                std::fill_n(transW.begin(), n, 0u);
                std::fill_n(scale.begin(), n * 4, 0u);
                maxChannel = 0;
                any = false;
            }
        };

        // rot32: 2 bits of the largest component's index, three 10 bit
        // fields. rot64: 4 bits, three 20 bit fields. A field is predicted
        // by the previous value's field when the index is the same, else by
        // the middle of its range.
        inline void EncodeRot32(std::uint32_t value, std::uint32_t previous, std::uint8_t& big, std::int16_t fields[3])
        {
            const std::uint32_t index = value >> 30, previousIndex = previous >> 30;
            big = static_cast<std::uint8_t>(index ^ previousIndex);
            for (int k = 0; k < 3; ++k)
            {
                const int shift = 20 - 10 * k;
                const int base = index == previousIndex ? static_cast<int>((previous >> shift) & 1023u) : 512;
                fields[k] = static_cast<std::int16_t>(static_cast<int>((value >> shift) & 1023u) - base);
            }
        }
        inline std::uint32_t DecodeRot32(std::uint32_t previous, std::uint8_t big, const std::uint8_t* fields)
        {
            const std::uint32_t previousIndex = previous >> 30;
            const std::uint32_t index = (big ^ previousIndex) & 3u;
            std::uint32_t value = index << 30;
            for (int k = 0; k < 3; ++k)
            {
                const int shift = 20 - 10 * k;
                const int base = index == previousIndex ? static_cast<int>((previous >> shift) & 1023u) : 512;
                const int residual = Load<std::int16_t>(fields + 2 * k);
                value |= (static_cast<std::uint32_t>(residual + base) & 1023u) << shift;
            }
            return value;
        }
        inline void EncodeRot64(std::uint64_t value, std::uint64_t previous, std::uint8_t& big, std::int32_t fields[3])
        {
            const std::uint64_t index = value >> 60, previousIndex = previous >> 60;
            big = static_cast<std::uint8_t>(index ^ previousIndex);
            for (int k = 0; k < 3; ++k)
            {
                const int shift = 40 - 20 * k;
                const std::int64_t base = index == previousIndex ? static_cast<std::int64_t>((previous >> shift) & 0xFFFFFull) : (1 << 19);
                fields[k] = static_cast<std::int32_t>(static_cast<std::int64_t>((value >> shift) & 0xFFFFFull) - base);
            }
        }
        inline std::uint64_t DecodeRot64(std::uint64_t previous, std::uint8_t big, const std::uint8_t* fields)
        {
            const std::uint64_t previousIndex = previous >> 60;
            const std::uint64_t index = (big ^ previousIndex) & 15u;
            std::uint64_t value = index << 60;
            for (int k = 0; k < 3; ++k)
            {
                const int shift = 40 - 20 * k;
                const std::int64_t base = index == previousIndex ? static_cast<std::int64_t>((previous >> shift) & 0xFFFFFull) : (1 << 19);
                const std::int64_t residual = Load<std::int32_t>(fields + 4 * k);
                value |= (static_cast<std::uint64_t>(residual + base) & 0xFFFFFull) << shift;
            }
            return value;
        }

        // Skips one skeleton layout (clip_file.cpp WriteLayout).
        void SkipLayout(Reader& r)
        {
            r.Get<std::uint32_t>();
            r.Get<std::uint8_t>();
            r.Get<std::uint8_t>();
            const std::size_t sizes[5] = {4, 1, 1, 4, 4};
            for (const std::size_t size : sizes)
            {
                const std::uint64_t count = r.Count(size);
                r.Take(count * size);
            }
        }

        struct SplitPart
        {
            const std::uint8_t* bytes = nullptr;
            std::size_t size = 0;
            bool chunk = false;
            std::uint32_t layout = 0;
            std::int32_t anchor = 0;
            const std::uint8_t* times = nullptr;
            std::uint64_t timeCount = 0;
            const std::uint8_t* offsets = nullptr;
            std::uint64_t offsetCount = 0;
        };

        // One track's field residuals in record order, before the channel
        // sort.
        struct TrackFields
        {
            std::vector<std::uint16_t> channels[kFieldBits];
            std::vector<std::uint8_t> data[kFieldBits][2];
            void Clear()
            {
                for (int b = 0; b < kFieldBits; ++b)
                {
                    channels[b].clear();
                    data[b][0].clear();
                    data[b][1].clear();
                }
            }
        };

        // Splits the records of one part (an anchor or a chunk). False when
        // a record is not one the transform knows.
        bool SplitRecords(const SplitPart& part, ChannelState& state, StreamSet& s, TrackFields& fields, std::uint64_t& records,
            bool& derived, std::string& why)
        {
            records = 0;
            derived = part.chunk;
            const std::uint8_t* data = part.bytes;
            const std::size_t size = part.size;
            std::uint32_t previousTime = 0;
            std::size_t at = 0;
            while (at < size)
            {
                if (size - at < kRecordHeader)
                {
                    why = "pose: truncated record";
                    return false;
                }
                const std::uint32_t recordBytes = Load<std::uint32_t>(data + at);
                if (recordBytes < kRecordHeader || recordBytes > size - at)
                {
                    why = "pose: bad record size";
                    return false;
                }
                const std::uint32_t time = Load<std::uint32_t>(data + at + 4);
                const std::uint16_t count = Load<std::uint16_t>(data + at + 10);
                if (derived)
                {
                    if (records >= part.timeCount || records >= part.offsetCount ||
                        Load<std::uint32_t>(part.times + 4 * records) != time ||
                        Load<std::uint32_t>(part.offsets + 4 * records) != at)
                        derived = false;
                }
                Put<std::uint32_t>(s[kPoseTime], time - previousTime);
                previousTime = time;
                PutBytes(s[kPoseFlags], data + at + 8, 2);
                PutBytes(s[kPoseCount], data + at + 10, 2);
                for (int k = 0; k < 16; ++k)
                {
                    const std::uint32_t value = Load<std::uint32_t>(data + at + 12 + 4 * k);
                    Put<std::uint32_t>(s[kPoseWorld], value - state.world[k]);
                    state.world[k] = value;
                }
                const std::size_t end = at + recordBytes;
                std::size_t q = at + kRecordHeader;
                std::uint16_t previousChannel = 0xFFFF;
                for (std::uint32_t e = 0; e < count; ++e)
                {
                    if (end - q < 3)
                    {
                        why = "pose: truncated entry";
                        return false;
                    }
                    const std::uint16_t channel = Load<std::uint16_t>(data + q);
                    const std::uint8_t mask = data[q + 2];
                    if (mask & ~kKnownMask)
                    {
                        why = "pose: unknown mask bits";
                        return false;
                    }
                    q += 3;
                    std::size_t need = 0;
                    for (int b = 0; b < kFieldBits; ++b)
                        if (mask & (1u << b))
                            need += kFieldBytes[b];
                    if (end - q < need)
                    {
                        why = "pose: truncated entry";
                        return false;
                    }
                    Put<std::uint16_t>(s[kPoseChannelMask], static_cast<std::uint16_t>(channel - previousChannel - 1));
                    s[kPoseChannelMask].push_back(mask);
                    previousChannel = channel;
                    state.Touch(channel);
                    if (mask & 1)
                    {
                        const std::uint32_t value = Load<std::uint32_t>(data + q);
                        q += 4;
                        std::uint8_t big;
                        std::int16_t residual[3];
                        EncodeRot32(value, state.rot32[channel], big, residual);
                        state.rot32[channel] = value;
                        fields.channels[0].push_back(channel);
                        fields.data[0][0].push_back(big);
                        PutBytes(fields.data[0][1], reinterpret_cast<const std::uint8_t*>(residual), 6);
                    }
                    if (mask & 2)
                    {
                        const std::uint64_t value = Load<std::uint64_t>(data + q);
                        q += 8;
                        std::uint8_t big;
                        std::int32_t residual[3];
                        EncodeRot64(value, state.rot64[channel], big, residual);
                        state.rot64[channel] = value;
                        fields.channels[1].push_back(channel);
                        fields.data[1][0].push_back(big);
                        PutBytes(fields.data[1][1], reinterpret_cast<const std::uint8_t*>(residual), 12);
                    }
                    if (mask & 4)
                    {
                        fields.channels[2].push_back(channel);
                        for (int k = 0; k < 3; ++k)
                        {
                            const std::uint16_t value = Load<std::uint16_t>(data + q);
                            q += 2;
                            Put<std::uint16_t>(fields.data[2][0], static_cast<std::uint16_t>(value - state.trans16[channel * 3 + k]));
                            state.trans16[channel * 3 + k] = value;
                        }
                    }
                    if (mask & 8)
                    {
                        fields.channels[3].push_back(channel);
                        for (int k = 0; k < 3; ++k)
                        {
                            const std::uint32_t value = Load<std::uint32_t>(data + q);
                            q += 4;
                            Put<std::uint32_t>(fields.data[3][0], value - state.transFloat[channel * 3 + k]);
                            state.transFloat[channel * 3 + k] = value;
                        }
                    }
                    if (mask & 16)
                    {
                        fields.channels[4].push_back(channel);
                        const std::uint32_t value = Load<std::uint32_t>(data + q);
                        q += 4;
                        Put<std::uint32_t>(fields.data[4][0], value - state.transW[channel]);
                        state.transW[channel] = value;
                    }
                    if (mask & 32)
                    {
                        fields.channels[5].push_back(channel);
                        for (int k = 0; k < 4; ++k)
                        {
                            const std::uint32_t value = Load<std::uint32_t>(data + q);
                            q += 4;
                            Put<std::uint32_t>(fields.data[5][0], value - state.scale[channel * 4 + k]);
                            state.scale[channel * 4 + k] = value;
                        }
                    }
                }
                if (q != end)
                {
                    why = "pose: record size does not match its entries";
                    return false;
                }
                at = end;
                ++records;
            }
            if (derived && (part.timeCount != records || part.offsetCount != records))
                derived = false;
            return true;
        }

        // A track's field residuals appended to the streams in channel
        // order (stable: time order within a channel).
        void AppendChannelMajor(TrackFields& fields, StreamSet& s, std::vector<std::uint32_t>& counts)
        {
            for (int b = 0; b < kFieldBits; ++b)
            {
                const std::vector<std::uint16_t>& channels = fields.channels[b];
                const std::size_t n = channels.size();
                if (n == 0)
                    continue;
                std::fill(counts.begin(), counts.end(), 0u);
                for (const std::uint16_t channel : channels)
                    ++counts[channel];
                std::uint32_t sum = 0;
                for (std::size_t c = 0; c < kChannels; ++c)
                {
                    const std::uint32_t count = counts[c];
                    counts[c] = sum;
                    sum += count;
                }
                std::vector<std::uint32_t> position(n);
                for (std::size_t i = 0; i < n; ++i)
                    position[i] = counts[channels[i]]++;
                for (int k = 0; k < kFieldStreams[b].count; ++k)
                {
                    const FieldStream& field = kFieldStreams[b].streams[k];
                    std::vector<std::uint8_t>& out = s[field.stream];
                    const std::size_t base = out.size();
                    out.resize(base + n * field.bytes);
                    const std::uint8_t* from = fields.data[b][k].data();
                    std::uint8_t* to = out.data() + base;
                    for (std::size_t i = 0; i < n; ++i)
                        std::memcpy(to + static_cast<std::size_t>(position[i]) * field.bytes, from + i * field.bytes, field.bytes);
                }
            }
            fields.Clear();
        }

        // ----- POSE join -----
        struct JoinPart
        {
            bool chunk = false;
            std::uint32_t layout = 0;
            std::int32_t anchor = 0;
            std::uint64_t records = 0;
            bool derived = true;
            const std::uint8_t* times = nullptr; // explicit (not derived)
            std::uint64_t timeCount = 0;
            const std::uint8_t* offsets = nullptr;
            std::uint64_t offsetCount = 0;
            // Filled by the plan.
            std::uint64_t recordStart = 0;
            std::uint64_t entryStart = 0;
            std::uint64_t entries = 0;
            std::uint64_t bytes = 0;
            std::uint64_t outRecords = 0;
            std::uint64_t outTimes = 0;   // where a derived frameTimes array goes
            std::uint64_t outOffsets = 0; // and frameOffsets
        };
        struct JoinTrack
        {
            const std::uint8_t* head = nullptr;
            std::uint64_t layoutCount = 0;
            const std::uint8_t* layouts = nullptr;
            std::uint64_t layoutBytes = 0;
            std::uint64_t anchorCount = 0;
            std::uint64_t chunkCount = 0;
            std::vector<JoinPart> parts; // anchors, then chunks
            const std::uint8_t* tail = nullptr;
            std::uint64_t recordStart = 0;
            std::uint64_t records = 0;
            std::uint64_t entryStart = 0;
            std::uint64_t entries = 0;
            std::uint64_t fieldCount[kFieldBits]{};
            std::uint64_t fieldBase[kFieldBits]{};
            std::uint64_t bytes = 0; // its records
        };
        struct PosePlan
        {
            std::vector<JoinTrack> tracks;
            std::uint64_t size = 0;
        };

        bool StreamCountIs(const StreamSet& s, std::uint32_t stream, std::uint64_t count, std::size_t elementBytes)
        {
            return s[stream].size() % elementBytes == 0 && s[stream].size() / elementBytes == count;
        }

        bool PlanPose(const StreamSet& s, unsigned threads, PosePlan& plan, std::string& error)
        {
            plan = PosePlan{};
            const std::vector<std::uint8_t>& meta = s[kPoseMeta];
            Reader r(meta.data(), meta.size());
            const std::uint64_t trackCount = r.Count(kTrackHead + 32 + kTrackTail);
            const std::uint64_t totalRecords = s[kPoseTime].size() / 4;
            plan.tracks.resize(static_cast<std::size_t>(trackCount));
            std::uint64_t recordAt = 0;
            for (JoinTrack& track : plan.tracks)
            {
                track.head = r.Take(kTrackHead);
                track.layoutCount = r.Get<std::uint64_t>();
                track.layoutBytes = r.Count(1);
                track.layouts = r.Take(track.layoutBytes);
                track.anchorCount = r.Count(8);
                track.chunkCount = r.Count(17);
                if (!r.Ok() || track.anchorCount + track.chunkCount > r.Left() / 8)
                {
                    error = "packed clip: pose meta damaged";
                    return false;
                }
                track.parts.resize(static_cast<std::size_t>(track.anchorCount + track.chunkCount));
                track.recordStart = recordAt;
                for (std::size_t n = 0; n < track.parts.size() && r.Ok(); ++n)
                {
                    JoinPart& part = track.parts[n];
                    part.chunk = n >= track.anchorCount;
                    if (part.chunk)
                    {
                        part.layout = r.Get<std::uint32_t>();
                        part.anchor = r.Get<std::int32_t>();
                    }
                    part.records = r.Get<std::uint64_t>();
                    if (part.records > totalRecords - recordAt)
                        r.Fail();
                    if (part.chunk)
                    {
                        const std::uint8_t flag = r.Get<std::uint8_t>();
                        if (flag > 1)
                            r.Fail();
                        part.derived = flag == 0;
                        if (!part.derived)
                        {
                            part.timeCount = r.Count(4);
                            part.times = r.Take(part.timeCount * 4);
                            part.offsetCount = r.Count(4);
                            part.offsets = r.Take(part.offsetCount * 4);
                        }
                    }
                    part.recordStart = recordAt;
                    recordAt += part.records;
                }
                track.records = recordAt - track.recordStart;
                track.tail = r.Take(kTrackTail);
                if (!r.Ok())
                {
                    error = "packed clip: pose meta damaged";
                    return false;
                }
            }
            if (!r.AtEnd() || recordAt != totalRecords || !StreamCountIs(s, kPoseTime, totalRecords, 4) ||
                !StreamCountIs(s, kPoseFlags, totalRecords, 2) || !StreamCountIs(s, kPoseCount, totalRecords, 2) ||
                !StreamCountIs(s, kPoseWorld, totalRecords, 64))
            {
                error = "packed clip: pose record streams do not match";
                return false;
            }
            // Entry starts (sequential over the entry counts).
            const std::uint8_t* counts = s[kPoseCount].data();
            std::uint64_t entryAt = 0;
            for (JoinTrack& track : plan.tracks)
            {
                track.entryStart = entryAt;
                for (JoinPart& part : track.parts)
                {
                    part.entryStart = entryAt;
                    for (std::uint64_t n = 0; n < part.records; ++n)
                        entryAt += Load<std::uint16_t>(counts + 2 * (part.recordStart + n));
                    part.entries = entryAt - part.entryStart;
                }
                track.entries = entryAt - track.entryStart;
            }
            if (!StreamCountIs(s, kPoseChannelMask, entryAt, 3))
            {
                error = "packed clip: pose entry stream does not match";
                return false;
            }
            // Per track: the bytes of each part and the entries of each bit.
            const std::uint8_t* masks = s[kPoseChannelMask].data();
            std::atomic<bool> badMask{false};
            ParallelFor(plan.tracks.size(), threads, [&](std::size_t index) {
                JoinTrack& track = plan.tracks[index];
                std::uint64_t bits[kFieldBits]{};
                std::uint64_t trackBytes = 0;
                for (JoinPart& part : track.parts)
                {
                    std::uint64_t bytes = part.records * kRecordHeader + part.entries * 3;
                    for (std::uint64_t e = part.entryStart; e < part.entryStart + part.entries; ++e)
                    {
                        const std::uint8_t mask = masks[3 * e + 2];
                        if (mask & ~kKnownMask)
                        {
                            badMask.store(true);
                            return;
                        }
                        for (int b = 0; b < kFieldBits; ++b)
                        {
                            if (mask & (1u << b))
                            {
                                ++bits[b];
                                bytes += kFieldBytes[b];
                            }
                        }
                    }
                    part.bytes = bytes;
                    trackBytes += bytes;
                }
                for (int b = 0; b < kFieldBits; ++b)
                    track.fieldCount[b] = bits[b];
                track.bytes = trackBytes;
            });
            if (badMask.load())
            {
                error = "packed clip: pose mask damaged";
                return false;
            }
            // Field bases and checks.
            std::uint64_t fieldAt[kFieldBits]{};
            for (JoinTrack& track : plan.tracks)
            {
                for (int b = 0; b < kFieldBits; ++b)
                {
                    track.fieldBase[b] = fieldAt[b];
                    fieldAt[b] += track.fieldCount[b];
                }
            }
            for (int b = 0; b < kFieldBits; ++b)
            {
                for (int k = 0; k < kFieldStreams[b].count; ++k)
                {
                    const FieldStream& field = kFieldStreams[b].streams[k];
                    if (!StreamCountIs(s, field.stream, fieldAt[b], field.bytes))
                    {
                        error = std::string("packed clip: pose stream ") + StreamName(field.stream) + " does not match";
                        return false;
                    }
                }
            }
            // The payload's layout (the same walk as WritePoseFrame).
            std::uint64_t at = 8;
            for (JoinTrack& track : plan.tracks)
            {
                at += kTrackHead + 8 + track.layoutBytes + 8;
                for (std::size_t n = 0; n < track.parts.size(); ++n)
                {
                    JoinPart& part = track.parts[n];
                    if (n == track.anchorCount)
                        at += 8; // the chunk count
                    if (part.chunk)
                        at += 8;
                    at += 8;
                    part.outRecords = at;
                    at += part.bytes;
                    if (part.chunk)
                    {
                        const std::uint64_t times = part.derived ? part.records : part.timeCount;
                        const std::uint64_t offsets = part.derived ? part.records : part.offsetCount;
                        at += 8;
                        part.outTimes = at;
                        at += 4 * times + 8;
                        part.outOffsets = at;
                        at += 4 * offsets;
                    }
                }
                if (track.chunkCount == 0)
                    at += 8;
                at += kTrackTail;
            }
            plan.size = at;
            return true;
        }

        // The parts of the payload that come from the meta (sequential).
        void WritePoseFrame(const PosePlan& plan, std::uint8_t* out)
        {
            Store<std::uint64_t>(out, plan.tracks.size());
            std::uint64_t at = 8;
            for (const JoinTrack& track : plan.tracks)
            {
                std::memcpy(out + at, track.head, kTrackHead);
                at += kTrackHead;
                Store<std::uint64_t>(out + at, track.layoutCount);
                at += 8;
                if (track.layoutBytes)
                    std::memcpy(out + at, track.layouts, static_cast<std::size_t>(track.layoutBytes));
                at += track.layoutBytes;
                Store<std::uint64_t>(out + at, track.anchorCount);
                at += 8;
                for (std::size_t n = 0; n < track.parts.size(); ++n)
                {
                    const JoinPart& part = track.parts[n];
                    if (n == track.anchorCount)
                    {
                        Store<std::uint64_t>(out + at, track.chunkCount);
                        at += 8;
                    }
                    if (part.chunk)
                    {
                        Store<std::uint32_t>(out + at, part.layout);
                        Store<std::int32_t>(out + at + 4, part.anchor);
                        at += 8;
                    }
                    Store<std::uint64_t>(out + at, part.bytes);
                    at += 8 + part.bytes;
                    if (part.chunk)
                    {
                        const std::uint64_t times = part.derived ? part.records : part.timeCount;
                        const std::uint64_t offsets = part.derived ? part.records : part.offsetCount;
                        Store<std::uint64_t>(out + at, times);
                        at += 8;
                        if (!part.derived && times)
                            std::memcpy(out + at, part.times, static_cast<std::size_t>(4 * times));
                        at += 4 * times;
                        Store<std::uint64_t>(out + at, offsets);
                        at += 8;
                        if (!part.derived && offsets)
                            std::memcpy(out + at, part.offsets, static_cast<std::size_t>(4 * offsets));
                        at += 4 * offsets;
                    }
                }
                if (track.chunkCount == 0)
                {
                    Store<std::uint64_t>(out + at, 0);
                    at += 8;
                }
                std::memcpy(out + at, track.tail, kTrackTail);
                at += kTrackTail;
            }
        }

        // Per thread: the channel cursors of each bit and the channel state.
        struct JoinScratch
        {
            std::vector<std::uint64_t> cursor = std::vector<std::uint64_t>(kFieldBits * kChannels);
            ChannelState state;
        };

        // One track's records rebuilt into `out` (fused: a counting pass
        // over its entries gives each channel's cursor into the
        // channel-major field streams, then one pass writes the records).
        void WriteTrackRecords(const JoinTrack& track, const StreamSet& s, std::uint8_t* out, JoinScratch& scratch)
        {
            const std::uint8_t* chmask = s[kPoseChannelMask].data();
            const std::uint8_t* times = s[kPoseTime].data();
            const std::uint8_t* flags = s[kPoseFlags].data();
            const std::uint8_t* counts = s[kPoseCount].data();
            const std::uint8_t* world = s[kPoseWorld].data();
            const std::uint8_t* r32big = s[kPoseRot32Big].data();
            const std::uint8_t* r32f = s[kPoseRot32Fields].data();
            const std::uint8_t* r64big = s[kPoseRot64Big].data();
            const std::uint8_t* r64f = s[kPoseRot64Fields].data();
            const std::uint8_t* t16 = s[kPoseTrans16].data();
            const std::uint8_t* tf = s[kPoseTransFloat].data();
            const std::uint8_t* tw = s[kPoseTransW].data();
            const std::uint8_t* sc = s[kPoseScale].data();
            std::uint64_t* cursor = scratch.cursor.data();
            ChannelState& state = scratch.state;

            // Counting pass.
            std::size_t maxChannel = 0;
            bool any = false;
            {
                std::uint64_t e = track.entryStart;
                for (std::uint64_t r = track.recordStart; r < track.recordStart + track.records; ++r)
                {
                    const std::uint32_t count = Load<std::uint16_t>(counts + 2 * r);
                    std::uint16_t channel = 0xFFFF;
                    for (std::uint32_t n = 0; n < count; ++n, ++e)
                    {
                        channel = static_cast<std::uint16_t>(Load<std::uint16_t>(chmask + 3 * e) + channel + 1);
                        const std::uint8_t mask = chmask[3 * e + 2];
                        for (int b = 0; b < kFieldBits; ++b)
                            if (mask & (1u << b))
                                ++cursor[b * kChannels + channel];
                        maxChannel = std::max<std::size_t>(maxChannel, channel);
                        any = true;
                    }
                }
            }
            if (any)
            {
                for (int b = 0; b < kFieldBits; ++b)
                {
                    std::uint64_t sum = track.fieldBase[b];
                    std::uint64_t* row = cursor + b * kChannels;
                    for (std::size_t c = 0; c <= maxChannel; ++c)
                    {
                        const std::uint64_t count = row[c];
                        row[c] = sum;
                        sum += count;
                    }
                }
            }
            // Rebuild pass.
            state.Reset();
            std::uint64_t r = track.recordStart;
            std::uint64_t e = track.entryStart;
            for (const JoinPart& part : track.parts)
            {
                std::uint8_t* base = out + part.outRecords;
                std::uint64_t at = 0;
                std::uint32_t previousTime = 0;
                for (std::uint64_t n = 0; n < part.records; ++n, ++r)
                {
                    std::uint8_t* record = base + at;
                    const std::uint32_t time = previousTime + Load<std::uint32_t>(times + 4 * r);
                    previousTime = time;
                    const std::uint16_t count = Load<std::uint16_t>(counts + 2 * r);
                    Store<std::uint32_t>(record + 4, time);
                    std::memcpy(record + 8, flags + 2 * r, 2);
                    Store<std::uint16_t>(record + 10, count);
                    for (int k = 0; k < 16; ++k)
                    {
                        const std::uint32_t value = state.world[k] + Load<std::uint32_t>(world + 64 * r + 4 * k);
                        state.world[k] = value;
                        Store<std::uint32_t>(record + 12 + 4 * k, value);
                    }
                    std::uint8_t* q = record + kRecordHeader;
                    std::uint16_t channel = 0xFFFF;
                    for (std::uint32_t c = 0; c < count; ++c, ++e)
                    {
                        channel = static_cast<std::uint16_t>(Load<std::uint16_t>(chmask + 3 * e) + channel + 1);
                        const std::uint8_t mask = chmask[3 * e + 2];
                        Store<std::uint16_t>(q, channel);
                        q[2] = mask;
                        q += 3;
                        state.Touch(channel);
                        if (mask & 1)
                        {
                            const std::uint64_t i = cursor[0 * kChannels + channel]++;
                            const std::uint32_t value = DecodeRot32(state.rot32[channel], r32big[i], r32f + 6 * i);
                            state.rot32[channel] = value;
                            Store<std::uint32_t>(q, value);
                            q += 4;
                        }
                        if (mask & 2)
                        {
                            const std::uint64_t i = cursor[1 * kChannels + channel]++;
                            const std::uint64_t value = DecodeRot64(state.rot64[channel], r64big[i], r64f + 12 * i);
                            state.rot64[channel] = value;
                            Store<std::uint64_t>(q, value);
                            q += 8;
                        }
                        if (mask & 4)
                        {
                            const std::uint64_t i = cursor[2 * kChannels + channel]++;
                            for (int k = 0; k < 3; ++k)
                            {
                                const std::uint16_t value = static_cast<std::uint16_t>(
                                    state.trans16[channel * 3 + k] + Load<std::uint16_t>(t16 + 6 * i + 2 * k));
                                state.trans16[channel * 3 + k] = value;
                                Store<std::uint16_t>(q, value);
                                q += 2;
                            }
                        }
                        if (mask & 8)
                        {
                            const std::uint64_t i = cursor[3 * kChannels + channel]++;
                            for (int k = 0; k < 3; ++k)
                            {
                                const std::uint32_t value = state.transFloat[channel * 3 + k] + Load<std::uint32_t>(tf + 12 * i + 4 * k);
                                state.transFloat[channel * 3 + k] = value;
                                Store<std::uint32_t>(q, value);
                                q += 4;
                            }
                        }
                        if (mask & 16)
                        {
                            const std::uint64_t i = cursor[4 * kChannels + channel]++;
                            const std::uint32_t value = state.transW[channel] + Load<std::uint32_t>(tw + 4 * i);
                            state.transW[channel] = value;
                            Store<std::uint32_t>(q, value);
                            q += 4;
                        }
                        if (mask & 32)
                        {
                            const std::uint64_t i = cursor[5 * kChannels + channel]++;
                            for (int k = 0; k < 4; ++k)
                            {
                                const std::uint32_t value = state.scale[channel * 4 + k] + Load<std::uint32_t>(sc + 16 * i + 4 * k);
                                state.scale[channel * 4 + k] = value;
                                Store<std::uint32_t>(q, value);
                                q += 4;
                            }
                        }
                    }
                    const std::uint32_t recordBytes = static_cast<std::uint32_t>(q - record);
                    Store<std::uint32_t>(record, recordBytes);
                    if (part.chunk && part.derived)
                    {
                        Store<std::uint32_t>(out + part.outTimes + 4 * n, time);
                        Store<std::uint32_t>(out + part.outOffsets + 4 * n, static_cast<std::uint32_t>(at));
                    }
                    at += recordBytes;
                }
            }
            if (any)
            {
                for (int b = 0; b < kFieldBits; ++b)
                    std::fill_n(cursor + b * kChannels, maxChannel + 1, 0ull);
            }
        }

        bool WritePose(const PosePlan& plan, const StreamSet& s, unsigned threads, std::uint8_t* out, std::string& error)
        {
            WritePoseFrame(plan, out);
            // Largest tracks first, so one large track does not end last alone.
            std::vector<std::size_t> order(plan.tracks.size());
            for (std::size_t n = 0; n < order.size(); ++n)
                order[n] = n;
            std::sort(order.begin(), order.end(),
                [&](std::size_t a, std::size_t b) { return plan.tracks[a].bytes > plan.tracks[b].bytes; });
            const unsigned workers = static_cast<unsigned>(std::min<std::size_t>(std::max(1u, threads), std::max<std::size_t>(1, order.size())));
            std::vector<std::unique_ptr<JoinScratch>> scratch;
            try
            {
                for (unsigned n = 0; n < workers; ++n)
                    scratch.push_back(std::make_unique<JoinScratch>());
            }
            catch (const std::bad_alloc&)
            {
                error = "packed clip: out of memory";
                return false;
            }
            // Each worker takes its own scratch by a thread slot.
            std::atomic<unsigned> slots{0};
            std::atomic<std::size_t> next{0};
            const auto work = [&]() {
                JoinScratch& mine = *scratch[slots.fetch_add(1)];
                for (;;)
                {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= order.size())
                        break;
                    WriteTrackRecords(plan.tracks[order[index]], s, out, mine);
                }
            };
            std::vector<std::thread> pool;
            try
            {
                for (unsigned n = 1; n < workers; ++n)
                    pool.emplace_back(work);
            }
            catch (...)
            {
                // Fewer threads: the ones started and this one do the work.
            }
            work();
            for (std::thread& thread : pool)
                thread.join();
            return true;
        }

        // ---------------------------------------------------------------
        // ROPE transform
        // ---------------------------------------------------------------
        // A rope record up to its times (clip_file.cpp WriteRopes): engine id,
        // type, vertices, attachments (id, bone), near ped, two slot event
        // lists (17 bytes each), held events (5 bytes each).
        bool SkipRopeHead(Reader& r, std::uint32_t& vertices)
        {
            r.Get<std::uint32_t>();
            r.Get<std::uint32_t>();
            vertices = r.Get<std::uint32_t>();
            const std::uint64_t attachments = r.Count(8);
            r.Take(attachments * 8);
            r.Get<std::uint32_t>();
            for (int n = 0; n < 2; ++n)
            {
                const std::uint64_t events = r.Count(17);
                r.Take(events * 17);
            }
            const std::uint64_t held = r.Count(5);
            r.Take(held * 5);
            return r.Ok();
        }

        bool RopeChannelMajor(std::uint32_t vertices, std::uint64_t times, std::uint64_t positions)
        {
            const std::uint64_t stride = static_cast<std::uint64_t>(vertices) * 3u;
            return stride > 0 && positions % stride == 0 && positions / stride == times;
        }

        // ---------------------------------------------------------------
        // Blocks
        // ---------------------------------------------------------------
        struct StreamCoding
        {
            std::uint8_t lzmaVariant; // index into kLzmaVariants
            std::uint8_t shuffle;     // byte shuffle width (0 none)
        };
        // Tuned on a busy town clip (Blackwater), measured per stream.
        constexpr StreamCoding kCoding[kStreamCount] = {
            {0, 0}, // rest
            {0, 0}, // pose meta
            {3, 0}, // time
            {1, 2}, // flags
            {4, 2}, // count
            {3, 0}, // world
            {4, 0}, // channel and mask
            {1, 0}, // rot32 index
            {2, 0}, // rot32 fields
            {1, 0}, // rot64 index
            {1, 4}, // rot64 fields
            {3, 0}, // trans16
            {3, 0}, // trans float
            {3, 0}, // trans w
            {3, 0}, // scale
            {0, 0}, // rope meta
            {3, 0}, // rope times
            {3, 0}, // rope positions
        };
        struct LzmaVariant
        {
            int lc, lp, pb;
        };
        constexpr LzmaVariant kLzmaVariants[] = {{3, 0, 2}, {0, 0, 0}, {0, 1, 1}, {1, 2, 2}, {4, 0, 0}};

        void* SdkAlloc(ISzAllocPtr, size_t size) { return size ? std::malloc(size) : nullptr; }
        void SdkFree(ISzAllocPtr, void* address) { std::free(address); }
        const ISzAlloc kSdkAlloc = {SdkAlloc, SdkFree};

        struct CancelProgress
        {
            ICompressProgress vt;
            const std::atomic<bool>* cancel;
        };
        SRes ProgressCheck(ICompressProgressPtr p, UInt64, UInt64)
        {
            const CancelProgress* self = reinterpret_cast<const CancelProgress*>(p);
            return Cancelled(self->cancel) ? SZ_ERROR_PROGRESS : SZ_OK;
        }

        void Shuffle(const std::uint8_t* in, std::size_t size, std::size_t width, std::uint8_t* out)
        {
            const std::size_t n = size / width;
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t k = 0; k < width; ++k)
                    out[k * n + i] = in[i * width + k];
        }
        void Unshuffle(const std::uint8_t* in, std::size_t size, std::size_t width, std::uint8_t* out)
        {
            const std::size_t n = size / width;
            for (std::size_t k = 0; k < width; ++k)
                for (std::size_t i = 0; i < n; ++i)
                    out[i * width + k] = in[k * n + i];
        }

        struct Block
        {
            std::uint32_t stream = 0;
            std::uint64_t offset = 0; // in the stream
            std::uint64_t rawBytes = 0;
            Codec codec = Codec::Stored;
            std::uint8_t shuffle = 0;
            std::uint8_t props[5]{};
            std::vector<std::uint8_t> packed;
            std::uint64_t packedBytes = 0;
            std::uint64_t hash = 0;
            std::uint64_t fileOffset = 0; // unpack: where its bytes are
        };

        // One block compressed (stored when that is not smaller). False
        // only when cancelled or out of memory.
        bool CompressBlock(const std::uint8_t* raw, Block& block, const PackOptions& options, std::string& error)
        {
            const std::size_t size = static_cast<std::size_t>(block.rawBytes);
            const StreamCoding coding = kCoding[block.stream];
            std::vector<std::uint8_t> shuffled;
            const std::uint8_t* input = raw;
            block.shuffle = 0;
            if (coding.shuffle > 1 && size % coding.shuffle == 0 && size >= 64)
            {
                shuffled.resize(size);
                Shuffle(raw, size, coding.shuffle, shuffled.data());
                input = shuffled.data();
                block.shuffle = coding.shuffle;
            }
            block.codec = Codec::Stored;
            if (size >= 64 && options.codec == Codec::Lzma)
            {
                CLzmaEncProps props;
                LzmaEncProps_Init(&props);
                props.level = std::clamp(options.lzmaLevel, 1, 9);
                const LzmaVariant variant = kLzmaVariants[coding.lzmaVariant];
                props.lc = variant.lc;
                props.lp = variant.lp;
                props.pb = variant.pb;
                props.reduceSize = size;
                props.numThreads = 1;
                if (props.level >= 9)
                    props.fb = 273;
                std::vector<std::uint8_t> out(size + size / 64 + 256);
                SizeT outBytes = out.size();
                SizeT propsBytes = LZMA_PROPS_SIZE;
                CancelProgress progress{{ProgressCheck}, options.cancel};
                const SRes result = LzmaEncode(out.data(), &outBytes, input, size, &props, block.props, &propsBytes, 0,
                    &progress.vt, &kSdkAlloc, &kSdkAlloc);
                if (result == SZ_ERROR_PROGRESS || Cancelled(options.cancel))
                {
                    error = "cancelled";
                    return false;
                }
                if (result == SZ_ERROR_MEM)
                {
                    error = "lzma: out of memory";
                    return false;
                }
                if (result == SZ_OK && propsBytes == LZMA_PROPS_SIZE && outBytes < size)
                {
                    out.resize(outBytes);
                    out.shrink_to_fit();
                    block.packed.swap(out);
                    block.codec = Codec::Lzma;
                }
            }
            else if (size >= 64 && options.codec == Codec::Zstd)
            {
                std::vector<std::uint8_t> out(ZSTD_compressBound(size));
                const std::size_t result = ZSTD_compress(out.data(), out.size(), input, size, std::clamp(options.zstdLevel, 1, ZSTD_maxCLevel()));
                if (!ZSTD_isError(result) && result < size)
                {
                    out.resize(result);
                    out.shrink_to_fit();
                    block.packed.swap(out);
                    block.codec = Codec::Zstd;
                }
            }
            if (block.codec == Codec::Stored)
            {
                block.packed.assign(input, input + size);
                std::memset(block.props, 0, sizeof(block.props));
            }
            block.packedBytes = block.packed.size();
            block.hash = Hash(block.packed.data(), block.packed.size());
            return true;
        }

        bool DecompressBlock(const std::uint8_t* packed, const Block& block, std::uint8_t* out, std::vector<std::uint8_t>& scratch,
            std::string& error)
        {
            const std::size_t size = static_cast<std::size_t>(block.rawBytes);
            if (Hash(packed, static_cast<std::size_t>(block.packedBytes)) != block.hash)
            {
                error = std::string("packed clip: block of ") + StreamName(block.stream) + " damaged (hash)";
                return false;
            }
            std::uint8_t* target = out;
            if (block.shuffle > 1)
            {
                scratch.resize(size);
                target = scratch.data();
            }
            if (block.codec == Codec::Stored)
            {
                if (block.packedBytes != size)
                {
                    error = "packed clip: stored block size";
                    return false;
                }
                std::memcpy(target, packed, size);
            }
            else if (block.codec == Codec::Lzma)
            {
                SizeT outBytes = size;
                SizeT inBytes = static_cast<SizeT>(block.packedBytes);
                ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
                const SRes result = LzmaDecode(target, &outBytes, packed, &inBytes, block.props, LZMA_PROPS_SIZE, LZMA_FINISH_END,
                    &status, &kSdkAlloc);
                if (result != SZ_OK || outBytes != size ||
                    (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
                {
                    error = std::string("packed clip: block of ") + StreamName(block.stream) + " does not decompress (lzma " +
                        std::to_string(result) + ")";
                    return false;
                }
            }
            else if (block.codec == Codec::Zstd)
            {
                const std::size_t result = ZSTD_decompress(target, size, packed, static_cast<std::size_t>(block.packedBytes));
                if (ZSTD_isError(result) || result != size)
                {
                    error = std::string("packed clip: block of ") + StreamName(block.stream) + " does not decompress (zstd)";
                    return false;
                }
            }
            else
            {
                error = "packed clip: unknown codec";
                return false;
            }
            if (block.shuffle > 1)
                Unshuffle(scratch.data(), size, block.shuffle, out);
            return true;
        }

        // ---------------------------------------------------------------
        // Pack
        // ---------------------------------------------------------------
        struct Cut
        {
            std::uint32_t tag = 0;
            std::uint64_t offset = 0; // of the payload in the clip
            std::uint64_t bytes = 0;
        };

        // The sections of a plain clip; false when the framing is not the
        // one this code knows (then the whole clip goes into rest).
        struct SectionRef
        {
            std::uint32_t tag;
            std::uint16_t version;
            std::size_t payload;
            std::uint64_t bytes;
        };
        bool Sections(const std::uint8_t* clip, std::size_t size, std::vector<SectionRef>& out)
        {
            out.clear();
            if (size < kClipHeaderBytes || Load<std::uint32_t>(clip + 8) != kClipFormatVersion)
                return false;
            std::size_t at = kClipHeaderBytes;
            while (at < size)
            {
                if (size - at < kSectionHeaderBytes)
                    return false;
                const std::uint64_t bytes = Load<std::uint64_t>(clip + at + 6);
                if (bytes > size - at - kSectionHeaderBytes || size - at - kSectionHeaderBytes - bytes < kSectionHashBytes)
                    return false;
                out.push_back({Load<std::uint32_t>(clip + at), Load<std::uint16_t>(clip + at + 4), at + kSectionHeaderBytes, bytes});
                at += kSectionHeaderBytes + static_cast<std::size_t>(bytes) + kSectionHashBytes;
            }
            return true;
        }

        bool PackImpl(const std::uint8_t* clip, std::size_t size, std::vector<std::uint8_t>* owned, const PackOptions& options,
            std::vector<std::uint8_t>& out, std::string& error, PackStats* stats)
        {
            out.clear();
            PackStats local;
            PackStats& st = stats ? *stats : local;
            st = PackStats{};
            st.clipBytes = size;
            if (!clip || size < kClipHeaderBytes || std::memcmp(clip, kClipMagic, sizeof(kClipMagic)) != 0)
            {
                error = "pack: not a plain clip";
                return false;
            }
            const auto transformStart = std::chrono::steady_clock::now();
            StreamSet streams(kStreamCount);
            std::vector<Cut> cuts;
            std::vector<SectionRef> sections;
            if (options.transform && Sections(clip, size, sections))
            {
                for (const SectionRef& section : sections)
                {
                    const bool pose = section.tag == kPoseTag;
                    const bool rope = section.tag == kRopeTag;
                    if ((!pose && !rope) || (pose && st.pose) || (rope && st.rope))
                        continue;
                    std::string& skip = pose ? st.poseSkip : st.ropeSkip;
                    if (section.version != (pose ? kPoseSectionVersion : kRopeSectionVersion))
                    {
                        skip = "section version " + std::to_string(section.version);
                        continue;
                    }
                    StreamSet part(kStreamCount);
                    std::string why;
                    const std::size_t bytes = static_cast<std::size_t>(section.bytes);
                    const bool ok = pose ? SplitPose(clip + section.payload, bytes, std::max(1u, options.threads), part, why)
                                         : SplitRope(clip + section.payload, bytes, part, why);
                    if (!ok)
                    {
                        skip = why;
                        continue;
                    }
                    for (std::uint32_t id = 0; id < kStreamCount; ++id)
                        if (!part[id].empty())
                            streams[id].swap(part[id]);
                    (pose ? st.pose : st.rope) = true;
                    cuts.push_back({section.tag, section.payload, section.bytes});
                }
            }
            std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.offset < b.offset; });
            // Rest: the clip without the cut payloads.
            {
                std::uint64_t cutBytes = 0;
                for (const Cut& cut : cuts)
                    cutBytes += cut.bytes;
                std::vector<std::uint8_t>& rest = streams[kRest];
                rest.reserve(static_cast<std::size_t>(size - cutBytes));
                std::size_t at = 0;
                for (const Cut& cut : cuts)
                {
                    rest.insert(rest.end(), clip + at, clip + cut.offset);
                    at = static_cast<std::size_t>(cut.offset + cut.bytes);
                }
                rest.insert(rest.end(), clip + at, clip + size);
            }
            if (owned)
            {
                owned->clear();
                owned->shrink_to_fit();
                clip = nullptr;
            }
            st.transformMs = MsSince(transformStart);
            if (Cancelled(options.cancel))
            {
                error = "cancelled";
                return false;
            }

            // Blocks.
            const auto compressStart = std::chrono::steady_clock::now();
            const std::size_t blockBytes = std::clamp<std::size_t>(options.blockBytes & ~static_cast<std::size_t>(15), 4096, kMaxBlockBytes);
            std::vector<Block> blocks;
            std::vector<std::uint32_t> remaining(kStreamCount, 0);
            for (std::uint32_t id = 0; id < kStreamCount; ++id)
            {
                const std::uint64_t bytes = streams[id].size();
                for (std::uint64_t at = 0; at < bytes; at += blockBytes)
                {
                    Block block;
                    block.stream = id;
                    block.offset = at;
                    block.rawBytes = std::min<std::uint64_t>(blockBytes, bytes - at);
                    blocks.push_back(std::move(block));
                    ++remaining[id];
                }
            }
            if (blocks.size() > kMaxBlocks)
            {
                error = "pack: too many blocks";
                return false;
            }
            std::vector<std::atomic<std::uint32_t>> left(kStreamCount);
            for (std::uint32_t id = 0; id < kStreamCount; ++id)
                left[id].store(remaining[id]);
            // Largest first (the order in the file stays the table order).
            std::vector<std::size_t> order(blocks.size());
            for (std::size_t n = 0; n < order.size(); ++n)
                order[n] = n;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return blocks[a].rawBytes > blocks[b].rawBytes; });
            ErrorSlot failure;
            ParallelFor(order.size(), std::max(1u, options.threads), [&](std::size_t index) {
                if (failure.Failed())
                    return;
                Block& block = blocks[order[index]];
                try
                {
                    std::string blockError;
                    if (!CompressBlock(streams[block.stream].data() + block.offset, block, options, blockError))
                    {
                        failure.Set(blockError);
                        return;
                    }
                    // The stream's memory goes back once its last block is done.
                    if (left[block.stream].fetch_sub(1) == 1)
                    {
                        streams[block.stream].clear();
                        streams[block.stream].shrink_to_fit();
                    }
                }
                catch (const std::bad_alloc&)
                {
                    failure.Set("pack: out of memory");
                }
                catch (...)
                {
                    failure.Set("pack: exception");
                }
            });
            if (failure.Failed())
            {
                error = failure.Error();
                return false;
            }
            streams.clear();
            st.compressMs = MsSince(compressStart);

            // The file.
            const std::size_t headerBytes = kFileHeaderBytes + cuts.size() * kCutBytes + blocks.size() * kBlockEntryBytes + 8;
            std::uint64_t total = headerBytes;
            for (const Block& block : blocks)
                total += block.packedBytes;
            out.resize(static_cast<std::size_t>(total));
            std::uint8_t* at = out.data();
            std::memcpy(at, kMagic, sizeof(kMagic));
            Store<std::uint32_t>(at + 8, kVersion);
            Store<std::uint32_t>(at + 12, static_cast<std::uint32_t>(blocks.size()));
            Store<std::uint64_t>(at + 16, size);
            Store<std::uint32_t>(at + 24, static_cast<std::uint32_t>(cuts.size()));
            Store<std::uint32_t>(at + 28, 0);
            at += kFileHeaderBytes;
            for (const Cut& cut : cuts)
            {
                Store<std::uint32_t>(at, cut.tag);
                Store<std::uint32_t>(at + 4, 0);
                Store<std::uint64_t>(at + 8, cut.offset);
                at += kCutBytes;
            }
            for (const Block& block : blocks)
            {
                Store<std::uint32_t>(at, block.stream);
                at[4] = static_cast<std::uint8_t>(block.codec);
                at[5] = block.shuffle;
                std::memcpy(at + 6, block.props, 5);
                at[11] = 0;
                Store<std::uint64_t>(at + 12, block.rawBytes);
                Store<std::uint64_t>(at + 20, block.packedBytes);
                Store<std::uint64_t>(at + 28, block.hash);
                Store<std::uint32_t>(at + 36, 0);
                at += kBlockEntryBytes;
            }
            Store<std::uint64_t>(at, Hash(out.data(), static_cast<std::size_t>(at - out.data())));
            at += 8;
            st.streams.assign(kStreamCount, StreamStat{});
            for (std::uint32_t id = 0; id < kStreamCount; ++id)
                st.streams[id].stream = id;
            for (Block& block : blocks)
            {
                if (block.packedBytes)
                    std::memcpy(at, block.packed.data(), static_cast<std::size_t>(block.packedBytes));
                at += block.packedBytes;
                StreamStat& stat = st.streams[block.stream];
                ++stat.blocks;
                stat.rawBytes += block.rawBytes;
                stat.packedBytes += block.packedBytes;
                std::vector<std::uint8_t>().swap(block.packed);
            }
            st.packedBytes = out.size();
            error.clear();
            return true;
        }

        // ---------------------------------------------------------------
        // Unpack
        // ---------------------------------------------------------------
        struct Header
        {
            std::uint64_t clipBytes = 0;
            std::vector<Cut> cuts;
            std::vector<Block> blocks;
            std::uint64_t streamBytes[kStreamCount]{};
        };

        bool ReadHeader(const std::uint8_t* data, std::size_t size, std::uint64_t maxClipBytes, Header& header, std::string& error)
        {
            const auto fail = [&](const char* why) {
                error = std::string("packed clip: ") + why;
                return false;
            };
            if (!IsPacked(data, size) || size < kFileHeaderBytes + 8)
                return fail("truncated header");
            if (Load<std::uint32_t>(data + 8) != kVersion)
                return fail(("container version " + std::to_string(Load<std::uint32_t>(data + 8)) + " not read by this build").c_str());
            const std::uint32_t blockCount = Load<std::uint32_t>(data + 12);
            header.clipBytes = Load<std::uint64_t>(data + 16);
            const std::uint32_t cutCount = Load<std::uint32_t>(data + 24);
            if (blockCount > kMaxBlocks || cutCount > kMaxCuts)
                return fail("bad header");
            if (header.clipBytes < kClipHeaderBytes || header.clipBytes > maxClipBytes)
                return fail("clip size out of range");
            const std::uint64_t headerBytes =
                kFileHeaderBytes + static_cast<std::uint64_t>(cutCount) * kCutBytes + static_cast<std::uint64_t>(blockCount) * kBlockEntryBytes + 8;
            if (headerBytes > size)
                return fail("truncated header");
            if (Hash(data, static_cast<std::size_t>(headerBytes - 8)) != Load<std::uint64_t>(data + headerBytes - 8))
                return fail("header damaged (hash)");
            const std::uint8_t* at = data + kFileHeaderBytes;
            std::uint64_t previous = 0;
            for (std::uint32_t n = 0; n < cutCount; ++n, at += kCutBytes)
            {
                Cut cut;
                cut.tag = Load<std::uint32_t>(at);
                cut.offset = Load<std::uint64_t>(at + 8);
                if ((cut.tag != kPoseTag && cut.tag != kRopeTag) || cut.offset > header.clipBytes || (n > 0 && cut.offset < previous))
                    return fail("bad section cut");
                for (const Cut& other : header.cuts)
                    if (other.tag == cut.tag)
                        return fail("bad section cut");
                previous = cut.offset;
                header.cuts.push_back(cut);
            }
            std::uint64_t fileAt = headerBytes;
            std::uint64_t streamTotal = 0;
            header.blocks.resize(blockCount);
            for (Block& block : header.blocks)
            {
                block.stream = Load<std::uint32_t>(at);
                block.codec = static_cast<Codec>(at[4]);
                block.shuffle = at[5];
                std::memcpy(block.props, at + 6, 5);
                block.rawBytes = Load<std::uint64_t>(at + 12);
                block.packedBytes = Load<std::uint64_t>(at + 20);
                block.hash = Load<std::uint64_t>(at + 28);
                at += kBlockEntryBytes;
                if (block.stream >= kStreamCount || block.codec > Codec::Zstd || block.rawBytes > kMaxBlockBytes ||
                    block.packedBytes > size || (block.shuffle != 0 && block.shuffle != 2 && block.shuffle != 4) ||
                    (block.shuffle > 1 && block.rawBytes % block.shuffle != 0))
                    return fail("bad block");
                if (block.packedBytes > size - fileAt)
                    return fail("truncated (a block runs past the end)");
                block.fileOffset = fileAt;
                fileAt += block.packedBytes;
                block.offset = header.streamBytes[block.stream];
                header.streamBytes[block.stream] += block.rawBytes;
                streamTotal += block.rawBytes;
                if (streamTotal > 2 * header.clipBytes + (1u << 20))
                    return fail("streams larger than the clip allows");
            }
            // After the blocks: only skippable frames (the summary).
            for (std::uint64_t tail = fileAt; tail < size;)
            {
                if (size - tail < 8)
                    return fail("trailing data");
                const std::uint32_t magic = Load<std::uint32_t>(data + tail);
                const std::uint32_t length = Load<std::uint32_t>(data + tail + 4);
                if ((magic & 0xFFFFFFF0u) != 0x184D2A50u || length > size - tail - 8)
                    return fail("trailing data");
                tail += 8 + static_cast<std::uint64_t>(length);
            }
            return true;
        }

        bool UnpackImpl(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>* owned, const UnpackOptions& options,
            std::vector<std::uint8_t>& out, std::string& error, UnpackStats* stats)
        {
            out.clear();
            UnpackStats local;
            UnpackStats& st = stats ? *stats : local;
            st = UnpackStats{};
            const unsigned threads = DefaultThreads(options.threads);
            st.threads = threads;
            const auto blocksStart = std::chrono::steady_clock::now();
            Header header;
            if (!ReadHeader(data, size, options.maxClipBytes, header, error))
                return false;
            StreamSet streams(kStreamCount);
            for (std::uint32_t id = 0; id < kStreamCount; ++id)
            {
                streams[id].resize(static_cast<std::size_t>(header.streamBytes[id]));
                st.streamBytes += header.streamBytes[id];
            }
            st.blocks = static_cast<std::uint32_t>(header.blocks.size());
            // Largest first, so a large block does not end last alone.
            std::vector<std::size_t> order(header.blocks.size());
            for (std::size_t n = 0; n < order.size(); ++n)
                order[n] = n;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return header.blocks[a].rawBytes + header.blocks[a].packedBytes > header.blocks[b].rawBytes + header.blocks[b].packedBytes;
            });
            ErrorSlot failure;
            ParallelFor(order.size(), threads, [&](std::size_t index) {
                if (failure.Failed())
                    return;
                if (Cancelled(options.cancel))
                {
                    failure.Set("cancelled");
                    return;
                }
                const Block& block = header.blocks[order[index]];
                try
                {
                    std::vector<std::uint8_t> scratch;
                    std::string blockError;
                    if (!DecompressBlock(data + block.fileOffset, block, streams[block.stream].data() + block.offset, scratch, blockError))
                        failure.Set(blockError);
                }
                catch (const std::bad_alloc&)
                {
                    failure.Set("packed clip: out of memory");
                }
                catch (...)
                {
                    failure.Set("packed clip: exception");
                }
            });
            if (failure.Failed())
            {
                error = failure.Error();
                return false;
            }
            if (owned)
            {
                owned->clear();
                owned->shrink_to_fit();
                data = nullptr;
            }
            st.blocksMs = MsSince(blocksStart);
            if (Cancelled(options.cancel))
            {
                error = "cancelled";
                return false;
            }

            // The clip: rest with the payloads put back at their cuts.
            const auto rebuildStart = std::chrono::steady_clock::now();
            const bool havePose = std::any_of(header.cuts.begin(), header.cuts.end(), [](const Cut& c) { return c.tag == kPoseTag; });
            const bool haveRope = std::any_of(header.cuts.begin(), header.cuts.end(), [](const Cut& c) { return c.tag == kRopeTag; });
            PosePlan plan;
            if (havePose && !PlanPose(streams, threads, plan, error))
                return false;
            std::vector<std::uint8_t> rope;
            if (haveRope)
            {
                if (!JoinRope(streams, rope, error))
                    return false;
                for (const std::uint32_t id : {kRopeMeta, kRopeTimes, kRopePositions})
                    std::vector<std::uint8_t>().swap(streams[id]);
            }
            const std::vector<std::uint8_t>& rest = streams[kRest];
            std::uint64_t insertBytes = (havePose ? plan.size : 0) + rope.size();
            if (rest.size() + insertBytes != header.clipBytes)
            {
                error = "packed clip: rebuilt size does not match";
                return false;
            }
            out.resize(static_cast<std::size_t>(header.clipBytes));
            std::uint64_t outAt = 0, restAt = 0;
            for (const Cut& cut : header.cuts)
            {
                if (cut.offset < outAt || cut.offset - outAt > rest.size() - restAt)
                {
                    out.clear();
                    error = "packed clip: bad section cut";
                    return false;
                }
                const std::uint64_t take = cut.offset - outAt;
                std::memcpy(out.data() + outAt, rest.data() + restAt, static_cast<std::size_t>(take));
                outAt += take;
                restAt += take;
                if (cut.tag == kPoseTag)
                {
                    if (!WritePose(plan, streams, threads, out.data() + outAt, error))
                    {
                        out.clear();
                        return false;
                    }
                    outAt += plan.size;
                }
                else
                {
                    if (!rope.empty())
                        std::memcpy(out.data() + outAt, rope.data(), rope.size());
                    outAt += rope.size();
                    std::vector<std::uint8_t>().swap(rope);
                }
            }
            if (rest.size() - restAt != header.clipBytes - outAt)
            {
                out.clear();
                error = "packed clip: rebuilt size does not match";
                return false;
            }
            std::memcpy(out.data() + outAt, rest.data() + restAt, rest.size() - static_cast<std::size_t>(restAt));
            st.rebuildMs = MsSince(rebuildStart);
            error.clear();
            return true;
        }
    }

    const char* StreamName(std::uint32_t stream)
    {
        static const char* const kNames[kStreamCount] = {"rest", "pose.meta", "pose.time", "pose.flags", "pose.count",
            "pose.world", "pose.chmask", "pose.r32big", "pose.r32f", "pose.r64big", "pose.r64f", "pose.t16", "pose.tfloat",
            "pose.tw", "pose.scale", "rope.meta", "rope.times", "rope.pos"};
        return stream < kStreamCount ? kNames[stream] : "unknown";
    }

    bool IsPacked(const std::uint8_t* data, std::size_t size)
    {
        return data && size >= sizeof(kMagic) && std::memcmp(data, kMagic, sizeof(kMagic)) == 0;
    }

    std::size_t HeaderBytes(const std::uint8_t* data, std::size_t size)
    {
        if (!IsPacked(data, size) || size < kFileHeaderBytes || Load<std::uint32_t>(data + 8) != kVersion)
            return 0;
        const std::uint32_t blockCount = Load<std::uint32_t>(data + 12);
        const std::uint32_t cutCount = Load<std::uint32_t>(data + 24);
        if (blockCount > kMaxBlocks || cutCount > kMaxCuts)
            return 0;
        return kFileHeaderBytes + cutCount * kCutBytes + static_cast<std::size_t>(blockCount) * kBlockEntryBytes + 8;
    }

    bool ReadHeaderInfo(const std::uint8_t* data, std::size_t size, HeaderInfo& info)
    {
        info = HeaderInfo{};
        const std::size_t headerBytes = HeaderBytes(data, size);
        if (headerBytes == 0 || headerBytes > size)
            return false;
        if (Hash(data, headerBytes - 8) != Load<std::uint64_t>(data + headerBytes - 8))
            return false;
        info.clipBytes = Load<std::uint64_t>(data + 16);
        info.blocks = Load<std::uint32_t>(data + 12);
        const std::uint8_t* at = data + kFileHeaderBytes + Load<std::uint32_t>(data + 24) * kCutBytes;
        for (std::uint32_t n = 0; n < info.blocks; ++n, at += kBlockEntryBytes)
        {
            switch (static_cast<Codec>(at[4]))
            {
            case Codec::Stored: ++info.stored; break;
            case Codec::Lzma: ++info.lzma; break;
            case Codec::Zstd: ++info.zstd; break;
            default: return false;
            }
        }
        return true;
    }

    std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size) { return Hash(data, size); }

    std::uint64_t ContentSize(const std::uint8_t* data, std::size_t size)
    {
        if (!IsPacked(data, size) || size < kFileHeaderBytes || Load<std::uint32_t>(data + 8) != kVersion)
            return 0;
        return Load<std::uint64_t>(data + 16);
    }

    namespace
    {
        // One pose track as read from the payload (pointers into it).
        struct SplitTrackRef
        {
            const std::uint8_t* head = nullptr;
            std::uint64_t layoutCount = 0;
            const std::uint8_t* layouts = nullptr;
            std::size_t layoutBytes = 0;
            std::uint64_t anchorCount = 0;
            std::uint64_t chunkCount = 0;
            std::vector<SplitPart> parts; // anchors, then chunks
            const std::uint8_t* tail = nullptr;
            std::size_t payloadBytes = 0; // the track's bytes in the payload (the work's weight)
        };

        bool ReadSplitTrack(Reader& r, const std::uint8_t* payload, SplitTrackRef& track)
        {
            const std::size_t trackStart = r.At();
            track.head = r.Take(kTrackHead);
            track.layoutCount = r.Count(46);
            const std::size_t layoutStart = r.At();
            for (std::uint64_t n = 0; n < track.layoutCount && r.Ok(); ++n)
                SkipLayout(r);
            track.layoutBytes = r.Ok() ? r.At() - layoutStart : 0;
            track.layouts = payload + layoutStart;
            track.parts.clear();
            track.anchorCount = r.Count(8);
            for (std::uint64_t n = 0; n < track.anchorCount && r.Ok(); ++n)
            {
                SplitPart part;
                part.size = static_cast<std::size_t>(r.Count(1));
                part.bytes = r.Take(part.size);
                track.parts.push_back(part);
            }
            track.chunkCount = r.Count(32);
            for (std::uint64_t n = 0; n < track.chunkCount && r.Ok(); ++n)
            {
                SplitPart part;
                part.chunk = true;
                part.layout = r.Get<std::uint32_t>();
                part.anchor = r.Get<std::int32_t>();
                part.size = static_cast<std::size_t>(r.Count(1));
                part.bytes = r.Take(part.size);
                part.timeCount = r.Count(4);
                part.times = r.Take(part.timeCount * 4);
                part.offsetCount = r.Count(4);
                part.offsets = r.Take(part.offsetCount * 4);
                track.parts.push_back(part);
            }
            track.tail = r.Take(kTrackTail);
            track.payloadBytes = r.Ok() ? r.At() - trackStart : 0;
            return r.Ok();
        }

        // One track's split appended to `s` (its meta and its fields; the
        // tracks are independent: the channel state starts over each).
        bool SplitTrack(const SplitTrackRef& track, StreamSet& s, ChannelState& state, TrackFields& fields,
            std::vector<std::uint32_t>& counts, std::string& why)
        {
            std::vector<std::uint8_t>& meta = s[kPoseMeta];
            PutBytes(meta, track.head, kTrackHead);
            Put<std::uint64_t>(meta, track.layoutCount);
            Put<std::uint64_t>(meta, track.layoutBytes);
            PutBytes(meta, track.layouts, track.layoutBytes);
            Put<std::uint64_t>(meta, track.anchorCount);
            Put<std::uint64_t>(meta, track.chunkCount);
            state.Reset();
            for (const SplitPart& part : track.parts)
            {
                std::uint64_t records = 0;
                bool derived = false;
                if (!SplitRecords(part, state, s, fields, records, derived, why))
                    return false;
                if (part.chunk)
                {
                    Put<std::uint32_t>(meta, part.layout);
                    Put<std::int32_t>(meta, part.anchor);
                }
                Put<std::uint64_t>(meta, records);
                if (part.chunk)
                {
                    meta.push_back(derived ? 0 : 1);
                    if (!derived)
                    {
                        Put<std::uint64_t>(meta, part.timeCount);
                        PutBytes(meta, part.times, static_cast<std::size_t>(part.timeCount * 4));
                        Put<std::uint64_t>(meta, part.offsetCount);
                        PutBytes(meta, part.offsets, static_cast<std::size_t>(part.offsetCount * 4));
                    }
                }
            }
            AppendChannelMajor(fields, s, counts);
            PutBytes(meta, track.tail, kTrackTail);
            return true;
        }
    }

    bool SplitPose(const std::uint8_t* payload, std::size_t size, StreamSet& s, std::string& why)
    {
        return SplitPose(payload, size, 1, s, why);
    }

    bool SplitPose(const std::uint8_t* payload, std::size_t size, unsigned threads, StreamSet& s, std::string& why)
    {
        s.assign(kStreamCount, {});
        try
        {
            Reader r(payload, size);
            const std::uint64_t trackCount = r.Count(kTrackHead + 32 + kTrackTail);
            Put<std::uint64_t>(s[kPoseMeta], trackCount);
            // The tracks read first (pointers only), then split: on one
            // thread straight into `s`, or in contiguous groups of tracks
            // on several, each group into its own streams, appended in
            // order (the same bytes as one thread).
            std::vector<SplitTrackRef> tracks(static_cast<std::size_t>(trackCount));
            for (SplitTrackRef& track : tracks)
            {
                if (!ReadSplitTrack(r, payload, track))
                {
                    why = "pose: track layout not known";
                    s.assign(kStreamCount, {});
                    return false;
                }
            }
            if (!r.AtEnd())
            {
                why = "pose: bytes after the tracks";
                s.assign(kStreamCount, {});
                return false;
            }
            const unsigned workers = static_cast<unsigned>(std::min<std::size_t>(std::max(1u, threads), tracks.size()));
            if (workers <= 1)
            {
                // About the payload's size in all.
                s[kPoseChannelMask].reserve(size / 6);
                auto state = std::make_unique<ChannelState>();
                TrackFields fields;
                std::vector<std::uint32_t> counts(kChannels);
                for (const SplitTrackRef& track : tracks)
                    if (!SplitTrack(track, s, *state, fields, counts, why))
                    {
                        s.assign(kStreamCount, {});
                        return false;
                    }
                return true;
            }
            // Groups of about equal payload bytes, a few per thread.
            const std::size_t groupCount = std::min<std::size_t>(tracks.size(), static_cast<std::size_t>(workers) * 4);
            std::vector<std::size_t> groupStart(groupCount + 1, tracks.size());
            {
                std::uint64_t total = 0;
                for (const SplitTrackRef& track : tracks)
                    total += track.payloadBytes;
                std::uint64_t seen = 0;
                std::size_t group = 0;
                groupStart[0] = 0;
                for (std::size_t t = 0; t < tracks.size() && group + 1 < groupCount; ++t)
                {
                    seen += tracks[t].payloadBytes;
                    if (seen * groupCount >= total * (group + 1) && t + 1 < tracks.size())
                        groupStart[++group] = t + 1;
                }
                for (std::size_t g = group + 1; g <= groupCount; ++g)
                    groupStart[g] = tracks.size();
            }
            std::vector<StreamSet> parts(groupCount);
            ErrorSlot failure;
            ParallelFor(groupCount, workers, [&](std::size_t g) {
                if (failure.Failed())
                    return;
                try
                {
                    StreamSet& part = parts[g];
                    part.assign(kStreamCount, {});
                    auto state = std::make_unique<ChannelState>();
                    TrackFields fields;
                    std::vector<std::uint32_t> counts(kChannels);
                    std::string groupWhy;
                    for (std::size_t t = groupStart[g]; t < groupStart[g + 1]; ++t)
                        if (!SplitTrack(tracks[t], part, *state, fields, counts, groupWhy))
                        {
                            failure.Set(groupWhy);
                            return;
                        }
                }
                catch (const std::bad_alloc&)
                {
                    failure.Set("pose: out of memory");
                }
            });
            if (failure.Failed())
            {
                why = failure.Error();
                s.assign(kStreamCount, {});
                return false;
            }
            // Appended in order, each group's stream released once copied.
            for (std::uint32_t id = 0; id < kStreamCount; ++id)
            {
                std::size_t total = s[id].size();
                for (const StreamSet& part : parts)
                    total += part[id].size();
                if (total == 0)
                    continue;
                s[id].reserve(total);
                for (StreamSet& part : parts)
                {
                    s[id].insert(s[id].end(), part[id].begin(), part[id].end());
                    std::vector<std::uint8_t>().swap(part[id]);
                }
            }
            return true;
        }
        catch (const std::bad_alloc&)
        {
            why = "pose: out of memory";
            s.assign(kStreamCount, {});
            return false;
        }
    }

    bool JoinPose(const StreamSet& streams, unsigned threads, std::vector<std::uint8_t>& payload, std::string& error)
    {
        payload.clear();
        if (streams.size() != kStreamCount)
        {
            error = "pose: no streams";
            return false;
        }
        PosePlan plan;
        if (!PlanPose(streams, DefaultThreads(threads), plan, error))
            return false;
        payload.resize(static_cast<std::size_t>(plan.size));
        if (!WritePose(plan, streams, DefaultThreads(threads), payload.data(), error))
        {
            payload.clear();
            return false;
        }
        return true;
    }

    bool SplitRope(const std::uint8_t* payload, std::size_t size, StreamSet& s, std::string& why)
    {
        s.assign(kStreamCount, {});
        Reader r(payload, size);
        const std::uint64_t count = r.Count(76);
        std::vector<std::uint8_t>& meta = s[kRopeMeta];
        Put<std::uint64_t>(meta, count);
        for (std::uint64_t n = 0; n < count && r.Ok(); ++n)
        {
            const std::size_t start = r.At();
            std::uint32_t vertices = 0;
            if (!SkipRopeHead(r, vertices))
                break;
            const std::size_t headBytes = r.At() - start;
            const std::uint64_t times = r.Count(4);
            const std::uint8_t* timeBytes = r.Take(times * 4);
            const std::uint64_t positions = r.Count(4);
            const std::uint8_t* positionBytes = r.Take(positions * 4);
            const std::uint64_t windows = r.Count(4);
            const std::uint8_t* windowBytes = r.Take(windows * 4 + 4); // and nextStateLogMs
            if (!r.Ok())
                break;
            const bool channelMajor = RopeChannelMajor(vertices, times, positions);
            Put<std::uint64_t>(meta, headBytes);
            PutBytes(meta, payload + start, headBytes);
            Put<std::uint64_t>(meta, times);
            Put<std::uint64_t>(meta, positions);
            meta.push_back(channelMajor ? 1 : 0);
            Put<std::uint64_t>(meta, windows);
            PutBytes(meta, windowBytes, static_cast<std::size_t>(windows * 4 + 4));
            std::uint32_t previous = 0;
            for (std::uint64_t t = 0; t < times; ++t)
            {
                const std::uint32_t time = Load<std::uint32_t>(timeBytes + 4 * t);
                Put<std::uint32_t>(s[kRopeTimes], time - previous);
                previous = time;
            }
            std::vector<std::uint8_t>& out = s[kRopePositions];
            const std::size_t base = out.size();
            out.resize(base + static_cast<std::size_t>(positions * 4));
            if (channelMajor)
            {
                const std::uint64_t stride = static_cast<std::uint64_t>(vertices) * 3u;
                std::uint8_t* to = out.data() + base;
                for (std::uint64_t v = 0; v < stride; ++v)
                {
                    std::uint32_t last = 0;
                    for (std::uint64_t t = 0; t < times; ++t)
                    {
                        const std::uint32_t value = Load<std::uint32_t>(positionBytes + 4 * (t * stride + v));
                        Store<std::uint32_t>(to + 4 * (v * times + t), value - last);
                        last = value;
                    }
                }
            }
            else if (positions)
                std::memcpy(out.data() + base, positionBytes, static_cast<std::size_t>(positions * 4));
        }
        if (!r.AtEnd())
        {
            why = "rope: layout not known";
            s.assign(kStreamCount, {});
            return false;
        }
        return true;
    }

    bool JoinRope(const StreamSet& s, std::vector<std::uint8_t>& payload, std::string& error)
    {
        payload.clear();
        if (s.size() != kStreamCount)
        {
            error = "rope: no streams";
            return false;
        }
        const std::vector<std::uint8_t>& meta = s[kRopeMeta];
        const std::vector<std::uint8_t>& timeStream = s[kRopeTimes];
        const std::vector<std::uint8_t>& positionStream = s[kRopePositions];
        Reader r(meta.data(), meta.size());
        const std::uint64_t count = r.Count(8 + 8 + 8 + 1 + 8 + 4);
        payload.reserve(meta.size() + timeStream.size() + positionStream.size() + 64);
        Put<std::uint64_t>(payload, count);
        std::size_t timeAt = 0, positionAt = 0;
        for (std::uint64_t n = 0; n < count && r.Ok(); ++n)
        {
            const std::uint64_t headBytes = r.Count(1);
            const std::uint8_t* head = r.Take(headBytes);
            const std::uint64_t times = r.Get<std::uint64_t>();
            const std::uint64_t positions = r.Get<std::uint64_t>();
            const std::uint8_t channelMajor = r.Get<std::uint8_t>();
            const std::uint64_t windows = r.Count(4);
            const std::uint8_t* windowBytes = r.Take(windows * 4 + 4);
            if (!r.Ok())
                break;
            // The head must parse as a rope head and give the same layout.
            Reader headReader(head, static_cast<std::size_t>(headBytes));
            std::uint32_t vertices = 0;
            if (!SkipRopeHead(headReader, vertices) || !headReader.AtEnd() || channelMajor > 1 ||
                (channelMajor == 1) != RopeChannelMajor(vertices, times, positions) ||
                times > (timeStream.size() - timeAt) / 4 || positions > (positionStream.size() - positionAt) / 4)
            {
                r.Fail();
                break;
            }
            PutBytes(payload, head, static_cast<std::size_t>(headBytes));
            Put<std::uint64_t>(payload, times);
            std::uint32_t time = 0;
            for (std::uint64_t t = 0; t < times; ++t, timeAt += 4)
            {
                time += Load<std::uint32_t>(timeStream.data() + timeAt);
                Put<std::uint32_t>(payload, time);
            }
            Put<std::uint64_t>(payload, positions);
            const std::size_t base = payload.size();
            payload.resize(base + static_cast<std::size_t>(positions * 4));
            const std::uint8_t* from = positionStream.data() + positionAt;
            if (channelMajor)
            {
                const std::uint64_t stride = static_cast<std::uint64_t>(vertices) * 3u;
                std::uint8_t* to = payload.data() + base;
                for (std::uint64_t v = 0; v < stride; ++v)
                {
                    std::uint32_t value = 0;
                    for (std::uint64_t t = 0; t < times; ++t)
                    {
                        value += Load<std::uint32_t>(from + 4 * (v * times + t));
                        Store<std::uint32_t>(to + 4 * (t * stride + v), value);
                    }
                }
            }
            else if (positions)
                std::memcpy(payload.data() + base, from, static_cast<std::size_t>(positions * 4));
            positionAt += static_cast<std::size_t>(positions * 4);
            Put<std::uint64_t>(payload, windows);
            PutBytes(payload, windowBytes, static_cast<std::size_t>(windows * 4 + 4));
        }
        if (!r.AtEnd() || timeAt != timeStream.size() || positionAt != positionStream.size())
        {
            payload.clear();
            error = "packed clip: rope streams do not match";
            return false;
        }
        return true;
    }

    bool Pack(const std::uint8_t* clip, std::size_t size, const PackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, PackStats* stats)
    {
        try
        {
            return PackImpl(clip, size, nullptr, options, out, error, stats);
        }
        catch (const std::bad_alloc&)
        {
            out.clear();
            error = "pack: out of memory";
            return false;
        }
    }

    bool PackConsume(std::vector<std::uint8_t>&& clip, const PackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, PackStats* stats)
    {
        try
        {
            return PackImpl(clip.data(), clip.size(), &clip, options, out, error, stats);
        }
        catch (const std::bad_alloc&)
        {
            out.clear();
            error = "pack: out of memory";
            return false;
        }
    }

    bool Unpack(const std::uint8_t* data, std::size_t size, const UnpackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, UnpackStats* stats)
    {
        try
        {
            return UnpackImpl(data, size, nullptr, options, out, error, stats);
        }
        catch (const std::bad_alloc&)
        {
            out.clear();
            error = "packed clip: out of memory";
            return false;
        }
    }

    bool UnpackConsume(std::vector<std::uint8_t>&& file, const UnpackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, UnpackStats* stats)
    {
        try
        {
            return UnpackImpl(file.data(), file.size(), &file, options, out, error, stats);
        }
        catch (const std::bad_alloc&)
        {
            out.clear();
            error = "packed clip: out of memory";
            return false;
        }
    }
}
