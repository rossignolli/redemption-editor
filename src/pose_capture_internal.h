// pose_capture_internal.h: the state and helpers the pose_capture*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "pose_capture.h"
#include "game_sig.h"
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

        namespace sig = replay::sig;

        // The pose finalizer is tpose_check.h kPoseFinalizer (the bind pose
        // reads its loop). Refuse to hook if its prologue changes.

        // The vehicle pre-render base. The vehicle classes' pre-render first
        // write every wheel's local from the wheel state (a frozen puppet's
        // wheel does not turn) and then reach this base, so a write after it
        // is the last one of the frame.
        const sig::Signature kVehiclePreRender{"pose_vehicle_prerender",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 57 48 8D A8 B8 FE FF FF 48 81 EC 08 02 00 00 45 33 ED",
            0x15AD298};

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
            // kDefaultStallPollMs in QPC ticks, fixed while the record lives (read
            // by the detour after the g_armed load that published the record).
            std::uint64_t stallPollTicks = 0;
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
        extern std::vector<std::unique_ptr<Record>> g_records;
        extern std::unordered_map<std::uint32_t, Record*> g_recordIndex;
        extern std::vector<Record*> g_drainList;
        extern std::unordered_set<std::uint32_t> g_prunedIds;
        extern std::uint64_t g_prunedRecords;
        extern std::atomic<Record*> g_armed[kLiveTargetSlots];
        extern std::atomic<std::uint8_t*> g_armedRecordSkeleton[kLiveTargetSlots];
        extern std::atomic<std::uint32_t> g_armedRecordId[kLiveTargetSlots];
        extern std::atomic<std::uint32_t> g_armedScanEnd;
        extern std::atomic<std::uint32_t> g_detoursInFlight;
        extern replay::lifetime::StripedCounter g_finalizerCalls;
        extern std::atomic<std::uint32_t> g_armedCount;
        extern std::atomic<std::uint32_t> g_lateRecords;
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
        extern DetourStripe g_detourStripes[replay::lifetime::kStripes];
        extern std::atomic<std::uint64_t> g_pressureWakes;
        extern double g_workerBusyUs;
        extern double g_workerDiscoveryUs;
        extern std::uint64_t g_pendingPeak;

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
        extern WorkerPhases g_workerPhases;
        extern std::atomic<std::uint64_t> g_workerCpuDone;
        extern std::atomic<std::uint64_t> g_scriptLockWaitTicks;
        extern std::atomic<std::uint64_t> g_scriptLockWaits;
        extern std::atomic<std::uint64_t> g_scriptLockWaitMaxTicks;
        extern std::atomic<std::uint64_t> g_scriptLockSiteMaxTicks[kScriptLockSites];
        extern std::atomic<std::uint64_t> g_discoveryHoldMaxTicks;
        extern std::size_t g_ringPoolBytes;
        extern std::atomic<std::uint64_t> g_ringPoolHits;
        extern std::atomic<std::uint64_t> g_ringPoolMisses;
        extern std::atomic<std::uint64_t> g_startTicksTotal;
        extern std::atomic<std::uint64_t> g_startTicksMax;
        extern std::atomic<std::uint64_t> g_starts;
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
        extern std::atomic<std::size_t> g_poseReserveSize;
        extern std::atomic<std::uint64_t> g_reserveHits;
        extern std::atomic<std::uint64_t> g_reserveMisses;
        void PoolRing(std::unique_ptr<Slot[]>& ring, std::vector<BoneQts>& late, std::uint32_t boneCount);
        std::unique_ptr<Slot[]> TakePooledRing(std::uint32_t boneCount, std::vector<BoneQts>& late);
        std::vector<BoneQts> MakeReserveBuffer();
        bool GiveReserveBuffer(std::vector<BoneQts>& buffer);
        std::size_t TakeReserveBuffers(std::vector<BoneQts>* out, std::size_t wanted);
        extern std::atomic<bool> g_contentTimes;

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
        extern Override g_overrides[kMaxOverrides];
        extern std::atomic<std::uint32_t> g_guardedSlots;
        extern std::atomic<std::uint8_t*> g_armedSkeleton[kMaxOverrides];
        extern std::atomic<std::uint32_t> g_overrideScanEnd;
        void SetLateBit(std::uint32_t slotIndex, bool late);
        bool GuardedPoseHash(std::uint8_t* skeleton, std::size_t bytes, std::uint64_t& hash);
        extern std::atomic<PoseFinalizerFn> g_original;
        extern std::uint64_t g_qpcFrequency;
        extern std::uint64_t g_startQpc;
        extern std::mutex g_pauseMutex;
        extern PauseSpans g_pauses;
        extern std::atomic<bool> g_clockPaused;
        extern std::atomic<std::uint32_t> g_cutSerial;
        extern std::uint64_t g_lastStampedQpc;
        extern std::uint64_t g_lastTickQpc;
        std::uint64_t PausedTicksBefore(std::uint64_t qpc);
        std::uint32_t RecordMsAtQpc(std::uint64_t qpc);
        std::uint32_t StampFrameMs(std::uint64_t qpc);
        extern std::mutex g_mutex;
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
        extern RawThread g_worker;
        extern std::atomic<bool> g_workerExited;
        extern std::atomic<bool> g_discoveryExited;
        extern HANDLE g_wake;
        extern std::atomic<bool> g_workerRunning;
        extern RawThread g_discoveryThread;
        extern HANDLE g_discoveryWake;
        extern std::atomic<bool> g_discoveryRunning;
        extern std::deque<Record*> g_discoveryQueue;
        extern std::deque<Record*> g_reparentQueue;
        extern RawThread g_reserveThread;
        extern std::atomic<bool> g_reserveRunning;
        extern std::atomic<bool> g_reserveExited;

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
        extern std::deque<CachedLayout> g_layoutCache;
        void RememberLayout(const SkeletonLayout& layout);
        extern void* g_hookTarget;
        extern bool g_hookInstalled;
        void AtomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value);
        std::uint64_t QpcNow();
        double TicksToUs(std::uint64_t ticks);

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
        std::uint64_t ThreadCpu(HANDLE thread);
        bool GuardedCopy(void* destination, const void* source, std::size_t size);
        bool GuardedReadSkeleton(
            std::uint8_t* skeleton,
            std::uint8_t*& poseBuffer,
            std::uint8_t*& world,
            std::uint32_t& boneCount);

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
        extern SkeletonInfoSlot g_skeletonInfo[kSkeletonInfoSlots];
        extern std::atomic<std::uint32_t> g_skeletonInfoWanted;
        bool ApplyOverrideFromDetour(Override& slot, std::uint8_t* skeleton, std::uint64_t* writeHash);

        using VehiclePreRenderFn = std::uint64_t(__fastcall*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
        extern std::atomic<VehiclePreRenderFn> g_vehicleOriginal;
        extern void* g_vehicleHookTarget;
        std::uint64_t __fastcall VehiclePreRenderDetour(void* vehicle, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4);
        extern rdr2::finalizer_owner::Slot g_owner;
        extern bool g_ownerHeld;
        bool TakeFinalizer(std::string& error);
        bool EnsureHook(std::string& error);
        bool IsDistinct(const Frame& a, const Frame& b, std::uint32_t boneCount);
        void CompareEngineParentTable(Record& record);
        void CheckEngineParents(Record& record, const BoneQts* locals, const BoneQts* objects);
        bool FlushHeldFrames();
        std::uint32_t DrainReady(Record& record);
        void StartReserveFill();
        void StartWorker();
        void StopDiscovery();
        void StopWorker();
        void StopForUnload(RawThread& thread, std::atomic<bool>& running, HANDLE wake, std::atomic<bool>& exited,
            const char* what);
        void ClearArmedHints(std::size_t index);
        void DisarmAllRecords();
        Record* FindRecord(std::uint32_t recordId);
        void RebuildRecordIndex();
        Stats SnapshotLocked(const Record& record);
        Record* FindArmedRecord(std::uint32_t recordId);
}
