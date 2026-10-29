#include "pose_capture.h"
#include "mount_placement.h"
#include "pose_capture_rules.h"
#include "prefix_hold.h"
#include "pause_spans.h"
#include "pose_hash.h"
#include "profiler.h"

#include <Windows.h>
#include <MinHook.h>
#include "hook_batch.h"
#include "finalizer_owner.h"
#include "module_lifetime.h"
#include "thread_names.h"
#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace replay::pose::capture
{
    namespace
    {
        // Build 1491.50 pose finalizer. Refuse to hook if the prologue changes.
        constexpr std::uintptr_t kPoseFinalizerRva = 0x266C294;
        constexpr std::uint8_t kPoseFinalizerSignature[] = {
            0x48, 0x8B, 0xC4,
            0x48, 0x89, 0x58, 0x10,
            0x48, 0x89, 0x48, 0x08,
            0x55, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55,
            0x41, 0x56, 0x41, 0x57,
            0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00,
            0x44, 0x8B, 0xB1, 0xD0, 0x00, 0x00, 0x00,
        };

        // The vehicle pre-render base (build 1491.50). The vehicle classes'
        // pre-render first write every wheel's local from the wheel state (a
        // frozen puppet's wheel does not turn) and then reach this base, so a
        // write after it is the last one of the frame.
        constexpr std::uintptr_t kVehiclePreRenderRva = 0x15AD298;
        constexpr std::uint8_t kVehiclePreRenderSignature[] = {
            0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x8D, 0xA8, 0xB8, 0xFE, 0xFF, 0xFF,
            0x48, 0x81, 0xEC, 0x08, 0x02, 0x00, 0x00,
            0x45,
        };
        constexpr std::size_t kVehicleFragment = 0x938; // entity -> fragment (virtual +0xB8)
        constexpr std::size_t kFragmentOwner = 0x128;
        constexpr std::size_t kOwnerSkeleton = 0x160;

        // Skeleton fields used by the resolver.
        constexpr std::size_t kSkeletonPoseBuffer = 0x00;
        constexpr std::size_t kSkeletonWorldMatrix = 0x08; // parent (world) matrix pointer
        constexpr std::size_t kSkeletonBoneCount = 0x108;

        // 8 samples (266 ms at 30 Hz) of slack for the worker; 16 would double
        // the per-entity allocation at register (1.5 MB for a metaped) and a few
        // slots are all the worker ever falls behind.
        constexpr std::uint32_t kRingSlots = 8;
        constexpr std::size_t kMaxPendingFrames = 300;
        constexpr std::uint32_t kDiscoveryFrames = 6;
        // A still entity never yields six distinct poses; after this many
        // frames discovery runs with what it has and the runtime check
        // demotes any bone a later motion contradicts.
        constexpr std::size_t kDiscoveryDeadlineFrames = 30;
        // Runtime check is looser than discovery so float noise never flaps.
        constexpr float kDemoteTranslation = 0.002f;
        constexpr float kDemoteRotation = 0.004f;
        constexpr std::uint32_t kCompositionCheckEvery = 15;
        constexpr std::size_t kSpareBuffers = 4;
        // Contradicting frames kept per record for re-parenting (besides the
        // discovery samples, which stay after discovery).
        constexpr std::size_t kReparentFramesKept = 4;
        // Held frames after which a queued re-parent job is given up for an
        // immediate demotion (4 s at 30 Hz; the pending cap is 300).
        constexpr std::size_t kReparentHoldLimit = 120;
        // Worker poll period (captures do not signal it): the ini key
        // PoseWorkerPollMs (SetWorkerPollMs, 1 to 100), 5 unless set.
        std::atomic<std::uint32_t> g_workerPollMs{kWorkerPollMsDefault};

        using PoseFinalizerFn = void(__fastcall*)(std::uint8_t*, std::uint8_t*, std::uint8_t*);

        enum SlotState : LONG
        {
            kSlotFree = 0,
            kSlotWriting = 1,
            kSlotReady = 2,
            // No buffer yet; the worker allocates it (AllocateLazySlots) and frees
            // the slot. The detour and the drain never touch it.
            kSlotUnallocated = 3,
        };
        // Slots a new record gets at once; the rest come from the worker. The
        // first scan of a recording can register ~60 records in one tick, and
        // allocating every slot then costs tens of ms in fresh pages.
        constexpr std::uint32_t kEagerSlots = 2;

        struct Slot
        {
            volatile LONG state = kSlotFree;
            std::uint64_t qpc = 0;
            float world[16] = {};
            std::vector<BoneQts> pose; // boneCount locals, then boneCount objects
            // False when only the locals were copied (detour samples with the
            // engine parent table known); the worker then composes the objects
            // with that table, which is what the finalizer had just done.
            bool objectsCaptured = true;
            // The recording cut count when the sample was taken.
            std::uint32_t cutSerial = 0;
            // Evidence only: the record's finalizer call count when the copy
            // began, whether the script thread took it (poll, end-of-frame), and
            // whether the count moved during the copy.
            std::uint64_t detourStamp = 0;
            bool fromScript = false;
            bool stampRaced = false;
        };

        struct Frame
        {
            std::uint32_t timeMs = 0;
            float world[16] = {};
            std::vector<BoneQts> pose;
            bool objectsCaptured = true;
            std::uint32_t cutSerial = 0;
            // The objects were composed here from the locals with the engine
            // table (not copied from the engine); objectsPartial: only the bones
            // the encoder reads (EngineComposeBones), the others hold stale data.
            bool composedByWorker = false;
            bool objectsPartial = false;
            std::uint64_t detourStamp = 0; // see Slot
            bool fromScript = false;
            bool stampRaced = false;
        };

        // What the detour and the capture write on every finalizer call or
        // sample, on cache lines of their own. Next to id, skeleton, boneCount,
        // intervalTicks, ring and the late flags (read on every call), each of
        // these writes would take the line away from the other job threads.
        // The alignment pads (C4324, wanted here), so the warning is off for
        // these two structs only. Records come from std::make_unique: C++17
        // aligned new (/Zc:alignedNew, on with /std:c++17).
#pragma warning(push)
#pragma warning(disable : 4324)
        struct alignas(64) RecordHot
        {
            std::atomic<std::uint64_t> writeSeq{0};
            std::atomic<std::uint64_t> captured{0};
            std::atomic<std::uint64_t> droppedRingFull{0};
            std::atomic<std::uint64_t> boneCountMismatch{0};
            std::atomic<std::uint64_t> copyFaults{0};
            std::atomic<std::uint64_t> detourCalls{0};
            std::atomic<std::uint64_t> captureTicksTotal{0};
            std::atomic<std::uint64_t> captureTicksMax{0};
            std::atomic<std::uint64_t> nextDeadlineQpc{0};
            std::atomic<std::uint64_t> worldReadFaults{0};
            std::atomic<std::uint64_t> scriptPolls{0};
            // Records armed with PoseStallPollMs on: the detour's time of the
            // record's last finalizer call; the stall polls' own rate gate (the
            // detour's is nextDeadlineQpc); the ring's turn, held by the capture
            // writing it (the detour and a stall poll do not share a gate), and the
            // captures that found it taken. lastCallQpc is also kept for the
            // fidelity judge (Record::contentTimes, ContentTimeMs).
            std::atomic<std::uint64_t> lastCallQpc{0};
            std::atomic<std::uint64_t> pollDeadlineQpc{0};
            std::atomic<bool> ringTurn{false};
            std::atomic<std::uint64_t> ringTurnBusy{0};
        };
        static_assert(alignof(RecordHot) == 64 && sizeof(RecordHot) % 64 == 0,
            "the hot fields start a cache line and the next member starts another");

        // One recorded entity. The engine-thread fields are immutable or
        // atomic while the record is armed; the ring is freed by the worker
        // once the record is retired, drained and no detour is running.
        struct Record
        {
            std::uint32_t id = 0;
            std::uint8_t* skeleton = nullptr;
            std::uint32_t boneCount = 0;
            std::uint64_t intervalTicks = 0;
            // The keys as Start found them, fixed while the record lives (read by
            // the detour after the g_armed load that published the record):
            // PoseStallPollMs in QPC ticks (0: plain polls) and PoseLateBackoff.
            std::uint64_t stallPollTicks = 0;
            bool lateBackoff = false;
            // The pose fidelity judge was on at Start (SetContentTimes): the
            // detour keeps lastCallQpc and the script captures scriptReadQpc, for
            // ContentTimeMs. Fixed while the record lives, like the keys.
            bool contentTimes = false;

            // Engine-thread shared state.
            // Set only after the record left g_armed (Stop, StopAll, failed
            // Start). A record that is not armed yet is not retired: its ring
            // is about to be used.
            std::atomic<bool> retired{false};
            RecordHot hot;
            std::uint64_t scriptSeenDetourCalls = ~0ull; // script thread only
            std::uint64_t scriptReadQpc = 0; // the script's last capture of it, script thread only
            std::unique_ptr<Slot[]> ring;
            std::atomic<bool> lazySlots{false}; // kSlotUnallocated slots left
            // Engine parent table: read once from the finalizer's indices argument
            // on the engine thread, consumed by the worker after engineParentsReady.
            std::vector<std::uint16_t> engineParents;
            std::atomic<bool> engineParentsTried{false};
            std::atomic<bool> engineParentsReady{false};
            float engineRootScale = 0.0f;
            std::uint32_t engineGroups = 0;
            bool engineCompared = false; // worker: layout agreement counted

            // End-of-frame check (PollAndCheckLate). The script thread raises
            // lateWanted; the next detour call copies the locals the finalizer
            // just wrote into lateAfterFinalizer, notes its detourCalls value and
            // raises lateReady; the next script tick compares. lateCapture moves
            // the record's sampling from the detour to the script thread (end of
            // frame, with objects).
            std::atomic<bool> lateWanted{false};
            std::atomic<bool> lateReady{false};
            std::atomic<bool> lateCapture{false};
            std::uint64_t lateCopyCall = 0; // written by the detour before lateReady
            std::vector<BoneQts> lateAfterFinalizer;
            // Script thread only.
            std::uint64_t nextLateCheckQpc = 0;
            std::uint64_t lateWantedQpc = 0;
            std::uint32_t lateCleanChecks = 0;
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
            std::uint64_t lateEqualChecks = 0;
            std::uint64_t lateVerified = 0;
            std::uint64_t lateVerifyMismatch = 0;
            // Checks settled in place; stall polls taken and skipped; the
            // backoff's next clean wait, whether the first check was scheduled,
            // and the longest wait scheduled.
            std::uint64_t lateDirect = 0;
            std::uint64_t stallPolls = 0;
            std::uint64_t pollsSkippedRecent = 0;
            std::uint64_t lateCleanWaitMs = kLateCheckMs;
            bool lateFirstScheduled = false;
            std::uint64_t lateIntervalMaxMs = 0;

            // Worker state, guarded by g_mutex.
            std::uint64_t readSeq = 0;
            bool hasRebaseProbe = false;
            RebaseProbe lastRebaseProbe;
            // The cut count of the last encoded frame; a frame with another one is
            // the first after a cut (no interpolation).
            bool hasCutSerial = false;
            std::uint32_t lastCutSerial = 0;
            std::vector<Frame> pending;
            std::vector<Frame> discovery;
            // Pending size at which discovery runs even without six
            // distinct poses; pushed back after a failed attempt.
            std::size_t discoveryDeadline = kDiscoveryDeadlineFrames;
            // Discovery runs on its own thread so draining never waits for it.
            bool discoveryQueued = false;
            std::vector<Frame> discoverySamples;
            // Re-parenting state (guarded by g_mutex). The frames that contradicted
            // a parent, the bones waiting for a parent, and a serial that changes
            // with every layout change so a job computed on an older layout is
            // redone.
            std::vector<Frame> reparentFrames;
            std::vector<std::uint32_t> reparentBones;
            bool reparentQueued = false;
            // A job the discovery thread popped and has not applied yet
            // (reparentQueued is cleared at the pop). The held frames wait for its
            // layout; the worker appends them only once it is in.
            bool reparentRunning = false;
            // The worker found the record finished (RecordFinished), sealed its
            // track and took it off g_drainList, for good.
            bool finished = false;
            std::uint64_t layoutSerial = 0;
            // Pose buffers handed back by the encoder, swapped into ring
            // slots so draining a frame neither allocates nor copies.
            std::vector<std::vector<BoneQts>> spareBuffers;
            std::unique_ptr<PoseTrack> track;
            // track->Bytes(), stored under g_mutex by whoever changes the track's
            // size (AppendFrame, DropBefore, ImportRecords), read by TrackBytes
            // without the lock.
            std::atomic<std::uint64_t> trackBytes{0};
            Stats stats;
            double entriesTotal = 0.0;
            double encodeUsTotal = 0.0;
            // Whether the track's layout is the exact engine layout
            // (IsExactEngineLayout) and the bones a composed frame needs
            // (EngineComposeBones), for the layout they were computed on (track and
            // layoutSerial; the layout only changes through those).
            const PoseTrack* exactForTrack = nullptr;
            std::uint64_t exactForSerial = 0;
            bool exactEngineLayout = false;
            std::vector<std::uint32_t> composeBones;
            // The last validated frame that passed (locals then objects) and the
            // layout it passed on; the next validated frame only recomputes the
            // bones that changed (FindFailingBonesChanged).
            std::vector<BoneQts> lastPassed;
            const PoseTrack* lastPassedTrack = nullptr;
            std::uint64_t lastPassedSerial = 0;
            bool lastPassedValid = false;
            // Evidence: the finalizer call count of the frame lastPassed holds.
            std::uint64_t lastPassedStamp = 0;

            // Evidence (Stats::unwritten*): written only by the capture that won
            // the record's rate gate (one per interval), read by SnapshotLocked
            // after the record stopped. Per bone, the samples with its translation,
            // rotation or scale row unwritten (boneCount x 3).
            std::vector<std::uint32_t> unwrittenRows;
            std::uint32_t unwrittenWarmup = 0; // samples left out at the start (kUnwrittenWarmupSamples)
            std::uint32_t unwrittenStride = 0; // samples since the warm-up (kUnwrittenStride)
            std::uint64_t unwrittenSamples = 0;
            std::uint32_t unwrittenMinRows = ~0u;
            std::uint32_t unwrittenMaxRows = 0;
            std::uint64_t unwrittenPartialRows = 0;
            std::uint64_t unwrittenFaults = 0;
            std::uint64_t unwrittenScanTicks = 0;
            std::uint64_t unwrittenScanMaxTicks = 0;
            std::uint64_t unwrittenFirstQpc = 0;
            std::uint32_t unwrittenLastRows = ~0u;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> unwrittenChanges; // ms after the first sample, rows
        };
#pragma warning(pop)
        static_assert(alignof(Record) == 64, "Record::hot is on its own cache lines (item 2b)");

        // Records of the current recording; entries are only appended until
        // BeginRecording, which runs with the worker stopped and nothing armed.
        std::vector<std::unique_ptr<Record>> g_records;
        // Id to record, kept with g_records (under g_mutex), so a sample does
        // not scan the records.
        std::unordered_map<std::uint32_t, Record*> g_recordIndex;
        // The records the worker still visits (DrainRings, AllocateLazySlots),
        // in creation order, guarded by g_mutex. Start appends each new record
        // with its g_records push; the worker takes off the finished ones
        // (RecordFinished); BeginRecording clears it with the worker stopped.
        // Imported records are never in it, so in the always-on buffer the
        // records of entities that left do not stay in the worker's passes.
        std::vector<Record*> g_drainList;
        // The ids of the records DropBefore removed (guarded by g_mutex,
        // cleared by BeginRecording). Start keeps refusing them
        // ("record_exists") as it did while the record was in g_records: a
        // packed ReplayId recurs only after its 16-bit instance counter wrapped,
        // and that entity was not recorded then either. And the count, for the
        // evidence lines.
        std::unordered_set<std::uint32_t> g_prunedIds;
        std::uint64_t g_prunedRecords = 0;
        // Armed records, read by the detour. kLiveTargetSlots slots; Start
        // takes the lowest free one among the session's places
        // (kMaxLiveTargets, 64 by default).
        std::atomic<Record*> g_armed[kLiveTargetSlots];
        // Each armed record's skeleton and id, indexed like g_armed, in two
        // compact arrays for the lookups (64 pointers in 8 cache lines, 64 ids
        // in 4), so the detour does not read every armed Record (a heap line
        // each) on every finalizer call, also for skeletons that are not
        // recorded. Only hints, like the override's g_armedSkeleton: the script
        // thread writes them after the g_armed store (Start) and clears them
        // with the slot (Stop, DisarmAllRecords); every use raises
        // g_detoursInFlight when the caller does and checks the authoritative
        // g_armed[i] before it touches the record. An id of 0 is no record:
        // record ids are packed ReplayIds, whose instance is never 0, or segment
        // ids with kRecordBit set.
        std::atomic<std::uint8_t*> g_armedRecordSkeleton[kLiveTargetSlots];
        std::atomic<std::uint32_t> g_armedRecordId[kLiveTargetSlots];
        // The hint lookups scan the slots below this (capacity.h RaisedScanEnd):
        // Start raises it before it publishes a slot, DisarmAllRecords lowers it
        // to 0 once every slot is free. With the lowest free slot taken first it
        // stays at the live count, so the default capacity scans 64 slots.
        std::atomic<std::uint32_t> g_armedScanEnd{0};
        // Detours currently between reading g_armed and finishing their copy.
        std::atomic<std::uint32_t> g_detoursInFlight{0};
        // Finalizer detours from entry to return (the original and every early
        // return included), for RemoveFinalizerHook. Striped by thread like
        // DetourScope's count (module_lifetime.h); the call's stripe is also its
        // EngineTiming stripe.
        replay::lifetime::StripedCounter g_finalizerCalls;
        struct FinalizerCall
        {
            FinalizerCall() noexcept : stripe(replay::lifetime::ThreadStripe()) { g_finalizerCalls.Enter(stripe); }
            ~FinalizerCall() noexcept { g_finalizerCalls.Leave(stripe); }
            FinalizerCall(const FinalizerCall&) = delete;
            FinalizerCall& operator=(const FinalizerCall&) = delete;
            const std::uint32_t stripe;
        };
        // Fast path for the finalizer calls of every other skeleton.
        std::atomic<std::uint32_t> g_armedCount{0};
        // Records sampled at the end of the frame (lateCapture).
        std::atomic<std::uint32_t> g_lateRecords{0};
        // End-of-frame check pacing and tolerances (1 cm, 0.01 rad: the
        // finalizer's composition agrees with ours to 1e-7). kLateCheckMs and
        // kLateCheckChangedMs are in pose_capture_rules.h.
        constexpr std::uint32_t kLateCleanChecksToLeave = 8; // 2 s at 4 Hz
        constexpr float kLateTranslation = 0.01f;
        constexpr float kLateRotation = 0.01f;

        // EngineTiming counters (reset by BeginRecording). The detour's calls
        // and ticks striped by the calling thread: two relaxed adds on a line
        // that thread mostly has to itself, instead of every job thread writing
        // the same two counters on every finalizer call. EngineTimingSnapshot
        // sums the stripes.
        struct alignas(64) DetourStripe
        {
            std::atomic<std::uint64_t> calls{0};
            std::atomic<std::uint64_t> ticks{0};
            std::uint8_t padding[48] = {}; // fills the line: the alignment adds no padding
        };
        static_assert(sizeof(DetourStripe) == 64, "one cache line per stripe");
        DetourStripe g_detourStripes[replay::lifetime::kStripes];
        std::atomic<std::uint64_t> g_pressureWakes{0};
        double g_workerBusyUs = 0.0;      // guarded by g_mutex
        double g_workerDiscoveryUs = 0.0; // guarded by g_mutex
        std::uint64_t g_pendingPeak = 0;  // guarded by g_mutex

        // The worker's busy time by phase, in QPC ticks (guarded by g_mutex),
        // and its thread CPU time.
        struct WorkerPhases
        {
            std::uint64_t frames = 0;
            std::uint64_t drainTicks = 0;
            std::uint64_t composeTicks = 0;
            std::uint64_t validateTicks = 0;
            std::uint64_t encodeTicks = 0;
            std::uint64_t lockWaitTicks = 0;
            std::uint64_t lockWaitMaxTicks = 0;
        };
        WorkerPhases g_workerPhases;
        // CPU time (100 ns units) of worker threads that already exited in
        // this recording (a new worker starts at zero).
        std::atomic<std::uint64_t> g_workerCpuDone{0};
        // Script-thread waits for g_mutex.
        std::atomic<std::uint64_t> g_scriptLockWaitTicks{0};
        std::atomic<std::uint64_t> g_scriptLockWaits{0};
        std::atomic<std::uint64_t> g_scriptLockWaitMaxTicks{0};
        // The longest script-thread wait at each place it takes g_mutex
        // (ScriptLockSite), and the longest single hold of g_mutex by the
        // discovery thread.
        std::atomic<std::uint64_t> g_scriptLockSiteMaxTicks[kScriptLockSites];
        std::atomic<std::uint64_t> g_discoveryHoldMaxTicks{0};
        // Rings of records that left the capture, kept for the next record with
        // the same bone count (a town brings the same metaped sizes again and
        // again; the buffer mode starts a new recording every block), with the
        // record's end-of-frame copy (lateAfterFinalizer, 2 x bones). Allocating
        // and zeroing 8 slots of 2 x bones x 48 bytes costs Start up to ~2 ms.
        // Guarded by g_mutex; capped in bytes.
        struct PooledRing
        {
            std::uint32_t boneCount = 0;
            std::unique_ptr<Slot[]> ring;
            std::vector<BoneQts> late; // empty, or 2 x boneCount
        };
        std::vector<PooledRing> g_ringPool;
        std::size_t g_ringPoolBytes = 0;
        constexpr std::size_t kRingPoolMaxBytes = std::size_t{96} << 20;
        std::atomic<std::uint64_t> g_ringPoolHits{0};
        std::atomic<std::uint64_t> g_ringPoolMisses{0};
        std::atomic<std::uint64_t> g_startTicksTotal{0};
        std::atomic<std::uint64_t> g_startTicksMax{0};
        std::atomic<std::uint64_t> g_starts{0};
        // Pose buffers whose pages were already touched, for what Start would
        // allocate and fill on the script thread (the eager slots on a ring pool
        // miss, the end-of-frame copy without a pooled one): the first scan of a
        // fight can make ~50 registrations in one tick, ~200 KB of fresh pages
        // each, and the pool only helps from the second recording. Each has room
        // for the largest common skeleton (1050 bones) as locals and objects, and
        // size 0 (resize then clear keeps the capacity); Start resizes it to
        // 2 x bones, which writes default BoneQts on resident pages. Filled off
        // the script thread: a one-shot thread from WarmUpFinalizer at load, then
        // the worker between its passes. Guarded by g_mutex (g_poseReserveSize
        // mirrors the size for the fillers, which check it without the lock).
        constexpr std::size_t kPoseBufferReserve = 64;
        constexpr std::uint32_t kReserveBones = 1100;
        constexpr std::size_t kReserveTopUpPerPass = 8; // per worker pass, ~0.5 ms of page faults at most
        std::vector<std::vector<BoneQts>> g_poseReserve;
        std::atomic<std::size_t> g_poseReserveSize{0};
        // Buffers Start took from the reserve, and buffers it had to allocate
        // (reserve empty, or a skeleton above kReserveBones or under half of it).
        std::atomic<std::uint64_t> g_reserveHits{0};
        std::atomic<std::uint64_t> g_reserveMisses{0};

        // What a pooled ring and its end-of-frame copy hold, by capacity (a pose
        // made from a reserve buffer keeps the reserve's capacity), so
        // kRingPoolMaxBytes bounds the real memory. PoolRing and TakePooledRing
        // use this same expression; nothing changes the capacities while the ring
        // is in the pool.
        std::size_t PooledRingBytes(const Slot* ring, const std::vector<BoneQts>& late)
        {
            std::size_t elements = late.capacity();
            for (std::uint32_t index = 0; index < kRingSlots; ++index)
                elements += ring[index].pose.capacity();
            return elements * sizeof(BoneQts);
        }

        // Called with g_mutex held, with no detour able to reach the ring (all
        // slots free, record out of g_armed and none in flight). `late`, the
        // record's lateAfterFinalizer, goes with the ring when it has its full
        // size; what is not pooled stays with the caller.
        void PoolRing(std::unique_ptr<Slot[]>& ring, std::vector<BoneQts>& late, std::uint32_t boneCount)
        {
            if (!ring)
                return;
            const std::size_t poseSize = static_cast<std::size_t>(boneCount) * 2u;
            const bool withLate = late.size() == poseSize;
            const std::vector<BoneQts> noLate;
            const std::size_t bytes = PooledRingBytes(ring.get(), withLate ? late : noLate);
            bool intact = g_ringPoolBytes + bytes <= kRingPoolMaxBytes;
            for (std::uint32_t index = 0; intact && index < kRingSlots; ++index)
                intact = ring[index].state == kSlotFree && ring[index].pose.size() == poseSize;
            if (!intact)
            {
                ring.reset();
                return;
            }
            for (std::uint32_t index = 0; index < kRingSlots; ++index)
            {
                ring[index].qpc = 0;
                ring[index].objectsCaptured = true;
                ring[index].cutSerial = 0;
            }
            g_ringPoolBytes += bytes;
            PooledRing pooled{boneCount, std::move(ring), {}};
            if (withLate)
                pooled.late = std::move(late);
            g_ringPool.push_back(std::move(pooled));
        }

        // Called with g_mutex held. `late` gets the ring's end-of-frame copy
        // (empty when it came without one).
        std::unique_ptr<Slot[]> TakePooledRing(std::uint32_t boneCount, std::vector<BoneQts>& late)
        {
            for (std::size_t index = g_ringPool.size(); index-- > 0;)
            {
                if (g_ringPool[index].boneCount != boneCount)
                    continue;
                std::unique_ptr<Slot[]> ring = std::move(g_ringPool[index].ring);
                late = std::move(g_ringPool[index].late);
                g_ringPool[index] = std::move(g_ringPool.back());
                g_ringPool.pop_back();
                g_ringPoolBytes -= PooledRingBytes(ring.get(), late);
                return ring;
            }
            return nullptr;
        }

        // A reserve buffer, its pages touched: resize writes every element, clear
        // keeps the capacity.
        std::vector<BoneQts> MakeReserveBuffer()
        {
            std::vector<BoneQts> buffer;
            buffer.resize(static_cast<std::size_t>(kReserveBones) * 2u);
            buffer.clear();
            return buffer;
        }

        // Called with g_mutex held: hands a filler's buffer to the reserve;
        // false when it is full (the caller frees the buffer, outside).
        bool GiveReserveBuffer(std::vector<BoneQts>& buffer)
        {
            if (g_poseReserve.size() >= kPoseBufferReserve)
                return false;
            g_poseReserve.push_back(std::move(buffer));
            g_poseReserveSize.store(g_poseReserve.size(), std::memory_order_relaxed);
            return true;
        }

        // Called with g_mutex held: up to `wanted` reserve buffers into
        // `out`; returns how many.
        std::size_t TakeReserveBuffers(std::vector<BoneQts>* out, std::size_t wanted)
        {
            std::size_t taken = 0;
            for (; taken < wanted && !g_poseReserve.empty(); ++taken)
            {
                out[taken] = std::move(g_poseReserve.back());
                g_poseReserve.pop_back();
            }
            g_poseReserveSize.store(g_poseReserve.size(), std::memory_order_relaxed);
            return taken;
        }
        // kLateVerifyEvery (one end-of-frame check in that many also runs
        // through the full CompareLatePose) is in pose_capture_rules.h. The keys
        // PoseStallPollMs and PoseLateBackoff are copied into each record by
        // Start (PoseStallPollMs at its default, kDefaultStallPollMs, until the
        // ini is read).
        std::atomic<std::uint32_t> g_stallPollMs{kDefaultStallPollMs};
        std::atomic<bool> g_lateBackoff{false};
        // The pose fidelity judge's content times (SetContentTimes), copied into
        // each record by Start.
        std::atomic<bool> g_contentTimes{false};

        struct Override
        {
            std::atomic<std::uint8_t*> armed{nullptr};
            SRWLOCK lock = SRWLOCK_INIT; // guards pose + hasData
            std::uint32_t boneCount = 0;
            // Locals and objects in two vectors, so a published sample is swapped in
            // (PublishOverrideSample) instead of copied; the engine buffer holds them
            // one after the other.
            std::vector<BoneQts> poseLocals;
            std::vector<BoneQts> poseObjects;
            bool hasData = false;
            // Alternate pose for the skeleton's other bone count: the engine
            // rebuilds a skeleton in place (same pointer, new bone count)
            // after the script tick, so the pose of the neighbouring record
            // is kept ready and written when the bone count matches it.
            std::uint32_t altBoneCount = 0;
            std::vector<BoneQts> altPose;
            bool altHasData = false;
            std::atomic<std::uint64_t> altApplies{0};
            std::atomic<std::uint64_t> applies{0};
            std::atomic<std::uint64_t> boneCountMismatch{0};
            std::atomic<std::uint64_t> writeFaults{0};
            std::atomic<std::uint64_t> publishes{0};
            // Survival evidence for ApplyOverrideNow and late writes.
            std::atomic<std::uint64_t> lastWriteHash{0};
            std::atomic<bool> hasWrite{false};
            std::atomic<bool> late{false};
            std::atomic<std::uint64_t> lateWrites{0};
            // The late write is made once per publication (the first finalizer call
            // after it), not on every finalizer call of the frame (~60), as long as
            // the vehicle pre-render detour writes this arm after the engine's wheel
            // update; without it (preRenderSeen false) every call writes. poseHash is
            // the hash of `pose`, taken when it is published (guarded by `lock`): a
            // write copies exactly those bytes, so the detours do not read the buffer
            // back to hash it.
            std::atomic<bool> latePending{false};
            std::atomic<bool> preRenderSeen{false};
            std::uint64_t poseHash = 0;
            bool poseHashValid = false;
            // SetOverrideRebuildGuard.
            std::atomic<bool> rebuildGuard{false};
            std::atomic<std::uint32_t> guardWritesThisTick{0};
            std::atomic<std::uint64_t> guardApplies{0};
            // SetOverridePrefixBones: the body group of the armed record, and
            // the prefix writes.
            std::atomic<std::uint32_t> prefixBones{0};
            std::atomic<std::uint64_t> prefixApplies{0};
            std::atomic<std::uint64_t> preRenderWrites{0};
            std::atomic<std::uint64_t> skippedOriginal{0};
            std::atomic<std::uint64_t> staleApplies{0}; // ApplyOverrideIfStale
            std::atomic<std::uint64_t> weaponPassWrites{0}; // ApplyOverrideAfterWeaponPass
            std::uint64_t scriptWrites = 0;
            std::uint64_t scriptSurvived = 0;
            std::uint64_t scriptOverwritten = 0;
            // `applies` when the pose was last published (under the exclusive lock),
            // so CheckOverrideWrite knows the detour has written it since; and the
            // check counters.
            std::uint64_t appliesAtPublish = 0;
            std::uint64_t writeChecks = 0;
            std::uint64_t writeChanged = 0;
        };

        Override g_overrides[kMaxOverrides];
        std::atomic<std::uint32_t> g_guardedSlots{0}; // slots with rebuildGuard on
        // The late slots by bit (the detours skip every slot at once when none is
        // late), and each slot's armed skeleton in one compact array for the
        // finalizer's lookup (pointers in cache lines, not Override structs). Only
        // a hint: every write still checks `armed` under the slot lock. The late
        // mask spans several words (capture_capacity.h SlotMask), and the lookups
        // are bounded by g_overrideScanEnd (raised by ArmOverride before it
        // publishes the slot, lowered to 0 by DisarmOverrides once every slot is
        // free).
        replay::capacity::SlotMask<kMaxOverrides> g_lateMask;
        constexpr std::uint32_t kLateWords = replay::capacity::SlotMask<kMaxOverrides>::kWords;
        std::atomic<std::uint8_t*> g_armedSkeleton[kMaxOverrides];
        std::atomic<std::uint32_t> g_overrideScanEnd{0};

        // Our part of the finalizer detour (the pose write and the late writes),
        // without the engine's original.
        replay::prof::ZoneId PoseOverrideZone()
        {
            static const replay::prof::ZoneId zone = replay::prof::Register("detour.pose_override");
            return zone;
        }

        int FindArmedSlot(const std::uint8_t* skeleton)
        {
            const std::uint32_t end = g_overrideScanEnd.load(std::memory_order_acquire);
            for (std::uint32_t index = 0; index < end && index < kMaxOverrides; ++index)
            {
                if (g_armedSkeleton[index].load(std::memory_order_acquire) == skeleton)
                    return static_cast<int>(index);
            }
            return -1;
        }

        void SetLateBit(std::uint32_t slotIndex, bool late)
        {
            g_lateMask.Set(slotIndex, late); // the slot's word
        }

        // Called with the slot lock held (either mode).
        std::uint64_t PublishedPoseHash(const std::vector<BoneQts>& locals, const std::vector<BoneQts>& objects)
        {
            PoseHasher hasher;
            hasher.Add(locals.data(), locals.size() * sizeof(BoneQts));
            hasher.Add(objects.data(), objects.size() * sizeof(BoneQts));
            return hasher.Finish();
        }

        // Hash of the skeleton's current pose buffer; false on a fault.
        bool GuardedPoseHash(std::uint8_t* skeleton, std::size_t bytes, std::uint64_t& hash)
        {
            __try
            {
                std::uint8_t* poseBuffer = *reinterpret_cast<std::uint8_t**>(skeleton + kSkeletonPoseBuffer);
                if (!poseBuffer)
                    return false;
                PoseHasher hasher;
                hasher.Add(poseBuffer, bytes);
                hash = hasher.Finish();
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
        std::atomic<PoseFinalizerFn> g_original{nullptr};
        std::uint64_t g_qpcFrequency = 1;
        std::uint64_t g_startQpc = 0;
        // The game's pause menu stops the recording clock, so the pause is left
        // out of the clip. Spans are in QPC ticks; every recording time (NowMs,
        // the drained pose frames) is the time since the start minus the paused
        // ticks before it. The spans keep their running total (pause_spans.h),
        // guarded by g_pauseMutex.
        std::mutex g_pauseMutex;
        PauseSpans g_pauses;
        std::atomic<bool> g_clockPaused{false};
        // Grows at each recording cut (MarkCut).
        std::atomic<std::uint32_t> g_cutSerial{0};

        // The latest QPC a pose frame was stamped at (under g_pauseMutex). A
        // script stall excluded after the fact starts no earlier than it, so no
        // track gets a frame earlier than one it already has.
        std::uint64_t g_lastStampedQpc = 0;
        // The previous recording tick (script thread only).
        std::uint64_t g_lastTickQpc = 0;

        // Called with g_pauseMutex held. O(1) at or after the last span's start,
        // a binary search before it (pause_spans.h).
        std::uint64_t PausedTicksBeforeLocked(std::uint64_t qpc)
        {
            return g_pauses.PausedBefore(qpc);
        }

        std::uint64_t PausedTicksBefore(std::uint64_t qpc)
        {
            std::lock_guard<std::mutex> lock(g_pauseMutex);
            return PausedTicksBeforeLocked(qpc);
        }

        std::uint32_t RecordMsAtQpc(std::uint64_t qpc)
        {
            const std::uint64_t active = qpc - g_startQpc - PausedTicksBefore(qpc);
            return static_cast<std::uint32_t>(active * 1000ull / g_qpcFrequency);
        }

        // RecordMsAtQpc for a pose frame, noting its QPC.
        std::uint32_t StampFrameMs(std::uint64_t qpc)
        {
            std::lock_guard<std::mutex> lock(g_pauseMutex);
            if (qpc > g_lastStampedQpc)
                g_lastStampedQpc = qpc;
            const std::uint64_t active = qpc - g_startQpc - PausedTicksBeforeLocked(qpc);
            return static_cast<std::uint32_t>(active * 1000ull / g_qpcFrequency);
        }

        std::mutex g_mutex;
        // Plain Win32 threads. std::thread (the CRT's _beginthreadex) pins this
        // module while the thread runs, so with the always-on replay buffer a
        // Ctrl+R's FreeLibrary would never reach DllMain and the mod would never
        // unload.
        class RawThread
        {
        public:
            template <typename Fn>
            void Start(Fn fn)
            {
                m_fn = fn;
                m_handle = CreateThread(nullptr, 0, &RawThread::Entry, this, 0, nullptr);
            }
            bool joinable() const { return m_handle != nullptr; }
            HANDLE handle() const { return m_handle; } // the unload wait
            void join()
            {
                WaitForSingleObject(m_handle, INFINITE);
                CloseHandle(m_handle);
                m_handle = nullptr;
            }
            void detach()
            {
                CloseHandle(m_handle);
                m_handle = nullptr;
            }

        private:
            static DWORD WINAPI Entry(void* self)
            {
                static_cast<RawThread*>(self)->m_fn();
                return 0;
            }
            void (*m_fn)() = nullptr;
            HANDLE m_handle = nullptr;
        };
        RawThread g_worker;
        // Set by the threads as they leave (see StopForUnload).
        std::atomic<bool> g_workerExited{false};
        std::atomic<bool> g_discoveryExited{false};
        HANDLE g_wake = nullptr;
        std::atomic<bool> g_workerRunning{false};

        // Discovery thread: records whose layout search is queued (guarded
        // by g_mutex). It finishes the queue before it exits.
        RawThread g_discoveryThread;
        HANDLE g_discoveryWake = nullptr;
        std::atomic<bool> g_discoveryRunning{false};
        std::deque<Record*> g_discoveryQueue;
        std::deque<Record*> g_reparentQueue; // guarded by g_mutex

        // The one-shot thread that fills the pose buffer reserve at mod load
        // (WarmUpFinalizer), stopped on unload like the worker threads.
        RawThread g_reserveThread;
        std::atomic<bool> g_reserveRunning{false};
        std::atomic<bool> g_reserveExited{false};

        // Layouts already discovered, newest first (guarded by g_mutex, kept
        // across recordings): a crowd of the same ped model shares a handful of
        // skeletons, and the search costs about 250 ms per metaped on one thread
        // (records queued behind it would fill their pending lists and drop
        // frames). Demoted layouts are not remembered (they would push the clean
        // ones out and be reused) and a layout already present is not added
        // twice.
        struct CachedLayout
        {
            std::uint32_t boneCount = 0;
            std::shared_ptr<const SkeletonLayout> layout;
        };
        std::deque<CachedLayout> g_layoutCache;
        constexpr std::size_t kLayoutCacheSize = 24;

        // Called with g_mutex held.
        void RememberLayout(const SkeletonLayout& layout)
        {
            for (const CachedLayout& cached : g_layoutCache)
            {
                if (cached.boneCount == layout.boneCount && SameLayout(*cached.layout, layout))
                    return;
            }
            g_layoutCache.push_front({layout.boneCount, std::make_shared<const SkeletonLayout>(layout)});
            if (g_layoutCache.size() > kLayoutCacheSize)
                g_layoutCache.pop_back();
        }

        void* g_hookTarget = nullptr;
        bool g_hookInstalled = false;

        void AtomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value)
        {
            std::uint64_t current = target.load(std::memory_order_relaxed);
            while (value > current &&
                !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
            {
            }
        }

        std::uint64_t QpcNow()
        {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return static_cast<std::uint64_t>(value.QuadPart);
        }

        double TicksToUs(std::uint64_t ticks)
        {
            return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(g_qpcFrequency);
        }

        // g_mutex taken by the script thread, the wait noted, and the longest
        // wait at each place it is taken.
        class ScriptLock
        {
        public:
            explicit ScriptLock(ScriptLockSite site) : m_started(QpcNow()), m_lock(g_mutex)
            {
                const std::uint64_t waited = QpcNow() - m_started;
                g_scriptLockWaitTicks.fetch_add(waited, std::memory_order_relaxed);
                g_scriptLockWaits.fetch_add(1, std::memory_order_relaxed);
                AtomicMax(g_scriptLockWaitMaxTicks, waited);
                AtomicMax(g_scriptLockSiteMaxTicks[static_cast<std::size_t>(site)], waited);
            }
            ScriptLock(const ScriptLock&) = delete;
            ScriptLock& operator=(const ScriptLock&) = delete;

        private:
            std::uint64_t m_started;
            std::unique_lock<std::mutex> m_lock;
        };

        // g_mutex taken by the discovery thread, the hold measured (QPC from the
        // lock to the unlock; the lock is the first member, so it is taken before
        // the start is read and released after the destructor's reading).
        class DiscoveryHold
        {
        public:
            DiscoveryHold() : m_lock(g_mutex), m_started(QpcNow()) {}
            ~DiscoveryHold()
            {
                if (m_measured)
                    AtomicMax(g_discoveryHoldMaxTicks, QpcNow() - m_started);
            }
            DiscoveryHold(const DiscoveryHold&) = delete;
            DiscoveryHold& operator=(const DiscoveryHold&) = delete;
            // A hold that is not part of the recording (see DiscoveryMain).
            void Unmeasured() { m_measured = false; }

        private:
            std::unique_lock<std::mutex> m_lock;
            std::uint64_t m_started;
            bool m_measured = true;
        };

        // The current thread's CPU time, 100 ns units (0 on failure).
        std::uint64_t ThreadCpu(HANDLE thread)
        {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!thread || !GetThreadTimes(thread, &created, &exited, &kernel, &user))
                return 0;
            const auto value = [](const FILETIME& time) {
                return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
            };
            return value(kernel) + value(user);
        }

        // SEH-guarded copy; the finalizer hands us a live skeleton, but a
        // fault here must never take the game down.
        bool GuardedCopy(void* destination, const void* source, std::size_t size)
        {
            __try
            {
                std::memcpy(destination, source, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Capture copy: the ring slot is read later by the worker on another
        // core, so plain stores only pull 90 KB per sample into the engine
        // thread's cache (read for ownership, then evicted). Streaming stores
        // skip that; the source, written by the finalizer with non-temporal
        // stores, is prefetched ahead. Falls back to memcpy when either side is
        // not 16-byte aligned.
        bool GuardedStreamCopy(void* destination, const void* source, std::size_t size)
        {
            __try
            {
                if ((reinterpret_cast<std::uintptr_t>(destination) & 15u) != 0 || (size & 15u) != 0)
                {
                    std::memcpy(destination, source, size);
                    return true;
                }
                auto* out = static_cast<__m128i*>(destination);
                const auto* in = static_cast<const __m128i*>(source);
                const std::size_t blocks = size / 16u;
                std::size_t index = 0;
                for (; index + 4 <= blocks; index += 4)
                {
                    _mm_prefetch(reinterpret_cast<const char*>(in + index + 32), _MM_HINT_NTA);
                    const __m128i a = _mm_loadu_si128(in + index);
                    const __m128i b = _mm_loadu_si128(in + index + 1);
                    const __m128i c = _mm_loadu_si128(in + index + 2);
                    const __m128i d = _mm_loadu_si128(in + index + 3);
                    _mm_stream_si128(out + index, a);
                    _mm_stream_si128(out + index + 1, b);
                    _mm_stream_si128(out + index + 2, c);
                    _mm_stream_si128(out + index + 3, d);
                }
                for (; index < blocks; ++index)
                    _mm_stream_si128(out + index, _mm_loadu_si128(in + index));
                // Streaming stores are weakly ordered: publish them before
                // the slot state says the frame is ready.
                _mm_sfence();
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                _mm_sfence();
                return false;
            }
        }

        bool GuardedReadSkeleton(
            std::uint8_t* skeleton,
            std::uint8_t*& poseBuffer,
            std::uint8_t*& world,
            std::uint32_t& boneCount)
        {
            __try
            {
                std::memcpy(&poseBuffer, skeleton + kSkeletonPoseBuffer, sizeof(poseBuffer));
                std::memcpy(&world, skeleton + kSkeletonWorldMatrix, sizeof(world));
                std::memcpy(&boneCount, skeleton + kSkeletonBoneCount, sizeof(boneCount));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The end-of-frame check's in-place comparison of the engine pose buffer
        // with the detour's copy (read only). False on a fault too: the copy path
        // then reads the buffer again and decides.
        bool GuardedLateBlocksEqual(const void* engine, const void* copy, std::size_t bytes)
        {
            __try
            {
                return LateBlocksEqual(engine, copy, bytes);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The parent index of every bone from the finalizer's indices table (u16
        // at +0xC of each 16-byte record), the root scale and the group count of
        // the skeleton.
        bool GuardedReadEngineParents(
            const std::uint8_t* indices,
            const std::uint8_t* skeleton,
            std::uint32_t boneCount,
            std::uint16_t* parents,
            float& rootScale,
            std::uint32_t& groups)
        {
            __try
            {
                for (std::uint32_t bone = 0; bone < boneCount; ++bone)
                    std::memcpy(&parents[bone], indices + static_cast<std::size_t>(bone) * 16u + 0xC, sizeof(std::uint16_t));
                std::memcpy(&rootScale, skeleton + 0x110, sizeof(rootScale));
                std::memcpy(&groups, skeleton + 0xD0, sizeof(groups));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Evidence: the rows of the finalizer's source the animation left
        // unwritten, per bone. indices: 16 bytes per bone, u16 offsets of the
        // translation (+0x0), rotation (+0x2) and scale (+0x4) rows inside
        // source; a lane 0xFFFFFFFF is "not animated" and the finalizer takes the
        // bind pose's lane (pcmpeqd against all-ones at 0x266C367). Translation
        // and scale: lanes x, y, z; rotation: all four. rows gets one count per
        // unwritten row (boneCount x 3: t, r, s); unwritten, the rows unwritten
        // in this sample; partial, those with only some lanes unwritten.
        bool GuardedUnwrittenScan(const std::uint8_t* source, const std::uint8_t* indices, std::uint32_t boneCount,
            std::uint32_t* rows, std::uint32_t& unwritten, std::uint32_t& partial)
        {
            __try
            {
                const __m128i ones = _mm_set1_epi32(-1);
                std::uint32_t unwrittenRows = 0;
                std::uint32_t partialRows = 0;
                for (std::uint32_t bone = 0; bone < boneCount; ++bone)
                {
                    std::uint16_t offsets[3];
                    std::memcpy(offsets, indices + static_cast<std::size_t>(bone) * 16u, sizeof(offsets));
                    for (std::uint32_t row = 0; row < 3; ++row)
                    {
                        const __m128i lanes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + offsets[row]));
                        const int used = row == 1 ? 0xF : 0x7;
                        const int hit = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(lanes, ones))) & used;
                        if (hit == 0)
                            continue;
                        ++rows[static_cast<std::size_t>(bone) * 3u + row];
                        ++unwrittenRows;
                        if (hit != used)
                            ++partialRows;
                    }
                }
                unwritten = unwrittenRows;
                partial = partialRows;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The first samples of a record are not counted: the recording sets
        // the anim-visible byte (anim_visible.h) when the ped enters it and
        // the engine applies it a frame or two later (0x8F0740 in the next
        // pre-render pass, then the expressions' mask on the creature's next
        // update), so the first sample of a ped still carries the bind rows.
        constexpr std::uint32_t kUnwrittenWarmupSamples = 6;
        // One sample in four is scanned (7.5 Hz at the 30 Hz rate): the detector
        // needs coverage, not every sample, and a scan costs 7 to 12 us next to a
        // capture of about 11 us.
        constexpr std::uint32_t kUnwrittenStride = 4;

        // Called by the capture that won the record's rate gate, after its
        // copy (the finalizer does not write source).
        void CountUnwritten(Record& record, const std::uint8_t* source, const std::uint8_t* indices, std::uint64_t qpc)
        {
            if (!source || !indices || record.unwrittenRows.size() != static_cast<std::size_t>(record.boneCount) * 3u)
                return;
            if (record.unwrittenWarmup < kUnwrittenWarmupSamples)
            {
                ++record.unwrittenWarmup;
                return;
            }
            if (record.unwrittenStride++ % kUnwrittenStride != 0)
                return;
            const std::uint64_t started = QpcNow();
            std::uint32_t rows = 0;
            std::uint32_t partial = 0;
            if (!GuardedUnwrittenScan(source, indices, record.boneCount, record.unwrittenRows.data(), rows, partial))
            {
                ++record.unwrittenFaults;
                return;
            }
            if (record.unwrittenSamples == 0)
                record.unwrittenFirstQpc = qpc;
            ++record.unwrittenSamples;
            record.unwrittenMinRows = (std::min)(record.unwrittenMinRows, rows);
            record.unwrittenMaxRows = (std::max)(record.unwrittenMaxRows, rows);
            record.unwrittenPartialRows += partial;
            if (rows != record.unwrittenLastRows && record.unwrittenChanges.size() < 64)
            {
                const std::uint64_t ms = g_qpcFrequency ? (qpc - record.unwrittenFirstQpc) * 1000u / g_qpcFrequency : 0;
                record.unwrittenChanges.emplace_back(static_cast<std::uint32_t>(ms), rows);
            }
            record.unwrittenLastRows = rows;
            const std::uint64_t spent = QpcNow() - started;
            record.unwrittenScanTicks += spent;
            record.unwrittenScanMaxTicks = (std::max)(record.unwrittenScanMaxTicks, spent);
        }

        void ReadEngineParents(Record& record, std::uint8_t* skeleton, const std::uint8_t* indices)
        {
            // A load first, so the calls after the first one (all but one) make no
            // locked write; the exchange still lets one call through.
            if (!indices || record.engineParentsTried.load(std::memory_order_acquire) ||
                record.engineParentsTried.exchange(true, std::memory_order_acq_rel))
                return;
            if (record.engineParents.size() != record.boneCount)
                return;
            float rootScale = 0.0f;
            std::uint32_t groups = 0;
            if (GuardedReadEngineParents(indices, skeleton, record.boneCount,
                    record.engineParents.data(), rootScale, groups))
            {
                record.engineRootScale = rootScale;
                record.engineGroups = groups;
                record.engineParentsReady.store(true, std::memory_order_release);
            }
        }

        // The engine parent table and the locals of a puppet skeleton no record
        // matches, for the skeleton map. The script thread asks
        // (RequestSkeletonInfo); the next finalizer call for that skeleton reads
        // them right after the original wrote the pose, the same guarded reads as
        // a record's (ReadEngineParents, GuardedReadSkeleton), read only.
        // States: free, wanted (script raised it), busy (the detour reads), done
        // (ok or failed; the script takes it and frees the slot).
        constexpr std::uint32_t kSkeletonInfoSlots = 8;
        constexpr int kInfoFree = 0;
        constexpr int kInfoWanted = 1;
        constexpr int kInfoBusy = 2;
        constexpr int kInfoReady = 3;
        constexpr int kInfoFailed = 4;
        struct SkeletonInfoSlot
        {
            std::atomic<int> state{kInfoFree};
            std::atomic<std::uint8_t*> skeleton{nullptr};
            std::uint32_t boneCount = 0;
            std::vector<std::uint16_t> parents;
            std::vector<BoneQts> locals;
            float rootScale = 0.0f;
            std::uint32_t groups = 0;
            const char* error = "none";
        };
        SkeletonInfoSlot g_skeletonInfo[kSkeletonInfoSlots];
        std::atomic<std::uint32_t> g_skeletonInfoWanted{0};

        void ServeSkeletonInfo(std::uint8_t* skeleton, const std::uint8_t* indices)
        {
            for (SkeletonInfoSlot& slot : g_skeletonInfo)
            {
                if (slot.skeleton.load(std::memory_order_acquire) != skeleton)
                    continue;
                int expected = kInfoWanted;
                if (!slot.state.compare_exchange_strong(expected, kInfoBusy, std::memory_order_acq_rel))
                    continue;
                if (slot.skeleton.load(std::memory_order_acquire) != skeleton)
                {
                    // Freed and asked again for another skeleton since the
                    // first load: not ours.
                    slot.state.store(kInfoWanted, std::memory_order_release);
                    continue;
                }
                std::uint8_t* poseBuffer = nullptr;
                std::uint8_t* world = nullptr;
                std::uint32_t boneCount = 0;
                bool ok = false;
                if (!indices)
                    slot.error = "no_indices";
                else if (!GuardedReadSkeleton(skeleton, poseBuffer, world, boneCount) || !poseBuffer)
                    slot.error = "skeleton_read";
                else if (boneCount != slot.boneCount)
                    slot.error = "bone_count_changed";
                else if (!GuardedReadEngineParents(indices, skeleton, boneCount, slot.parents.data(), slot.rootScale, slot.groups))
                    slot.error = "parents_read";
                else if (!GuardedCopy(slot.locals.data(), poseBuffer, static_cast<std::size_t>(boneCount) * sizeof(BoneQts)))
                    slot.error = "locals_read";
                else
                    ok = true;
                g_skeletonInfoWanted.fetch_sub(1, std::memory_order_acq_rel);
                slot.state.store(ok ? kInfoReady : kInfoFailed, std::memory_order_release);
            }
        }

        // With PoseStallPollMs on, the detour (a job thread) and a stall poll
        // (the script thread) pass two different rate gates, so two captures of
        // one record can start together whenever both gates are due. A ring takes
        // one writer at a time: a second one can leave a frame behind the read
        // position, drained later out of time order. So such a record's captures
        // take the ring's turn first and read their clock inside it (slots are
        // claimed in the order of their times); a capture that finds the turn
        // taken gives this sample up, the other capture is taking one. Records
        // armed with the key at 0 never touch the turn.
        class RingTurn
        {
        public:
            explicit RingTurn(Record& record)
                : m_turn(record.stallPollTicks != 0 ? &record.hot.ringTurn : nullptr)
            {
                if (m_turn && m_turn->exchange(true, std::memory_order_acquire))
                {
                    record.hot.ringTurnBusy.fetch_add(1, std::memory_order_relaxed);
                    m_turn = nullptr;
                    m_busy = true;
                }
            }
            ~RingTurn()
            {
                if (m_turn)
                    m_turn->store(false, std::memory_order_release);
            }
            RingTurn(const RingTurn&) = delete;
            RingTurn& operator=(const RingTurn&) = delete;
            bool Busy() const { return m_busy; }

        private:
            std::atomic<bool>* m_turn;
            bool m_busy = false;
        };

        // `stallPoll` passes the stall polls' own rate gate (pollDeadlineQpc)
        // instead of the detour's (nextDeadlineQpc). `source` and `indices`, the
        // finalizer call's (detour only), for the unwritten-track census
        // (evidence).
        void CaptureFromDetour(Record& record, std::uint8_t* skeleton, bool localsOnly, bool fromScript = false,
            bool stallPoll = false, const std::uint8_t* source = nullptr, const std::uint8_t* indices = nullptr)
        {
            if (g_clockPaused.load(std::memory_order_acquire))
                return; // nothing is recorded while the game is paused
            const RingTurn turn(record); // nothing for a record armed with the key at 0
            if (turn.Busy())
                return;
            // Deadline gate: capture when within a quarter interval of the
            // next deadline, then advance it by exactly one interval. At
            // 60 fps this keeps 30 Hz instead of skipping to every third
            // frame when two frames add up to slightly less than 33.3 ms.
            const std::uint64_t started = QpcNow();
            // The time this capture by the script gives the skeleton's pose, taken or
            // refused by the gate below (ContentTimeMs). Not on the detour.
            if (fromScript && record.contentTimes)
                record.scriptReadQpc = started;
            std::atomic<std::uint64_t>& gate = stallPoll ? record.hot.pollDeadlineQpc : record.hot.nextDeadlineQpc;
            std::uint64_t deadline = gate.load(std::memory_order_relaxed);
            const std::uint64_t tolerance = record.intervalTicks / 4u;
            if (deadline != 0 && started + tolerance < deadline)
                return;
            std::uint64_t next = deadline == 0 ? started + record.intervalTicks
                                               : deadline + record.intervalTicks;
            if (next + record.intervalTicks < started)
                next = started + record.intervalTicks; // resync after a stall
            if (!gate.compare_exchange_strong(deadline, next, std::memory_order_acq_rel))
                return;

            const std::uint64_t sequence = record.hot.writeSeq.load(std::memory_order_acquire);
            Slot& slot = record.ring[sequence % kRingSlots];
            if (InterlockedCompareExchange(&slot.state, kSlotWriting, kSlotFree) != kSlotFree)
            {
                record.hot.droppedRingFull.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t boneCount = 0;
            // Evidence only: finalizer calls before the copy.
            const std::uint64_t stampBefore = record.hot.detourCalls.load(std::memory_order_acquire);
            bool ok = GuardedReadSkeleton(skeleton, poseBuffer, world, boneCount);
            if (ok && boneCount != record.boneCount)
            {
                record.hot.boneCountMismatch.fetch_add(1, std::memory_order_relaxed);
                ok = false;
            }
            else if (ok)
            {
                ok = poseBuffer &&
                    GuardedStreamCopy(slot.pose.data(), poseBuffer,
                        static_cast<std::size_t>(boneCount) * (localsOnly ? 1u : 2u) * sizeof(BoneQts));
                slot.objectsCaptured = !localsOnly;
                // Skeleton parent matrix: the entity world transform at the
                // same instant as the pose (sector-relative, see
                // DecodeWorldMatrix).
                if (!ok)
                {
                    record.hot.copyFaults.fetch_add(1, std::memory_order_relaxed);
                }
                else if (!world || !GuardedCopy(slot.world, world, sizeof(slot.world)))
                {
                    std::memset(slot.world, 0, sizeof(slot.world));
                    record.hot.worldReadFaults.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (!ok)
            {
                InterlockedExchange(&slot.state, kSlotFree);
                return;
            }

            CountUnwritten(record, source, indices, started); // evidence
            slot.qpc = started;
            slot.cutSerial = g_cutSerial.load(std::memory_order_acquire);
            slot.detourStamp = stampBefore;
            slot.fromScript = fromScript;
            slot.stampRaced = fromScript && record.hot.detourCalls.load(std::memory_order_acquire) != stampBefore;
            InterlockedExchange(&slot.state, kSlotReady);
            record.hot.writeSeq.store(sequence + 1, std::memory_order_release);
            record.hot.captured.fetch_add(1, std::memory_order_relaxed);

            const std::uint64_t elapsed = QpcNow() - started;
            record.hot.captureTicksTotal.fetch_add(elapsed, std::memory_order_relaxed);
            AtomicMax(record.hot.captureTicksMax, elapsed);
            // No wake per sample: SetEvent after every capture would wake the worker
            // ~1200 times a second at ~4 us each on the engine thread. The worker
            // polls every kWorkerPollMs (the ring holds 266 ms); a wake is only sent
            // when the slot after this one is still unread (worker late).
            if (g_wake && record.ring[(sequence + 1) % kRingSlots].state != kSlotFree)
            {
                g_pressureWakes.fetch_add(1, std::memory_order_relaxed);
                SetEvent(g_wake);
            }
        }

        // The finalizer's locals and objects for the end-of-frame check, once per
        // request (see PollAndCheckLate), so the script's check is a memcmp when
        // the frame left them as they were. Detour only.
        void CopyLateAfterFinalizer(Record& record, std::uint8_t* skeleton)
        {
            bool wanted = true;
            if (!record.lateWanted.compare_exchange_strong(wanted, false, std::memory_order_acq_rel))
                return;
            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t boneCount = 0;
            if (!GuardedReadSkeleton(skeleton, poseBuffer, world, boneCount) || !poseBuffer ||
                boneCount != record.boneCount ||
                record.lateAfterFinalizer.size() != static_cast<std::size_t>(boneCount) * 2u ||
                !GuardedCopy(record.lateAfterFinalizer.data(), poseBuffer,
                    static_cast<std::size_t>(boneCount) * 2u * sizeof(BoneQts)))
            {
                return; // the script thread asks again
            }
            record.lateCopyCall = record.hot.detourCalls.load(std::memory_order_relaxed);
            record.lateReady.store(true, std::memory_order_release);
        }

        // Runs after the engine finalizer for a playback clone skeleton. The
        // caller saw `armed == skeleton` before the lock, and the script thread
        // may have disarmed the slot in between (then deleted the clone, or armed
        // the slot for another skeleton). Writes the published pose into the
        // engine buffer (locals, then objects); false on a fault.
        bool GuardedCopyPublished(std::uint8_t* poseBuffer, const std::vector<BoneQts>& locals,
            const std::vector<BoneQts>& objects)
        {
            const std::size_t localBytes = locals.size() * sizeof(BoneQts);
            return GuardedCopy(poseBuffer, locals.data(), localBytes) &&
                GuardedCopy(poseBuffer + localBytes, objects.data(), objects.size() * sizeof(BoneQts));
        }

        // A skeleton rebuilt in place with fewer bones than the slot, within the
        // armed record's body group (prefix_hold.h). Under the slot's lock.
        bool PrefixWritable(const Override& slot, std::uint32_t boneCount)
        {
            return slot.hasData && replay::prefixhold::Writable(boneCount, slot.boneCount,
                                       slot.prefixBones.load(std::memory_order_acquire)) &&
                slot.poseLocals.size() >= boneCount && slot.poseObjects.size() >= boneCount;
        }

        // The published pose's first boneCount locals and objects, laid out as
        // the rebuilt skeleton's buffer holds them (its locals, then its
        // objects).
        bool GuardedCopyPrefix(std::uint8_t* poseBuffer, const Override& slot, std::uint32_t boneCount)
        {
            const std::size_t bytes = static_cast<std::size_t>(boneCount) * sizeof(BoneQts);
            return GuardedCopy(poseBuffer, slot.poseLocals.data(), bytes) &&
                GuardedCopy(poseBuffer + bytes, slot.poseObjects.data(), bytes);
        }

        // DisarmOverride clears `armed` before it takes the lock exclusively, so
        // `armed` checked again inside the shared section proves the skeleton is
        // the armed one for as long as the lock is held: the write and, with
        // `writeHash`, the hash of what was written are both taken there. Returns
        // false when the arm changed (nothing written); with `writeHash`, also
        // when the published pose was not the one written. The hash is the
        // published pose's (taken at publication), not a read-back of the buffer.
        bool ApplyOverrideFromDetour(Override& slot, std::uint8_t* skeleton, std::uint64_t* writeHash = nullptr)
        {
            bool wrotePose = false;
            AcquireSRWLockShared(&slot.lock);
            if (slot.armed.load(std::memory_order_acquire) != skeleton)
            {
                ReleaseSRWLockShared(&slot.lock);
                return false;
            }
            if (slot.hasData)
            {
                std::uint8_t* poseBuffer = nullptr;
                std::uint8_t* world = nullptr;
                std::uint32_t boneCount = 0;
                if (!GuardedReadSkeleton(skeleton, poseBuffer, world, boneCount) || !poseBuffer)
                {
                    slot.writeFaults.fetch_add(1, std::memory_order_relaxed);
                }
                else if (boneCount == slot.boneCount)
                {
                    // The finalizer uses non-temporal stores; order them
                    // before the override.
                    _mm_sfence();
                    // Plain stores: streaming ones are slower here, the source is hot.
                    if (GuardedCopyPublished(poseBuffer, slot.poseLocals, slot.poseObjects))
                    {
                        slot.applies.fetch_add(1, std::memory_order_relaxed);
                        wrotePose = true;
                    }
                    else
                        slot.writeFaults.fetch_add(1, std::memory_order_relaxed);
                }
                else if (slot.altHasData && boneCount == slot.altBoneCount)
                {
                    _mm_sfence();
                    if (GuardedCopy(poseBuffer, slot.altPose.data(),
                            slot.altPose.size() * sizeof(BoneQts)))
                        slot.altApplies.fetch_add(1, std::memory_order_relaxed);
                    else
                        slot.writeFaults.fetch_add(1, std::memory_order_relaxed);
                }
                else if (PrefixWritable(slot, boneCount))
                {
                    _mm_sfence();
                    if (GuardedCopyPrefix(poseBuffer, slot, boneCount))
                        slot.prefixApplies.fetch_add(1, std::memory_order_relaxed);
                    else
                        slot.writeFaults.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    slot.boneCountMismatch.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (writeHash && wrotePose)
            {
                *writeHash = slot.poseHashValid ? slot.poseHash
                    : PublishedPoseHash(slot.poseLocals, slot.poseObjects);
            }
            ReleaseSRWLockShared(&slot.lock);
            return writeHash ? wrotePose : true;
        }

        // The rebuild guard's write: the alternate pose, only when the armed
        // skeleton already has the alternate's bone count (rebuilt), under the
        // same lock and arm check as the detour's.
        constexpr std::uint32_t kGuardWritesPerTick = 4;
        void ApplyRebuildGuard(Override& slot)
        {
            std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
            if (!skeleton || slot.guardWritesThisTick.load(std::memory_order_relaxed) >= kGuardWritesPerTick)
                return;
            AcquireSRWLockShared(&slot.lock);
            if (slot.armed.load(std::memory_order_acquire) == skeleton && (slot.altHasData || slot.hasData))
            {
                std::uint8_t* poseBuffer = nullptr;
                std::uint8_t* world = nullptr;
                std::uint32_t boneCount = 0;
                if (GuardedReadSkeleton(skeleton, poseBuffer, world, boneCount) && poseBuffer &&
                    boneCount != slot.boneCount)
                {
                    if (slot.altHasData && boneCount == slot.altBoneCount)
                    {
                        _mm_sfence();
                        if (GuardedCopy(poseBuffer, slot.altPose.data(), slot.altPose.size() * sizeof(BoneQts)))
                        {
                            slot.guardApplies.fetch_add(1, std::memory_order_relaxed);
                            slot.guardWritesThisTick.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                            slot.writeFaults.fetch_add(1, std::memory_order_relaxed);
                    }
                    else if (PrefixWritable(slot, boneCount))
                    {
                        // Mid-rebuild (fewer bones, no record has them): the
                        // armed pose's first bones.
                        _mm_sfence();
                        if (GuardedCopyPrefix(poseBuffer, slot, boneCount))
                        {
                            slot.prefixApplies.fetch_add(1, std::memory_order_relaxed);
                            slot.guardWritesThisTick.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                            slot.writeFaults.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            ReleaseSRWLockShared(&slot.lock);
        }

        // The rebuild guard of every guarded slot in use (g_overrideScanEnd).
        void ApplyRebuildGuards()
        {
            const std::uint32_t end = g_overrideScanEnd.load(std::memory_order_acquire);
            for (std::uint32_t index = 0; index < end && index < kMaxOverrides; ++index)
            {
                Override& slot = g_overrides[index];
                if (slot.rebuildGuard.load(std::memory_order_acquire))
                    ApplyRebuildGuard(slot);
            }
        }

        // Late writes for skeletons the finalizer never visits. Called on every
        // finalizer call that wrote a puppet (~60 per frame in a city), so
        // nothing to do returns at once; a late slot is written once per
        // publication, or on every call while the pre-render detour has not
        // written it (a vehicle family that does not go through it).
        void ApplyLateOverrides()
        {
            std::uint64_t lateWords[kLateWords]; // the mask's words
            const bool anyLate = g_lateMask.Load(lateWords);
            if (!anyLate && g_guardedSlots.load(std::memory_order_acquire) == 0)
                return;
            REPLAY_PROFILE_ZONE("detour.late_overrides");
            if (g_guardedSlots.load(std::memory_order_acquire) != 0)
                ApplyRebuildGuards();
            for (std::uint32_t word = 0; word < kLateWords; ++word)
            {
                std::uint64_t late = lateWords[word];
                while (late)
                {
                    unsigned long bit = 0;
                    _BitScanForward64(&bit, late);
                    late &= late - 1;
                    Override& slot = g_overrides[word * 64u + bit];
                    if (!slot.late.load(std::memory_order_acquire))
                        continue;
                    std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
                    if (!skeleton)
                        continue;
                    const bool pending = slot.latePending.exchange(false, std::memory_order_acq_rel);
                    if (!pending && slot.preRenderSeen.load(std::memory_order_acquire))
                        continue;
                    std::uint64_t hash = 0;
                    if (ApplyOverrideFromDetour(slot, skeleton, &hash)) // armed checked under the lock
                    {
                        slot.lastWriteHash.store(hash, std::memory_order_release);
                        slot.hasWrite.store(true, std::memory_order_release);
                        slot.lateWrites.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        using VehiclePreRenderFn = std::uint64_t(__fastcall*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
        std::atomic<VehiclePreRenderFn> g_vehicleOriginal{nullptr};
        void* g_vehicleHookTarget = nullptr;

        bool GuardedVehicleSkeleton(const std::uint8_t* vehicle, std::uint8_t*& skeleton)
        {
            __try
            {
                const std::uint8_t* fragment = *reinterpret_cast<std::uint8_t* const*>(vehicle + kVehicleFragment);
                const std::uint8_t* owner = fragment
                    ? *reinterpret_cast<std::uint8_t* const*>(fragment + kFragmentOwner) : nullptr;
                skeleton = owner ? *reinterpret_cast<std::uint8_t* const*>(owner + kOwnerSkeleton) : nullptr;
                return skeleton != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::uint64_t __fastcall VehiclePreRenderDetour(void* vehicle, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            const VehiclePreRenderFn original = g_vehicleOriginal.load(std::memory_order_acquire);
            replay::mount::VehiclePreRender(vehicle); // a vehicle puppet back to its placement, before the base
            const std::uint64_t result = original ? original(vehicle, a2, a3, a4) : 0;
            if (!vehicle)
                return result;
            std::uint64_t lateWords[kLateWords]; // the mask's words
            if (!g_lateMask.Load(lateWords))
                return result;
            REPLAY_PROFILE_ZONE("detour.vehicle_prerender");
            std::uint8_t* skeleton = nullptr;
            if (!GuardedVehicleSkeleton(static_cast<const std::uint8_t*>(vehicle), skeleton))
                return result;
            for (std::uint32_t word = 0; word < kLateWords; ++word)
            {
                std::uint64_t late = lateWords[word];
                while (late)
                {
                    unsigned long bit = 0;
                    _BitScanForward64(&bit, late);
                    late &= late - 1;
                    Override& slot = g_overrides[word * 64u + bit];
                    if (!slot.late.load(std::memory_order_acquire) ||
                        slot.armed.load(std::memory_order_acquire) != skeleton)
                        continue;
                    std::uint64_t hash = 0;
                    if (ApplyOverrideFromDetour(slot, skeleton, &hash)) // armed checked under the lock
                    {
                        slot.lastWriteHash.store(hash, std::memory_order_release);
                        slot.hasWrite.store(true, std::memory_order_release);
                        slot.preRenderWrites.fetch_add(1, std::memory_order_relaxed);
                        slot.preRenderSeen.store(true, std::memory_order_release);
                    }
                    return result; // the first slot armed for this skeleton, as the one-word loop's break
                }
            }
            return result;
        }

        // Writes the published pose without the original finalizer having
        // run; false when the pose cannot be fully ours (no data yet, other
        // bone count), so the caller runs the original path.
        bool SkipOriginalAndApply(Override& slot, std::uint8_t* skeleton)
        {
            bool applied = false;
            AcquireSRWLockShared(&slot.lock);
            // Still armed for this skeleton (see ApplyOverrideFromDetour); disarmed
            // meanwhile: the original runs.
            if (slot.hasData && slot.armed.load(std::memory_order_acquire) == skeleton)
            {
                std::uint8_t* poseBuffer = nullptr;
                std::uint8_t* world = nullptr;
                std::uint32_t boneCount = 0;
                if (GuardedReadSkeleton(skeleton, poseBuffer, world, boneCount) && poseBuffer &&
                    boneCount == slot.boneCount &&
                    GuardedCopyPublished(poseBuffer, slot.poseLocals, slot.poseObjects))
                {
                    slot.applies.fetch_add(1, std::memory_order_relaxed);
                    slot.skippedOriginal.fetch_add(1, std::memory_order_relaxed);
                    applied = true;
                }
            }
            ReleaseSRWLockShared(&slot.lock);
            return applied;
        }

        void __fastcall PoseFinalizerDetour(
            std::uint8_t* skeleton,
            std::uint8_t* source,
            std::uint8_t* indices)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            FinalizerCall finalizerCall;
            REPLAY_PROFILE_ZONE("detour.pose_finalizer");
            const PoseFinalizerFn original = g_original.load(std::memory_order_acquire);
            // A puppet whose skeleton is fully armed (data published, same bone
            // count) gets its pose from us alone; the original finalizer only writes
            // the two pose arrays, so its output would be overwritten anyway and it
            // is not run. The compact skeleton array finds the slot.
            const int armedSlot = skeleton ? FindArmedSlot(skeleton) : -1;
            if (armedSlot >= 0)
            {
                replay::prof::Scope overrideZone(PoseOverrideZone());
                if (SkipOriginalAndApply(g_overrides[armedSlot], skeleton))
                {
                    ApplyLateOverrides();
                    return;
                }
            }
            if (original)
                original(skeleton, source, indices);

            if (!skeleton)
                return;
            // A puppet skeleton waiting for its skeleton map.
            if (g_skeletonInfoWanted.load(std::memory_order_acquire) != 0)
                ServeSkeletonInfo(skeleton, indices);
            // A record is only read after g_detoursInFlight is raised, so a
            // count of zero here never touches record memory.
            if (g_armedCount.load(std::memory_order_acquire) != 0)
            {
                const std::uint64_t started = QpcNow();
                bool captured = false;
                // The skeleton hints first, with no shared write and no Record
                // read; g_detoursInFlight goes up only on a hit, and the record
                // used is the one the authoritative g_armed[i] holds. A stale hit
                // (the slot changed after the hint was read) is skipped and the
                // scan goes on.
                bool inFlight = false;
                // The slots in use, not all the slots.
                const std::uint32_t scanEnd = g_armedScanEnd.load(std::memory_order_acquire);
                for (std::uint32_t index = 0; index < scanEnd && index < kLiveTargetSlots; ++index)
                {
                    if (g_armedRecordSkeleton[index].load(std::memory_order_acquire) != skeleton)
                        continue;
                    if (!inFlight)
                    {
                        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
                        inFlight = true;
                    }
                    Record* record = g_armed[index].load(std::memory_order_seq_cst);
                    if (!record || record->skeleton != skeleton)
                        continue;
                    record->hot.detourCalls.fetch_add(1, std::memory_order_relaxed);
                    // The call's time for the stall polls (PoseStallPollMs on) and for
                    // the fidelity judge (ContentTimeMs). The stall polls read it only
                    // when stallPollTicks != 0.
                    if (record->stallPollTicks != 0 || record->contentTimes)
                        record->hot.lastCallQpc.store(started, std::memory_order_relaxed);
                    ReadEngineParents(*record, skeleton, indices);
                    if (record->lateWanted.load(std::memory_order_acquire))
                        CopyLateAfterFinalizer(*record, skeleton);
                    // A record the engine changes after the finalizer is sampled at
                    // the end of the frame instead.
                    if (!record->lateCapture.load(std::memory_order_acquire))
                    {
                        CaptureFromDetour(*record, skeleton,
                            record->engineParentsReady.load(std::memory_order_acquire), false, false, source, indices);
                    }
                    captured = true;
                    break;
                }
                if (inFlight)
                    g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
                // The same calls and span, on this thread's stripe.
                DetourStripe& stripe = g_detourStripes[finalizerCall.stripe];
                stripe.calls.fetch_add(1, std::memory_order_relaxed);
                stripe.ticks.fetch_add(QpcNow() - started, std::memory_order_relaxed);
                if (captured)
                    return;
            }
            if (armedSlot >= 0)
            {
                replay::prof::Scope overrideZone(PoseOverrideZone());
                ApplyOverrideFromDetour(g_overrides[armedSlot], skeleton);
                ApplyLateOverrides();
                return;
            }
            // Any other skeleton's call serves the rebuild guard (the guarded
            // puppet's own finalizer may not run this frame).
            if (g_guardedSlots.load(std::memory_order_acquire) != 0)
                ApplyRebuildGuards(); // over the slots in use
        }

        // The pose finalizer is shared with the Pose Editor mod: both installed,
        // one active at a time. The owner slot is the Pose Editor's shared page
        // (finalizer_owner.h, v2). The detour stays installed while the slot is
        // held (an install or removal freezes the game for 60 to 160 ms); the
        // Pose Editor asks for it through `wanted` and ServiceFinalizer gives it
        // back when nothing is recorded or played.
        rdr2::finalizer_owner::Slot g_owner;
        bool g_ownerHeld = false;

        bool TakeFinalizer(std::string& error)
        {
            if (g_ownerHeld)
                return true;
            LONG holder = rdr2::finalizer_owner::kFree;
            if (!g_owner.TryClaim(rdr2::finalizer_owner::kReplayPoC, holder))
            {
                error = holder == rdr2::finalizer_owner::kFree
                    ? "pose_finalizer_slot_unavailable"
                    : std::string("pose_finalizer_held_by_") + rdr2::finalizer_owner::OwnerName(holder);
                return false;
            }
            g_ownerHeld = true;
            return true;
        }

        bool EnsureHook(std::string& error)
        {
            if (g_hookInstalled)
                return true;
            if (!TakeFinalizer(error))
                return false;

            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint8_t* target = base + kPoseFinalizerRva;
            std::uint8_t prologue[sizeof(kPoseFinalizerSignature)] = {};
            if (!GuardedCopy(prologue, target, sizeof(prologue)) ||
                std::memcmp(prologue, kPoseFinalizerSignature, sizeof(prologue)) != 0)
            {
                error = "pose_finalizer_signature_mismatch";
                return false;
            }

            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                error = std::string("mh_initialize_") + MH_StatusToString(init);
                return false;
            }
            void* trampoline = nullptr;
            MH_STATUS status = MH_CreateHook(
                target, reinterpret_cast<void*>(&PoseFinalizerDetour), &trampoline);
            if (status != MH_OK || !trampoline)
            {
                error = std::string("mh_create_") + MH_StatusToString(status);
                return false;
            }
            g_original.store(reinterpret_cast<PoseFinalizerFn>(trampoline), std::memory_order_release);
            status = replay::hooks::Enable(target);
            if (status != MH_OK)
            {
                MH_RemoveHook(target);
                g_original.store(nullptr, std::memory_order_release);
                error = std::string("mh_enable_") + MH_StatusToString(status);
                return false;
            }
            g_hookTarget = target;
            g_hookInstalled = true;
            return true;
        }

        bool IsDistinct(const Frame& a, const Frame& b, std::uint32_t boneCount)
        {
            std::uint32_t moved = 0;
            for (std::uint32_t bone = 0; bone < boneCount && moved < 10; ++bone)
            {
                if (AngleBetween(a.pose[bone].rotation, b.pose[bone].rotation) > 0.02f)
                    ++moved;
            }
            return moved >= 10;
        }

        // Evidence (worker, g_mutex held): the engine parent table against the
        // track's layout, and the objects composed from the captured locals with
        // the engine parents (the finalizer's own rule: ParentThenLocal with
        // inherited scale, first root scaled by the skeleton's root scale)
        // against the captured objects. The one-time part (table against layout)
        // also runs for the frames whose composition checks are skipped.
        void CompareEngineParentTable(Record& record)
        {
            Stats& stats = record.stats;
            const std::uint32_t boneCount = record.boneCount;
            const std::vector<std::uint16_t>& parents = record.engineParents;
            if (parents.size() != boneCount)
                return;
            if (!record.engineCompared)
            {
                record.engineCompared = true;
                stats.engineParents = true;
                stats.engineRootScale = record.engineRootScale;
                stats.engineGroups = record.engineGroups;
                bool orderOk = true;
                const SkeletonLayout& layout = record.track->Layout();
                for (std::uint32_t bone = 1; bone < boneCount; ++bone)
                {
                    if (parents[bone] >= bone)
                        orderOk = false;
                    const std::int32_t mine = layout.parent[bone];
                    if (mine == kUnresolvedParent)
                        ++stats.engineParentUnresolved;
                    else if (mine == static_cast<std::int32_t>(parents[bone]))
                        ++stats.engineParentAgree;
                    else
                        ++stats.engineParentDisagree;
                }
                stats.engineParentOrderOk = orderOk;
            }
        }

        void CheckEngineParents(Record& record, const BoneQts* locals, const BoneQts* objects)
        {
            Stats& stats = record.stats;
            const std::uint32_t boneCount = record.boneCount;
            const std::vector<std::uint16_t>& parents = record.engineParents;
            if (parents.size() != boneCount)
                return;
            CompareEngineParentTable(record);
            static std::vector<BoneQts> composed;
            composed.resize(boneCount);
            float maxRotation = 0.0f;
            float maxTranslation = 0.0f;
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (bone == 0)
                {
                    composed[0] = locals[0];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        composed[0].translation[axis] *= record.engineRootScale;
                        composed[0].scale[axis] *= record.engineRootScale;
                    }
                    composed[0].scale[3] *= record.engineRootScale;
                }
                else if (parents[bone] < bone)
                {
                    composed[bone] = Compose(composed[parents[bone]], locals[bone],
                        CompositionOrder::ParentThenLocal, true);
                }
                else
                {
                    composed[bone] = objects[bone]; // cannot compose: out of order
                }
                maxRotation = std::max(maxRotation, AngleBetween(composed[bone].rotation, objects[bone].rotation));
                maxTranslation = std::max(maxTranslation, TranslationDistance(composed[bone], objects[bone]));
            }
            ++stats.engineComposeChecks;
            stats.engineComposeMaxRotation = std::max(stats.engineComposeMaxRotation, maxRotation);
            stats.engineComposeMaxTranslation = std::max(stats.engineComposeMaxTranslation, maxTranslation);
        }

        // Called with g_mutex held. False when the frame contradicts the layout:
        // the bones that failed and the frame go to the discovery thread, which
        // gives them a parent that explains the discovery samples and the
        // contradicting frames, or object space, in one layout change; the caller
        // holds the frame in `pending` until then, so no frame is recorded with a
        // wrong parent and the track gets one keyframe per contradiction instead
        // of two.
        // ExactEngineLayout (g_mutex held): whether the record's current layout
        // is the exact engine layout, and the bones a composed frame needs,
        // recomputed when the layout changed.
        bool ExactEngineLayout(Record& record)
        {
            if (!record.track || !record.engineParentsReady.load(std::memory_order_acquire) ||
                record.engineParents.size() != record.boneCount)
            {
                return false;
            }
            if (record.exactForTrack != record.track.get() || record.exactForSerial != record.layoutSerial)
            {
                record.exactForTrack = record.track.get();
                record.exactForSerial = record.layoutSerial;
                const SkeletonLayout& layout = record.track->Layout();
                record.exactEngineLayout = IsExactEngineLayout(layout, record.engineParents.data(), record.boneCount,
                    record.engineRootScale);
                record.composeBones.clear();
                if (record.exactEngineLayout)
                    EngineComposeBones(layout, record.engineParents.data(), record.boneCount, record.composeBones);
            }
            return record.exactEngineLayout;
        }

        bool AppendFrame(Record& record, const Frame& frame)
        {
            PoseTrack& track = *record.track;
            // A frame the worker composed with the engine table on the exact
            // engine layout cannot fail the check below (every expected object
            // is the composed one), so it is not run; frames with objects from
            // the engine (script samples, late capture) and any other layout
            // are checked. A partial frame only exists under this same
            // condition.
            const bool skipValidation = frame.composedByWorker && ExactEngineLayout(record);
            const std::uint64_t validateStarted = QpcNow();
            std::vector<std::uint32_t> failing;
            if (skipValidation)
            {
                ++record.stats.validationSkipped;
            }
            else
            {
                ++record.stats.validatedFrames;
                const std::size_t poseSize = static_cast<std::size_t>(record.boneCount) * 2u;
                // Same layout as the last frame that passed and the same pose
                // size: only the bones that changed are recomputed (bit for bit
                // the result of the full check).
                if (record.lastPassedValid && record.lastPassedTrack == record.track.get() && record.lastPassedSerial == record.layoutSerial &&
                    record.lastPassed.size() == poseSize && frame.pose.size() >= poseSize)
                {
                    std::uint32_t checked = 0;
                    ChangedBoneCounts changedCounts;
                    failing = FindFailingBonesChanged(
                        track.Layout(),
                        frame.pose.data(),
                        frame.pose.data() + record.boneCount,
                        record.lastPassed.data(),
                        record.lastPassed.data() + record.boneCount,
                        kDemoteTranslation,
                        kDemoteRotation,
                        checked,
                        &changedCounts);
                    ++record.stats.validationIncremental;
                    record.stats.validationBonesChecked += checked;
                    // Evidence only: a script copy against the last frame that
                    // passed. Idle: no finalizer call finished between the two
                    // copies, so if the finalizer is the only writer nothing may
                    // differ.
                    if (frame.fromScript)
                    {
                        Stats& poll = record.stats;
                        const std::uint64_t calls = frame.detourStamp - record.lastPassedStamp;
                        if (frame.detourStamp == record.lastPassedStamp)
                        {
                            ++poll.pollIdleFrames;
                            poll.pollIdleLocalFrames += changedCounts.locals ? 1u : 0u;
                            poll.pollIdleObjectFrames += changedCounts.objects ? 1u : 0u;
                            poll.pollIdleRootFrames += changedCounts.root ? 1u : 0u;
                            poll.pollIdleLocalBones += changedCounts.locals;
                            poll.pollIdleObjectBones += changedCounts.objects;
                            poll.pollIdleChecked += checked;
                        }
                        else
                        {
                            ++poll.pollCallFrames;
                            poll.pollCallsSum += calls;
                            poll.pollCallSameFrames += changedCounts.locals == 0 && changedCounts.objects == 0 ? 1u : 0u;
                            poll.pollCallLocalBones += changedCounts.locals;
                            poll.pollCallObjectBones += changedCounts.objects;
                            poll.pollCallChecked += checked;
                        }
                    }
                    // lastPassed now holds this frame (only the bones that
                    // differed were written).
                    record.lastPassedValid = failing.empty();
                    record.lastPassedStamp = frame.detourStamp;
                }
                else
                {
                    failing = FindFailingBones(
                        track.Layout(),
                        frame.pose.data(),
                        frame.pose.data() + record.boneCount,
                        kDemoteTranslation,
                        kDemoteRotation);
                    record.stats.validationBonesChecked += record.boneCount;
                    record.lastPassedValid = failing.empty() && frame.pose.size() >= poseSize;
                    if (record.lastPassedValid)
                    {
                        record.lastPassed.assign(frame.pose.begin(), frame.pose.begin() + static_cast<std::ptrdiff_t>(poseSize));
                        record.lastPassedTrack = record.track.get();
                        record.lastPassedSerial = record.layoutSerial;
                        record.lastPassedStamp = frame.detourStamp; // evidence
                    }
                }
            }
            g_workerPhases.validateTicks += QpcNow() - validateStarted;
            if (!failing.empty())
            {
                if (record.stats.demotionEvents++ == 0)
                    record.stats.firstDemotionMs = frame.timeMs;
                record.stats.lastDemotionMs = frame.timeMs;
                for (const std::uint32_t bone : failing)
                {
                    if (std::find(record.reparentBones.begin(), record.reparentBones.end(), bone) ==
                        record.reparentBones.end())
                    {
                        record.reparentBones.push_back(bone);
                    }
                }
                if (record.reparentFrames.size() >= kReparentFramesKept)
                    record.reparentFrames.erase(record.reparentFrames.begin());
                record.reparentFrames.push_back(frame);
                if (!record.reparentQueued)
                {
                    record.reparentQueued = true;
                    g_reparentQueue.push_back(&record);
                    if (g_discoveryWake)
                        SetEvent(g_discoveryWake);
                }
                return false;
            }

            FrameInput input;
            input.timeMs = frame.timeMs;
            input.world = frame.world;
            input.locals = frame.pose.data();
            input.objects = frame.pose.data() + record.boneCount;

            // Rebase or teleport since the previous frame: no interpolation
            // into this one (see RebaseProbe).
            WorldTransform world{};
            if (DecodeWorldMatrix(frame.world, world))
            {
                const RebaseProbe probe = MakeRebaseProbe(world, input.objects[0], frame.timeMs);
                if (record.hasRebaseProbe && IsRebase(record.lastRebaseProbe, probe))
                {
                    input.flags |= kFrameNoInterpolation;
                    ++record.stats.rebaseFrames;
                }
                record.lastRebaseProbe = probe;
                record.hasRebaseProbe = true;
            }
            // The first frame after a recording cut is not blended with the
            // one before it (the playhead holds the earlier frame up to it, as
            // for a rebase).
            if (record.hasCutSerial && frame.cutSerial != record.lastCutSerial)
            {
                input.flags |= kFrameNoInterpolation;
                ++record.stats.cutFrames;
            }
            record.lastCutSerial = frame.cutSerial;
            record.hasCutSerial = true;

            const std::uint64_t started = QpcNow();
            const EncodeStats encoded = track.Append(input);
            const std::uint64_t encodeTicks = QpcNow() - started;
            const double us = TicksToUs(encodeTicks);
            g_workerPhases.encodeTicks += encodeTicks;
            record.trackBytes.store(track.Bytes(), std::memory_order_relaxed);

            Stats& stats = record.stats;
            stats.rawDeltaChannels += encoded.deltaChannels;
            stats.rawSameChannels += encoded.rawSameChannels;
            stats.rawSameFrames += encoded.rawSameFrame ? 1u : 0u;
            record.entriesTotal += encoded.entries;
            record.encodeUsTotal += us;
            stats.encodeMaxUs = std::max(stats.encodeMaxUs, us);
            stats.encodedFrames = track.Stats().frames;
            stats.keyframes = track.Stats().keyframes;
            stats.relativeKeyframes = track.Stats().relativeKeyframes;
            stats.fullKeyframeBytes = track.Stats().fullKeyframeBytes;
            stats.relativeKeyframeBytes = track.Stats().relativeKeyframeBytes;
            stats.trackBytes = track.Stats().bytes;
            stats.droppedChunks = track.Stats().droppedChunks;
            stats.encodeAvgUs = record.encodeUsTotal / static_cast<double>(stats.encodedFrames);
            stats.entriesPerFrameAvg = record.entriesTotal / static_cast<double>(stats.encodedFrames);
            stats.durationMs = track.Stats().lastTimeMs - track.Stats().firstTimeMs;

            if (stats.encodedFrames % kCompositionCheckEvery == 1)
            {
                const std::uint64_t checkStarted = QpcNow();
                if (skipValidation)
                {
                    // Both checks compare the frame's objects with a composition of
                    // its locals, which is how its objects were made; only the table
                    // comparison is kept.
                    CompareEngineParentTable(record);
                }
                else
                {
                    const CompositionError error = MeasureCompositionError(
                        track.Layout(), input.locals, input.objects);
                    stats.compositionMaxTranslation =
                        std::max(stats.compositionMaxTranslation, error.maxTranslation);
                    stats.compositionMaxRotation =
                        std::max(stats.compositionMaxRotation, error.maxRotation);
                    if (record.engineParentsReady.load(std::memory_order_acquire))
                        CheckEngineParents(record, input.locals, input.objects);
                }
                g_workerPhases.validateTicks += QpcNow() - checkStarted;
            }
            return true;
        }

        // Called with g_mutex held: appends the held frames in order until one
        // contradicts the layout (that one and the rest stay held, the job for it
        // is queued by AppendFrame). At most `most` of them (0: all); returns how
        // many. A discovery, a re-parent job and the safety valve append
        // kHeldAppendPerHold, and the worker the rest, a few per visit to the
        // record (DrainRings), with the record's new frames held behind them
        // (ProcessFrame).
        std::size_t AppendHeldFrames(Record& record, std::size_t most)
        {
            const std::size_t appended = AppendHeldPrefix(record.pending, most,
                [&record](const Frame& frame) { return AppendFrame(record, frame); });
            if (record.pending.empty())
                record.pending.shrink_to_fit();
            return appended;
        }

        // Called with g_mutex held where every held frame must be in the track
        // before going on: the discovery thread's exit, StopAll, ExportRecords
        // and DropBefore. Each record with its layout and no re-parent job queued
        // or running appends all its held frames (up to a contradiction, whose
        // job AppendFrame queues). True when a re-parent job is queued.
        bool FlushHeldFrames()
        {
            for (const std::unique_ptr<Record>& record : g_records)
            {
                if (HeldFramesAppendable(record->stats.layoutReady, record->reparentQueued, record->reparentRunning,
                        !record->pending.empty()))
                {
                    AppendHeldFrames(*record, 0);
                }
            }
            return !g_reparentQueue.empty();
        }

        // Called with g_mutex held. Frames before the layout is known go to
        // `pending`; once enough distinct frames exist the record is queued for
        // the discovery thread, and pending keeps filling meanwhile.
        // Objects of a locals-only frame are composed with the engine parent
        // table exactly as the finalizer does it (q = qp*ql, t = tp + rot(qp,
        // tl*sp), s = sl*sp; first root scaled by the skeleton's root scale), by
        // replay::pose::ComposeEngineObjects (pose_stream.cpp, shared with the
        // tests). With `partial`, only the bones the encoder and the rebase probe
        // read (the caller made sure the frame goes straight to AppendFrame on
        // the exact engine layout, where nothing else reads its objects).
        bool ComposeFrameObjects(Record& record, Frame& frame, bool partial)
        {
            const std::uint32_t boneCount = record.boneCount;
            const std::vector<std::uint16_t>& parents = record.engineParents;
            if (!record.engineParentsReady.load(std::memory_order_acquire) ||
                parents.size() != boneCount || frame.pose.size() < static_cast<std::size_t>(boneCount) * 2u)
            {
                return false;
            }
            const std::uint64_t started = QpcNow();
            if (partial)
            {
                ComposeEngineObjectsSubset(parents.data(), boneCount, record.engineRootScale, record.composeBones,
                    frame.pose.data(), frame.pose.data() + boneCount);
                ++record.stats.partialComposed;
            }
            else
            {
                replay::pose::ComposeEngineObjects(parents.data(), boneCount, record.engineRootScale,
                    frame.pose.data(), frame.pose.data() + boneCount);
            }
            g_workerPhases.composeTicks += QpcNow() - started;
            frame.objectsCaptured = true;
            frame.composedByWorker = true;
            frame.objectsPartial = partial;
            ++record.stats.objectsComposed;
            return true;
        }

        void ProcessFrame(Record& record, Frame&& frame)
        {
            const std::uint32_t boneCount = record.boneCount;
            Stats& stats = record.stats;
            // A locals-only frame that will be appended right away on the
            // exact engine layout (no re-parent job holding frames) gets only
            // the objects the encoder reads; AppendFrame then skips its
            // validation and cannot hold it, so no partial frame reaches
            // `pending`, discovery or re-parenting. Nor when frames are held
            // before it (DrainedFrameDirect): it is held behind them, whole.
            const bool direct = DrainedFrameDirect(record.reparentQueued, !record.pending.empty());
            const bool partial = !frame.objectsCaptured && stats.layoutReady && direct && ExactEngineLayout(record);
            if (!frame.objectsCaptured && !ComposeFrameObjects(record, frame, partial))
            {
                ++stats.framesWithoutObjects;
                return;
            }
            if (stats.layoutReady)
            {
                if (direct && AppendFrame(record, frame))
                {
                    if (record.spareBuffers.size() < kSpareBuffers)
                        record.spareBuffers.push_back(std::move(frame.pose));
                    return;
                }
                // Unreachable for a partial frame (see above); complete it
                // anyway before it is held, discovery and re-parenting read
                // every object.
                if (frame.objectsPartial)
                {
                    replay::pose::ComposeEngineObjects(record.engineParents.data(), boneCount, record.engineRootScale,
                        frame.pose.data(), frame.pose.data() + boneCount);
                    frame.objectsPartial = false;
                }
                // Held until the re-parent job applies its layout (or until the
                // frames held before it are in).
                if (record.pending.size() < kMaxPendingFrames)
                    record.pending.push_back(std::move(frame));
                else
                    ++stats.pendingDropped;
                g_pendingPeak = std::max<std::uint64_t>(g_pendingPeak, record.pending.size());
                // Safety valve: a job still queued after this many held frames
                // gets an immediate demotion instead, so a busy discovery thread
                // never costs frames. The queued job then finds no bones and does
                // nothing.
                if (record.reparentQueued && record.pending.size() >= kReparentHoldLimit &&
                    !record.reparentBones.empty())
                {
                    SkeletonLayout updated = record.track->Layout();
                    DemoteBones(updated, record.reparentBones);
                    record.track->UpdateLayout(updated);
                    ++record.layoutSerial;
                    stats.demotedBones += record.reparentBones.size();
                    ++stats.reparentTimeouts;
                    stats.discovery.unresolved =
                        static_cast<std::uint32_t>(updated.unresolvedBones.size());
                    record.reparentBones.clear();
                    record.reparentQueued = false; // the queue entry is skipped when popped
                    AppendHeldFrames(record, kHeldAppendPerHold); // the worker appends the rest
                }
                return;
            }

            if (!record.discoveryQueued &&
                (record.discovery.empty() || IsDistinct(record.discovery.back(), frame, boneCount)))
            {
                record.discovery.push_back(frame);
            }
            if (record.pending.size() < kMaxPendingFrames)
                record.pending.push_back(std::move(frame));
            else
                ++stats.pendingDropped;

            g_pendingPeak = std::max<std::uint64_t>(g_pendingPeak, record.pending.size());
            if (record.discoveryQueued || record.pending.empty())
                return;
            const bool deadline = record.pending.size() >= record.discoveryDeadline;
            if (record.discovery.size() < kDiscoveryFrames && !deadline)
                return;

            record.discoverySamples.clear();
            record.discoverySamples.swap(record.discovery);
            if (record.discoverySamples.size() < 2)
                record.discoverySamples.push_back(record.pending.back()); // still entity
            record.discoveryQueued = true;
            g_discoveryQueue.push_back(&record);
            if (g_discoveryWake)
                SetEvent(g_discoveryWake);
        }

        void DiscoveryMain()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                Record* record = nullptr;
                std::vector<Frame> samples;
                std::vector<std::shared_ptr<const SkeletonLayout>> known;
                Record* reparent = nullptr;
                std::vector<Frame> reparentFrames;
                std::vector<std::uint32_t> reparentBones;
                SkeletonLayout reparentLayout;
                std::uint64_t reparentSerial = 0;
                {
                    DiscoveryHold hold;
                    // Re-parent jobs first: they take tens of milliseconds and their
                    // record holds frames meanwhile; a discovery takes hundreds.
                    if (!g_reparentQueue.empty())
                    {
                        reparent = g_reparentQueue.front();
                        g_reparentQueue.pop_front();
                        reparent->reparentQueued = false;
                        reparentBones.swap(reparent->reparentBones);
                        reparent->reparentBones.clear();
                        if (reparent->track && !reparentBones.empty())
                        {
                            reparentLayout = reparent->track->Layout();
                            reparentSerial = reparent->layoutSerial;
                            reparentFrames = reparent->discoverySamples;
                            for (const Frame& kept : reparent->reparentFrames)
                                reparentFrames.push_back(kept);
                            reparent->reparentRunning = true; // until it is applied
                        }
                        else
                        {
                            reparent = nullptr; // emergency demotion already applied
                        }
                    }
                    else if (!g_discoveryQueue.empty())
                    {
                        record = g_discoveryQueue.front();
                        g_discoveryQueue.pop_front();
                        samples.swap(record->discoverySamples);
                        for (const CachedLayout& cached : g_layoutCache)
                        {
                            if (cached.boneCount == record->boneCount)
                                known.push_back(cached.layout);
                        }
                    }
                    else if (!g_discoveryRunning.load(std::memory_order_acquire))
                    {
                        // The jobs leave held frames for the worker, which is
                        // stopped by now (BeginRecording, StopAll and the unload
                        // stop it first). They all go in before the thread
                        // leaves, and a contradiction among them queues its job,
                        // which this loop then runs. Not a recording hold: the
                        // script thread waits for this thread, not for the lock.
                        hold.Unmeasured();
                        if (FlushHeldFrames())
                            continue;
                        return;
                    }
                }
                if (reparent)
                {
                    std::vector<DiscoveryFrame> views;
                    for (const Frame& sample : reparentFrames)
                        views.push_back({sample.pose.data(), sample.pose.data() + reparent->boneCount});
                    const std::uint64_t started = QpcNow();
                    std::uint32_t count = 0;
                    if (!views.empty() && reparentLayout.boneCount == reparent->boneCount)
                    {
                        DemoteBones(reparentLayout, reparentBones);
                        count = ReparentBones(reparentLayout, reparentBones,
                            views.data(), static_cast<std::uint32_t>(views.size()), {});
                    }
                    const double us = TicksToUs(QpcNow() - started);
                    DiscoveryHold hold;
                    g_workerDiscoveryUs += us;
                    reparent->reparentRunning = false; // applied in this hold
                    ++reparent->stats.reparentJobs;
                    if (!reparent->track)
                        continue;
                    // Appends were held while the job was queued and while it ran
                    // (its frames held behind the job's, see DrainedFrameDirect), so
                    // the layout cannot have changed; a mismatch is a bug guard.
                    if (reparent->layoutSerial == reparentSerial && !views.empty())
                    {
                        reparent->track->UpdateLayout(reparentLayout);
                        ++reparent->layoutSerial;
                        if (count)
                            RememberLayout(reparentLayout);
                        reparent->stats.reparentedBones += count;
                        reparent->stats.demotedBones += reparentBones.size() - count;
                        reparent->stats.discovery.unresolved =
                            static_cast<std::uint32_t>(reparentLayout.unresolvedBones.size());
                    }
                    // The held frames, up to the next contradiction (which queues
                    // the next job): the first few; the worker appends the rest.
                    AppendHeldFrames(*reparent, kHeldAppendPerHold);
                    continue;
                }
                if (!record)
                {
                    WaitForSingleObject(g_discoveryWake, 50);
                    continue;
                }

                const std::uint32_t boneCount = record->boneCount;
                std::vector<DiscoveryFrame> views;
                for (const Frame& sample : samples)
                    views.push_back({sample.pose.data(), sample.pose.data() + boneCount});
                SkeletonLayout layout;
                DiscoveryStats discovery;
                const std::uint64_t started = QpcNow();
                bool reused = false;
                bool engineLayout = false;
                // The engine's own parent table, checked against the discovery
                // frames; no search when it explains them.
                if (record->engineParentsReady.load(std::memory_order_acquire) &&
                    record->engineParents.size() == boneCount)
                {
                    const bool rootScaled = std::fabs(record->engineRootScale - 1.0f) > 1e-6f;
                    if (BuildLayoutFromParents(record->engineParents.data(), boneCount, rootScaled, {}, layout, discovery) &&
                        LayoutExplainsFrames(layout, views.data(), static_cast<std::uint32_t>(views.size()), boneCount, {}))
                    {
                        engineLayout = true;
                    }
                    else
                    {
                        ++record->stats.engineLayoutRejected;
                    }
                }
                std::vector<const SkeletonLayout*> candidates;
                for (const std::shared_ptr<const SkeletonLayout>& candidate : known)
                    candidates.push_back(candidate.get());
                const std::size_t picked = engineLayout ? candidates.size() :
                    PickReusableLayout(candidates.data(), candidates.size(),
                        views.data(), static_cast<std::uint32_t>(views.size()), boneCount, {});
                if (!engineLayout && picked < candidates.size())
                {
                    layout = *candidates[picked];
                    discovery = DescribeLayout(layout);
                    reused = true;
                }
                const bool ok = engineLayout || reused || DiscoverSkeletonLayout(
                    views.data(),
                    static_cast<std::uint32_t>(views.size()),
                    boneCount,
                    {},
                    layout,
                    discovery);
                const double discoveryMs = TicksToUs(QpcNow() - started) / 1000.0;

                DiscoveryHold hold;
                g_workerDiscoveryUs += discoveryMs * 1000.0;
                Stats& stats = record->stats;
                stats.discovery = discovery;
                stats.discoveryMs = discoveryMs;
                record->discoveryQueued = false;
                if (!ok)
                {
                    // Retry one deadline later; a full pending list drops its
                    // oldest frames instead of growing.
                    if (record->pending.size() >= kMaxPendingFrames)
                        record->pending.erase(record->pending.begin(),
                            record->pending.begin() + kDiscoveryDeadlineFrames);
                    record->discoveryDeadline = record->pending.size() + kDiscoveryDeadlineFrames;
                    continue;
                }

                if (!reused && !engineLayout)
                    RememberLayout(layout);
                stats.layoutReused = reused;
                stats.engineLayout = engineLayout;
                stats.layoutCandidates = static_cast<std::uint32_t>(candidates.size());
                record->track = std::make_unique<PoseTrack>();
                {
                    // The recorder's budgets trim the tracks, not each track's
                    // own (pose_capture_rules.h kCaptureTrackByteBudget, QA 224).
                    StreamConfig config;
                    config.byteBudget = kCaptureTrackByteBudget;
                    record->track->Begin(layout, config);
                }
                stats.layoutReady = true;
                // The samples stay with the record for re-parenting.
                record->discoverySamples.swap(samples);
                AppendHeldFrames(*record, kHeldAppendPerHold); // the worker appends the rest
            }
        }

        // Called with g_mutex held.
        bool DrainOne(Record& record)
        {
            if (!record.ring)
                return false;
            Slot& slot = record.ring[record.readSeq % kRingSlots];
            // An acquire load of the state instead of a locked compare-exchange
            // that never changes it: the same value, and the capture's writes
            // before its InterlockedExchange to ready are visible after it.
            if (ReadAcquire(&slot.state) != kSlotReady)
                return false;

            const std::uint64_t drainStarted = QpcNow();
            Frame frame;
            frame.timeMs = StampFrameMs(slot.qpc);
            frame.cutSerial = slot.cutSerial;
            std::memcpy(frame.world, slot.world, sizeof(frame.world));
            frame.objectsCaptured = slot.objectsCaptured;
            frame.detourStamp = slot.detourStamp; // evidence
            frame.fromScript = slot.fromScript;
            frame.stampRaced = slot.stampRaced;
            record.stats.pollRaced += slot.stampRaced ? 1u : 0u;
            // The slot is the worker's until it is marked free: its buffer
            // moves into the frame and a spare one of the same size takes
            // its place.
            const std::size_t poseSize = static_cast<std::size_t>(record.boneCount) * 2u;
            std::vector<BoneQts> replacement;
            if (!record.spareBuffers.empty())
            {
                replacement = std::move(record.spareBuffers.back());
                record.spareBuffers.pop_back();
            }
            if (replacement.size() != poseSize)
                replacement.assign(poseSize, BoneQts{});
            frame.pose.swap(slot.pose);
            slot.pose.swap(replacement);
            InterlockedExchange(&slot.state, kSlotFree);
            ++record.readSeq;
            g_workerPhases.drainTicks += QpcNow() - drainStarted;
            ++g_workerPhases.frames;
            ProcessFrame(record, std::move(frame));
            return true;
        }

        // Called with g_mutex held. A disarmed record's ring is freed once it
        // is drained and no detour is between reading g_armed and finishing
        // a copy: any later detour can no longer see the record.
        void ReleaseRetiredRing(Record& record)
        {
            if (!record.ring || !record.retired.load(std::memory_order_seq_cst))
                return;
            // No detour in flight after the record left g_armed: nothing can
            // write its ring any more, so the slot states below are final.
            if (g_detoursInFlight.load(std::memory_order_seq_cst) != 0)
                return;
            for (std::uint32_t index = 0; index < kRingSlots; ++index)
            {
                if (record.ring[index].state != kSlotFree && record.ring[index].state != kSlotUnallocated)
                    return;
            }
            // Not while the worker still appends the frames a job left held
            // (they must be in the track before the ring can go, and a
            // contradiction among them needs the samples cleared below); the next
            // visit after the last one releases it.
            if (HeldFramesAppendable(record.stats.layoutReady, record.reparentQueued, record.reparentRunning,
                    !record.pending.empty()))
            {
                return;
            }
            // The end-of-frame copy goes to the pool with the ring.
            PoolRing(record.ring, record.lateAfterFinalizer, record.boneCount);
            record.spareBuffers.clear();
            record.spareBuffers.shrink_to_fit();
            record.lateAfterFinalizer.clear(); // no detour can reach it any more (what the pool did not take)
            record.lateAfterFinalizer.shrink_to_fit();
            if (!record.reparentQueued)
            {
                record.discoverySamples.clear();
                record.discoverySamples.shrink_to_fit();
                record.reparentFrames.clear();
                record.reparentFrames.shrink_to_fit();
                record.lastPassed.clear();
                record.lastPassed.shrink_to_fit();
                record.lastPassedValid = false;
            }
        }

        // Called with g_mutex held after a visit that drained nothing (the
        // ring's release was tried). A finished record gets its track sealed
        // and true: the caller takes it off g_drainList.
        bool FinishIfDone(Record& record)
        {
            // Frames held before a layout that can no longer come
            // (HeldFramesStranded) would keep the record on the list for good;
            // they go, and the record finishes below.
            if (HeldFramesStranded(record.retired.load(std::memory_order_seq_cst), !record.ring,
                    record.stats.layoutReady, record.discoveryQueued))
            {
                record.pending.clear();
                record.pending.shrink_to_fit();
                record.discovery.clear();
                record.discovery.shrink_to_fit();
                record.discoverySamples.clear();
                record.discoverySamples.shrink_to_fit();
            }
            if (!RecordFinished(record.retired.load(std::memory_order_seq_cst), !record.ring, !record.pending.empty(),
                    record.discoveryQueued, record.reparentQueued, record.reparentRunning))
            {
                return false;
            }
            if (record.track)
                record.track->Seal();
            record.finished = true;
            return true;
        }

        // Called with g_mutex held. Every slot of the record that is ready, in
        // ring order (at most one ring's worth: a slot the detour fills meanwhile
        // waits for the next pass). Returns how many.
        std::uint32_t DrainReady(Record& record)
        {
            std::uint32_t drained = 0;
            while (drained < kRingSlots && DrainOne(record))
                ++drained;
            return drained;
        }

        // Over g_drainList, not every record of the recording: a finished record
        // (retired, ring released, nothing held, no job) leaves it, so the records
        // of the entities that left the buffer cost nothing for the rest of the
        // session. The visits keep creation order. One record per g_mutex hold;
        // the list only grows at its end between two holds (Start), and only this
        // loop takes records off it, so the index stays right. A visit drains all
        // the record's ready slots, in order.
        void DrainRings()
        {
            bool progressed = true;
            while (progressed)
            {
                progressed = false;
                for (std::size_t index = 0;;)
                {
                    const std::uint64_t waitStarted = QpcNow();
                    std::unique_lock<std::mutex> lock(g_mutex);
                    const std::uint64_t started = QpcNow();
                    const std::uint64_t waited = started - waitStarted;
                    g_workerPhases.lockWaitTicks += waited;
                    g_workerPhases.lockWaitMaxTicks = (std::max)(g_workerPhases.lockWaitMaxTicks, waited);
                    if (index >= g_drainList.size())
                        break;
                    Record& record = *g_drainList[index];
                    // The frames a discovery or a job left held, a few per hold,
                    // before the record's new frames (which wait behind them,
                    // ProcessFrame).
                    if (HeldFramesAppendable(record.stats.layoutReady, record.reparentQueued, record.reparentRunning,
                            !record.pending.empty()) &&
                        AppendHeldFrames(record, kHeldAppendPerHold) != 0)
                    {
                        progressed = true;
                    }
                    bool finished = false;
                    if (DrainReady(record) != 0)
                    {
                        progressed = true;
                    }
                    else
                    {
                        ReleaseRetiredRing(record);
                        finished = FinishIfDone(record);
                    }
                    if (finished)
                        g_drainList.erase(g_drainList.begin() + static_cast<std::ptrdiff_t>(index));
                    else
                        ++index;
                    g_workerBusyUs += TicksToUs(QpcNow() - started);
                }
            }
        }

        // The worker gives new records the slots Start left unallocated. Only
        // this thread touches an unallocated slot (the detour and the drain skip
        // any slot that is not free or ready), and a ring only goes away on this
        // thread (ReleaseRetiredRing) or with the worker stopped
        // (BeginRecording), so the buffers are made without g_mutex.
        void AllocateLazySlots()
        {
            static std::vector<Record*> waiting;
            waiting.clear();
            {
                // The drain list holds every record that still has a ring.
                std::lock_guard<std::mutex> lock(g_mutex);
                for (Record* record : g_drainList)
                    if (record->ring && record->lazySlots.load(std::memory_order_relaxed))
                        waiting.push_back(record);
            }
            for (Record* record : waiting)
            {
                for (std::uint32_t index = 0; index < kRingSlots; ++index)
                {
                    Slot& slot = record->ring[index];
                    if (slot.state != kSlotUnallocated)
                        continue;
                    slot.pose.assign(static_cast<std::size_t>(record->boneCount) * 2u, BoneQts{});
                    InterlockedExchange(&slot.state, kSlotFree);
                }
                record->lazySlots.store(false, std::memory_order_relaxed);
            }
        }

        // Up to `most` buffers into the reserve while it has room and `running`
        // holds, each made and touched outside g_mutex and handed over under it.
        void TopUpReserve(std::size_t most, const std::atomic<bool>& running)
        {
            for (std::size_t made = 0; made < most && running.load(std::memory_order_acquire) &&
                 g_poseReserveSize.load(std::memory_order_relaxed) < kPoseBufferReserve;
                 ++made)
            {
                std::vector<BoneQts> buffer = MakeReserveBuffer();
                bool kept = false;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    kept = GiveReserveBuffer(buffer);
                }
                if (!kept)
                    break; // another filler got there first; freed here, outside the lock
            }
        }

        // The one-shot load-time fill (WarmUpFinalizer). The unload lowers
        // g_reserveRunning; it then stops between buffers.
        void ReserveMain()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            TopUpReserve(kPoseBufferReserve, g_reserveRunning);
        }

        void StartReserveFill()
        {
            if (g_reserveThread.joinable())
                return; // once per load (the handle stays until the unload)
            g_reserveRunning.store(true, std::memory_order_release);
            g_reserveExited.store(false);
            g_reserveThread.Start([] {
                {
                    replay::threads::Scope name("pose_reserve"); // closed before the exit flag
                    ReserveMain();
                }
                g_reserveExited.store(true, std::memory_order_release);
            });
        }

        void WorkerMain()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (g_workerRunning.load(std::memory_order_acquire))
            {
                WaitForSingleObject(g_wake, g_workerPollMs.load(std::memory_order_relaxed));
                AllocateLazySlots();
                DrainRings();
                // What Start took from the reserve, a few buffers per pass, after
                // the rings.
                TopUpReserve(kReserveTopUpPerPass, g_workerRunning);
            }
            // This last drain also appends every held frame that can go in
            // (DrainRings goes on while a pass appends).
            DrainRings();
            g_workerCpuDone.fetch_add(ThreadCpu(GetCurrentThread()), std::memory_order_relaxed);
        }

        void StartWorker()
        {
            if (!g_wake)
                g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_discoveryWake)
                g_discoveryWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_worker.joinable())
            {
                g_workerRunning.store(true, std::memory_order_release);
                g_workerExited.store(false);
                g_worker.Start([] {
                    {
                        replay::threads::Scope name("pose_worker");
                        WorkerMain();
                    }
                    g_workerExited.store(true, std::memory_order_release);
                });
            }
            if (!g_discoveryThread.joinable())
            {
                g_discoveryRunning.store(true, std::memory_order_release);
                g_discoveryExited.store(false);
                g_discoveryThread.Start([] {
                    {
                        replay::threads::Scope name("pose_discovery");
                        DiscoveryMain();
                    }
                    g_discoveryExited.store(true, std::memory_order_release);
                });
            }
        }

        // Finishes every queued discovery, then exits.
        void StopDiscovery()
        {
            if (!g_discoveryThread.joinable())
                return;
            g_discoveryRunning.store(false, std::memory_order_release);
            SetEvent(g_discoveryWake);
            g_discoveryThread.join();
        }

        void StopWorker()
        {
            if (!g_worker.joinable())
                return;
            g_workerRunning.store(false, std::memory_order_release);
            SetEvent(g_wake);
            g_worker.join();
        }

        // Module unload: DllMain holds the loader lock and a thread's exit needs
        // it, so join() there deadlocks (the always-on replay buffer keeps these
        // threads alive). The threads say when they left their loop; they are
        // then detached and end in ExitThread once DllMain returns. The flag is
        // the thread's last statement and the wait also needs its instruction
        // pointer out of this image (module_lifetime.h).
        void StopForUnload(RawThread& thread, std::atomic<bool>& running, HANDLE wake, std::atomic<bool>& exited,
            const char* what)
        {
            if (!thread.joinable())
                return;
            running.store(false, std::memory_order_release);
            if (wake)
                SetEvent(wake);
            replay::lifetime::StopWait(thread.handle(), exited, 3000, what);
            thread.detach();
        }

        // The hints of one g_armed slot, cleared right after the slot (Stop,
        // DisarmAllRecords): an armed slot always has its hints, except in Start
        // between the slot's store and theirs.
        void ClearArmedHints(std::size_t index)
        {
            g_armedRecordSkeleton[index].store(nullptr, std::memory_order_release);
            g_armedRecordId[index].store(0, std::memory_order_release);
        }

        void DisarmAllRecords()
        {
            for (std::atomic<Record*>& armed : g_armed)
            {
                Record* record = armed.exchange(nullptr, std::memory_order_seq_cst);
                ClearArmedHints(static_cast<std::size_t>(&armed - g_armed));
                if (record)
                {
                    record->retired.store(true, std::memory_order_seq_cst);
                    g_armedCount.fetch_sub(1, std::memory_order_acq_rel);
                    if (record->lateCapture.exchange(false, std::memory_order_acq_rel))
                        g_lateRecords.fetch_sub(1, std::memory_order_acq_rel);
                }
            }
            // Every slot is free, the lookups scan nothing until the next
            // Start raises it.
            g_armedScanEnd.store(0, std::memory_order_seq_cst);
        }

        // Called with g_mutex held.
        Record* FindRecord(std::uint32_t recordId)
        {
            const auto found = g_recordIndex.find(recordId);
            return found != g_recordIndex.end() ? found->second : nullptr;
        }

        // Called with g_mutex held, after g_records changed as a whole. The first
        // record of an id wins.
        void RebuildRecordIndex()
        {
            g_recordIndex.clear();
            g_recordIndex.reserve(g_records.size());
            for (const std::unique_ptr<Record>& record : g_records)
                g_recordIndex.emplace(record->id, record.get());
        }

        Stats SnapshotLocked(const Record& record)
        {
            Stats stats = record.stats;
            stats.recordId = record.id;
            stats.boneCount = record.boneCount;
            stats.captured = record.hot.captured.load();
            stats.droppedRingFull = record.hot.droppedRingFull.load();
            stats.boneCountMismatch = record.hot.boneCountMismatch.load();
            stats.copyFaults = record.hot.copyFaults.load();
            stats.detourCalls = record.hot.detourCalls.load();
            stats.worldReadFaults = record.hot.worldReadFaults.load();
            stats.scriptPolls = record.hot.scriptPolls.load();
            stats.lateChecks = record.lateChecks;
            stats.lateSkipped = record.lateSkipped;
            stats.lateChanged = record.lateChanged;
            stats.lateMaxLocalBones = record.lateMaxLocalBones;
            stats.lateMaxObjectBones = record.lateMaxObjectBones;
            stats.lateWorstBone = record.lateWorstBone;
            stats.lateMaxTranslation = record.lateMaxTranslation;
            stats.lateSwitches = record.lateSwitches;
            stats.lateCapped = record.lateCapped;
            stats.lateSamples = record.lateSamples;
            stats.lateEqualChecks = record.lateEqualChecks;
            stats.lateVerified = record.lateVerified;
            stats.lateVerifyMismatch = record.lateVerifyMismatch;
            stats.lateDirect = record.lateDirect;
            stats.stallPolls = record.stallPolls;
            stats.pollsSkippedRecent = record.pollsSkippedRecent;
            stats.ringTurnBusy = record.hot.ringTurnBusy.load();
            stats.lateIntervalMaxMs = record.lateIntervalMaxMs;
            if (stats.captured > 0)
            {
                stats.detourCaptureAvgUs =
                    TicksToUs(record.hot.captureTicksTotal.load()) / static_cast<double>(stats.captured);
            }
            stats.detourCaptureMaxUs = TicksToUs(record.hot.captureTicksMax.load());
            // Evidence: the unwritten-row census.
            stats.unwrittenSamples = record.unwrittenSamples;
            stats.unwrittenMinRows = record.unwrittenSamples ? record.unwrittenMinRows : 0;
            stats.unwrittenMaxRows = record.unwrittenMaxRows;
            stats.unwrittenPartialRows = record.unwrittenPartialRows;
            stats.unwrittenFaults = record.unwrittenFaults;
            if (record.unwrittenSamples > 0 &&
                record.unwrittenRows.size() == static_cast<std::size_t>(record.boneCount) * 3u)
            {
                stats.unwrittenScanAvgUs =
                    TicksToUs(record.unwrittenScanTicks) / static_cast<double>(record.unwrittenSamples);
                stats.unwrittenScanMaxUs = TicksToUs(record.unwrittenScanMaxTicks);
                struct Mixed
                {
                    std::uint32_t bone;
                    std::uint32_t row;
                    std::uint32_t samples;
                };
                std::vector<Mixed> mixed;
                const std::uint64_t samples = record.unwrittenSamples;
                for (std::uint32_t bone = 0; bone < record.boneCount; ++bone)
                {
                    for (std::uint32_t row = 0; row < 3; ++row)
                    {
                        const std::uint32_t count = record.unwrittenRows[static_cast<std::size_t>(bone) * 3u + row];
                        if (count == 0)
                            continue;
                        if (count >= samples)
                        {
                            ++stats.unwrittenAlways[row];
                            continue;
                        }
                        ++stats.unwrittenMixed[row];
                        mixed.push_back({bone, row, count});
                    }
                }
                std::sort(mixed.begin(), mixed.end(), [](const Mixed& a, const Mixed& b) {
                    if (a.samples != b.samples)
                        return a.samples > b.samples;
                    return a.bone != b.bone ? a.bone < b.bone : a.row < b.row;
                });
                for (std::size_t index = 0; index < mixed.size() && index < 40; ++index)
                {
                    if (index)
                        stats.unwrittenTop += '|';
                    stats.unwrittenTop += std::to_string(mixed[index].bone);
                    stats.unwrittenTop += "trs"[mixed[index].row];
                    stats.unwrittenTop += ':' + std::to_string(mixed[index].samples);
                }
                for (std::size_t index = 0; index < record.unwrittenChanges.size(); ++index)
                {
                    if (index)
                        stats.unwrittenTimeline += '|';
                    stats.unwrittenTimeline += std::to_string(record.unwrittenChanges[index].first) + ':' +
                        std::to_string(record.unwrittenChanges[index].second);
                }
            }
            return stats;
        }
    }

    void SetStallPollMs(std::uint32_t ms)
    {
        g_stallPollMs.store(ms, std::memory_order_relaxed);
    }

    std::uint32_t StallPollMs()
    {
        return g_stallPollMs.load(std::memory_order_relaxed);
    }

    void SetLateBackoff(bool on)
    {
        g_lateBackoff.store(on, std::memory_order_relaxed);
    }

    bool LateBackoff()
    {
        return g_lateBackoff.load(std::memory_order_relaxed);
    }

    void SetContentTimes(bool on)
    {
        g_contentTimes.store(on, std::memory_order_relaxed);
    }

    void BeginRecording()
    {
        DisarmAllRecords();
        // Let an in-flight detour finish its copy before records go away.
        while (g_detoursInFlight.load(std::memory_order_seq_cst) != 0)
            Sleep(0);
        StopWorker();
        StopDiscovery();
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        g_qpcFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_discoveryQueue.clear();
        g_reparentQueue.clear();
        // Nothing is armed and no detour is in flight, the worker is stopped:
        // every ring left goes to the pool, with its end-of-frame copy.
        for (const std::unique_ptr<Record>& record : g_records)
            if (record)
                PoolRing(record->ring, record->lateAfterFinalizer, record->boneCount);
        g_records.clear();
        g_recordIndex.clear();
        g_drainList.clear(); // the worker is stopped
        g_prunedIds.clear();
        g_prunedRecords = 0;
        for (DetourStripe& stripe : g_detourStripes)
        {
            stripe.calls.store(0);
            stripe.ticks.store(0);
        }
        g_pressureWakes.store(0);
        g_workerBusyUs = 0.0;
        g_workerDiscoveryUs = 0.0;
        g_pendingPeak = 0;
        g_workerPhases = {};
        g_workerCpuDone.store(0);
        g_scriptLockWaitTicks.store(0);
        g_scriptLockWaits.store(0);
        g_scriptLockWaitMaxTicks.store(0);
        for (std::atomic<std::uint64_t>& site : g_scriptLockSiteMaxTicks)
            site.store(0);
        g_discoveryHoldMaxTicks.store(0);
        g_ringPoolHits.store(0);
        g_ringPoolMisses.store(0);
        g_reserveHits.store(0);
        g_reserveMisses.store(0);
        g_starts.store(0);
        g_startTicksTotal.store(0);
        g_startTicksMax.store(0);
        g_lateRecords.store(0);
        g_startQpc = QpcNow();
        {
            std::lock_guard<std::mutex> lock(g_pauseMutex);
            g_pauses.Clear();
            g_lastStampedQpc = g_startQpc;
        }
        g_lastTickQpc = g_startQpc;
        g_clockPaused.store(false, std::memory_order_release);
    }

    EngineTiming EngineTimingSnapshot()
    {
        EngineTiming timing;
        // The sum of the detour stripes.
        std::uint64_t detourTicks = 0;
        for (const DetourStripe& stripe : g_detourStripes)
        {
            timing.detourCalls += stripe.calls.load();
            detourTicks += stripe.ticks.load();
        }
        timing.pressureWakes = g_pressureWakes.load();
        const double seconds = static_cast<double>(NowMs()) / 1000.0;
        if (seconds > 0.0)
            timing.detourUsPerSecond = TicksToUs(detourTicks) / seconds;
        // Script lock waits and the worker's CPU time (the running worker's so
        // far plus those that exited).
        timing.scriptLockWaitMs = TicksToUs(g_scriptLockWaitTicks.load()) / 1000.0;
        timing.scriptLockWaits = g_scriptLockWaits.load();
        timing.scriptLockWaitMaxUs = TicksToUs(g_scriptLockWaitMaxTicks.load());
        for (std::size_t site = 0; site < kScriptLockSites; ++site)
            timing.scriptLockSiteMaxUs[site] = TicksToUs(g_scriptLockSiteMaxTicks[site].load());
        timing.discoveryHoldMaxUs = TicksToUs(g_discoveryHoldMaxTicks.load());
        timing.ringPoolHits = g_ringPoolHits.load();
        timing.ringPoolMisses = g_ringPoolMisses.load();
        timing.reserveHits = g_reserveHits.load();
        timing.reserveMisses = g_reserveMisses.load();
        timing.starts = g_starts.load();
        timing.startAvgUs = timing.starts ? TicksToUs(g_startTicksTotal.load()) / static_cast<double>(timing.starts) : 0.0;
        timing.startMaxUs = TicksToUs(g_startTicksMax.load());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            timing.ringPoolMb = static_cast<double>(g_ringPoolBytes) / (1024.0 * 1024.0);
        }
        std::uint64_t cpu = g_workerCpuDone.load();
        if (g_worker.joinable() && !g_workerExited.load(std::memory_order_acquire))
            cpu += ThreadCpu(g_worker.handle());
        timing.workerCpuMs = static_cast<double>(cpu) / 10000.0;
        std::lock_guard<std::mutex> lock(g_mutex);
        timing.workerBusyMs = g_workerBusyUs / 1000.0;
        timing.workerDiscoveryMs = g_workerDiscoveryUs / 1000.0;
        timing.pendingPeakFrames = g_pendingPeak;
        timing.workerFrames = g_workerPhases.frames;
        timing.workerDrainMs = TicksToUs(g_workerPhases.drainTicks) / 1000.0;
        timing.workerComposeMs = TicksToUs(g_workerPhases.composeTicks) / 1000.0;
        timing.workerValidateMs = TicksToUs(g_workerPhases.validateTicks) / 1000.0;
        timing.workerEncodeMs = TicksToUs(g_workerPhases.encodeTicks) / 1000.0;
        timing.workerLockWaitMs = TicksToUs(g_workerPhases.lockWaitTicks) / 1000.0;
        timing.workerLockWaitMaxUs = TicksToUs(g_workerPhases.lockWaitMaxTicks);
        return timing;
    }

    std::uint32_t NowMs()
    {
        return RecordMsAtQpc(QpcNow());
    }

    bool SetClockPaused(bool paused)
    {
        if (g_clockPaused.load(std::memory_order_acquire) == paused)
            return false;
        const std::uint64_t now = QpcNow();
        {
            std::lock_guard<std::mutex> lock(g_pauseMutex);
            // The clock ran, so the last span is closed (pause_spans.h).
            if (paused)
                g_pauses.Push({now, 0});
            else if (!g_pauses.Empty() && !g_pauses.Back().end)
                g_pauses.CloseBack(now);
        }
        g_clockPaused.store(paused, std::memory_order_release);
        return true;
    }

    bool IsClockPaused()
    {
        return g_clockPaused.load(std::memory_order_acquire);
    }

    void MarkCut()
    {
        g_cutSerial.fetch_add(1, std::memory_order_acq_rel);
    }

    std::uint32_t ExcludeScriptStall(std::uint32_t minGapMs)
    {
        const std::uint64_t now = QpcNow();
        const std::uint64_t last = g_lastTickQpc;
        g_lastTickQpc = now;
        if (g_clockPaused.load(std::memory_order_acquire) || now <= last)
            return 0;
        std::lock_guard<std::mutex> lock(g_pauseMutex);
        // Never before the end of the previous span or a stamped frame.
        std::uint64_t start = last;
        if (!g_pauses.Empty() && g_pauses.Back().end > start)
            start = g_pauses.Back().end;
        if (g_lastStampedQpc > start)
            start = g_lastStampedQpc;
        if (now <= start)
            return 0;
        const std::uint64_t gapMs = (now - start) * 1000ull / g_qpcFrequency;
        if (gapMs < minGapMs)
            return 0;
        g_pauses.Push({start, now}); // the clock runs, the last span is closed
        return static_cast<std::uint32_t>(gapMs);
    }

    std::uint32_t RecordMsAgo(std::uint64_t ageMs, std::uint32_t nowMs)
    {
        const std::uint64_t now = QpcNow();
        const std::uint64_t back = ageMs * g_qpcFrequency / 1000ull;
        if (now <= g_startQpc || back >= now - g_startQpc)
            return 0;
        const std::uint32_t ms = RecordMsAtQpc(now - back);
        return ms < nowMs ? ms : nowMs;
    }

    std::uint32_t PausedMsTotal()
    {
        const std::uint64_t now = QpcNow();
        return static_cast<std::uint32_t>(PausedTicksBefore(now) * 1000ull / g_qpcFrequency);
    }

    // The weapon places.
    std::atomic<std::uint32_t> g_extraPlaces{0};
    // Up to every slot (Start clamps places to kLiveTargetSlots).
    void SetExtraPlaces(std::uint32_t places)
    {
        g_extraPlaces.store(places > kLiveTargetSlots ? kLiveTargetSlots : places, std::memory_order_relaxed);
    }

    bool Start(
        std::uint32_t recordId,
        void* skeleton,
        std::uint32_t boneCount,
        std::uint32_t sampleHz,
        std::string& error)
    {
        if (!skeleton || boneCount == 0 || boneCount > 4096 || sampleHz == 0)
        {
            error = "invalid_target";
            return false;
        }
        // A free slot among the session's places (kMaxLiveTargets: PedCapacity
        // plus the vehicles, 64 by default), the duplicate check over every slot;
        // plus the weapon places (SetExtraPlaces), which only the weapons in a
        // hand use.
        const std::uint32_t places = (std::min)(
            static_cast<std::uint32_t>(kMaxLiveTargets) + g_extraPlaces.load(std::memory_order_relaxed), kLiveTargetSlots);
        std::atomic<Record*>* freeArm = nullptr;
        for (std::atomic<Record*>& armed : g_armed)
        {
            const Record* record = armed.load(std::memory_order_acquire);
            if (record && record->skeleton == skeleton)
            {
                error = "skeleton_already_armed";
                return false;
            }
            if (!record && !freeArm && static_cast<std::uint32_t>(&armed - g_armed) < places)
                freeArm = &armed;
        }
        if (!freeArm)
        {
            error = "no_free_target";
            return false;
        }

        // The record is built outside g_mutex (the worker would wait behind the
        // allocation), with a pooled ring when one of this bone count is free.
        const std::uint64_t startStarted = QpcNow();
        std::unique_ptr<Slot[]> ring;
        // The pooled ring's end-of-frame copy, and reserve buffers for what has
        // to be made here (the eager slots on a pool miss, the copy when the pool
        // gave none), when the skeleton fits them.
        std::vector<BoneQts> late;
        std::vector<BoneQts> reserved[kEagerSlots + 1];
        std::size_t wanted = 0;
        std::size_t reservedCount = 0;
        {
            ScriptLock lock(ScriptLockSite::Start);
            ring = TakePooledRing(boneCount, late);
            wanted = (ring ? 0u : kEagerSlots) + (late.empty() ? 1u : 0u);
            // Not for a skeleton under half a reserve buffer, which would keep
            // most of its capacity idle.
            if (boneCount <= kReserveBones && static_cast<std::size_t>(boneCount) * 2u >= kReserveBones)
                reservedCount = TakeReserveBuffers(reserved, wanted);
        }
        (ring ? g_ringPoolHits : g_ringPoolMisses).fetch_add(1, std::memory_order_relaxed);
        g_reserveHits.fetch_add(reservedCount, std::memory_order_relaxed);
        g_reserveMisses.fetch_add(wanted - reservedCount, std::memory_order_relaxed);
        // 2 x bones default BoneQts: a reserve buffer resized (the same values,
        // written on resident pages), else a new allocation.
        const std::size_t poseSize = static_cast<std::size_t>(boneCount) * 2u;
        std::size_t nextReserved = 0;
        const auto makePose = [&](std::vector<BoneQts>& pose) {
            if (nextReserved < reservedCount)
            {
                pose = std::move(reserved[nextReserved++]);
                pose.resize(poseSize);
            }
            else
            {
                pose.assign(poseSize, BoneQts{});
            }
        };
        bool lazy = false;
        if (!ring)
        {
            ring = std::make_unique<Slot[]>(kRingSlots);
            for (std::uint32_t index = 0; index < kRingSlots; ++index)
            {
                if (index < kEagerSlots)
                    makePose(ring[index].pose);
                else
                    ring[index].state = kSlotUnallocated;
            }
            lazy = true;
        }
        // A pooled copy keeps the last record's bytes. Nothing reads
        // lateAfterFinalizer before the detour copied this skeleton into all of it
        // for a request of this record: the script reads it only after lateReady,
        // which CopyLateAfterFinalizer raises after a complete copy, and a new
        // record starts with lateReady false. So it is not reset.
        if (late.empty())
            makePose(late);
        auto created = std::make_unique<Record>();
        created->id = recordId;
        created->skeleton = static_cast<std::uint8_t*>(skeleton);
        created->boneCount = boneCount;
        created->intervalTicks = g_qpcFrequency / sampleHz;
        // The keys for this record's life.
        created->stallPollTicks = static_cast<std::uint64_t>(g_stallPollMs.load(std::memory_order_relaxed)) *
            g_qpcFrequency / 1000u;
        created->lateBackoff = g_lateBackoff.load(std::memory_order_relaxed);
        created->contentTimes = g_contentTimes.load(std::memory_order_relaxed);
        created->ring = std::move(ring);
        created->lazySlots.store(lazy, std::memory_order_relaxed);
        created->stats.boneCount = boneCount;
        created->engineParents.assign(boneCount, 0xFFFFu);
        created->unwrittenRows.assign(static_cast<std::size_t>(boneCount) * 3u, 0u); // evidence
        created->lateAfterFinalizer = std::move(late); // locals, objects (see above)
        Record* record = nullptr;
        {
            ScriptLock lock(ScriptLockSite::Start); // the wait is measured
            // A pruned id too, as when its record was still there.
            if (FindRecord(recordId) || g_prunedIds.count(recordId) != 0)
            {
                PoolRing(created->ring, created->lateAfterFinalizer, boneCount);
                error = "record_exists";
                return false;
            }
            record = created.get();
            g_records.push_back(std::move(created));
            g_recordIndex.emplace(recordId, record);
            g_drainList.push_back(record);
        }
        {
            const std::uint64_t took = QpcNow() - startStarted;
            g_starts.fetch_add(1, std::memory_order_relaxed);
            g_startTicksTotal.fetch_add(took, std::memory_order_relaxed);
            AtomicMax(g_startTicksMax, took);
        }

        StartWorker();
        if (!EnsureHook(error))
        {
            record->retired.store(true, std::memory_order_seq_cst);
            return false;
        }
        g_armedCount.fetch_add(1, std::memory_order_acq_rel);
        // The lookups' scan end covers the slot before it is published (a
        // lookup that read the old end misses the slot as one made before this
        // Start would).
        const std::size_t armIndex = static_cast<std::size_t>(freeArm - g_armed);
        g_armedScanEnd.store(replay::capacity::RaisedScanEnd(g_armedScanEnd.load(std::memory_order_relaxed),
                                 static_cast<std::uint32_t>(armIndex)),
            std::memory_order_seq_cst);
        freeArm->store(record, std::memory_order_seq_cst);
        // The lookup hints of the slot, after the authoritative store.
        g_armedRecordSkeleton[armIndex].store(record->skeleton, std::memory_order_release);
        g_armedRecordId[armIndex].store(recordId, std::memory_order_release);
        return true;
    }

    namespace
    {
        // The armed record of this id, found through the id hints and checked
        // against the authoritative g_armed[i] (the caller raised
        // g_detoursInFlight when it touches the record's ring). Script thread:
        // the hints' only writer, so a hint is never stale here. The id 0, which
        // the recorder never makes, is no hint: it is looked up in every slot.
        Record* FindArmedRecord(std::uint32_t recordId)
        {
            const std::uint32_t end = g_armedScanEnd.load(std::memory_order_acquire);
            for (std::uint32_t index = 0; index < end && index < kLiveTargetSlots; ++index)
            {
                if (recordId != 0 && g_armedRecordId[index].load(std::memory_order_acquire) != recordId)
                    continue;
                Record* record = g_armed[index].load(std::memory_order_seq_cst);
                if (record && record->id == recordId)
                    return record;
            }
            return nullptr;
        }

        // PollIfStalled for an armed record (g_detoursInFlight raised).
        void PollIfStalledArmed(Record& record)
        {
            const std::uint64_t calls = record.hot.detourCalls.load(std::memory_order_relaxed);
            if (calls == record.scriptSeenDetourCalls)
            {
                if (record.stallPollTicks == 0)
                {
                    record.hot.scriptPolls.fetch_add(1, std::memory_order_relaxed);
                    CaptureFromDetour(record, record.skeleton, false, true);
                }
                // PoseStallPollMs on: copied here only when the finalizer
                // never visited the skeleton or left it alone for the key's
                // time, through the stall polls' own gate. An NPC the engine
                // animates a few times per second keeps the detour's samples
                // (the script's copy of a call's output would take a shared
                // gate and refuse the next call). A record sampled at the
                // end of the frame (lateCapture) is not sampled by the
                // detour, and its end-of-frame capture later in this tick
                // passes the detour's gate: its poll takes that gate, so
                // only one of the two samples.
                else if (StallPollDue(calls, record.hot.lastCallQpc.load(std::memory_order_relaxed),
                             record.stallPollTicks, QpcNow()))
                {
                    record.hot.scriptPolls.fetch_add(1, std::memory_order_relaxed);
                    ++record.stallPolls;
                    CaptureFromDetour(record, record.skeleton, false, true,
                        !record.lateCapture.load(std::memory_order_acquire));
                }
                else
                {
                    ++record.pollsSkippedRecent;
                }
            }
            record.scriptSeenDetourCalls = calls;
        }

        // The end-of-frame check of an armed record (g_detoursInFlight raised, the
        // clock not paused).
        LateCheck CheckLatePoseArmed(Record& record)
        {
            LateCheck result;
            static std::vector<BoneQts> late;    // end-of-frame locals then objects
            static std::vector<BoneQts> scratch; // CompareLatePose's composed objects
            static std::vector<std::uint8_t> changedMask; // bones whose bytes differ
            const std::uint32_t boneCount = record.boneCount;
            const std::uint64_t now = QpcNow();
            const bool parentsReady = record.engineParentsReady.load(std::memory_order_acquire) &&
                record.engineParents.size() == boneCount;
            // PoseLateBackoff on: the first check waits a hash of the
            // record's id (0 to 999 ms) from the tick that first sees the
            // engine parent table, instead of running in that tick (so the
            // records registered by one scan do not all check in the same
            // tick). No request is out yet: one needs the table.
            if (record.lateBackoff && parentsReady && !record.lateFirstScheduled)
            {
                record.lateFirstScheduled = true;
                record.nextLateCheckQpc = now + LateFirstCheckDelayMs(record.id) * g_qpcFrequency / 1000u;
            }
            if (record.lateReady.load(std::memory_order_acquire))
            {
                // The buffer as the frame left it, only when no finalizer
                // call came after the copy (checked before and after the
                // read, so a call during it discards the check too).
                bool ok = parentsReady &&
                    record.hot.detourCalls.load(std::memory_order_acquire) == record.lateCopyCall;
                // A check that is not a verification check first compares
                // the buffer in place with the detour's copy, between the
                // same two call checks. Equal bytes (the common case) settle
                // it with no copy: below, the result CompareLatePoseChanged
                // gives a copy of them. Different bytes, a fault or a call
                // during the read: the copy path, which reads the buffer
                // again and decides.
                bool direct = false;
                if (ok && LateDirectCompare(true, record.lateChecks))
                {
                    REPLAY_PROFILE_ZONE("rec.pose_late_direct");
                    std::uint8_t* poseBuffer = nullptr;
                    std::uint8_t* world = nullptr;
                    std::uint32_t liveBones = 0;
                    direct = GuardedReadSkeleton(record.skeleton, poseBuffer, world, liveBones) && poseBuffer &&
                        liveBones == boneCount &&
                        GuardedLateBlocksEqual(poseBuffer, record.lateAfterFinalizer.data(),
                            record.lateAfterFinalizer.size() * sizeof(BoneQts)) &&
                        record.hot.detourCalls.load(std::memory_order_acquire) == record.lateCopyCall;
                }
                if (ok && !direct)
                {
                    REPLAY_PROFILE_ZONE("rec.pose_late_copy");
                    std::uint8_t* poseBuffer = nullptr;
                    std::uint8_t* world = nullptr;
                    std::uint32_t liveBones = 0;
                    late.resize(static_cast<std::size_t>(boneCount) * 2u);
                    ok = GuardedReadSkeleton(record.skeleton, poseBuffer, world, liveBones) && poseBuffer &&
                        liveBones == boneCount &&
                        GuardedCopy(late.data(), poseBuffer, late.size() * sizeof(BoneQts)) &&
                        record.hot.detourCalls.load(std::memory_order_acquire) == record.lateCopyCall;
                }
                record.lateReady.store(false, std::memory_order_release);
                if (!ok)
                {
                    ++record.lateSkipped;
                    record.nextLateCheckQpc = now; // ask again next tick
                }
                else
                {
                    ++record.lateChecks;
                    result.checked = true;
                    // The detour kept the finalizer's locals and objects; only
                    // bones whose bytes changed since are compared (none, one
                    // memcmp per block, in almost every check). One check in
                    // kLateVerifyEvery also runs the full comparison and uses
                    // its result; a difference is counted.
                    LatePoseChange change;
                    if (direct)
                    {
                        // The zero change (no bones, worst bone -1, maxima 0) with
                        // equal blocks, as counted by the copy path.
                        ++record.lateEqualChecks;
                        ++record.lateDirect;
                    }
                    else
                    {
                        REPLAY_PROFILE_ZONE("rec.pose_late_compare");
                        bool equalBlocks = false;
                        change = CompareLatePoseChanged(record.lateAfterFinalizer.data(),
                            record.lateAfterFinalizer.data() + boneCount, late.data(), late.data() + boneCount,
                            record.engineParents.data(), boneCount, record.engineRootScale, kLateTranslation,
                            kLateRotation, scratch, changedMask, equalBlocks);
                        if (equalBlocks)
                            ++record.lateEqualChecks;
                        if (IsLateVerifyCheck(record.lateChecks)) // the rule LateDirectCompare leaves to this path
                        {
                            const LatePoseChange full = CompareLatePose(record.lateAfterFinalizer.data(), late.data(),
                                late.data() + boneCount, record.engineParents.data(), boneCount,
                                record.engineRootScale, kLateTranslation, kLateRotation, scratch);
                            ++record.lateVerified;
                            if (full.localBones != change.localBones || full.objectBones != change.objectBones ||
                                full.worstBone != change.worstBone ||
                                std::memcmp(&full.maxTranslation, &change.maxTranslation, sizeof(float)) != 0 ||
                                std::memcmp(&full.maxRotation, &change.maxRotation, sizeof(float)) != 0)
                            {
                                ++record.lateVerifyMismatch;
                            }
                            change = full;
                        }
                    }
                    result.localBones = change.localBones;
                    result.objectBones = change.objectBones;
                    result.worstBone = change.worstBone;
                    result.maxTranslation = change.maxTranslation;
                    result.maxRotation = change.maxRotation;
                    result.changed = change.localBones + change.objectBones > 0;
                    if (result.changed)
                    {
                        ++record.lateChanged;
                        record.lateMaxLocalBones = (std::max)(record.lateMaxLocalBones, change.localBones);
                        record.lateMaxObjectBones = (std::max)(record.lateMaxObjectBones, change.objectBones);
                        if (change.maxTranslation >= record.lateMaxTranslation)
                        {
                            record.lateMaxTranslation = change.maxTranslation;
                            record.lateWorstBone = change.worstBone;
                        }
                        record.lateCleanChecks = 0;
                        if (!record.lateCapture.load(std::memory_order_relaxed))
                        {
                            // Claim a place under the cap.
                            std::uint32_t current = g_lateRecords.load(std::memory_order_acquire);
                            bool claimed = false;
                            while (current < kMaxLateRecords &&
                                !(claimed = g_lateRecords.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)))
                            {
                            }
                            if (claimed)
                            {
                                record.lateCapture.store(true, std::memory_order_release);
                                ++record.lateSwitches;
                                result.switchedOn = true;
                            }
                            else
                            {
                                ++record.lateCapped;
                                result.capped = true;
                            }
                        }
                    }
                    else if (record.lateCapture.load(std::memory_order_relaxed) &&
                        ++record.lateCleanChecks >= kLateCleanChecksToLeave)
                    {
                        record.lateCapture.store(false, std::memory_order_release);
                        g_lateRecords.fetch_sub(1, std::memory_order_acq_rel);
                        record.lateCleanChecks = 0;
                        result.switchedOff = true;
                    }
                    // The wait, 250 ms while it changed or is sampled at the
                    // end of the frame, else 1 s; with PoseLateBackoff, clean
                    // checks wait 1, 2, 4, then 8 s (NextLateCheckMs).
                    const std::uint64_t waitMs = NextLateCheckMs(record.lateBackoff, result.changed,
                        record.lateCapture.load(std::memory_order_relaxed), record.lateCleanWaitMs);
                    record.lateIntervalMaxMs = (std::max)(record.lateIntervalMaxMs, waitMs);
                    record.nextLateCheckQpc = now + waitMs * g_qpcFrequency / 1000u;
                }
            }
            else if (record.lateWanted.load(std::memory_order_acquire))
            {
                // No finalizer call took the request for a second: the
                // skeleton is not animated any more (a corpse), PollIfStalled
                // samples it; the request is withdrawn and the record leaves
                // end-of-frame capture.
                bool wanted = true;
                if (now >= record.lateWantedQpc + g_qpcFrequency &&
                    record.lateWanted.compare_exchange_strong(wanted, false, std::memory_order_acq_rel))
                {
                    ++record.lateSkipped;
                    record.nextLateCheckQpc = now + kLateCheckMs * g_qpcFrequency / 1000u;
                    if (record.lateCapture.exchange(false, std::memory_order_acq_rel))
                    {
                        g_lateRecords.fetch_sub(1, std::memory_order_acq_rel);
                        record.lateCleanChecks = 0;
                        result.switchedOff = true;
                    }
                }
            }
            else if (parentsReady && now >= record.nextLateCheckQpc)
            {
                record.lateWantedQpc = now;
                record.lateWanted.store(true, std::memory_order_release);
            }
            // End-of-frame capture: the same rate gate and ring as the
            // detour, locals and objects as the frame left them.
            if (record.lateCapture.load(std::memory_order_acquire))
            {
                const std::uint64_t before = record.hot.captured.load(std::memory_order_relaxed);
                CaptureFromDetour(record, record.skeleton, false, true);
                if (record.hot.captured.load(std::memory_order_relaxed) != before)
                    ++record.lateSamples;
            }
            return result;
        }
    }

    void PollFromScript(std::uint32_t recordId)
    {
        if (g_clockPaused.load(std::memory_order_acquire))
            return;
        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (Record* record = FindArmedRecord(recordId))
        {
            record->hot.scriptPolls.fetch_add(1, std::memory_order_relaxed);
            CaptureFromDetour(*record, record->skeleton, false, true);
        }
        g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
    }

    void PollIfStalled(std::uint32_t recordId)
    {
        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (Record* record = FindArmedRecord(recordId))
            PollIfStalledArmed(*record);
        g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
    }

    LateCheck PollAndCheckLate(std::uint32_t recordId)
    {
        // The two calls' work with one lookup and one g_detoursInFlight
        // bracket. Only the script thread changes g_armed and the pause, so the
        // second call finds the same record and the same clock state.
        LateCheck result;
        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (Record* record = FindArmedRecord(recordId))
        {
            PollIfStalledArmed(*record);
            if (!g_clockPaused.load(std::memory_order_acquire))
                result = CheckLatePoseArmed(*record);
        }
        g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
        return result;
    }

    bool ContentTimeMs(std::uint32_t recordId, std::uint32_t& ms)
    {
        // The later of the two is what the buffer holds, the time the capture
        // stamps it with. With PoseStallPollMs at 0 a skeleton the finalizer
        // visited since the previous tick is not polled (its call is the later);
        // one it did not visit is polled in this tick, as every vehicle and every
        // record sampled at the end of the frame are (the script's capture is the
        // later: it follows the frame's call). With the key on (the default, 250
        // ms), a skeleton called less than that ago is not polled either: its last
        // call. Reads only.
        std::uint64_t qpc = 0;
        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (const Record* record = FindArmedRecord(recordId))
        {
            if (record->contentTimes)
                qpc = (std::max)(record->hot.lastCallQpc.load(std::memory_order_relaxed), record->scriptReadQpc);
        }
        g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
        if (qpc <= g_startQpc)
            return false; // neither (or a call stamped before this recording's clock started)
        ms = RecordMsAtQpc(qpc);
        return true;
    }

    void Stop(std::uint32_t recordId)
    {
        for (std::atomic<Record*>& armed : g_armed)
        {
            Record* record = armed.load(std::memory_order_acquire);
            if (!record || record->id != recordId)
                continue;
            armed.store(nullptr, std::memory_order_seq_cst);
            ClearArmedHints(static_cast<std::size_t>(&armed - g_armed));
            record->retired.store(true, std::memory_order_seq_cst);
            g_armedCount.fetch_sub(1, std::memory_order_acq_rel);
            if (record->lateCapture.exchange(false, std::memory_order_acq_rel))
                g_lateRecords.fetch_sub(1, std::memory_order_acq_rel);
        }
        if (g_wake)
            SetEvent(g_wake);
    }

    void StopAll()
    {
        DisarmAllRecords();
        while (g_detoursInFlight.load(std::memory_order_seq_cst) != 0)
            Sleep(0);
        // The worker drains every ring before it exits; discovery then
        // finishes what the drain queued, so every record has its track.
        StopWorker();
        StopDiscovery();
        // Every held frame that can go in is in by now: the worker's last
        // drain appends them, and the discovery thread appends the ones its last
        // jobs left before it leaves, running the jobs a contradiction among
        // them queues (a flush here could not run those). This one only finds
        // work if the discovery thread was not there to do it.
        ScriptLock lock(ScriptLockSite::Other);
        FlushHeldFrames();
        // The recording is over (worker and discovery thread stopped, every held
        // frame in): every track only decodes from now on.
        for (const std::unique_ptr<Record>& record : g_records)
        {
            if (record->track)
                record->track->Seal();
        }
    }

    std::vector<std::uint32_t> RecordIds()
    {
        ScriptLock lock(ScriptLockSite::RecordIds); // the wait is measured
        std::vector<std::uint32_t> ids;
        for (const std::unique_ptr<Record>& record : g_records)
            ids.push_back(record->id);
        return ids;
    }

    Stats Snapshot(std::uint32_t recordId)
    {
        ScriptLock lock(ScriptLockSite::Snapshot); // the wait is measured
        const Record* record = FindRecord(recordId);
        return record ? SnapshotLocked(*record) : Stats{};
    }

    bool TrackSpan(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs)
    {
        ScriptLock lock(ScriptLockSite::TrackSpan); // the wait is measured
        const Record* record = FindRecord(recordId);
        if (!record || !record->track || record->track->Stats().frames == 0)
            return false;
        firstMs = record->track->Stats().firstTimeMs;
        lastMs = record->track->Stats().lastTimeMs;
        return true;
    }

    bool ReadTrackSignature(std::uint32_t recordId, tpose::TrackSignature& out)
    {
        TrackExport exported;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const Record* record = FindRecord(recordId);
            if (!record || !record->track || record->track->Stats().frames == 0)
                return false;
            record->track->Export(exported); // the chunk bytes are shared, not copied
        }
        out = tpose::ReadSignature(exported);
        return true;
    }

    bool SampleTrack(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Record* record = FindRecord(recordId);
        return record && record->track && record->track->Sample(timeMs, sample);
    }

    bool TrackLayoutParents(std::uint32_t recordId, std::vector<std::int32_t>& parents)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Record* record = FindRecord(recordId);
        if (!record || !record->track || record->track->Stats().frames == 0)
            return false;
        const SkeletonLayout& layout = record->track->Layout();
        if (layout.parent.size() != layout.boneCount || layout.boneCount == 0)
            return false;
        parents = layout.parent;
        return true;
    }

    bool TrackLayoutSignatureAt(std::uint32_t recordId, std::uint32_t timeMs, std::vector<std::int32_t>* parents,
        std::vector<std::uint8_t>* ambiguous, std::uintptr_t& layoutKey, std::vector<std::uint8_t>* rebuilt)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Record* record = FindRecord(recordId);
        if (!record || !record->track || record->track->Stats().frames == 0)
            return false;
        const SkeletonLayout& layout = record->track->LayoutAt(timeMs);
        if (layout.parent.size() != layout.boneCount || layout.boneCount == 0)
            return false;
        if (parents)
            *parents = layout.parent;
        if (ambiguous)
            *ambiguous = layout.ambiguous;
        if (rebuilt)
            record->track->RebuiltBonesAt(timeMs, *rebuilt);
        layoutKey = reinterpret_cast<std::uintptr_t>(&layout);
        return true;
    }

    int RequestSkeletonInfo(void* skeleton, std::uint32_t boneCount)
    {
        if (!skeleton || boneCount == 0)
            return -1;
        for (std::uint32_t index = 0; index < kSkeletonInfoSlots; ++index)
        {
            SkeletonInfoSlot& slot = g_skeletonInfo[index];
            if (slot.state.load(std::memory_order_acquire) != kInfoFree)
                continue;
            // Free: no detour touches the slot (it only claims a wanted one).
            slot.boneCount = boneCount;
            slot.parents.assign(boneCount, 0);
            slot.locals.assign(boneCount, BoneQts{});
            slot.rootScale = 0.0f;
            slot.groups = 0;
            slot.error = "none";
            slot.skeleton.store(static_cast<std::uint8_t*>(skeleton), std::memory_order_release);
            g_skeletonInfoWanted.fetch_add(1, std::memory_order_acq_rel);
            slot.state.store(kInfoWanted, std::memory_order_release);
            return static_cast<int>(index);
        }
        return -1;
    }

    SkeletonInfoState PollSkeletonInfo(int request, SkeletonInfo& info)
    {
        if (request < 0 || static_cast<std::uint32_t>(request) >= kSkeletonInfoSlots)
            return SkeletonInfoState::Failed;
        SkeletonInfoSlot& slot = g_skeletonInfo[request];
        const int state = slot.state.load(std::memory_order_acquire);
        if (state == kInfoWanted || state == kInfoBusy)
            return SkeletonInfoState::Pending;
        if (state != kInfoReady && state != kInfoFailed)
            return SkeletonInfoState::Failed;
        info.error = slot.error;
        info.rootScale = slot.rootScale;
        info.groups = slot.groups;
        info.parents.swap(slot.parents);
        info.locals.swap(slot.locals);
        slot.skeleton.store(nullptr, std::memory_order_release);
        slot.state.store(kInfoFree, std::memory_order_release);
        return state == kInfoReady ? SkeletonInfoState::Ready : SkeletonInfoState::Failed;
    }

    void CancelSkeletonInfo(int request)
    {
        if (request < 0 || static_cast<std::uint32_t>(request) >= kSkeletonInfoSlots)
            return;
        SkeletonInfoSlot& slot = g_skeletonInfo[request];
        for (;;)
        {
            int state = slot.state.load(std::memory_order_acquire);
            if (state == kInfoWanted)
            {
                if (slot.state.compare_exchange_strong(state, kInfoFree, std::memory_order_acq_rel))
                {
                    g_skeletonInfoWanted.fetch_sub(1, std::memory_order_acq_rel);
                    break;
                }
                continue;
            }
            if (state != kInfoBusy)
                break; // done (the detour counted it) or free
            // A detour reading it finishes in microseconds (two guarded copies).
            YieldProcessor();
        }
        slot.skeleton.store(nullptr, std::memory_order_release);
        slot.state.store(kInfoFree, std::memory_order_release);
    }

    std::size_t DropBefore(std::uint32_t timeMs)
    {
        ScriptLock lock(ScriptLockSite::Drop); // the wait is measured
        // The held frames first. One of them can open the chunk that lets an
        // older one go, and a chunk kept here would reach a save (with its
        // layout in the export's table).
        FlushHeldFrames();
        std::size_t dropped = 0;
        for (const std::unique_ptr<Record>& record : g_records)
        {
            if (record->track)
            {
                dropped += record->track->DropBefore(timeMs);
                record->trackBytes.store(record->track->Bytes(), std::memory_order_relaxed);
            }
        }
        // The finished records whose last frame is before timeMs go, track and
        // all. The buffer passes replay::buffer::ExportableBaseMs, the base every
        // later window cut starts at or after, and the cut drops a record that
        // ended before its base (recording_window.cpp CutPoseRecord, "ended
        // before the window"): the saved clips do not change. Otherwise their
        // last chunk would stay (PoseTrack::DropBefore keeps it), counted in
        // TrackBytes, moving the buffer's head and shrinking the window.
        // Nothing else holds a finished record: it is off g_drainList and out of
        // g_armed with no detour in flight (its ring was released), and the
        // discovery thread only holds records with a discovery queued or a job
        // running, or a re-parent queue entry (checked here). The rest keep their
        // creation order.
        const auto queued = [](const Record* record) {
            return std::find(g_reparentQueue.begin(), g_reparentQueue.end(), record) != g_reparentQueue.end() ||
                std::find(g_discoveryQueue.begin(), g_discoveryQueue.end(), record) != g_discoveryQueue.end();
        };
        g_prunedRecords += StableRemove(g_records,
            [&](const std::unique_ptr<Record>& record) {
                return RecordPrunable(record->finished, queued(record.get()), record->track != nullptr,
                    record->track ? record->track->Stats().lastTimeMs : 0u, timeMs);
            },
            [](const std::unique_ptr<Record>& record) {
                const auto found = g_recordIndex.find(record->id);
                if (found != g_recordIndex.end() && found->second == record.get())
                    g_recordIndex.erase(found);
                g_prunedIds.insert(record->id);
            });
        return dropped;
    }

    void FlushRings()
    {
        // With a PoseWorkerPollMs above 5 a save could miss that many ms of
        // frames still in the rings: they are drained here, as the worker drains
        // them (DrainOne, each record's slots in order). Measured with the
        // export's wait (it runs right before ExportRecords).
        ScriptLock lock(ScriptLockSite::Export);
        for (bool progressed = true; progressed;)
        {
            progressed = false;
            for (Record* record : g_drainList)
                progressed = DrainReady(*record) != 0 || progressed;
        }
    }

    RecordCounts RecordCountsSnapshot()
    {
        // Evidence, every 10 s and at the stop (not one of the measured script
        // sites).
        RecordCounts counts;
        std::lock_guard<std::mutex> lock(g_mutex);
        counts.records = g_records.size();
        counts.drain = g_drainList.size();
        for (const std::unique_ptr<Record>& record : g_records)
            counts.sealed += record->track && record->track->Sealed() ? 1u : 0u;
        counts.pruned = g_prunedRecords;
        return counts;
    }

    void SetWorkerPollMs(std::uint32_t ms)
    {
        g_workerPollMs.store(ClampWorkerPollMs(static_cast<int>(ms)), std::memory_order_relaxed);
    }

    std::uint32_t WorkerPollMs()
    {
        return g_workerPollMs.load(std::memory_order_relaxed);
    }

    std::size_t TrackBytes()
    {
        // Without g_mutex, the sum of the records' trackBytes. g_records only
        // changes on the script thread (Start, BeginRecording, ImportRecords and
        // DropBefore's prune; the worker walks g_drainList and the discovery
        // thread only reads g_records, under g_mutex), which is the caller's, so
        // the vector and its records stay put while this reads them; each count
        // is stored under g_mutex by whoever changes its track's size
        // (AppendFrame, DropBefore, ImportRecords). A record without a track
        // counts 0.
        std::size_t bytes = 0;
        for (const std::unique_ptr<Record>& record : g_records)
            bytes += static_cast<std::size_t>(record->trackBytes.load(std::memory_order_relaxed));
        return bytes;
    }

    std::vector<PoseRecordExport> ExportRecords()
    {
        ScriptLock lock(ScriptLockSite::Export); // the wait is measured
        // While the recording runs (the buffer's saves), the held frames go in
        // first, so the tracks are exported whole (after StopAll there are none).
        FlushHeldFrames();
        std::vector<PoseRecordExport> out;
        for (const std::unique_ptr<Record>& record : g_records)
        {
            if (!record->track)
                continue;
            PoseRecordExport exported;
            exported.recordId = record->id;
            exported.boneCount = record->boneCount;
            record->track->Export(exported.track);
            out.push_back(std::move(exported));
        }
        return out;
    }

    bool ImportRecords(const std::vector<PoseRecordExport>& records, std::string& error)
    {
        std::vector<std::unique_ptr<Record>> loaded;
        for (const PoseRecordExport& exported : records)
        {
            auto track = std::make_unique<PoseTrack>();
            std::string trackError;
            if (!track->Import(exported.track, trackError))
            {
                error = "record_" + std::to_string(exported.recordId) + "_" + trackError;
                return false;
            }
            auto record = std::make_unique<Record>();
            record->id = exported.recordId;
            record->boneCount = exported.boneCount;
            // Never armed: no ring, retired from the start.
            record->retired.store(true, std::memory_order_seq_cst);
            record->stats.boneCount = exported.boneCount;
            record->stats.layoutReady = true;
            const TrackStats& trackStats = track->Stats();
            record->stats.encodedFrames = trackStats.frames;
            record->stats.keyframes = trackStats.keyframes;
            record->stats.relativeKeyframes = trackStats.relativeKeyframes;
            record->stats.fullKeyframeBytes = trackStats.fullKeyframeBytes;
            record->stats.relativeKeyframeBytes = trackStats.relativeKeyframeBytes;
            record->stats.trackBytes = trackStats.bytes;
            record->stats.droppedChunks = trackStats.droppedChunks;
            record->stats.durationMs = trackStats.lastTimeMs - trackStats.firstTimeMs;
            record->trackBytes.store(track->Bytes(), std::memory_order_relaxed);
            record->track = std::move(track);
            loaded.push_back(std::move(record));
        }
        BeginRecording();
        ScriptLock lock(ScriptLockSite::Import); // measured like the other script sites
        g_records = std::move(loaded);
        RebuildRecordIndex();
        return true;
    }

    bool ArmOverride(
        std::uint32_t slotIndex,
        void* skeleton,
        std::uint32_t boneCount,
        std::string& error)
    {
        if (slotIndex >= kMaxOverrides || !skeleton || boneCount == 0 || boneCount > 4096)
        {
            error = "invalid_override";
            return false;
        }
        // Through the skeleton hints (script thread, their writer), then the
        // slot; only the capture slots in use.
        const std::uint32_t captureEnd = g_armedScanEnd.load(std::memory_order_acquire);
        for (std::uint32_t index = 0; index < captureEnd && index < kLiveTargetSlots; ++index)
        {
            if (g_armedRecordSkeleton[index].load(std::memory_order_acquire) != skeleton)
                continue;
            const Record* record = g_armed[index].load(std::memory_order_acquire);
            if (record && record->skeleton == skeleton)
            {
                error = "skeleton_is_capture_target";
                return false;
            }
        }
        if (!EnsureHook(error))
            return false;

        Override& slot = g_overrides[slotIndex];
        DisarmOverride(slotIndex);
        AcquireSRWLockExclusive(&slot.lock);
        slot.boneCount = boneCount;
        slot.poseLocals.assign(boneCount, BoneQts{});
        slot.poseObjects.assign(boneCount, BoneQts{});
        slot.hasData = false;
        slot.altBoneCount = 0;
        slot.altPose.clear();
        slot.altHasData = false;
        slot.poseHashValid = false;
        ReleaseSRWLockExclusive(&slot.lock);
        slot.applies.store(0);
        slot.altApplies.store(0);
        slot.boneCountMismatch.store(0);
        slot.writeFaults.store(0);
        slot.publishes.store(0);
        slot.hasWrite.store(false);
        slot.late.store(false);
        SetLateBit(slotIndex, false);
        slot.latePending.store(false);
        slot.preRenderSeen.store(false);
        slot.lateWrites.store(0);
        if (slot.rebuildGuard.exchange(false))
            g_guardedSlots.fetch_sub(1, std::memory_order_acq_rel);
        slot.guardWritesThisTick.store(0);
        slot.guardApplies.store(0);
        slot.prefixBones.store(0);
        slot.prefixApplies.store(0);
        slot.skippedOriginal.store(0);
        slot.preRenderWrites.store(0);  // not kept from the slot's last arm
        slot.weaponPassWrites.store(0);
        slot.staleApplies.store(0);
        slot.scriptWrites = 0;
        slot.scriptSurvived = 0;
        slot.scriptOverwritten = 0;
        slot.appliesAtPublish = 0;
        slot.writeChecks = 0;
        slot.writeChanged = 0;
        // The lookups' scan end covers the slot before it is published.
        g_overrideScanEnd.store(replay::capacity::RaisedScanEnd(g_overrideScanEnd.load(std::memory_order_relaxed), slotIndex),
            std::memory_order_seq_cst);
        slot.armed.store(static_cast<std::uint8_t*>(skeleton), std::memory_order_release);
        g_armedSkeleton[slotIndex].store(static_cast<std::uint8_t*>(skeleton), std::memory_order_release);
        return true;
    }

    namespace
    {
        // PublishOverride and PublishOverrideSample share this; `store` puts the
        // pose into the slot vectors (copy or swap).
        template <typename Store>
        bool PublishPose(std::uint32_t slotIndex, const BoneQts* locals, const BoneQts* objects,
            std::uint32_t boneCount, Store&& store)
        {
            Override& slot = g_overrides[slotIndex];
            // The hash of what every write of this pose puts in the
            // buffer, taken once here and outside the lock; only the late
            // (vehicle) slots use it on the detour side, the others hash on
            // demand.
            const bool late = slot.late.load(std::memory_order_acquire);
            std::uint64_t hash = 0;
            if (late)
            {
                PoseHasher hasher;
                hasher.Add(locals, sizeof(BoneQts) * boneCount);
                hasher.Add(objects, sizeof(BoneQts) * boneCount);
                hash = hasher.Finish();
            }
            bool published = false;
            AcquireSRWLockExclusive(&slot.lock);
            if (slot.armed.load(std::memory_order_acquire) && boneCount == slot.boneCount)
            {
                store(slot);
                slot.hasData = true;
                slot.poseHashValid = late;
                slot.poseHash = hash;
                // The detour writes under the shared lock, so every apply
                // counted after this one used the pose just stored.
                slot.appliesAtPublish = slot.applies.load(std::memory_order_acquire);
                slot.guardWritesThisTick.store(0, std::memory_order_relaxed); // once per tick
                published = true;
            }
            ReleaseSRWLockExclusive(&slot.lock);
            if (published)
            {
                slot.publishes.fetch_add(1, std::memory_order_relaxed);
                if (late)
                    slot.latePending.store(true, std::memory_order_release); // one late write per publication
            }
            return published;
        }
    }

    bool PublishOverride(
        std::uint32_t slotIndex,
        const BoneQts* locals,
        const BoneQts* objects,
        std::uint32_t boneCount)
    {
        if (slotIndex >= kMaxOverrides || !locals || !objects)
            return false;
        return PublishPose(slotIndex, locals, objects, boneCount, [&](Override& slot) {
            std::memcpy(slot.poseLocals.data(), locals, sizeof(BoneQts) * boneCount);
            std::memcpy(slot.poseObjects.data(), objects, sizeof(BoneQts) * boneCount);
        });
    }

    bool PublishOverrideSample(std::uint32_t slotIndex, PoseSample& sample, std::uint32_t boneCount)
    {
        if (slotIndex >= kMaxOverrides || sample.locals.size() < boneCount || sample.objects.size() < boneCount)
            return false;
        if (sample.locals.size() != boneCount || sample.objects.size() != boneCount)
            return PublishOverride(slotIndex, sample.locals.data(), sample.objects.data(), boneCount);
        return PublishPose(slotIndex, sample.locals.data(), sample.objects.data(), boneCount, [&](Override& slot) {
            // The slot's vectors hold boneCount entries (ArmOverride), as the
            // sample's: the detours see one whole pose or the other.
            slot.poseLocals.swap(sample.locals);
            slot.poseObjects.swap(sample.objects);
        });
    }

    bool PublishOverrideAlternate(
        std::uint32_t slotIndex,
        const BoneQts* locals,
        const BoneQts* objects,
        std::uint32_t boneCount)
    {
        if (slotIndex >= kMaxOverrides || !locals || !objects || boneCount == 0 || boneCount > 4096)
            return false;
        Override& slot = g_overrides[slotIndex];
        bool published = false;
        AcquireSRWLockExclusive(&slot.lock);
        if (slot.armed.load(std::memory_order_acquire) && boneCount != slot.boneCount)
        {
            if (slot.altBoneCount != boneCount)
            {
                slot.altBoneCount = boneCount;
                slot.altPose.assign(static_cast<std::size_t>(boneCount) * 2u, BoneQts{});
            }
            std::memcpy(slot.altPose.data(), locals, sizeof(BoneQts) * boneCount);
            std::memcpy(slot.altPose.data() + boneCount, objects, sizeof(BoneQts) * boneCount);
            slot.altHasData = true;
            published = true;
        }
        ReleaseSRWLockExclusive(&slot.lock);
        return published;
    }

    void ClearOverrideAlternate(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        AcquireSRWLockExclusive(&slot.lock);
        slot.altHasData = false;
        ReleaseSRWLockExclusive(&slot.lock);
    }

    void ApplyOverrideNow(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton)
            return;
        const std::size_t bytes = static_cast<std::size_t>(slot.boneCount) * 2u * sizeof(BoneQts);
        std::uint64_t hash = 0;
        if (slot.hasWrite.load(std::memory_order_acquire) && GuardedPoseHash(skeleton, bytes, hash))
        {
            if (hash == slot.lastWriteHash.load(std::memory_order_acquire))
                ++slot.scriptSurvived;
            else
                ++slot.scriptOverwritten;
        }
        // The written pose's hash from the publication, not a second
        // read of the buffer.
        if (ApplyOverrideFromDetour(slot, skeleton, &hash))
        {
            slot.lastWriteHash.store(hash, std::memory_order_release);
            slot.hasWrite.store(true, std::memory_order_release);
            ++slot.scriptWrites;
        }
    }

    // The weapon system's per-frame pass 0x117D49C writes the weapon object's
    // own bones (0x1182660 -> 0x2648C40 local of the turning part, 0x264967C
    // object matrices of the component bone pairs) after the script tick; the
    // slot's pose is written again right after that pass, on the thread that
    // runs it (see WeaponPassDetour in weapon_observer.cpp). No natives, the
    // slot checked under its lock (ApplyOverrideFromDetour).
    bool ApplyOverrideAfterWeaponPass(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return false;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton)
            return false;
        std::uint64_t hash = 0;
        if (!ApplyOverrideFromDetour(slot, skeleton, &hash))
            return false;
        slot.lastWriteHash.store(hash, std::memory_order_release);
        slot.hasWrite.store(true, std::memory_order_release);
        slot.weaponPassWrites.fetch_add(1, std::memory_order_relaxed);
        // The finalizers' late writes then only follow a publication.
        slot.preRenderSeen.store(true, std::memory_order_release);
        return true;
    }

    // The recording side of the weapon pass. A weapon object's skeleton is
    // never visited by the finalizer; a script read at the tick comes before
    // the weapon system's pass turns the cylinder (0x1182660 -> 0x2648C40
    // rotates the local it reads, so the base is posed again every frame),
    // and would miss the chamber turn. The detour copies the pose before the
    // pass, compares after it and samples the record there (as the finalizer
    // detour does for a body: detourCalls rises, so the script poll leaves
    // it).
    bool WeaponPassPoseBefore(std::uint32_t recordId, BoneQts* before, std::size_t capacity, std::size_t& count)
    {
        bool ok = false;
        count = 0;
        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (Record* record = FindArmedRecord(recordId))
        {
            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t boneCount = 0;
            if (GuardedReadSkeleton(record->skeleton, poseBuffer, world, boneCount) && poseBuffer &&
                boneCount == record->boneCount && before && static_cast<std::size_t>(boneCount) * 2u <= capacity)
            {
                count = static_cast<std::size_t>(boneCount) * 2u;
                ok = GuardedCopy(before, poseBuffer, count * sizeof(BoneQts));
                if (!ok)
                    count = 0;
            }
        }
        g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
        return ok;
    }

    bool WeaponPassPoseAfter(std::uint32_t recordId, std::size_t beforeCount, BoneQts* after, std::size_t capacity,
        bool capture)
    {
        bool compared = false;
        g_detoursInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (Record* record = FindArmedRecord(recordId))
        {
            if (beforeCount && after && beforeCount <= capacity)
            {
                std::uint8_t* poseBuffer = nullptr;
                std::uint8_t* world = nullptr;
                std::uint32_t boneCount = 0;
                if (GuardedReadSkeleton(record->skeleton, poseBuffer, world, boneCount) && poseBuffer &&
                    boneCount == record->boneCount && beforeCount == static_cast<std::size_t>(boneCount) * 2u)
                    compared = GuardedCopy(after, poseBuffer, beforeCount * sizeof(BoneQts));
            }
            if (capture)
            {
                record->hot.detourCalls.fetch_add(1, std::memory_order_relaxed);
                if (record->stallPollTicks != 0 || record->contentTimes)
                    record->hot.lastCallQpc.store(QpcNow(), std::memory_order_relaxed);
                CaptureFromDetour(*record, record->skeleton, false, false);
            }
        }
        g_detoursInFlight.fetch_sub(1, std::memory_order_seq_cst);
        return compared;
    }

    std::uint64_t OverrideApplyCount(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return 0;
        const Override& slot = g_overrides[slotIndex];
        return slot.applies.load(std::memory_order_acquire) + slot.altApplies.load(std::memory_order_acquire);
    }

    bool ApplyOverrideIfStale(std::uint32_t slotIndex, std::uint64_t appliesAtPublish)
    {
        if (slotIndex >= kMaxOverrides)
            return false;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton || OverrideApplyCount(slotIndex) != appliesAtPublish)
            return false;
        std::uint64_t hash = 0;
        if (!ApplyOverrideFromDetour(slot, skeleton, &hash)) // armed checked again under the lock
            return false;
        slot.lastWriteHash.store(hash, std::memory_order_release);
        slot.hasWrite.store(true, std::memory_order_release);
        slot.staleApplies.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    OverrideWriteCheck CheckOverrideWrite(std::uint32_t slotIndex)
    {
        OverrideWriteCheck result;
        if (slotIndex >= kMaxOverrides)
            return result;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton)
            return result;
        static std::vector<BoneQts> now; // the buffer as the frame left it
        AcquireSRWLockShared(&slot.lock);
        const std::uint64_t before = slot.applies.load(std::memory_order_acquire);
        if (slot.hasData && before > slot.appliesAtPublish)
        {
            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t liveBones = 0;
            now.resize(static_cast<std::size_t>(slot.boneCount) * 2u);
            const bool ok = GuardedReadSkeleton(skeleton, poseBuffer, world, liveBones) && poseBuffer &&
                liveBones == slot.boneCount &&
                GuardedCopy(now.data(), poseBuffer, now.size() * sizeof(BoneQts)) &&
                slot.applies.load(std::memory_order_acquire) == before;
            if (ok)
            {
                result.checked = true;
                result.boneCount = slot.boneCount;
                // The published pose in one array for the compare (every few seconds
                // per puppet, not per tick).
                static std::vector<BoneQts> published;
                published.assign(slot.poseLocals.begin(), slot.poseLocals.end());
                published.insert(published.end(), slot.poseObjects.begin(), slot.poseObjects.end());
                result.change = CompareWrittenPose(published.data(), now.data(), slot.boneCount,
                    kLateTranslation, kLateRotation);
                ++slot.writeChecks;
                if (result.change.localBones + result.change.objectBones > 0)
                    ++slot.writeChanged;
            }
        }
        ReleaseSRWLockShared(&slot.lock);
        return result;
    }

    // The drawn check of a script-written slot (an attached object the
    // finalizer never visits). Script thread, at the start of the tick,
    // before the publish: the buffer then holds what the last frame drew
    // (hypothesis: the update ends before the next script tick) and the slot
    // still the pose written last tick. Read only; counted in
    // writeChecks/writeChanged by NoteOverrideDrawn, for
    // POSE_V2_PLAYBACK_DELETE.
    bool ReadOverrideDrawn(std::uint32_t slotIndex, std::vector<BoneQts>& published, std::vector<BoneQts>& drawn)
    {
        published.clear();
        drawn.clear();
        if (slotIndex >= kMaxOverrides)
            return false;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton || !slot.hasWrite.load(std::memory_order_acquire))
            return false;
        bool ok = false;
        AcquireSRWLockShared(&slot.lock);
        if (slot.hasData && slot.boneCount && slot.poseLocals.size() == slot.boneCount)
        {
            // Locals then object matrices, as the engine buffer holds them
            // (only the locals when the slot has no objects published).
            const bool objects = slot.poseObjects.size() == slot.boneCount;
            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t liveBones = 0;
            drawn.resize(static_cast<std::size_t>(slot.boneCount) * (objects ? 2u : 1u));
            ok = GuardedReadSkeleton(skeleton, poseBuffer, world, liveBones) && poseBuffer &&
                liveBones == slot.boneCount &&
                GuardedCopy(drawn.data(), poseBuffer, drawn.size() * sizeof(BoneQts));
            if (ok)
            {
                published.assign(slot.poseLocals.begin(), slot.poseLocals.end());
                if (objects)
                    published.insert(published.end(), slot.poseObjects.begin(), slot.poseObjects.end());
            }
        }
        ReleaseSRWLockShared(&slot.lock);
        if (!ok)
            drawn.clear();
        return ok;
    }

    void NoteOverrideDrawn(std::uint32_t slotIndex, bool changed)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        // Script thread only (CheckOverrideWrite is the other writer, also
        // the script thread), like the script* counters.
        ++slot.writeChecks;
        if (changed)
            ++slot.writeChanged;
    }

    void SetOverrideRebuildGuard(std::uint32_t slotIndex, bool on)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        if (slot.rebuildGuard.exchange(on, std::memory_order_acq_rel) != on)
        {
            if (on)
                g_guardedSlots.fetch_add(1, std::memory_order_acq_rel);
            else
                g_guardedSlots.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void SetOverridePrefixBones(std::uint32_t slotIndex, std::uint32_t bodyBones)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        g_overrides[slotIndex].prefixBones.store(bodyBones, std::memory_order_release);
    }

    void SetOverrideLate(std::uint32_t slotIndex, bool late)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        g_overrides[slotIndex].late.store(late, std::memory_order_release);
        SetLateBit(slotIndex, late);
    }

    void DisarmOverride(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        g_armedSkeleton[slotIndex].store(nullptr, std::memory_order_release);
        slot.armed.store(nullptr, std::memory_order_release);
        slot.late.store(false, std::memory_order_release);
        SetLateBit(slotIndex, false);
        slot.latePending.store(false, std::memory_order_release);
        slot.prefixBones.store(0, std::memory_order_release);
        if (slot.rebuildGuard.exchange(false, std::memory_order_acq_rel))
            g_guardedSlots.fetch_sub(1, std::memory_order_acq_rel);
        // Wait out any detour still inside the shared section.
        AcquireSRWLockExclusive(&slot.lock);
        slot.hasData = false;
        slot.altHasData = false;
        ReleaseSRWLockExclusive(&slot.lock);
    }

    void DisarmOverrides()
    {
        for (std::uint32_t slot = 0; slot < kMaxOverrides; ++slot)
            DisarmOverride(slot);
        // Every slot is free (each disarm waited out the detours inside its
        // slot), the lookups scan nothing until the next ArmOverride raises it.
        g_overrideScanEnd.store(0, std::memory_order_seq_cst);
    }

    OverrideStats OverrideSnapshot(std::uint32_t slotIndex)
    {
        OverrideStats stats;
        if (slotIndex >= kMaxOverrides)
            return stats;
        const Override& slot = g_overrides[slotIndex];
        stats.applies = slot.applies.load();
        stats.altApplies = slot.altApplies.load();
        stats.boneCountMismatch = slot.boneCountMismatch.load();
        stats.writeFaults = slot.writeFaults.load();
        stats.publishes = slot.publishes.load();
        stats.scriptWrites = slot.scriptWrites;
        stats.scriptSurvived = slot.scriptSurvived;
        stats.scriptOverwritten = slot.scriptOverwritten;
        stats.lateWrites = slot.lateWrites.load();
        stats.preRenderWrites = slot.preRenderWrites.load();
        stats.skippedOriginal = slot.skippedOriginal.load();
        stats.writeChecks = slot.writeChecks;
        stats.writeChanged = slot.writeChanged;
        stats.guardApplies = slot.guardApplies.load();
        stats.prefixApplies = slot.prefixApplies.load();
        stats.staleApplies = slot.staleApplies.load();
        stats.weaponPassWrites = slot.weaponPassWrites.load();
        return stats;
    }

    bool AcquireFinalizer(std::string& error)
    {
        return TakeFinalizer(error);
    }

    namespace
    {
        // Removes the detour once nothing is armed: new calls go to the
        // original as soon as the jump is gone, calls already inside finish
        // on the trampoline, which is freed only after they drained.
        bool RemoveFinalizerHook()
        {
            if (g_armedCount.load(std::memory_order_acquire) != 0)
                return false;
            for (const Override& slot : g_overrides)
            {
                if (slot.armed.load(std::memory_order_acquire))
                    return false;
            }
            if (g_hookInstalled && g_hookTarget)
            {
                MH_DisableHook(g_hookTarget);
                // The whole detour, not only its copy, counts as in flight (a call
                // inside the trampoline or an override return must be waited for).
                // Twice around the grace for a call that jumped in just before the
                // jump went away; still busy: the hook goes back on and the removal
                // waits for the next tick. The count is the sum of the stripes,
                // which is enough here for the reason given in module_lifetime.h
                // (StripedCounter::Sum).
                const ULONGLONG deadline = GetTickCount64() + 500;
                bool drained = false;
                for (int pass = 0; pass < 2; ++pass)
                {
                    while (g_finalizerCalls.Sum() != 0 && GetTickCount64() < deadline)
                        Sleep(1);
                    drained = g_finalizerCalls.Sum() == 0;
                    if (!drained)
                        break;
                    if (pass == 0)
                        Sleep(20);
                }
                if (!drained)
                {
                    MH_EnableHook(g_hookTarget);
                    return false;
                }
                MH_RemoveHook(g_hookTarget);
                g_hookInstalled = false;
                g_hookTarget = nullptr;
                g_original.store(nullptr, std::memory_order_release);
            }
            return true;
        }
    }

    bool WarmUpFinalizer(std::string& error)
    {
        // The load is where the pose buffer reserve starts filling, off the
        // script thread, before the first recording's first scan.
        StartReserveFill();
        return EnsureHook(error);
    }

    bool WarmUpVehiclePreRender(std::string& error)
    {
        if (g_vehicleHookTarget)
            return true;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        std::uint8_t* target = base + kVehiclePreRenderRva;
        std::uint8_t prologue[sizeof(kVehiclePreRenderSignature)] = {};
        if (!GuardedCopy(prologue, target, sizeof(prologue)) ||
            std::memcmp(prologue, kVehiclePreRenderSignature, sizeof(prologue)) != 0)
        {
            error = "vehicle_prerender_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&VehiclePreRenderDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_vehicleOriginal.store(reinterpret_cast<VehiclePreRenderFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_vehicleOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_vehicleHookTarget = target;
        return true;
    }

    void ReleaseFinalizer()
    {
        // The detour and the slot stay; ServiceFinalizer gives them to the
        // Pose Editor when it asks.
    }

    FinalizerEvent ServiceFinalizer(bool busy, std::string& error)
    {
        if (g_ownerHeld)
        {
            if (busy || g_owner.Wanted() != rdr2::finalizer_owner::kPoseEditor)
                return FinalizerEvent::None;
            if (!RemoveFinalizerHook())
                return FinalizerEvent::None;
            g_owner.Release(rdr2::finalizer_owner::kReplayPoC);
            g_ownerHeld = false;
            return FinalizerEvent::Yielded;
        }
        // Claim it back once the Pose Editor let it go and nobody waits.
        if (!g_owner.Open() || g_owner.Holder() != rdr2::finalizer_owner::kFree ||
            g_owner.Wanted() != rdr2::finalizer_owner::kFree)
        {
            return FinalizerEvent::None;
        }
        if (!EnsureHook(error))
            return FinalizerEvent::Failed;
        return FinalizerEvent::Reclaimed;
    }

    bool FinalizerWanted()
    {
        return g_owner.Open() && g_owner.Wanted() == rdr2::finalizer_owner::kPoseEditor;
    }

    void Shutdown()
    {
        DisarmAllRecords();
        DisarmOverrides();
        if (g_hookInstalled && g_hookTarget)
        {
            MH_DisableHook(g_hookTarget);
            MH_RemoveHook(g_hookTarget);
            g_hookInstalled = false;
        }
        if (g_vehicleHookTarget)
        {
            MH_DisableHook(g_vehicleHookTarget);
            MH_RemoveHook(g_vehicleHookTarget);
            g_vehicleHookTarget = nullptr;
        }
        StopForUnload(g_worker, g_workerRunning, g_wake, g_workerExited, "pose_worker");
        StopForUnload(g_discoveryThread, g_discoveryRunning, g_discoveryWake, g_discoveryExited, "pose_discovery");
        // The reserve's load-time fill, if it still runs (it stops between two
        // buffers).
        StopForUnload(g_reserveThread, g_reserveRunning, nullptr, g_reserveExited, "pose_reserve");
        g_original.store(nullptr, std::memory_order_release);
        if (g_ownerHeld)
            g_owner.Release(rdr2::finalizer_owner::kReplayPoC);
        g_ownerHeld = false;
        g_owner.Close();
        if (g_wake)
        {
            CloseHandle(g_wake);
            g_wake = nullptr;
        }
        if (g_discoveryWake)
        {
            CloseHandle(g_discoveryWake);
            g_discoveryWake = nullptr;
        }
    }
}
