// Pose sampling without copies. A simpler copying sampler is kept here as the
// reference: every test drives it and PoseTrack::Sample over the same recorded
// track and the same playhead path (60 fps play, slow motion, 2x, backward
// scrub through the checkpoints, fine scrub, seeks, pause) and requires the
// output bit for bit. The same code times both samplers (micro-benchmark: time
// per sample for N bones), and with REPLAYPOC_POSE_CLIP_DIR=<folder> it runs
// on every track of the clips there.

#include "pose_stream.h"

#include "clip_file.h"
#include "pose_hash.h"
#include "profiler.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
    using namespace replay::pose;

    int g_sampleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_sampleFailures;
    }
}

// ---------------------------------------------------------------------------
// The reference sampler (PoseDecoder::Apply, PoseTrack::DecodeTo, Materialize
// and Sample of the copying design), constructed from a TrackExport.
namespace legacy
{
    using namespace replay::pose;

    constexpr std::size_t kRecordHeaderBytes =
        sizeof(std::uint32_t) * 2 + sizeof(std::uint16_t) * 2 + sizeof(float) * 16;

    template <typename T>
    bool Get(const std::uint8_t* data, std::size_t size, std::size_t& offset, T& value)
    {
        if (size < offset || size - offset < sizeof(T))
            return false;
        std::memcpy(&value, data + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    class Decoder
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config)
        {
            m_layout = &layout;
            m_config = config;
            m_channels.assign(layout.ChannelCount(), BoneQts{});
            m_timeMs = 0;
            m_flags = 0;
            std::memset(m_world, 0, sizeof(m_world));
            m_hasKeyframe = false;
        }

        bool Apply(const std::uint8_t* data, std::size_t size, std::size_t& consumed)
        {
            consumed = 0;
            if (!m_layout || !data)
                return false;
            std::size_t offset = 0;
            std::uint32_t recordSize = 0;
            std::uint32_t timeMs = 0;
            std::uint16_t flags = 0;
            std::uint16_t entryCount = 0;
            if (!Get(data, size, offset, recordSize) ||
                recordSize < kRecordHeaderBytes || recordSize > size ||
                !Get(data, recordSize, offset, timeMs) ||
                !Get(data, recordSize, offset, flags) ||
                !Get(data, recordSize, offset, entryCount))
            {
                return false;
            }
            const bool keyframe = (flags & kFrameKeyframe) != 0;
            if (!keyframe && !m_hasKeyframe)
                return false;
            float world[16] = {};
            for (float& value : world)
            {
                if (!Get(data, recordSize, offset, value))
                    return false;
            }
            std::vector<BoneQts>& channels = m_scratch;
            if (keyframe)
                channels.assign(m_channels.size(), BoneQts{});
            else
                channels.assign(m_channels.begin(), m_channels.end());
            for (std::uint16_t entry = 0; entry < entryCount; ++entry)
            {
                std::uint16_t channel = 0;
                std::uint8_t mask = 0;
                if (!Get(data, recordSize, offset, channel) ||
                    !Get(data, recordSize, offset, mask) ||
                    channel >= channels.size())
                {
                    return false;
                }
                BoneQts& value = channels[channel];
                if (mask & kEntryRot32)
                {
                    std::uint32_t packed = 0;
                    if (!Get(data, recordSize, offset, packed))
                        return false;
                    value.rotation = UnpackQuat32(packed);
                }
                if (mask & kEntryRot64)
                {
                    std::uint64_t packed = 0;
                    if (!Get(data, recordSize, offset, packed))
                        return false;
                    value.rotation = UnpackQuat64(packed);
                }
                if (mask & kEntryTrans16)
                {
                    std::int16_t packed[3] = {};
                    for (std::int16_t& axis : packed)
                    {
                        if (!Get(data, recordSize, offset, axis))
                            return false;
                    }
                    UnpackTranslation16(packed, m_config.translationRange, value.translation);
                }
                if (mask & kEntryTransFloat)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        if (!Get(data, recordSize, offset, value.translation[axis]))
                            return false;
                    }
                }
                if ((mask & kEntryTransW) &&
                    !Get(data, recordSize, offset, value.translation[3]))
                {
                    return false;
                }
                if (mask & kEntryScale)
                {
                    for (float& axis : value.scale)
                    {
                        if (!Get(data, recordSize, offset, axis))
                            return false;
                    }
                }
            }
            if (offset != recordSize)
                return false;
            m_channels.swap(channels);
            m_timeMs = timeMs;
            m_flags = flags;
            std::memcpy(m_world, world, sizeof(world));
            m_hasKeyframe = true;
            consumed = recordSize;
            return true;
        }

        std::uint32_t TimeMs() const { return m_timeMs; }
        std::uint16_t Flags() const { return m_flags; }
        const float* World() const { return m_world; }
        const std::vector<BoneQts>& Channels() const { return m_channels; }

        struct State
        {
            std::vector<BoneQts> channels;
            std::uint32_t timeMs = 0;
            std::uint16_t flags = 0;
            float world[16] = {};
            bool hasKeyframe = false;
        };
        void SaveState(State& out) const
        {
            out.channels.assign(m_channels.begin(), m_channels.end());
            out.timeMs = m_timeMs;
            out.flags = m_flags;
            std::memcpy(out.world, m_world, sizeof(out.world));
            out.hasKeyframe = m_hasKeyframe;
        }
        void RestoreState(const State& state)
        {
            m_channels.assign(state.channels.begin(), state.channels.end());
            m_timeMs = state.timeMs;
            m_flags = state.flags;
            std::memcpy(m_world, state.world, sizeof(m_world));
            m_hasKeyframe = state.hasKeyframe;
        }

    private:
        const SkeletonLayout* m_layout = nullptr;
        StreamConfig m_config;
        std::vector<BoneQts> m_channels;
        std::vector<BoneQts> m_scratch;
        std::uint32_t m_timeMs = 0;
        std::uint16_t m_flags = 0;
        float m_world[16] = {};
        bool m_hasKeyframe = false;
    };

    class Track
    {
    public:
        bool Import(const TrackExport& in)
        {
            if (in.layouts.empty())
                return false;
            std::vector<std::shared_ptr<const SkeletonLayout>> layouts;
            for (const SkeletonLayout& layout : in.layouts)
                layouts.push_back(std::make_shared<const SkeletonLayout>(layout));
            std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> anchors;
            for (const std::vector<std::uint8_t>& anchor : in.anchors)
                anchors.push_back(std::make_shared<const std::vector<std::uint8_t>>(anchor));
            m_config = in.config;
            m_chunks.clear();
            for (const TrackChunkExport& exported : in.chunks)
            {
                Chunk chunk;
                chunk.layout = layouts[exported.layout];
                if (exported.anchor >= 0)
                    chunk.anchor = anchors[static_cast<std::size_t>(exported.anchor)];
                chunk.bytes = exported.bytes;
                chunk.frameTimes = exported.frameTimes;
                chunk.frameOffsets = exported.frameOffsets;
                m_chunks.push_back(std::move(chunk));
            }
            if (m_chunks.empty())
                return false;
            m_firstTimeMs = m_chunks.front().frameTimes.front();
            m_lastTimeMs = m_chunks.back().frameTimes.back();
            m_cursor.Begin(*layouts[in.currentLayout], m_config);
            m_next.Begin(*layouts[in.currentLayout], m_config);
            m_keepLayout = layouts[in.currentLayout];
            return true;
        }

        static void SetCheckpointsEnabled(bool enabled) { CheckpointsEnabled() = enabled; }
        std::uint64_t UnflaggedRebaseHolds() const { return m_unflaggedRebaseHolds; }
        std::uint64_t CheckpointRestores() const { return m_checkpointRestores; }

        bool Sample(std::uint32_t timeMs, PoseSample& sample)
        {
            if (m_chunks.empty())
                return false;
            timeMs = std::clamp(timeMs, m_firstTimeMs, m_lastTimeMs);
            std::size_t chunkIndex = 0;
            for (std::size_t index = m_chunks.size(); index-- > 0;)
            {
                if (m_chunks[index].frameTimes.front() <= timeMs)
                {
                    chunkIndex = index;
                    break;
                }
            }
            const Chunk& chunk = m_chunks[chunkIndex];
            const auto upper = std::upper_bound(chunk.frameTimes.begin(), chunk.frameTimes.end(), timeMs);
            const std::size_t frameIndex = static_cast<std::size_t>(upper - chunk.frameTimes.begin()) - 1u;
            const std::uint64_t serial = m_firstChunkSerial + chunkIndex;
            if (m_checkpointSerial != ~0ull && serial > m_checkpointSerial + 1)
                ResetCheckpoints(true);

            if (m_cursorChunkSerial != serial || m_cursorFrame != frameIndex)
            {
                if (m_nextChunkSerial == serial && m_nextFrame == frameIndex)
                {
                    std::swap(m_cursor, m_next);
                    std::swap(m_poseA, m_poseB);
                    m_cursorChunkSerial = serial;
                    m_cursorFrame = frameIndex;
                    m_nextChunkSerial = ~0ull;
                }
                else
                {
                    m_nextChunkSerial = ~0ull;
                    if (!DecodeTo(chunkIndex, frameIndex, m_cursor, m_cursorFrame))
                        return false;
                }
            }

            bool hasNext = false;
            const SkeletonLayout* nextLayout = chunk.layout.get();
            std::uint64_t nextSerial = serial;
            std::size_t nextFrame = frameIndex + 1;
            if (frameIndex + 1 >= chunk.frameTimes.size())
            {
                nextSerial = serial + 1;
                nextFrame = 0;
            }
            const bool nextExists = frameIndex + 1 < chunk.frameTimes.size() || chunkIndex + 1 < m_chunks.size();
            if (nextExists && m_nextChunkSerial == nextSerial && m_nextFrame == nextFrame)
            {
                hasNext = true;
                if (nextSerial != serial)
                    nextLayout = m_chunks[chunkIndex + 1].layout.get();
            }
            else if (frameIndex + 1 < chunk.frameTimes.size())
            {
                m_next = m_cursor;
                const std::size_t offset = chunk.frameOffsets[frameIndex + 1];
                std::size_t consumed = 0;
                hasNext = m_next.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed);
            }
            else if (chunkIndex + 1 < m_chunks.size())
            {
                std::size_t unused = 0;
                nextLayout = m_chunks[chunkIndex + 1].layout.get();
                hasNext = DecodeTo(chunkIndex + 1, 0, m_next, unused);
            }
            if (hasNext)
            {
                m_nextChunkSerial = nextSerial;
                m_nextFrame = nextFrame;
            }
            else
            {
                m_nextChunkSerial = ~0ull;
            }

            float t = 0.0f;
            if (hasNext && !(m_next.Flags() & kFrameNoInterpolation) && m_next.TimeMs() > m_cursor.TimeMs())
            {
                t = static_cast<float>(timeMs - m_cursor.TimeMs()) /
                    static_cast<float>(m_next.TimeMs() - m_cursor.TimeMs());
            }
            Materialize(*chunk.layout, m_cursor, serial, frameIndex, m_poseA);
            if (t > 0.0f)
            {
                Materialize(*nextLayout, m_next, nextSerial, nextFrame, m_poseB);
                WorldTransform worldA{};
                WorldTransform worldB{};
                if (!m_poseA.objects.empty() && !m_poseB.objects.empty() &&
                    DecodeWorldMatrix(m_cursor.World(), worldA) && DecodeWorldMatrix(m_next.World(), worldB) &&
                    IsRebase(MakeRebaseProbe(worldA, m_poseA.objects[0], m_cursor.TimeMs()),
                        MakeRebaseProbe(worldB, m_poseB.objects[0], m_next.TimeMs())))
                {
                    t = 0.0f;
                    ++m_unflaggedRebaseHolds;
                }
            }
            if (t > 0.0f)
            {
                const std::size_t bones = m_poseA.locals.size();
                sample.locals.resize(bones);
                sample.objects.resize(bones);
                const auto blend = [t](const BoneQts& a, const BoneQts& b) {
                    return std::memcmp(&a, &b, sizeof(BoneQts)) == 0 ? a : Interpolate(a, b, t);
                };
                for (std::size_t bone = 0; bone < bones; ++bone)
                {
                    sample.locals[bone] = blend(m_poseA.locals[bone], m_poseB.locals[bone]);
                    sample.objects[bone] = blend(m_poseA.objects[bone], m_poseB.objects[bone]);
                }
            }
            else
            {
                sample.locals.assign(m_poseA.locals.begin(), m_poseA.locals.end());
                sample.objects.assign(m_poseA.objects.begin(), m_poseA.objects.end());
            }
            std::memcpy(sample.worldA, m_cursor.World(), sizeof(sample.worldA));
            std::memcpy(sample.worldB, hasNext ? m_next.World() : m_cursor.World(), sizeof(sample.worldB));
            sample.t = t;
            sample.frameTimeA = m_cursor.TimeMs();
            sample.frameTimeB = hasNext ? m_next.TimeMs() : m_cursor.TimeMs();
            return true;
        }

    private:
        struct Chunk
        {
            std::shared_ptr<const SkeletonLayout> layout;
            std::shared_ptr<const std::vector<std::uint8_t>> anchor;
            std::vector<std::uint8_t> bytes;
            std::vector<std::uint32_t> frameTimes;
            std::vector<std::uint32_t> frameOffsets;
        };
        struct Materialized
        {
            std::uint64_t serial = ~0ull;
            std::size_t frame = 0;
            std::vector<BoneQts> locals;
            std::vector<BoneQts> objects;
        };

        static bool& CheckpointsEnabled()
        {
            static bool enabled = true;
            return enabled;
        }

        void ResetCheckpoints(bool release)
        {
            m_checkpointSerial = ~0ull;
            std::fill(m_checkpointValid.begin(), m_checkpointValid.end(), static_cast<std::uint8_t>(0));
            if (release)
            {
                std::vector<Decoder::State>().swap(m_checkpoints);
                std::vector<std::uint8_t>().swap(m_checkpointValid);
            }
        }

        bool DecodeTo(std::size_t chunkIndex, std::size_t frameIndex, Decoder& decoder, std::size_t& cursorFrame)
        {
            const Chunk& chunk = m_chunks[chunkIndex];
            const std::uint64_t serial = m_firstChunkSerial + chunkIndex;
            std::size_t start = 0;
            bool keepCheckpoints = false;
            if (&decoder == &m_cursor && m_cursorChunkSerial == serial && cursorFrame <= frameIndex)
            {
                start = cursorFrame + 1;
            }
            else if (&decoder == &m_cursor && CheckpointsEnabled())
            {
                keepCheckpoints = true;
                if (m_checkpointSerial != serial)
                {
                    std::fill(m_checkpointValid.begin(), m_checkpointValid.end(), static_cast<std::uint8_t>(0));
                    const std::size_t slots = (chunk.frameTimes.size() + kCheckpointStride - 1) / kCheckpointStride;
                    if (m_checkpoints.size() < slots)
                    {
                        m_checkpoints.resize(slots);
                        m_checkpointValid.resize(slots, 0);
                    }
                    m_checkpointSerial = serial;
                }
                decoder.Begin(*chunk.layout, m_config);
                std::size_t slot = frameIndex / kCheckpointStride + 1;
                while (slot-- > 0)
                {
                    if (slot < m_checkpointValid.size() && m_checkpointValid[slot])
                        break;
                }
                if (slot != static_cast<std::size_t>(-1))
                {
                    decoder.RestoreState(m_checkpoints[slot]);
                    start = slot * kCheckpointStride + 1;
                    ++m_checkpointRestores;
                }
                else if (chunk.anchor)
                {
                    std::size_t consumed = 0;
                    if (!decoder.Apply(chunk.anchor->data(), chunk.anchor->size(), consumed))
                    {
                        m_cursorChunkSerial = ~0ull;
                        return false;
                    }
                }
            }
            else
            {
                decoder.Begin(*chunk.layout, m_config);
                if (chunk.anchor)
                {
                    std::size_t consumed = 0;
                    if (!decoder.Apply(chunk.anchor->data(), chunk.anchor->size(), consumed))
                    {
                        if (&decoder == &m_cursor)
                            m_cursorChunkSerial = ~0ull;
                        return false;
                    }
                }
            }
            for (std::size_t frame = start; frame <= frameIndex; ++frame)
            {
                const std::size_t offset = chunk.frameOffsets[frame];
                std::size_t consumed = 0;
                if (!decoder.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed))
                {
                    if (&decoder == &m_cursor)
                        m_cursorChunkSerial = ~0ull;
                    return false;
                }
                if (keepCheckpoints && frame % kCheckpointStride == 0)
                {
                    const std::size_t slot = frame / kCheckpointStride;
                    if (slot < m_checkpoints.size() && !m_checkpointValid[slot])
                    {
                        decoder.SaveState(m_checkpoints[slot]);
                        m_checkpointValid[slot] = 1;
                    }
                }
            }
            if (&decoder == &m_cursor)
                m_cursorChunkSerial = serial;
            cursorFrame = frameIndex;
            return true;
        }

        void Materialize(const SkeletonLayout& layout, const Decoder& decoder, std::uint64_t serial,
            std::size_t frame, Materialized& out)
        {
            if (out.serial == serial && out.frame == frame && out.locals.size() == layout.boneCount)
                return;
            const std::vector<BoneQts>& channels = decoder.Channels();
            out.locals.assign(channels.begin(), channels.begin() + layout.boneCount);
            out.objects.resize(layout.boneCount);
            ComposeObjects(layout, out.locals.data(), channels.data() + layout.boneCount, out.objects.data());
            out.serial = serial;
            out.frame = frame;
        }

        static constexpr std::size_t kCheckpointStride = 8;
        StreamConfig m_config;
        std::vector<Chunk> m_chunks;
        std::shared_ptr<const SkeletonLayout> m_keepLayout;
        std::uint32_t m_firstTimeMs = 0;
        std::uint32_t m_lastTimeMs = 0;
        Decoder m_cursor;
        std::uint64_t m_cursorChunkSerial = ~0ull;
        std::size_t m_cursorFrame = 0;
        std::uint64_t m_firstChunkSerial = 0;
        Decoder m_next;
        std::uint64_t m_nextChunkSerial = ~0ull;
        std::size_t m_nextFrame = 0;
        std::uint64_t m_unflaggedRebaseHolds = 0;
        std::uint64_t m_checkpointSerial = ~0ull;
        std::vector<Decoder::State> m_checkpoints;
        std::vector<std::uint8_t> m_checkpointValid;
        std::uint64_t m_checkpointRestores = 0;
        Materialized m_poseA;
        Materialized m_poseB;
    };
}

