// The pose capture path against a straightforward reference. The reference
// (namespace v170: the encoder with its quantization, the track's keyframe
// logic and the worker's composition of a locals-only frame with the engine
// parent table) composes every bone, validates and encodes; the shipped path
// composes only what the encoder reads, skips the validation of a
// worker-composed frame on the exact engine layout, skips channels whose
// input bytes did not change and runs cheap tests before the exact math.
// Every test feeds the same engine-like frames (locals, world matrix, time)
// through both and requires the recorded track byte for byte. The same code
// times each phase of both (ns per bone per frame).
//
// Raw engine samples are not kept anywhere (clips hold quantized tracks), so
// the input is (a) synthetic skeletons with engine-like parent tables (out of
// order bones, root scale 1 and 0.919) and motion made to sit on the
// thresholds, and (b) with REPLAYPOC_POSE_CLIP_DIR=<folder>, every track of
// the clips there, decoded frame by frame into engine locals on the clip's
// own layout and parent hierarchy, as recorded and with noise added around
// the thresholds (decoded locals repeat more than raw ones, so the plain run
// overstates the share of unchanged channels; the noisy one understates it).

#include "pose_stream.h"

#include "clip_file.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <emmintrin.h>

// tests/pose_stream_tests.cpp keeps the reference FindFailingBonesChanged and
// the hook that moves the SSE expected objects by a few ulps.
namespace v175
{
    std::vector<std::uint32_t> FindFailingBonesChanged(
        const replay::pose::SkeletonLayout& layout,
        const replay::pose::BoneQts* locals,
        const replay::pose::BoneQts* objects,
        replay::pose::BoneQts* previousLocals,
        replay::pose::BoneQts* previousObjects,
        float translationTolerance,
        float rotationTolerance,
        std::uint32_t& checked,
        replay::pose::ChangedBoneCounts* counts);
}

namespace band_nudge
{
    float StepUlps(float value, int steps);
    void Set(int maxUlps, float translationUnits, float rotationUnits, std::uint32_t seed);
    void Hook(replay::pose::BoneQts& composed, const replay::pose::BoneQts& parentObject, std::uint32_t bone);
}

namespace
{
    using namespace replay::pose;

    int g_captureFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_captureFailures;
    }

    double NowUs()
    {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    }
}

// ---------------------------------------------------------------------------
// The reference: pose_math.cpp quantization (std::lround), a plain
// PoseEncoder::Encode and the keyframe logic of PoseTrack::Append,
// pose_capture.cpp ComposeEngineObjects composing every bone.
namespace v170
{
    using namespace replay::pose;

    constexpr float kInvSqrt2 = 0.70710678118654752f;

    float Component(const Quat& q, int index)
    {
        switch (index)
        {
        case 0: return q.x;
        case 1: return q.y;
        case 2: return q.z;
        default: return q.w;
        }
    }

    int CanonicalLargest(Quat& q)
    {
        q = Normalize(q);
        int largest = 0;
        float largestAbs = std::fabs(q.x);
        for (int index = 1; index < 4; ++index)
        {
            const float value = std::fabs(Component(q, index));
            if (value > largestAbs)
            {
                largestAbs = value;
                largest = index;
            }
        }
        if (Component(q, largest) < 0.0f)
            q = {-q.x, -q.y, -q.z, -q.w};
        return largest;
    }

    std::uint64_t QuantizeUnit(float value, std::uint64_t maxCode)
    {
        const float normalized =
            (std::clamp(value, -kInvSqrt2, kInvSqrt2) + kInvSqrt2) /
            (2.0f * kInvSqrt2);
        return static_cast<std::uint64_t>(
            std::lround(normalized * static_cast<float>(maxCode)));
    }

    template <typename Packed>
    Packed PackSmallestThree(Quat q, unsigned bits)
    {
        const int largest = CanonicalLargest(q);
        const std::uint64_t maxCode = (1ull << bits) - 1ull;
        std::uint64_t packed = static_cast<std::uint64_t>(largest)
            << (bits * 3u);
        unsigned shift = bits * 2u;
        for (int index = 0; index < 4; ++index)
        {
            if (index == largest)
                continue;
            packed |= QuantizeUnit(Component(q, index), maxCode) << shift;
            shift -= bits;
        }
        return static_cast<Packed>(packed);
    }

    std::uint32_t PackQuat32(Quat q) { return PackSmallestThree<std::uint32_t>(q, 10u); }
    std::uint64_t PackQuat64(Quat q) { return PackSmallestThree<std::uint64_t>(q, 20u); }

    bool PackTranslation16(const float translation[3], float range, std::int16_t packed[3])
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = translation[axis];
            if (!(std::fabs(value) <= range))
                return false;
            packed[axis] = static_cast<std::int16_t>(
                std::lround(value / range * 32767.0f));
        }
        return true;
    }

    template <typename T>
    void Put(std::vector<std::uint8_t>& out, const T& value)
    {
        const std::size_t offset = out.size();
        out.resize(offset + sizeof(T));
        std::memcpy(out.data() + offset, &value, sizeof(T));
    }

    bool SameBits(const float* a, const float* b, std::size_t count)
    {
        return std::memcmp(a, b, sizeof(float) * count) == 0;
    }

    class Encoder
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config)
        {
            m_layout = &layout;
            m_config = config;
            m_emitted.assign(layout.ChannelCount(), BoneQts{});
            m_emittedCode.assign(layout.ChannelCount(), ~0ull);
            m_anchorEmitted.clear();
            m_anchorCode.clear();
            m_hasAnchor = false;
        }
        bool HasAnchor() const { return m_hasAnchor; }

        const BoneQts& Source(const FrameInput& input, std::uint32_t channel) const
        {
            if (channel < m_layout->boneCount)
                return input.locals[channel];
            return input.objects[m_layout->unresolvedBones[channel - m_layout->boneCount]];
        }

        EncodeStats Encode(const FrameInput& input, KeyframeKind kind, std::vector<std::uint8_t>& out)
        {
            EncodeStats stats;
            const std::size_t recordStart = out.size();
            if (kind == KeyframeKind::Relative && !m_hasAnchor)
                kind = KeyframeKind::Full;
            const bool keyframe = kind == KeyframeKind::Full;
            if (keyframe)
            {
                std::fill(m_emitted.begin(), m_emitted.end(), BoneQts{});
                std::fill(m_emittedCode.begin(), m_emittedCode.end(), ~0ull);
            }
            else if (kind == KeyframeKind::Relative)
            {
                m_emitted = m_anchorEmitted;
                m_emittedCode = m_anchorCode;
            }

            Put<std::uint32_t>(out, 0); // patched below
            Put<std::uint32_t>(out, input.timeMs);
            Put<std::uint16_t>(out, static_cast<std::uint16_t>(
                input.flags | (keyframe ? kFrameKeyframe : 0) |
                (kind == KeyframeKind::Relative ? kFrameRelativeKeyframe : 0)));
            const std::size_t countOffset = out.size();
            Put<std::uint16_t>(out, 0); // patched below
            float world[16] = {};
            if (input.world)
                std::memcpy(world, input.world, sizeof(world));
            for (const float value : world)
                Put<float>(out, value);

            const std::uint32_t channels = m_layout->ChannelCount();
            for (std::uint32_t channel = 0; channel < channels; ++channel)
            {
                const BoneQts& current = Source(input, channel);
                BoneQts& emitted = m_emitted[channel];
                const bool highPrecision =
                    channel >= m_layout->boneCount || m_layout->highPrecision[channel] != 0;

                std::uint8_t mask = 0;
                const std::uint64_t code = highPrecision
                    ? v170::PackQuat64(current.rotation)
                    : v170::PackQuat32(current.rotation);
                if (keyframe ||
                    (code != m_emittedCode[channel] &&
                        AngleBetween(emitted.rotation, current.rotation) > m_config.rotationEpsilon))
                {
                    mask |= highPrecision ? kEntryRot64 : kEntryRot32;
                }

                std::int16_t packedTranslation[3] = {};
                if (keyframe ||
                    TranslationDistance(emitted, current) > m_config.translationEpsilon)
                {
                    mask |= v170::PackTranslation16(
                                current.translation,
                                m_config.translationRange,
                                packedTranslation)
                        ? kEntryTrans16
                        : kEntryTransFloat;
                }
                if (!SameBits(&emitted.translation[3], &current.translation[3], 1))
                    mask |= kEntryTransW;
                if (!SameBits(emitted.scale, current.scale, 4))
                    mask |= kEntryScale;

                if (mask == 0)
                    continue;

                Put<std::uint16_t>(out, static_cast<std::uint16_t>(channel));
                Put<std::uint8_t>(out, mask);
                if (mask & kEntryRot32)
                {
                    const std::uint32_t packed = static_cast<std::uint32_t>(code);
                    Put(out, packed);
                    emitted.rotation = UnpackQuat32(packed);
                    m_emittedCode[channel] = code;
                }
                if (mask & kEntryRot64)
                {
                    Put(out, code);
                    emitted.rotation = UnpackQuat64(code);
                    m_emittedCode[channel] = code;
                }
                if (mask & kEntryTrans16)
                {
                    for (const std::int16_t value : packedTranslation)
                        Put(out, value);
                    UnpackTranslation16(
                        packedTranslation, m_config.translationRange, emitted.translation);
                }
                if (mask & kEntryTransFloat)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        Put(out, current.translation[axis]);
                        emitted.translation[axis] = current.translation[axis];
                    }
                }
                if (mask & kEntryTransW)
                {
                    Put(out, current.translation[3]);
                    emitted.translation[3] = current.translation[3];
                }
                if (mask & kEntryScale)
                {
                    for (int axis = 0; axis < 4; ++axis)
                    {
                        Put(out, current.scale[axis]);
                        emitted.scale[axis] = current.scale[axis];
                    }
                }
                ++stats.entries;
            }

            const std::uint32_t recordSize =
                static_cast<std::uint32_t>(out.size() - recordStart);
            const std::uint16_t entryCount = static_cast<std::uint16_t>(stats.entries);
            std::memcpy(out.data() + recordStart, &recordSize, sizeof(recordSize));
            std::memcpy(out.data() + countOffset, &entryCount, sizeof(entryCount));
            stats.bytes = recordSize;
            if (keyframe)
            {
                m_anchorEmitted = m_emitted;
                m_anchorCode = m_emittedCode;
                m_hasAnchor = true;
            }
            return stats;
        }

    private:
        const SkeletonLayout* m_layout = nullptr;
        StreamConfig m_config;
        std::vector<BoneQts> m_emitted;
        std::vector<std::uint64_t> m_emittedCode;
        std::vector<BoneQts> m_anchorEmitted;
        std::vector<std::uint64_t> m_anchorCode;
        bool m_hasAnchor = false;
    };

    // PoseTrack::Begin, UpdateLayout and Append without playback caches or
    // the byte budget (the tests give both tracks a budget never reached).
    struct Chunk
    {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint32_t> frameTimes;
        bool relative = false;
    };

    class Track
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config)
        {
            m_layout = std::make_shared<const SkeletonLayout>(layout);
            m_forceKeyframe = false;
            m_anchor.reset();
            m_config = config;
            m_encoder.Begin(*m_layout, m_config);
            chunks.clear();
        }

        void UpdateLayout(const SkeletonLayout& layout)
        {
            m_layout = std::make_shared<const SkeletonLayout>(layout);
            m_encoder.Begin(*m_layout, m_config);
            m_forceKeyframe = true;
            m_anchor.reset();
        }

        EncodeStats Append(const FrameInput& input)
        {
            const std::uint32_t interval = m_config.keyframeIntervalMs ? m_config.keyframeIntervalMs : 1;
            const bool keyframe = m_forceKeyframe || chunks.empty() ||
                input.timeMs / interval != chunks.back().frameTimes.front() / interval;
            KeyframeKind kind = KeyframeKind::None;
            if (keyframe)
            {
                kind = (m_forceKeyframe || chunks.empty() || !m_anchor || !m_encoder.HasAnchor())
                    ? KeyframeKind::Full
                    : KeyframeKind::Relative;
                chunks.emplace_back();
                chunks.back().relative = kind == KeyframeKind::Relative;
                m_forceKeyframe = false;
            }
            Chunk& chunk = chunks.back();
            chunk.frameTimes.push_back(input.timeMs);
            const std::size_t recordStart = chunk.bytes.size();
            const EncodeStats encoded = m_encoder.Encode(input, kind, chunk.bytes);
            if (keyframe)
            {
                std::uint16_t flags = 0;
                std::memcpy(&flags, chunk.bytes.data() + recordStart + sizeof(std::uint32_t) * 2, sizeof(flags));
                if (flags & kFrameKeyframe)
                {
                    chunk.relative = false;
                    m_anchor = std::make_shared<const std::vector<std::uint8_t>>(
                        chunk.bytes.begin() + static_cast<std::ptrdiff_t>(recordStart), chunk.bytes.end());
                }
            }
            return encoded;
        }

        std::vector<Chunk> chunks;

    private:
        std::shared_ptr<const SkeletonLayout> m_layout;
        bool m_forceKeyframe = false;
        std::shared_ptr<const std::vector<std::uint8_t>> m_anchor;
        StreamConfig m_config;
        Encoder m_encoder;
    };

    // pose_capture.cpp ComposeEngineObjects, every bone.
    void ComposeEngineObjects(const std::uint16_t* parents, std::uint32_t boneCount, float rootScale,
        const BoneQts* locals, BoneQts* objects)
    {
        objects[0] = locals[0];
        for (int axis = 0; axis < 3; ++axis)
        {
            objects[0].translation[axis] *= rootScale;
            objects[0].scale[axis] *= rootScale;
        }
        objects[0].scale[3] *= rootScale;
        for (std::uint32_t bone = 1; bone < boneCount; ++bone)
        {
            if (parents[bone] < bone)
                objects[bone] = Compose(objects[parents[bone]], locals[bone], CompositionOrder::ParentThenLocal, true);
            else
                objects[bone] = locals[bone];
        }
    }
}

