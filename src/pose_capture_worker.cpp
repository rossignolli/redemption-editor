// pose_capture_worker.cpp: the worker side: the engine layout check, frame composition, discovery,
// the rings' drain and release, the pose reserve, the worker threads' start and stop, the records' index
// and the snapshot.
// Split from pose_capture.cpp (moved code); shared internals in pose_capture_internal.h.

#include "pose_capture_internal.h"
#include "pose_deadband_smooth.h"

#include <algorithm>

namespace replay::pose::capture
{

    namespace
    {
        // A track begun at maximum detail (64 bit rotations on every bone)
        // keeps them when its layout changes mid-recording.
        void KeepPrecision(const PoseTrack& track, SkeletonLayout& layout)
        {
            const std::vector<std::uint8_t>& current = track.Layout().highPrecision;
            if (!current.empty() && std::all_of(current.begin(), current.end(), [](std::uint8_t v) { return v != 0; }))
                layout.highPrecision.assign(layout.boneCount, std::uint8_t{1});
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

    namespace
    {

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
                    KeepPrecision(*record.track, updated);
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
                        KeepPrecision(*reparent->track, reparentLayout);
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
                    // The codec of PoseDetail (pose_deadband_smooth.h): the
                    // old coarse deadband played as a trembling on nearly
                    // still bones. The record's own layout stays as found
                    // (RememberLayout's), the track's gets the precision.
                    const smooth::CodecSetting codec = smooth::CodecFor(smooth::CurrentDetail());
                    config.rotationEpsilon = codec.rotationEpsilon;
                    config.translationEpsilon = codec.translationEpsilon;
                    SkeletonLayout trackLayout = layout;
                    smooth::ApplyPrecision(codec, trackLayout);
                    record->track->Begin(trackLayout, config);
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

    namespace
    {

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

    namespace
    {

        void WorkerMain()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (g_workerRunning.load(std::memory_order_acquire))
            {
                WaitForSingleObject(g_wake, kWorkerPollMs);
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
