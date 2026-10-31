// replay_playback_watch.cpp: the watches: the settle state, the puppet watch, blinks, jerks.
// Split from replay_playback.cpp (moved code); shared internals in replay_playback_internal.h.

#include "replay_playback_internal.h"

namespace replay::playback
{
    namespace
    {
    std::vector<replay::jerk::Body> g_poseV2JerkBodies;
    constexpr std::uint32_t kPoseV2JerkLogCap = 200;
    }

    // What the seek settle waits for, after this tick's work: bodies alive at
    // the playhead still being built (Loading, Dressing, or created and not
    // placed yet), those whose model is not loaded, and those the create budget
    // left waiting with their model loaded (the hold in normal play).
    void UpdatePoseV2SettleState(std::uint32_t timeMs)
    {
        (void)timeMs;
        replay::playback::SettleState state;
        const ULONGLONG now = GetTickCount64();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind == replay::entity::EntityKind::Weapon ||
                puppet.packet.kind == replay::entity::EntityKind::Prop)
                continue;
            ++state.puppets;
            const bool loading = puppet.state == replay::playback::PuppetState::Loading && !puppet.waitingForParent &&
                STREAMING::IS_MODEL_VALID(puppet.packet.modelHash); // an unknown model only fails, never builds
            const bool building = loading || puppet.state == replay::playback::PuppetState::Dressing ||
                (puppet.state == replay::playback::PuppetState::Ready && puppet.visibleState < 0);
            if (!building)
                continue;
            ++state.building;
            if (!loading)
                continue;
            if (STREAMING::HAS_MODEL_LOADED(puppet.packet.modelHash))
            {
                // Only bodies the budget alone keeps back: not a vehicle
                // waiting for its prop sets, not an entity waiting for a
                // later pose segment (wait_segment).
                const bool body = puppet.packet.kind == replay::entity::EntityKind::Ped ||
                    puppet.packet.kind == replay::entity::EntityKind::Horse;
                const bool waitsSegment = std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(),
                    puppet.packet.id.Packed()) != g_poseV2WaitSegmentLogged.end();
                if (body && !waitsSegment && !puppet.clone && now >= puppet.nextCreateAt)
                    ++state.dueWithModel;
            }
            else
                ++state.modelsMissing;
        }
        g_poseV2SettleState = state;
    }
    namespace
    {

    // The missing puppet detector (puppet_watch.h). In normal play, every
    // checkMs: the bodies alive at the playhead within radiusM of the camera
    // whose puppet is not shown, and why. PUPPET_MISSING when one stays so for
    // thresholdMs, PUPPET_MISSING_END when it shows, and PUPPET_WATCH_SUMMARY
    // at the end. Also: PUPPET_POP_IN, a body shown for the first time close to
    // the camera right after it entered the recording (the recording found it
    // late: entity_discovery.h), and PUPPET_INTERIOR, a shown body whose engine
    // interior is not the one at its position (a suspect for bodies unseen from
    // inside a building; evidence only).
    replay::watch::PuppetWatch g_poseV2Watch;
    }
    bool g_poseV2WatchHeld = false;     // the seek settle or the load wait (script.cpp)
    namespace
    {
    bool g_poseV2WatchActive = false;
    ULONGLONG g_poseV2WatchNextAt = 0;
    ULONGLONG g_poseV2InteriorNextAt = 0;
    std::uint32_t g_poseV2WatchLastPlayhead = 0;
    std::uint32_t g_poseV2WatchStartMs = 0;
    std::uint32_t g_poseV2WatchLogs = 0;
    std::uint32_t g_poseV2WatchSuspends = 0;
    std::uint32_t g_poseV2PopIns = 0;
    std::uint32_t g_poseV2PopInsOnScreen = 0;
    std::uint32_t g_poseV2PopInLogs = 0;
    std::vector<std::uint32_t> g_poseV2PopInJudged;
    }
    // Ready puppets whose record gave no sample at a playhead where the
    // entity's recording has one (the record the puppet plays is not the one
    // the clip has, e.g. a wagon kept on its empty create record). One line per
    // entity, counted in the summary.
    std::vector<std::uint32_t> g_poseV2RecordUnsampled;
    namespace
    {
    // Interior: consecutive mismatching checks per id, and the ids logged.
    std::unordered_map<std::uint32_t, std::uint32_t> g_poseV2InteriorStreak;
    std::vector<std::uint32_t> g_poseV2InteriorLogged;
    std::vector<replay::watch::Event> g_poseV2WatchEvents;

    void LogPoseV2WatchEvents(std::uint32_t timeMs)
    {
        for (const replay::watch::Event& event : g_poseV2WatchEvents)
        {
            if (g_poseV2WatchLogs >= 200)
                break;
            ++g_poseV2WatchLogs;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(event.id);
            const replay::entity::ReplayId id = replay::entity::ReplayId::FromPacked(event.id);
            std::ostream& log = Ctx().log;
            log << (event.begin ? "PUPPET_MISSING" : "PUPPET_MISSING_END") << ",id=" << replay::entity::IdText(id)
                << ",kind=" << (create ? replay::entity::KindText(create->kind) : "?") << ",model=0x" << std::hex
                << (create ? create->modelHash : 0u) << std::dec << ",reason=" << replay::watch::ReasonText(event.reason)
                << ",camera_m=" << static_cast<int>(event.distanceM) << ",missing_ms=" << event.missingMs;
            if (!event.begin)
                log << ",suspended=" << (event.suspended ? 1 : 0);
            if (create)
                log << ",entry_ms=" << create->timeMs;
            log << ",playhead_ms=" << timeMs << '\n';
        }
        g_poseV2WatchEvents.clear();
    }

    void SuspendPoseV2Watch(std::uint32_t timeMs)
    {
        g_poseV2Watch.Suspend(GetTickCount64(), g_poseV2WatchEvents);
        LogPoseV2WatchEvents(timeMs);
        g_poseV2InteriorStreak.clear();
        ++g_poseV2WatchSuspends;
    }

    replay::watch::Reason PoseV2MissingReason(const replay::playback::Puppet* puppet)
    {
        using replay::watch::Reason;
        if (!puppet)
            return Reason::NoPuppet;
        switch (puppet->state)
        {
        case replay::playback::PuppetState::Loading:
            if (!STREAMING::HAS_MODEL_LOADED(puppet->packet.modelHash))
                return Reason::ModelLoading;
            if (std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(), puppet->packet.id.Packed()) !=
                g_poseV2WaitSegmentLogged.end())
                return Reason::WaitSegment;
            return Reason::CreateWait;
        case replay::playback::PuppetState::Dressing:
            return Reason::Dressing;
        case replay::playback::PuppetState::Failed:
            return Reason::Failed;
        default:
            break;
        }
        if (!puppet->clone || !replay::entities::Exists(puppet->clone))
            return Reason::NoPuppet;
        if (puppet->visibleState == 1)
            return Reason::Invisible;
        const bool body = puppet->packet.kind == replay::entity::EntityKind::Ped ||
            puppet->packet.kind == replay::entity::EntityKind::Horse;
        // A body that could not be armed stays hidden (arm_refused).
        if (body && puppet->overrideSlot < 0)
            return puppet->armFailureLogged ? Reason::ArmRefused : Reason::Unposed;
        return Reason::NoPose;
    }
    }

    void UpdatePoseV2PuppetWatch(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.puppet_watch");
        const bool active = PoseV2NormalPlay() && !g_poseV2WatchHeld && !g_poseV2CheapScrub;
        // A jump inside normal play (a marker, a loop) is a new start too.
        const bool jumped = timeMs < g_poseV2WatchLastPlayhead || timeMs > g_poseV2WatchLastPlayhead + 1500;
        g_poseV2WatchLastPlayhead = timeMs;
        if (!active || jumped)
        {
            if (g_poseV2WatchActive)
                SuspendPoseV2Watch(timeMs);
            g_poseV2WatchActive = active && !jumped;
            if (!active)
                return;
        }
        g_poseV2WatchActive = true;
        const ULONGLONG now = GetTickCount64();
        const replay::watch::Config& config = g_poseV2Watch.Settings();
        if (now < g_poseV2WatchNextAt)
            return;
        g_poseV2WatchNextAt = now + config.checkMs;
        const bool interiorCheck = now >= g_poseV2InteriorNextAt;
        if (interiorCheck)
            g_poseV2InteriorNextAt = now + 1000;
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        static std::vector<replay::watch::Observation> missing;
        missing.clear();
        std::uint32_t recordedSamples = 0;
        constexpr std::uint32_t kMaxRecordedSamples = 16; // a full pose decode each
        for (const replay::entity::CreatePacket& packet : g_poseV2Alive)
        {
            if (packet.kind != replay::entity::EntityKind::Ped && packet.kind != replay::entity::EntityKind::Horse &&
                packet.kind != replay::entity::EntityKind::Vehicle && packet.kind != replay::entity::EntityKind::TrainCar)
                continue;
            const replay::playback::Puppet* puppet = FindPoseV2Puppet(packet.id);
            const bool exists = puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                replay::entities::Exists(puppet->clone);
            const bool shown = exists && puppet->visibleState == 1 && ENTITY::IS_ENTITY_VISIBLE(puppet->clone);
            Vector3 at{};
            bool haveAt = false;
            if (exists && puppet->visibleState == 1)
            {
                at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
                haveAt = true;
            }
            else if (puppet && puppet->hasWorld)
            {
                at.x = puppet->lastWorld.position.x;
                at.y = puppet->lastWorld.position.y;
                at.z = puppet->lastWorld.position.z;
                haveAt = true;
            }
            else if (recordedSamples < kMaxRecordedSamples)
            {
                ++recordedSamples;
                replay::pose::WorldTransform world{};
                float track[3]{};
                if (packet.kind == replay::entity::EntityKind::TrainCar) // its rigid track
                {
                    if (replay::trains::RecordedWorldAt(packet.id, timeMs, track))
                    {
                        at.x = track[0];
                        at.y = track[1];
                        at.z = track[2];
                        haveAt = true;
                    }
                }
                else if (PoseV2RecordedWorldAt(packet.id, timeMs, world))
                {
                    at.x = world.position.x;
                    at.y = world.position.y;
                    at.z = world.position.z;
                    haveAt = true;
                }
            }
            if (!haveAt)
                continue; // no pose anywhere near the playhead: nothing to place, nothing to miss
            const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > config.radiusM)
                continue;
            if (!shown)
            {
                missing.push_back({packet.id.Packed(), PoseV2MissingReason(puppet), distance});
                continue;
            }
            const std::uint32_t packed = packet.id.Packed();
            if (std::find(g_poseV2PopInJudged.begin(), g_poseV2PopInJudged.end(), packed) == g_poseV2PopInJudged.end())
            {
                g_poseV2PopInJudged.push_back(packed);
                if (replay::watch::IsPopIn(packet.timeMs, timeMs, g_poseV2WatchStartMs, distance))
                {
                    float sx = 0.0f, sy = 0.0f;
                    const bool onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
                    ++g_poseV2PopIns;
                    g_poseV2PopInsOnScreen += onScreen ? 1 : 0;
                    if (g_poseV2PopInLogs < 64)
                    {
                        ++g_poseV2PopInLogs;
                        Ctx().log << "PUPPET_POP_IN,id=" << replay::entity::IdText(packet.id)
                                  << ",kind=" << replay::entity::KindText(packet.kind) << ",model=0x" << std::hex
                                  << packet.modelHash << std::dec << ",camera_m=" << static_cast<int>(distance)
                                  << ",on_screen=" << (onScreen ? 1 : 0) << ",entry_ms=" << packet.timeMs
                                  << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            if (interiorCheck)
            {
                const Interior mine = INTERIOR::GET_INTERIOR_FROM_ENTITY(puppet->clone);
                Interior there = INTERIOR::GET_INTERIOR_AT_COORDS(at.x, at.y, at.z);
                if (there && !INTERIOR::IS_VALID_INTERIOR(there))
                    there = 0;
                std::uint32_t& streak = g_poseV2InteriorStreak[packed];
                streak = mine != there ? streak + 1 : 0;
                // Two checks in a row (a doorway flips for a moment).
                if (streak == 2 && std::find(g_poseV2InteriorLogged.begin(), g_poseV2InteriorLogged.end(), packed) ==
                                       g_poseV2InteriorLogged.end())
                {
                    g_poseV2InteriorLogged.push_back(packed);
                    if (g_poseV2InteriorLogged.size() <= 32)
                        Ctx().log << "PUPPET_INTERIOR,id=" << replay::entity::IdText(packet.id)
                                  << ",kind=" << replay::entity::KindText(packet.kind) << ",entity_interior=" << mine
                                  << ",position_interior=" << there << ",camera_m=" << static_cast<int>(distance)
                                  << ",camera_interior=" << INTERIOR::GET_INTERIOR_AT_COORDS(camera.x, camera.y, camera.z)
                                  << ",frozen=" << (ENTITY::_IS_ENTITY_FROZEN(puppet->clone) ? 1 : 0)
                                  << ",playhead_ms=" << timeMs << '\n';
                }
            }
        }
        g_poseV2Watch.Update(now, missing, g_poseV2WatchEvents);
        LogPoseV2WatchEvents(timeMs);
    }
    namespace
    {

    // The puppet blink detector (puppet_blink.h). Every tick of normal play,
    // every body (ped, horse) the recording says is visible at the playhead is
    // classified: drawn, hidden, unposed, bind or missing. Each episode closes
    // into a PUPPET_BLINK line (kBlinkLogCap a playback); PUPPET_BLINK_SUMMARY
    // at the end of the playback has the totals, the counted episodes (on
    // screen and near: the bench fails the run on them) and the worst ones.
    // The engine is read (camera, screen) only on a blink tick, on the
    // episode's first and every Rule::whereEveryFrames: a drawn body costs a
    // few compares and the override slot's counters (play.blink).
    replay::blink::Tracker g_blink;
    std::vector<replay::blink::Episode> g_blinkClosed;
    std::uint32_t g_blinkLastPlayhead = 0;
    std::uint32_t g_blinkLogs = 0;
    std::uint32_t g_blinkDropped = 0;
    std::uint32_t g_blinkPlaybacks = 0;
    constexpr std::uint32_t kBlinkLogCap = 96;

    void LogPoseV2Blinks()
    {
        for (const replay::blink::Episode& e : g_blinkClosed)
        {
            // Every counted episode before the far ones: past the cap only
            // the counted keep a line until twice the cap.
            if (g_blinkLogs >= (e.counts ? 2 * kBlinkLogCap : kBlinkLogCap))
            {
                ++g_blinkDropped;
                continue;
            }
            ++g_blinkLogs;
            Ctx().log << "PUPPET_BLINK,id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(e.id))
                      << ",model=0x" << std::hex << e.model << std::dec << ",kind=" << replay::blink::KindText(e.kind)
                      << ",from_ms=" << e.fromMs << ",frames=" << e.frames << ",ms=" << e.wallMs
                      << ",cause=" << replay::blink::CauseText(e.cause)
                      << ",camera_dist_m=" << (e.cameraM < 0.0f ? -1 : static_cast<int>(e.cameraM + 0.5f))
                      << ",on_screen=" << (e.onScreen ? 1 : 0) << ",counts=" << (e.counts ? 1 : 0)
                      << ",to_ms=" << e.toMs << ",cut=" << (e.cut ? 1 : 0) << '\n';
        }
        g_blinkClosed.clear();
    }

    void ResetPoseV2Blink(std::uint32_t startMs)
    {
        g_blink.Reset();
        g_blinkClosed.clear();
        g_blinkLastPlayhead = startMs;
        g_blinkLogs = 0;
        g_blinkDropped = 0;
    }
    }

    // The tick's gate: normal play outside the seek settle and the cheap
    // scrub. A jump (a marker, a loop) ends the stretch like a pause.
    bool BeginPoseV2BlinkTick(std::uint32_t timeMs)
    {
        const bool active = PoseV2NormalPlay() && !g_poseV2WatchHeld && !g_poseV2CheapScrub;
        const bool jumped = timeMs < g_blinkLastPlayhead || timeMs > g_blinkLastPlayhead + 1500;
        g_blinkLastPlayhead = timeMs;
        if ((!active || jumped) && g_blink.Active())
        {
            g_blink.Suspend(GetTickCount64(), g_blinkClosed);
            LogPoseV2Blinks();
        }
        if (!active)
            return false;
        g_blink.BeginTick(timeMs);
        return true;
    }

    void EndPoseV2BlinkTick()
    {
        REPLAY_PROFILE_ZONE("play.blink.end");
        g_blink.EndTick(GetTickCount64(), g_blinkClosed);
        if (!g_blinkClosed.empty())
            LogPoseV2Blinks();
    }
    namespace
    {

    // Where a blinking body is: its clone, its last placement or its recorded
    // pose; the distance to the rendered camera and whether that point
    // projects on screen.
    replay::blink::Where PoseV2BlinkWhere(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        replay::blink::Where where;
        Vector3 at{};
        bool haveAt = false;
        if (puppet.clone && replay::entities::Exists(puppet.clone))
        {
            at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
            haveAt = true;
        }
        else if (puppet.hasWorld)
        {
            at.x = puppet.lastWorld.position.x;
            at.y = puppet.lastWorld.position.y;
            at.z = puppet.lastWorld.position.z;
            haveAt = true;
        }
        else
        {
            replay::pose::WorldTransform world{};
            if (PoseV2RecordedWorldAt(puppet.packet.id, timeMs, world))
            {
                at.x = world.position.x;
                at.y = world.position.y;
                at.z = world.position.z;
                haveAt = true;
            }
        }
        if (!haveAt)
            return where;
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
        where.cameraM = std::sqrt(dx * dx + dy * dy + dz * dz);
        float sx = 0.0f, sy = 0.0f;
        where.onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
        return where;
    }

    void NotePoseV2Blink(const replay::playback::Puppet& puppet, replay::blink::Kind kind, replay::blink::Cause cause,
        std::uint32_t timeMs)
    {
        const std::uint32_t packed = puppet.packet.id.Packed();
        const ULONGLONG now = GetTickCount64();
        if (kind == replay::blink::Kind::None)
        {
            g_blink.Clear(packed, now, g_blinkClosed);
            return;
        }
        replay::blink::Where where;
        const bool read = g_blink.WantsWhere(packed, kind);
        if (read)
            where = PoseV2BlinkWhere(puppet, timeMs);
        g_blink.Note(packed, puppet.packet.modelHash, kind, cause, timeMs, now, read ? &where : nullptr, g_blinkClosed);
    }
    }

    // A body with no Ready puppet this tick (loading, dressing, failed, its
    // clone gone): missing when the recording shows it.
    void JudgePoseV2BlinkAbsent(const replay::playback::Puppet& puppet, bool alive, std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.blink");
        const std::uint32_t packed = puppet.packet.id.Packed();
        replay::blink::Frame frame;
        frame.alive = alive;
        frame.recordedVisible = alive && PoseV2RecordedVisible(packed, timeMs);
        frame.intentional = puppet.waitingForParent ||
            std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(), packed) !=
                g_poseV2WaitSegmentLogged.end();
        if (frame.recordedVisible && !frame.intentional &&
            (g_blink.ShownBefore(packed) || timeMs >= puppet.packet.timeMs + g_blink.Settings().graceMs))
        {
            // Inside the pose track of the segment at the playhead (the
            // Ready path's rule): before its first sample, or after its last
            // with no later record, the body is not drawn by design.
            const replay::entity::PoseSegment* segment = PoseV2SegmentAt(puppet.packet.id, timeMs);
            const std::uint32_t recordId = segment ? segment->recordId : packed;
            std::uint32_t firstMs = 0, lastMs = 0;
            const bool span = replay::pose::capture::TrackSpan(recordId, firstMs, lastMs);
            // The recorded player holds his nearest pose outside it (player_hold.h).
            frame.inTrack = span && ((g_poseV2PlayerPacked && packed == g_poseV2PlayerPacked) ||
                (timeMs >= firstMs && (timeMs <= lastMs + 100 || PoseV2RecordHasSuccessor(puppet.packet.id, recordId))));
        }
        const replay::blink::Kind kind = g_blink.Judge(packed, frame, timeMs, puppet.packet.timeMs);
        replay::blink::Cause cause = replay::blink::Cause::NoPuppet;
        if (kind != replay::blink::Kind::None && g_blink.WantsWhere(packed, kind))
        {
            switch (PoseV2MissingReason(&puppet))
            {
            case replay::watch::Reason::ModelLoading:
                cause = replay::blink::Cause::ModelLoading;
                break;
            case replay::watch::Reason::CreateWait:
                cause = replay::blink::Cause::CreateWait;
                break;
            case replay::watch::Reason::Dressing:
                cause = replay::blink::Cause::Dressing;
                break;
            case replay::watch::Reason::Failed:
                cause = replay::blink::Cause::Failed;
                break;
            default:
                break;
            }
        }
        NotePoseV2Blink(puppet, kind, cause, timeMs);
    }

    // A Ready body after this tick's visibility decision.
    void JudgePoseV2BlinkReady(replay::playback::Puppet& puppet, bool alive, bool recordedVisible, bool inTrack,
        bool sampled, std::uint32_t poseRecord, bool recordChanged, bool moved, std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.blink");
        const std::uint32_t packed = puppet.packet.id.Packed();
        replay::blink::Frame frame;
        frame.alive = alive;
        frame.recordedVisible = recordedVisible;
        frame.inTrack = inTrack;
        // The never animated hide judged for this record (a bind pose recording
        // is hidden on purpose).
        frame.intentional = puppet.neverRecord == poseRecord && puppet.neverHidden;
        frame.ready = true;
        frame.shown = puppet.visibleState == 1;
        frame.armed = puppet.overrideSlot >= 0;
        // The frame since the last tick: the same slot, shown then, the
        // playhead moving (a paused frame is not judged).
        std::uint64_t writes = 0, mismatches = 0;
        if (frame.armed)
        {
            const replay::pose::capture::OverrideStats stats =
                replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(puppet.overrideSlot));
            writes = stats.applies + stats.altApplies + stats.guardApplies + stats.prefixApplies + stats.staleApplies +
                stats.lateWrites + stats.weaponPassWrites;
            mismatches = stats.boneCountMismatch;
            frame.writesKnown = moved &&
                puppet.blinkWrites.Delta(puppet.overrideSlot, writes, mismatches, frame.writes, frame.mismatches);
        }
        puppet.blinkWrites.Keep(puppet.overrideSlot, frame.shown, writes, mismatches);
        const replay::blink::Kind kind = g_blink.Judge(packed, frame, timeMs, puppet.packet.timeMs);
        replay::blink::Cause cause = replay::blink::Cause::Other;
        if (kind == replay::blink::Kind::Bind)
            cause = replay::blink::BindCause(frame);
        else if (kind != replay::blink::Kind::None)
        {
            replay::blink::ArmState state;
            state.sampled = sampled;
            state.armRefused = puppet.armFailureLogged;
            state.prefixHold = puppet.prefixHold.Active();
            state.layerRetry = puppet.layerRetries > 0;
            state.segmentHole = puppet.switchGap.inHole;
            state.recordChanged = recordChanged;
            state.lostToChange = puppet.lostToChange;
            state.shownBefore = g_blink.ShownBefore(packed);
            cause = replay::blink::ArmCause(state);
        }
        NotePoseV2Blink(puppet, kind, cause, timeMs);
    }
    namespace
    {

    // PUPPET_BLINK_SUMMARY: one line per playback (Debug=0 too), read by the
    // bench (puppet_blink check) and tools/qa_triage.py.
    void LogPoseV2BlinkSummary()
    {
        g_blink.Suspend(GetTickCount64(), g_blinkClosed);
        LogPoseV2Blinks();
        using replay::blink::Kind;
        const replay::blink::Summary& s = g_blink.Stats();
        const replay::blink::Rule& rule = g_blink.Settings();
        const auto kindCount = [&](const std::uint32_t* table, Kind kind) { return table[static_cast<int>(kind)]; };
        ++g_blinkPlaybacks;
        Ctx().log << "PUPPET_BLINK_SUMMARY,playback=" << g_blinkPlaybacks << ",ticks=" << s.ticks
                  << ",body_ticks=" << s.judged << ",episodes=" << s.episodes << ",frames=" << s.frames
                  << ",on_screen_episodes=" << s.onScreenEpisodes << ",counted=" << s.counted
                  << ",on_screen_frames=" << s.countedFrames
                  << ",hidden=" << kindCount(s.perKind, Kind::Hidden) << ",unposed=" << kindCount(s.perKind, Kind::Unposed)
                  << ",bind=" << kindCount(s.perKind, Kind::Bind) << ",missing=" << kindCount(s.perKind, Kind::Missing)
                  << ",counted_hidden=" << kindCount(s.countedPerKind, Kind::Hidden)
                  << ",counted_unposed=" << kindCount(s.countedPerKind, Kind::Unposed)
                  << ",counted_bind=" << kindCount(s.countedPerKind, Kind::Bind)
                  << ",counted_missing=" << kindCount(s.countedPerKind, Kind::Missing)
                  << ",lines=" << g_blinkLogs << ",dropped=" << g_blinkDropped << ",suspends=" << s.suspends
                  << ",radius_m=" << static_cast<int>(rule.radiusM) << ",min_frames=" << rule.minFrames
                  << ",no_write_min_frames=" << rule.noWriteMinFrames << ",grace_ms=" << rule.graceMs << ",worst="
                  << replay::blink::WorstText(s.worst, [](std::uint32_t id) {
                         return replay::entity::IdText(replay::entity::ReplayId::FromPacked(id));
                     })
                  << '\n';
    }
    }

    void LogPoseV2WatchSummary()
    {
        g_poseV2Watch.Finish(GetTickCount64(), g_poseV2WatchEvents);
        LogPoseV2WatchEvents(g_poseV2WatchLastPlayhead);
        const replay::watch::Summary& summary = g_poseV2Watch.Stats();
        const replay::watch::Config& config = g_poseV2Watch.Settings();
        std::ostream& log = Ctx().log;
        log << "PUPPET_WATCH_SUMMARY,checks=" << summary.checks << ",episodes=" << summary.episodes
            << ",entities=" << summary.entities << ",worst_ms=" << summary.worstMs << ",total_ms=" << summary.totalMs;
        for (int reason = 0; reason < static_cast<int>(replay::watch::Reason::Count); ++reason)
            log << ',' << replay::watch::ReasonText(static_cast<replay::watch::Reason>(reason)) << '='
                << summary.perReason[reason];
        log << ",pop_ins=" << g_poseV2PopIns << ",pop_ins_on_screen=" << g_poseV2PopInsOnScreen
            << ",interior_mismatch=" << g_poseV2InteriorLogged.size() << ",create_refused=" << g_poseV2CreateRefused
            << ",record_unsampled=" << g_poseV2RecordUnsampled.size()
            << ",suspends=" << g_poseV2WatchSuspends << ",radius_m=" << config.radiusM
            << ",threshold_ms=" << config.thresholdMs << '\n';
        // Arms through the skeleton map, arms given up (hidden since, never a
        // T-pose) and their lines.
        log << "POSE_V2_ARM_SUMMARY,mapped=" << g_poseV2MappedArms << ",refused=" << g_poseV2RefusedArms
            << ",refused_hidden_lines=" << g_poseV2RefusedHiddenLogs << '\n';
        // The skeleton signature checks (Debug=0 too).
        log << "POSE_V2_SKELETON_SIGNATURE_SUMMARY,same=" << g_poseV2Signature.same
            << ",differ=" << g_poseV2Signature.differ << ",unknown=" << g_poseV2Signature.unknown
            << ",mapped=" << g_poseV2Signature.mapped << ",refused=" << g_poseV2Signature.refused
            << ",rechecks=" << g_poseV2Signature.rechecks << ",rearms=" << g_poseV2Signature.rearms
            << ",timeouts=" << g_poseV2Signature.timeouts << ",rebuilt=" << g_poseV2Signature.rebuilt << '\n';
        // The unposed episodes, Debug=0 too (_SUMMARY).
        g_poseV2Unposed.Finish();
        {
            const replay::watch::UnposedSummary& unposed = g_poseV2Unposed.Stats();
            const replay::watch::UnposedRule& rule = g_poseV2Unposed.Rule();
            log << "POSE_V2_UNPOSED_SUMMARY,episodes=" << unposed.episodes << ",transient=" << unposed.transient
                << ",persistent=" << unposed.persistent
                << ",create=" << unposed.perCause[static_cast<int>(replay::watch::UnposedCause::Create)]
                << ",rearm=" << unposed.perCause[static_cast<int>(replay::watch::UnposedCause::Rearm)]
                << ",persistent_create=" << unposed.persistentPerCause[static_cast<int>(replay::watch::UnposedCause::Create)]
                << ",persistent_rearm=" << unposed.persistentPerCause[static_cast<int>(replay::watch::UnposedCause::Rearm)]
                << ",worst_transient_ms=" << unposed.worstTransientMs << ",worst_transient_ticks=" << unposed.worstTransientTicks
                << ",worst_ms=" << unposed.worstMs << ",lines=" << g_poseV2UnposedHideLogs
                << ",rule_ms=" << rule.persistMs << ",rule_ticks=" << rule.persistTicks << '\n';
        }
        LogPoseV2TPoseSummary(); // Debug=0 too (_SUMMARY)
        LogPoseV2BlinkSummary(); // Debug=0 too (_SUMMARY)
        log.flush();
    }

    void ResetPoseV2Watch(std::uint32_t startMs)
    {
        g_poseV2Watch.Reset();
        ResetPoseV2Blink(startMs);
        g_poseV2WatchActive = false;
        g_poseV2WatchNextAt = 0;
        g_poseV2InteriorNextAt = 0;
        g_poseV2WatchLastPlayhead = startMs;
        g_poseV2WatchStartMs = startMs;
        g_poseV2WatchLogs = 0;
        g_poseV2WatchSuspends = 0;
        g_poseV2PopIns = 0;
        g_poseV2PopInsOnScreen = 0;
        g_poseV2PopInLogs = 0;
        g_poseV2PopInJudged.clear();
        g_poseV2RecordUnsampled.clear();
        g_poseV2InteriorStreak.clear();
        g_poseV2InteriorLogged.clear();
        g_poseV2WatchEvents.clear();
        g_poseV2CreateRefused = 0;
        g_poseV2CreateRefusedLogs = 0;
        g_poseV2MappedArms = 0;
        g_poseV2RefusedArms = 0;
        g_poseV2RefusedHiddenLogs = 0;
        g_poseV2Signature = {};
    }

    // After every puppet of the tick: the rider and mount pairs for the next
    // tick (from the recorded transforms just placed), the pair check and the
    // time step of the detector, and the POSE_JERK lines (pose_jerk.h).
    void UpdatePoseV2Jerks(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.jerk");
        g_poseV2JerkBodies.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            const bool horse = puppet.packet.kind == replay::entity::EntityKind::Horse;
            if ((!horse && puppet.packet.kind != replay::entity::EntityKind::Ped) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.hasWorld || puppet.visibleState != 1)
                continue;
            replay::jerk::Body body;
            body.packed = puppet.packet.id.Packed();
            body.horse = horse;
            body.world = puppet.lastWorld;
            g_poseV2JerkBodies.push_back(body);
        }
        const std::unordered_map<std::uint32_t, std::uint32_t> before = g_poseV2Mounts.Map();
        g_poseV2Mounts.Update(g_poseV2JerkBodies);
        const auto logPair = [&](std::uint32_t rider, std::uint32_t mount, const char* phase, int seat) {
            if (g_poseV2MountLogs >= 32)
                return;
            ++g_poseV2MountLogs;
            // The seat (-1 the saddle, 0 behind it) and how many riders the
            // mount carries after this tick's update.
            Ctx().log << "POSE_V2_MOUNT_PAIR,phase=" << phase
                << ",rider=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(rider))
                << ",mount=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(mount))
                << ",playhead_ms=" << timeMs << ",seat=" << seat
                << ",riders=" << g_poseV2Mounts.RidersOf(mount) << '\n';
        };
        for (const auto& [rider, mount] : g_poseV2Mounts.Map())
        {
            const auto was = before.find(rider);
            if (was == before.end() || was->second != mount)
                logPair(rider, mount, "enter", g_poseV2Mounts.SeatOf(rider));
        }
        // A leave shows the seat the rider had; a rider who keeps his mount but
        // changes seat (the first one on is alone in front, -1, until a second
        // gets on in front of him) gets a seat line, so the log never keeps a
        // stale seat.
        for (const auto& [rider, mount] : before)
        {
            if (g_poseV2Mounts.MountOf(rider) != mount)
                logPair(rider, mount, "leave", g_poseV2Mounts.PreviousSeatOf(rider));
        }
        for (const std::uint32_t rider : g_poseV2Mounts.SeatChanges())
            logPair(rider, g_poseV2Mounts.MountOf(rider), "seat", g_poseV2Mounts.SeatOf(rider));

        g_poseV2Jerk.EndTick(g_poseV2JerkFindings);
        const replay::jerk::TickInfo& tick = g_poseV2Jerk.LastTick();
        for (const replay::jerk::Finding& finding : g_poseV2JerkFindings)
        {
            if (g_poseV2JerkLogs >= kPoseV2JerkLogCap)
                break;
            ++g_poseV2JerkLogs;
            const replay::entity::ReplayId id = replay::entity::ReplayId::FromPacked(finding.packed);
            const replay::playback::Puppet* puppet = FindPoseV2Puppet(id);
            float velocity = -1.0f;
            if (puppet && puppet->clone && (finding.kind == replay::jerk::Kind::EngineMoved ||
                    finding.kind == replay::jerk::Kind::MountOffset))
            {
                const Vector3 v = ENTITY::GET_ENTITY_VELOCITY(puppet->clone, 0);
                velocity = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            }
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "POSE_JERK,id=%s,kind=%s,playhead_ms=%u,magnitude_m=%.3f,engine_m=%.3f,mount=%s,mount_m=%.3f,"
                "speed_mps=%.1f,engine_velocity_mps=%.2f,wall_step_ms=%.1f,play_step_ms=%.0f,lost_ms=%.1f,causes=%s,"
                "riders=%u\n", // riders a mount_offset shows on
                replay::entity::IdText(id).c_str(), replay::jerk::KindText(finding.kind), finding.playheadMs,
                finding.magnitudeM, finding.engineM,
                finding.mount ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(finding.mount)).c_str() : "none",
                finding.mountM, finding.speedMps, velocity, tick.wallStepMs, tick.playStepMs, tick.lostMs,
                replay::jerk::CauseText(finding.causes).c_str(), finding.riders);
            Ctx().log << line;
        }
    }

    // The detector's totals for the playback (and the worst puppets); the bench
    // regression reads POSE_JERK_SUMMARY.
    void LogPoseV2JerkSummary()
    {
        const replay::jerk::Summary& total = g_poseV2Jerk.Totals();
        if (total.ticks == 0)
            return;
        std::ostream& log = Ctx().log;
        char line[1024]{}; // room for every field
        std::snprintf(line, sizeof(line),
            "POSE_JERK_SUMMARY,ticks=%llu,puppet_ticks=%llu,jerks=%llu,engine_moved=%llu,mount_offset=%llu,time_step=%llu,"
            "stall=%llu,pose_stale=%llu,worst_m=%.3f,worst_id=%s,worst_kind=%s,worst_ms=%u,held_ticks=%llu,capped_ticks=%llu,"
            "slow_ticks=%llu,mount_pairs_entered=%llu,mount_pairs_left=%llu,carried_ticks=%llu,pushed_holds=%llu,logged=%u,carried_physics=%d,"
            "frame_end=%d,physics_filter=%d,engine_moved_placed_back=%llu," // placed back
            "mount_offset_placed_back=%llu,mount_offset_riders=%llu,max_riders_per_mount=%u,shared_mount_ticks=%llu\n",
            static_cast<unsigned long long>(total.ticks), static_cast<unsigned long long>(total.observations),
            static_cast<unsigned long long>(total.jerks),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::EngineMoved)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::MountOffset)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::TimeStep)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::Stall)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::PoseStale)]),
            total.worstM,
            total.worstPacked ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(total.worstPacked)).c_str() : "none",
            replay::jerk::KindText(total.worstKind), total.worstMs, static_cast<unsigned long long>(total.heldTicks),
            static_cast<unsigned long long>(total.cappedTicks), static_cast<unsigned long long>(total.slowTicks),
            static_cast<unsigned long long>(g_poseV2Mounts.Enters()), static_cast<unsigned long long>(g_poseV2Mounts.Leaves()),
            static_cast<unsigned long long>(g_poseV2CarriedTicks), static_cast<unsigned long long>(g_poseV2PushedHolds),
            g_poseV2JerkLogs, 0, replay::mount::FrameEnd() ? 1 : 0,
            replay::collision::Enabled() ? 1 : 0, static_cast<unsigned long long>(total.placedBack),
            static_cast<unsigned long long>(total.pairPlacedBack), static_cast<unsigned long long>(total.mountOffsetRiders),
            g_poseV2Mounts.MaxRiders(), static_cast<unsigned long long>(g_poseV2Mounts.SharedUpdates()));
        log << line;
        log << "POSE_JERK_CAUSES";
        static const char* const kCauseNames[] = {"slow_tick", "capped", "held", "physics", "not_written", "segment",
            "created", "mounted", "frozen", "attached", "prerender"}; // prerender
        static_assert(sizeof(kCauseNames) / sizeof(kCauseNames[0]) == replay::jerk::kCauseBits, "one name per cause");
        for (int bit = 0; bit < replay::jerk::kCauseBits; ++bit)
            log << ',' << kCauseNames[bit] << '=' << total.byCause[bit];
        log << '\n';
        for (const replay::jerk::PuppetSummary& puppet : g_poseV2Jerk.Worst(8))
        {
            log << "POSE_JERK_PUPPET,id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(puppet.packed))
                << ",jerks=" << puppet.jerks << ",worst_m=" << puppet.worstM << ",worst_ms=" << puppet.worstMs
                << ",worst_kind=" << replay::jerk::KindText(puppet.worstKind);
            for (int kind = 0; kind < static_cast<int>(replay::jerk::Kind::Count); ++kind)
                log << ',' << replay::jerk::KindText(static_cast<replay::jerk::Kind>(kind)) << '=' << puppet.byKind[kind];
            log << '\n';
        }
        log.flush();
    }
}
