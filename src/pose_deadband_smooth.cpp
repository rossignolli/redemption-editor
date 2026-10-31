#include "pose_deadband_smooth.h"

#include "pose_skeleton_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <atomic>
#include <new>
#include <thread>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace replay::pose::smooth
{
    namespace
    {
        struct Frame
        {
            std::uint32_t timeMs = 0;
            std::uint16_t flags = 0; // kFrameNoInterpolation only
            float world[16] = {};
            std::uint32_t layout = 0;
            std::vector<BoneQts> locals;
            std::vector<BoneQts> objects;
        };

        std::uint16_t RecordFlags(const TrackChunkExport& chunk, std::size_t frame)
        {
            if (frame >= chunk.frameOffsets.size())
                return 0;
            const std::size_t at = chunk.frameOffsets[frame] + 8; // u32 size, u32 time, u16 flags
            if (at + 2 > chunk.bytes.size())
                return 0;
            std::uint16_t flags = 0;
            std::memcpy(&flags, chunk.bytes.data() + at, sizeof(flags));
            return flags;
        }

        // The neighbours belong to the same run as the middle frame.
        bool SameRun(const Frame& prev, const Frame& cur, const Frame& next)
        {
            return prev.layout == cur.layout && next.layout == cur.layout && cur.timeMs > prev.timeMs &&
                next.timeMs > cur.timeMs && cur.timeMs - prev.timeMs <= kMaxGapMs && next.timeMs - cur.timeMs <= kMaxGapMs &&
                !(cur.flags & kFrameNoInterpolation) && !(next.flags & kFrameNoInterpolation) &&
                prev.locals.size() == cur.locals.size() && next.locals.size() == cur.locals.size();
        }
    }

    std::vector<WorldFrame> WorldFrames(const TrackExport& track)
    {
        std::vector<WorldFrame> frames;
        for (const TrackChunkExport& chunk : track.chunks)
        {
            for (std::size_t index = 0; index < chunk.frameTimes.size() && index < chunk.frameOffsets.size(); ++index)
            {
                // u32 size, u32 time, u16 flags, u16 count, f32 world[16]
                const std::size_t at = chunk.frameOffsets[index];
                if (at + 12 + sizeof(float) * 16 > chunk.bytes.size())
                    continue;
                WorldFrame frame;
                frame.timeMs = chunk.frameTimes[index];
                if (!frames.empty() && frame.timeMs <= frames.back().timeMs)
                    continue;
                std::memcpy(&frame.flags, chunk.bytes.data() + at + 8, sizeof(frame.flags));
                std::memcpy(frame.world, chunk.bytes.data() + at + 12, sizeof(frame.world));
                frames.push_back(frame);
            }
        }
        return frames;
    }

    std::vector<std::size_t> FindWorldSpikes(const std::vector<WorldFrame>& frames)
    {
        std::vector<std::size_t> spikes;
        if (frames.size() < 5)
            return spikes;
        std::vector<WorldTransform> world(frames.size());
        std::vector<std::uint8_t> valid(frames.size(), 0);
        for (std::size_t n = 0; n < frames.size(); ++n)
            valid[n] = DecodeWorldMatrix(frames[n].world, world[n]) ? 1 : 0;
        const auto distance = [&](std::size_t a, std::size_t b) {
            const float dx = world[a].position.x - world[b].position.x;
            const float dy = world[a].position.y - world[b].position.y;
            const float dz = world[a].position.z - world[b].position.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const auto turn = [&](std::size_t a, std::size_t b) { return AngleBetween(world[a].rotation, world[b].rotation); };
        for (std::size_t n = 2; n + 2 < frames.size(); ++n)
        {
            bool ok = true;
            for (std::size_t k = n - 2; k <= n + 2 && ok; ++k)
            {
                ok = valid[k] != 0;
                if (ok && k > n - 2)
                    ok = frames[k].timeMs > frames[k - 1].timeMs && frames[k].timeMs - frames[k - 1].timeMs <= kMaxGapMs &&
                        !(frames[k].flags & kFrameNoInterpolation);
            }
            if (!ok)
                continue;
            const bool left = distance(n, n - 1) >= kSpikeMinM || turn(n, n - 1) >= kSpikeMinRad;
            const bool back = distance(n + 1, n - 1) <= kSpikeBackM && turn(n + 1, n - 1) <= kSpikeBackRad;
            const bool still = distance(n - 1, n - 2) <= kSpikeStillM && distance(n + 2, n + 1) <= kSpikeStillM;
            if (left && back && still)
                spikes.push_back(n);
        }
        return spikes;
    }

    bool SmoothTrack(const TrackExport& in, TrackExport& out, SmoothStats& stats, std::string& error, bool smoothBones)
    {
        stats = {};
        PoseTrack source;
        if (!source.Import(in, error))
            return false;
        if (in.layouts.empty() || in.chunks.empty())
        {
            error = "empty_track";
            return false;
        }

        StreamConfig config = in.config;
        if (smoothBones)
        {
            config.rotationEpsilon = kStandardRotationEpsilon;
            config.translationEpsilon = kStandardTranslationEpsilon;
        }
        config.byteBudget = std::numeric_limits<std::size_t>::max();
        // The one frame matrix spikes: the repaired matrix by frame time.
        std::vector<std::pair<std::uint32_t, std::array<float, 16>>> repaired;
        {
            const std::vector<WorldFrame> frames = WorldFrames(in);
            for (const std::size_t n : FindWorldSpikes(frames))
            {
                const std::size_t from =
                    frames[n].timeMs - frames[n - 1].timeMs <= frames[n + 1].timeMs - frames[n].timeMs ? n - 1 : n + 1;
                std::array<float, 16> world{};
                std::memcpy(world.data(), frames[from].world, sizeof(frames[from].world));
                repaired.emplace_back(frames[n].timeMs, world);
            }
            stats.worldSpikes = static_cast<std::uint32_t>(repaired.size());
        }
        std::size_t nextRepair = 0;
        const Gate gate = Gate::For(in.config);

        PoseTrack target;
        bool begun = false;
        std::uint32_t targetLayout = 0;
        std::vector<BoneQts> smoothed, composed, unresolved;
        const auto emit = [&](const Frame& frame, const Frame* prev, const Frame* next) {
            const SkeletonLayout& layout = in.layouts[frame.layout];
            if (!begun)
            {
                target.Begin(layout, config);
                targetLayout = frame.layout;
                begun = true;
            }
            else if (frame.layout != targetLayout)
            {
                target.UpdateLayout(layout);
                targetLayout = frame.layout;
            }
            const BoneQts* locals = frame.locals.data();
            const BoneQts* objects = frame.objects.data();
            if (smoothBones && prev && next && SameRun(*prev, frame, *next) && frame.locals.size() == layout.boneCount &&
                frame.objects.size() == layout.boneCount)
            {
                smoothed.resize(layout.boneCount);
                SmoothDeadband(prev->locals.data(), frame.locals.data(), next->locals.data(), layout.boneCount, gate,
                    smoothed.data());
                // Object space bones keep their decoded transform.
                for (const std::uint32_t bone : layout.unresolvedBones)
                    if (bone < layout.boneCount)
                        smoothed[bone] = frame.locals[bone];
                unresolved.clear();
                for (const std::uint32_t bone : layout.unresolvedBones)
                    unresolved.push_back(bone < frame.objects.size() ? frame.objects[bone] : BoneQts{});
                composed.resize(layout.boneCount);
                ComposeObjects(layout, smoothed.data(), unresolved.empty() ? nullptr : unresolved.data(), composed.data());
                locals = smoothed.data();
                objects = composed.data();
                ++stats.smoothedFrames;
            }
            FrameInput input;
            input.timeMs = frame.timeMs;
            input.flags = frame.flags;
            input.world = frame.world;
            while (nextRepair < repaired.size() && repaired[nextRepair].first < frame.timeMs)
                ++nextRepair;
            if (nextRepair < repaired.size() && repaired[nextRepair].first == frame.timeMs)
                input.world = repaired[nextRepair].second.data();
            input.locals = locals;
            input.objects = objects;
            target.Append(input);
            ++stats.frames;
        };

        // A window of three decoded frames: emit the middle one when the
        // next arrives.
        std::unique_ptr<Frame> window[3];
        std::size_t filled = 0;
        PoseSample sample;
        bool haveLast = false;
        std::uint32_t lastTime = 0;
        for (const TrackChunkExport& chunk : in.chunks)
        {
            for (std::size_t index = 0; index < chunk.frameTimes.size(); ++index)
            {
                const std::uint32_t t = chunk.frameTimes[index];
                if (haveLast && t <= lastTime)
                    continue;
                if (!source.Sample(t, sample) || sample.frameTimeA != t)
                    continue;
                auto frame = std::make_unique<Frame>();
                frame->timeMs = t;
                frame->flags = static_cast<std::uint16_t>(RecordFlags(chunk, index) & kFrameNoInterpolation);
                std::memcpy(frame->world, sample.worldA, sizeof(frame->world));
                frame->layout = chunk.layout;
                frame->locals = sample.locals;
                frame->objects = sample.objects;
                haveLast = true;
                lastTime = t;
                if (filled < 3)
                {
                    window[filled++] = std::move(frame);
                    if (filled == 1)
                        continue;
                    if (filled == 2)
                    {
                        emit(*window[0], nullptr, window[1].get());
                        continue;
                    }
                    emit(*window[1], window[0].get(), window[2].get());
                    continue;
                }
                window[0] = std::move(window[1]);
                window[1] = std::move(window[2]);
                window[2] = std::move(frame);
                emit(*window[1], window[0].get(), window[2].get());
            }
        }
        if (filled == 1)
            emit(*window[0], nullptr, nullptr);
        else if (filled >= 2)
            emit(*window[filled - 1], window[filled - 2].get(), nullptr);

        if (!begun)
        {
            error = "no_frames";
            return false;
        }
        target.Export(out);
        for (const TrackChunkExport& chunk : in.chunks)
            stats.bytesIn += chunk.bytes.size();
        for (const TrackChunkExport& chunk : out.chunks)
            stats.bytesOut += chunk.bytes.size();
        return true;
    }

    void SmoothRecords(std::vector<PoseRecordExport>& records, RecordsSummary& summary)
    {
        summary = {};
        summary.records = static_cast<std::uint32_t>(records.size());
        // The records are independent: up to half the cores (the load runs
        // beside the game), each worker taking the next record.
        struct Result
        {
            bool tried = false;
            bool ok = false;
            bool spikeOnly = false;
            SmoothStats stats;
            TrackExport track; // the smoothed track until it replaces the record's
        };
        std::vector<Result> results(records.size());
        std::atomic<std::size_t> nextRecord{0};
        const auto work = [&] {
            for (std::size_t n = nextRecord++; n < records.size(); n = nextRecord++)
            {
                const bool coarse = NeedsSmoothing(records[n].track.config);
                if (!coarse && FindWorldSpikes(WorldFrames(records[n].track)).empty())
                    continue;
                Result& result = results[n];
                result.tried = true;
                result.spikeOnly = !coarse;
                std::string error;
                try
                {
                    result.ok = SmoothTrack(records[n].track, result.track, result.stats, error, coarse);
                    // Each worker owns its record: the loaded track goes at once
                    // (the peak holds one extra track per worker, not the clip twice).
                    if (result.ok)
                        records[n].track = std::move(result.track);
                }
                catch (const std::bad_alloc&)
                {
                    result.ok = false;
                    result.track = {};
                }
            }
        };
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t workers = std::min<std::size_t>(records.size(), std::max(1u, cores / 2));
        std::vector<std::thread> threads;
        for (std::size_t n = 1; n < workers; ++n)
            threads.emplace_back(work);
        work();
        for (std::thread& thread : threads)
            thread.join();
        for (std::size_t n = 0; n < records.size(); ++n)
        {
            Result& result = results[n];
            if (!result.tried)
                continue;
            if (!result.ok)
            {
                ++summary.failed;
                continue;
            }
            summary.worldSpikes += result.stats.worldSpikes;
            if (result.spikeOnly)
            {
                ++summary.spikeRecords;
                continue;
            }
            ++summary.smoothed;
            summary.frames += result.stats.frames;
            summary.smoothedFrames += result.stats.smoothedFrames;
            summary.bytesIn += result.stats.bytesIn;
            summary.bytesOut += result.stats.bytesOut;
        }
    }

    namespace
    {
        std::atomic<bool> g_loadSmoothing{true};
        std::atomic<std::uint8_t> g_detail{static_cast<std::uint8_t>(Detail::Standard)};
    }
    void SetLoadSmoothing(bool on) { g_loadSmoothing.store(on); }
    bool LoadSmoothing() { return g_loadSmoothing.load(); }
    void SetDetail(Detail detail) { g_detail.store(static_cast<std::uint8_t>(detail)); }
    Detail CurrentDetail() { return static_cast<Detail>(g_detail.load()); }
}
