#include "pose_capture_internal.h"

namespace replay::pose::capture
{

    namespace
    {
        constexpr std::size_t kVehicleFragment = 0x938; // entity -> fragment (virtual +0xB8)
        constexpr std::size_t kFragmentOwner = 0x128;
        constexpr std::size_t kOwnerSkeleton = 0x160;

        // Skeleton fields used by the resolver.
        constexpr std::size_t kSkeletonPoseBuffer = 0x00;
        constexpr std::size_t kSkeletonWorldMatrix = 0x08; // parent (world) matrix pointer
        constexpr std::size_t kSkeletonBoneCount = 0x108;
    }

    namespace
    {
        static_assert(alignof(Record) == 64, "Record::hot is on its own cache lines (item 2b)");
    }

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

    namespace
    {
        struct FinalizerCall
        {
            FinalizerCall() noexcept : stripe(replay::lifetime::ThreadStripe()) { g_finalizerCalls.Enter(stripe); }
            ~FinalizerCall() noexcept { g_finalizerCalls.Leave(stripe); }
            FinalizerCall(const FinalizerCall&) = delete;
            FinalizerCall& operator=(const FinalizerCall&) = delete;
            const std::uint32_t stripe;
        };
    }
        // Fast path for the finalizer calls of every other skeleton.
        std::atomic<std::uint32_t> g_armedCount{0};
        // Records sampled at the end of the frame (lateCapture).
        std::atomic<std::uint32_t> g_lateRecords{0};

    namespace
    {
        // End-of-frame check pacing and tolerances (1 cm, 0.01 rad: the
        // finalizer's composition agrees with ours to 1e-7). kLateCheckMs and
        // kLateCheckChangedMs are in pose_capture_rules.h.
        constexpr std::uint32_t kLateCleanChecksToLeave = 8; // 2 s at 4 Hz
        static_assert(sizeof(DetourStripe) == 64, "one cache line per stripe");
    }
        DetourStripe g_detourStripes[replay::lifetime::kStripes];
        std::atomic<std::uint64_t> g_pressureWakes{0};
        double g_workerBusyUs = 0.0;      // guarded by g_mutex
        double g_workerDiscoveryUs = 0.0; // guarded by g_mutex
        std::uint64_t g_pendingPeak = 0;  // guarded by g_mutex
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

    namespace
    {
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
    }
        std::size_t g_ringPoolBytes = 0;

    namespace
    {
        constexpr std::size_t kRingPoolMaxBytes = std::size_t{96} << 20;
    }
        std::atomic<std::uint64_t> g_ringPoolHits{0};
        std::atomic<std::uint64_t> g_ringPoolMisses{0};
        std::atomic<std::uint64_t> g_startTicksTotal{0};
        std::atomic<std::uint64_t> g_startTicksMax{0};
        std::atomic<std::uint64_t> g_starts{0};

    namespace
    {
        std::vector<std::vector<BoneQts>> g_poseReserve;
    }
        std::atomic<std::size_t> g_poseReserveSize{0};
        // Buffers Start took from the reserve, and buffers it had to allocate
        // (reserve empty, or a skeleton above kReserveBones or under half of it).
        std::atomic<std::uint64_t> g_reserveHits{0};
        std::atomic<std::uint64_t> g_reserveMisses{0};

    namespace
    {

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
        // through the full CompareLatePose) is in pose_capture_rules.h.
        // The pose fidelity judge's content times (SetContentTimes), copied into
        // each record by Start.
        std::atomic<bool> g_contentTimes{false};

        Override g_overrides[kMaxOverrides];
        std::atomic<std::uint32_t> g_guardedSlots{0}; // slots with rebuildGuard on

    namespace
    {
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
    }
        std::atomic<std::uint8_t*> g_armedSkeleton[kMaxOverrides];
        std::atomic<std::uint32_t> g_overrideScanEnd{0};

    namespace
    {

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
    }

        void SetLateBit(std::uint32_t slotIndex, bool late)
        {
            g_lateMask.Set(slotIndex, late); // the slot's word
        }

    namespace
    {

        // Called with the slot lock held (either mode).
        std::uint64_t PublishedPoseHash(const std::vector<BoneQts>& locals, const std::vector<BoneQts>& objects)
        {
            PoseHasher hasher;
            hasher.Add(locals.data(), locals.size() * sizeof(BoneQts));
            hasher.Add(objects.data(), objects.size() * sizeof(BoneQts));
            return hasher.Finish();
        }
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

    namespace
    {

        // Called with g_pauseMutex held. O(1) at or after the last span's start,
        // a binary search before it (pause_spans.h).
        std::uint64_t PausedTicksBeforeLocked(std::uint64_t qpc)
        {
            return g_pauses.PausedBefore(qpc);
        }
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
        std::deque<CachedLayout> g_layoutCache;

    namespace
    {
        constexpr std::size_t kLayoutCacheSize = 24;
    }

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

    namespace
    {

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

    namespace
    {

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
        // bind pose's lane (its pcmpeqd against all-ones). Translation
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
        // the engine applies it a frame or two later (the visibility test in the
        // next pre-render pass, then the expressions' mask on the creature's next
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
    }
        SkeletonInfoSlot g_skeletonInfo[kSkeletonInfoSlots];
        std::atomic<std::uint32_t> g_skeletonInfoWanted{0};

    namespace
    {

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

    namespace
    {

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
    }
        std::atomic<VehiclePreRenderFn> g_vehicleOriginal{nullptr};
        void* g_vehicleHookTarget = nullptr;

    namespace
    {

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

    namespace
    {

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

            auto* target = reinterpret_cast<std::uint8_t*>(sig::Find(tpose::kPoseFinalizer));
            // Its bytes again now: the Pose Editor detours the same function and
            // takes its detour off before it frees the slot (its pattern leaves
            // the detour's bytes out, so the jump is checked apart).
            if (!target || !sig::MatchesAt(reinterpret_cast<std::uintptr_t>(target), tpose::kPoseFinalizer.pattern) ||
                sig::StartsWithJump(reinterpret_cast<std::uintptr_t>(target)))
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

    namespace
    {

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
                    // end of the frame, else 1 s (NextLateCheckMs).
                    const std::uint64_t waitMs = NextLateCheckMs(result.changed,
                        record.lateCapture.load(std::memory_order_relaxed));
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

    // The recording side of the weapon pass. A weapon object's skeleton is
    // never visited by the finalizer; a script read at the tick comes before
    // the weapon system's pass turns the cylinder (it rotates the local it
    // reads, so the base is posed again every frame),
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
}
