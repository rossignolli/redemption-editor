// The mod side of the pose fidelity judge (pose_fidelity.h): the engine reads on the script
// thread, their clock, and the evaluation against the capture's tracks at
// the stop. The pure part is pose_fidelity_core.cpp.

#include "pose_fidelity.h"

#include "deferred_log.h"
#include "log_policy.h"
#include "pose_capture.h"
#include "pose_skeleton_fields.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <ostream>

namespace replay::pose::fidelity
{
    namespace
    {
        std::uint32_t g_hz = kDefaultHz;
        bool g_async = true; // PoseFidelityAsync
        Store g_store;
        // A record's first read copies its whole objects block once, for the
        // key bone choice (script thread only).
        std::vector<BoneQts> g_objects;

        enum class ReadStatus
        {
            Ok,
            Fault,
            NoPoseBuffer,
            BoneCount,
        };

        // The skeleton fields pose_capture.cpp reads (pose_skeleton_fields.h),
        // under SEH and without objects that have destructors: the pose
        // buffer, the parent (world) matrix and the bone count, which must be
        // the record's. Then the objects block (after boneCount locals):
        // whole (keyBones null) or the key bones in key order. A null world
        // matrix pointer leaves `world` as it was (zero: no origin compared).
        // Reads only.
        ReadStatus GuardedReadPose(const std::uint8_t* skeleton, std::uint32_t expectedBones,
            const std::uint32_t* keyBones, std::uint32_t keyCount, float* world, BoneQts* out)
        {
            __try
            {
                const std::uint8_t* poseBuffer = nullptr;
                const std::uint8_t* worldMatrix = nullptr;
                std::uint32_t boneCount = 0;
                std::memcpy(&poseBuffer, skeleton + skeleton_fields::kSkeletonPoseBuffer, sizeof(poseBuffer));
                std::memcpy(&worldMatrix, skeleton + skeleton_fields::kSkeletonWorldMatrix, sizeof(worldMatrix));
                std::memcpy(&boneCount, skeleton + skeleton_fields::kSkeletonBoneCount, sizeof(boneCount));
                if (!poseBuffer)
                    return ReadStatus::NoPoseBuffer;
                if (boneCount != expectedBones)
                    return ReadStatus::BoneCount;
                if (worldMatrix)
                    std::memcpy(world, worldMatrix, 16u * sizeof(float));
                const std::uint8_t* objects = poseBuffer + static_cast<std::size_t>(boneCount) * sizeof(BoneQts);
                if (!keyBones)
                {
                    std::memcpy(out, objects, static_cast<std::size_t>(boneCount) * sizeof(BoneQts));
                }
                else
                {
                    for (std::uint32_t key = 0; key < keyCount; ++key)
                    {
                        std::memcpy(&out[key], objects + static_cast<std::size_t>(keyBones[key]) * sizeof(BoneQts),
                            sizeof(BoneQts));
                    }
                }
                return ReadStatus::Ok;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return ReadStatus::Fault;
            }
        }

        // The recorded tracks, as the playback samples them (after StopAll).
        class CaptureTracks final : public TrackSource
        {
        public:
            bool Span(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs) override
            {
                return capture::TrackSpan(recordId, firstMs, lastMs);
            }
            bool Sample(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample) override
            {
                return capture::SampleTrack(recordId, timeMs, sample);
            }
        };
    }

    void Configure(bool enabled, std::uint32_t hz)
    {
        g_hz = std::clamp(hz, kMinHz, kMaxHz);
        detail::g_enabled.store(enabled, std::memory_order_relaxed);
        // The records started from now on keep the times of their content (capture::ContentTimeMs); nothing with the key at 0.
        capture::SetContentTimes(enabled);
    }

    std::uint32_t Hz()
    {
        return g_hz;
    }

    void SetBufferMode(bool buffer)
    {
        g_store.SetBufferMode(buffer);
    }

    void Reset()
    {
        g_store.Reset();
        std::vector<BoneQts>().swap(g_objects);
    }

    bool Due(std::uint32_t recordId, std::uint32_t nowMs)
    {
        return Enabled() && g_store.Due(recordId, nowMs, IntervalMs(g_hz));
    }

