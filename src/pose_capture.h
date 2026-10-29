#pragma once

// PoseTrack v2: live skeleton capture.
//
// A detour on the engine pose finalizer copies each armed skeleton's pose
// buffer, together with its parent (world) matrix from the same instant, into
// that record's preallocated ring, at most `sampleHz` times per second. That
// is the only work done on the engine thread: a compare against the armed
// pointers, one rate check and a memcpy. A low-priority worker discovers each
// skeleton's layout from its first distinct frames and then feeds one
// replay::pose::PoseTrack per record.
//
// One record per recorded entity, keyed by its packed replay::entity::ReplayId.
// A record is armed while its entity is live in the recording and keeps its
// track after Stop, so entities that leave the recording stay playable.
//
// Capture never writes engine memory. The detour stays installed after the
// first Start and is removed by Shutdown (module detach).

#include "capture_capacity.h"
#include "pose_stream.h"
#include "tpose_check.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::pose::capture
{
    // Skeletons armed at the same time (peds and vehicles; about 13 to 17 us
    // per ped per frame plus ~250 us fixed). Playback overrides use as many
    // slots. The arrays hold kLiveTargetSlots; the places in use are the ini key
    // PedCapacity plus the vehicle class (capture_capacity.h), which
    // kMaxLiveTargets reads.
    constexpr std::uint32_t kLiveTargetSlots = replay::capacity::kLiveTargetSlots;
    inline constexpr replay::capacity::LiveTargetsValue kMaxLiveTargets{};

    struct Stats
    {
        std::uint32_t recordId = 0;

        // Engine-thread side.
        std::uint64_t captured = 0;
        std::uint64_t droppedRingFull = 0;
        std::uint64_t boneCountMismatch = 0;
        std::uint64_t copyFaults = 0;
        std::uint64_t detourCalls = 0;
        double detourCaptureAvgUs = 0.0;
        double detourCaptureMaxUs = 0.0;
        // Frames whose skeleton parent (world) matrix could not be copied.
        std::uint64_t worldReadFaults = 0;
        // Samples taken by PollFromScript (skeletons the finalizer does not
        // visit).
        std::uint64_t scriptPolls = 0;

        // Worker side.
        bool layoutReady = false;
        // The layout came from another record of the same skeleton that
        // explained this record's discovery frames (no search ran).
        bool layoutReused = false;
        // Cached layouts of this bone count tried before the search.
        std::uint32_t layoutCandidates = 0;
        DiscoveryStats discovery;
        std::uint64_t encodedFrames = 0;
        std::uint64_t keyframes = 0;
        std::uint64_t relativeKeyframes = 0;
        std::uint64_t fullKeyframeBytes = 0;
        std::uint64_t relativeKeyframeBytes = 0;
        std::uint64_t trackBytes = 0;
        std::uint64_t droppedChunks = 0;
        std::uint64_t pendingDropped = 0;
        double entriesPerFrameAvg = 0.0;
        double encodeAvgUs = 0.0;
        double encodeMaxUs = 0.0;
        double discoveryMs = 0.0;
        float compositionMaxTranslation = 0.0f;
        float compositionMaxRotation = 0.0f;
        std::uint32_t durationMs = 0;
        std::uint32_t boneCount = 0;

        // Bones demoted to object space because a later frame contradicted
        // the discovered parent.
        std::uint64_t demotedBones = 0;
        std::uint64_t demotionEvents = 0;
        // Demoted bones given a parent again by the discovery thread (a parent
        // that explains the discovery samples and the contradicting frames), and
        // the jobs that ran.
        std::uint64_t reparentedBones = 0;
        std::uint64_t reparentJobs = 0;
        // Jobs given up for an immediate demotion because the record had held too
        // many frames (safety valve).
        std::uint64_t reparentTimeouts = 0;
        // Record time of the first and last demotion (evidence for how soon
        // a skeleton's layout settles).
        std::uint32_t firstDemotionMs = 0;
        std::uint32_t lastDemotionMs = 0;
        // Frames flagged kFrameNoInterpolation by the rebase detector
        // (ragdoll exit, warp, teleport).
        std::uint64_t rebaseFrames = 0;
        std::uint64_t cutFrames = 0; // first frames after a recording cut

        // Engine parent table evidence: parents read from the finalizer's indices
        // argument (u16 at +0xC of each 16-byte record), root scale at
        // skeleton+0x110, group count at +0xD0; agreement with the discovered layout
        // and the error of composing the captured locals with the engine parents
        // against the captured objects.
        bool engineParents = false;
        bool engineParentOrderOk = false;
        float engineRootScale = 0.0f;
        std::uint32_t engineGroups = 0;
        std::uint32_t engineParentAgree = 0;
        std::uint32_t engineParentDisagree = 0;
        std::uint32_t engineParentUnresolved = 0;
        std::uint64_t engineComposeChecks = 0;
        float engineComposeMaxRotation = 0.0f;
        float engineComposeMaxTranslation = 0.0f;
        // Layout taken from the engine table (no search), frames whose objects the
        // worker composed from the locals with it, the table rejected by the
        // discovery frames, and locals-only frames that could not be completed
        // (dropped).
        bool engineLayout = false;
        std::uint64_t objectsComposed = 0;
        std::uint32_t engineLayoutRejected = 0;
        std::uint64_t framesWithoutObjects = 0;

        // End-of-frame checks (PollAndCheckLate). Checks run, checks skipped (the
        // finalizer ran again before the script tick), checks that found bones the
        // engine changed after the finalizer, the most local and object bones one
        // check found, the worst bone and its distance, the times the record went to
        // end-of-frame capture, the times the cap refused it, and its samples.
        std::uint64_t lateChecks = 0;
        std::uint64_t lateSkipped = 0;
        std::uint64_t lateChanged = 0;
        std::uint32_t lateMaxLocalBones = 0;
        std::uint32_t lateMaxObjectBones = 0;
        std::int32_t lateWorstBone = -1;
        float lateMaxTranslation = 0.0f;
        std::uint64_t lateSwitches = 0;
        std::uint64_t lateCapped = 0;
        std::uint64_t lateSamples = 0;

        // Capture shortcuts H1 to H5. Frames FindFailingBones ran on; frames the
        // worker composed itself on the exact engine layout, whose validation is
        // skipped (H1); of those, the ones composed only for the bones the encoder
        // reads (H2). Delta frame channels, those whose input had the previous
        // frame's 48 bytes (skipped by the encoder, H3), and delta frames where that
        // was every channel. End-of-frame checks settled by the memcmp alone (H5),
        // checks also run through the full comparison (1 in kLateVerifyEvery) and,
        // of those, results that differed (must stay 0: the full result is the one
        // used).
        std::uint64_t validatedFrames = 0;
        std::uint64_t validationSkipped = 0;
        // Validated frames checked only on their changed bones, and the bones
        // recomputed over all validated frames.
        std::uint64_t validationIncremental = 0;
        std::uint64_t validationBonesChecked = 0;
        std::uint64_t partialComposed = 0;
        std::uint64_t rawDeltaChannels = 0;
        std::uint64_t rawSameChannels = 0;
        std::uint64_t rawSameFrames = 0;
        std::uint64_t lateEqualChecks = 0;
        std::uint64_t lateVerified = 0;
        std::uint64_t lateVerifyMismatch = 0;

        // Evidence only: script copies (polls, end-of-frame) checked incrementally
        // against the last frame that passed. Idle: no finalizer call finished
        // between the two copies; frames with any local, any object or the root
        // different, and the bones that differed and were recomputed. After calls:
        // the finalizer ran in between; the calls, frames with nothing different,
        // bones that differed and were recomputed. Raced: script copies during which
        // the record's finalizer count moved (all script copies, checked or not).
        std::uint64_t pollIdleFrames = 0;
        std::uint64_t pollIdleLocalFrames = 0;
        std::uint64_t pollIdleObjectFrames = 0;
        std::uint64_t pollIdleRootFrames = 0;
        std::uint64_t pollIdleLocalBones = 0;
        std::uint64_t pollIdleObjectBones = 0;
        std::uint64_t pollIdleChecked = 0;
        std::uint64_t pollCallFrames = 0;
        std::uint64_t pollCallsSum = 0;
        std::uint64_t pollCallSameFrames = 0;
        std::uint64_t pollCallLocalBones = 0;
        std::uint64_t pollCallObjectBones = 0;
        std::uint64_t pollCallChecked = 0;
        std::uint64_t pollRaced = 0;

        // End-of-frame checks settled by comparing the engine buffer in place,
        // without the copy (counted in lateEqualChecks too). PoseStallPollMs: script
        // copies taken under the stall rule, and ticks where the default rule would
        // have copied but the last finalizer call was younger than the key; captures
        // that gave up because another capture of the record was at its ring (with
        // the key on, the detour and the script take turns). PoseLateBackoff: the
        // longest wait scheduled after an end-of-frame check.
        std::uint64_t lateDirect = 0;
        std::uint64_t stallPolls = 0;
        std::uint64_t pollsSkippedRecent = 0;
        std::uint64_t ringTurnBusy = 0;
        std::uint64_t lateIntervalMaxMs = 0;

        // Evidence: the rows the animation left unwritten in the finalizer's source
        // (a lane 0xFFFFFFFF: the finalizer takes the bind pose there), scanned on
        // detour samples through the call's indices (translation, rotation and scale
        // row of each bone): one sample in four after the first six. Most
        // translation and scale rows are never animated (always unwritten); the rows
        // that matter are the mixed ones, written in some samples and not in others
        // (the engine computed them only part of the time: hat fit, face, brim). Per
        // row kind (t, r, s): rows unwritten in every sample and mixed rows. The
        // fewest and most rows unwritten in one sample; rows with only some lanes
        // unwritten; faults; the scan's cost; the mixed rows, most unwritten first
        // ("bone" + t|r|s + ":samples"); the unwritten row count over the record's
        // time ("ms:rows", one entry per change).
        std::uint64_t unwrittenSamples = 0;
        std::uint32_t unwrittenAlways[3] = {};
        std::uint32_t unwrittenMixed[3] = {};
        std::uint32_t unwrittenMinRows = 0;
        std::uint32_t unwrittenMaxRows = 0;
        std::uint64_t unwrittenPartialRows = 0;
        std::uint64_t unwrittenFaults = 0;
        double unwrittenScanAvgUs = 0.0;
        double unwrittenScanMaxUs = 0.0;
        std::string unwrittenTop;
        std::string unwrittenTimeline;
    };

    // The places the script thread takes the capture's lock (g_mutex), for the
    // longest wait at each. Start: the pooled ring, then the new record; Other:
    // StopAll's last flush of held frames. Import runs between recordings, so a
    // recording's log shows 0 there.
    enum class ScriptLockSite : std::uint32_t
    {
        Start,
        Drop,
        Snapshot,
        RecordIds,
        Export,
        TrackSpan,
        Import,
        Other,
        Count,
    };
    constexpr std::size_t kScriptLockSites = static_cast<std::size_t>(ScriptLockSite::Count);
    // Their names in the POSE_V2_WORKER line (script_lock_max_us_<name>=).
    constexpr const char* kScriptLockSiteNames[kScriptLockSites] = {
        "start", "drop", "snapshot", "record_ids", "export", "track_span", "import", "other"};

    // Whole-recording cost, for the performance log.
    struct EngineTiming
    {
        // Detour work for every skeleton while anything is armed (the
        // original finalizer excluded).
        std::uint64_t detourCalls = 0;
        // Wakes sent to the worker because the next ring slot was still unread
        // (captures do not signal the worker otherwise).
        std::uint64_t pressureWakes = 0;
        double detourUsPerSecond = 0.0;
        double workerBusyMs = 0.0;
        double workerDiscoveryMs = 0.0;
        std::uint64_t pendingPeakFrames = 0;
        // The worker thread's time, which no profiler zone sees (the profiler
        // measures the script tick). CPU time of the worker thread (GetThreadTimes,
        // kernel plus user), frames it drained, and its busy time split: slot
        // handoff and stamping (drain), composition of locals-only frames,
        // validation (FindFailingBones and the periodic composition checks),
        // encoding (PoseTrack::Append), and the wait to take g_mutex (worker side,
        // total and longest). The script thread's waits for g_mutex (Start,
        // DropBefore, Snapshot, RecordIds, ExportRecords, TrackSpan, ImportRecords
        // and StopAll's flush): total, count and longest.
        double workerCpuMs = 0.0;
        std::uint64_t workerFrames = 0;
        double workerDrainMs = 0.0;
        double workerComposeMs = 0.0;
        double workerValidateMs = 0.0;
        double workerEncodeMs = 0.0;
        double workerLockWaitMs = 0.0;
        double workerLockWaitMaxUs = 0.0;
        double scriptLockWaitMs = 0.0;
        double scriptLockWaitMaxUs = 0.0;
        std::uint64_t scriptLockWaits = 0;
        // The longest script wait at each ScriptLockSite, and the longest single
        // hold of the lock by the discovery thread while recording (a discovery or a
        // re-parent job applied, with its first held frames).
        double scriptLockSiteMaxUs[kScriptLockSites] = {};
        double discoveryHoldMaxUs = 0.0;
        // The ring pool and the cost of Start.
        std::uint64_t ringPoolHits = 0;
        std::uint64_t ringPoolMisses = 0;
        double ringPoolMb = 0.0;
        std::uint64_t starts = 0;
        double startAvgUs = 0.0;
        double startMaxUs = 0.0;
        // Pose buffers Start took from the prefaulted reserve, and those it had to
        // allocate (reserve empty, or a skeleton of more than 1100 bones).
        std::uint64_t reserveHits = 0;
        std::uint64_t reserveMisses = 0;
    };
    EngineTiming EngineTimingSnapshot();

    // The capture path's shortcuts H1 to H5 (skip validating the worker's own
    // frames, compose only what the encoder reads, skip unchanged channels, cheap
    // tests and rounding, the end-of-frame check by memcmp) and the detour's
    // streaming copy (H9) are always on.
    // RedemptionEditor.ini PoseStallPollMs (0 to 1000, default 250,
    // kDefaultStallPollMs). N: PollIfStalled copies a ped the finalizer did not
    // visit since the previous script tick only when the finalizer never
    // visited it or its last visit is at least N ms old (a corpse, a ragdoll, a
    // posed object), through a rate gate of its own, so the detour's gate keeps
    // the detour's samples. 0 (the default up to 1.0): whenever the finalizer
    // did not visit it, through the detour's rate gate; an NPC the engine
    // animates on fewer frames than the game runs (off the game camera) is then
    // copied on the skipped frames, with the previous pose still in the buffer,
    // and the track repeats poses while the body moves. Changes the recorded
    // samples. Each record keeps the value Start saw.
    void SetStallPollMs(std::uint32_t ms);
    std::uint32_t StallPollMs();
    // RedemptionEditor.ini PoseLateBackoff (default 0). 1: the end-of-frame
    // check of a clean record waits 1, 2, 4, then 8 s (a change: 250 ms, then 1 s
    // again), and a record's first check waits a hash of its id (0 to 999 ms)
    // after its engine parent table is ready instead of running at once. Each
    // record keeps the value Start saw.
    void SetLateBackoff(bool on);
    bool LateBackoff();
    // The pose fidelity judge is on (fidelity::Configure, at load): each record
    // Start arms keeps the times ContentTimeMs reads (the detour the time of
    // every finalizer call, the script captures their own). Off: nothing kept.
    // Each record keeps the value Start saw.
    void SetContentTimes(bool on);

    // Drops every record of the previous recording and starts the recording
    // clock. Every record's times are milliseconds on this clock.
    void BeginRecording();
    std::uint32_t NowMs();
    // The pause menu stops the recording clock and the pose capture (the clip
    // leaves the pause out, as a replay editor usually does). True when the
    // state changed. Cleared by BeginRecording.
    bool SetClockPaused(bool paused);
    // The recording gate's single check. While true nothing is recorded (every
    // observer skips its samplers and discards what its detour queue
    // delivers).
    bool IsClockPaused();
    // The clock goes on after a pause that is a cut (cutscene, fade, loading):
    // the next frame of every pose record is flagged kFrameNoInterpolation. Call
    // before SetClockPaused(false).
    void MarkCut();
    std::uint32_t PausedMsTotal();
    // The recording time of an event `ageMs` old (hook events carry QPC
    // stamps): a pause or a script stall between the event and now is not
    // counted, so an event queued just before the pause menu keeps its place
    // instead of landing a whole pause earlier. Capped at nowMs.
    std::uint32_t RecordMsAgo(std::uint64_t ageMs, std::uint32_t nowMs);
    // The game does not run the script thread while its pause menu is open (no
    // tick, so no RECORD_PAUSE, while the QPC clock keeps counting). Called first
    // in every recording tick: when the previous tick is at least minGapMs back
    // (and the clock was not already paused) the gap becomes a pause span after
    // the fact, starting no earlier than the last stamped pose frame. Returns
    // the milliseconds excluded (0 when none).
    std::uint32_t ExcludeScriptStall(std::uint32_t minGapMs);

    // Capture places beyond kMaxLiveTargets for the weapons in a hand
    // (WeaponBonePlaces); the classes of bodies never reach them.
    void SetExtraPlaces(std::uint32_t places);

    // Installs the finalizer detour on first use (signature-checked) and arms
    // a new record for one skeleton. `skeleton` must be the object passed to
    // the finalizer (ResolvePedPoseSkeleton().skeleton).
    bool Start(
        std::uint32_t recordId,
        void* skeleton,
        std::uint32_t boneCount,
        std::uint32_t sampleHz,
        std::string& error);

    // Samples an armed record from the script thread with the same rate
    // gate, ring and copy as the detour, for skeletons the pose finalizer
    // does not visit (vehicles). Harmless when the detour also captures it:
    // the rate gate keeps one sample per interval.
    void PollFromScript(std::uint32_t recordId);
    // The same, only when the finalizer did not visit the record's skeleton
    // since the previous script tick (a dead or ragdolled ped, a corpse, stops
    // going through the finalizer; its pose keeps coming from here). Script
    // thread only.
    void PollIfStalled(std::uint32_t recordId);

    // The end-of-frame check. A replay editor usually reads a ped's bones after
    // the whole frame's animation work, where this capture copies them right
    // after the pose finalizer. Once a second per record (4 times while it
    // differs) the detour also keeps the finalizer's locals, and the next script
    // tick compares them with the same buffer as the frame left it (only when
    // the finalizer did not run in between). A record whose skeleton the engine
    // changed after the finalizer (a local rewritten, or an object written
    // directly) is sampled here, locals and objects at the end of the frame,
    // instead of in the detour, until 2 s of checks come back clean. Script
    // thread only, for peds (after PollIfStalled); at most kMaxLateRecords
    // records at a time.
    constexpr std::uint32_t kMaxLateRecords = 12;
    struct LateCheck
    {
        bool checked = false;
        bool changed = false;
        bool switchedOn = false;
        bool switchedOff = false;
        bool capped = false;
        std::uint32_t localBones = 0;
        std::uint32_t objectBones = 0;
        std::int32_t worstBone = -1;
        float maxTranslation = 0.0f;
        float maxRotation = 0.0f;
    };
    // PollIfStalled then the end-of-frame check for one record, with one
    // lookup: the same order and effects as the two calls. Script thread, for
    // peds.
    LateCheck PollAndCheckLate(std::uint32_t recordId);
    // The recording time the capture gives the pose an armed record's skeleton
    // holds now, for the fidelity judge's reads (SetContentTimes on): the later
    // of the record's last finalizer call (its output stays in the buffer until
    // the next call; the detour stamps a frame with the call) and the last
    // capture the script made of it (a vehicle's poll, a stalled skeleton's, the
    // end-of-frame capture, taken or refused by the rate gate; stamped with the
    // script's time). After every script capture of the tick. False when the
    // record is not armed or has neither. Script thread.
    bool ContentTimeMs(std::uint32_t recordId, std::uint32_t& ms);

    // Disarms one record; its track stays available.
    void Stop(std::uint32_t recordId);
    // Disarms every record and lets the worker drain the rings (bounded wait).
    void StopAll();

    // Records of the current/last recording, in creation order.
    std::vector<std::uint32_t> RecordIds();
    Stats Snapshot(std::uint32_t recordId);

    // Recorded track access for playback (valid after StopAll).
    bool TrackSpan(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs);
    bool SampleTrack(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample);
    // The record's never-animated signature (tpose_check.h), read from its
    // frame headers (no decode). False when the record has no track.
    bool ReadTrackSignature(std::uint32_t recordId, tpose::TrackSignature& out);

    // Clip file (clip_file.h): every record of the last recording that has a
    // track, in creation order (call after StopAll).
    std::vector<PoseRecordExport> ExportRecords();
    // Replay buffer: drop every track's chunks that end before timeMs (safe
    // while capturing: under the worker's lock). DropBefore also removes the
    // records the worker finished (retired, ring released, nothing held, no job;
    // their tracks sealed) whose last frame is before timeMs: every later window
    // cut drops them. TrackBytes: the bytes the tracks hold now (without the
    // lock; script thread, like every function that changes the record list).
    std::size_t DropBefore(std::uint32_t timeMs);
    std::size_t TrackBytes();
    // Drains every frame waiting in the rings now, as the worker would (script
    // thread, before a save's ExportRecords while the recording runs: with
    // PoseWorkerPollMs above 5 they wait longer).
    void FlushRings();
    // The records of the recording (g_records), those the worker still visits
    // (its drain list), those whose track is sealed, and those DropBefore
    // removed so far.
    struct RecordCounts
    {
        std::uint64_t records = 0;
        std::uint64_t drain = 0;
        std::uint64_t sealed = 0;
        std::uint64_t pruned = 0;
    };
    RecordCounts RecordCountsSnapshot();
    // RedemptionEditor.ini PoseWorkerPollMs (1 to 100, default 5), the
    // worker's wait between two passes over the rings.
    void SetWorkerPollMs(std::uint32_t ms);
    std::uint32_t WorkerPollMs();
    // Replaces the records with loaded ones: the reset of BeginRecording,
    // then one disarmed record per entry, played through TrackSpan and
    // SampleTrack like a finished recording. Script thread, nothing armed.
    // On failure the current records stay and `error` names the record.
    bool ImportRecords(const std::vector<PoseRecordExport>& records, std::string& error);

    // Playback override. The detour writes the published pose into the armed
    // skeleton's pose buffer (locals then objects) right after the engine
    // finalizer, so animation cannot overwrite it. Only skeletons of playback
    // clones may be armed here.
    // Every slot, whatever this session's PedCapacity: a clip recorded with a
    // larger one plays here too.
    constexpr std::uint32_t kMaxOverrides = kLiveTargetSlots;

    struct OverrideStats
    {
        std::uint64_t applies = 0;
        // Writes of the alternate pose (the skeleton had the other bone
        // count when the finalizer ran).
        std::uint64_t altApplies = 0;
        std::uint64_t boneCountMismatch = 0;
        std::uint64_t writeFaults = 0;
        std::uint64_t publishes = 0;
        // ApplyOverrideNow evidence: before each script-thread write, is the
        // previous write still in the skeleton?
        std::uint64_t scriptWrites = 0;
        std::uint64_t scriptSurvived = 0;
        std::uint64_t scriptOverwritten = 0;
        // Writes made inside the detour after a ped puppet's finalizer
        // (SetOverrideLate), later in the frame than the script thread.
        std::uint64_t lateWrites = 0;
        // Writes after the vehicle pre-render (the wheels).
        std::uint64_t preRenderWrites = 0;
        // Detour calls where the original finalizer was not run because the
        // puppet's pose is entirely ours (the finalizer is a pure function of its
        // inputs).
        std::uint64_t skippedOriginal = 0;
        // End-of-frame checks of the written pose (CheckOverrideWrite) and how many
        // found an engine write after it.
        std::uint64_t writeChecks = 0;
        std::uint64_t writeChanged = 0;
        // Alternate pose writes by the rebuild guard.
        std::uint64_t guardApplies = 0;
        // Writes of the published pose's first bones into a skeleton rebuilt
        // with fewer bones (SetOverridePrefixBones; the detour and the guard).
        std::uint64_t prefixApplies = 0;
        // Writes from the ped pre-render (ApplyOverrideIfStale).
        std::uint64_t staleApplies = 0;
        // Writes after the weapon system's pass (WeaponPassPose).
        std::uint64_t weaponPassWrites = 0;
    };

    // For the skeleton map of a puppet no record matches (skeleton_map.h). The
    // record's layout parents (one per bone; kRootParent / kUnresolvedParent as
    // in SkeletonLayout).
    bool TrackLayoutParents(std::uint32_t recordId, std::vector<std::int32_t>& parents);
    // The parents and ambiguous flags of the layout that plays timeMs
    // (PoseTrack::LayoutAt, skeleton_signature.h), and a key of that layout (its
    // address: the same key while the chunk's layout is the same object), for
    // the signature check at the arm. Null `parents` and `ambiguous`: the key
    // alone (no copy). `rebuilt` (PoseTrack::RebuiltBonesAt): the bones demoted
    // to object space since an earlier layout of the record.
    bool TrackLayoutSignatureAt(std::uint32_t recordId, std::uint32_t timeMs, std::vector<std::int32_t>* parents,
        std::vector<std::uint8_t>* ambiguous, std::uintptr_t& layoutKey, std::vector<std::uint8_t>* rebuilt = nullptr);
    // The puppet's engine parent table and its locals, read by the next
    // finalizer call for that skeleton (read only, guarded). Request returns
    // a handle (-1: no free slot); Poll gives Pending until the detour read
    // it, then Ready or Failed once (the slot is then free again); Cancel
    // frees a request not taken (also while the detour reads it).
    enum class SkeletonInfoState { Pending, Ready, Failed };
    struct SkeletonInfo
    {
        std::vector<std::uint16_t> parents;
        std::vector<BoneQts> locals;
        float rootScale = 0.0f;
        std::uint32_t groups = 0; // the skeleton's own group count (+0xD0)
        const char* error = "none";
    };
    int RequestSkeletonInfo(void* skeleton, std::uint32_t boneCount);
    SkeletonInfoState PollSkeletonInfo(int request, SkeletonInfo& info);
    void CancelSkeletonInfo(int request);

    bool ArmOverride(
        std::uint32_t slot,
        void* skeleton,
        std::uint32_t boneCount,
        std::string& error);
    // Copies boneCount locals and boneCount objects for the next detour calls.
    bool PublishOverride(
        std::uint32_t slot,
        const BoneQts* locals,
        const BoneQts* objects,
        std::uint32_t boneCount);
    // PublishOverride without the copy. The sample's locals and objects
    // (boneCount each) are swapped into the slot and the sample gets the slot's
    // previous buffers (their content is stale: sample again before reading
    // it). A sample of another size is copied; a smaller one is refused.
    bool PublishOverrideSample(std::uint32_t slot, PoseSample& sample, std::uint32_t boneCount);
    // Alternate pose for another bone count of the same skeleton: the engine
    // rebuilds a puppet's skeleton in place after the script tick (layer
    // change, dropped hat), so the neighbouring record's pose is kept ready
    // and the detour writes it when the skeleton's bone count matches it.
    // Rejected when boneCount equals the armed one. Cleared by ArmOverride.
    bool PublishOverrideAlternate(
        std::uint32_t slot,
        const BoneQts* locals,
        const BoneQts* objects,
        std::uint32_t boneCount);
    void ClearOverrideAlternate(std::uint32_t slot);
    // Writes the published pose now, from the script thread, for skeletons
    // the pose finalizer never visits (vehicles). Same write as the detour.
    // Before writing it records whether the previous write (script or late)
    // was still in the skeleton.
    void ApplyOverrideNow(std::uint32_t slot);
    // The slot's pose write count (finalizer, skipped original, alternate, and
    // these; mount_placement.h), and the published pose written from the ped
    // pre-render detour when that count still equals `appliesAtPublish` (the
    // finalizer did not visit the puppet since the playback tick: the frame
    // would draw an older pose, the jerk detector's pose_stale). Detour context;
    // the same guarded write under the slot lock as the finalizer detour. True
    // when written.
    std::uint64_t OverrideApplyCount(std::uint32_t slot);
    bool ApplyOverrideIfStale(std::uint32_t slot, std::uint64_t appliesAtPublish);
    // Detour-safe write of the slot's pose after the weapon system's per-frame
    // pass (weapon_observer.cpp, WeaponPassPose).
    bool ApplyOverrideAfterWeaponPass(std::uint32_t slot);
    // Detour-safe, the weapon pass's recording side: the record's skeleton
    // (locals then objects) before the pass, the same after it, and the record
    // sampled after it (capture). Buffers are the caller's (no allocation on the
    // engine thread); count is locals plus objects (2 x bones).
    bool WeaponPassPoseBefore(std::uint32_t recordId, BoneQts* before, std::size_t capacity, std::size_t& count);
    bool WeaponPassPoseAfter(std::uint32_t recordId, std::size_t beforeCount, BoneQts* after, std::size_t capacity,
        bool capture);
    // Also write this slot inside the detour, after every armed puppet's
    // finalizer, which runs later in the frame than the script thread.
    void SetOverrideLate(std::uint32_t slot, bool late);
    // Near a skeleton change (a hat knocked off, a layer change: the engine
    // rebuilds the skeleton in place with another bone count), the alternate
    // pose is also written from any skeleton's finalizer call once the armed
    // skeleton has the alternate's bone count, not only from this skeleton's own
    // call: the engine does not finalize every puppet every frame, and a rebuilt
    // skeleton nobody writes renders its bind pose (a T-pose). At most a few
    // writes per published tick.
    void SetOverrideRebuildGuard(std::uint32_t slot, bool on);
    // The armed record's body group (prefix_hold.h): while the armed skeleton,
    // rebuilt in place, has fewer bones than the slot and no more than
    // `bodyBones`, the detour (and the rebuild guard) writes the published
    // pose's first bones into it instead of nothing (a bind pose frame). 0
    // turns it off; ArmOverride and DisarmOverride reset it.
    void SetOverridePrefixBones(std::uint32_t slot, std::uint32_t bodyBones);
    // Is the pose the detour wrote still in the puppet's buffer at the end of
    // the frame? Script thread, before the tick's PublishOverride: the published
    // pose is then the one the finalizer wrote (only checked when a detour write
    // came after the last publish, and discarded when another write lands during
    // the read). A change is an engine write after ours (physics, IK, a
    // constraint, a secondary pass on a layer's bones); none, with the puppet
    // still wrong on screen, means the geometry does not follow these bones
    // (cloth, blend shapes, LOD).
    struct OverrideWriteCheck
    {
        bool checked = false;
        std::uint32_t boneCount = 0;
        WrittenPoseChange change;
    };
    OverrideWriteCheck CheckOverrideWrite(std::uint32_t slot);
    // The drawn check of a slot the script thread writes (props): the published
    // locals and the locals in the skeleton buffer at the start of the tick,
    // before the publish (only after a script write). NoteOverrideDrawn counts
    // the check in writeChecks/writeChanged.
    bool ReadOverrideDrawn(std::uint32_t slot, std::vector<BoneQts>& published, std::vector<BoneQts>& drawn);
    void NoteOverrideDrawn(std::uint32_t slot, bool changed);
    void DisarmOverride(std::uint32_t slot);
    void DisarmOverrides();
    OverrideStats OverrideSnapshot(std::uint32_t slot);

    // Module detach: disables and removes the detour, stops the worker.
    // The pose finalizer detour is shared with the Pose Editor mod through the
    // owner slot of finalizer_owner.h. Acquire claims the slot (false with
    // "pose_finalizer_held_by_PoseEditor" when the other mod holds it).
    // WarmUpFinalizer claims and installs at mod load, inside the hook batch.
    // The detour then stays: Release is a no-op, and ServiceFinalizer (every
    // tick) removes it and frees the slot when the Pose Editor asks and `busy`
    // is false, and takes it back when the slot is free again.
    enum class FinalizerEvent { None, Yielded, Reclaimed, Failed };
    bool AcquireFinalizer(std::string& error);
    bool WarmUpFinalizer(std::string& error);
    // Detour after the vehicle pre-render, which rewrites the wheel bones every
    // frame; writes the late (vehicle) overrides after it. Not shared with the
    // Pose Editor. Installed at load, in the batch.
    bool WarmUpVehiclePreRender(std::string& error);
    void ReleaseFinalizer();
    FinalizerEvent ServiceFinalizer(bool busy, std::string& error);
    // The Pose Editor asked for the slot (the replay buffer gives way).
    bool FinalizerWanted();
    void Shutdown();
}
