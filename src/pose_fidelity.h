#pragma once

// Pose fidelity judge.
//
// A numeric replay quality check, so a change set can be judged without
// screenshots. With RedemptionEditor.ini [RedemptionEditor] PoseFidelity=1 (default 0)
// the script thread reads, during a recording, the ENGINE pose of every
// recorded entity PoseFidelityHz times a second (default 10, 1 to 60): the
// skeleton's parent (world) matrix and the object transforms of 16 key
// bones, stamped with the recording time the capture gives that same
// content (capture::ContentTimeMs: the finalizer call that wrote it, or the
// script's own capture of it in this tick; the tick's capture::NowMs() when
// the record has neither). A stamp taken at the read would put a
// finalizer-captured entity ahead of its content by the time from the
// finalizer to the read (7.7 m/s for 8 ms: 62 mm on an exact track), and
// move with the work done earlier in the script tick.
// When the recording stops every read is compared with what the recorded
// track gives at the same recording time (capture::SampleTrack, the sample
// the playback publishes): per key bone the position error in mm and the
// rotation error in mrad, and the entity origin error (the placement the
// playback derives from the sample) in mm. That measures the whole capture
// chain (sampling instants, validation, quantization, thresholds,
// interpolation) against the pose the engine had at those instants.
//
// It only reads: engine reads under SEH on the script thread, no natives, no
// writes; in the detours only the time of each finalizer call is kept for
// it, in the capture record (capture::SetContentTimes, key on); the tracks
// are sampled after the recording stopped, before any playback. Nothing
// recorded changes. With the key at 0 nothing runs (one flag test per
// script tick, and one per call of an armed record in the detour and the
// script captures).
//
// The pure part (key bone choice, error math, histogram and percentiles, the
// comparison of one engine sample with one PoseSample, the storage and the
// evaluation over a track source, the log lines) is pose_fidelity_core.cpp,
// which ReplayCoreTests compiles too (tests/pose_fidelity_tests.cpp); the
// engine reads, the clock and the log are pose_fidelity.cpp (ReplayProbe).

