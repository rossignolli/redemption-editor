// pose_capture_recording.cpp: a recording's public side: begin, the clock and its pauses, start,
// stop, the tracks and their signatures, the skeleton info, drop, export and import.
// Split from pose_capture.cpp (moved code); shared internals in pose_capture_internal.h.

#include "pose_capture_internal.h"

namespace replay::pose::capture
{

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
            std::lock_guard<std::mutex> pauseLock(g_pauseMutex);
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
        created->stallPollTicks = static_cast<std::uint64_t>(kDefaultStallPollMs) * g_qpcFrequency / 1000u;
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
}
