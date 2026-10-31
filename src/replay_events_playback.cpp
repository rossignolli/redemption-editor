// replay_events_playback.cpp: the playback's begin, tick and end, and what a restart forgets.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {
    replay::decal::guard::Refusals g_poseV2DecalGuardAtBegin; // the core's guard counters at playback start
    int g_poseV2PlayerProofs = -1;
    }

    // An older clip may keep an area that a local offset taken for a world
    // position stretched to the map origin. It is rebuilt from the clip's own
    // positions, each one checked (cliparea::PositionValid): world effects,
    // decals not attached, registered effects not attached, the game camera.
    void RepairPoseV2ClipArea()
    {
        auto& clip = Ctx().clip;
        if (!replay::cliparea::AreaReachesOrigin(clip.hasArea, clip.areaMin, clip.areaMax))
            return;
        const replay::cliparea::Area area = replay::cliparea::RebuildArea([&](auto&& extend) {
            for (const replay::entity::FxEvent& event : clip.fx.Events())
                if (!event.entity.Valid())
                    extend(event.matrix[12], event.matrix[13], event.matrix[14]);
            for (const replay::entity::DecalEvent& decal : clip.decals.Events())
            {
                if (decal.attached)
                    continue;
                float pos[3];
                std::memcpy(pos, decal.settings, sizeof(pos));
                extend(pos[0], pos[1], pos[2]);
            }
            for (const replay::entity::RegisteredFxTrack& track : clip.registeredFx)
            {
                if (track.attached || track.frames.empty())
                    continue;
                for (const replay::entity::RegisteredFxFrame* frame : {&track.frames.front(), &track.frames.back()})
                {
                    float world[16];
                    replay::entity::ComposeFxWorld(frame->matrix, frame->parent, world);
                    extend(world[12], world[13], world[14]);
                }
            }
            for (const auto& sample : clip.gameCamera)
                extend(sample.position.x, sample.position.y, sample.position.z);
        });
        Ctx().log << "POSE_V2_PLAYBACK_AREA_REPAIR,old_min=" << clip.areaMin[0] << ";" << clip.areaMin[1] << ";" << clip.areaMin[2]
            << ",old_max=" << clip.areaMax[0] << ";" << clip.areaMax[1] << ";" << clip.areaMax[2] << ",has=" << area.has
            << ",min=" << area.min[0] << ";" << area.min[1] << ";" << area.min[2]
            << ",max=" << area.max[0] << ";" << area.max[1] << ";" << area.max[2] << ",rejected=" << area.rejected << '\n';
        clip.hasArea = area.has;
        std::memcpy(clip.areaMin, area.min, sizeof(clip.areaMin));
        std::memcpy(clip.areaMax, area.max, sizeof(clip.areaMax));
    }

    void BeginPlayback(std::uint32_t firstCreateMs)
    {
        replay::log::NewSession(); // every LogLimit counts again (a second playback logs too)
        // The playback's sound census (compared at the end) and the census-level
        // captured sounds.
        replay::census::BeginPlayback(Ctx());
        replay::mixprobe::BeginPlayback(Ctx());
        replay::rendertarget::BeginPlayback(Ctx().clip.renderTargets, Ctx().log);
        replay::mixscene::BeginPlayback(Ctx());
        replay::catstate::BeginPlayback(Ctx().clip, Ctx().log);
        replay::speechduck::BeginPlayback(Ctx().clip, Ctx().log);
        replay::speechlevel::BeginPlayback(Ctx());
        replay::volumeprobe::BeginPlayback(Ctx());
        replay::windreplay::BeginPlayback(Ctx().clip, Ctx().log);
        replay::capture::BeginPlayback(Ctx());
        // The generic sound player (the SGEN packets), after the census and the
        // capture.
        replay::soundgeneric::BeginPlayback(Ctx(), static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()));
        // The observer counts what the engine and the replay trigger during
        // playback (read-only detour; the histogram is logged at the end).
        replay::fx::SetEnabled(g_poseV2FxObserverReady);
        g_poseV2FxPhaseStartUs = replay::fx::NowUs();
        BeginPoseV2FxCheck();
        g_poseV2FxRegisteredCursor = replay::fx::TotalRegistered();
        g_poseV2FxRegistered.clear();
        g_poseV2FxRegisteredSamples = 0;
        g_poseV2ShotCursor.Reset(firstCreateMs);
        BeginPoseV2PlayerAudio();
        BeginPoseV2PlayerWeaponBank();
        BeginPoseV2PlayerMountAudio();
        replay::horseaudio::Begin("play", Ctx().log);
        BeginEntityQuery(false);
        BeginPedAudioState(false);
        BeginFootEvents(false);
        g_poseV2SpeechCursorPlay.Reset(firstCreateMs);
        // The recorded player (owner of the clip's player-class generic sounds),
        // whose lines the hidden player says.
        g_speechPlayerPacked = 0xFFFFFFFFu;
        g_speechOnPlayer = 0;
        g_speechPlayerFar = 0;
        g_speechPlayerFarLogs = 0;
        ReleasePlayerSpeechHold();
        g_speechHeld = g_speechHeldStarted = 0;
        g_speechHeldLogs = 0;
        g_speechCharacterMismatch = 0;
        g_speechCharacterMismatchLogs = 0;
        g_speechPlayerRefused = g_speechPlayerRefusedPuppetCreated = 0;
        g_speechPlayerRefusedLogs = 0;
        g_speechPlayerNamedVoice = g_speechPlayerNamedVoiceCreated = 0;
        g_speechPlayerNamedVoiceLogs = 0;
        g_speechConversationPlainRetries = g_speechConversationPlainCreated = 0;
        g_speechConversationPlainLogs = 0;
        g_speechConversationTally = {};
        g_speechConversationLogs = 0;
        for (const replay::soundgeneric::CreatePacket& create : Ctx().clip.soundGeneric.creates)
            if (create.ownerClass == replay::census::OwnerClass::Player && create.owner != replay::census::kNoOwner)
            {
                g_speechPlayerPacked = create.owner;
                break;
            }
        replay::census::SetPlayerSpeechProxy(true); // sound_check_core.h ProxySpeechKey
        Ctx().log << "SPEECH_PLAYER_ROUTE,on=1,player="
                  << (g_speechPlayerPacked == 0xFFFFFFFFu ? std::string("by_model")
                                                          : replay::entity::IdText(replay::entity::ReplayId::FromPacked(g_speechPlayerPacked)))
                  << '\n';
        g_poseV2FootstepCursorPlay.Reset(firstCreateMs);
        g_footEventCursorPlay.Reset(firstCreateMs);
        g_poseV2ExplosionCursorPlay.Reset(firstCreateMs);
        g_poseV2AnimCursorPlay.Reset(firstCreateMs);
        g_poseV2AnimPlayed = 0;
        g_poseV2AnimRefused = 0;
        g_poseV2AnimNoPuppet = 0;
        g_poseV2AnimRetried = 0;
        g_poseV2AnimConsumed = 0;
        g_poseV2AnimNotConsumed = 0;
        g_poseV2AnimPlayLines = 0;
        g_poseV2AnimRefusals.clear();
        g_poseV2AnimCarried.clear();
        g_poseV2AnimQueuedAudio.clear();
        g_poseV2SurfaceIndexBuilt = false;
        g_poseV2SurfaceApplied.clear();
        g_poseV2SurfacesApplied = 0;
        g_poseV2SurfacesFailed = 0;
        g_poseV2SurfaceLastError.clear();
        g_poseV2ExplosionsPlayed = 0;
        g_poseV2ExplosionsRefused = 0;
        g_poseV2ExplosionLastError.clear();
        g_poseV2FxCursorPlay.Reset(firstCreateMs);
        g_wfxCursor.Reset(firstCreateMs);
        g_poseV2RegisteredLastPlayhead = 0;
        g_poseV2RegisteredCrossed = 0;
        g_poseV2CameraAmbientPlaySkipped = 0;
        g_poseV2FxAttachPlayLogs = 0;
        g_poseV2FxAttachPlayApplied = 0;
        g_poseV2FxAttachPlayWaiting = 0;
        g_poseV2RegisteredEnded.clear();
        g_poseV2RegisteredAfterEndLogged = 0;
        g_poseV2RegisteredEnd = RegisteredEndStats{};
        g_poseV2RegisteredOwnerRetryMs.clear();
        g_poseV2RegisteredMismatchLogs = 0;
        RepairPoseV2ClipArea();
        g_poseV2FireCheckAt = 0;
        g_poseV2FiresOurs.clear();
        if (Ctx().clip.hasArea)
        {
            // Fires and particles still alive in the recording area (a molotov still
            // burning) leave before playback.
            const float cx = (Ctx().clip.areaMin[0] + Ctx().clip.areaMax[0]) * 0.5f;
            const float cy = (Ctx().clip.areaMin[1] + Ctx().clip.areaMax[1]) * 0.5f;
            const float cz = (Ctx().clip.areaMin[2] + Ctx().clip.areaMax[2]) * 0.5f;
            const float dx = Ctx().clip.areaMax[0] - Ctx().clip.areaMin[0];
            const float dy = Ctx().clip.areaMax[1] - Ctx().clip.areaMin[1];
            const float dz = Ctx().clip.areaMax[2] - Ctx().clip.areaMin[2];
            float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) + 15.0f;
            radius = radius > 250.0f ? 250.0f : radius;
            const int firesBefore = FIRE::GET_NUMBER_OF_FIRES_IN_RANGE(cx, cy, cz, radius);
            const float clearCenter[3] = {cx, cy, cz};
            FIRE::STOP_FIRE_IN_RANGE(cx, cy, cz, radius);
            GRAPHICS::REMOVE_PARTICLE_FX_IN_RANGE(cx, cy, cz, radius);
            // The whole area is cleared; the decals that existed before the recording
            // come back from its initial state (the decal monitor).
            GRAPHICS::REMOVE_DECALS_IN_RANGE(cx, cy, cz, radius);
            static int objects[1024];
            const int objectCount = worldGetAllObjects(objects, 1024);
            int objectsCleaned = 0;
            for (int n = 0; n < objectCount; ++n)
            {
                if (!objects[n] || !replay::entities::Exists(objects[n]))
                    continue;
                const Vector3 p = ENTITY::GET_ENTITY_COORDS(objects[n], TRUE, FALSE);
                const float ox = p.x - cx;
                const float oy = p.y - cy;
                const float oz = p.z - cz;
                if (ox * ox + oy * oy + oz * oz > radius * radius)
                    continue;
                GRAPHICS::REMOVE_DECALS_FROM_OBJECT(objects[n]);
                ++objectsCleaned;
            }
            Ctx().log << "POSE_V2_PLAYBACK_WORLD_DECAL_CLEAR,radius=" << radius << ",objects=" << objectsCleaned << '\n';
            // The recording's footprints and trails leave with it (trail map reset).
            GRAPHICS::_0xEC3F7F24EEEB3BA3();
            // fires_after is read in the tick of the stop (a stopped fire can
            // stay counted until the manager's update): the survivors are
            // read on a later tick (POSE_V2_PLAYBACK_FIRE_CLEAR_CHECK).
            Ctx().log << "POSE_V2_PLAYBACK_WORLD_FX_CLEAR,center=" << cx << ";" << cy << ";" << cz << ",radius=" << radius
                << ",fires_before=" << firesBefore << ",fires_after=" << FIRE::GET_NUMBER_OF_FIRES_IN_RANGE(cx, cy, cz, radius)
                << '\n';
            SchedulePoseV2FireCheck("start", clearCenter, radius);
        }
        {
            // The recording's own decals leave the world before playback adds its
            // copies.
            std::uint32_t checked = 0;
            const std::uint32_t aliveBefore = RecordedDecalsAlive(checked);
            const std::uint32_t removed = RemoveRecordedDecals();
            const std::uint32_t aliveAfter = RecordedDecalsAlive(checked);
            Ctx().log << "POSE_V2_PLAYBACK_DECAL_CLEAR,recorded_ids=" << checked << ",alive_before=" << aliveBefore
                << ",removed=" << removed << ",alive_after=" << aliveAfter << '\n';
        }
        g_poseV2DecalCursorPlay.Reset(firstCreateMs);
        g_poseV2DecalInitialPlayPending = true;
        g_poseV2DecalRemoveCursorPlay.Reset(firstCreateMs);
        g_poseV2ProjCursorPlay.Reset(firstCreateMs);
        g_poseV2GoreCursorPlay.Reset(firstCreateMs);
        g_poseV2GoreHitCursorPlay.Reset(firstCreateMs);
        g_poseV2GorePending.clear();
        g_poseV2GoreApplied.clear();
        g_poseV2FiresOurs.clear();
        g_poseV2FirePlayed = 0;
        g_poseV2FirePlayRefused = 0;
        g_poseV2FireCharShielded = 0;
        g_poseV2FireStopped = 0;
        g_poseV2FireEngineEnded = 0;
        g_poseV2FireEntityLogs = 0;
        g_poseV2FireEntityNextMs = 0;
        g_poseV2FireReconciles = 0;
        g_poseV2FireRestored = 0;
        g_poseV2FireRestoreMissed = 0;
        g_poseV2FireRemnantsCleared = 0;
        g_poseV2FireLastError.clear();
        g_poseV2FireRetry.assign(Ctx().clip.fireStarts.Events().size(), replay::fire::StartRetry{});
        g_poseV2FirePending.clear();
        g_poseV2FireEntityWaits = 0;
        g_poseV2FireWorldFound = 0;
        g_poseV2FireWorldMissing = 0;
        g_poseV2FireRetries = 0;
        g_poseV2FireGaveUp = 0;
        g_poseV2FireParentCleared = 0;
        g_poseV2FireIncremental = 0;
        g_poseV2FireEndedQueue.clear();
        g_poseV2FireHoldLastMs = 0xFFFFFFFFu;
        g_poseV2FireEndedQueued = 0;
        g_poseV2FireDuplicates = 0;
        g_poseV2FireSlotLogs = 0;
        g_poseV2FireFirstSampleMs.clear();
        g_poseV2FireSkipLogs = 0;
        g_poseV2FireReconcileLogs = 0;
        g_poseV2FireStartCursorPlay.Reset(firstCreateMs);
        g_poseV2FireStopCursorPlay.Reset(firstCreateMs);
        // 0xFFFFFFFF: the first tick counts as a jump, so the reconcile
        // builds the state at the playhead (the fires lit before recording).
        g_poseV2FireLastPlayhead = 0xFFFFFFFFu;
        g_poseV2FirePlaying = g_poseV2FireReady && !Ctx().clip.fireStarts.Events().empty();
        if (g_poseV2FirePlaying)
        {
            PoseV2FireArea();
            replay::fire::SetBlockSpread(true);
            replay::fire::SetObserving(true);
            g_poseV2FirePlayCursor = replay::fire::TotalCalls();
            ClearPoseV2Fires(true);
            if (!Ctx().clip.hasArea)
                SchedulePoseV2FireCheck("start", g_poseV2FireCenter, g_poseV2FireRadius);
            // The real player must not burn in the recreated fires (proof bit 2).
            const Ped player = PLAYER::PLAYER_PED_ID();
            g_poseV2PlayerProofs = ENTITY::_GET_ENTITY_PROOFS(player);
            ENTITY::SET_ENTITY_PROOFS(player, g_poseV2PlayerProofs | 2, FALSE);
            Ctx().log << "POSE_V2_PLAYBACK_FIRE_BEGIN,starts=" << Ctx().clip.fireStarts.Events().size() << ",center="
                << g_poseV2FireCenter[0] << ";" << g_poseV2FireCenter[1] << ";" << g_poseV2FireCenter[2] << ",radius=" << g_poseV2FireRadius
                << ",remnants_cleared=" << g_poseV2FireRemnantsCleared << ",proofs_before=" << g_poseV2PlayerProofs << '\n';
        }
        else if (g_poseV2FireReady && Ctx().clip.hasArea)
        {
            // A clip without recorded fires owns the fire state of its area too (a
            // replay editor resets every fire and particle on entry, whatever the
            // clip). The area clear above stops live fires only; the remnants a map
            // fire leaves (smouldering, flames and smoke drawn by the area fire system)
            // and the burnt ground would stay, so a fire lit in the world after the
            // recording would show in it. Same clear as a clip with fires: remnants and
            // burnt ground in the clip area, the puppets' burnt look written back by
            // FREC.
            const float dx = Ctx().clip.areaMax[0] - Ctx().clip.areaMin[0];
            const float dy = Ctx().clip.areaMax[1] - Ctx().clip.areaMin[1];
            const float dz = Ctx().clip.areaMax[2] - Ctx().clip.areaMin[2];
            for (int axis = 0; axis < 3; ++axis)
                g_poseV2FireCenter[axis] = (Ctx().clip.areaMin[axis] + Ctx().clip.areaMax[axis]) * 0.5f;
            g_poseV2FireRadius = (std::min)(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) + 15.0f, 250.0f);
            ClearPoseV2Fires(true);
            Ctx().log << "POSE_V2_PLAYBACK_FIRE_WORLD_CLEAR,starts=0,center=" << g_poseV2FireCenter[0] << ";"
                << g_poseV2FireCenter[1] << ";" << g_poseV2FireCenter[2] << ",radius=" << g_poseV2FireRadius
                << ",remnants_cleared=" << g_poseV2FireRemnantsCleared << ",error="
                << (g_poseV2FireLastError.empty() ? "none" : g_poseV2FireLastError.c_str()) << '\n';
            g_poseV2FireRadius = 0.0f; // nothing of ours to clear again at a jump or the end
        }
        g_poseV2BloodCursorPlay.Reset(firstCreateMs);
        g_poseV2BloodLastPlayhead = firstCreateMs;
        g_poseV2BloodAppliedUpTo.clear();
        g_poseV2BloodAppliedClone.clear();
        g_poseV2BloodPending.clear();
        g_poseV2BloodQueue.Reset();
        g_poseV2BloodSlowLogs = 0;
        g_poseV2BloodTickLogs = 0;
        g_poseV2BloodQueueLogs = 0;
        replay::blood::ForgetReadableRegions();
        g_poseV2BloodPlayed = 0;
        g_poseV2BloodFailed = 0;
        g_poseV2BloodPresent = 0;
        g_poseV2BloodBusy = 0;
        g_poseV2BloodReconciles = 0;
        g_poseV2BloodLastError.clear();
        g_poseV2GoreLastPlayhead = firstCreateMs;
        g_poseV2GoreReconciles = 0;
        g_poseV2GoreHitsPlayed = 0;
        g_poseV2GoreHitsFailed = 0;
        g_poseV2GoreHitsNoPuppet = 0;
        g_poseV2GoreMaskSkipped = 0;
        g_poseV2GorePlayed = 0;
        g_poseV2GoreFailed = 0;
        g_poseV2GoreNoPuppet = 0;
        g_poseV2GoreLastError.clear();
        g_poseV2ProjPlayed = 0;
        g_poseV2ProjFailed = 0;
        g_poseV2ProjMissing = 0;
        g_poseV2ProjNoInfo = 0;
        g_poseV2ProjLastError.clear();
        // Projected decals, entity decals, aging, engine fx.
        g_poseV2ProjNullOwner = 0;
        g_poseV2ProjEngineCursor = replay::decal::TotalProj();
        g_poseV2ProjEngineSeen = 0;
        g_poseV2ProjEngineMirrors = 0;
        g_poseV2ProjEngineLogs = 0;
        replay::decal::SetEnabled(g_poseV2DecalObserverReady); // the projected detour keeps observing
        g_poseV2DecalsEntityPassed = 0;
        g_poseV2DecalsEntityMissing = 0;
        g_poseV2DecalEntityLogs = 0;
        g_poseV2DecalsAgedPlay = 0;
        g_poseV2DecalsExpiredPlay = 0;
        g_poseV2DecalAgeLogs = 0;
        g_poseV2BloodEvolved = 0;
        g_poseV2BloodEvolveFailed = 0;
        g_poseV2BloodEvolvePresent = 0;
        g_poseV2BloodEvolveLogs = 0;
        g_poseV2NotPedLogs = 0;
        BeginPoseV2EngineFxStop();
        BeginPoseV2OwnFeet();
        g_poseV2DecalIds.clear();
        g_poseV2DecalRemoveRetries.clear();
        g_poseV2DecalRemoveRetried = 0;
        g_poseV2DecalChainsBuilt = false;
        g_poseV2DecalChains.clear();
        g_poseV2DecalLastPlayhead = 0;
        g_poseV2DecalReconciles = 0;
        g_poseV2DecalStateRemoved = 0;
        g_poseV2EngineDecalCursor = replay::decal::TotalCalls();
        g_poseV2EngineDecalsRemoved = 0;
        g_poseV2EngineDecalsOnPeds = 0;
        g_poseV2DecalStateRestored = 0;
        g_poseV2DecalRegrows = 0;
        g_poseV2DecalRegrowRefused = 0;
        g_poseV2DecalRegrowGaveUp = 0;
        g_poseV2DecalRegrowDeferred = 0;
        g_poseV2DecalRegrowLogs = 0;
        g_poseV2DecalRegrowLastError.clear();
        g_poseV2DecalsRemoved = 0;
        g_poseV2DecalRemovesUnmapped = 0;
        g_poseV2DecalsReplaced = 0;
        g_poseV2DecalsPlayed = 0;
        g_poseV2DecalsSkippedInternal = 0;
        g_poseV2DecalsNoEntity = 0;
        g_poseV2DecalsNoInfo = 0;
        g_poseV2DecalsNullInfo = 0;
        g_poseV2DecalNullInfoLogs = 0;
        g_poseV2DecalsDeadIdReadd = 0;
        g_poseV2DecalGuardLogs = 0;
        g_poseV2DecalGuardAtBegin = replay::decal::GuardRefusals();
        g_poseV2DecalsFailed = 0;
        g_poseV2DecalLastError.clear();
        g_poseV2MeleeCursorPlay.Reset(firstCreateMs);
        g_poseV2SoundCursorPlay.Reset(firstCreateMs);
        StopPoseV2CollisionSounds();
        g_poseV2EntitySoundLastMs = firstCreateMs;
        g_poseV2EntitySoundsPlayed = 0;
        ResetPoseV2PuppetCreates();
        g_poseV2ShotFireViaFallback = 0;
        g_poseV2ShotFirePending.clear();
        g_poseV2EntitySoundsRefused = 0;
        g_poseV2EntitySoundsStolen = 0;
        g_poseV2EntitySoundOutcome.clear();
        g_poseV2EntitySoundLastRefusal.clear();
        g_poseV2EntitySoundsTooFar = 0;
        g_poseV2EntitySoundsEnded = 0;
        g_poseV2EntitySoundsLate = 0;
        g_poseV2PedAudioLengthStops = 0;
        g_poseV2EsndTally.Clear();
        g_poseV2LineOutcomes.clear();
        g_poseV2EntitySoundsBySpeech = 0;
        g_poseV2EntitySoundsSpeechFallback = 0;
        g_poseV2EntitySoundsToGeneric = 0;
        g_poseV2SoundsToGeneric = 0;
        g_poseV2CollisionToGeneric = 0;
        g_poseV2AnimToGeneric = 0;
        g_poseV2EsndPassCounted.clear();
        g_poseV2EsndNewPass = true;
        g_poseV2EntitySoundWaiting.clear();
        g_poseV2EntitySoundLast.clear();
        g_poseV2EntitySoundPlayLogs = 0;
        g_poseV2CollisionLastMs = firstCreateMs;
        g_poseV2CollisionNext = 0;
        g_poseV2CollisionPlayed = 0;
        g_poseV2CollisionRefused = 0;
        g_poseV2CollisionLast.clear();
        g_poseV2SoundsPlayed = 0;
        g_poseV2SoundsRefused = 0;
        g_poseV2SoundsNeighbor = 0;
        g_poseV2SoundLast.clear();
        g_poseV2BodyImpactsPlayed = 0;
        g_poseV2BodyImpactsRefused = 0;
        g_poseV2BodyImpactLast.clear();
        g_poseV2MeleePlayed = 0;
        g_poseV2MeleeRefused = 0;
        g_poseV2MeleeLastStatus.clear();
        g_meleeCheck.Reset();
        g_poseV2FootstepsPlayed = 0;
        g_poseV2FootstepsRefused = 0;
        g_poseV2FootstepsPuppetEngine = 0;
        g_poseV2FxPlayed = 0;
        g_poseV2FxNoPuppet = 0;
        g_poseV2FxNoWorldPlay = 0;
        g_poseV2FxFailed = 0;
        g_poseV2FxLastError.clear();
        g_poseV2RegisteredPlay.clear();
        g_fxPendingEntries.clear();
        g_poseV2RegisteredCreated = 0;
        g_poseV2RegisteredUpdates = 0;
        g_poseV2RegisteredFailed = 0;
        g_poseV2RegisteredLastError.clear();
        g_poseV2RegisteredPlaneFailures = 0;
        g_poseV2RegisteredPlaneError.clear();
        g_poseV2RegisteredAttachMisses = 0;
        g_poseV2RegisteredAttachError.clear();
        g_poseV2FootstepLastError.clear();
        g_poseV2FootstepRefusals.clear();
        g_poseV2AudioStateLogged.clear();
        g_poseV2AudioStateSeen.clear();
        g_poseV2FootForced.clear();
        g_poseV2FootLodLogged.clear();
        g_poseV2FootLodWrites = 0;
        g_poseV2FootLodIds.clear();
        for (const replay::entity::FootstepEvent& step : Ctx().clip.footsteps.Events())
            g_poseV2FootLodIds.push_back(step.entity.Packed());
        std::sort(g_poseV2FootLodIds.begin(), g_poseV2FootLodIds.end());
        g_poseV2FootLodIds.erase(std::unique(g_poseV2FootLodIds.begin(), g_poseV2FootLodIds.end()), g_poseV2FootLodIds.end());
        // The recorded LODs per owner (the track is in time order).
        g_poseV2FootLodPoints.clear();
        g_poseV2FootLodRecordedWrites = 0;
        for (std::uint64_t& n : g_poseV2FootLodChosen)
            n = 0;
        for (const replay::entity::FootstepEvent& step : Ctx().clip.footsteps.Events())
            if (step.audioLod <= replay::footmaterial::kLodNoOverride)
                g_poseV2FootLodPoints[step.entity.Packed()].push_back({step.timeMs, step.audioLod});
        {
            std::vector<replay::audio::FootstepCall> stale;
            replay::audio::CollectFootstepsSince(g_poseV2FootstepEngineCursor, stale);
        }
        g_poseV2SpeechPlayed = 0;
        g_poseV2SpeechMatches = 0;
        g_poseV2SpeechMismatches = 0;
        g_poseV2SpeechVoiceInRequest = 0;
        g_poseV2SpeechRefused = 0;
        g_poseV2VoicePinned = 0;
        g_poseV2VoiceFailed = 0;
        g_poseV2VoiceAnimal = 0;
        g_poseV2SpeechStopsPlayed = 0;
        g_poseV2AnimalVocalRetries = 0;
        g_poseV2AnimalVocalCreated = 0;
        g_poseV2AnimalNativePlayed = 0;
        g_poseV2AnimalNativeFallbacks = 0;
        g_poseV2SoundTallies.Clear();
        {
            // The switches, read at each playback start from the game folder's ini.
            char directory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, directory);
            const std::string ini = replay::identity::IniPathIn(directory);
            replay::audio::SetFootWetnessReplay(
                replay::identity::IniInt("FootWetnessReplay", 1, ini.c_str()) != 0);
            // The recorded per-foot volume, filters and base volume of the hoof hits
            // are always written (foot_material.h FootMix, FootDrive); FootImpactReplay:
            // the recorded impact too (the puppet's stale ground plane would give a far
            // too loud impact on dirt and snow, 1 to 4.7 against 0.4 to 0.8 live).
            replay::audio::SetFootImpactReplay(replay::identity::IniInt("FootImpactReplay", 1, ini.c_str()) != 0);
            // The -2 dB term of the movement volume given back to the puppets when the
            // recording would not have applied it (foot_material.h
            // MovementTermCorrection).
            g_poseV2MovementTermBase = replay::audio::MovementTermSnapshot();
            // The recorded ground layers of each step and the recorded audio LOD are
            // used instead of LOD 0.
            replay::audio::SetFootLayerReplay(replay::identity::IniInt("FootLayerReplay", 1, ini.c_str()) != 0);
            g_poseV2PuppetAudioLodRecorded = replay::identity::IniInt("PuppetAudioLodRecorded", 1, ini.c_str()) != 0;
            // The puppets' own speech refused at the core.
            replay::audio::SetPuppetOwnSpeechBlock(replay::identity::IniInt("PuppetOwnSpeech", 1, ini.c_str()) != 0);
            // The recorded duck byte and sound set on every replayed human line (the
            // conversation lines out of the ambient set and its self duck)
            // (speech_conversation.h).
            g_speechConversationLive = replay::identity::IniInt("SpeechConversationLive", 1, ini.c_str()) != 0;
            // The replay's lines pass the rules' world context codes (28 another
            // speaker, 41 out of range, 48 a scripted conversation on), as a replay
            // editor's own say path skips the can-say rules (speech_replay_rules.h).
            replay::theatre::SetReplayLineRules(replay::identity::IniInt("SpeechReplayRules", 1, ini.c_str()) != 0);
        }
        replay::theatre::ClearReplayLines();
        g_poseV2PlayedSpeech.clear();
        g_poseV2SpeechLines = {};
        // The jump rule's state and counters.
        g_poseV2SpeechWatch.Reset();
        g_poseV2LearnedEndMs.clear();
        g_poseV2PendingResume.clear();
        g_poseV2RequestTally = {};
        g_poseV2StoppedCopies.clear();
        g_poseV2ResumeOldStopping = g_poseV2ResumeOldGone = g_poseV2ResumeOldTimeout = 0;
        g_poseV2ResumeOldWaitMaxMs = g_poseV2ResumeOldWaitTotalMs = 0;
        for (std::uint64_t& seen : g_poseV2ResumeOldSeen)
            seen = 0;
        g_poseV2ResumeOldLogs = 0;
        g_poseV2SpeechResumes = g_poseV2SpeechResumeAsked = g_poseV2SpeechResumeCreated = 0;
        g_poseV2SpeechResumeWithOffset = g_poseV2SpeechResumeNoPuppet = g_poseV2SpeechResumeAnimalSkipped = 0;
        g_poseV2SpeechResumeLate = g_poseV2SpeechJumpStops = g_poseV2SpeechLeaves = 0;
        g_poseV2SpeechCuts = g_poseV2SpeechShortEnds = g_poseV2SpeechLearnedEnds = 0;
        g_poseV2SilentByCause.clear();
        // The slot gate's state and counters.
        g_poseV2SlotQueue.clear();
        g_poseV2AmbientFailed.clear();
        g_poseV2SlotGateAsks = g_poseV2SlotGateWaits = g_poseV2SlotGateWaitMaxMs = g_poseV2SlotGateWaitAsked = 0;
        g_poseV2SlotGateEvicted = g_poseV2SlotGateDropInaudible = g_poseV2SlotGateDropLate = g_poseV2SlotGateDropPlay = 0;
        g_poseV2SlotIdleReleased = g_poseV2SlotIdleFailed = g_poseV2SlotCensusUnread = 0;
        g_poseV2SlotMinFree = -1;
        g_poseV2SourceAmbient = g_poseV2SourceBank = g_poseV2SourceOther = g_poseV2AmbientFallbacks = 0;
        g_poseV2AmbientRefusedRetries = g_poseV2AmbientRefusedRetryCreated = 0;
        g_poseV2AmbientKilledRetries = g_poseV2AmbientKilledRetryCreated = 0;
        g_poseV2State3Codes.clear();
        g_poseV2State3ByCode.clear();
        {
            std::vector<replay::theatre::State3Failure> stale;
            replay::theatre::CollectState3Failures(g_poseV2State3Cursor, stale);
        }
        replay::theatre::SetSpeechObserve(true);
        BeginPoseV2PuppetSpeech();
        g_poseV2AnimalContextFirst.clear();
        g_poseV2AnimalContextRetries = 0;
        BeginPoseV2AnimalBanks(firstCreateMs);
        g_poseV2AnimalLineCuts = 0;
        g_poseV2AnimalDeferDropped = 0;
        g_poseV2HumanOwnVoice.clear();
        g_poseV2OwnVoiceRetries = 0;
        g_poseV2OwnVoiceRetryCreated = 0;
        g_poseV2SlotWaits = g_poseV2SlotRetries = g_poseV2SlotRetryCreated = 0;
        g_poseV2DeferredSpeech.clear();
        g_poseV2SpeechDeferred = g_poseV2SpeechDeferStops = g_poseV2SpeechDeferDropped = g_poseV2SpeechDeferMaxWaitMs = 0;
        g_poseV2SpeechDeferOffsetLines = g_poseV2SpeechDeferLateDropped = 0;
        g_poseV2SpeechDeferOffsetMaxMs = 0;
        g_poseV2SpeechSyncLines = g_poseV2SpeechSyncLate = g_poseV2SpeechSyncAhead = 0;
        g_poseV2SpeechSyncWorstMs = 0;
        g_poseV2SpeechSyncWorstEventMs = 0;
        MeasurePoseV2SpeechLoad();
        {
            std::vector<replay::audio::SpeechLineStart> stale;
            replay::audio::CollectSpeechStartsSince(g_poseV2SpeechPlayStartCursor, stale);
        }
        g_poseV2QueuedLines.clear();
        g_poseV2QueuedLineBudget = 48; // 16 is too few: a long clip's later lines go unfollowed

        g_poseV2SpeechCancelled = 0;
        {
            std::string error;
            g_poseV2FxPlayerReady = replay::fx::EnsurePlayer(error);
            std::ostream& log = Ctx().log;
            log << "POSE_V2_FX_PLAYER"
                << ",status=" << (g_poseV2FxPlayerReady ? "ready" : "failed")
                << ",error=" << (error.empty() ? "none" : error.c_str())
                << '\n';
            log.flush();
        }
        g_poseV2ShotsPlayed = 0;
        g_poseV2ShotsCancelled = 0;
        BeginShotAudioDelay("play");
    }

    void Play(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.events"); // its parts have their own zones
        replay::fx::ReplayScope fxScope; // starts made in here are the replay's
        // What the replay plays from here is its own (the puppet one-shot rule,
        // puppet_audio_rule.h, refuses only the puppets' own).
        replay::audio::ReplayEmitScope emit;
        replay::census::Tick(Ctx(), "play", timeMs, Ctx().hooks && Ctx().hooks->NormalPlay());
        replay::mixprobe::Tick(Ctx(), "play", timeMs, Ctx().hooks && Ctx().hooks->NormalPlay());
        // The clip's render targets (and UI apps) at the playhead, in any play
        // state (a state, reconciled; a seek too).
        replay::rendertarget::Play(Ctx().clip.renderTargets, timeMs, Ctx().log);
        // The recorded mix groups and scenes before the lines of this tick are
        // asked; the probe after them.
        replay::mixscene::Play(Ctx(), timeMs);
        // The recorded category values and the speech duck's inputs at the
        // playhead, while the replay owns the world.
        replay::catstate::Play(Ctx().clip, timeMs, replay::theatre::Owning(), Ctx().log);
        replay::speechduck::Play(Ctx().clip, timeMs, replay::theatre::Owning(), Ctx().log);
        PlayPoseV2Shots(timeMs);
        TickShotAudioDelay(); // the replay's fires of earlier ticks
        PlayPoseV2Speech(timeMs);
        replay::speechlevel::Tick(Ctx(), true, timeMs);
        replay::volumeprobe::Tick(Ctx(), "play", timeMs, Ctx().hooks && Ctx().hooks->NormalPlay());
        replay::worldsoundprobe::Tick(Ctx(), "play", timeMs, Ctx().hooks && Ctx().hooks->NormalPlay());
        // The engine's wind strength against the recorded one.
        replay::windreplay::Play(Ctx().clip, timeMs, Ctx().hooks && Ctx().hooks->NormalPlay(), Ctx().log);
        UpdatePoseV2OwnFeet();
        PlayPoseV2Footsteps(timeMs);
        PlayFootEvents(timeMs);
        TickMovementDetail(false, timeMs); // evidence
        PublishFootMovement(timeMs);
        PlayPoseV2Explosions(timeMs);
        PlayPoseV2AnimAudio(timeMs);
        PlayPoseV2Surfaces(timeMs);
        PlayPoseV2Fx(timeMs);
        StopPoseV2EngineFx(timeMs);
        PlayPoseV2EntitySounds(timeMs);
        replay::capture::Play(Ctx(), timeMs);
        replay::soundgeneric::Play(Ctx(), timeMs); // the generic packets, after the capture
        PlayPoseV2RegisteredFx(timeMs);
        if (g_poseV2ResearchWindows)
            TracePoseV2Emitters("play", timeMs); // research
        PlayPoseV2Decals(timeMs);
        PlayPoseV2DecalRemoves(timeMs);
        PlayPoseV2GoreHits(timeMs);
        PlayPoseV2Blood(timeMs);
        CheckPoseV2FireClear();
        PlayPoseV2Fires(timeMs);
        PlayPoseV2Gore(timeMs);
        PlayPoseV2ProjDecals(timeMs);
        PlayPoseV2MeleeHits(timeMs);
        // The body impact function is not replayed: its hit sounds are
        // requests recorded by the generic sound path (PlayPoseV2Sounds).
        PlayPoseV2Sounds(timeMs);
        PlayPoseV2CollisionSounds(timeMs);
        TallyPoseV2FxCheckPlay(timeMs);
        if (g_poseV2FxObserverReady)
        {
            CheckFxPending();
            RebuildFxOwned();
        }
    }

    // The ESND rows of a playhead window and their latest outcome (the QA
    // mark's window).
    EntitySoundWindow EntitySoundWindowOf(std::uint32_t fromMs, std::uint32_t toMs)
    {
        EntitySoundWindow out;
        const auto& sounds = Ctx().clip.entitySounds;
        for (std::size_t index = 0; index < sounds.size(); ++index)
        {
            const replay::EntitySound& sound = sounds[index];
            if (sound.startMs < fromMs || sound.startMs > toMs)
                continue;
            const std::uint8_t outcome = index < g_poseV2EntitySoundOutcome.size() ? g_poseV2EntitySoundOutcome[index] : kEsndOutcomeNone;
            EntitySoundWindowOwner* row = nullptr;
            for (EntitySoundWindowOwner& candidate : out.owners)
                if (candidate.owner == sound.entity.Packed())
                    row = &candidate;
            if (!row)
            {
                EntitySoundWindowOwner fresh;
                fresh.owner = sound.entity.Packed();
                const auto refusal = g_poseV2EntitySoundLastRefusal.find(fresh.owner);
                if (refusal != g_poseV2EntitySoundLastRefusal.end())
                    fresh.lastStatus = refusal->second;
                out.owners.push_back(fresh);
                row = &out.owners.back();
            }
            const auto bump = [&](std::uint32_t EntitySoundWindow::*total, std::uint32_t EntitySoundWindowOwner::*mine) {
                ++(out.*total);
                ++(row->*mine);
            };
            bump(&EntitySoundWindow::rows, &EntitySoundWindowOwner::rows);
            switch (outcome)
            {
            case kEsndOutcomePlayed: bump(&EntitySoundWindow::played, &EntitySoundWindowOwner::played); break;
            case kEsndOutcomeRefused: bump(&EntitySoundWindow::refused, &EntitySoundWindowOwner::refused); break;
            case kEsndOutcomeTooFar: bump(&EntitySoundWindow::tooFar, &EntitySoundWindowOwner::tooFar); break;
            case kEsndOutcomeByPuppet: bump(&EntitySoundWindow::byPuppet, &EntitySoundWindowOwner::byPuppet); break;
            case kEsndOutcomePassed: bump(&EntitySoundWindow::passed, &EntitySoundWindowOwner::passed); break;
            case kEsndOutcomeToGeneric: bump(&EntitySoundWindow::toGeneric, &EntitySoundWindowOwner::toGeneric); break;
            default: bump(&EntitySoundWindow::untried, &EntitySoundWindowOwner::untried); break;
            }
        }
        std::stable_sort(out.owners.begin(), out.owners.end(),
            [](const EntitySoundWindowOwner& a, const EntitySoundWindowOwner& b) { return a.rows > b.rows; });
        if (out.owners.size() > 16)
            out.owners.resize(16);
        return out;
    }

    void EndPlayback()
    {
        std::ostream& log = Ctx().log;
        replay::rendertarget::EndPlayback(log); // what the replay registered and launched is undone
        g_poseV2BloodQueue.Reset();
        replay::blood::ForgetReadableRegions();
        if (g_poseV2FirePlaying)
        {
            ClearPoseV2Fires(true);
            replay::fire::SetFreeze(false);
            replay::fire::SetBlockSpread(false);
            replay::fire::SetObserving(false);
            if (g_poseV2PlayerProofs >= 0)
                ENTITY::SET_ENTITY_PROOFS(PLAYER::PLAYER_PED_ID(), g_poseV2PlayerProofs, FALSE);
            g_poseV2PlayerProofs = -1;
            g_poseV2FirePlaying = false;
            log << "POSE_V2_PLAYBACK_FIRE_TOTAL,recorded=" << Ctx().clip.fireStarts.Events().size() << ",played=" << g_poseV2FirePlayed
                << ",char_shielded=" << g_poseV2FireCharShielded
                << ",refused=" << g_poseV2FirePlayRefused << ",stopped=" << g_poseV2FireStopped << ",engine_ended=" << g_poseV2FireEngineEnded
                << ",reconciles=" << g_poseV2FireReconciles << ",restored=" << g_poseV2FireRestored << ",restore_missed=" << g_poseV2FireRestoreMissed
                << ",remnants_cleared=" << g_poseV2FireRemnantsCleared
                << ",entity_waits=" << g_poseV2FireEntityWaits << ",retries=" << g_poseV2FireRetries << ",gave_up=" << g_poseV2FireGaveUp
                << ",parent_cleared=" << g_poseV2FireParentCleared << ",incremental=" << g_poseV2FireIncremental
                << ",ended_queued=" << g_poseV2FireEndedQueued << ",duplicates=" << g_poseV2FireDuplicates
                << ",world_entity_found=" << g_poseV2FireWorldFound << ",world_entity_missing=" << g_poseV2FireWorldMissing
                << ",last_error=" << (g_poseV2FireLastError.empty() ? "none" : g_poseV2FireLastError.c_str()) << '\n';
        }
        if (g_poseV2FxObserverReady)
        {
            LogPoseV2FxHistogram("play");
            LogPoseV2FxRegistered("play");
        }
        EndPoseV2FxCheck(log);
        EndPoseV2EngineFxStop(log);
        EndPoseV2OwnFeet(log);
        replay::fx::SetEnabled(false);
        EndPoseV2AnimalBanks(log);
        {
            log << "POSE_V2_PLAYBACK_SPEECH_TOTAL"
                << ",recorded=" << Ctx().clip.speechLines.Events().size()
                << ",played=" << g_poseV2SpeechPlayed
                << ",cancelled=" << g_poseV2SpeechCancelled
                << ",matches=" << g_poseV2SpeechMatches
                << ",mismatches=" << g_poseV2SpeechMismatches
                << ",voice_in_request=" << g_poseV2SpeechVoiceInRequest
                << ",refused=" << g_poseV2SpeechRefused
                << ",voice_pinned=" << g_poseV2VoicePinned
                << ",voice_failed=" << g_poseV2VoiceFailed
                << ",voice_animal=" << g_poseV2VoiceAnimal
                << ",stops_played=" << g_poseV2SpeechStopsPlayed
                << ",animal_retries=" << g_poseV2AnimalVocalRetries
                << ",animal_retry_lines=" << g_poseV2AnimalVocalCreated
                << ",animal_native_played=" << g_poseV2AnimalNativePlayed
                << ",animal_native_fallbacks=" << g_poseV2AnimalNativeFallbacks
                << ",animal_context_first=" << g_poseV2AnimalContextFirst.size()
                << ",animal_context_retries=" << g_poseV2AnimalContextRetries
                << ",animal_line_cuts=" << g_poseV2AnimalLineCuts << ",animal_defer_dropped=" << g_poseV2AnimalDeferDropped
                // The puppets' own core calls.
                << ",own_speech_block=" << (replay::audio::PuppetOwnSpeechBlock() ? 1 : 0)
                << ",own_speech_seen=" << replay::audio::PuppetOwnSpeechSeen() - g_poseV2OwnSpeechSeenBase
                << ",own_speech_refused=" << replay::audio::PuppetOwnSpeechRefused() - g_poseV2OwnSpeechRefusedBase
                << ",deferred=" << g_poseV2SpeechDeferred << ",defer_stops=" << g_poseV2SpeechDeferStops
                << ",defer_dropped=" << g_poseV2SpeechDeferDropped + g_poseV2DeferredSpeech.size()
                << ",defer_max_wait_ms=" << g_poseV2SpeechDeferMaxWaitMs
                << ",defer_offset_lines=" << g_poseV2SpeechDeferOffsetLines
                << ",defer_offset_max_ms=" << g_poseV2SpeechDeferOffsetMaxMs
                << ",defer_late_dropped=" << g_poseV2SpeechDeferLateDropped
                << ",sync_lines=" << g_poseV2SpeechSyncLines << ",sync_late=" << g_poseV2SpeechSyncLate
                << ",sync_ahead=" << g_poseV2SpeechSyncAhead << ",sync_worst_ms=" << g_poseV2SpeechSyncWorstMs
                << ",sync_worst_event_ms=" << g_poseV2SpeechSyncWorstEventMs
                << ",transport_lost_ms=" << Ctx().transport.LostMs()
                << ",slot_waits=" << g_poseV2SlotWaits << ",slot_retries=" << g_poseV2SlotRetries
                << ",slot_retry_lines=" << g_poseV2SlotRetryCreated
                << ",own_voice_retries=" << g_poseV2OwnVoiceRetries << ",own_voice_retry_created=" << g_poseV2OwnVoiceRetryCreated
                << ",own_voice_puppets=" << g_poseV2HumanOwnVoice.size()
                << ",world_speech=" << replay::audio::WorldSpeechDuringPlayback() - g_poseV2WorldSpeechBase
                << ",world_speech_queued=" << replay::audio::WorldSpeechQueuedDuringPlayback() - g_poseV2WorldSpeechQueuedBase
                << ",on_player=" << g_speechOnPlayer
                << ",player_far=" << g_speechPlayerFar
                << ",player_held=" << g_speechHeld << ",player_held_started=" << g_speechHeldStarted
                << ",player_character_mismatch=" << g_speechCharacterMismatch
                << ",player_refused=" << g_speechPlayerRefused
                << ",player_refused_puppet=" << g_speechPlayerRefusedPuppetCreated
                << ",player_named_voice=" << g_speechPlayerNamedVoice
                << ",player_named_voice_created=" << g_speechPlayerNamedVoiceCreated
                // The jump rule (speech_resume.h).
                << ",resume=1,resumes=" << g_poseV2SpeechResumes
                << ",resume_asked=" << g_poseV2SpeechResumeAsked << ",resume_created=" << g_poseV2SpeechResumeCreated
                << ",resume_with_offset=" << g_poseV2SpeechResumeWithOffset
                << ",resume_late=" << g_poseV2SpeechResumeLate << ",resume_no_puppet=" << g_poseV2SpeechResumeNoPuppet
                << ",resume_pending_left=" << g_poseV2PendingResume.size()
                << ",resume_animal_skipped=" << g_poseV2SpeechResumeAnimalSkipped
                << ",jump_stops=" << g_poseV2SpeechJumpStops << ",leaves=" << g_poseV2SpeechLeaves
                << ",learned_ends=" << g_poseV2SpeechLearnedEnds
                << ",speech_cuts=" << g_poseV2SpeechCuts << ",speech_short_ends=" << g_poseV2SpeechShortEnds
                // The slot gate (speech_slots.h).
                << ",slot_manager=1,ambient_source=0"
                << ",audible_m=" << kPoseV2AudibleM
                << ",source_ambient=" << g_poseV2SourceAmbient << ",source_bank=" << g_poseV2SourceBank
                << ",source_other=" << g_poseV2SourceOther << ",ambient_fallbacks=" << g_poseV2AmbientFallbacks
                << ",ambient_refused_retries=" << g_poseV2AmbientRefusedRetries
                << ",ambient_refused_retry_created=" << g_poseV2AmbientRefusedRetryCreated
                << ",ambient_killed_retries=" << g_poseV2AmbientKilledRetries
                << ",ambient_killed_retry_created=" << g_poseV2AmbientKilledRetryCreated
                << ",gate_waits=" << g_poseV2SlotGateWaits << ",gate_wait_asked=" << g_poseV2SlotGateWaitAsked
                << ",gate_wait_max_ms=" << g_poseV2SlotGateWaitMaxMs << ",gate_waiting_left=" << g_poseV2SlotQueue.size()
                << ",gate_evicted=" << g_poseV2SlotGateEvicted << ",gate_drop_inaudible=" << g_poseV2SlotGateDropInaudible
                << ",gate_drop_late=" << g_poseV2SlotGateDropLate << ",gate_drop_jump=" << g_poseV2SlotGateDropPlay
                << ",gate_asks_short=" << g_poseV2SlotGateAsks << ",idle_released=" << g_poseV2SlotIdleReleased
                << ",idle_release_failed=" << g_poseV2SlotIdleFailed << ",census_unread=" << g_poseV2SlotCensusUnread
                << ",min_free=" << g_poseV2SlotMinFree;
            {
                // The state 3 handler's failure codes.
                std::string codes;
                for (const auto& [code, count] : g_poseV2State3ByCode)
                    codes += (codes.empty() ? "" : "|") + std::to_string(code) + ":" + std::to_string(count);
                log << ",state3_codes=" << (codes.empty() ? std::string(replay::theatre::State3HandlerInstalled() ? "none" : "off") : codes);
                // The replay's lines let past the rules' world context codes.
                std::uint32_t other = 0, range = 0, conversation = 0;
                replay::theatre::ReplayRulesBypassed(other, range, conversation);
                log << ",replay_rules=" << (replay::theatre::ReplayLineRulesReady() ? "on" : "off")
                    << ",rules_passed=28:" << other << "|41:" << range << "|48:" << conversation;
            }
            replay::theatre::SetSpeechObserve(false);
            {
                // The silent lines by cause.
                std::string causes;
                for (const auto& [cause, count] : g_poseV2SilentByCause)
                    causes += (causes.empty() ? "" : "|") + cause + ":" + std::to_string(count);
                log << ",silent_causes=" << (causes.empty() ? std::string("none") : causes);
            }
            {
                // Created, audible, silent.
                std::uint64_t pending = 0;
                for (const PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech)
                    pending += played.judged ? 0u : 1u;
                log << g_poseV2SpeechLines.Text(pending, replay::audio::SpeechStartObserved());
            }
            log << '\n';
            {
                // The line type and request fields asked again, and the resumes held for
                // the old copy.
                const replay::speech::request::Tally& t = g_poseV2RequestTally;
                log << "SPEECH_REQUEST_TOTAL,lines=" << t.lines
                    << ",type_asked=" << t.typeAsked << ",type_kept=" << t.typeKept << ",type_other=" << t.typeOther
                    << ",fields_asked=" << t.fieldsAsked << ",unread=" << t.unread << ",same=" << t.same
                    << ",field_diffs=" << t.FieldText()
                    << ",resume_old_gone_at_ask=" << g_poseV2ResumeOldSeen[0]
                    << ",resume_old_stopping=" << g_poseV2ResumeOldStopping
                    << ",resume_old_active=" << g_poseV2ResumeOldSeen[2] << ",resume_old_other=" << g_poseV2ResumeOldSeen[3]
                    << ",resume_old_unread=" << g_poseV2ResumeOldSeen[4]
                    << ",resume_old_waited_gone=" << g_poseV2ResumeOldGone << ",resume_old_timeout=" << g_poseV2ResumeOldTimeout
                    << ",resume_old_wait_max_ms=" << g_poseV2ResumeOldWaitMaxMs
                    << ",resume_old_wait_total_ms=" << g_poseV2ResumeOldWaitTotalMs << '\n';
            }
            {
                // The duck byte and the sound set asked again, and what the lines got
                // (speech_conversation.h).
                const replay::speech::conversation::Tally& c = g_speechConversationTally;
                std::string failed = "off";
                const bool callsOk = g_speechConversationLive && replay::audio::SpeechSoundSetCallsOk(failed);
                log << "SPEECH_CONVERSATION_TOTAL,on=" << (g_speechConversationLive ? 1 : 0)
                    << ",calls_ok=" << (callsOk ? 1 : 0) << ",calls_failed=" << failed << ",lines=" << c.lines
                    << ",duck_asked=" << c.duckAsked << ",duck_cleared=" << c.duckCleared << ",duck_kept=" << c.duckKept
                    << ",set_recorded=" << c.setRecorded << ",set_from_sounds=" << c.setSounds << ",set_none=" << c.setNone
                    << ",set_kept=" << c.setKept << ",set_other=" << c.setOther << ",set_unread=" << c.setUnread
                    << ",set_status=" << c.StatusText()
                    << ",plain_retries=" << g_speechConversationPlainRetries
                    << ",plain_created=" << g_speechConversationPlainCreated << '\n';
            }
            LogPoseV2SoundTallies(log);
            log << "POSE_V2_PLAYBACK_MELEE_TOTAL"
                << ",recorded=" << Ctx().clip.meleeHits.Events().size()
                << ",played=" << g_poseV2MeleePlayed
                << ",refused=" << g_poseV2MeleeRefused
                << ",last=" << (g_poseV2MeleeLastStatus.empty() ? "none" : g_poseV2MeleeLastStatus.c_str())
                << g_meleeCheck.TotalFields()
                << '\n';
            StopPoseV2CollisionSounds();
            log << "POSE_V2_PLAYBACK_ENTITY_SOUNDS,recorded=" << Ctx().clip.entitySounds.size()
                << ",fire_via_fallback=" << g_poseV2ShotFireViaFallback
                << ",played=" << g_poseV2EntitySoundsPlayed << ",refused=" << g_poseV2EntitySoundsRefused
                << ",late=" << g_poseV2EntitySoundsLate
                << ",stolen=" << g_poseV2EntitySoundsStolen << ",too_far=" << g_poseV2EntitySoundsTooFar
                << ",ended_in_span=" << g_poseV2EntitySoundsEnded
                << ",ped_audio_played=" << g_poseV2PedAudioPlayed << ",ped_audio_by_puppet=" << g_poseV2PedAudioByPuppet
                << ",puppet_creates=" << g_poseV2PuppetCreatesSeen
                << ",length_stops=" << g_poseV2PedAudioLengthStops
                << ",by_speech_line=" << g_poseV2EntitySoundsBySpeech
                << ",speech_fallback=" << g_poseV2EntitySoundsSpeechFallback
                << ",to_generic=" << g_poseV2EntitySoundsToGeneric
                << ",requests_to_generic=" << g_poseV2SoundsToGeneric << ",collisions_to_generic=" << g_poseV2CollisionToGeneric
                << ",anim_audio_to_generic=" << g_poseV2AnimToGeneric
                << ",last=" << (g_poseV2EntitySoundLast.empty() ? "none" : g_poseV2EntitySoundLast.c_str()) << '\n';
            replay::audio::SetObserveEntityCreates(false);
            // The captured sounds stop; the playback's census is compared with the
            // recording's (ReplayDebug\<clip>.sound-check.txt). The generic player's
            // section joins the report (empty when the clip has no packets); it stops
            // its sounds here.
            const std::string genericSection = replay::soundgeneric::EndPlayback(Ctx());
            // The ESND track per family, in the log (every family) and in the report.
            for (const auto& family : g_poseV2EsndTally.Families())
                log << replay::esndtally::LogLine(family.first, family.second) << '\n';
            // The notice, and the hits played mute.
            std::vector<replay::census::Finding> extraFindings = replay::soundgeneric::LastFindings();
            for (replay::census::Finding& finding : g_meleeCheck.Findings(replay::census::RecordedPlayerId()))
                extraFindings.push_back(std::move(finding));
            replay::census::NoteExtraFindings(std::move(extraFindings));
            const std::string meleeSection = g_meleeCheck.Section(Ctx().clip.meleeHits.Events().size(),
                replay::soundgeneric::MeleeComplementsPlayed());
            // The comparison of where each sound is heard from (its own file in
            // ReplayDebug, and a section of the sound-check report when that one is
            // written).
            const std::string mixSection = replay::mixprobe::EndPlayback(Ctx());
            replay::mixscene::EndPlayback(Ctx()); // scenes stop, groups come off
            replay::speechduck::EndPlayback(Ctx().clip, Ctx().log);
            replay::catstate::EndPlayback(Ctx().clip, Ctx().log);
            replay::speechlevel::EndPlayback(Ctx());
            replay::volumeprobe::EndPlayback(Ctx());
            replay::worldsoundprobe::EndPlayback(Ctx());
            replay::windreplay::EndPlayback(Ctx().clip, Ctx().log);
            replay::census::EndPlayback(Ctx(), replay::capture::EndPlayback(Ctx()),
                replay::horsesound::SummaryLine(g_poseV2SoundTallies),
                mixSection + replay::horsesound::FormatSection(g_poseV2SoundTallies) + genericSection + meleeSection +
                    replay::esndtally::FormatSection(g_poseV2EsndTally));
            log << "POSE_V2_PLAYBACK_COLLISION_SOUNDS"
                << ",recorded=" << Ctx().clip.collisionSounds.size()
                << ",played=" << g_poseV2CollisionPlayed
                << ",refused=" << g_poseV2CollisionRefused
                << ",last=" << (g_poseV2CollisionLast.empty() ? "none" : g_poseV2CollisionLast.c_str())
                << '\n';
            log << "SHOT_ENV_TOTAL,phase=play,weapon_sound_given=" << g_shotEnvGiven << ",weapon_sound_missing="
                << g_shotEnvMissing << ",rule=" << (g_shotEnvGroupReplay ? 1 : 0) << '\n';
            g_shotEnvGiven = g_shotEnvMissing = 0;
            g_shotEnvLines = 0;
            log << "POSE_V2_PLAYBACK_SOUNDS"
                << ",recorded=" << Ctx().clip.sounds.Events().size()
                << ",played=" << g_poseV2SoundsPlayed
                << ",refused=" << g_poseV2SoundsRefused
                << ",neighbor_dropped=" << g_poseV2SoundsNeighbor
                << ",last=" << (g_poseV2SoundLast.empty() ? "none" : g_poseV2SoundLast.c_str())
                << '\n';
            log << "POSE_V2_PLAYBACK_BODY_IMPACT_TOTAL"
                << ",recorded=" << Ctx().clip.bodyImpacts.Events().size()
                << ",played=" << g_poseV2BodyImpactsPlayed
                << ",refused=" << g_poseV2BodyImpactsRefused
                << ",last=" << (g_poseV2BodyImpactLast.empty() ? "none" : g_poseV2BodyImpactLast.c_str())
                << '\n';
            log << "POSE_V2_PLAYBACK_FOOTSTEPS"
                << ",recorded=" << Ctx().clip.footsteps.Events().size()
                << ",played=" << g_poseV2FootstepsPlayed
                << ",refused=" << g_poseV2FootstepsRefused
                << ",last_error=" << (g_poseV2FootstepLastError.empty() ? "none" : g_poseV2FootstepLastError.c_str())
                << ",puppet_engine_steps=" << g_poseV2FootstepsPuppetEngine
                << ",lod_puppets=" << g_poseV2FootLodIds.size()
                << ",lod_writes=" << g_poseV2FootLodWrites
                << ",lod_recorded=" << (g_poseV2PuppetAudioLodRecorded ? 1 : 0)
                << ",lod_recorded_puppets=" << g_poseV2FootLodPoints.size()
                << ",lod_recorded_writes=" << g_poseV2FootLodRecordedWrites
                << ",lod_chosen=0:" << g_poseV2FootLodChosen[0] << "|1:" << g_poseV2FootLodChosen[1] << "|2:"
                << g_poseV2FootLodChosen[2] << "|3:" << g_poseV2FootLodChosen[3]
                << '\n';
            for (const auto& refusal : g_poseV2FootstepRefusals)
                log << "POSE_V2_PLAYBACK_FOOTSTEP_REFUSED," << refusal.first << ",count=" << refusal.second << '\n';
            {
                std::vector<std::uint8_t> snapshot;
                if (replay::fx::TakeReplaySnapshot(snapshot))
                    LogPoseV2FxSnapshot("replay", snapshot);
            }
            log << "POSE_V2_PLAYBACK_REGISTERED_FX,tracks=" << Ctx().clip.registeredFx.size() << ",crossed=" << g_poseV2RegisteredCrossed
                << ",created=" << g_poseV2RegisteredCreated << ",updates=" << g_poseV2RegisteredUpdates
                << ",failed=" << g_poseV2RegisteredFailed
                << ",last_error=" << (g_poseV2RegisteredLastError.empty() ? "none" : g_poseV2RegisteredLastError.c_str())
                << ",plane_failures=" << g_poseV2RegisteredPlaneFailures
                << ",plane_error=" << (g_poseV2RegisteredPlaneError.empty() ? "none" : g_poseV2RegisteredPlaneError.c_str())
                << ",attach_misses=" << g_poseV2RegisteredAttachMisses
                << ",attach_error=" << (g_poseV2RegisteredAttachError.empty() ? "none" : g_poseV2RegisteredAttachError.c_str())
                << ",attach_changes_applied=" << g_poseV2FxAttachPlayApplied
                << ",attach_changes_waiting=" << g_poseV2FxAttachPlayWaiting
                << ",camera_ambient_ticks_skipped=" << g_poseV2CameraAmbientPlaySkipped << '\n';
            // The live instances leave with the playback (killed, as a replay editor
            // clears the world when playback ends).
            for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
                if (entry->track < Ctx().clip.registeredFx.size())
                    EndPoseV2RegisteredEntry(*entry, Ctx().clip.registeredFx[entry->track], 0, true, false);
            log << "POSE_V2_FX_REGISTERED_END_SUMMARY,ended=" << g_poseV2RegisteredEnd.ended
                << ",stopped=" << g_poseV2RegisteredEnd.stopped << ",stop_failed=" << g_poseV2RegisteredEnd.stopFailed
                << ",left_to_engine=" << g_poseV2RegisteredEnd.stopNotOurs
                << ",scrub_killed=" << g_poseV2RegisteredEnd.scrubKilled << ",owner_gone=" << g_poseV2RegisteredEnd.ownerGone
                << ",owner_missing_ticks=" << g_poseV2RegisteredEnd.ownerMissing
                << ",owner_mismatch_ticks=" << g_poseV2RegisteredEnd.ownerMismatch
                << ",not_created_paused_ticks=" << g_poseV2RegisteredEnd.notCreatedPaused
                << ",after1s_alive_ours=" << g_poseV2RegisteredEnd.afterAlive
                << ",after1s_finishing=" << g_poseV2RegisteredEnd.afterFinishing
                << ",after1s_reused=" << g_poseV2RegisteredEnd.afterReused
                << ",after1s_unreadable=" << g_poseV2RegisteredEnd.afterUnreadable
                << ",after1s_pending=" << g_poseV2RegisteredEnded.size()
                << ",stop_error=" << (g_poseV2RegisteredEnd.lastStopError.empty() ? "none" : g_poseV2RegisteredEnd.lastStopError.c_str())
                << '\n';
            g_poseV2RegisteredPlay.clear();
            g_fxPendingEntries.clear();
            g_poseV2RegisteredEnded.clear();
            // A replay editor clears the world of its decals when playback ends.
            std::uint64_t cleared = 0;
            std::uint64_t alive = 0;
            std::set<std::uint32_t> ours;
            for (const auto& entry : g_poseV2DecalIds)
                ours.insert(entry.second);
            for (const std::uint32_t id : ours)
            {
                std::string error;
                if (replay::decal::IsDecalAlive(id, error))
                    ++alive;
                if (replay::decal::RemoveDecal(id, error))
                    ++cleared;
            }
            // Removed ones the core built after their remove.
            std::uint64_t lateCleared = 0;
            for (const PoseV2DecalRemoveRetry& retry : g_poseV2DecalRemoveRetries)
            {
                std::string error;
                if (!ours.count(retry.id) && replay::decal::IsDecalAlive(retry.id, error) && replay::decal::RemoveDecal(retry.id, error))
                    ++lateCleared;
            }
            g_poseV2DecalRemoveRetries.clear();
            g_poseV2DecalIds.clear();
            log << "POSE_V2_PLAYBACK_BLOOD_TOTAL,recorded=" << Ctx().clip.blood.Events().size() << ",played=" << g_poseV2BloodPlayed
                << ",failed=" << g_poseV2BloodFailed << ",already_present=" << g_poseV2BloodPresent << ",busy_waits=" << g_poseV2BloodBusy << ",reconciles=" << g_poseV2BloodReconciles
                << ",pending_left=" << g_poseV2BloodPending.size() << ",evolved=" << g_poseV2BloodEvolved
                << ",evolve_present=" << g_poseV2BloodEvolvePresent << ",evolve_failed=" << g_poseV2BloodEvolveFailed
                << ",last_error=" << (g_poseV2BloodLastError.empty() ? "none" : g_poseV2BloodLastError.c_str());
            {
                // The readability checks since load, by VirtualQuery and by the page's
                // working set entry.
                std::uint64_t queries = 0, hits = 0, pageAnswers = 0;
                double queryMs = 0.0, pageMs = 0.0;
                replay::blood::QueryStats(queries, hits, queryMs);
                replay::blood::PageAnswerStats(pageAnswers, pageMs);
                log << ",readable_queries_total=" << queries << ",readable_query_ms_total=" << queryMs
                    << ",readable_cache_hits_total=" << hits << ",readable_page_answers_total=" << pageAnswers
                    << ",readable_page_ms_total=" << pageMs << '\n';
            }
            log << "POSE_V2_PLAYBACK_GORE_HITS_TOTAL,recorded=" << Ctx().clip.goreHits.Events().size() << ",played=" << g_poseV2GoreHitsPlayed
                << ",failed=" << g_poseV2GoreHitsFailed << ",no_puppet=" << g_poseV2GoreHitsNoPuppet << ",mask_skipped=" << g_poseV2GoreMaskSkipped << '\n';
            log << "POSE_V2_PLAYBACK_GORE_TOTAL,recorded=" << Ctx().clip.gore.Events().size() << ",played=" << g_poseV2GorePlayed
                << ",failed=" << g_poseV2GoreFailed << ",no_puppet=" << g_poseV2GoreNoPuppet
                << ",last_error=" << (g_poseV2GoreLastError.empty() ? "none" : g_poseV2GoreLastError.c_str()) << '\n';
            log << "POSE_V2_PLAYBACK_SURFACES,recorded=" << Ctx().clip.surfaces.Events().size() << ",applied=" << g_poseV2SurfacesApplied
                << ",failed=" << g_poseV2SurfacesFailed << ",last_error=" << (g_poseV2SurfaceLastError.empty() ? "none" : g_poseV2SurfaceLastError.c_str()) << '\n';
            {
                std::ostringstream refusals;
                for (const auto& [status, count] : g_poseV2AnimRefusals)
                    refusals << status << ":" << count << ";";
                log << "POSE_V2_ANIM_AUDIO,phase=play,kind=total,recorded=" << Ctx().clip.animAudio.Events().size()
                    << ",played=" << g_poseV2AnimPlayed << ",refused=" << g_poseV2AnimRefused << ",no_puppet=" << g_poseV2AnimNoPuppet
                    << ",retried=" << g_poseV2AnimRetried << ",consumed=" << g_poseV2AnimConsumed
                    << ",not_consumed=" << g_poseV2AnimNotConsumed
                    << ",refusals=" << (refusals.str().empty() ? "none" : refusals.str()) << '\n';
            }
            log << "POSE_V2_PLAYBACK_EXPLOSION_AUDIO,recorded=" << Ctx().clip.explosions.Events().size() << ",played=" << g_poseV2ExplosionsPlayed
                << ",refused=" << g_poseV2ExplosionsRefused << ",last_error=" << (g_poseV2ExplosionLastError.empty() ? "none" : g_poseV2ExplosionLastError.c_str()) << '\n';
            log << "POSE_V2_PLAYBACK_PROJ_DECALS,recorded=" << Ctx().clip.projDecals.Events().size() << ",played=" << g_poseV2ProjPlayed
                << ",missing_entity=" << g_poseV2ProjMissing << ",no_info=" << g_poseV2ProjNoInfo << ",failed=" << g_poseV2ProjFailed
                << ",null_owner=" << g_poseV2ProjNullOwner << ",engine_seen=" << g_poseV2ProjEngineSeen
                << ",engine_mirrors=" << g_poseV2ProjEngineMirrors
                << ",last_error=" << (g_poseV2ProjLastError.empty() ? "none" : g_poseV2ProjLastError.c_str()) << '\n';
            log << "POSE_V2_PLAYBACK_DECALS,recorded=" << Ctx().clip.decals.Events().size() << ",played=" << g_poseV2DecalsPlayed
                << ",replaced=" << g_poseV2DecalsReplaced << ",removed=" << g_poseV2DecalsRemoved << ",removes_unmapped=" << g_poseV2DecalRemovesUnmapped
                << ",ours=" << ours.size() << ",alive_at_end=" << alive << ",cleared=" << cleared
                << ",engine_removed=" << g_poseV2EngineDecalsRemoved << ",engine_on_peds=" << g_poseV2EngineDecalsOnPeds
                << ",remove_retried=" << g_poseV2DecalRemoveRetried << ",late_cleared=" << lateCleared
                << ",skipped_internal=" << g_poseV2DecalsSkippedInternal << ",no_entity=" << g_poseV2DecalsNoEntity
                << ",no_info=" << g_poseV2DecalsNoInfo << ",info_unknown_at_record=" << g_poseV2DecalsInfoUnknown
                << ",entity_passed=" << g_poseV2DecalsEntityPassed << ",entity_missing=" << g_poseV2DecalsEntityMissing
                << ",aged=" << g_poseV2DecalsAgedPlay << ",expired=" << g_poseV2DecalsExpiredPlay
                << ",chains=" << g_poseV2DecalChains.size() << ",reconciles=" << g_poseV2DecalReconciles
                << ",state_removed=" << g_poseV2DecalStateRemoved << ",state_restored=" << g_poseV2DecalStateRestored
                << ",regrows=" << g_poseV2DecalRegrows << ",regrow_refused=" << g_poseV2DecalRegrowRefused
                << ",regrow_gave_up=" << g_poseV2DecalRegrowGaveUp << ",regrow_deferred=" << g_poseV2DecalRegrowDeferred
                << ",regrow_last_error=" << (g_poseV2DecalRegrowLastError.empty() ? "none" : g_poseV2DecalRegrowLastError.c_str())
                << ",failed=" << g_poseV2DecalsFailed << ",last_error=" << (g_poseV2DecalLastError.empty() ? "none" : g_poseV2DecalLastError.c_str());
            {
                // What never reached the core, this playback (decal_add_guard.h).
                const replay::decal::guard::Refusals now = replay::decal::GuardRefusals();
                const replay::decal::guard::Refusals& was = g_poseV2DecalGuardAtBegin;
                log << ",null_info=" << g_poseV2DecalsNullInfo << ",guard_info_null=" << (now.infoNull - was.infoNull)
                    << ",guard_info_unreadable=" << (now.infoUnreadable - was.infoUnreadable)
                    << ",guard_info_no_data=" << (now.infoNoData - was.infoNoData)
                    << ",guard_id_out_of_range=" << (now.idOutOfRange - was.idOutOfRange) << ",guard_id_dead=" << (now.idDead - was.idDead)
                    << ",guard_ring_stale=" << (now.ringStale - was.ringStale) << ",dead_id_readded=" << g_poseV2DecalsDeadIdReadd;
            }
            {
                // Did the replay's adds and the engine's deferred decal job ever meet at
                // the ring.
                const replay::decal::GateCounters gate = replay::decal::GateStats();
                log << ",gate=" << (replay::decal::DrainGateInstalled() ? 1 : 0) << ",gate_entries=" << gate.entries
                    << ",gate_waits=" << gate.waits << ",gate_wait_us_max=" << gate.waitUsMax << ",gate_wait_us_total=" << gate.waitUsTotal
                    << ",gate_timeouts=" << gate.timeouts << ",gate_reentries=" << gate.reentries << '\n';
            }
            log << "POSE_V2_PLAYBACK_FX"
                << ",recorded=" << Ctx().clip.fx.Events().size()
                << ",played=" << g_poseV2FxPlayed
                << ",no_puppet=" << g_poseV2FxNoPuppet
                << ",no_world_position=" << g_poseV2FxNoWorldPlay
                << ",failed=" << g_poseV2FxFailed
                << ",last_error=" << (g_poseV2FxLastError.empty() ? "none" : g_poseV2FxLastError.c_str())
                << '\n';
            log << "POSE_V2_PLAYBACK_SHOTS"
                << ",recorded=" << Ctx().clip.shots.Shots().size()
                << ",played=" << g_poseV2ShotsPlayed
                << ",cancelled=" << g_poseV2ShotsCancelled
                << '\n';
            {
                // The replayed gunshots' start per shooter, beside the clip's own
                // reference.
                const auto refs = ShotAudioClipReference();
                EndShotAudioDelay(&refs);
            }
            // The puppets stop being the player for the weapon audio with the
            // playback.
            std::size_t playerShots = 0;
            for (const replay::entity::ShotEvent& shot : Ctx().clip.shots.Shots())
                playerShots += shot.shooterPlayer == 2 ? 1 : 0;
            LogPoseV2PlayerAudioTotal("play_total", playerShots);
            LogPoseV2PlayerMountAudioTotal("play_total");
            EndEntityQuery("play_total");
            EndPedAudioState("play_total");
            EndFootEvents("play_total");
            replay::horseaudio::End(Ctx().log);
            EndPoseV2PlayerMountAudio();
            EndPoseV2PlayerWeaponBank(true);
            replay::audio::SetPlayerAudioPeds({});
            g_poseV2PlayerAudioPeds.clear();
            log.flush();
        }
    }

    // A script restart with a playback open. EndPlayback above stops fires,
    // registered fx and decals by their engine ids and pointers and puts the
    // player's proofs back: all of them the old world's (a registered fx kept
    // across a load crashes). Here the tables are emptied and the detours'
    // lists cleared; the sounds in the engine's weak slots are stopped by the
    // existing guarded path (the engine nulls a slot when its sound ends, so a
    // non zero slot is still our sound). The decal monitor is emptied too,
    // with the info key cache: across a load its decals' info pointers point
    // into the freed info table and their engine ids are handed out again, so
    // a new recording would take them as its initial decals.
    std::uint32_t ForgetDecalMonitorAfterRestart()
    {
        const std::uint32_t dropped = static_cast<std::uint32_t>(g_poseV2DecalMonitor.size());
        g_poseV2DecalMonitor.clear();
        g_poseV2MonitorCheck.clear();
        g_poseV2MonitorCheckAt = 0;
        g_poseV2DecalInitialPending = false;
        // The rings' cursors stay: what was added before the load is skipped.
        {
            std::vector<replay::decal::AddCall> stale;
            replay::decal::CollectSince(g_poseV2MonitorAddCursor, stale);
            std::vector<replay::decal::RemoveCall> staleRemoves;
            replay::decal::CollectRemovesSince(g_poseV2MonitorRemoveCursor, staleRemoves);
        }
        replay::decal::ClearInfoKeys();
        ++g_poseV2DecalMonitorSession;
        Ctx().log << "POSE_V2_DECAL_MONITOR_CLEARED,why=session_reset,dropped=" << dropped << ",resets=" << g_poseV2DecalMonitorSession
            << '\n';
        return dropped;
    }

    std::uint32_t ForgetPlaybackAfterRestart()
    {
        std::uint32_t forgotten = 0;
        replay::rendertarget::ForgetAfterRestart();
        forgotten += static_cast<std::uint32_t>(g_poseV2RegisteredPlay.size() + g_poseV2RegisteredEnded.size());
        forgotten += static_cast<std::uint32_t>(g_poseV2DecalIds.size() + g_poseV2DecalRemoveRetries.size());
        for (const OurFire& ours : g_poseV2FiresOurs)
            forgotten += ours.alive ? 1u : 0u;
        g_poseV2RegisteredPlay.clear();
        g_fxPendingEntries.clear();
        g_poseV2RegisteredEnded.clear();
        // Nothing of the old world's instances is kept.
        g_fxOwned.clear();
        g_fxPlacedThisTick.clear();
        g_fxProbeBefore.clear();
        g_fxPendingChecks.clear();
        g_fxWorldOwnersLive.clear();
        replay::fx::SetPauseFreeze(false);
        replay::fx::SetWeaponSitesExempt(true);
        replay::vegetation::SetFxVelocity(false);
        g_poseV2DecalIds.clear();
        g_poseV2DecalRemoveRetries.clear();
        g_poseV2DecalChains.clear();
        g_poseV2BloodQueue.Reset();
        g_poseV2BloodPending.clear();
        replay::blood::ForgetReadableRegions();
        // Fires: our fire records are engine pointers of the old world.
        g_poseV2FiresOurs.clear();
        g_poseV2FireEndedQueue.clear();
        g_poseV2FireRadius = 0.0f;
        replay::fire::SetFrozenOwners({});
        replay::fire::SetFreeze(false);
        replay::fire::SetBlockSpread(false);
        replay::fire::SetObserving(false);
        g_poseV2FirePlaying = false;
        g_poseV2PlayerProofs = -1; // the new player ped has the save's proofs
        // The detours' lists of puppet owners, feet and audio holders.
        replay::fx::SetSuppressedOwners({});
        g_poseV2EngineFxOwnerCount = 0;
        replay::audio::SetSilencedFootAudios({});
        replay::audio::SetSilencedPuppetAudios({});
        replay::audio::SetPuppetSpeechEntities({}, {});
        replay::audio::SetSilencedPuppetWeaponAudios({});
        replay::audio::SetOwnPersistentCallers({});
        replay::audio::SetOwnPersistentKeys({});
        replay::audio::ClearFootMaterials();
        // The speed detour's table holds the old world's puppet audio holders.
        // The load freed them and the engine hands the addresses to new peds (the
        // player and his horse included): without this a new ped in an old slot
        // would read the puppet's last recorded speed until the next playback or
        // recording. EndPedAudioState does not run here.
        replay::pedaudiostate::SetSpeedTargets({});
        g_pedSpeedTargets.clear();
        g_pedSpeedPublished = false;
        // The motion detour keys on the old puppets' peds, the same reuse.
        replay::pedaudiostate::SetMotionTargets({});
        g_motionTargets.clear();
        g_motionPublished = false;
        replay::audio::SetFootEventRefuseOwn(false);
        g_footEventRefusing = false;
        replay::audio::SetMovementDetail(false);
        // The movement volume detour's entries name the old puppets' peds, the
        // same address reuse as the speed table above; EndFootEvents does not run
        // here.
        replay::audio::SetMovementReplay(false);
        for (std::size_t n = 0; n < replay::audio::RecordedMovementSlots(); ++n)
            replay::audio::PublishRecordedMovement(n, 0, 0, nullptr);
        // The start sites the old playback refused (MixSceneOwnPlayerState) are
        // refused no more.
        replay::theatre::SetSceneRefusals({});
        // The category values and the duck inputs of the old playback stop now
        // (otherwise they stop by themselves only once the publication goes stale,
        // 1 to 2 s into the new world).
        replay::catstate::ForgetPlayback();
        replay::speechduck::ForgetPlayback();
        replay::pedaudiostate::SetNamedRanges({});
        replay::fx::SetEngineFootVfxSuppressed(false);
        replay::fx::SetEnabled(false);
        EndPoseV2PlayerMountAudio();
        BeginPoseV2PlayerWeaponBank(); // session reset, no natives
        g_pwbVerifyAtMs = 0; // the old world's loadout check
        replay::audio::SetPlayerAudioPeds({});
        g_poseV2PlayerAudioPeds.clear();
        g_poseV2FootLodIds.clear(); // the LOD override lived on the old puppets
        g_poseV2FootLodPoints.clear();
        // Sounds: the weak slots (StopPersistentSound checks the engine's
        // pool and the engine zeroes a slot when its sound ends).
        StopPoseV2CollisionSounds();
        replay::audio::SetObserveEntityCreates(false);
        forgotten += replay::soundgeneric::ForgetPlaybackAfterRestart(Ctx());
        replay::capture::ForgetPlaybackAfterRestart();
        replay::census::ForgetPlaybackAfterRestart();
        return forgotten;
    }
}