namespace
{
    // ---------------------------------------------------------------------
    // Synthetic metaped-sized skeleton: a spine, limbs, a face of many small
    // bones and props that never move, a few bones recorded in object space.
    // The motion is sparse like a walking ped: the body moves every frame,
    // the face now and then, the props never.
    struct BigSkeleton
    {
        SkeletonLayout layout;
        std::vector<BoneQts> bind;

        explicit BigSkeleton(std::uint32_t bones)
        {
            layout.boneCount = bones;
            layout.order = CompositionOrder::ParentThenLocal;
            layout.parent.assign(bones, kRootParent);
            layout.ambiguous.assign(bones, 0);
            layout.highPrecision.assign(bones, 0);
            bind.assign(bones, BoneQts{});
            std::mt19937 random(bones);
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                BoneQts& b = bind[bone];
                b.translation[0] = 0.01f * static_cast<float>(random() % 30);
                b.translation[1] = 0.01f * static_cast<float>(random() % 30);
                b.translation[2] = 0.02f + 0.01f * static_cast<float>(random() % 20);
                b.translation[3] = 0.04773f;
                if (bone == 0)
                    continue;
                // Parents precede children: a chain of 12 spine bones, then
                // every bone hangs from a random earlier one.
                const std::uint32_t parent = bone <= 12 ? bone - 1 : static_cast<std::uint32_t>(random() % bone);
                layout.parent[bone] = static_cast<std::int32_t>(parent);
                if (bone % 97 == 13)
                    layout.parent[bone] = kUnresolvedParent;
            }
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                layout.evaluationOrder.push_back(bone);
                if (layout.parent[bone] == kUnresolvedParent)
                    layout.unresolvedBones.push_back(bone);
                if (bone < 12)
                    layout.highPrecision[bone] = 1;
            }
        }

        static Quat AxisAngle(float x, float y, float z, float angle)
        {
            const float length = std::sqrt(x * x + y * y + z * z);
            const float s = std::sin(angle * 0.5f) / length;
            return Normalize({x * s, y * s, z * s, std::cos(angle * 0.5f)});
        }

        // A third of the bones is the body (moves every frame), a third the
        // face (moves on every third frame), a third props (never moves).
        void Pose(std::uint32_t frame, std::vector<BoneQts>& locals, std::vector<BoneQts>& objects) const
        {
            const float time = static_cast<float>(frame) / 30.0f;
            const std::uint32_t bones = layout.boneCount;
            locals = bind;
            locals[0].rotation = AxisAngle(0.0f, 0.0f, 1.0f, 0.4f * time);
            locals[0].translation[0] = 1.3f * time;
            locals[0].translation[1] = 12.0f; // outside the s16 range: float entries
            for (std::uint32_t bone = 1; bone < bones; ++bone)
            {
                const std::uint32_t group = bone * 3 / bones;
                if (group == 0 || (group == 1 && frame % 3 == 0))
                {
                    const float phase = time * (group == 0 ? 4.0f : 1.0f) + 0.37f * static_cast<float>(bone);
                    locals[bone].rotation = AxisAngle(1.0f, 0.3f * static_cast<float>(bone % 5), 0.2f,
                        0.35f * std::sin(phase));
                    if (bone % 11 == 0)
                        locals[bone].translation[2] = bind[bone].translation[2] + 0.02f * std::sin(phase);
                }
                else if (group == 1)
                {
                    // Held from the last update (every third frame).
                    const std::uint32_t held = frame - frame % 3;
                    const float phase = static_cast<float>(held) / 30.0f + 0.37f * static_cast<float>(bone);
                    locals[bone].rotation = AxisAngle(1.0f, 0.3f * static_cast<float>(bone % 5), 0.2f,
                        0.35f * std::sin(phase));
                    if (bone % 11 == 0)
                        locals[bone].translation[2] = bind[bone].translation[2] + 0.02f * std::sin(phase);
                }
            }
            objects.assign(bones, BoneQts{});
            ComposeObjectsForInput(locals, objects, time);
        }

    private:
        void ComposeObjectsForInput(const std::vector<BoneQts>& locals, std::vector<BoneQts>& objects, float time) const
        {
            for (const std::uint32_t bone : layout.evaluationOrder)
            {
                const std::int32_t parent = layout.parent[bone];
                if (parent == kRootParent)
                    objects[bone] = locals[bone];
                else if (parent >= 0)
                    objects[bone] = Compose(objects[static_cast<std::uint32_t>(parent)], locals[bone], layout.order);
                else
                {
                    objects[bone] = locals[bone];
                    objects[bone].rotation = AxisAngle(0.0f, 1.0f, 0.0f, 0.5f * std::sin(time + bone));
                    objects[bone].translation[0] = 0.3f + 0.05f * std::sin(time * 2.0f + bone);
                }
            }
        }
    };

    // A track of `frames` frames at 30 fps; a layout change (an unresolved
    // bone more) at 60 % of the track, no-interpolation frames and a
    // teleport (a rebase no flag marked) on the way.
    PoseTrack MakeTrack(std::uint32_t bones, std::uint32_t frames)
    {
        const BigSkeleton skeleton(bones);
        StreamConfig config;
        PoseTrack track;
        track.Begin(skeleton.layout, config);
        SkeletonLayout changed = skeleton.layout;
        changed.parent[bones / 2] = kUnresolvedParent;
        changed.unresolvedBones.clear();
        for (std::uint32_t bone = 0; bone < bones; ++bone)
        {
            if (changed.parent[bone] == kUnresolvedParent)
                changed.unresolvedBones.push_back(bone);
        }
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        float world[16] = {};
        world[0] = 1.0f;
        world[5] = 1.0f;
        world[10] = 1.0f;
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            if (frame == frames * 3 / 5)
                track.UpdateLayout(changed);
            skeleton.Pose(frame, locals, objects);
            world[12] = 0.04f * static_cast<float>(frame) + (frame >= frames / 3 ? 6.0f : 0.0f); // teleport
            world[13] = 1.0f;
            FrameInput input;
            input.timeMs = frame * 33u + (frame % 4 == 1 ? 1u : 0u);
            input.flags = (frame % 53 == 17) ? kFrameNoInterpolation : 0;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        return track;
    }

    // Playhead paths of the editor: play at 60 fps, slow motion, 2x, pause,
    // backward scrub at 2x and fine (the checkpoint path), seeks followed
    // by play, and a jump back to the start.
    std::vector<std::uint32_t> PlayheadPath(std::uint32_t first, std::uint32_t last)
    {
        std::vector<std::uint32_t> times;
        const auto forward = [&](std::uint32_t from, std::uint32_t to, std::uint32_t step) {
            for (std::uint32_t time = from; time <= to; time += step)
                times.push_back(time);
        };
        const auto backward = [&](std::uint32_t from, std::uint32_t to, std::uint32_t step) {
            for (std::uint32_t time = from; time >= to + step; time -= step)
                times.push_back(time);
        };
        const std::uint32_t span = last - first;
        for (std::uint32_t time = first, tick = 0; time <= last; time += (tick++ % 3 == 0) ? 17u : 16u)
            times.push_back(time);                                   // 60 fps play
        forward(first + span / 5, first + span / 5 + 900u, 4u);      // 0.25x slow motion
        forward(first + span / 3, first + span / 3 + 3000u, 33u);    // 2x
        for (int pause = 0; pause < 5; ++pause)
            times.push_back(first + span / 2 + 7u);                  // paused
        backward(last, first, 33u);                                  // 2x scrub back
        forward(first, first + 1500u, 16u);
        backward(first + span / 2 + 800u, first + span / 2, 10u);    // fine scrub back
        std::mt19937 random(7);
        for (int seek = 0; seek < 24; ++seek)
        {
            const std::uint32_t at = first + static_cast<std::uint32_t>(random() % (span + 1));
            forward(at, std::min(last, at + 400u), 16u);             // seek, then play
            backward(std::min(last, at + 400u), at > 300u ? at - 300u : 0u, 23u); // and back
        }
        times.push_back(0u);                                         // before the first frame
        times.push_back(last + 5000u);                               // after the last
        forward(first, std::min(last, first + 700u), 16u);
        return times;
    }

    bool SameSample(const PoseSample& a, const PoseSample& b)
    {
        return a.t == b.t && a.frameTimeA == b.frameTimeA && a.frameTimeB == b.frameTimeB &&
            a.locals.size() == b.locals.size() && a.objects.size() == b.objects.size() &&
            std::memcmp(a.locals.data(), b.locals.data(), a.locals.size() * sizeof(BoneQts)) == 0 &&
            std::memcmp(a.objects.data(), b.objects.data(), a.objects.size() * sizeof(BoneQts)) == 0 &&
            std::memcmp(a.worldA, b.worldA, sizeof(a.worldA)) == 0 &&
            std::memcmp(a.worldB, b.worldB, sizeof(a.worldB)) == 0;
    }

    struct Comparison
    {
        std::size_t samples = 0;
        std::size_t mismatches = 0;
        std::size_t firstMismatch = ~static_cast<std::size_t>(0);
        std::size_t okMismatches = 0;
        bool holdsMatch = true;
        std::uint64_t checkpointRestores = 0;
    };

    // Both samplers over the same path, with checkpoints on and off.
    Comparison CompareOverPath(const PoseTrack& pristine, const std::vector<std::uint32_t>& times, bool checkpoints)
    {
        Comparison result;
        TrackExport exported;
        pristine.Export(exported);
        legacy::Track reference;
        if (!reference.Import(exported))
        {
            result.mismatches = 1;
            return result;
        }
        PoseTrack current = pristine;
        PoseTrack::SetCheckpointsEnabled(checkpoints);
        legacy::Track::SetCheckpointsEnabled(checkpoints);
        PoseSample a;
        PoseSample b;
        for (std::size_t index = 0; index < times.size(); ++index)
        {
            const bool okA = reference.Sample(times[index], a);
            const bool okB = current.Sample(times[index], b);
            ++result.samples;
            if (okA != okB)
                ++result.okMismatches;
            if (okA && okB && !SameSample(a, b))
            {
                if (result.mismatches == 0)
                    result.firstMismatch = index;
                ++result.mismatches;
            }
        }
        result.holdsMatch = reference.UnflaggedRebaseHolds() == current.UnflaggedRebaseHolds() &&
            reference.CheckpointRestores() == current.CheckpointRestores();
        result.checkpointRestores = current.CheckpointRestores();
        PoseTrack::SetCheckpointsEnabled(true);
        legacy::Track::SetCheckpointsEnabled(true);
        return result;
    }

    // The SSE bone math against the scalar functions, bit for bit: random bones
    // (both hemispheres, near and far rotations, scaled parents, both composition
    // orders, degenerate quaternions).
    void TestSimdMatchesScalar()
    {
        std::mt19937 random(1234);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        const auto randomBone = [&](bool close, const BoneQts* around) {
            BoneQts bone;
            if (close && around)
            {
                bone = *around;
                bone.rotation = Normalize({around->rotation.x + 0.01f * unit(random),
                    around->rotation.y + 0.01f * unit(random), around->rotation.z + 0.01f * unit(random),
                    around->rotation.w + 0.01f * unit(random)});
            }
            else
            {
                bone.rotation = Normalize({unit(random), unit(random), unit(random), unit(random)});
            }
            for (int axis = 0; axis < 4; ++axis)
            {
                bone.translation[axis] = 3.0f * unit(random);
                bone.scale[axis] = 1.0f + 0.3f * unit(random);
            }
            if (random() % 3 == 0)
                bone.rotation = {-bone.rotation.x, -bone.rotation.y, -bone.rotation.z, -bone.rotation.w};
            if (random() % 97 == 0)
                bone.rotation = {0.0f, 0.0f, 0.0f, 0.0f};
            return bone;
        };
        std::size_t interpolateDiffs = 0;
        std::size_t composeDiffs = 0;
        for (int round = 0; round < 200000; ++round)
        {
            const BoneQts a = randomBone(false, nullptr);
            const BoneQts b = randomBone(round % 2 == 0, &a);
            const float t = round % 5 == 0 ? 0.5f : 0.5f + 0.5f * unit(random);
            BoneQts simd;
            InterpolateSimd(a, b, t, simd);
            const BoneQts scalar = Interpolate(a, b, t);
            interpolateDiffs += std::memcmp(&simd, &scalar, sizeof(BoneQts)) != 0;
            const CompositionOrder order = round % 2 ? CompositionOrder::LocalThenParent : CompositionOrder::ParentThenLocal;
            const bool inherit = round % 3 == 0;
            ComposeSimd(a, b, order, inherit, simd);
            const BoneQts composed = Compose(a, b, order, inherit);
            composeDiffs += std::memcmp(&simd, &composed, sizeof(BoneQts)) != 0;
        }
        Check(interpolateDiffs == 0, "SSE Interpolate equals the scalar one bit for bit");
        Check(composeDiffs == 0, "SSE Compose equals the scalar one bit for bit");
    }

    // The override write hash of a pose kept in two arrays equals the hash of the
    // engine buffer holding them back to back, and changes with any bone.
    void TestPoseHash()
    {
        std::vector<BoneQts> locals(301);
        std::vector<BoneQts> objects(301);
        std::mt19937 random(5);
        for (BoneQts& bone : locals)
            bone.translation[0] = static_cast<float>(random() % 1000);
        for (BoneQts& bone : objects)
            bone.translation[1] = static_cast<float>(random() % 1000);
        std::vector<BoneQts> buffer(locals);
        buffer.insert(buffer.end(), objects.begin(), objects.end());
        PoseHasher split;
        split.Add(locals.data(), locals.size() * sizeof(BoneQts));
        split.Add(objects.data(), objects.size() * sizeof(BoneQts));
        PoseHasher whole;
        whole.Add(buffer.data(), buffer.size() * sizeof(BoneQts));
        Check(split.Finish() == whole.Finish(), "pose hash: two arrays hash like the buffer holding them");
        bool allDiffer = true;
        for (std::size_t bone = 0; bone < buffer.size(); bone += 37)
        {
            std::vector<BoneQts> changed(buffer);
            changed[bone].scale[3] += 1.0f;
            PoseHasher other;
            other.Add(changed.data(), changed.size() * sizeof(BoneQts));
            allDiffer = allDiffer && other.Finish() != whole.Finish();
        }
        Check(allDiffer, "pose hash: a changed bone changes the hash");

        // What one late write costs in hashing with a byte-wise FNV-1a over the whole
        // pose and with the current hash, for a 300-bone vehicle.
        std::vector<BoneQts> vehicle(600);
        const std::size_t bytes = vehicle.size() * sizeof(BoneQts);
        const auto* data = reinterpret_cast<const std::uint8_t*>(vehicle.data());
        std::uint64_t sink = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < 200; ++round)
        {
            std::uint64_t hash = 1469598103934665603ull;
            for (std::size_t index = 0; index < bytes; ++index)
            {
                hash ^= data[index];
                hash *= 1099511628211ull;
            }
            sink ^= hash + static_cast<std::uint64_t>(round);
        }
        const auto middle = std::chrono::steady_clock::now();
        for (int round = 0; round < 200; ++round)
        {
            PoseHasher hasher;
            hasher.Add(data, bytes);
            sink ^= hasher.Finish() + static_cast<std::uint64_t>(round);
        }
        const auto end = std::chrono::steady_clock::now();
        std::printf("  pose hash of a 300-bone pose (%zu bytes): byte FNV-1a %.2f us, word hash %.2f us (sink %llu)\n", bytes,
            std::chrono::duration<double, std::micro>(middle - start).count() / 200.0,
            std::chrono::duration<double, std::micro>(end - middle).count() / 200.0,
            static_cast<unsigned long long>(sink & 1));
    }

    void TestSampleMatchesLegacy()
    {
        for (const std::uint32_t bones : {40u, 300u, 1050u})
        {
            const PoseTrack track = MakeTrack(bones, 260); // 8.6 s, 5 chunks, a layout change
            const std::vector<std::uint32_t> path = PlayheadPath(track.Stats().firstTimeMs, track.Stats().lastTimeMs);
            for (const bool checkpoints : {true, false})
            {
                const Comparison result = CompareOverPath(track, path, checkpoints);
                char label[200];
                std::snprintf(label, sizeof(label),
                    "sampling without copies equals the reference sampler bit for bit (%u bones, checkpoints %s, %zu samples, %zu differ, first at %zu)",
                    bones, checkpoints ? "on" : "off", result.samples, result.mismatches, result.firstMismatch);
                Check(result.mismatches == 0 && result.okMismatches == 0, label);
                Check(result.holdsMatch, "same unflagged rebase holds and checkpoint restores as the reference sampler");
                if (checkpoints)
                    Check(result.checkpointRestores > 0, "the path goes through the scrub checkpoints");
            }
        }
    }

    // ---------------------------------------------------------------------
    // Micro-benchmark: time per sample at 60 fps play (two samples per
    // recorded frame, the city case) and per backward scrub tick.
    struct Timing
    {
        double legacyUs = 0.0;
        double currentUs = 0.0;
        std::size_t samples = 0;
    };

    template <typename Sampler>
    double TimePath(Sampler& sampler, const std::vector<std::uint32_t>& times, PoseSample& out)
    {
        const auto start = std::chrono::steady_clock::now();
        for (const std::uint32_t time : times)
            sampler.Sample(time, out);
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(end - start).count();
    }

    Timing TimeBoth(const PoseTrack& pristine, const std::vector<std::uint32_t>& times, int rounds)
    {
        Timing timing;
        TrackExport exported;
        pristine.Export(exported);
        double bestLegacy = 1e30;
        double bestCurrent = 1e30;
        PoseSample out;
        for (int round = 0; round < rounds; ++round)
        {
            legacy::Track reference;
            reference.Import(exported);
            PoseTrack current = pristine;
            bestLegacy = std::min(bestLegacy, TimePath(reference, times, out));
            bestCurrent = std::min(bestCurrent, TimePath(current, times, out));
        }
        timing.samples = times.size();
        timing.legacyUs = bestLegacy / static_cast<double>(times.size());
        timing.currentUs = bestCurrent / static_cast<double>(times.size());
        return timing;
    }

    std::vector<std::uint32_t> Play60(std::uint32_t first, std::uint32_t last)
    {
        std::vector<std::uint32_t> times;
        for (std::uint32_t time = first, tick = 0; time <= last; time += (tick++ % 3 == 0) ? 17u : 16u)
            times.push_back(time);
        return times;
    }

    std::vector<std::uint32_t> ScrubBack(std::uint32_t first, std::uint32_t last)
    {
        std::vector<std::uint32_t> times;
        for (std::uint32_t time = last; time >= first + 33u; time -= 33u)
            times.push_back(time);
        return times;
    }

    void BenchmarkSynthetic()
    {
        std::printf("  pose sample micro-benchmark (best of 5, us per sample, reference -> current):\n");
        for (const std::uint32_t bones : {200u, 600u, 1050u})
        {
            const PoseTrack track = MakeTrack(bones, 300);
            const Timing play = TimeBoth(track, Play60(track.Stats().firstTimeMs, track.Stats().lastTimeMs), 5);
            const Timing scrub = TimeBoth(track, ScrubBack(track.Stats().firstTimeMs, track.Stats().lastTimeMs), 5);
            std::printf("    %4u bones: play 60 fps %.2f -> %.2f (x%.2f), scrub back 2x %.2f -> %.2f (x%.2f)\n",
                bones, play.legacyUs, play.currentUs, play.legacyUs / std::max(play.currentUs, 1e-9),
                scrub.legacyUs, scrub.currentUs, scrub.legacyUs / std::max(scrub.currentUs, 1e-9));
        }
    }

    // ---------------------------------------------------------------------
    // Recorded clips: REPLAYPOC_POSE_CLIP_DIR=<folder with .rpclip files>
    // (copies, the tests only read them). Every track of every clip: the
    // same playhead path through both samplers, bit for bit, and the time
    // per sample of 60 fps play over all tracks.
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
                std::fprintf(stderr, "  pose clip %ls: not loaded (%s)\n", entry.path().filename().c_str(), error.c_str());
                continue;
            }
            std::size_t tracks = 0;
            std::size_t samples = 0;
            std::size_t mismatches = 0;
            std::size_t bonesTotal = 0;
            std::uint32_t bonesMax = 0;
            double legacyUs = 0.0;
            double currentUs = 0.0;
            std::size_t timedSamples = 0;
            replay::prof::ResetSession();
            const SampleWork workBefore = SampleWorkCounters();
            for (const PoseRecordExport& record : poses)
            {
                PoseTrack track;
                std::string importError;
                if (record.track.chunks.empty() || !track.Import(record.track, importError))
                    continue;
                ++tracks;
                bonesTotal += record.boneCount;
                bonesMax = std::max(bonesMax, record.boneCount);
                const std::uint32_t first = track.Stats().firstTimeMs;
                const std::uint32_t last = track.Stats().lastTimeMs;
                const std::vector<std::uint32_t> path = PlayheadPath(first, last);
                const Comparison result = CompareOverPath(track, path, true);
                samples += result.samples;
                mismatches += result.mismatches + result.okMismatches + (result.holdsMatch ? 0 : 1);
                if (result.mismatches)
                {
                    std::fprintf(stderr, "  pose clip %ls: record 0x%x (%u bones) differs at sample %zu\n",
                        entry.path().filename().c_str(), record.recordId, record.boneCount, result.firstMismatch);
                }
                const Timing timing = TimeBoth(track, Play60(first, last), 2);
                legacyUs += timing.legacyUs * static_cast<double>(timing.samples);
                currentUs += timing.currentUs * static_cast<double>(timing.samples);
                timedSamples += timing.samples;
            }
            std::printf("  pose clip %ls: %zu tracks (%zu bones avg, %u max), %zu samples compared, %zu differ; "
                        "play 60 fps %.2f -> %.2f us per sample\n",
                entry.path().filename().c_str(), tracks, tracks ? bonesTotal / tracks : 0, bonesMax, samples, mismatches,
                timedSamples ? legacyUs / static_cast<double>(timedSamples) : 0.0,
                timedSamples ? currentUs / static_cast<double>(timedSamples) : 0.0);
            // Where the new sampler's time goes (its profiler zones; the
            // comparison runs count too, so this is a split, not a total).
            const replay::prof::Snapshot session = replay::prof::Session();
            double zoneTotal = 0.0;
            for (const replay::prof::ZoneStats& zone : session.zones)
            {
                if (std::strncmp(zone.name, "play.pose.", 10) == 0)
                    zoneTotal += zone.totalUs;
            }
            std::printf("    split:");
            for (const replay::prof::ZoneStats& zone : session.zones)
            {
                if (std::strncmp(zone.name, "play.pose.", 10) == 0 && zoneTotal > 0.0)
                    std::printf(" %s %.0f%% (%.2f us/call)", zone.name + 10, 100.0 * zone.totalUs / zoneTotal, zone.avgUs);
            }
            std::printf("\n");
            const SampleWork work = SampleWorkCounters();
            const double pairs = static_cast<double>(std::max<std::uint64_t>(1, work.pairs - workBefore.pairs));
            const double pairBones = static_cast<double>(std::max<std::uint64_t>(1, work.pairBones - workBefore.pairBones));
            std::printf("    pairs: %.0f%% from A plus one record, bones reached %.0f%%, interpolated locals %.0f%% objects %.0f%%, "
                        "next frame advanced %llu copied %llu\n",
                100.0 * static_cast<double>(work.incrementalPairs - workBefore.incrementalPairs) / pairs,
                100.0 * static_cast<double>(work.reachedBones - workBefore.reachedBones) / pairBones,
                100.0 * static_cast<double>(work.blendLocals - workBefore.blendLocals) / pairBones,
                100.0 * static_cast<double>(work.blendObjects - workBefore.blendObjects) / pairBones,
                static_cast<unsigned long long>(work.nextAdvanced - workBefore.nextAdvanced),
                static_cast<unsigned long long>(work.nextCopied - workBefore.nextCopied));
            replay::prof::ResetSession();
            Check(mismatches == 0, "recorded clip: sampling equals the reference sampler bit for bit");
        }
    }

    // ---------------------------------------------------------------------
    // Every track of the recorded clips, sealed again every kSealEvery samples
    // while it plays (decoders, materialized poses, checkpoints and pair scratch
    // freed, then rebuilt by the next Sample), samples bit for bit what the same
    // track samples with its caches kept, and exports the same bytes. The
    // comparison above already walks PlayheadPath with the imported (sealed)
    // tracks; this path is lighter: 60 fps play and a 2x backward scrub over the
    // first 20 s, and seeks over the whole track, each followed by play and a
    // scrub back.
    constexpr std::size_t kSealEvery = 97;

    std::vector<std::uint32_t> SealPath(std::uint32_t first, std::uint32_t last)
    {
        std::vector<std::uint32_t> times;
        const std::uint32_t end = std::min(last, first + 20000u);
        for (std::uint32_t time = first, tick = 0; time <= end; time += (tick++ % 3 == 0) ? 17u : 16u)
            times.push_back(time);
        for (std::uint32_t time = end; time >= first + 33u; time -= 33u)
            times.push_back(time);
        std::mt19937 random(176);
        const std::uint32_t span = last - first;
        for (int seek = 0; seek < 16; ++seek)
        {
            const std::uint32_t at = first + static_cast<std::uint32_t>(random() % (span + 1u));
            const std::uint32_t to = std::min(last, at + 300u);
            for (std::uint32_t time = at; time <= to; time += 16u)
                times.push_back(time);
            for (std::uint32_t time = to; time >= at + 23u; time -= 23u)
                times.push_back(time);
        }
        return times;
    }

    // Two exports byte for byte: chunks (indices, bytes, times, offsets),
    // layouts, anchors, config, current layout and stats.
    bool SameTrackExport(const TrackExport& a, const TrackExport& b)
    {
        if (a.chunks.size() != b.chunks.size() || a.layouts.size() != b.layouts.size() || a.anchors != b.anchors ||
            a.currentLayout != b.currentLayout || a.config.rotationEpsilon != b.config.rotationEpsilon ||
            a.config.translationEpsilon != b.config.translationEpsilon ||
            a.config.translationRange != b.config.translationRange ||
            a.config.keyframeIntervalMs != b.config.keyframeIntervalMs || a.config.byteBudget != b.config.byteBudget ||
            a.stats.frames != b.stats.frames || a.stats.keyframes != b.stats.keyframes ||
            a.stats.relativeKeyframes != b.stats.relativeKeyframes ||
            a.stats.fullKeyframeBytes != b.stats.fullKeyframeBytes ||
            a.stats.relativeKeyframeBytes != b.stats.relativeKeyframeBytes || a.stats.bytes != b.stats.bytes ||
            a.stats.droppedChunks != b.stats.droppedChunks || a.stats.firstTimeMs != b.stats.firstTimeMs ||
            a.stats.lastTimeMs != b.stats.lastTimeMs)
        {
            return false;
        }
        for (std::size_t index = 0; index < a.layouts.size(); ++index)
        {
            const SkeletonLayout& la = a.layouts[index];
            const SkeletonLayout& lb = b.layouts[index];
            if (la.boneCount != lb.boneCount || la.order != lb.order || la.inheritScale != lb.inheritScale ||
                la.parent != lb.parent || la.ambiguous != lb.ambiguous || la.highPrecision != lb.highPrecision ||
                la.evaluationOrder != lb.evaluationOrder || la.unresolvedBones != lb.unresolvedBones)
            {
                return false;
            }
        }
        for (std::size_t index = 0; index < a.chunks.size(); ++index)
        {
            const TrackChunkExport& ca = a.chunks[index];
            const TrackChunkExport& cb = b.chunks[index];
            if (ca.layout != cb.layout || ca.anchor != cb.anchor || ca.bytes != cb.bytes ||
                ca.frameTimes != cb.frameTimes || ca.frameOffsets != cb.frameOffsets)
            {
                return false;
            }
        }
        return true;
    }

    struct SealComparison
    {
        std::size_t samples = 0;
        std::size_t mismatches = 0;
        std::size_t seals = 0;
        std::size_t cacheBytes = 0; // the unsealed track's playback caches after the path
        bool exportSame = false;
        bool freed = false;
    };

    SealComparison CompareSealedOverPath(const PoseTrack& pristine, const std::vector<std::uint32_t>& times)
    {
        SealComparison result;
        PoseTrack kept = pristine;
        PoseTrack sealed = pristine;
        PoseSample a;
        PoseSample b;
        for (std::size_t index = 0; index < times.size(); ++index)
        {
            if (index % kSealEvery == 0)
            {
                sealed.Seal();
                ++result.seals;
            }
            const bool okA = kept.Sample(times[index], a);
            const bool okB = sealed.Sample(times[index], b);
            ++result.samples;
            if (!okA || !okB || !SameSample(a, b))
                ++result.mismatches; // a recorded track samples every time (clamped), so a failure counts too
        }
        result.cacheBytes = kept.PlaybackCacheBytes();
        kept.Seal();
        sealed.Seal();
        result.freed = kept.EncoderBytes() == 0 && kept.PlaybackCacheBytes() == 0 && sealed.EncoderBytes() == 0 &&
            sealed.PlaybackCacheBytes() == 0;
        TrackExport before;
        TrackExport afterKept;
        TrackExport afterSealed;
        pristine.Export(before);
        kept.Export(afterKept);
        sealed.Export(afterSealed);
        result.exportSame = SameTrackExport(before, afterKept) && SameTrackExport(before, afterSealed);
        return result;
    }

    void TestSealedClipsFromEnv()
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
                std::fprintf(stderr, "  sealed clip %ls: not loaded (%s)\n", entry.path().filename().c_str(), error.c_str());
                continue;
            }
            const auto start = std::chrono::steady_clock::now();
            std::size_t tracks = 0;
            std::size_t samples = 0;
            std::size_t mismatches = 0;
            std::size_t seals = 0;
            std::size_t exportsSame = 0;
            std::size_t freed = 0;
            std::size_t importedEmpty = 0;
            std::uint64_t channels = 0;
            std::uint64_t cacheBytes = 0;
            for (const PoseRecordExport& record : poses)
            {
                PoseTrack track;
                std::string importError;
                if (record.track.chunks.empty() || !track.Import(record.track, importError))
                    continue;
                ++tracks;
                importedEmpty += (track.Sealed() && track.EncoderBytes() == 0 && track.PlaybackCacheBytes() == 0) ? 1u : 0u;
                channels += track.Layout().ChannelCount();
                const SealComparison result =
                    CompareSealedOverPath(track, SealPath(track.Stats().firstTimeMs, track.Stats().lastTimeMs));
                samples += result.samples;
                mismatches += result.mismatches;
                seals += result.seals;
                exportsSame += result.exportSame ? 1u : 0u;
                freed += result.freed ? 1u : 0u;
                cacheBytes += result.cacheBytes;
                if (result.mismatches)
                {
                    std::fprintf(stderr, "  sealed clip %ls: record 0x%x (%u bones) differs in %zu samples\n",
                        entry.path().filename().c_str(), record.recordId, record.boneCount, result.mismatches);
                }
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("  sealed clip %ls: %zu tracks, %zu samples compared (sealed every %zu samples, %zu seals), %zu differ; "
                        "exports identical %zu of %zu, sealed caches freed %zu of %zu, imported without encoder or decoder "
                        "memory %zu of %zu; playback caches after the path %.1f MB (%.0f bytes per channel), none after Seal "
                        "(%.1f s)\n",
                entry.path().filename().c_str(), tracks, samples, kSealEvery, seals, mismatches, exportsSame, tracks, freed,
                tracks, importedEmpty, tracks, static_cast<double>(cacheBytes) / (1024.0 * 1024.0),
                channels ? static_cast<double>(cacheBytes) / static_cast<double>(channels) : 0.0, seconds);
            Check(mismatches == 0 && exportsSame == tracks && freed == tracks && importedEmpty == tracks,
                "recorded clip: a track sealed while it plays samples and exports what the unsealed track does");
        }
    }
}

int RunPoseSampleTests()
{
    g_sampleFailures = 0;
    TestSimdMatchesScalar();
    TestPoseHash();
    TestSampleMatchesLegacy();
    BenchmarkSynthetic();
    TestRecordedClipsFromEnv();
    TestSealedClipsFromEnv();
    if (g_sampleFailures == 0)
        std::printf("Pose sample tests passed.\n");
    return g_sampleFailures;
}