// ---------------------------------------------------------------------------
// A single-pass PoseEncoder (one vector::resize per field written, one pass
// over the channels with the equal-input skip inside it) with its own
// shortcut switches, as the reference of the record scratch and the two-pass
// delta frame; and PoseTrack::Append's keyframe logic around it (as
// v170::Track, returning the stats).
namespace v175
{
    using namespace replay::pose;

    namespace
    {
        template <typename T>
        void Put(std::vector<std::uint8_t>& out, const T& value)
        {
            const std::size_t offset = out.size();
            out.resize(offset + sizeof(T));
            std::memcpy(out.data() + offset, &value, sizeof(T));
        }

        bool SameBits(const float* a, const float* b, std::size_t count)
        {
            return std::memcmp(a, b, sizeof(float) * count) == 0;
        }

        // The 48 bytes of two bone records are equal.
        inline bool SameBone(const BoneQts& a, const BoneQts& b)
        {
            const auto* pa = reinterpret_cast<const __m128i*>(&a);
            const auto* pb = reinterpret_cast<const __m128i*>(&b);
            const __m128i e0 = _mm_cmpeq_epi8(_mm_loadu_si128(pa), _mm_loadu_si128(pb));
            const __m128i e1 = _mm_cmpeq_epi8(_mm_loadu_si128(pa + 1), _mm_loadu_si128(pb + 1));
            const __m128i e2 = _mm_cmpeq_epi8(_mm_loadu_si128(pa + 2), _mm_loadu_si128(pb + 2));
            return _mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0, e1), e2)) == 0xFFFF;
        }

        // PoseEncoder::SetShortcuts of this copy (the tests set both alike).
        std::atomic<bool> s_rawSkip{true};
        std::atomic<bool> s_cheapTests{true};
    }

    class Encoder
    {
    public:
        static void SetShortcuts(bool rawSkip, bool cheapTests)
        {
            s_rawSkip.store(rawSkip, std::memory_order_relaxed);
            s_cheapTests.store(cheapTests, std::memory_order_relaxed);
        }

        void Begin(const SkeletonLayout& layout, const StreamConfig& config)
        {
            m_layout = &layout;
            m_config = config;
            m_emitted.assign(layout.ChannelCount(), BoneQts{});
            m_emittedCode.assign(layout.ChannelCount(), ~0ull);
            m_anchorEmitted.clear();
            m_anchorCode.clear();
            m_hasAnchor = false;
            m_lastInput.assign(layout.ChannelCount(), BoneQts{});
            m_hasLastInput = false;
            m_angleTest = MakeAngleTest(config.rotationEpsilon);
        }
        bool HasAnchor() const { return m_hasAnchor; }

        const BoneQts& Source(const FrameInput& input, std::uint32_t channel) const
        {
            if (channel < m_layout->boneCount)
                return input.locals[channel];
            return input.objects[m_layout->unresolvedBones[channel - m_layout->boneCount]];
        }

        EncodeStats Encode(
            const FrameInput& input,
            KeyframeKind kind,
            std::vector<std::uint8_t>& out)
        {
            EncodeStats stats;
            const std::size_t recordStart = out.size();
            if (kind == KeyframeKind::Relative && !m_hasAnchor)
                kind = KeyframeKind::Full;
            const bool keyframe = kind == KeyframeKind::Full;
            if (keyframe)
            {
                std::fill(m_emitted.begin(), m_emitted.end(), BoneQts{});
                std::fill(m_emittedCode.begin(), m_emittedCode.end(), ~0ull);
            }
            else if (kind == KeyframeKind::Relative)
            {
                m_emitted = m_anchorEmitted;
                m_emittedCode = m_anchorCode;
            }

            Put<std::uint32_t>(out, 0); // patched below
            Put<std::uint32_t>(out, input.timeMs);
            Put<std::uint16_t>(out, static_cast<std::uint16_t>(
                input.flags | (keyframe ? kFrameKeyframe : 0) |
                (kind == KeyframeKind::Relative ? kFrameRelativeKeyframe : 0)));
            const std::size_t countOffset = out.size();
            Put<std::uint16_t>(out, 0); // patched below
            float world[16] = {};
            if (input.world)
                std::memcpy(world, input.world, sizeof(world));
            for (const float value : world)
                Put<float>(out, value);

            const std::uint32_t channels = m_layout->ChannelCount();
            // Only a delta frame may skip on equal input bytes; a
            // keyframe resets the state (full) or restores the anchor's
            // (relative), after which the previous frame's input proves nothing.
            const bool rawSkipOn = s_rawSkip.load(std::memory_order_relaxed) && m_lastInput.size() == channels;
            const bool rawSkip = rawSkipOn && kind == KeyframeKind::None && m_hasLastInput;
            const bool cheap = s_cheapTests.load(std::memory_order_relaxed);
            if (kind == KeyframeKind::None)
                stats.deltaChannels = channels;
            for (std::uint32_t channel = 0; channel < channels; ++channel)
            {
                const BoneQts& current = Source(input, channel);
                if (rawSkip)
                {
                    BoneQts& last = m_lastInput[channel];
                    if (SameBone(last, current))
                    {
                        ++stats.rawSameChannels;
                        continue;
                    }
                    last = current;
                }
                else if (rawSkipOn)
                {
                    m_lastInput[channel] = current;
                }
                BoneQts& emitted = m_emitted[channel];
                const bool highPrecision =
                    channel >= m_layout->boneCount || m_layout->highPrecision[channel] != 0;

                std::uint8_t mask = 0;
                // The rotation is emitted when its code differs
                // from the last emitted one AND it turned more than epsilon from
                // the emitted rotation. Both are pure, so the order does not
                // matter: the angle (cheap test, exact near the threshold)
                // comes first and the quantization only runs when it passes or
                // on a keyframe (the code is only used to emit).
                std::uint64_t code = 0;
                bool rotate = keyframe;
                if (!cheap)
                {
                    // Reference order: quantize first, then the exact angle.
                    code = highPrecision ? PackQuat64(current.rotation) : PackQuat32(current.rotation);
                    rotate = keyframe ||
                        (code != m_emittedCode[channel] &&
                            AngleBetween(emitted.rotation, current.rotation) > m_config.rotationEpsilon);
                }
                else if (!keyframe && AngleExceeds(m_angleTest, emitted.rotation, current.rotation))
                {
                    code = highPrecision ? PackQuat64(current.rotation) : PackQuat32(current.rotation);
                    rotate = code != m_emittedCode[channel];
                }
                else if (keyframe)
                {
                    code = highPrecision ? PackQuat64(current.rotation) : PackQuat32(current.rotation);
                }
                if (rotate)
                    mask |= highPrecision ? kEntryRot64 : kEntryRot32;

                std::int16_t packedTranslation[3] = {};
                if (keyframe ||
                    (cheap ? TranslationExceeds(emitted, current, m_config.translationEpsilon)
                           : TranslationDistance(emitted, current) > m_config.translationEpsilon))
                {
                    mask |= PackTranslation16(
                                current.translation,
                                m_config.translationRange,
                                packedTranslation)
                        ? kEntryTrans16
                        : kEntryTransFloat;
                }
                if (!SameBits(&emitted.translation[3], &current.translation[3], 1))
                    mask |= kEntryTransW;
                if (!SameBits(emitted.scale, current.scale, 4))
                    mask |= kEntryScale;

                if (mask == 0)
                    continue;

                Put<std::uint16_t>(out, static_cast<std::uint16_t>(channel));
                Put<std::uint8_t>(out, mask);
                if (mask & kEntryRot32)
                {
                    const std::uint32_t packed = static_cast<std::uint32_t>(code);
                    Put(out, packed);
                    emitted.rotation = UnpackQuat32(packed);
                    m_emittedCode[channel] = code;
                }
                if (mask & kEntryRot64)
                {
                    Put(out, code);
                    emitted.rotation = UnpackQuat64(code);
                    m_emittedCode[channel] = code;
                }
                if (mask & kEntryTrans16)
                {
                    for (const std::int16_t value : packedTranslation)
                        Put(out, value);
                    UnpackTranslation16(
                        packedTranslation, m_config.translationRange, emitted.translation);
                }
                if (mask & kEntryTransFloat)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        Put(out, current.translation[axis]);
                        emitted.translation[axis] = current.translation[axis];
                    }
                }
                if (mask & kEntryTransW)
                {
                    Put(out, current.translation[3]);
                    emitted.translation[3] = current.translation[3];
                }
                if (mask & kEntryScale)
                {
                    for (int axis = 0; axis < 4; ++axis)
                    {
                        Put(out, current.scale[axis]);
                        emitted.scale[axis] = current.scale[axis];
                    }
                }
                ++stats.entries;
            }

            m_hasLastInput = rawSkipOn; // off: nothing was kept
            stats.rawSameFrame = rawSkip && channels > 0 && stats.rawSameChannels == channels;

            const std::uint32_t recordSize =
                static_cast<std::uint32_t>(out.size() - recordStart);
            const std::uint16_t entryCount = static_cast<std::uint16_t>(stats.entries);
            std::memcpy(out.data() + recordStart, &recordSize, sizeof(recordSize));
            std::memcpy(out.data() + countOffset, &entryCount, sizeof(entryCount));
            stats.bytes = recordSize;
            if (keyframe)
            {
                m_anchorEmitted = m_emitted;
                m_anchorCode = m_emittedCode;
                m_hasAnchor = true;
            }
            return stats;
        }

    private:
        const SkeletonLayout* m_layout = nullptr;
        StreamConfig m_config;
        std::vector<BoneQts> m_lastInput;
        bool m_hasLastInput = false;
        AngleTest m_angleTest;
        std::vector<BoneQts> m_emitted;
        std::vector<std::uint64_t> m_emittedCode;
        std::vector<BoneQts> m_anchorEmitted;
        std::vector<std::uint64_t> m_anchorCode;
        bool m_hasAnchor = false;
    };

    // PoseTrack::Begin and Append (the keyframe and chunk logic) around it,
    // without playback caches or the byte budget, as v170::Track.
    struct Chunk
    {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint32_t> frameTimes;
        bool relative = false;
    };

    class Track
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config)
        {
            m_layout = std::make_shared<const SkeletonLayout>(layout);
            m_forceKeyframe = false;
            m_anchor.reset();
            m_config = config;
            m_encoder.Begin(*m_layout, m_config);
            chunks.clear();
        }

        EncodeStats Append(const FrameInput& input)
        {
            const std::uint32_t interval = m_config.keyframeIntervalMs ? m_config.keyframeIntervalMs : 1;
            const bool keyframe = m_forceKeyframe || chunks.empty() ||
                input.timeMs / interval != chunks.back().frameTimes.front() / interval;
            KeyframeKind kind = KeyframeKind::None;
            if (keyframe)
            {
                kind = (m_forceKeyframe || chunks.empty() || !m_anchor || !m_encoder.HasAnchor())
                    ? KeyframeKind::Full
                    : KeyframeKind::Relative;
                chunks.emplace_back();
                chunks.back().relative = kind == KeyframeKind::Relative;
                m_forceKeyframe = false;
            }
            Chunk& chunk = chunks.back();
            chunk.frameTimes.push_back(input.timeMs);
            const std::size_t recordStart = chunk.bytes.size();
            const EncodeStats encoded = m_encoder.Encode(input, kind, chunk.bytes);
            if (keyframe)
            {
                std::uint16_t flags = 0;
                std::memcpy(&flags, chunk.bytes.data() + recordStart + sizeof(std::uint32_t) * 2, sizeof(flags));
                if (flags & kFrameKeyframe)
                {
                    chunk.relative = false;
                    m_anchor = std::make_shared<const std::vector<std::uint8_t>>(
                        chunk.bytes.begin() + static_cast<std::ptrdiff_t>(recordStart), chunk.bytes.end());
                }
            }
            return encoded;
        }

        std::vector<Chunk> chunks;

    private:
        std::shared_ptr<const SkeletonLayout> m_layout;
        bool m_forceKeyframe = false;
        std::shared_ptr<const std::vector<std::uint8_t>> m_anchor;
        StreamConfig m_config;
        Encoder m_encoder;
    };
}

namespace
{
    // ---------------------------------------------------------------------
    // Helpers.
    Quat RandomUnit(std::mt19937& rng)
    {
        std::normal_distribution<float> n(0.0f, 1.0f);
        return Normalize({n(rng), n(rng), n(rng), n(rng)});
    }

    Quat AxisAngle(Vec3 axis, float angle)
    {
        const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        const float s = std::sin(angle * 0.5f) / length;
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
    }

    Vec3 RandomAxis(std::mt19937& rng)
    {
        std::normal_distribution<float> n(0.0f, 1.0f);
        Vec3 axis{n(rng), n(rng), n(rng)};
        if (axis.x == 0.0f && axis.y == 0.0f && axis.z == 0.0f)
            axis.z = 1.0f;
        return axis;
    }

    bool SameBytes(const void* a, const void* b, std::size_t size) { return std::memcmp(a, b, size) == 0; }