    void Sample(std::uint32_t recordId, const void* skeleton, std::uint32_t boneCount, Kind kind,
        std::uint32_t tickMs)
    {
        if (!Enabled())
            return;
        // The buffer's trim on the tick's clock, before the record is taken
        // (Trim may release records). Trimming only after a kept read would
        // never trim a full store again (every read refused).
        g_store.TrimOnClock(tickMs);
        if (!skeleton || boneCount == 0)
            return;
        RecordSamples& record = g_store.Record(recordId);
        record.kind = kind;
        if (record.boneCount == 0)
            record.boneCount = boneCount;
        if (boneCount != record.boneCount)
        {
            ++record.skipped;
            return;
        }
        if (!g_store.HasRoom(record))
        {
            ++record.dropped; // over the memory cap: no more reads
            return;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(skeleton);
        TruthSample truth;
        ReadStatus status = ReadStatus::Ok;
        if (record.keyBones.empty())
        {
            // First read: the whole objects block, the key bones chosen from it.
            g_objects.resize(boneCount);
            status = GuardedReadPose(bytes, boneCount, nullptr, 0, truth.world, g_objects.data());
            if (status == ReadStatus::Ok)
            {
                record.keyBones = ChooseKeyBones(g_objects.data(), boneCount, kKeyBones);
                FillTruth(truth, record.keyBones.data(), static_cast<std::uint32_t>(record.keyBones.size()),
                    g_objects.data());
            }
        }
        else
        {
            BoneQts keyObjects[kKeyBones];
            status = GuardedReadPose(bytes, boneCount, record.keyBones.data(),
                static_cast<std::uint32_t>(record.keyBones.size()), truth.world, keyObjects);
            if (status == ReadStatus::Ok)
                FillTruthFromKeys(truth, keyObjects, static_cast<std::uint32_t>(record.keyBones.size()));
        }
        const std::uint32_t keyCount = static_cast<std::uint32_t>(record.keyBones.size());
        if (status != ReadStatus::Ok || keyCount == 0 || !TruthUsable(truth, keyCount))
        {
            ++record.skipped;
            return;
        }
        // The time the capture gives what was
        // read (the finalizer call that wrote it, or the script's capture
        // of it in this tick), where the track holds it; the tick's time
        // when the record has neither. Not the time of this read: the
        // buffer keeps a call's output until the next call, so the read
        // comes up to a frame after its content, later still behind the
        // reads before it in the tick.
        std::uint32_t contentMs = 0;
        truth.timeMs = capture::ContentTimeMs(recordId, contentMs) ? contentMs : tickMs;
        g_store.Append(record, truth);
    }

    void SetAsync(bool async)
    {
        g_async = async;
    }

    bool Async()
    {
        return g_async;
    }

    namespace
    {
        double ElapsedMs(const LARGE_INTEGER& started)
        {
            LARGE_INTEGER frequency{};
            LARGE_INTEGER ended{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&ended);
            return frequency.QuadPart > 0
                ? 1000.0 * static_cast<double>(ended.QuadPart - started.QuadPart) / static_cast<double>(frequency.QuadPart)
                : 0.0;
        }
    }

    void Evaluate(std::ostream& log)
    {
        if (!Enabled())
            return;
        if (g_async)
        {
            // A long clip can spend seconds of its stop tick here, the game
            // frozen (about 2.8 s for 108k reads). The worker gets the reads (moved: the next
            // recording starts with an empty store) and the tracks' export
            // (ExportedTracks: copies of its own, so a recording or a
            // playback that starts meanwhile shares nothing with it). It
            // reads no engine memory, calls no native and writes no log;
            // its text is written by the script thread (deferred_log.h),
            // after the stop's own deferred lines.
            auto store = std::make_shared<Store>(std::move(g_store));
            Reset(); // the moved-from store and the key bone scratch, empty
            g_store.SetBufferMode(store->BufferMode());
            auto records = std::make_shared<std::vector<PoseRecordExport>>(capture::ExportRecords());
            const std::uint32_t hz = g_hz;
            const bool perRecord = replay::log::Verbose(); // per record: Debug only (evidence)
            replay::deferredlog::Log().Submit([store, records, hz, perRecord]() {
                LARGE_INTEGER started{};
                QueryPerformanceCounter(&started);
                Report report;
                ExportedTracks tracks(std::move(*records));
                EvaluateStore(*store, tracks, report);
                const double evalMs = ElapsedMs(started);
                std::string text = ReportText(report, hz, evalMs, perRecord);
                if (tracks.ImportFailures() != 0)
                    text += "POSE_FIDELITY_IMPORT,failed=" + std::to_string(tracks.ImportFailures()) +
                        ",last_error=" + tracks.LastImportError() + "\n";
                return text;
            });
            return;
        }
        LARGE_INTEGER frequency{};
        LARGE_INTEGER started{};
        LARGE_INTEGER ended{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        Report report;
        CaptureTracks tracks;
        EvaluateStore(g_store, tracks, report);
        QueryPerformanceCounter(&ended);
        const double evalMs = frequency.QuadPart > 0
            ? 1000.0 * static_cast<double>(ended.QuadPart - started.QuadPart) / static_cast<double>(frequency.QuadPart)
            : 0.0;
        // Per record: Debug only (evidence); the summaries always.
        if (replay::log::Verbose())
        {
            for (const RecordResult& record : report.records)
                log << RecordLine(record) << '\n';
        }
        log << SummaryLine("all", report.all, g_hz, evalMs) << '\n';
        for (std::size_t kind = 0; kind < kKindCount; ++kind)
        {
            if (report.kinds[kind].records != 0)
                log << SummaryLine(KindText(static_cast<Kind>(kind)), report.kinds[kind], g_hz, evalMs) << '\n';
        }
        log.flush();
        Reset(); // the reads are done with; the next recording starts empty
    }
}