#include "pose_stream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::pose::fidelity
{
    constexpr std::uint32_t kKeyBones = 16;
    constexpr std::uint32_t kDefaultHz = 10;
    constexpr std::uint32_t kMinHz = 1;
    constexpr std::uint32_t kMaxHz = 60;
    // Samples kept at most (then sampling stops and the reads count as
    // dropped), and in buffer mode the window of samples kept.
    // A read is 516 bytes; a 207 s town clip (849 records at 10 Hz) asks for
    // about 128k reads, 66 MB without the vectors' growth room, more than a
    // 64 MB cap holds.
    // Reserved only with PoseFidelity=1, released when the evaluation ends.
    constexpr std::size_t kMemoryCapBytes = 128u * 1024u * 1024u;
    constexpr std::uint32_t kBufferKeepMs = 120000;
    constexpr std::uint32_t kBufferTrimEveryMs = 1000; // Store::TrimOnClock
    // An error that is not a finite number (a NaN in the recorded sample), or
    // above this (1 km in mm: garbage), counts as this many units: in the
    // overflow bin, and the max shows it.
    constexpr float kNonFiniteError = 1.0e6f;

    enum class Kind : std::uint8_t
    {
        Ped = 0,
        Horse = 1,
        Vehicle = 2,
        Other = 3, // posed objects (a carried pelt) and anything without a ped, horse or vehicle create
    };
    constexpr std::size_t kKindCount = 4;
    const char* KindText(Kind kind);

    // ------------------------------------------------------------------
    // Pure part (pose_fidelity_core.cpp).

    // A position usable for the key bone choice: finite (tested on the bits,
    // so /fp:fast cannot drop it) and within 10 km of the object origin.
    bool UsablePosition(const BoneQts& bone);

    // The key bones of a skeleton: farthest-point sampling in object space
    // from bone 0 (or the first usable bone when bone 0 is not usable):
    // each next bone is the one whose distance to the nearest chosen bone
    // is the largest (ties: the lowest index). Unusable positions are never
    // chosen; fewer than `count` usable bones gives them all. Deterministic.
    std::vector<std::uint32_t> ChooseKeyBones(const BoneQts* objects, std::uint32_t boneCount,
        std::uint32_t count = kKeyBones);

    // One engine read of a record: the skeleton parent (world) matrix and the
    // object transform of each key bone, in the order of the record's key
    // bones.
    struct TruthSample
    {
        std::uint32_t timeMs = 0;
        float world[16] = {};
        Vec3 position[kKeyBones] = {};
        Quat rotation[kKeyBones] = {};
    };
    // Fills the key bones of `truth` from a whole objects block (key bone k is
    // objects[keyBones[k]]) or, with `keyObjects`, from the key bones' own
    // records in key order.
    void FillTruth(TruthSample& truth, const std::uint32_t* keyBones, std::uint32_t keyCount,
        const BoneQts* objects);
    void FillTruthFromKeys(TruthSample& truth, const BoneQts* keyObjects, std::uint32_t keyCount);
    // Every key bone's position and rotation finite.
    bool TruthUsable(const TruthSample& truth, std::uint32_t keyCount);

    // The placement the playback gives a sample (replay_playback.cpp
    // SampleWorldTransform): world A decoded, blended with a decodable world
    // B by the sample's t (InterpolateWorld), else A. False without A.
    bool PlacementOf(const PoseSample& sample, WorldTransform& world);
    // |recorded translation - truth| in mm; AngleBetween in mrad.
    float PositionErrorMm(const Vec3& truth, const BoneQts& recorded);
    float RotationErrorMrad(const Quat& truth, const BoneQts& recorded);

    // One engine read against the sample the playback publishes at its time.
    struct SampleError
    {
        std::uint32_t bones = 0; // key bones compared (those inside the sample)
        std::uint32_t bone[kKeyBones] = {};
        float positionMm[kKeyBones] = {};
        float rotationMrad[kKeyBones] = {};
        bool hasOrigin = false; // both placements decoded
        float originMm = 0.0f;
    };
    bool Compare(const TruthSample& truth, const std::uint32_t* keyBones, std::uint32_t keyCount,
        const PoseSample& sample, SampleError& out);

    // Errors of one kind (mm or mrad): mean, exact max, counts over 10, 50
    // and 200, and a histogram of 0.5 wide bins up to 2000 plus an overflow
    // bin for the percentiles.
    class Distribution
    {
    public:
        static constexpr float kBinWidth = 0.5f;
        static constexpr std::uint32_t kBins = 4000; // [0, 2000), then the overflow bin

        Distribution();
        void Reset();
        // Negative values count as 0; non-finite ones as kNonFiniteError.
        void Add(float value);
        std::uint64_t Count() const { return m_count; }
        double Mean() const { return m_count ? m_sum / static_cast<double>(m_count) : 0.0; }
        float Max() const { return m_max; }
        // Nearest rank: the value of rank NearestRank(p, Count()) lies in a
        // histogram bin; the result is that bin's upper edge capped by the max
        // (the max itself in the overflow bin): never below the exact
        // percentile and at most one bin above it. 0 when empty.
        float Percentile(double p) const;
        std::uint64_t Over10() const { return m_over10; }
        std::uint64_t Over50() const { return m_over50; }
        std::uint64_t Over200() const { return m_over200; }

    private:
        std::vector<std::uint32_t> m_bins;
        std::uint64_t m_count = 0;
        double m_sum = 0.0;
        float m_max = 0.0f;
        std::uint64_t m_over10 = 0;
        std::uint64_t m_over50 = 0;
        std::uint64_t m_over200 = 0;
    };
    // The rank (1-based) of the p percentile among `count` values:
    // ceil(p * count), within [1, count]; 0 when count is 0.
    std::uint64_t NearestRank(double p, std::uint64_t count);

    // Per record: its kind, key bones and engine reads.
    struct RecordSamples
    {
        std::uint32_t recordId = 0;
        Kind kind = Kind::Other;
        std::uint32_t boneCount = 0;
        std::uint32_t nextDueMs = 0;
        std::uint32_t lastActiveMs = 0; // the last Due for it (the buffer forgets idle records)
        std::vector<std::uint32_t> keyBones;
        std::vector<TruthSample> samples; // from `head` on (the buffer drops older ones)
        std::size_t head = 0;
        std::uint64_t skipped = 0; // reads refused (bone count, fault, no pose buffer, not finite)
        std::uint64_t dropped = 0; // reads over the memory cap
        std::size_t Held() const { return samples.size() - head; }
    };

    // The reads of one recording (script thread). Records stay in creation
    // order.
    class Store
    {
    public:
        void Reset();
        // Buffer mode keeps only the last kBufferKeepMs of reads (Trim).
        void SetBufferMode(bool buffer) { m_buffer = buffer; }
        bool BufferMode() const { return m_buffer; }
        void SetCapBytes(std::size_t bytes) { m_capBytes = bytes; } // tests
        // True when the record's next read is due at nowMs, and then the one
        // after is scheduled intervalMs later (from now again when it fell
        // behind). A record seen for the first time is scheduled
        // StaggerMs(recordId, intervalMs) after nowMs, so the entities of one
        // scan do not all read in the same tick.
        bool Due(std::uint32_t recordId, std::uint32_t nowMs, std::uint32_t intervalMs);
        RecordSamples& Record(std::uint32_t recordId);
        const RecordSamples* Find(std::uint32_t recordId) const;
        // Room for one more read of the record under the memory cap (the
        // mod skips the engine read, counted as dropped, without it).
        bool HasRoom(const RecordSamples& record) const;
        // False (and counted in `dropped`) when the memory cap refuses it.
        bool Append(RecordSamples& record, const TruthSample& sample);
        // Buffer mode: drops the reads older than kBufferKeepMs before nowMs
        // and forgets the records with none left that no Due asked for since.
        void Trim(std::uint32_t nowMs);
        // Buffer mode, on the recording clock: Trim(nowMs) once every
        // kBufferTrimEveryMs. The mod calls it before each read it is due to
        // take, kept or refused (Trim may release records: before Record). A
        // trim that waited for a kept read would never run again once the cap
        // was full (every read refused).
        void TrimOnClock(std::uint32_t nowMs);
        // Bytes reserved for the reads (vector capacity), what the cap counts.
        std::size_t Bytes() const { return m_bytes; }
        const std::vector<std::unique_ptr<RecordSamples>>& Records() const { return m_records; }

    private:
        std::vector<std::unique_ptr<RecordSamples>> m_records;
        std::unordered_map<std::uint32_t, RecordSamples*> m_index;
        std::size_t m_bytes = 0;
        std::size_t m_capBytes = kMemoryCapBytes;
        bool m_buffer = false;
        std::uint32_t m_nextTrimMs = 0; // TrimOnClock
    };
    std::uint32_t StaggerMs(std::uint32_t recordId, std::uint32_t intervalMs);
    std::uint32_t IntervalMs(std::uint32_t hz);

    // Where the evaluation reads the recorded tracks (capture::TrackSpan and
    // capture::SampleTrack in the mod, a PoseTrack in the tests).
    class TrackSource
    {
    public:
        virtual ~TrackSource() = default;
        virtual bool Span(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs) = 0;
        virtual bool Sample(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample) = 0;
    };

    struct RecordResult
    {
        std::uint32_t recordId = 0;
        Kind kind = Kind::Other;
        std::uint64_t samples = 0; // reads compared
        std::uint64_t bones = 0;   // key bone comparisons
        std::uint64_t outside = 0; // reads outside the track span (or without a track)
        std::uint64_t skipped = 0;
        std::uint64_t dropped = 0;
        double posMeanMm = 0.0;
        float posP95Mm = 0.0f;
        float posMaxMm = 0.0f;
        float rotP95Mrad = 0.0f;
        float originMaxMm = 0.0f;
        std::uint32_t worstMs = 0;
        std::int32_t worstBone = -1;
    };

    struct KindSummary
    {
        std::uint64_t records = 0;
        std::uint64_t samples = 0;
        std::uint64_t bones = 0;
        std::uint64_t outside = 0;
        std::uint64_t skipped = 0;
        std::uint64_t dropped = 0;
        Distribution position; // mm
        Distribution rotation; // mrad
        Distribution origin;   // mm
        float worstMm = -1.0f;
        std::uint32_t worstRecord = 0;
        std::uint32_t worstMs = 0;
        std::int32_t worstBone = -1;
    };

    struct Report
    {
        KindSummary all;
        KindSummary kinds[kKindCount];
        std::vector<RecordResult> records; // records with any read, in creation order
    };

    // Every record's reads, in time order, inside the track span against
    // the track (outside: counted). Samples each track forward in time.
    void EvaluateStore(const Store& store, TrackSource& source, Report& report);

    // The recorded tracks as copies of
    // their own, for an evaluation off the script thread. The capture's
    // export of the stopped recording (capture::ExportRecords: the chunk
    // bytes are shared, copied on write, never changed in place), each
    // record imported into a PoseTrack of this source when the evaluation
    // first asks for it (PoseTrack::Import, as a clip load does: the same
    // chunks, layouts, anchors and span, sealed), so nothing the script
    // thread or a playback starting meanwhile touches is shared: not the
    // capture's records, not their playback caches. The first record of an
    // id answers, as capture::FindRecord does. Only the last record asked
    // for keeps its track (EvaluateStore asks for one record at a time);
    // one asked for again is imported again. A record that does not import
    // has no track (its reads count as outside) and is counted.
    class ExportedTracks final : public TrackSource
    {
    public:
        explicit ExportedTracks(std::vector<PoseRecordExport> records);
        bool Span(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs) override;
        bool Sample(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample) override;
        std::uint64_t ImportFailures() const { return m_importFailures; }
        const std::string& LastImportError() const { return m_lastImportError; }

    private:
        PoseTrack* TrackOf(std::uint32_t recordId);

        std::vector<PoseRecordExport> m_records;
        std::unordered_map<std::uint32_t, std::size_t> m_index; // first record of an id
        std::uint32_t m_currentId = 0;
        bool m_hasCurrent = false;
        std::unique_ptr<PoseTrack> m_current; // null: the current id has no track
        std::uint64_t m_importFailures = 0;
        std::string m_lastImportError;
    };

    // POSE_FIDELITY (per record) and POSE_FIDELITY_SUMMARY lines, without the
    // line break.
    std::string RecordLine(const RecordResult& record);
    std::string SummaryLine(const char* kind, const KindSummary& summary, std::uint32_t hz, double evalMs);
    // Every line the evaluation writes, each
    // with its line break, in the order the stop wrote them: the per-record
    // lines when `perRecord` (Debug), then the summary of all and of each
    // kind with records.
    std::string ReportText(const Report& report, std::uint32_t hz, double evalMs, bool perRecord);

    // ------------------------------------------------------------------
    // The mod (pose_fidelity.cpp, script thread).

    namespace detail
    {
        inline std::atomic<bool> g_enabled{false};
    }
    // PoseFidelity and PoseFidelityHz (clamped to [kMinHz, kMaxHz]), at load.
    void Configure(bool enabled, std::uint32_t hz);
    inline bool Enabled() { return detail::g_enabled.load(std::memory_order_relaxed); }
    std::uint32_t Hz();
    // The recording that starts is the always-on buffer (script.cpp
    // StartRecording); set after Reset.
    void SetBufferMode(bool buffer);
    // A recording starts (replay_capture.cpp, with capture::BeginRecording):
    // every read of the previous one goes.
    void Reset();
    // UpdatePoseV2Capture: true when the record's read is due now.
    bool Due(std::uint32_t recordId, std::uint32_t nowMs);
    // Reads the engine pose of a recorded entity (only when Due said so):
    // `skeleton` is the skeleton the record is armed on, `boneCount` the
    // record's bone count, `tickMs` the tick's capture::NowMs() (the
    // buffer's trim clock, and the read's time when the capture has no time
    // for its content). After every script capture of the tick.
    void Sample(std::uint32_t recordId, const void* skeleton, std::uint32_t boneCount, Kind kind,
        std::uint32_t tickMs);
    // The recording stopped (after capture::StopAll, before any playback):
    // the reads against the tracks, logged; the reads are then released.
    // With PoseFidelityAsync=1 (default) the stop tick only exports the
    // tracks (ExportedTracks) and hands the reads and that export to a
    // worker (deferred_log.h); the same lines are written on a later tick,
    // eval_ms the worker's time. At 0 it runs here, on the stop tick (seconds
    // for a long clip).
    void Evaluate(std::ostream& log);
    void SetAsync(bool async);
    bool Async();
}