    // A valid entity matrix (right, forward, up, position with the 32 m cell
    // in the w lane), as the skeleton parent matrix holds it.
    void MakeWorld(float heading, float x, float y, float z, float world[16])
    {
        const float c = std::cos(heading);
        const float s = std::sin(heading);
        const float right[3] = {c, s, 0.0f};
        const float forward[3] = {-s, c, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const auto cellX = static_cast<std::int16_t>(std::floor(x / kWorldCellSize));
        const auto cellY = static_cast<std::int16_t>(std::floor(y / kWorldCellSize));
        const std::uint32_t cell = static_cast<std::uint16_t>(cellX) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(cellY)) << 16);
        float w = 0.0f;
        std::memcpy(&w, &cell, sizeof(w));
        const float column3[4] = {x - cellX * kWorldCellSize, y - cellY * kWorldCellSize, z, w};
        // Right, forward, up, position: as DecodeWorldMatrix reads them.
        for (int i = 0; i < 3; ++i)
        {
            world[i] = right[i];
            world[4 + i] = forward[i];
            world[8 + i] = up[i];
        }
        world[3] = 0.0f;
        world[7] = 0.0f;
        world[11] = 0.0f;
        for (int i = 0; i < 4; ++i)
            world[12 + i] = column3[i];
    }

    // One engine sample: the locals the detour copies, the world matrix and
    // the frame time; objects are composed by the pipeline.
    struct EngineFrame
    {
        std::uint32_t timeMs = 0;
        float world[16] = {};
        std::vector<BoneQts> locals;
    };

    struct EngineSkeleton
    {
        std::vector<std::uint16_t> parents; // the finalizer's indices table
        float rootScale = 1.0f;
        SkeletonLayout layout; // the track's layout
        // false: a layout that is not an engine one (a clip's discovered
        // layout); the frames' objects then come from the layout itself, as
        // the engine's objects would agree with it, and only the encoders
        // are compared.
        bool engine = true;
    };

    // ---------------------------------------------------------------------
    // The worker's frame pipeline, reference against shipped: composition,
    // validation, rebase flag, encoding. Returns false on a divergence.
    struct PipelineTimes
    {
        double composeOld = 0.0, composeNew = 0.0;
        double validateOld = 0.0;
        double encodeOld = 0.0, encodeNew = 0.0;
        std::uint64_t boneFrames = 0;
        std::uint64_t frames = 0;
        std::uint64_t deltaChannels = 0, rawSameChannels = 0, rawSameFrames = 0;
        std::uint64_t failingOld = 0; // FindFailingBones hits on composed frames (must be 0)
        std::uint64_t composedBones = 0; // bones the new pipeline composed
        std::uint64_t layoutBones = 0;
        void Add(const PipelineTimes& other)
        {
            composeOld += other.composeOld; composeNew += other.composeNew; validateOld += other.validateOld;
            encodeOld += other.encodeOld; encodeNew += other.encodeNew; boneFrames += other.boneFrames;
            frames += other.frames; deltaChannels += other.deltaChannels; rawSameChannels += other.rawSameChannels;
            rawSameFrames += other.rawSameFrames; failingOld += other.failingOld; composedBones += other.composedBones;
            layoutBones += other.layoutBones;
        }
    };

    StreamConfig BigBudget()
    {
        StreamConfig config;
        config.byteBudget = ~std::size_t{0} >> 1;
        return config;
    }

    // Runs both pipelines over the frames. `engine` true: locals-only frames
    // composed with the table (the skips apply when the layout is exact);
    // false: the frames carry their objects (composed here with the table too,
    // as the engine's), validated by both, encoded by both.
    bool RunPipelines(const EngineSkeleton& skeleton, const std::vector<EngineFrame>& frames, PipelineTimes& times,
        const char* what, bool timed)
    {
        const std::uint32_t boneCount = static_cast<std::uint32_t>(skeleton.parents.size());
        const bool exact = IsExactEngineLayout(skeleton.layout, skeleton.parents.data(), boneCount, skeleton.rootScale);
        std::vector<std::uint32_t> composeBones;
        if (exact)
            EngineComposeBones(skeleton.layout, skeleton.parents.data(), boneCount, composeBones);

        v170::Track oldTrack;
        oldTrack.Begin(skeleton.layout, BigBudget());
        PoseTrack newTrack;
        newTrack.Begin(skeleton.layout, BigBudget());

        std::vector<BoneQts> oldPose(static_cast<std::size_t>(boneCount) * 2u);
        std::vector<BoneQts> newPose(static_cast<std::size_t>(boneCount) * 2u);
        // Stale objects in the new pipeline's buffer, as a recycled ring slot
        // holds: whatever a partial composition leaves must never be read.
        std::mt19937 rng(1234u + boneCount);
        for (BoneQts& bone : newPose)
        {
            bone.rotation = RandomUnit(rng);
            bone.translation[0] = 123.0f;
        }
        bool haveProbeOld = false, haveProbeNew = false;
        RebaseProbe probeOld, probeNew;
        bool ok = true;
        for (const EngineFrame& frame : frames)
        {
            std::memcpy(oldPose.data(), frame.locals.data(), sizeof(BoneQts) * boneCount);
            std::memcpy(newPose.data(), frame.locals.data(), sizeof(BoneQts) * boneCount);
            BoneQts* oldObjects = oldPose.data() + boneCount;
            BoneQts* newObjects = newPose.data() + boneCount;

            if (!skeleton.engine)
            {
                // Engine objects that agree with the layout (script sample).
                std::vector<BoneQts> unresolved;
                for (const std::uint32_t bone : skeleton.layout.unresolvedBones)
                    unresolved.push_back(frame.locals[bone]);
                ComposeObjects(skeleton.layout, oldPose.data(), unresolved.data(), oldObjects);
                std::memcpy(newObjects, oldObjects, sizeof(BoneQts) * boneCount);
            }
            double t0 = timed ? NowUs() : 0.0;
            if (skeleton.engine)
                v170::ComposeEngineObjects(skeleton.parents.data(), boneCount, skeleton.rootScale, oldPose.data(), oldObjects);
            double t1 = timed ? NowUs() : 0.0;
            times.composeOld += t1 - t0;
            const std::vector<std::uint32_t> failing =
                FindFailingBones(skeleton.layout, oldPose.data(), oldObjects, 0.002f, 0.004f);
            double t2 = timed ? NowUs() : 0.0;
            times.validateOld += t2 - t1;
            times.failingOld += failing.size();

            t0 = timed ? NowUs() : 0.0;
            if (exact)
            {
                ComposeEngineObjectsSubset(skeleton.parents.data(), boneCount, skeleton.rootScale, composeBones,
                    newPose.data(), newObjects);
                times.composedBones += composeBones.size();
            }
            else if (skeleton.engine)
            {
                ComposeEngineObjects(skeleton.parents.data(), boneCount, skeleton.rootScale, newPose.data(), newObjects);
                times.composedBones += boneCount;
                if (!SameBytes(newObjects, oldObjects, sizeof(BoneQts) * boneCount))
                {
                    std::fprintf(stderr, "  %s: moved ComposeEngineObjects differs\n", what);
                    ok = false;
                }
            }
            t1 = timed ? NowUs() : 0.0;
            times.composeNew += t1 - t0;
            // The composed bones are the reference values bit for bit.
            if (exact)
            {
                for (const std::uint32_t bone : composeBones)
                {
                    if (!SameBytes(&newObjects[bone], &oldObjects[bone], sizeof(BoneQts)))
                    {
                        std::fprintf(stderr, "  %s: partial composition differs at bone %u\n", what, bone);
                        ok = false;
                        break;
                    }
                }
                if (!failing.empty())
                {
                    std::fprintf(stderr, "  %s: FindFailingBones found %zu bones on an exact engine layout\n", what,
                        failing.size());
                    ok = false;
                }
            }

            FrameInput oldInput;
            oldInput.timeMs = frame.timeMs;
            oldInput.world = frame.world;
            oldInput.locals = oldPose.data();
            oldInput.objects = oldObjects;
            FrameInput newInput = oldInput;
            newInput.locals = newPose.data();
            newInput.objects = newObjects;
            WorldTransform world{};
            if (DecodeWorldMatrix(frame.world, world))
            {
                const RebaseProbe a = MakeRebaseProbe(world, oldObjects[0], frame.timeMs);
                if (haveProbeOld && IsRebase(probeOld, a))
                    oldInput.flags |= kFrameNoInterpolation;
                probeOld = a;
                haveProbeOld = true;
                const RebaseProbe b = MakeRebaseProbe(world, newObjects[0], frame.timeMs);
                if (haveProbeNew && IsRebase(probeNew, b))
                    newInput.flags |= kFrameNoInterpolation;
                probeNew = b;
                haveProbeNew = true;
            }
            if (oldInput.flags != newInput.flags)
            {
                std::fprintf(stderr, "  %s: rebase flag differs at %u ms\n", what, frame.timeMs);
                ok = false;
            }

            // A frame that fails validation is held by the worker (re-parent
            // job): neither pipeline encodes it here, as neither would.
            if (!failing.empty() && !exact)
                continue;
            t0 = timed ? NowUs() : 0.0;
            oldTrack.Append(oldInput);
            t1 = timed ? NowUs() : 0.0;
            const EncodeStats encoded = newTrack.Append(newInput);
            t2 = timed ? NowUs() : 0.0;
            times.encodeOld += t1 - t0;
            times.encodeNew += t2 - t1;
            times.deltaChannels += encoded.deltaChannels;
            times.rawSameChannels += encoded.rawSameChannels;
            times.rawSameFrames += encoded.rawSameFrame ? 1u : 0u;
            times.boneFrames += boneCount;
            ++times.frames;
        }
        times.layoutBones += boneCount;

        TrackExport exported;
        newTrack.Export(exported);
        if (exported.chunks.size() != oldTrack.chunks.size())
        {
            std::fprintf(stderr, "  %s: %zu chunks against %zu\n", what, exported.chunks.size(), oldTrack.chunks.size());
            return false;
        }
        for (std::size_t index = 0; index < exported.chunks.size(); ++index)
        {
            const TrackChunkExport& a = exported.chunks[index];
            const v170::Chunk& b = oldTrack.chunks[index];
            if (a.bytes != b.bytes || a.frameTimes != b.frameTimes || (a.anchor >= 0) != b.relative)
            {
                std::size_t at = 0;
                while (at < a.bytes.size() && at < b.bytes.size() && a.bytes[at] == b.bytes[at])
                    ++at;
                std::fprintf(stderr, "  %s: chunk %zu differs (bytes %zu against %zu, first difference at %zu)\n", what,
                    index, a.bytes.size(), b.bytes.size(), at);
                return false;
            }
        }
        return ok;
    }

    // ---------------------------------------------------------------------
    // Synthetic engine skeletons and motion on the thresholds.
    EngineSkeleton MakeSkeleton(std::uint32_t boneCount, float rootScale, std::uint32_t seed, std::uint32_t outOfOrder)
    {
        std::mt19937 rng(seed);
        EngineSkeleton skeleton;
        skeleton.rootScale = rootScale;
        skeleton.parents.assign(boneCount, 0);
        skeleton.parents[0] = 0xFFFF;
        for (std::uint32_t bone = 1; bone < boneCount; ++bone)
        {
            // Mostly chains (the previous bone), sometimes a branch back.
            const std::uint32_t pick = rng() % 100u;
            if (pick < 60)
                skeleton.parents[bone] = static_cast<std::uint16_t>(bone - 1);
            else
                skeleton.parents[bone] = static_cast<std::uint16_t>(rng() % bone);
        }
        // Bones whose table parent is not an earlier bone (object space).
        for (std::uint32_t index = 0; index < outOfOrder && boneCount > 2; ++index)
        {
            const std::uint32_t bone = 1 + rng() % (boneCount - 1);
            skeleton.parents[bone] = (rng() & 1u) ? 0xFFFFu
                : static_cast<std::uint16_t>(bone + rng() % (boneCount - bone));
        }
        DiscoveryStats stats;
        const bool rootScaled = std::fabs(rootScale - 1.0f) > 1e-6f;
        BuildLayoutFromParents(skeleton.parents.data(), boneCount, rootScaled, {}, skeleton.layout, stats);
        return skeleton;
    }

    // Motion classes per bone: still (bind pose bits), creeping (steps around
    // the rotation and translation thresholds), moving, jittering in place,
    // far translations (float path), scale and opaque w changes.
    std::vector<EngineFrame> MakeFrames(const EngineSkeleton& skeleton, std::uint32_t frameCount, std::uint32_t seed,
        std::uint32_t startMs)
    {
        const std::uint32_t boneCount = static_cast<std::uint32_t>(skeleton.parents.size());
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<BoneQts> bind(boneCount);
        std::vector<std::uint8_t> kind(boneCount);
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            bind[bone].rotation = RandomUnit(rng);
            for (int axis = 0; axis < 3; ++axis)
                bind[bone].translation[axis] = (unit(rng) - 0.5f) * 0.4f;
            bind[bone].translation[3] = 0.04773f;
            const float pick = unit(rng);
            kind[bone] = pick < 0.55f ? 0 : pick < 0.75f ? 1 : pick < 0.85f ? 2 : pick < 0.93f ? 3 : pick < 0.97f ? 4 : 5;
            if (kind[bone] == 4)
                bind[bone].translation[0] = 3.9f + unit(rng) * 0.2f; // around the s16 range
        }
        std::vector<EngineFrame> frames(frameCount);
        std::vector<BoneQts> current = bind;
        float heading = 0.3f, x = 1000.0f, y = -2000.0f, z = 40.0f;
        for (std::uint32_t index = 0; index < frameCount; ++index)
        {
            EngineFrame& frame = frames[index];
            frame.timeMs = startMs + index * 33u + (index % 3 == 0 ? 1u : 0u);
            x += 0.05f;
            if (index % 97 == 50)
                x += 5.0f; // a teleport: the rebase flag
            if (index % 131 == 70)
                heading += 2.5f; // a heading rebase
            MakeWorld(heading, x, y, z, frame.world);
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                BoneQts& b = current[bone];
                switch (kind[bone])
                {
                case 0: // still: the same bytes every frame
                    break;
                case 1: // creeping: steps of 0.5 to 1.5 times the epsilons
                {
                    const float step = 0.002f * (0.5f + unit(rng));
                    b.rotation = Normalize(Multiply(b.rotation, AxisAngle(RandomAxis(rng), step)));
                    b.translation[1] += 0.0005f * (0.5f + unit(rng)) * (unit(rng) < 0.5f ? -1.0f : 1.0f);
                    break;
                }
                case 2: // moving
                    b.rotation = Normalize(Multiply(b.rotation, AxisAngle(RandomAxis(rng), 0.05f)));
                    b.translation[2] += 0.01f;
                    break;
                case 3: // jittering around the bind pose, sometimes still
                    if (unit(rng) < 0.5f)
                    {
                        b = bind[bone];
                        b.rotation = Normalize(Multiply(b.rotation, AxisAngle(RandomAxis(rng), 0.004f * unit(rng))));
                    }
                    break;
                case 4: // across the translation range (s16 or float)
                    b.translation[0] = 3.9f + unit(rng) * 0.2f;
                    break;
                default: // scale and opaque w
                    if (unit(rng) < 0.3f)
                    {
                        b.scale[0] = 1.0f + unit(rng) * 0.1f;
                        b.translation[3] = unit(rng);
                    }
                    break;
                }
                // Now and then a quaternion in the other hemisphere.
                if (kind[bone] != 0 && unit(rng) < 0.05f)
                    b.rotation = {-b.rotation.x, -b.rotation.y, -b.rotation.z, -b.rotation.w};
            }
            frame.locals = current;
        }
        return frames;
    }

    void TestSyntheticPipelines()
    {
        struct Case
        {
            std::uint32_t bones;
            float rootScale;
            std::uint32_t outOfOrder;
        };
        const Case cases[] = {
            {120, 1.0f, 0}, {420, 1.0f, 12}, {671, 0.919f, 9}, {1041, 1.0f, 25}, {300, 0.919f, 0},
        };
        PipelineTimes total;
        bool allOk = true;
        std::uint32_t seed = 7;
        for (const Case& test : cases)
        {
            const EngineSkeleton skeleton = MakeSkeleton(test.bones, test.rootScale, seed, test.outOfOrder);
            Check(IsExactEngineLayout(skeleton.layout, skeleton.parents.data(), test.bones, test.rootScale),
                "synthetic: the layout built from the table is exact");
            const std::vector<EngineFrame> frames = MakeFrames(skeleton, 400, seed * 3u, 1990u);
            PipelineTimes times;
            char what[64];
            std::snprintf(what, sizeof(what), "synthetic %u bones", test.bones);
            const bool ok = RunPipelines(skeleton, frames, times, what, false);
            allOk = allOk && ok;
            total.Add(times);
            ++seed;
        }
        Check(allOk, "synthetic skeletons: the current capture path records the reference track byte for byte");
        Check(total.failingOld == 0, "synthetic skeletons: FindFailingBones never fails a composed frame (H1)");

        // A layout changed by a demotion (or any non engine layout) is not
        // exact: its frames keep the validation.
        EngineSkeleton demoted = MakeSkeleton(200, 1.0f, 99u, 3);
        SkeletonLayout layout = demoted.layout;
        DemoteBones(layout, {17u, 40u});
        Check(!IsExactEngineLayout(layout, demoted.parents.data(), 200, 1.0f), "a demoted layout is not exact");
        Check(!IsExactEngineLayout(demoted.layout, demoted.parents.data(), 200, 1.0000005f),
            "a root scale a hair off 1 is not exact");
        layout = demoted.layout;
        layout.order = CompositionOrder::LocalThenParent;
        Check(!IsExactEngineLayout(layout, demoted.parents.data(), 200, 1.0f), "another composition order is not exact");

        // A demoted layout still records the same bytes through both
        // encoders (the validation runs in both; frames it fails are held).
        DemoteBones(demoted.layout, {17u, 40u});
        PipelineTimes times;
        Check(RunPipelines(demoted, MakeFrames(demoted, 200, 5u, 0u), times, "demoted layout", false),
            "demoted layout: both encoders record the same bytes");
    }

    // ---------------------------------------------------------------------
    // The encoder against the single-pass one (v175::Encoder), record by
    // record: the
    // bytes appended to a chunk, every EncodeStats field and the anchor flag,
    // with each shortcut combination, one encoder of each kind Begun again on
    // every skeleton (the record scratch grows and shrinks). Paths: a delta
    // frame right after Begin (no previous input), a relative keyframe
    // without an anchor (encoded full), full, relative and delta frames,
    // Begin again mid-run, Release then Begin. Frames: engine-like
    // motion, the same bytes again (every channel skipped), only the last
    // channel moved (an object-space one when there is one), w and scale
    // bytes alone, 0 against -0 alone, every channel moved by one ulp, NaN,
    // translations outside the s16 range, no world matrix. Then a layout
    // whose every channel is high precision encodes the largest records
    // there are (every channel with every field, full and two-pass delta),
    // which fill the record scratch to the byte.
    struct EncoderCompare
    {
        std::uint64_t records = 0, full = 0, relative = 0, delta = 0, twoPass = 0, allSame = 0;
        std::uint64_t bytes = 0, differ = 0;
    };

    bool SameStats(const EncodeStats& a, const EncodeStats& b)
    {
        return a.entries == b.entries && a.bytes == b.bytes && a.deltaChannels == b.deltaChannels &&
            a.rawSameChannels == b.rawSameChannels && a.rawSameFrame == b.rawSameFrame;
    }

    // One record through both encoders, each appending to its own chunk.
    EncodeStats EncodeBoth(PoseEncoder& now, v175::Encoder& old, const FrameInput& input, KeyframeKind kind,
        std::vector<std::uint8_t>& nowBytes, std::vector<std::uint8_t>& oldBytes, EncoderCompare& compare)
    {
        const std::size_t start = oldBytes.size();
        const bool sameStart = nowBytes.size() == start;
        const EncodeStats a = now.Encode(input, kind, nowBytes);
        const EncodeStats b = old.Encode(input, kind, oldBytes);
        const bool same = sameStart && SameStats(a, b) && now.HasAnchor() == old.HasAnchor() &&
            nowBytes.size() == oldBytes.size() && oldBytes.size() - start == b.bytes &&
            SameBytes(nowBytes.data() + start, oldBytes.data() + start, oldBytes.size() - start);
        std::uint16_t flags = 0;
        if (oldBytes.size() - start >= sizeof(std::uint32_t) * 2 + sizeof(flags))
            std::memcpy(&flags, oldBytes.data() + start + sizeof(std::uint32_t) * 2, sizeof(flags));
        ++compare.records;
        compare.full += (flags & kFrameKeyframe) ? 1u : 0u;
        compare.relative += (flags & kFrameRelativeKeyframe) ? 1u : 0u;
        compare.delta += (flags & (kFrameKeyframe | kFrameRelativeKeyframe)) ? 0u : 1u;
        compare.allSame += b.rawSameFrame ? 1u : 0u;
        compare.bytes += b.bytes;
        if (!same && ++compare.differ <= 3)
        {
            std::fprintf(stderr, "  encoder against reference: record %llu differs (kind %d, entries %u/%u, bytes %u/%u, "
                                 "unchanged channels %u/%u)\n",
                static_cast<unsigned long long>(compare.records), static_cast<int>(kind), a.entries, b.entries, a.bytes,
                b.bytes, a.rawSameChannels, b.rawSameChannels);
        }
        return b;
    }

    void TestEncoderAgainstV175()
    {
        struct Case
        {
            std::uint32_t bones;
            float rootScale;
            std::uint32_t outOfOrder;
        };
        // Small and large, with object-space channels (a root scale off 1
        // puts bone 0 in object space, out of order parents put others) and
        // without.
        const Case cases[] = {
            {2, 0.919f, 0}, {37, 1.0f, 3}, {120, 1.0f, 0}, {671, 0.919f, 9}, {300, 1.0f, 0}, {1041, 1.0f, 25},
        };
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const StreamConfig config{};
        EncoderCompare compare;
        for (int mode = 0; mode < 4; ++mode)
        {
            const bool rawSkip = (mode & 1) == 0; // mode 0: both shortcuts on, the default
            const bool cheap = (mode & 2) == 0;
            PoseEncoder::SetShortcuts(rawSkip, cheap);
            v175::Encoder::SetShortcuts(rawSkip, cheap);
            PoseEncoder now;
            v175::Encoder old;
            std::uint32_t seed = 101;
            for (const Case& test : cases)
            {
                const EngineSkeleton skeleton = MakeSkeleton(test.bones, test.rootScale, seed, test.outOfOrder);
                const SkeletonLayout& layout = skeleton.layout;
                const std::uint32_t boneCount = test.bones;
                const std::vector<EngineFrame> frames = MakeFrames(skeleton, 240, seed * 7u, 500u);
                ++seed;
                now.Begin(layout, config);
                old.Begin(layout, config);
                bool hasLast = false; // the encoder kept the previous input
                std::vector<std::uint8_t> nowBytes, oldBytes;
                std::vector<BoneQts> pose(static_cast<std::size_t>(boneCount) * 2u);
                std::vector<BoneQts> previous;
                for (std::size_t index = 0; index < frames.size(); ++index)
                {
                    const std::size_t shape = index % 10;
                    if (index > 0 && (shape == 3 || shape == 5 || shape == 7 || shape == 8))
                    {
                        pose = previous; // 3: the same bytes again
                        if (shape == 5)
                        {
                            // Only the last channel.
                            BoneQts& last = layout.unresolvedBones.empty()
                                ? pose[boneCount - 1]
                                : pose[boneCount + layout.unresolvedBones.back()];
                            last.rotation = Normalize(Multiply(last.rotation, AxisAngle({0.0f, 0.0f, 1.0f}, 0.01f)));
                        }
                        else if (shape == 7)
                        {
                            // w and scale bytes alone (and a zero for shape 8).
                            pose[0].translation[2] = 0.0f;
                            pose[0].translation[3] += 0.25f;
                            BoneQts& last = pose[boneCount - 1];
                            last.scale[1] = last.scale[1] == 1.25f ? 1.0f : 1.25f;
                        }
                        else if (shape == 8)
                        {
                            // The sign of a zero alone: other bytes, the same value.
                            pose[0].translation[2] = std::signbit(pose[0].translation[2]) ? 0.0f : -0.0f;
                        }
                    }
                    else
                    {
                        std::memcpy(pose.data(), frames[index].locals.data(), sizeof(BoneQts) * boneCount);
                        v170::ComposeEngineObjects(skeleton.parents.data(), boneCount, test.rootScale, pose.data(),
                            pose.data() + boneCount);
                        if (shape == 9)
                        {
                            // Every channel's bytes, by one ulp (under the epsilon).
                            for (BoneQts& bone : pose)
                                bone.translation[1] = std::nextafter(bone.translation[1], 1e30f);
                        }
                    }
                    if (index % 37 == 20)
                    {
                        pose[boneCount / 2].rotation.x = nan;
                        pose[0].translation[2] = nan;
                        if (!layout.unresolvedBones.empty())
                            pose[boneCount + layout.unresolvedBones.front()].translation[1] = nan;
                    }
                    if (index % 41 == 30)
                        pose[boneCount - 1].translation[0] = 7.5f; // outside the s16 range: float

                    KeyframeKind kind = KeyframeKind::None; // frame 0: a delta frame right after Begin
                    if (index == 1)
                        kind = KeyframeKind::Relative; // no anchor yet: encoded as a full keyframe
                    else if (index > 0 && index % 60 == 0)
                        kind = KeyframeKind::Full;
                    else if (index > 0 && index % 20 == 0)
                        kind = KeyframeKind::Relative;
                    if (index == 121)
                    {
                        // Begin again mid-run: a delta frame with no previous
                        // input, then relative keyframes without an anchor.
                        now.Begin(layout, config);
                        old.Begin(layout, config);
                        hasLast = false;
                    }
                    else if (index == 181)
                    {
                        now.Release(); // Release, then Begin
                        now.Begin(layout, config);
                        old.Begin(layout, config);
                        hasLast = false;
                    }

                    FrameInput input;
                    input.timeMs = frames[index].timeMs;
                    input.flags = static_cast<std::uint16_t>(index % 13 == 4 ? kFrameNoInterpolation : 0);
                    input.world = index % 17 == 6 ? nullptr : frames[index].world;
                    input.locals = pose.data();
                    input.objects = pose.data() + boneCount;
                    compare.twoPass += (kind == KeyframeKind::None && rawSkip && hasLast) ? 1u : 0u;
                    EncodeBoth(now, old, input, kind, nowBytes, oldBytes, compare);
                    hasLast = rawSkip;
                    previous = pose;
                }
            }
        }

        // The largest records: every channel high precision (64-bit
        // rotations), every translation outside the s16 range (floats), w and
        // scale off the state a full keyframe starts from; then every channel
        // moves every field (a two-pass delta frame with the default
        // shortcuts). Each is header + channels x 43 bytes, the room Begin
        // gives the record scratch. Begin's state holds the scratch and the
        // changed list; Release frees them with the rest.
        bool largestOk = true, memoryOk = true;
        std::uint64_t largestRecords = 0;
        std::size_t largestBytes = 0;
        {
            EngineSkeleton skeleton = MakeSkeleton(150, 0.919f, 77u, 6);
            SkeletonLayout& layout = skeleton.layout;
            layout.highPrecision.assign(layout.boneCount, 1);
            const std::uint32_t boneCount = layout.boneCount;
            const std::uint32_t channels = layout.ChannelCount();
            largestBytes = sizeof(std::uint32_t) * 2 + sizeof(std::uint16_t) * 2 + sizeof(float) * 16 +
                static_cast<std::size_t>(channels) * (sizeof(std::uint16_t) + sizeof(std::uint8_t) +
                    sizeof(std::uint64_t) + sizeof(float) * 3 + sizeof(float) + sizeof(float) * 4);
            std::mt19937 rng(9);
            std::vector<BoneQts> pose(static_cast<std::size_t>(boneCount) * 2u);
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                BoneQts& b = pose[bone];
                b.rotation = RandomUnit(rng);
                b.translation[0] = 5.0f + 0.01f * static_cast<float>(bone);
                b.translation[1] = -6.0f;
                b.translation[2] = 7.5f;
                b.translation[3] = 0.25f;
                b.scale[0] = b.scale[1] = b.scale[2] = 1.5f;
                pose[boneCount + bone] = b; // the object-space channels read these
            }
            std::vector<BoneQts> moved = pose;
            for (BoneQts& b : moved)
            {
                b.rotation = Normalize(Multiply(b.rotation, AxisAngle({1.0f, 0.0f, 0.0f}, 0.1f)));
                b.translation[0] += 0.5f;
                b.translation[3] += 0.25f;
                b.scale[1] = 2.0f;
            }
            for (int mode = 0; mode < 4; ++mode)
            {
                const bool rawSkip = (mode & 1) == 0;
                const bool cheap = (mode & 2) == 0;
                PoseEncoder::SetShortcuts(rawSkip, cheap);
                v175::Encoder::SetShortcuts(rawSkip, cheap);
                PoseEncoder now;
                v175::Encoder old;
                now.Begin(layout, config);
                old.Begin(layout, config);
                std::vector<std::uint8_t> nowBytes, oldBytes;
                FrameInput input;
                input.timeMs = 4000;
                input.locals = pose.data();
                input.objects = pose.data() + boneCount;
                const EncodeStats full = EncodeBoth(now, old, input, KeyframeKind::Full, nowBytes, oldBytes, compare);
                input.timeMs = 4033;
                input.locals = moved.data();
                input.objects = moved.data() + boneCount;
                compare.twoPass += rawSkip ? 1u : 0u;
                const EncodeStats delta = EncodeBoth(now, old, input, KeyframeKind::None, nowBytes, oldBytes, compare);
                largestOk = largestOk && full.bytes == largestBytes && delta.bytes == largestBytes &&
                    full.entries == channels && delta.entries == channels;
                largestRecords += 2;
                // The record scratch and the changed list are per thread,
                // outside StateBytes.
                const std::size_t held = static_cast<std::size_t>(channels) *
                        (2u * sizeof(BoneQts) + sizeof(std::uint64_t));
                memoryOk = memoryOk && now.StateBytes() >= held;
                now.Release();
                memoryOk = memoryOk && now.StateBytes() == 0;
            }
        }
        PoseEncoder::SetShortcuts(true, true);
        v175::Encoder::SetShortcuts(true, true);
        std::printf("  encoder current against reference (6 synthetic skeletons, 4 shortcut combinations): %llu records "
                    "(%llu full, %llu relative, %llu delta of which %llu two-pass and %llu with every channel unchanged), "
                    "%llu bytes, %llu differ; largest records (every channel, every field) %llu, %zu bytes each: %s\n",
            static_cast<unsigned long long>(compare.records), static_cast<unsigned long long>(compare.full),
            static_cast<unsigned long long>(compare.relative), static_cast<unsigned long long>(compare.delta),
            static_cast<unsigned long long>(compare.twoPass), static_cast<unsigned long long>(compare.allSame),
            static_cast<unsigned long long>(compare.bytes), static_cast<unsigned long long>(compare.differ),
            static_cast<unsigned long long>(largestRecords), largestBytes, largestOk ? "at the bound" : "WRONG SIZE");
        Check(compare.differ == 0, "encoder: the record scratch and the two-pass delta frame record the reference bytes and stats");
        Check(largestOk, "encoder: the largest records fill the scratch exactly (header + channels x 43 bytes)");
        Check(memoryOk, "encoder: Begin holds the record scratch and the changed list, Release frees them");
    }

    // ---------------------------------------------------------------------
    // The cheap rotation test primitives against the reference ones.
    void TestRounding()
    {
        // Every float in [2^-10, 2^21) of both signs (the callers' ranges are
        // [0, 2^20 - 1] and [-32767, 32767]), then specials and a sample of
        // the rest.
        std::uint64_t checked = 0;
        std::uint64_t differ = 0;
        const std::uint32_t first = 0x3A800000u; // 2^-10
        const std::uint32_t last = 0x4A000000u;  // 2^21
        for (std::uint32_t bits = first; bits < last; ++bits)
        {
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            if (RoundHalfAwayFromZero(value) != std::lround(value))
                ++differ;
            if (RoundHalfAwayFromZero(-value) != std::lround(-value))
                ++differ;
            checked += 2;
        }
        const float specials[] = {0.0f, -0.0f, 0.5f, -0.5f, 1.5f, -1.5f, 2.5f, -2.5f, 0.49999997f, -0.49999997f,
            8388607.5f, -8388607.5f, 8388608.0f, 16777217.0f, 1e20f, -1e20f, 1e-30f,
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        for (const float value : specials)
        {
            differ += RoundHalfAwayFromZero(value) != std::lround(value) ? 1u : 0u;
            ++checked;
        }
        std::mt19937 rng(42);
        for (int index = 0; index < 2000000; ++index)
        {
            const std::uint32_t bits = rng();
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            if (std::isnan(value))
                continue;
            differ += RoundHalfAwayFromZero(value) != std::lround(value) ? 1u : 0u;
            ++checked;
        }
        std::printf("  H4 rounding: %llu floats, %llu differ from std::lround\n",
            static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differ));
        Check(differ == 0, "RoundHalfAwayFromZero equals std::lround");

        // Quantization end to end.
        std::uint64_t packs = 0, packDiffer = 0;
        std::uniform_real_distribution<float> scale(0.5f, 1.5f);
        for (int index = 0; index < 3000000; ++index)
        {
            Quat q = RandomUnit(rng);
            if (index % 3 == 1)
                q = {q.x * scale(rng), q.y * scale(rng), q.z * scale(rng), q.w * scale(rng)};
            if (index % 7 == 0)
                q = UnpackQuat32(PackQuat32(q)); // on the grid
            packDiffer += PackQuat32(q) != v170::PackQuat32(q) ? 1u : 0u;
            packDiffer += PackQuat64(q) != v170::PackQuat64(q) ? 1u : 0u;
            packs += 2;
            float translation[3] = {(scale(rng) - 1.0f) * 9.0f, (scale(rng) - 1.0f) * 8.0f, (scale(rng) - 1.0f) * 2.0f};
            if (index % 11 == 0)
                translation[0] = 4.0f * static_cast<float>(static_cast<int>(rng() % 65535u) - 32767) / 32767.0f;
            std::int16_t a[3] = {}, b[3] = {};
            const bool okA = PackTranslation16(translation, 4.0f, a);
            const bool okB = v170::PackTranslation16(translation, 4.0f, b);
            packDiffer += (okA != okB || (okA && std::memcmp(a, b, sizeof(a)) != 0)) ? 1u : 0u;
            ++packs;
        }
        std::printf("  H4 quantization: %llu packs (32, 64 and 16 bits), %llu differ\n",
            static_cast<unsigned long long>(packs), static_cast<unsigned long long>(packDiffer));
        Check(packDiffer == 0, "PackQuat32/64 and PackTranslation16 equal the reference ones");
    }

    void TestCheapTests()
    {
        std::mt19937 rng(2024);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::uint64_t pairs = 0, differ = 0;
        const float epsilons[] = {0.002f, 0.0005f, 0.01f, 0.1f, 1.0f};
        for (const float epsilon : epsilons)
        {
            const AngleTest test = MakeAngleTest(epsilon);
            for (int index = 0; index < 1500000; ++index)
            {
                const Quat a = RandomUnit(rng);
                float angle = epsilon;
                const int shape = index % 8;
                if (shape <= 2)
                    angle = epsilon * (1.0f + (unit(rng) - 0.5f) * 0.2f); // within 10 percent
                else if (shape == 3)
                    angle = epsilon * (1.0f + (unit(rng) - 0.5f) * 2e-5f); // on the threshold
                else if (shape == 4)
                    angle = 3.14159265f - unit(rng) * 0.01f; // near pi
                else if (shape == 5)
                    angle = unit(rng) * 1e-5f; // near 0
                else
                    angle = unit(rng) * 3.14159f;
                Quat b = Multiply(a, AxisAngle(RandomAxis(rng), angle));
                Quat aa = a;
                if (rng() & 1u)
                    b = {-b.x, -b.y, -b.z, -b.w}; // other hemisphere
                if (index % 5 == 0)
                {
                    const float s = 0.72f + unit(rng) * 0.66f; // off-unit, inside the cheap range
                    aa = {a.x * s, a.y * s, a.z * s, a.w * s};
                }
                if (index % 13 == 0)
                    b = UnpackQuat32(PackQuat32(b));
                if (index % 17 == 0)
                    aa = UnpackQuat64(PackQuat64(aa));
                differ += AngleExceeds(test, aa, b) != (AngleBetween(aa, b) > epsilon) ? 1u : 0u;
                ++pairs;
            }
            // Degenerate inputs: zero, NaN, huge, tiny.
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const Quat odd[] = {{0, 0, 0, 0}, {nan, 0, 0, 1}, {0, 0, 0, nan}, {1e20f, 0, 0, 1e20f}, {1e-20f, 0, 0, 0},
                {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {0, 0, 0, -1}};
            for (const Quat& x : odd)
            {
                for (const Quat& y : odd)
                {
                    differ += AngleExceeds(test, x, y) != (AngleBetween(x, y) > epsilon) ? 1u : 0u;
                    ++pairs;
                }
            }
        }
        std::printf("  H4 angle test: %llu pairs (5 epsilons, 30%% within 10%% of the threshold), %llu differ\n",
            static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(differ));
        Check(differ == 0, "AngleExceeds equals AngleBetween > epsilon");

        std::uint64_t distances = 0, distanceDiffer = 0;
        for (const float epsilon : {0.0005f, 0.002f, 0.05f})
        {
            for (int index = 0; index < 1500000; ++index)
            {
                BoneQts a, b;
                for (int axis = 0; axis < 3; ++axis)
                    a.translation[axis] = (unit(rng) - 0.5f) * 8.0f;
                const Vec3 direction = RandomAxis(rng);
                const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
                const float distance = epsilon * (index % 2 ? (1.0f + (unit(rng) - 0.5f) * 2e-4f) : unit(rng) * 2.0f);
                b.translation[0] = a.translation[0] + direction.x / length * distance;
                b.translation[1] = a.translation[1] + direction.y / length * distance;
                b.translation[2] = a.translation[2] + direction.z / length * distance;
                distanceDiffer += TranslationExceeds(a, b, epsilon) != (TranslationDistance(a, b) > epsilon) ? 1u : 0u;
                ++distances;
            }
            BoneQts n;
            n.translation[0] = std::numeric_limits<float>::quiet_NaN();
            distanceDiffer += TranslationExceeds(n, BoneQts{}, epsilon) != (TranslationDistance(n, BoneQts{}) > epsilon) ? 1u : 0u;
            n.translation[0] = std::numeric_limits<float>::infinity();
            distanceDiffer += TranslationExceeds(n, BoneQts{}, epsilon) != (TranslationDistance(n, BoneQts{}) > epsilon) ? 1u : 0u;
            distances += 2;
        }
        std::printf("  H4 distance test: %llu pairs (half within 1e-4 of the threshold), %llu differ\n",
            static_cast<unsigned long long>(distances), static_cast<unsigned long long>(distanceDiffer));
        Check(distanceDiffer == 0, "TranslationExceeds equals TranslationDistance > epsilon");
    }

    // ---------------------------------------------------------------------
    // The memcmp comparison against CompareLatePose.
    bool SameChange(const LatePoseChange& a, const LatePoseChange& b)
    {
        return a.localBones == b.localBones && a.objectBones == b.objectBones && a.worstBone == b.worstBone &&
            SameBytes(&a.maxTranslation, &b.maxTranslation, sizeof(float)) &&
            SameBytes(&a.maxRotation, &b.maxRotation, sizeof(float));
    }

    struct LateBench
    {
        double oldUs = 0.0, newUs = 0.0;
        std::uint64_t checks = 0;
    };

    void TestLateCompare()
    {
        std::mt19937 rng(77);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::uint64_t cases = 0, differ = 0;
        LateBench equalBench, changedBench;
        for (const float rootScale : {1.0f, 0.919f})
        {
            for (const std::uint32_t boneCount : {94u, 671u, 1041u})
            {
                const EngineSkeleton skeleton = MakeSkeleton(boneCount, rootScale, boneCount + 5u, 8);
                const std::vector<EngineFrame> frames = MakeFrames(skeleton, 24, boneCount, 0);
                std::vector<BoneQts> scratchOld, scratchNew;
                std::vector<std::uint8_t> mask;
                for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
                {
                    // The finalizer's output: locals and the objects it
                    // composes from them with the table.
                    std::vector<BoneQts> finalizer(static_cast<std::size_t>(boneCount) * 2u);
                    std::memcpy(finalizer.data(), frames[frameIndex].locals.data(), sizeof(BoneQts) * boneCount);
                    v170::ComposeEngineObjects(skeleton.parents.data(), boneCount, rootScale, finalizer.data(),
                        finalizer.data() + boneCount);
                    for (int variant = 0; variant < 8; ++variant)
                    {
                        std::vector<BoneQts> late = finalizer;
                        BoneQts* lateLocals = late.data();
                        BoneQts* lateObjects = late.data() + boneCount;
                        const std::uint32_t pick = 1 + rng() % (boneCount - 1);
                        switch (variant)
                        {
                        case 0: // untouched (almost every check in the game)
                            break;
                        case 1: // a local rewritten after the finalizer (IK), objects not
                            lateLocals[pick].rotation = Normalize(Multiply(lateLocals[pick].rotation, AxisAngle({0, 0, 1}, 0.05f)));
                            break;
                        case 2: // an object written directly (ragdoll), a few bones
                            for (int k = 0; k < 3; ++k)
                                lateObjects[1 + rng() % (boneCount - 1)].translation[0] += 0.02f;
                            break;
                        case 3: // below the tolerances, many bones
                            for (std::uint32_t bone = 0; bone < boneCount; bone += 3)
                            {
                                lateLocals[bone].translation[1] += 1e-4f;
                                lateObjects[bone].translation[2] += 1e-4f;
                            }
                            break;
                        case 4: // bone 0, local and object
                            lateLocals[0].translation[0] += 0.05f;
                            lateObjects[0].rotation = Normalize(Multiply(lateObjects[0].rotation, AxisAngle({1, 0, 0}, 0.03f)));
                            break;
                        case 5: // a NaN
                            lateLocals[pick].translation[1] = std::numeric_limits<float>::quiet_NaN();
                            break;
                        case 6: // local and object moved together, consistent
                            lateLocals[pick].translation[0] += 0.03f;
                            lateObjects[pick] = Compose(lateObjects[skeleton.parents[pick] < pick ? skeleton.parents[pick] : 0],
                                lateLocals[pick], CompositionOrder::ParentThenLocal, true);
                            break;
                        default: // the whole pose replaced (a new animation frame)
                            if (frameIndex + 1 < frames.size())
                            {
                                std::memcpy(lateLocals, frames[frameIndex + 1].locals.data(), sizeof(BoneQts) * boneCount);
                                v170::ComposeEngineObjects(skeleton.parents.data(), boneCount, rootScale, lateLocals, lateObjects);
                            }
                            break;
                        }
                        const double t0 = NowUs();
                        const LatePoseChange expected = CompareLatePose(finalizer.data(), lateLocals, lateObjects,
                            skeleton.parents.data(), boneCount, rootScale, 0.01f, 0.01f, scratchOld);
                        const double t1 = NowUs();
                        bool equalBlocks = false;
                        const LatePoseChange actual = CompareLatePoseChanged(finalizer.data(), finalizer.data() + boneCount,
                            lateLocals, lateObjects, skeleton.parents.data(), boneCount, rootScale, 0.01f, 0.01f,
                            scratchNew, mask, equalBlocks);
                        const double t2 = NowUs();
                        LateBench& bench = SameBytes(late.data(), finalizer.data(), late.size() * sizeof(BoneQts))
                            ? equalBench : changedBench;
                        bench.oldUs += t1 - t0;
                        bench.newUs += t2 - t1;
                        ++bench.checks;
                        ++cases;
                        const bool unchanged = SameBytes(late.data(), finalizer.data(), late.size() * sizeof(BoneQts));
                        if (!SameChange(expected, actual) || unchanged != equalBlocks)
                        {
                            ++differ;
                            if (differ < 5)
                            {
                                std::fprintf(stderr, "  late compare: %u bones variant %d: %u/%u bones against %u/%u\n",
                                    boneCount, variant, actual.localBones, actual.objectBones, expected.localBones,
                                    expected.objectBones);
                            }
                        }
                    }
                }
            }
        }
        std::printf("  H5 late compare: %llu checks, %llu differ; us per check: unchanged buffer %.2f -> %.2f, "
                    "changed %.2f -> %.2f\n",
            static_cast<unsigned long long>(cases), static_cast<unsigned long long>(differ),
            equalBench.oldUs / std::max<double>(1.0, static_cast<double>(equalBench.checks)),
            equalBench.newUs / std::max<double>(1.0, static_cast<double>(equalBench.checks)),
            changedBench.oldUs / std::max<double>(1.0, static_cast<double>(changedBench.checks)),
            changedBench.newUs / std::max<double>(1.0, static_cast<double>(changedBench.checks)));
        Check(differ == 0, "CompareLatePoseChanged equals CompareLatePose over a finalizer output");
    }

    // ---------------------------------------------------------------------
    // Micro-benchmarks: per phase, ns per bone per frame, best of runs.
    void PrintTimes(const char* what, const PipelineTimes& t)
    {
        const double bf = static_cast<double>(std::max<std::uint64_t>(1, t.boneFrames)) / 1000.0; // us -> ns per bone frame
        const double composedShare = t.boneFrames
            ? 100.0 * static_cast<double>(t.composedBones) / static_cast<double>(t.boneFrames) : 0.0;
        const double rawShare = t.deltaChannels
            ? 100.0 * static_cast<double>(t.rawSameChannels) / static_cast<double>(t.deltaChannels) : 0.0;
        std::printf("    %s: ns per bone per frame: compose %.1f -> %.1f (%.1f%% of the bones), validate %.1f -> 0, "
                    "encode %.1f -> %.1f; worker total %.1f -> %.1f (x%.2f); unchanged channels %.1f%%, frames %llu of %llu\n",
            what, t.composeOld / bf, t.composeNew / bf, composedShare, t.validateOld / bf, t.encodeOld / bf,
            t.encodeNew / bf, (t.composeOld + t.validateOld + t.encodeOld) / bf, (t.composeNew + t.encodeNew) / bf,
            (t.composeOld + t.validateOld + t.encodeOld) / std::max(1e-9, t.composeNew + t.encodeNew), rawShare,
            static_cast<unsigned long long>(t.rawSameFrames), static_cast<unsigned long long>(t.frames));
    }

    // The encoder alone with each shortcut on its own (same bytes every way).
    void PrintEncoderSplit(const char* what, const EngineSkeleton& skeleton, const std::vector<EngineFrame>& frames)
    {
        const std::uint32_t boneCount = static_cast<std::uint32_t>(skeleton.parents.size());
        std::vector<std::vector<BoneQts>> composed(frames.size());
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            composed[index].resize(static_cast<std::size_t>(boneCount) * 2u);
            std::memcpy(composed[index].data(), frames[index].locals.data(), sizeof(BoneQts) * boneCount);
            v170::ComposeEngineObjects(skeleton.parents.data(), boneCount, skeleton.rootScale, composed[index].data(),
                composed[index].data() + boneCount);
        }
        std::vector<std::vector<std::uint8_t>> reference; // the reference's chunks
        bool combinationsSame = true;
        auto run = [&](int mode) {
            double best = 1e300;
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                v170::Track oldTrack;
                PoseTrack newTrack;
                if (mode == 0)
                    oldTrack.Begin(skeleton.layout, BigBudget());
                else
                    newTrack.Begin(skeleton.layout, BigBudget());
                PoseEncoder::SetShortcuts(mode == 1 || mode == 3, mode == 2 || mode == 3);
                SetFastRounding(mode == 2 || mode == 3);
                const double t0 = NowUs();
                for (std::size_t index = 0; index < frames.size(); ++index)
                {
                    FrameInput input;
                    input.timeMs = frames[index].timeMs;
                    input.world = frames[index].world;
                    input.locals = composed[index].data();
                    input.objects = composed[index].data() + boneCount;
                    if (mode == 0)
                        oldTrack.Append(input);
                    else
                        newTrack.Append(input);
                }
                best = std::min(best, NowUs() - t0);
                if (repeat == 0 && mode == 0)
                {
                    for (const v170::Chunk& chunk : oldTrack.chunks)
                        reference.push_back(chunk.bytes);
                }
                else if (repeat == 0)
                {
                    TrackExport exported;
                    newTrack.Export(exported);
                    bool same = exported.chunks.size() == reference.size();
                    for (std::size_t index = 0; same && index < reference.size(); ++index)
                        same = exported.chunks[index].bytes == reference[index];
                    combinationsSame = combinationsSame && same;
                }
            }
            PoseEncoder::SetShortcuts(true, true);
            SetFastRounding(true);
            return best;
        };
        const double bf = static_cast<double>(frames.size()) * boneCount / 1000.0;
        const double oldNs = run(0) / bf;
        const double noneNs = run(4) / bf;
        const double rawNs = run(1) / bf;
        const double cheapNs = run(2) / bf;
        const double bothNs = run(3) / bf;
        std::printf("    %s encoder, ns per bone per frame: reference %.1f; current without shortcuts %.1f, "
                    "H3 only %.1f, H4 only %.1f, H3+H4 %.1f\n",
            what, oldNs, noneNs, rawNs, cheapNs, bothNs);
        Check(combinationsSame, "encoder: every shortcut combination records the reference bytes");
    }

    void BenchmarkSynthetic()
    {
        std::printf("  capture micro-benchmark (synthetic, 1041 and 671 bones, 600 frames):\n");
        for (const std::uint32_t bones : {1041u, 671u})
        {
            const EngineSkeleton skeleton = MakeSkeleton(bones, bones == 671u ? 0.919f : 1.0f, 31u + bones, 10);
            const std::vector<EngineFrame> frames = MakeFrames(skeleton, 600, 17u, 0);
            PipelineTimes best;
            double bestTotal = 1e300;
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                PipelineTimes times;
                RunPipelines(skeleton, frames, times, "bench", true);
                const double total = times.composeNew + times.encodeNew;
                if (total < bestTotal)
                {
                    bestTotal = total;
                    best = times;
                }
            }
            char what[48];
            std::snprintf(what, sizeof(what), "%u bones", bones);
            PrintTimes(what, best);
            PrintEncoderSplit(what, skeleton, frames);
        }
    }

    // ---------------------------------------------------------------------
    // Recorded clips (REPLAYPOC_POSE_CLIP_DIR): each track's frames decoded
    // into engine locals on the clip's own layout.
    // The engine parent table a layout stands for, when it is an engine
    // layout (ParentThenLocal, inherited scale, parents before children).
    bool EngineTableFor(const SkeletonLayout& layout, std::vector<std::uint16_t>& parents, float& rootScale)
    {
        if (layout.order != CompositionOrder::ParentThenLocal || !layout.inheritScale || layout.boneCount == 0)
            return false;
        parents.assign(layout.boneCount, 0xFFFF);
        if (layout.parent[0] == kRootParent)
            rootScale = 1.0f;
        else if (layout.parent[0] == kUnresolvedParent)
            rootScale = 0.919f;
        else
            return false;
        for (std::uint32_t bone = 1; bone < layout.boneCount; ++bone)
        {
            const std::int32_t parent = layout.parent[bone];
            if (parent >= 0)
            {
                if (static_cast<std::uint32_t>(parent) >= bone)
                    return false;
                parents[bone] = static_cast<std::uint16_t>(parent);
            }
            else if (parent == kRootParent)
            {
                return false;
            }
        }
        return true;
    }

    // Decodes a chunk's frames; engine locals: the local channels, and for
    // an object-space bone its object channel (the engine table keeps its
    // local as its object).
    void DecodeChunk(const TrackExport& track, const TrackChunkExport& chunk, std::vector<EngineFrame>& out,
        std::size_t limit, bool noisy, std::mt19937& rng)
    {
        const SkeletonLayout& layout = track.layouts[chunk.layout];
        PoseDecoder decoder;
        decoder.Begin(layout, track.config);
        std::size_t consumed = 0;
        if (chunk.anchor >= 0)
        {
            const std::vector<std::uint8_t>& anchor = track.anchors[static_cast<std::size_t>(chunk.anchor)];
            if (!decoder.Apply(anchor.data(), anchor.size(), consumed))
                return;
        }
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (std::size_t frame = 0; frame < chunk.frameOffsets.size() && out.size() < limit; ++frame)
        {
            const std::size_t offset = chunk.frameOffsets[frame];
            if (!decoder.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed))
                return;
            EngineFrame engine;
            engine.timeMs = decoder.TimeMs();
            std::memcpy(engine.world, decoder.World(), sizeof(engine.world));
            const std::vector<BoneQts>& channels = decoder.Channels();
            engine.locals.assign(channels.begin(), channels.begin() + layout.boneCount);
            for (std::size_t index = 0; index < layout.unresolvedBones.size(); ++index)
            {
                const std::uint32_t bone = layout.unresolvedBones[index];
                if (bone != 0)
                    engine.locals[bone] = channels[layout.boneCount + index];
            }
            if (noisy)
            {
                // Around the thresholds on a fifth of the bones: rotations of
                // 0 to 3 mrad, translations of 0 to 0.8 mm.
                for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
                {
                    if (unit(rng) >= 0.2f)
                        continue;
                    BoneQts& b = engine.locals[bone];
                    b.rotation = Normalize(Multiply(b.rotation, AxisAngle(RandomAxis(rng), 0.003f * unit(rng))));
                    b.translation[0] += (unit(rng) - 0.5f) * 0.0016f;
                }
            }
            out.push_back(std::move(engine));
        }
    }

    void TestRecordedClipsFromEnv()
    {
        wchar_t folder[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"REPLAYPOC_POSE_CLIP_DIR", folder, MAX_PATH))
            return;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        {
            if (entry.path().extension() != L".rpclip")
                continue;
            replay::Recording recording;
            std::vector<PoseRecordExport> poses;
            replay::camera::Track camera;
            std::uint64_t duration = 0;
            std::string error;
            if (!replay::clipfile::Load(entry.path().wstring(), 0x140000000ull, 0x8000000, recording, poses, camera,
                    duration, error))
            {
                std::fprintf(stderr, "  capture clip %ls: not loaded (%s)\n", entry.path().filename().c_str(), error.c_str());
                continue;
            }
            for (const bool noisy : {false, true})
            {
                PipelineTimes total;
                std::size_t tracks = 0, engineTracks = 0, failures = 0;
                std::mt19937 rng(5);
                for (const PoseRecordExport& record : poses)
                {
                    const TrackExport& track = record.track;
                    if (track.chunks.empty())
                        continue;
                    // One run per layout the track used (a layout change
                    // restarts the encoder), at most 900 frames each.
                    std::size_t chunk = 0;
                    while (chunk < track.chunks.size())
                    {
                        const std::uint32_t layoutIndex = track.chunks[chunk].layout;
                        std::vector<EngineFrame> frames;
                        for (; chunk < track.chunks.size() && track.chunks[chunk].layout == layoutIndex; ++chunk)
                            DecodeChunk(track, track.chunks[chunk], frames, 900, noisy, rng);
                        if (frames.size() < 2)
                            continue;
                        EngineSkeleton skeleton;
                        skeleton.layout = track.layouts[layoutIndex];
                        const bool engine = EngineTableFor(skeleton.layout, skeleton.parents, skeleton.rootScale);
                        if (!engine)
                        {
                            // Not an engine layout: the encoder comparison
                            // alone, objects composed on the layout.
                            skeleton.parents.assign(skeleton.layout.boneCount, 0xFFFF);
                            skeleton.rootScale = 1.0f;
                            skeleton.engine = false;
                        }
                        ++tracks;
                        engineTracks += engine ? 1u : 0u;
                        PipelineTimes times;
                        char what[96];
                        std::snprintf(what, sizeof(what), "clip record 0x%x (%u bones)", record.recordId, record.boneCount);
                        if (!RunPipelines(skeleton, frames, times, what, true))
                            ++failures;
                        total.Add(times);
                    }
                }
                std::printf("  capture clip %ls (%s): %zu tracks (%zu engine layouts), %llu frames, %zu differ\n",
                    entry.path().filename().c_str(), noisy ? "noise on the thresholds" : "as recorded", tracks,
                    engineTracks, static_cast<unsigned long long>(total.frames), failures);
                PrintTimes(noisy ? "noisy" : "recorded", total);
                Check(failures == 0, "recorded clip: the current capture path records the reference track byte for byte");
            }
        }
    }

    // ---------------------------------------------------------------------
    // Is the encoder slower in the game than offline because its state is
    // cold when the worker comes back to a record? Every layout run of every track of a clip (REPLAYPOC_POSE_CLIP_DIR,
    // at most kCacheBenchFrames frames each, decoded as recorded) is encoded
    // by a track of its own, the same frames in the same order per track,
    // four ways: sequentially (each track whole: its state stays in the
    // core's caches); interleaved (one frame of each track per round, the
    // worker's order: every other track's state and input pass through the
    // caches before a track comes back); interleaved with the caches flushed
    // between rounds by reading 32 MB; and the same with a buffer of twice
    // the last-level cache (32 MB does not flush a 96 MB L3, as on a
    // 5700X3D). In the interleaved ways every track's frame of the round is
    // decoded before the round's encodes (the detour wrote the input before
    // the worker came), and the flush comes between the decodes and the
    // encodes, so the input is cold as well, as the detour's streaming
    // stores leave it. Each Append is timed alone (ns per bone per frame).
    // The same runs with the single-pass encoder (v175::Track) are the
    // baseline. All eight runs must record the same chunks with the same
    // EncodeStats per frame.
    constexpr std::size_t kCacheBenchFrames = 240; // per layout run (8 s at 30 Hz)

    struct CacheRun
    {
        const TrackExport* track = nullptr;
        std::size_t firstChunk = 0;
        std::size_t endChunk = 0;
        std::uint32_t layout = 0;
    };

    // A run's frames decoded one at a time as the encoder's input: the
    // locals are the decoder's channels (as recorded), an object-space
    // bone's object is its object channel.
    class RunFrames
    {
    public:
        explicit RunFrames(const CacheRun& run) : m_run(&run), m_chunk(run.firstChunk)
        {
            m_objects.resize(run.track->layouts[run.layout].boneCount);
        }

        // The next frame (valid until the next call); false after the last.
        bool Next(FrameInput& input)
        {
            const TrackExport& track = *m_run->track;
            const SkeletonLayout& layout = track.layouts[m_run->layout];
            while (m_produced < kCacheBenchFrames && m_chunk < m_run->endChunk)
            {
                const TrackChunkExport& chunk = track.chunks[m_chunk];
                std::size_t consumed = 0;
                if (m_frame == 0)
                {
                    m_decoder.Begin(layout, track.config);
                    if (chunk.anchor >= 0)
                    {
                        const std::vector<std::uint8_t>& anchor = track.anchors[static_cast<std::size_t>(chunk.anchor)];
                        if (!m_decoder.Apply(anchor.data(), anchor.size(), consumed))
                            break;
                    }
                }
                if (m_frame >= chunk.frameOffsets.size())
                {
                    ++m_chunk;
                    m_frame = 0;
                    continue;
                }
                const std::size_t offset = chunk.frameOffsets[m_frame++];
                if (!m_decoder.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed))
                    break;
                const std::vector<BoneQts>& channels = m_decoder.Channels();
                for (std::size_t index = 0; index < layout.unresolvedBones.size(); ++index)
                    m_objects[layout.unresolvedBones[index]] = channels[layout.boneCount + index];
                input = FrameInput{};
                input.timeMs = m_decoder.TimeMs();
                input.flags = static_cast<std::uint16_t>(m_decoder.Flags() & kFrameNoInterpolation);
                input.world = m_decoder.World();
                input.locals = channels.data();
                input.objects = m_objects.data();
                ++m_produced;
                return true;
            }
            m_chunk = m_run->endChunk; // done (a record that does not decode ends the run)
            return false;
        }

    private:
        const CacheRun* m_run = nullptr;
        std::size_t m_chunk = 0;
        std::size_t m_frame = 0;
        std::size_t m_produced = 0;
        PoseDecoder m_decoder;
        std::vector<BoneQts> m_objects;
    };

    // Reads a byte of every 64-byte line: the caches then hold the buffer
    // instead of what was in them.
    std::uint64_t ReadEveryLine(const std::vector<std::uint8_t>& buffer)
    {
        std::uint64_t sum = 0;
        for (std::size_t at = 0; at < buffer.size(); at += 64)
            sum += buffer[at];
        return sum;
    }

    // The largest cache of the machine (the L3), 0 when unknown.
    std::size_t LastLevelCacheBytes()
    {
        DWORD length = 0;
        GetLogicalProcessorInformation(nullptr, &length);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
        length = static_cast<DWORD>(info.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (!GetLogicalProcessorInformation(info.data(), &length))
            return 0;
        std::size_t largest = 0;
        for (std::size_t index = 0; index < length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++index)
        {
            if (info[index].Relationship == RelationCache)
                largest = std::max<std::size_t>(largest, info[index].Cache.Size);
        }
        return largest;
    }

    struct ChunkList
    {
        std::vector<std::vector<std::uint8_t>> bytes;
        std::vector<std::vector<std::uint32_t>> times;
        std::vector<std::uint8_t> relative;
        bool operator==(const ChunkList& other) const
        {
            return bytes == other.bytes && times == other.times && relative == other.relative;
        }
    };

    ChunkList ChunksOf(const PoseTrack& track)
    {
        TrackExport exported;
        track.Export(exported);
        ChunkList list;
        for (const TrackChunkExport& chunk : exported.chunks)
        {
            list.bytes.push_back(chunk.bytes.Get());
            list.times.push_back(chunk.frameTimes);
            list.relative.push_back(static_cast<std::uint8_t>(chunk.anchor >= 0 ? 1u : 0u));
        }
        return list;
    }

    ChunkList ChunksOf(const v175::Track& track)
    {
        ChunkList list;
        for (const v175::Chunk& chunk : track.chunks)
        {
            list.bytes.push_back(chunk.bytes);
            list.times.push_back(chunk.frameTimes);
            list.relative.push_back(static_cast<std::uint8_t>(chunk.relative ? 1u : 0u));
        }
        return list;
    }

    // One way of one encoder over every run: the Append time, each frame's
    // stats and each track's chunks.
    template <typename TrackType>
    void EncodeRuns(const std::vector<CacheRun>& runs, bool interleaved, const std::vector<std::uint8_t>* flush,
        double& us, std::vector<std::vector<EncodeStats>>& stats, std::vector<ChunkList>& chunks, std::uint64_t& sink)
    {
        std::vector<std::unique_ptr<TrackType>> tracks;
        std::vector<RunFrames> frames;
        frames.reserve(runs.size());
        stats.assign(runs.size(), {});
        for (const CacheRun& run : runs)
        {
            StreamConfig config = run.track->config;
            config.byteBudget = BigBudget().byteBudget;
            tracks.push_back(std::make_unique<TrackType>());
            tracks.back()->Begin(run.track->layouts[run.layout], config);
            frames.emplace_back(run);
        }
        FrameInput input;
        if (!interleaved)
        {
            for (std::size_t index = 0; index < runs.size(); ++index)
            {
                while (frames[index].Next(input))
                {
                    const double t0 = NowUs();
                    const EncodeStats encoded = tracks[index]->Append(input);
                    us += NowUs() - t0;
                    stats[index].push_back(encoded);
                }
            }
        }
        else
        {
            std::vector<FrameInput> inputs(runs.size());
            std::vector<std::uint8_t> ready(runs.size(), 0);
            for (;;)
            {
                bool any = false;
                for (std::size_t index = 0; index < runs.size(); ++index)
                {
                    ready[index] = static_cast<std::uint8_t>(frames[index].Next(inputs[index]) ? 1u : 0u);
                    any = any || ready[index] != 0;
                }
                if (!any)
                    break;
                if (flush)
                    sink += ReadEveryLine(*flush);
                for (std::size_t index = 0; index < runs.size(); ++index)
                {
                    if (!ready[index])
                        continue;
                    const double t0 = NowUs();
                    const EncodeStats encoded = tracks[index]->Append(inputs[index]);
                    us += NowUs() - t0;
                    stats[index].push_back(encoded);
                }
            }
        }
        chunks.clear();
        for (const std::unique_ptr<TrackType>& track : tracks)
            chunks.push_back(ChunksOf(*track));
    }

    void BenchmarkEncoderCacheFromEnv()
    {
        wchar_t folder[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"REPLAYPOC_POSE_CLIP_DIR", folder, MAX_PATH))
            return;
        const std::size_t lastLevel = LastLevelCacheBytes();
        const std::vector<std::uint8_t> flushSmall(std::size_t{32} << 20);
        const std::vector<std::uint8_t> flushLarge(std::max<std::size_t>(std::size_t{64} << 20, lastLevel * 2));
        const char* const wayNames[4] = {"sequential", "interleaved", "interleaved, 32 MB read between rounds",
            "interleaved, 2x last-level cache read between rounds"};
        double allUs[2][4] = {};
        std::uint64_t allBoneFrames = 0, allFrames = 0, allDelta = 0, allSame = 0, allDiffer = 0, allRuns = 0;
        std::uint64_t sink = 0;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        {
            if (entry.path().extension() != L".rpclip")
                continue;
            replay::Recording recording;
            std::vector<PoseRecordExport> poses;
            replay::camera::Track camera;
            std::uint64_t duration = 0;
            std::string error;
            if (!replay::clipfile::Load(entry.path().wstring(), 0x140000000ull, 0x8000000, recording, poses, camera,
                    duration, error))
            {
                std::fprintf(stderr, "  encoder cache clip %ls: not loaded (%s)\n", entry.path().filename().c_str(),
                    error.c_str());
                continue;
            }
            std::vector<CacheRun> runs;
            for (const PoseRecordExport& record : poses)
            {
                const TrackExport& track = record.track;
                std::size_t chunk = 0;
                while (chunk < track.chunks.size())
                {
                    CacheRun run;
                    run.track = &track;
                    run.firstChunk = chunk;
                    run.layout = track.chunks[chunk].layout;
                    std::size_t frames = 0;
                    for (; chunk < track.chunks.size() && track.chunks[chunk].layout == run.layout; ++chunk)
                        frames += track.chunks[chunk].frameOffsets.size();
                    run.endChunk = chunk;
                    if (frames >= 2 && track.layouts[run.layout].boneCount > 0)
                        runs.push_back(run);
                }
            }
            if (runs.empty())
                continue;
            // Way by way, the shipped encoder first (it pays any first use
            // of the heap), then the single-pass one; the first run is the
            // reference of the other seven.
            double us[2][4] = {};
            std::vector<std::vector<EncodeStats>> referenceStats;
            std::vector<ChunkList> referenceChunks;
            std::uint64_t differ = 0;
            for (int way = 0; way < 4; ++way)
            {
                const std::vector<std::uint8_t>* flush = way == 2 ? &flushSmall : way == 3 ? &flushLarge : nullptr;
                for (int encoder = 1; encoder >= 0; --encoder)
                {
                    std::vector<std::vector<EncodeStats>> stats;
                    std::vector<ChunkList> chunks;
                    if (encoder == 1)
                        EncodeRuns<PoseTrack>(runs, way != 0, flush, us[1][way], stats, chunks, sink);
                    else
                        EncodeRuns<v175::Track>(runs, way != 0, flush, us[0][way], stats, chunks, sink);
                    if (referenceChunks.empty())
                    {
                        referenceStats = std::move(stats);
                        referenceChunks = std::move(chunks);
                        continue;
                    }
                    for (std::size_t index = 0; index < runs.size(); ++index)
                    {
                        bool same = chunks[index] == referenceChunks[index] &&
                            stats[index].size() == referenceStats[index].size();
                        for (std::size_t frame = 0; same && frame < stats[index].size(); ++frame)
                            same = SameStats(stats[index][frame], referenceStats[index][frame]);
                        differ += same ? 0u : 1u;
                    }
                }
            }
            std::uint64_t boneFrames = 0, frames = 0, delta = 0, same = 0;
            for (std::size_t index = 0; index < runs.size(); ++index)
            {
                const std::uint32_t boneCount = runs[index].track->layouts[runs[index].layout].boneCount;
                for (const EncodeStats& stats : referenceStats[index])
                {
                    boneFrames += boneCount;
                    ++frames;
                    delta += stats.deltaChannels;
                    same += stats.rawSameChannels;
                }
            }
            const double bf = static_cast<double>(std::max<std::uint64_t>(1, boneFrames)) / 1000.0; // us -> ns per bone frame
            std::printf("  encoder cache clip %ls: %zu tracks, %llu frames (%.0f bones avg), unchanged channels %.1f%%; "
                        "ns per bone per frame reference -> current: %s %.1f -> %.1f, %s %.1f -> %.1f, %s %.1f -> %.1f, "
                        "%s (%zu MB) %.1f -> %.1f; 8 runs, %llu tracks differ\n",
                entry.path().filename().c_str(), runs.size(), static_cast<unsigned long long>(frames),
                frames ? static_cast<double>(boneFrames) / static_cast<double>(frames) : 0.0,
                delta ? 100.0 * static_cast<double>(same) / static_cast<double>(delta) : 0.0, wayNames[0],
                us[0][0] / bf, us[1][0] / bf, wayNames[1], us[0][1] / bf, us[1][1] / bf, wayNames[2], us[0][2] / bf,
                us[1][2] / bf, wayNames[3], flushLarge.size() >> 20, us[0][3] / bf, us[1][3] / bf,
                static_cast<unsigned long long>(differ));
            for (int encoder = 0; encoder < 2; ++encoder)
            {
                for (int way = 0; way < 4; ++way)
                    allUs[encoder][way] += us[encoder][way];
            }
            allBoneFrames += boneFrames;
            allFrames += frames;
            allDelta += delta;
            allSame += same;
            allDiffer += differ;
            allRuns += runs.size();
        }
        if (allFrames == 0)
            return;
        const double bf = static_cast<double>(allBoneFrames) / 1000.0;
        std::printf("  encoder cache, all clips: %llu tracks, %llu frames, unchanged channels %.1f%%, last-level cache "
                    "%zu MB; ns per bone per frame reference -> current: sequential %.1f -> %.1f, interleaved %.1f -> "
                    "%.1f, flushed 32 MB %.1f -> %.1f, flushed %zu MB %.1f -> %.1f; interleaved and flushed against "
                    "sequential (reference): x%.2f, x%.2f, x%.2f; %llu tracks differ (sink %llu)\n",
            static_cast<unsigned long long>(allRuns), static_cast<unsigned long long>(allFrames),
            allDelta ? 100.0 * static_cast<double>(allSame) / static_cast<double>(allDelta) : 0.0, lastLevel >> 20,
            allUs[0][0] / bf, allUs[1][0] / bf, allUs[0][1] / bf, allUs[1][1] / bf, allUs[0][2] / bf, allUs[1][2] / bf,
            flushLarge.size() >> 20, allUs[0][3] / bf, allUs[1][3] / bf, allUs[0][1] / std::max(1e-9, allUs[0][0]),
            allUs[0][2] / std::max(1e-9, allUs[0][0]), allUs[0][3] / std::max(1e-9, allUs[0][0]),
            static_cast<unsigned long long>(allDiffer), static_cast<unsigned long long>(sink));
        Check(allDiffer == 0, "encoder cache benchmark: every way and both encoders record the same chunks and stats");
    }

    // ---------------------------------------------------------------------
    // The worker's validation of frames with engine objects
    // (FindFailingBonesChanged after a frame that passed, FindFailingBones
    // otherwise) against the reference check, on every recorded track (REPLAYPOC_POSE_CLIP_DIR):
    // each frame's objects composed on its layout, as the engine's agree
    // with it, a tenth of the frames repeated whole (idle copies), and in
    // some frames one bone's object moved onto a tolerance (2 mm or 4 mrad,
    // a few ulps either side) or broken; as recorded and with noise on the
    // locals. Two more workers run the current check with the SSE expected
    // objects moved by +-32 ulps per lane, and on the scale of the
    // composition's rounding (tests/pose_stream_tests.cpp band_nudge). All
    // four must give the same failing bones, checked counts, counts and
    // previous arrays every frame.
    void TestValidationClipsFromEnv()
    {
        wchar_t folder[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"REPLAYPOC_POSE_CLIP_DIR", folder, MAX_PATH))
            return;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        {
            if (entry.path().extension() != L".rpclip")
                continue;
            replay::Recording recording;
            std::vector<PoseRecordExport> poses;
            replay::camera::Track camera;
            std::uint64_t duration = 0;
            std::string error;
            if (!replay::clipfile::Load(entry.path().wstring(), 0x140000000ull, 0x8000000, recording, poses, camera,
                    duration, error))
            {
                std::fprintf(stderr, "  validation clip %ls: not loaded (%s)\n", entry.path().filename().c_str(),
                    error.c_str());
                continue;
            }
            for (const bool noisy : {false, true})
            {
                std::mt19937 rng(11);
                std::uniform_real_distribution<float> unit(0.0f, 1.0f);
                std::uniform_real_distribution<float> signedUnit(-1.0f, 1.0f);
                std::uniform_int_distribution<int> ulps(-12, 12);
                std::uint64_t runs = 0, frameCount = 0, idle = 0, placed = 0, broken = 0;
                std::uint64_t changedChecks = 0, fullChecks = 0, recomputed = 0, failingBones = 0, differ = 0;
                std::uint64_t bandDecided = 0, bandScalar = 0;
                const ValidationBand band = MakeValidationBand(0.002f, 0.004f);
                double oldUs = 0.0, newUs = 0.0;
                for (const PoseRecordExport& record : poses)
                {
                    const TrackExport& track = record.track;
                    std::size_t chunk = 0;
                    while (chunk < track.chunks.size())
                    {
                        const std::uint32_t layoutIndex = track.chunks[chunk].layout;
                        std::vector<EngineFrame> frames;
                        for (; chunk < track.chunks.size() && track.chunks[chunk].layout == layoutIndex; ++chunk)
                            DecodeChunk(track, track.chunks[chunk], frames, 300, noisy, rng);
                        const SkeletonLayout& layout = track.layouts[layoutIndex];
                        const std::uint32_t boneCount = layout.boneCount;
                        if (frames.size() < 2 || boneCount == 0)
                            continue;
                        ++runs;
                        // Per worker (reference, current, current with the SSE
                        // objects moved by ulps, current with them moved on
                        // the rounding scale): the last frame that passed.
                        std::vector<BoneQts> lastPassed[4];
                        bool lastPassedValid[4] = {};
                        std::vector<BoneQts> pose(static_cast<std::size_t>(boneCount) * 2u);
                        BoneQts* locals = pose.data();
                        BoneQts* objects = pose.data() + boneCount;
                        std::vector<BoneQts> unresolved;
                        for (std::size_t index = 0; index < frames.size(); ++index)
                        {
                            ++frameCount;
                            if (index > 0 && unit(rng) < 0.1f)
                            {
                                ++idle; // the same pose again
                            }
                            else
                            {
                                std::memcpy(locals, frames[index].locals.data(), sizeof(BoneQts) * boneCount);
                                unresolved.clear();
                                for (const std::uint32_t bone : layout.unresolvedBones)
                                    unresolved.push_back(locals[bone]);
                                ComposeObjects(layout, locals, unresolved.data(), objects);
                                const float roll = unit(rng);
                                const std::uint32_t bone = static_cast<std::uint32_t>(rng() % boneCount);
                                const std::int32_t parent = layout.parent[bone];
                                if (roll < 0.15f && parent >= 0)
                                {
                                    const BoneQts expected = Compose(objects[static_cast<std::uint32_t>(parent)],
                                        locals[bone], layout.order, layout.inheritScale);
                                    BoneQts& object = objects[bone];
                                    object = expected;
                                    const Vec3 axis = RandomAxis(rng);
                                    const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                                    if (rng() % 2u == 0)
                                    {
                                        const float target = band_nudge::StepUlps(0.002f, ulps(rng));
                                        object.translation[0] += axis.x / length * target;
                                        object.translation[1] += axis.y / length * target;
                                        object.translation[2] += axis.z / length * target;
                                    }
                                    else
                                    {
                                        const float angle = band_nudge::StepUlps(0.004f, ulps(rng) * 64);
                                        object.rotation = Multiply(expected.rotation, AxisAngle(axis, angle));
                                    }
                                    ++placed;
                                }
                                else if (roll > 0.98f)
                                {
                                    objects[bone].translation[rng() % 3u] += 0.05f * signedUnit(rng);
                                    ++broken;
                                }
                            }
                            std::vector<std::uint32_t> failing[4];
                            std::uint32_t checked[4] = {};
                            ChangedBoneCounts counts[4];
                            const bool changedPath = lastPassedValid[0];
                            for (int turn = 0; turn < 4; ++turn)
                            {
                                // The first two take turns going first (timing).
                                const int which = turn >= 2 ? turn : (static_cast<int>(index % 2) + turn) % 2;
                                if (lastPassedValid[which])
                                {
                                    if (which >= 2)
                                    {
                                        if (which == 2)
                                            band_nudge::Set(32, 0.0f, 0.0f, static_cast<std::uint32_t>(frameCount));
                                        else
                                            band_nudge::Set(0, 64.0f, 32.0f, static_cast<std::uint32_t>(frameCount));
                                        SetExpectedObjectHook(&band_nudge::Hook);
                                    }
                                    BoneQts* previous = lastPassed[which].data();
                                    const double start = NowUs();
                                    failing[which] = which == 0
                                        ? v175::FindFailingBonesChanged(layout, locals, objects, previous,
                                              previous + boneCount, 0.002f, 0.004f, checked[which], &counts[which])
                                        : FindFailingBonesChanged(layout, locals, objects, previous, previous + boneCount,
                                              0.002f, 0.004f, checked[which], &counts[which]);
                                    const double elapsed = NowUs() - start;
                                    SetExpectedObjectHook(nullptr);
                                    if (which == 0)
                                        oldUs += elapsed;
                                    else if (which == 1)
                                        newUs += elapsed;
                                    lastPassedValid[which] = failing[which].empty();
                                }
                                else
                                {
                                    failing[which] = FindFailingBones(layout, locals, objects, 0.002f, 0.004f);
                                    checked[which] = boneCount;
                                    lastPassedValid[which] = failing[which].empty();
                                    if (lastPassedValid[which])
                                        lastPassed[which] = pose;
                                }
                            }
                            changedChecks += changedPath ? 1u : 0u;
                            fullChecks += changedPath ? 0u : 1u;
                            recomputed += changedPath ? checked[0] : 0u;
                            failingBones += failing[0].size();
                            for (int which = 1; which < 4; ++which)
                            {
                                const bool same = failing[which] == failing[0] && checked[which] == checked[0] &&
                                    counts[which].locals == counts[0].locals &&
                                    counts[which].objects == counts[0].objects && counts[which].root == counts[0].root &&
                                    lastPassedValid[which] == lastPassedValid[0] &&
                                    lastPassed[which].size() == lastPassed[0].size() &&
                                    (lastPassed[0].empty() ||
                                        SameBytes(lastPassed[which].data(), lastPassed[0].data(),
                                            lastPassed[0].size() * sizeof(BoneQts)));
                                differ += same ? 0u : 1u;
                            }
                            // How often the band decides on these skeletons:
                            // every parented bone of every fourth frame.
                            if (index % 4 == 0)
                            {
                                for (std::uint32_t bone = 0; bone < boneCount; ++bone)
                                {
                                    const std::int32_t parent = layout.parent[bone];
                                    if (parent < 0)
                                        continue;
                                    const BoneQts& parentObject = objects[static_cast<std::uint32_t>(parent)];
                                    BoneQts composed;
                                    ComposeSimd(parentObject, locals[bone], layout.order, layout.inheritScale, composed);
                                    const int verdict =
                                        ValidationVerdict(band, parentObject, locals[bone], composed, objects[bone]);
                                    ++(verdict == 0 ? bandScalar : bandDecided);
                                }
                            }
                        }
                    }
                }
                std::printf("  validation clip %ls (%s): %llu runs, %llu frames (%llu idle, %llu bones put on a "
                            "tolerance, %llu broken), %llu changed-bones checks recomputing %llu bones, %llu full "
                            "checks, %llu failing bones, %llu differ (current, and with the SSE objects moved by ulps or on the rounding scale); us per "
                            "changed-bones check reference %.2f -> current %.2f; the band decided %llu of %llu "
                            "parented bones (every fourth frame), %llu left to the scalar test\n",
                    entry.path().filename().c_str(), noisy ? "noise on the locals" : "as recorded",
                    static_cast<unsigned long long>(runs), static_cast<unsigned long long>(frameCount),
                    static_cast<unsigned long long>(idle), static_cast<unsigned long long>(placed),
                    static_cast<unsigned long long>(broken), static_cast<unsigned long long>(changedChecks),
                    static_cast<unsigned long long>(recomputed), static_cast<unsigned long long>(fullChecks),
                    static_cast<unsigned long long>(failingBones), static_cast<unsigned long long>(differ),
                    changedChecks ? oldUs / static_cast<double>(changedChecks) : 0.0,
                    changedChecks ? newUs / static_cast<double>(changedChecks) : 0.0,
                    static_cast<unsigned long long>(bandDecided),
                    static_cast<unsigned long long>(bandDecided + bandScalar),
                    static_cast<unsigned long long>(bandScalar));
                Check(differ == 0, "recorded clip: the validation of engine frames gives the reference result");
            }
        }
    }
}

int RunPoseCaptureTests()
{
    g_captureFailures = 0;
    TestRounding();
    TestCheapTests();
    TestSyntheticPipelines();
    TestEncoderAgainstV175();
    TestLateCompare();
    BenchmarkSynthetic();
    TestRecordedClipsFromEnv();
    BenchmarkEncoderCacheFromEnv();
    TestValidationClipsFromEnv();
    if (g_captureFailures == 0)
        std::printf("Pose capture tests passed.\n");
    return g_captureFailures;
}
