// script_clip_world.cpp: the clip world: the clip load wait, the clip's weather, sky, moon, timecycle and music, the player follow, the left world restored.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"

namespace
{
    // The world loads behind the black (SnapshotLoadingWorld): the caption, the
    // game's text, only when the editor does not draw its cue.
    void DrawLoadingCaption(const char* text)
    {
        if (EditorDrawsLoadingCue())
            return;
        GRAPHICS::SET_SCRIPT_GFX_DRAW_ORDER(7);
        replay::hud::DrawText(text, 0.86f, 0.92f, 0.35f, 255, 255, 255, 220);
        GRAPHICS::SET_SCRIPT_GFX_DRAW_ORDER(4);
    }
}
    PlayerLanding g_playerLanding;
namespace
{

    // The real player during a replay (both modes): taken out of the picture
    // reversibly, so only the puppets are left: invisible, no collision, frozen,
    // invincible, no control, ignored by everyone.
    void DisablePlayer(Ped player)
    {
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return;
        ENTITY::SET_ENTITY_VISIBLE(player, FALSE);
        ENTITY::SET_ENTITY_COLLISION(player, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(player, TRUE);
        PLAYER::SET_PLAYER_INVINCIBLE(PLAYER::PLAYER_ID(), TRUE);
        PLAYER::SET_EVERYONE_IGNORE_PLAYER(PLAYER::PLAYER_ID(), TRUE);
        PLAYER::SET_PLAYER_CONTROL(PLAYER::PLAYER_ID(), FALSE, 0, FALSE);
    }

    void RestorePlayer(bool visible, bool frozen, bool collisionOff, bool invincible)
    {
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (player && ENTITY::DOES_ENTITY_EXIST(player))
        {
            ENTITY::SET_ENTITY_VISIBLE(player, visible ? TRUE : FALSE);
            ENTITY::SET_ENTITY_COLLISION(player, collisionOff ? FALSE : TRUE, TRUE);
            ENTITY::FREEZE_ENTITY_POSITION(player, frozen ? TRUE : FALSE);
        }
        PLAYER::SET_PLAYER_INVINCIBLE(PLAYER::PLAYER_ID(), invincible ? TRUE : FALSE);
        PLAYER::SET_EVERYONE_IGNORE_PLAYER(PLAYER::PLAYER_ID(), FALSE);
        PLAYER::SET_PLAYER_CONTROL(PLAYER::PLAYER_ID(), TRUE, 0, FALSE);
    }
}
    ClipWorld g_clipWorld;

    // The session's moon angles being written back after a clip (or after a script
    // reload in one), and the check one second after the last write
    // (moon_track.h).
    replay::moontrack::GiveBack g_moonGiveBack;
namespace
{
    const char* g_moonGiveBackWhy = "end";
    std::uint64_t g_moonGiveBackFails = 0;
}
    MoonCheck g_moonCheck;
    RenderedCamera g_renderedCamera;

    bool ClipLoadWaiting() { return g_clipWorld.active && g_clipWorld.loadPhase != nullptr; }

    // The scrub and jump model (src/seek_settle.h). A scrub (keys or a timeline
    // drag) only moves pose, life and camera; the release and every jump settle
    // once: the time holds until the puppets, their models and the deferred state
    // rebuild are ready. A far jump also freezes the last picture with a spinner
    // (the editor add-on draws it).
    replay::settle::Config g_settleConfig;
    replay::settle::Settle g_settle;
    replay::settle::DragGate g_seekDrag;
    replay::settle::ScrubCost g_scrubCost;
    std::uint64_t g_settleLastPlayhead = ~0ull; // the playhead the last tick left
    const char* g_seekReason = nullptr;         // what moved the playhead this tick (a seek site)
    bool g_seekFine = false;                    // a frame step: never far by the camera
    bool g_settleCameraCheck = false;           // the jump's camera test, in UpdateReplayCamera
    replay::clipload::Vec3 g_settleCameraFrom{};
    bool g_settleCameraFromValid = false;
    bool g_settleJumpScene = false;             // the settle started the "jump" load scene
    std::uint64_t g_preloadCentreMs = 0;        // where the model window stands
    bool g_preloadCentreValid = false;
    ULONGLONG g_nextPreloadAt = 0;
    std::uint32_t g_seekLoadSequence = 0;
    // Per recording or playback (replay::log::NewSession), so a second playback of
    // the session logs again.
    replay::log::LogLimit g_jumpLoadLogs;
    replay::log::LogLimit g_scrubCostLogs;
    std::uint32_t g_creationHeldFrames = 0;
    bool g_creationHoldSpent = false; // the bound was reached: no hold until nothing is due
    replay::log::LogLimit g_creationHoldLogs;
    std::uint64_t g_creationHolds = 0;
    replay::settle::CostCounters g_tickStartCounters;
    std::uint64_t g_tickStartPlayhead = 0;
namespace
{

    // RedemptionEditor.ini RestoreWorldState and RestoreWorldStateInMission.
    replay::world::PlaybackConfig WorldConfig()
    {
        replay::world::PlaybackConfig config;
        config.restore = g_recConfig.restoreWorldState;
        config.inMission = g_recConfig.restoreWorldStateInMission;
        return config;
    }
}

    void StopClipLoadScene()
    {
        if (g_clipWorld.loadSceneActive)
            STREAMING::LOAD_SCENE_STOP();
        g_clipWorld.loadSceneActive = false;
    }
    CamPrefetch g_camPrefetch;
    std::uint64_t g_camPrefetchDoneCutMs = ~0ull; // the cut already served (no second start for it)
    std::uint32_t g_camPrefetchStarted = 0;
    std::uint32_t g_camPrefetchLoadedAtCut = 0;
    std::uint32_t g_camPrefetchNotLoadedAtCut = 0;
    std::uint32_t g_camPrefetchCancelled = 0;
    replay::log::LogLimit g_camPrefetchLogs;

    void StopCamPrefetch(const char* why)
    {
        if (g_camPrefetch.active)
        {
            STREAMING::LOAD_SCENE_STOP();
            if (!g_camPrefetch.cutSeen)
                ++g_camPrefetchCancelled;
            if (g_camPrefetchLogs.Take(200))
                LogFile() << "CAMERA_PREFETCH,phase=stop,why=" << why << ",cut_ms=" << g_camPrefetch.cutMs
                          << ",cut_seen=" << (g_camPrefetch.cutSeen ? 1 : 0)
                          << ",real_ms=" << (GetTickCount64() - g_camPrefetch.startedAt) << '\n';
        }
        g_camPrefetch = CamPrefetch{};
    }
namespace
{

    // The frustum load the game's own teleport helper uses (ambient_load:
    // position, camera direction, 500 m, flags 4).
    void StartClipLoadScene(const RenderedCamera& camera)
    {
        StopCamPrefetch("clip_load"); // the clip's load scene comes first
        StopClipLoadScene();
        const float pitch = camera.rotation.x * 3.14159265f / 180.0f;
        const float heading = camera.rotation.z * 3.14159265f / 180.0f;
        const float dirX = -std::sin(heading) * std::cos(pitch);
        const float dirY = std::cos(heading) * std::cos(pitch);
        const float dirZ = std::sin(pitch);
        // Refused (its handler, 1491.50 0x10AC82C, checks a game state first):
        // none of ours, tried again next tick.
        const bool started =
            STREAMING::LOAD_SCENE_START(camera.position.x, camera.position.y, camera.position.z, dirX, dirY, dirZ, 500.0f, 4) != FALSE;
        g_clipWorld.loadSceneActive = started;
        g_clipWorld.loadSceneAt = camera.position;
        g_clipWorld.loadSceneRefused += started ? 0 : 1;
    }
    replay::doors::MapSettle g_mapSettle;
    bool g_objectStandsWaitLogged = false; // the scenery wait's line, once per wait
    constexpr float kMapSettleDoorRadiusM = 60.0f;
    constexpr ULONGLONG kMapSettleAfterClearMs = 20000; // a clear this recent is this playback's
}

    void BeginClipLoadWait(const char* phase, ULONGLONG now)
    {
        g_clipWorld.loadPhase = phase;
        g_clipWorld.loadStartedAt = now;
        g_clipWorld.loadRetargets = 0;
        g_clipWorld.loadSceneLost = 0;
        g_clipWorld.loadSceneRefused = 0;
        g_clipWorld.loadIdle = replay::clipload::StreamingIdle{};
        g_mapSettle = replay::doors::MapSettle{};
        g_objectStandsWaitLogged = false;
        if (std::strcmp(phase, "start") == 0 && g_world.cleared && now - g_world.clearedAt < kMapSettleAfterClearMs)
            g_mapSettle.Begin(now, replay::doors::GetConfig().mapWaitMs);
    }

    // The streaming load a clip start puts on the engine in one go, logged when
    // the puppets start (at once, or at the stagger's release): the clip's
    // entities and models (all, and the opening census the first second builds),
    // the IPLs toggled since this start began (the previous clip's restore plus
    // this clip's apply), the engine's streaming requests and pools. Evidence for
    // a crash at the clip switch.
    void LogStreamPressure(const char* phase, const char* reason, ULONGLONG now)
    {
        std::uint32_t firstMs = ~0u;
        for (const replay::entity::CreatePacket& packet : g_clip.created)
            firstMs = (std::min)(firstMs, packet.timeMs);
        std::vector<std::uint32_t> models, openingModels;
        std::uint32_t opening = 0, bodies = 0, objects = 0;
        for (const replay::entity::CreatePacket& packet : g_clip.created)
        {
            models.push_back(packet.modelHash);
            const bool body = packet.kind == replay::entity::EntityKind::Ped || packet.kind == replay::entity::EntityKind::Horse ||
                packet.kind == replay::entity::EntityKind::Vehicle;
            bodies += body ? 1 : 0;
            objects += body ? 0 : 1;
            if (packet.timeMs <= firstMs + 1000u)
            {
                ++opening;
                openingModels.push_back(packet.modelHash);
            }
        }
        auto distinct = [](std::vector<std::uint32_t>& list) {
            std::sort(list.begin(), list.end());
            return static_cast<std::size_t>(std::unique(list.begin(), list.end()) - list.begin());
        };
        const replay::world::Pressure pressure = replay::world::CurrentPressure();
        const std::uint32_t peds = WorldPoolUsed(replay::pools::Kind::Ped);
        const std::uint32_t vehicles = WorldPoolUsed(replay::pools::Kind::Vehicle);
        const std::uint32_t poolObjects = WorldPoolUsed(replay::pools::Kind::Object);
        LogFile() << "STREAM_PRESSURE,phase=" << phase << ",reason=" << reason
                  << ",entities=" << g_clip.created.size()
                  << ",bodies=" << bodies << ",objects=" << objects << ",models=" << distinct(models)
                  << ",opening_entities=" << opening << ",opening_models=" << distinct(openingModels)
                  << ",ipl_toggles=" << (pressure.iplToggles - g_clipStartTogglesBefore)
                  << ",ipl_restored=" << pressure.lastRestoreIpls << ",ipl_on=" << pressure.lastApplyOn
                  << ",ipl_off=" << pressure.lastApplyOff << ",sets=" << pressure.lastApplySets
                  << ",ms_since_ipl_apply=" << (pressure.lastApplyAtMs && now >= pressure.lastApplyAtMs
                         ? static_cast<long long>(now - pressure.lastApplyAtMs) : -1LL)
                  << ",streaming_requests=" << STREAMING::GET_NUMBER_OF_STREAMING_REQUESTS()
                  << ",pool_peds=" << static_cast<int>(peds) << ",pool_vehicles=" << static_cast<int>(vehicles)
                  << ",pool_objects=" << static_cast<int>(poolObjects) << '\n';
    }
namespace
{

    // The map objects' wait ended (MapSettle): by its own rule ("map") or by
    // the load wait's timeout ("load_wait").
    void LogMapSettle(const char* endedBy)
    {
        LogFile() << "WORLD_MODEL,phase=map_objects,ms=" << g_mapSettle.EndMs() << ",objects=" << g_mapSettle.LastObjects()
                  << ",doors_missing=" << g_mapSettle.DoorsMissing() << ",timeout=" << (g_mapSettle.TimedOut() ? 1 : 0)
                  << ",cap_ms=" << replay::doors::GetConfig().mapWaitMs << ",over_cap_ms=" << g_mapSettle.OverCapMs()
                  << ",objects_at_cap=" << g_mapSettle.ObjectsAtCap() << ",ended_by=" << endedBy << '\n';
    }

    // The scenery stand-ins' wait ended (objcheck::SceneryReady), once per wait.
    void LogObjectStandsWait(std::uint32_t pending, std::uint64_t waitedMs, const char* endedBy)
    {
        g_objectStandsWaitLogged = true;
        const std::uint32_t capMs = replay::doors::GetConfig().mapWaitMs;
        LogFile() << "WORLD_MODEL,phase=object_stands,ms=" << waitedMs << ",pending=" << pending << ",timeout=" << (pending ? 1 : 0)
                  << ",phase_name=" << (g_clipWorld.loadPhase ? g_clipWorld.loadPhase : "none")
                  << ",over_cap_ms=" << (waitedMs > capMs ? waitedMs - capMs : 0) << ",ended_by=" << endedBy << '\n';
    }

    // Debug mode skips the start wait when the clip's first camera is this close
    // to where the game camera stood.
    constexpr float kDebugLoadNearMeters = 150.0f;

    // Once per tick while waiting, after UpdateReplayCamera. True when the
    // wait ended this tick (the caller fades in).
    bool UpdateClipLoadWait(ULONGLONG now)
    {
        ClipWorld& world = g_clipWorld;
        if (!world.loadPhase)
            return false;
        // The wait starts after the clip is read from disk (up to half a second),
        // later than the tick's `now`: unsigned, the elapsed time would wrap around and
        // the wait give up at once.
        now = std::max(now, world.loadStartedAt);
        // The clip's map state goes in behind the black (the fade out takes 250 ms),
        // before the load scene streams the area.
        if (world.worldApplyPending)
        {
            if (!CAM::IS_SCREEN_FADED_OUT() && now - world.loadStartedAt < 1000)
            {
                DrawLoadingCaption(TR("hud.loading_replay", "Loading replay..."));
                return false;
            }
            world.worldApplyPending = false;
            replay::world::BeginPlayback(g_clip, WorldConfig(), LogFile());
        }
        const RenderedCamera& camera = g_renderedCamera;
        if (camera.valid)
        {
            const bool engineActive = world.loadSceneActive && STREAMING::IS_LOAD_SCENE_ACTIVE() != FALSE;
            const replay::clipload::SceneStep step =
                replay::clipload::PlanScene(world.loadSceneActive, engineActive, world.loadSceneAt, camera.position);
            world.loadRetargets += step == replay::clipload::SceneStep::Retarget ? 1 : 0;
            world.loadSceneLost += step == replay::clipload::SceneStep::Lost ? 1 : 0;
            if (step != replay::clipload::SceneStep::Keep)
                StartClipLoadScene(camera);
            STREAMING::REQUEST_COLLISION_AT_COORD(camera.position.x, camera.position.y, camera.position.z);
        }
        const int streamingRequests = STREAMING::GET_NUMBER_OF_STREAMING_REQUESTS();
        world.loadIdle.Feed(now, streamingRequests);
        // No replay camera (a clip without a game camera, the freecam): the world
        // streams around whatever renders; only the puppets are awaited. Debug mode,
        // the clip's first camera near where the game camera stood: the area is
        // already loaded, the wait ends now.
        const bool nearOpened = !world.presentation && camera.valid && world.openedAtValid &&
            std::strcmp(world.loadPhase, "start") == 0 &&
            replay::clipload::DistanceSq(camera.position, world.openedAt) < kDebugLoadNearMeters * kDebugLoadNearMeters;
        const bool sceneLoaded = nearOpened ||
            (world.loadSceneActive ? STREAMING::IS_LOAD_SCENE_LOADED() != FALSE : !camera.valid);
        const bool puppetsReady = !replay::playback::Preparing();
        // The load scene or the puppets still hold the start: the map's and the
        // scenery's caps do not cut their waits short while the screen stays
        // black for them anyway (MapSettle, SceneryReady).
        const bool othersHolding = !sceneLoaded || !puppetsReady;
        bool worldReady = replay::world::Ready(); // interiors whose entity sets changed
        if (g_mapSettle.Active())
        {
            // The map objects back and the doors near the camera found before the
            // playhead runs.
            const std::uint32_t objects = WorldPoolUsed(replay::pools::Kind::Object);
            const std::uint32_t doors = camera.valid ? replay::doors::MissingNear(g_clip, camera.position.x,
                camera.position.y, camera.position.z, kMapSettleDoorRadiusM) : 0u;
            const replay::doors::MapSettle::Verdict map =
                g_mapSettle.Feed(now, objects == 0xFFFFFFFFu ? 0u : objects, doors, othersHolding);
            if (map.done)
                LogMapSettle("map");
            else
                worldReady = false;
        }
        // The stand-ins of the world objects and of the scenery due at the playhead
        // built before it runs (object_check.h SceneryReady), like the doors above,
        // bounded by the same MapObjectsWaitMs.
        if (worldReady && !g_objectStandsWaitLogged)
        {
            const std::uint32_t pending = replay::objects::PendingObjectStands(static_cast<std::uint32_t>(g_transport.PlayheadMs()));
            const std::uint64_t waitFrom = world.loadStartedAt;
            const std::uint64_t waited = now > waitFrom ? now - waitFrom : 0;
            if (replay::objcheck::SceneryReady(pending, waited, replay::doors::GetConfig().mapWaitMs, othersHolding))
                LogObjectStandsWait(pending, waited, "stands");
            else
                worldReady = false;
        }
        const std::uint64_t elapsed = now - world.loadStartedAt;
        replay::clipload::Config waitConfig;
        if (!world.presentation)
            waitConfig.minBlackMs = 0; // nothing black to hold in debug mode
        // The world facts at the camera, the staggered start fed with them, the
        // collision in the wait (ClipWorldWait) and a cap long enough for both.
        bool collisionLoaded = true;
        bool startPhase = std::strcmp(world.loadPhase, "start") == 0;
        bool waitWorldReady = worldReady;
        if (startPhase)
        {
            if (camera.valid)
                collisionLoaded = ENTITY::HAS_COLLISION_LOADED_AROUND_POSITION(camera.position.x, camera.position.y, camera.position.z) != FALSE;
            replay::clipstart::WorldFacts facts;
            facts.iplsApplied = !world.worldApplyPending;
            facts.sceneLoaded = sceneLoaded;
            facts.collisionLoaded = collisionLoaded;
            facts.interiorsReady = replay::world::Ready();
            facts.mapSettled = !g_mapSettle.Active();
            facts.streamingRequests = streamingRequests;
            if (g_clipStartConfig.worldWait)
                waitWorldReady = waitWorldReady && collisionLoaded;
            waitConfig.timeoutMs = replay::clipstart::StartWaitTimeoutMs(g_clipStartConfig, waitConfig.timeoutMs);
            const std::uint32_t bits = replay::clipstart::FactsBits(facts);
            if (replay::clipstart::LogThisTick(g_clipWorldLines, bits != g_clipWorldLastBits, now, g_clipWorldLastLogMs))
            {
                ++g_clipWorldLines;
                g_clipWorldLastLogMs = now;
                g_clipWorldLastBits = bits;
                const float farM = camera.valid && world.openedAtValid
                    ? std::sqrt(replay::clipload::DistanceSq(camera.position, world.openedAt)) : -1.0f;
                LogFile() << "CLIP_WORLD_READY,ms=" << elapsed << ",presentation=" << (world.presentation ? 1 : 0)
                          << ",ipls_applied=" << (facts.iplsApplied ? 1 : 0) << ",scene=" << (world.loadSceneActive ? 1 : 0)
                          << ",scene_loaded=" << (sceneLoaded ? 1 : 0) << ",near_opened=" << (nearOpened ? 1 : 0)
                          << ",collision=" << (collisionLoaded ? 1 : 0) << ",interiors=" << (facts.interiorsReady ? 1 : 0)
                          << ",map_settled=" << (facts.mapSettled ? 1 : 0) << ",world_ready=" << (worldReady ? 1 : 0)
                          << ",streaming=" << facts.streamingRequests << ",puppets_ready=" << (puppetsReady ? 1 : 0)
                          << ",camera_from_open_m=" << static_cast<int>(farM) << ",retargets=" << world.loadRetargets << '\n';
            }
        }
        // A far jump's wait is the settle's (the frozen picture, no black, a 6.6 s
        // stall limit; the editor draws the spinner).
        const bool jumpPhase = std::strcmp(world.loadPhase, "jump") == 0;
        if (jumpPhase)
        {
            waitConfig.minBlackMs = 0;
            waitConfig.timeoutMs = g_settleConfig.timeoutMs;
        }
        // Preparing read again (a start in this tick).
        const bool puppetsReadyNow = !replay::playback::Preparing();
        const replay::clipload::Verdict verdict = replay::clipload::Evaluate(elapsed, sceneLoaded && waitWorldReady,
            puppetsReady && puppetsReadyNow, waitConfig);
        if (!verdict.done && jumpPhase)
            return false;
        if (!verdict.done)
        {
            // "Loading replay..." over the black (draw order 7: after the
            // fade); the game's own loading icon on a faded screen stays.
            DrawLoadingCaption(TR("hud.loading_replay", "Loading replay..."));
            return false;
        }
        LogFile() << "CLIP_LOAD_WAIT,phase=" << world.loadPhase << ",presentation=" << (world.presentation ? 1 : 0)
                  << ",near_opened=" << (nearOpened ? 1 : 0) << ",ms=" << elapsed << ",loaded=" << (sceneLoaded ? 1 : 0)
                  << ",puppets_ready=" << (puppetsReady && puppetsReadyNow ? 1 : 0) << ",timeout=" << (verdict.timedOut ? 1 : 0)
                  << ",scene=" << (world.loadSceneActive ? 1 : 0) << ",retargets=" << world.loadRetargets
                  << ",jump_m=" << static_cast<int>(world.jumpMeters) << ",world_ready=" << (worldReady ? 1 : 0)
                  // The collision at the camera and the wait's own rules
                  << ",collision=" << (collisionLoaded ? 1 : 0) << ",world_wait=" << (g_clipStartConfig.worldWait ? 1 : 0)
                  << ",cap_ms=" << waitConfig.timeoutMs
                  // Why a scene never reported loaded: ours still active, stopped
                  // by someone else, refused; the streaming queue empty this long.
                  << ",scene_active=" << (world.loadSceneActive && STREAMING::IS_LOAD_SCENE_ACTIVE() ? 1 : 0)
                  << ",scene_lost=" << world.loadSceneLost << ",scene_refused=" << world.loadSceneRefused
                  << ",streaming=" << streamingRequests << ",streaming_idle_ms=" << world.loadIdle.IdleMs(now)
                  << ",interiors=" << (replay::world::Ready() ? 1 : 0) << '\n';
        // The map's and the scenery's waits still running at the load wait's own
        // timeout: their lines, ended by it.
        if (g_mapSettle.Active())
        {
            g_mapSettle.Stop(now);
            LogMapSettle("load_wait");
        }
        if (!g_objectStandsWaitLogged)
        {
            const std::uint64_t waitFrom = world.loadStartedAt;
            LogObjectStandsWait(replay::objects::PendingObjectStands(static_cast<std::uint32_t>(g_transport.PlayheadMs())),
                now > waitFrom ? now - waitFrom : 0, "load_wait");
        }
        if (startPhase)
            LogStreamPressure("wait_end", verdict.timedOut ? "timeout" : "ready", now);
        // The replay camera keeps the streaming focus (and clears it when it
        // stops); only the load scene is this wait's own.
        StopClipLoadScene();
        world.loadPhase = nullptr;
        world.jumpMeters = 0.0f;
        return true;
    }

    // The world to give back is also kept on disk while a clip's world is applied:
    // a script reload, a crash of the script or a save load ends the mod without
    // EndClipWorld, and natives cannot run at unload. The next load finds the file
    // and puts the world back.
    const char* kClipWorldStateFile = "RedemptionEditor.world-restore";
}
    // A state file of the base folder (storage_paths.h), for the next load.
    std::wstring StateFile(const char* name) { return replay::storage::Path(replay::identity::AsciiW(name)); }
namespace
{

    // The wind, rain and snow overrides back to what the world had
    // (weather_state.h): a negative value releases each one, as the scripts do
    // (SET_WIND_SPEED(-1.0f), SET_WIND_DIRECTION(-1.0f), SET_RAIN(-1.0f),
    // _SET_SNOW_LEVEL(-1.0f); the readers use an override only when it is >= 0.0
    // and the setters clear the wind flags below 0.0). A forced direction goes
    // back as the degrees that store the same vector.
    void RestoreWeatherOverrides(const replay::weatherstate::Overrides& o)
    {
        MISC::SET_RAIN(o.rain >= 0.0f ? o.rain : -1.0f);
        MISC::_SET_SNOW_LEVEL(o.snow >= 0.0f ? o.snow : -1.0f);
        MISC::SET_WIND_SPEED(o.windSpeedSet && o.windSpeed >= 0.0f ? o.windSpeed : -1.0f);
        MISC::SET_WIND_DIRECTION(o.windDirectionSet ? replay::envtrack::WindDegreesFromVector(o.windX, o.windY) : -1.0f);
    }

    std::string TransitionText(const replay::weatherstate::Transition& t)
    {
        std::ostringstream text;
        text << "index=" << t.index << ",total_ms=" << t.totalMs << ",left_ms=" << t.leftMs << ",progress=" << t.progress
             << ",flag=" << static_cast<int>(t.flag);
        return text.str();
    }

    // The transition the world had back, once the replay wrote its own.
    void GiveBackTransition(ClipWorld& world, const char* why)
    {
        if (!world.transitionWritten)
            return;
        world.transitionWritten = false;
        const bool ok = world.transitionSaved && replay::weatherstate::WriteTransition(world.savedTransition);
        LogFile() << "WEATHER_TRANSITION,phase=give_back,why=" << why << ",ok=" << (ok ? 1 : 0) << ','
                  << TransitionText(world.savedTransition) << '\n';
    }

    // Every tick with the clip's environment (ApplyClipEnvironment). Recorded
    // weather: the recorded transition into the manager (the target by its hash,
    // the progress of the playhead, the time left kept above 0 so the engine never
    // ends it on its own: environment_track.h TransitionFields) and the recorded
    // wind, rain and snow forced (missions force the wind themselves, and the
    // levels ramp with the transition). An editor weather: no transition over the
    // weather picked, the levels the world's. Live: the world's transition back,
    // nothing written. A clip whose transition is unknown (ENVT v1) leaves the
    // engine's alone.
    void ApplyClipTransition(ClipWorld& world, const replay::envoverride::Applied& at, const replay::EnvironmentSample* sample)
    {
        const bool recordedWeather = !at.weatherLive && !at.weatherOverridden;
        if (recordedWeather && sample && world.overridesSaved)
        {
            MISC::SET_WIND_SPEED((std::max)(sample->windSpeed, 0.0f));
            MISC::SET_WIND_DIRECTION(replay::envtrack::WindDegreesFromHeading(sample->windDirection));
            MISC::SET_RAIN((std::max)(sample->rain, 0.0f));
            MISC::_SET_SNOW_LEVEL((std::max)(sample->snow, 0.0f));
            world.levelsForced = true;
        }
        else if (world.levelsForced)
        {
            RestoreWeatherOverrides(world.savedOverrides);
            world.levelsForced = false;
        }
        if (!world.transitionSaved)
            return; // the manager did not resolve: unchanged
        if (at.weatherLive || (recordedWeather && (!sample || !sample->transitionKnown)))
        {
            GiveBackTransition(world, at.weatherLive ? "live" : "unknown");
            return;
        }
        replay::weatherstate::Transition write; // none: index -1, zeros
        if (recordedWeather && sample->transitionTo)
        {
            const std::int32_t index = replay::weatherstate::IndexOf(sample->transitionTo);
            if (index >= 0)
            {
                const replay::envtrack::TransitionWrite fields =
                    replay::envtrack::TransitionFields(sample->transitionTotalMs, sample->transitionLeftMs);
                write.index = index;
                write.totalMs = fields.totalMs;
                write.leftMs = fields.leftMs;
                write.progress = fields.progress;
                write.flag = sample->transitionFlag;
            }
            else if (world.transitionErrors++ < 5)
                LogFile() << "WEATHER_TRANSITION,phase=play,error=unknown_type,to=" << WeatherName(sample->transitionTo) << '\n';
        }
        if (replay::weatherstate::WriteTransition(write))
        {
            world.transitionWritten = true;
            world.lastTransition = write;
        }
        else if (world.transitionErrors++ < 5)
            LogFile() << "WEATHER_TRANSITION,phase=play,error=write," << TransitionText(write) << '\n';
    }

    void WriteClipWorldState(const ClipWorld& world)
    {
        if (FILE* file = _wfsopen(StateFile(kClipWorldStateFile).c_str(), L"w", _SH_DENYNO))
        {
            // The weather transition and the level overrides after the first 16 values;
            // then the session's six timecycle modifier fields (1 and the raw values, or 0
            // when the clip has no track). An older build reads its own fields and leaves
            // the rest.
            const replay::weatherstate::Transition& t = world.savedTransition;
            const replay::weatherstate::Overrides& o = world.savedOverrides;
            const replay::tctrack::ModifierFields& tc = world.tcBeforeFields;
            std::fprintf(file,
                "%d %d %d %u %u %.4f %d %d %d %d %d %d %.3f %.3f %.3f %.2f %d %d %.3f %.3f %.6f %d %d %.4f %.4f %d %.4f %d %.6f %.6f"
                " %d %d %.6f %d %.6f %.6f %d\n",
                world.hours, world.minutes, world.seconds,
                world.weatherFrom, world.weatherTo, world.weatherMix, world.playerVisible ? 1 : 0,
                world.playerFrozen ? 1 : 0, g_clip.environment.recorded ? 1 : 0, world.playerCollisionOff ? 1 : 0,
                world.playerInvincible ? 1 : 0, world.playerMoved ? 1 : 0, world.moverAt.x, world.moverAt.y,
                world.moverAt.z, world.moverHeading, world.transitionSaved ? 1 : 0, t.index, t.totalMs, t.leftMs, t.progress,
                static_cast<int>(t.flag), world.overridesSaved ? 1 : 0, o.rain, o.snow, o.windSpeedSet ? 1 : 0, o.windSpeed,
                o.windDirectionSet ? 1 : 0, o.windX, o.windY, world.tcBeforeValid ? 1 : 0, tc.index, tc.strength,
                tc.transitionIndex, tc.progress, tc.speed, static_cast<int>(tc.fadeOut));
            // Then the session's variations as keys (count, then type and variation pairs;
            // keys hold across sessions). An older build stops reading at its own last
            // field.
            const replay::TimecycleSample& variations = world.tcBefore;
            const int variationCount = world.tcBeforeValid ? static_cast<int>(variations.variationCount) : 0;
            std::fprintf(file, "%d", variationCount);
            for (int n = 0; n < variationCount; ++n)
                std::fprintf(file, " %u %u", variations.variations[n].type, variations.variations[n].variation);
            std::fprintf(file, "\n");
            // The session's moon angles, a line of its own after a word (an older build
            // never reads this far).
            std::fprintf(file, "moon %d %.17g %.17g\n", world.moonSaved ? 1 : 0, world.moonSavedAngle, world.moonSavedOrbit);
            std::fclose(file);
        }
    }
}

    // `afterRestart`: the game restarted its scripts (a save loaded, the mod's own
    // reload): the clock, the weather and the place in the file are the old
    // session's and the load brings the new one's, and the load scene, focus and
    // fade are the game's own during its loading screen. Only the player's flags
    // go back (visible, not frozen, collision, control: what the replay changed on
    // him).
    void RestoreLeftClipWorld(bool afterRestart = false)
    {
        FILE* file = _wfsopen(StateFile(kClipWorldStateFile).c_str(), L"r", _SH_DENYNO);
        if (!file)
            return;
        int hours = 0, minutes = 0, seconds = 0, visible = 1, frozen = 0, environment = 0, collisionOff = 0, invincible = 0;
        unsigned from = 0, to = 0;
        float mix = 0.0f;
        int atValid = 0;
        float atX = 0.0f, atY = 0.0f, atZ = 0.0f, heading = 0.0f;
        // The transition and the level overrides (14 more values; a file of an older
        // build has 16); then the six timecycle modifier fields (7 more values).
        int transitionSaved = 0, flag = 0, overridesSaved = 0, speedSet = 0, directionSet = 0;
        replay::weatherstate::Transition transition;
        replay::weatherstate::Overrides overrides;
        int tcValid = 0, tcFade = 0;
        replay::tctrack::ModifierFields tc;
        const int read = fscanf_s(file,
            "%d %d %d %u %u %f %d %d %d %d %d %d %f %f %f %f %d %d %f %f %f %d %d %f %f %d %f %d %f %f %d %d %f %d %f %f %d",
            &hours, &minutes, &seconds, &from, &to, &mix, &visible, &frozen, &environment, &collisionOff, &invincible, &atValid,
            &atX, &atY, &atZ, &heading, &transitionSaved, &transition.index, &transition.totalMs, &transition.leftMs,
            &transition.progress, &flag, &overridesSaved, &overrides.rain, &overrides.snow, &speedSet, &overrides.windSpeed,
            &directionSet, &overrides.windX, &overrides.windY, &tcValid, &tc.index, &tc.strength, &tc.transitionIndex,
            &tc.progress, &tc.speed, &tcFade);
        transition.flag = flag ? 1 : 0;
        overrides.windSpeedSet = speedSet != 0;
        overrides.windDirectionSet = directionSet != 0;
        // The session's variations (absent in an older file: no count read, nothing
        // given back).
        replay::TimecycleSample tcVariations;
        bool tcVariationsRead = false;
        if (read >= 37 && tcValid)
        {
            int count = -1;
            if (fscanf_s(file, "%d", &count) == 1 && count >= 0 && count <= static_cast<int>(replay::tctrack::kMaxVariations))
            {
                tcVariationsRead = true;
                for (int n = 0; n < count && tcVariationsRead; ++n)
                {
                    unsigned type = 0, variation = 0;
                    if (fscanf_s(file, "%u %u", &type, &variation) == 2)
                        replay::tctrack::AddVariation(tcVariations, type, variation);
                    else
                        tcVariationsRead = false;
                }
                tcVariationsRead = tcVariationsRead && replay::tctrack::Valid(tcVariations);
            }
        }
        // The session's moon angles, after the word "moon" (absent in an older file:
        // nothing given back).
        int moonValid = 0;
        double moonAngle = 0.0, moonOrbit = 0.0;
        {
            char token[16] = {};
            while (fscanf_s(file, "%15s", token, static_cast<unsigned>(sizeof(token))) == 1)
            {
                if (std::strcmp(token, "moon") != 0)
                    continue;
                if (fscanf_s(file, "%d %lf %lf", &moonValid, &moonAngle, &moonOrbit) != 3 || !std::isfinite(moonAngle) ||
                    !std::isfinite(moonOrbit) || std::fabs(moonAngle) > replay::moontrack::kLimit ||
                    std::fabs(moonOrbit) > replay::moontrack::kLimit)
                    moonValid = 0;
                break;
            }
        }
        std::fclose(file);
        _wremove(StateFile(kClipWorldStateFile).c_str());
        if (read < 9)
            return;
        if (afterRestart)
        {
            RestorePlayer(visible != 0, frozen != 0, collisionOff != 0, invincible != 0);
            LogFile() << "CLIP_WORLD,phase=dropped_after_restart,player_flags=1,environment=" << environment
                      << ",moved=" << (read >= 16 ? atValid : 0) << ",moon=" << moonValid << '\n';
            return;
        }
        if (environment)
        {
            CLOCK::SET_CLOCK_TIME(hours, minutes, seconds);
            CLOCK::PAUSE_CLOCK(FALSE, 0);
            MISC::SET_CURR_WEATHER_STATE(from, to, mix, TRUE);
            // The session's moon once this clock jump back is consumed (UpdateMoonGiveBack).
            if (moonValid && g_moonReplay)
            {
                g_moonGiveBack.Arm(moonAngle, moonOrbit, MISC::GET_FRAME_COUNT());
                g_moonGiveBackWhy = "reload";
                LogFile() << "MOON_STATE,phase=restored_after_reload,angle=" << moonAngle << ",orbit=" << moonOrbit << '\n';
            }
            // The transition and the overrides too.
            if (read >= 30 && WeatherStateReady())
            {
                const bool transitionOk = transitionSaved && replay::weatherstate::WriteTransition(transition);
                if (overridesSaved)
                    RestoreWeatherOverrides(overrides);
                LogFile() << "WEATHER_TRANSITION,phase=restored_after_reload,transition=" << (transitionOk ? 1 : 0)
                          << ",overrides=" << overridesSaved << '\n';
            }
        }
        // The session's timecycle modifier fields (same session after a script reload:
        // the raw indexes still hold) and its variations by key.
        if (read >= 37 && tcValid)
        {
            std::string tcError;
            tc.fadeOut = tcFade ? 1 : 0;
            const bool resolved = replay::tcstate::Resolve(tcError);
            const bool written = resolved && replay::tcstate::WriteFields(tc);
            replay::tcstate::VariationResult variations;
            if (resolved && tcVariationsRead)
                variations = replay::tcstate::ApplyVariations(tcVariations);
            LogFile() << "TIMECYCLE_TRACK,phase=restored_after_reload,written=" << (written ? 1 : 0) << ",mod_index=" << tc.index
                      << ",strength=" << tc.strength << ",variations_read=" << (tcVariationsRead ? 1 : 0)
                      << ",variation_count=" << static_cast<int>(tcVariations.variationCount) << ",var_set=" << variations.set
                      << ",var_cleared=" << variations.cleared << ",var_missing=" << variations.missing
                      << ",var_faults=" << variations.faults << '\n';
        }
        // The player back where he stood (the replay may have moved him with the
        // camera); frozen until the collision is there.
        if (read >= 16 && atValid)
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            const Ped mount = PED::IS_PED_ON_MOUNT(player) ? PED::GET_MOUNT(player) : 0;
            const Entity mover = mount ? mount : player;
            replay::pose::playback::MoveHiddenEntity(mover, atX, atY, atZ);
            ENTITY::SET_ENTITY_HEADING(mover, heading);
            if (mount)
            {
                ENTITY::_PAUSE_ENTITY_TRACKING(mount, FALSE);
                ENTITY::FREEZE_ENTITY_POSITION(mount, FALSE);
            }
            ENTITY::_PAUSE_ENTITY_TRACKING(player, FALSE);
            STREAMING::REQUEST_COLLISION_AT_COORD(atX, atY, atZ);
            g_playerLanding = PlayerLanding{true, frozen != 0, GetTickCount64() + replay::landing::kLandingTimeoutMs, GetTickCount64()};
        }
        RestorePlayer(visible != 0, frozen != 0 || (read >= 16 && atValid), collisionOff != 0, invincible != 0);
        // A clip's load scene and camera focus do not outlive it.
        if (STREAMING::IS_LOAD_SCENE_ACTIVE())
            STREAMING::LOAD_SCENE_STOP();
        STREAMING::CLEAR_FOCUS();
        if (!CAM::IS_SCREEN_FADED_IN())
            CAM::DO_SCREEN_FADE_IN(500);
        LogFile() << "CLIP_WORLD,phase=restored_after_reload\n";
    }
namespace
{

    void ApplyClipWeather()
    {
        const replay::Environment& environment = g_clip.environment;
        MISC::SET_CURR_WEATHER_STATE(environment.weatherFrom, environment.weatherTo, environment.weatherMix, TRUE);
    }
}

    // The recorded state at a playhead (the track, or the start state and
    // the clock in step with the timeline for a clip from before it).
    replay::envoverride::Recorded RecordedEnvironmentAt(std::uint64_t playheadMs)
    {
        const replay::Environment& environment = g_clip.environment;
        replay::envoverride::Recorded recorded;
        replay::EnvironmentSample at;
        replay::EnvironmentSample origin;
        const std::uint64_t originMs = EnvironmentOriginMs();
        if (replay::envtrack::At(environment.track, static_cast<std::uint32_t>(playheadMs), at) &&
            replay::envtrack::At(environment.track, static_cast<std::uint32_t>(originMs), origin))
        {
            recorded.clockS = at.clockS;
            recorded.startClockS = origin.clockS;
            recorded.weatherFrom = at.weatherFrom;
            recorded.weatherTo = at.weatherTo;
            recorded.weatherMix = at.weatherMix;
            return recorded;
        }
        recorded.clockS = replay::ClockAt(environment, playheadMs, g_transport.DurationMs());
        recorded.startClockS = replay::ClockAt(environment, originMs, g_transport.DurationMs());
        recorded.weatherFrom = environment.weatherFrom;
        recorded.weatherTo = environment.weatherTo;
        recorded.weatherMix = environment.weatherMix;
        return recorded;
    }
namespace
{

    // The clip's environment at the playhead, every tick: the clock and the
    // weather pair and mix of the track, interpolated. The weather is set again
    // when it moved or once a second (the weather system moves on by itself). With
    // the recorded weather, the transition over time and the wind, rain and snow
    // levels are written every tick too (ApplyClipTransition); EndClipWorld gives
    // the world's back. A clip without the track plays the start state. The
    // editor's settings (environment.edit) replace the recorded clock or weather
    // here (an overridden value is not replayed); the same natives, so
    // EndClipWorld's restore covers them.
    void ApplyClipEnvironment(ClipWorld& world, ULONGLONG now)
    {
        const replay::Environment& environment = g_clip.environment;
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        const replay::envoverride::Recorded recorded = RecordedEnvironmentAt(playheadMs);
        const replay::envoverride::Applied at = replay::envoverride::Resolve(recorded, environment.edit, playheadMs, EnvironmentOriginMs());
        // Live leaves the clock or the weather to the game, so another mod can change
        // them during the replay; EndClipWorld still gives back what the world had
        // before the clip.
        if (!at.clockLive)
            CLOCK::SET_CLOCK_TIME(static_cast<int>(at.clockS / 3600), static_cast<int>(at.clockS / 60 % 60), static_cast<int>(at.clockS % 60));
        world.clockLive = at.clockLive; // the moon follows (ApplyClipMoon)
        const bool moved = !world.envApplied || world.envFrom != at.weatherFrom || world.envTo != at.weatherTo ||
            std::fabs(world.envMix - at.weatherMix) >= 0.002f;
        // A clip without the track keeps its start weather, set every 2 s.
        const ULONGLONG every = environment.track.empty() && !at.weatherOverridden ? 2000 : 1000;
        if (at.weatherLive)
            world.envApplied = false; // back from Live: applied again at once
        else if (moved || now >= world.nextWeatherAt)
        {
            MISC::SET_CURR_WEATHER_STATE(at.weatherFrom, at.weatherTo, at.weatherMix, TRUE);
            world.envApplied = true;
            world.envFrom = at.weatherFrom;
            world.envTo = at.weatherTo;
            world.envMix = at.weatherMix;
            world.nextWeatherAt = now + every;
        }
        // The transition over time and the wind, rain and snow at the playhead. What
        // the engine made of the last write (one frame of its own update) is read for
        // the log first.
        replay::EnvironmentSample sample;
        const bool haveSample = replay::envtrack::At(environment.track, static_cast<std::uint32_t>(playheadMs), sample);
        const bool logNow = now >= world.envLogAt && world.envLogs < 120 && (!environment.track.empty() || environment.edit.Any());
        replay::weatherstate::Transition liveTransition;
        const bool liveTransitionRead = logNow && replay::weatherstate::ReadTransition(liveTransition);
        ApplyClipTransition(world, at, haveSample ? &sample : nullptr);
        if (logNow)
        {
            ++world.envLogs;
            world.envLogAt = now + 2000;
            Hash from = 0, to = 0;
            float mix = 0.0f;
            MISC::GET_CURR_WEATHER_STATE(&from, &to, &mix);
            const Vector3 liveDirection = MISC::GET_WIND_DIRECTION();
            LogFile() << "WEATHER_TRANSITION,phase=play,playhead_ms=" << playheadMs << ",known=" << (haveSample && sample.transitionKnown ? 1 : 0)
                      << ",to=" << (haveSample && sample.transitionTo ? WeatherName(sample.transitionTo) : std::string("none"))
                      << ",left_ms=" << (haveSample ? sample.transitionLeftMs : 0.0f) << ",total_ms=" << (haveSample ? sample.transitionTotalMs : 0.0f)
                      << ",progress=" << (haveSample && sample.transitionTo ? replay::envtrack::TransitionProgress(sample.transitionTotalMs, sample.transitionLeftMs) : 0.0f)
                      << ",written=" << (world.transitionWritten ? 1 : 0) << ",write_index=" << world.lastTransition.index
                      << ",write_total_ms=" << world.lastTransition.totalMs << ",write_left_ms=" << world.lastTransition.leftMs
                      << ",write_progress=" << world.lastTransition.progress
                      << ",live_read=" << (liveTransitionRead ? 1 : 0) << ",live_to="
                      << (liveTransitionRead && liveTransition.hash ? WeatherName(liveTransition.hash) : std::string("none"))
                      << ",live_progress=" << (liveTransitionRead ? liveTransition.progress : -1.0f)
                      << ",live_left_ms=" << (liveTransitionRead ? liveTransition.leftMs : -1.0f)
                      << ",forced=" << (world.levelsForced ? 1 : 0) << ",wind=" << (haveSample ? sample.windSpeed : 0.0f)
                      << ",wind_dir=" << (haveSample ? sample.windDirection : 0.0f)
                      << ",wind_dir_deg=" << (haveSample ? replay::envtrack::WindDegreesFromHeading(sample.windDirection) : -1.0f)
                      << ",live_wind_dir=" << std::atan2(liveDirection.y, liveDirection.x) << ",rain=" << (haveSample ? sample.rain : 0.0f)
                      << ",snow=" << (haveSample ? sample.snow : 0.0f) << '\n';
            LogFile() << "ENV_TRACK,phase=play,playhead_ms=" << playheadMs << ",from=" << WeatherName(at.weatherFrom)
                      << ",to=" << WeatherName(at.weatherTo) << ",mix=" << at.weatherMix << ",live_from=" << WeatherName(from)
                      << ",live_to=" << WeatherName(to) << ",live_mix=" << mix << ",clock_s=" << at.clockS
                      << ",recorded_clock_s=" << recorded.clockS << ",edit_clock=" << (at.clockOverridden ? 1 : 0)
                      << ",edit_weather=" << (at.weatherOverridden ? 1 : 0) << ",clouds_live=" << (at.pinClouds ? 0 : 1)
                      << ",clock_live=" << (at.clockLive ? 1 : 0) << ",weather_live=" << (at.weatherLive ? 1 : 0)
                      << ",live_rain=" << MISC::GET_RAIN_LEVEL() << ",live_snow=" << MISC::GET_SNOW_LEVEL()
                      << ",live_wind=" << MISC::GET_WIND_SPEED() << ",samples=" << environment.track.size();
            // The clock the game has now (clock_s is the one asked) and the moon's angles
            // (MOON_STATE has the rest).
            LogFile() << ",live_clock_s=" << LiveClockS();
            replay::MoonSample moon;
            if (ReadMoonNow(moon))
                LogFile() << ",moon_angle=" << moon.angle << ",moon_orbit=" << moon.orbit << ",live_date=" << MoonDateText(moon);
            LogFile() << '\n';
        }
    }

    // The moon at the playhead, every tick after the clock (ApplyClipEnvironment;
    // moon_track.h). The clip's two angles go into the sky object (guarded writes from the script fiber, no detour); the
    // game's own update then draws the moon, the shadows and the sky from them and
    // the clip's clock, as when recording (the clock is paused: its step is 0 and
    // the write holds; a clock jump costs at most a frame). A "Game (live)"
    // clock leaves the angles to the game; a clip without the section keeps the session's moon. Every 2 s (the
    // first 120) and on a change of source a MOON_STATE line compares what the
    // engine has before this tick's write (the last write and one frame of the
    // engine's own step) with the recording.
    void ApplyClipMoon(ClipWorld& world, ULONGLONG now)
    {
        const replay::Environment& environment = g_clip.environment;
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        const replay::moontrack::Source source =
            replay::moontrack::PlanPlayback(g_moonReplay, !environment.moon.empty(), world.clockLive);
        const char* sourceName = replay::moontrack::SourceName(source);
        replay::MoonSample at;
        const bool haveAt = replay::moontrack::At(environment.moon, static_cast<std::uint32_t>(playheadMs), at);
        const bool logNow = now >= world.moonLogAt && world.moonLogs < 120;
        const bool sourceMoved = world.moonSource != sourceName;
        if (logNow || sourceMoved)
        {
            if (logNow)
            {
                ++world.moonLogs;
                world.moonLogAt = now + 2000;
            }
            world.moonSource = sourceName;
            replay::MoonSample live;
            const bool liveOk = ReadMoonNow(live);
            std::ofstream& log = LogFile();
            log << "MOON_STATE,phase=play,why=" << (sourceMoved ? "source" : "tick") << ",playhead_ms=" << playheadMs
                << ",source=" << sourceName << ",live_clock_s=" << LiveClockS() << ",live_ok=" << (liveOk ? 1 : 0);
            if (liveOk)
                log << ",live_angle=" << live.angle << ",live_orbit=" << live.orbit << ",live_date=" << MoonDateText(live)
                    << ",live_sun_el=" << replay::moontrack::ElevationDeg(live.sun)
                    << ",live_moon_el=" << replay::moontrack::ElevationDeg(live.moon);
            if (haveAt)
            {
                log << ",recorded_angle=" << at.angle << ",recorded_orbit=" << at.orbit << ",recorded_date=" << MoonDateText(at)
                    << ",recorded_sun_el=" << replay::moontrack::ElevationDeg(at.sun)
                    << ",recorded_moon_el=" << replay::moontrack::ElevationDeg(at.moon);
                if (liveOk)
                    log << ",gap_angle=" << replay::moontrack::WrapDelta(at.angle, live.angle)
                        << ",gap_orbit=" << replay::moontrack::WrapDelta(at.orbit, live.orbit)
                        << ",moon_gap_deg=" << replay::moontrack::DirectionGapDeg(at.moon, live.moon)
                        << ",sun_gap_deg=" << replay::moontrack::DirectionGapDeg(at.sun, live.sun);
            }
            log << ",writes=" << world.moonWrites
                << ",write_fails=" << world.moonWriteFails << ",samples=" << environment.moon.size() << '\n';
        }
        if (source != replay::moontrack::Source::Recorded || !haveAt)
            return;
        if (replay::sunctl::WriteMoon(at.angle, at.orbit))
            ++world.moonWrites;
        else if (++world.moonWriteFails <= 5)
            LogFile() << "MOON_STATE,phase=play,error=write,playhead_ms=" << playheadMs << ",resolve="
                      << replay::sunctl::ResolveError() << '\n';
    }

    // The session's moon angles when the clip takes the clock (before anything is
    // written; a give-back still running from the last clip holds the session's
    // values, not the live ones).
    void SnapshotClipMoon(ClipWorld& world)
    {
        world.moonSaved = false;
        if (!g_moonReplay || !g_clip.environment.recorded)
            return;
        const char* from = "live";
        replay::MoonSample live;
        const bool liveOk = ReadMoonNow(live);
        if (g_moonGiveBack.armed)
        {
            world.moonSavedAngle = g_moonGiveBack.angle;
            world.moonSavedOrbit = g_moonGiveBack.orbit;
            world.moonSaved = true;
            g_moonGiveBack = replay::moontrack::GiveBack{};
            from = "pending_give_back";
        }
        else if (liveOk)
        {
            world.moonSavedAngle = live.angle;
            world.moonSavedOrbit = live.orbit;
            world.moonSaved = true;
        }
        g_moonCheck = MoonCheck{};
        std::ofstream& log = LogFile();
        log << "MOON_STATE,phase=begin,saved=" << (world.moonSaved ? 1 : 0) << ",from=" << from;
        if (!world.moonSaved)
            log << ",error=" << (liveOk ? "none" : replay::sunctl::ResolveError());
        log << ",angle=" << world.moonSavedAngle << ",orbit=" << world.moonSavedOrbit << ",clock_s=" << LiveClockS();
        if (liveOk)
            log << ",date=" << MoonDateText(live) << ",sun_el=" << replay::moontrack::ElevationDeg(live.sun)
                << ",moon_el=" << replay::moontrack::ElevationDeg(live.moon);
        log << ",samples=" << g_clip.environment.moon.size() << '\n';
    }

    // The session's moon angles written back every tick until the clock's jump
    // back has been consumed (a few game frames), then one check a second later:
    // the live angles against the given ones and the game's own steps since
    // (expected_orbit always, the turn only in the dark). See moon_track.h GiveBack.
    void UpdateMoonGiveBack(ULONGLONG now)
    {
        if (g_moonGiveBack.armed)
        {
            const bool first = g_moonGiveBack.ticks == 0;
            replay::MoonSample live;
            const bool liveOk = ReadMoonNow(live);
            const bool ok = replay::sunctl::WriteMoon(g_moonGiveBack.angle, g_moonGiveBack.orbit);
            if (!ok)
                ++g_moonGiveBackFails;
            const double angle = g_moonGiveBack.angle;
            const double orbit = g_moonGiveBack.orbit;
            const bool last = g_moonGiveBack.Step(MISC::GET_FRAME_COUNT());
            if (first || last)
            {
                std::ofstream& log = LogFile();
                log << "MOON_STATE,phase=give_back,why=" << g_moonGiveBackWhy << ",step=" << (first ? "first" : "last")
                    << ",ticks=" << g_moonGiveBack.ticks << ",angle=" << angle << ",orbit=" << orbit << ",live_ok=" << (liveOk ? 1 : 0);
                if (liveOk)
                    log << ",live_angle=" << live.angle << ",live_orbit=" << live.orbit
                        << ",moved_angle=" << replay::moontrack::WrapDelta(angle, live.angle)
                        << ",moved_orbit=" << replay::moontrack::WrapDelta(orbit, live.orbit)
                        << ",sun_el=" << replay::moontrack::ElevationDeg(live.sun) << ",moon_el=" << replay::moontrack::ElevationDeg(live.moon);
                log << ",clock_s=" << LiveClockS() << ",ok=" << (ok ? 1 : 0) << ",fails=" << g_moonGiveBackFails << '\n';
            }
            if (last)
                g_moonCheck = MoonCheck{true, now + 1000, angle, orbit, LiveClockS(), g_moonGiveBackWhy};
        }
        if (g_moonCheck.pending && now >= g_moonCheck.at)
        {
            g_moonCheck.pending = false;
            replay::MoonSample live;
            const bool liveOk = ReadMoonNow(live);
            const std::uint32_t clockS = LiveClockS();
            const std::uint32_t gameS = (clockS + 86400u - g_moonCheck.clockS) % 86400u;
            std::ofstream& log = LogFile();
            log << "MOON_STATE,phase=after,why=" << g_moonCheck.why << ",angle=" << g_moonCheck.angle << ",orbit=" << g_moonCheck.orbit
                << ",live_ok=" << (liveOk ? 1 : 0);
            if (liveOk)
                log << ",live_angle=" << live.angle << ",live_orbit=" << live.orbit
                    << ",diff_angle=" << replay::moontrack::WrapDelta(g_moonCheck.angle, live.angle)
                    << ",diff_orbit=" << replay::moontrack::WrapDelta(g_moonCheck.orbit, live.orbit)
                    << ",sun_el=" << replay::moontrack::ElevationDeg(live.sun) << ",moon_el=" << replay::moontrack::ElevationDeg(live.moon);
            log << ",game_s=" << gameS << ",expected_orbit=" << gameS * replay::moontrack::kOrbitPerSecond
                << ",expected_angle_if_dark=" << gameS * replay::moontrack::kTurnPerDarkSecond << '\n';
        }
    }
}

    // Unload (no natives): a give-back still running, or a clip still holding the
    // clock, writes the session's moon now (the world-restore file arms it again
    // after the next load's clock jump back).
    void GiveBackMoonAtUnload()
    {
        if (g_moonGiveBack.armed)
            replay::sunctl::WriteMoon(g_moonGiveBack.angle, g_moonGiveBack.orbit);
        else if (g_clipWorld.active && g_clipWorld.moonSaved)
            replay::sunctl::WriteMoon(g_clipWorld.moonSavedAngle, g_clipWorld.moonSavedOrbit);
        g_moonGiveBack = replay::moontrack::GiveBack{};
    }
namespace
{

    // The clouds at the playhead, every tick, on the clip's time: the accumulators
    // the sky update integrates (sky_track.h) set to the recorded state through the
    // game's own setters (rung 1; the handlers only store: 0xF8E578, 0xF8E530,
    // 0xF8E5B0, 0xF8E4FC, 0xF8F034). Nothing accumulates: a seek, a pause or a
    // second playback or export draws the same clouds at the same playhead. The
    // engine then moves them one frame of its own wind before it draws, as it did
    // while recording. Before the write, the log compares (every 2 s) what the
    // engine made of the last one (moved=, one frame of wind) and the offset with
    // no setter against the recording (unpinned_gap=).
    void ApplyClipSky(ClipWorld& world, ULONGLONG now)
    {
        const replay::Environment& environment = g_clip.environment;
        replay::SkySample at;
        if (!replay::skytrack::At(environment.sky, environment.skyPeriods, static_cast<std::uint32_t>(g_transport.PlayheadMs()), at))
            return;
        const bool logNow = now >= world.skyLogAt && world.skyLogs < 120;
        float moved = -1.0f;
        float unpinnedGap = -1.0f;
        if (logNow && world.skyApplied)
        {
            replay::SkySample live;
            if (replay::skystate::Read(live))
            {
                moved = replay::skytrack::Distance(world.skyLast, live, environment.skyPeriods);
                unpinnedGap = 0.0f;
                for (int c = 0; c < 3; ++c)
                    unpinnedGap = (std::max)(unpinnedGap, std::fabs(live.unpinned[c] - at.unpinned[c]));
            }
        }
        using namespace replay::sky;
        GRAPHICS::_SET_CLOUD_NOISE(at.value[kNoiseX], at.value[kNoiseY], at.value[kNoiseZ]);
        GRAPHICS::_SET_CLOUD_POSITION(at.value[kPositionX], at.value[kPositionY], at.value[kPositionZ]);
        GRAPHICS::_SET_CLOUD_LAYER(at.value[kLayer0X], at.value[kLayer0Y], 0);
        GRAPHICS::_SET_CLOUD_LAYER(at.value[kLayer1X], at.value[kLayer1Y], 1);
        // natives.h types these two as Any (an int): the handlers read floats.
        invoke<Void>(0x085C5B61A0114F32, at.value[kDriftX], at.value[kDriftY]);
        invoke<Void>(0x1FF8731BE1DFC0C0, at.value[kRegionX], at.value[kRegionY]);
        world.skyApplied = true;
        world.skyLast = at;
        ++world.skyWrites;
        if (logNow)
        {
            ++world.skyLogs;
            world.skyLogAt = now + 2000;
            LogFile() << "SKY_TRACK,phase=play,playhead_ms=" << g_transport.PlayheadMs() << ',' << SkyValuesText(at)
                      << ",moved=" << moved << ",unpinned_gap=" << unpinnedGap << ",writes=" << world.skyWrites
                      << ",samples=" << environment.sky.size() << '\n';
        }
    }

    // The script timecycle modifier and the weather variations at the playhead,
    // every tick (timecycle_track.h). The six modifier fields are written with the
    // session's index of the recorded key (looked up every tick, so an index that
    // names another key is never written) and speed 0: the playhead drives the
    // transition and fade progress (pause, seek and export draw the same look).
    // Each weather type gets the recorded variation (its default when the clip has
    // none) through the engine's own setter, only when it differs. Weather "Game
    // (live)" in the editor leaves the variations to the game. EndClipWorld gives
    // back what the session had.
    void ApplyClipTimecycle(ClipWorld& world, ULONGLONG now)
    {
        const replay::Environment& environment = g_clip.environment;
        replay::TimecycleSample at;
        if (!replay::tctrack::At(environment.timecycle, static_cast<std::uint32_t>(g_transport.PlayheadMs()), at))
            return;
        std::string error;
        if (!world.tcBeforeValid || !replay::tcstate::Resolve(error))
            return;
        const std::int32_t modifierIndex = replay::tcstate::IndexOf(at.modifierKey);
        const std::int32_t transitionIndex = replay::tcstate::IndexOf(at.transitionKey);
        bool unresolved = false;
        const replay::tctrack::ModifierFields fields =
            replay::tctrack::PlanModifier(at, modifierIndex, transitionIndex, &unresolved);
        world.tcTouched = true;
        if (replay::tcstate::WriteFields(fields))
            ++world.tcWrites;
        else
            ++world.tcWriteFails;
        if (unresolved)
            ++world.tcUnresolved;
        const bool variationsLive = environment.edit.WeatherLive();
        replay::tcstate::VariationResult variations;
        if (!variationsLive)
        {
            variations = replay::tcstate::ApplyVariations(at);
            world.tcVariationsTouched = true;
            world.tcVariationCalls += variations.cleared + variations.set;
            world.tcVariationMissing += variations.missing;
        }
        const bool logNow = now >= world.tcLogAt && world.tcLogs < 120;
        if (logNow || ((variations.cleared || variations.set || variations.faults) && world.tcLogs < 120))
        {
            ++world.tcLogs;
            world.tcLogAt = now + 2000;
            replay::TimecycleSample live;
            const bool liveOk = replay::tcstate::Read(live);
            LogFile() << "TIMECYCLE_TRACK,phase=play,playhead_ms=" << g_transport.PlayheadMs() << ',' << TimecycleText(at)
                      << ",write_index=" << fields.index << ",write_trans_index=" << fields.transitionIndex
                      << ",write_progress=" << fields.progress << ",unresolved=" << (unresolved ? 1 : 0)
                      << ",live=" << (liveOk ? TimecycleText(live) : std::string("read_failed"))
                      << ",variations_live=" << (variationsLive ? 1 : 0) << ",var_set=" << variations.set
                      << ",var_cleared=" << variations.cleared << ",var_missing=" << variations.missing
                      << ",var_faults=" << variations.faults << ",writes=" << world.tcWrites
                      << ",write_fails=" << world.tcWriteFails << ",samples=" << environment.timecycle.size() << '\n';
        }
    }

    // The session's timecycle modifier and variations, read once when the clip's
    // world is taken (before anything is written).
    void SnapshotClipTimecycle(ClipWorld& world)
    {
        world.tcBeforeValid = false;
        if (g_clip.environment.timecycle.empty())
            return;
        std::string error;
        if (!replay::tcstate::Resolve(error))
        {
            LogFile() << "TIMECYCLE_TRACK,phase=begin,ok=0,error=" << error << '\n';
            return;
        }
        if (!replay::tcstate::ReadFields(world.tcBeforeFields) || !replay::tcstate::Read(world.tcBefore))
        {
            LogFile() << "TIMECYCLE_TRACK,phase=begin,ok=0,error=read\n";
            return;
        }
        world.tcBeforeValid = true;
        LogFile() << "TIMECYCLE_TRACK,phase=begin,ok=1,session=" << TimecycleText(world.tcBefore)
                  << ",modifiers=" << replay::tcstate::ModifierCount() << ",samples=" << g_clip.environment.timecycle.size()
                  << '\n';
    }

    // Back to the session's modifier fields (the raw values read at the start: same
    // session, same indexes) and variations.
    void RestoreClipTimecycle(ClipWorld& world)
    {
        if (!world.tcBeforeValid || !world.tcTouched)
            return;
        const bool written = replay::tcstate::WriteFields(world.tcBeforeFields);
        // Variations left to the game the whole replay ("Game (live)") stay as the game
        // made them.
        replay::tcstate::VariationResult variations;
        if (world.tcVariationsTouched)
            variations = replay::tcstate::ApplyVariations(world.tcBefore);
        replay::TimecycleSample live;
        const bool liveOk = replay::tcstate::Read(live);
        LogFile() << "TIMECYCLE_TRACK,phase=restore,written=" << (written ? 1 : 0)
                  << ",var_restore=" << (world.tcVariationsTouched ? 1 : 0) << ",var_set=" << variations.set
                  << ",var_cleared=" << variations.cleared << ",var_missing=" << variations.missing
                  << ",var_faults=" << variations.faults << ",session=" << TimecycleText(world.tcBefore)
                  << ",live=" << (liveOk ? TimecycleText(live) : std::string("read_failed")) << ",writes=" << world.tcWrites
                  << ",write_fails=" << world.tcWriteFails << ",unresolved_ticks=" << world.tcUnresolved
                  << ",variation_calls=" << world.tcVariationCalls << ",variation_missing=" << world.tcVariationMissing << '\n';
        world.tcTouched = false;
        world.tcVariationsTouched = false;
    }
}

    // The clip's clock and weather apply in both modes (they are the clip's own
    // data); the black screen and the hidden player are the product's presentation
    // only.
    // ---- the game's score off while the replay owns the world ----
    // (src/music_stop.h). Rung 1 only: public natives on the script thread.
    replay::music::Guard g_music;

    void BeginClipWorld()
    {
        ClipWorld& world = g_clipWorld;
        world = ClipWorld{};
        world.active = true;
        world.presentation = !g_debugMode;
        world.fadeInPending = world.presentation;
        if (world.presentation)
            CAM::DO_SCREEN_FADE_OUT(250);
        // Both modes wait for the world at the clip's camera (a load scene, the
        // transport held); only product mode goes black.
        {
            const Vector3 opened = CAM::GET_FINAL_RENDERED_CAM_COORD();
            world.openedAt = {opened.x, opened.y, opened.z};
            world.openedAtValid = true;
        }
        g_renderedCamera = RenderedCamera{};
        BeginClipLoadWait("start", GetTickCount64());
        world.hours = CLOCK::GET_CLOCK_HOURS();
        world.minutes = CLOCK::GET_CLOCK_MINUTES();
        world.seconds = CLOCK::GET_CLOCK_SECONDS();
        MISC::GET_CURR_WEATHER_STATE(&world.weatherFrom, &world.weatherTo, &world.weatherMix);
        // The weather transition and the level overrides the world has, given back at
        // the end (EndClipWorld).
        if (g_clip.environment.recorded && WeatherStateReady())
        {
            world.transitionSaved = replay::weatherstate::ReadTransition(world.savedTransition);
            world.overridesSaved = replay::weatherstate::ReadOverrides(world.savedOverrides);
            const replay::weatherstate::Overrides& o = world.savedOverrides;
            LogFile() << "WEATHER_TRANSITION,phase=begin,saved=" << (world.transitionSaved ? 1 : 0) << ','
                      << TransitionText(world.savedTransition) << ",to="
                      << (world.savedTransition.hash ? WeatherName(world.savedTransition.hash) : std::string("none"))
                      << ",overrides=" << (world.overridesSaved ? 1 : 0) << ",rain=" << o.rain << ",snow=" << o.snow
                      << ",wind_speed_set=" << (o.windSpeedSet ? 1 : 0) << ",wind_speed=" << o.windSpeed
                      << ",wind_dir_set=" << (o.windDirectionSet ? 1 : 0) << '\n';
        }
        world.player = PLAYER::PLAYER_PED_ID();
        if (world.player && ENTITY::DOES_ENTITY_EXIST(world.player))
        {
            world.playerVisible = ENTITY::IS_ENTITY_VISIBLE(world.player) != FALSE;
            world.playerFrozen = ENTITY::_IS_ENTITY_FROZEN(world.player) != FALSE;
            world.playerCollisionOff = ENTITY::GET_ENTITY_COLLISION_DISABLED(world.player) != FALSE;
        }
        world.playerInvincible = PLAYER::GET_PLAYER_INVINCIBLE(PLAYER::PLAYER_ID()) != FALSE;
        if (world.player && ENTITY::DOES_ENTITY_EXIST(world.player))
        {
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(world.player, TRUE, FALSE);
            world.playerAt = {at.x, at.y, at.z};
            world.playerHeading = ENTITY::GET_ENTITY_HEADING(world.player);
            world.playerAtValid = true;
        }
        SnapshotClipTimecycle(world); // before anything is written, kept in the file below
        SnapshotClipMoon(world); // before the clock moves, kept in the file below
        WriteClipWorldState(world); // before touching the player: a reload finds what to give back
        DisablePlayer(world.player);
        if (g_clip.environment.recorded)
        {
            CLOCK::PAUSE_CLOCK(TRUE, 0);
            if (!g_clip.environment.edit.WeatherLive()) // the game's weather stays
                ApplyClipWeather();
        }
        // The clip's map state (IPLs, entity sets). Product mode waits for the black
        // screen (UpdateClipLoadWait); debug mode now.
        if (world.presentation)
            world.worldApplyPending = true;
        else
            replay::world::BeginPlayback(g_clip, WorldConfig(), LogFile());
        LogFile() << "CLIP_WORLD,phase=begin,presentation=" << (world.presentation ? 1 : 0)
                  << ",environment=" << (g_clip.environment.recorded ? 1 : 0)
                  << ",clock_start_s=" << g_clip.environment.clockStartS
                  << ",env_samples=" << g_clip.environment.track.size()
                  << ",sky_samples=" << g_clip.environment.sky.size()
                  << ",timecycle_samples=" << g_clip.environment.timecycle.size() << '\n';
        MusicEnter();
    }
namespace
{

    // The hidden player goes along with the replay. The engine's snow and mud
    // stamping works only around the player ped: a clip played far from where the
    // player stands leaves no marks even with the trail map centred on the puppets
    // (TRAIL_MAP override_is_target=1). Every tick the player (his horse when
    // mounted: the engine and the rider follow it) stands at the trail focus (the
    // puppet the map is centred on) or, without one, under the replay camera. His
    // own trail is paused (no prints of his there); he stays invisible, frozen,
    // collision-off, invincible and ignored, and goes back where he stood when the
    // clip ends (the world-restore file keeps the place for a reload). Not in a
    // vehicle, and not in a mission unless the clip's world is restored in
    // missions (a mission script may fail on distance).
    void UpdatePlayerFollow(ClipWorld& world, ULONGLONG now)
    {
        // Every return before the move tells the speech hold he is not followed.
        if (!world.playerAtValid)
        {
            replay::events::NotePlayerFollow(false);
            return;
        }
        const Ped player = world.player;
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
        {
            replay::events::NotePlayerFollow(false);
            return;
        }
        const char* skip = nullptr;
        if (PED::IS_PED_IN_ANY_VEHICLE(player, FALSE))
            skip = "in_vehicle";
        else if (MISC::GET_MISSION_FLAG() && !g_recConfig.restoreWorldStateInMission)
            skip = "mission";
        if (skip)
        {
            replay::events::NotePlayerFollow(false);
            if (world.followLogs < 4 && !world.playerMoved)
            {
                ++world.followLogs;
                LogFile() << "TRAIL_PLAYER_FOLLOW,phase=skipped,why=" << skip << '\n';
            }
            return;
        }
        float x = 0.0f, y = 0.0f, z = 0.0f;
        const bool focus = replay::playback::TrailFocus(x, y, z);
        if (!focus)
        {
            if (!g_renderedCamera.valid)
            {
                replay::events::NotePlayerFollow(false);
                return;
            }
            x = g_renderedCamera.position.x;
            y = g_renderedCamera.position.y;
            z = g_renderedCamera.position.z - 1.5f;
        }
        // A line of the recorded player held on him at his puppet (his pain
        // voices die on the puppet): he stays there while it plays.
        const bool held = replay::events::PlayerSpeechHoldAt(x, y, z);
        if (!world.playerMoved)
        {
            // The entity that moves: the horse under him (the rider goes
            // with it), else the player.
            world.mover = PED::IS_PED_ON_MOUNT(player) ? PED::GET_MOUNT(player) : player;
            if (!world.mover || !ENTITY::DOES_ENTITY_EXIST(world.mover))
                world.mover = player;
            const Vector3 moverAt = ENTITY::GET_ENTITY_COORDS(world.mover, TRUE, FALSE);
            world.moverAt = {moverAt.x, moverAt.y, moverAt.z};
            world.moverHeading = ENTITY::GET_ENTITY_HEADING(world.mover);
            ENTITY::_PAUSE_ENTITY_TRACKING(player, TRUE);
            if (world.mover != player)
                ENTITY::_PAUSE_ENTITY_TRACKING(world.mover, TRUE);
            world.playerMoved = true;
            WriteClipWorldState(world);
        }
        replay::pose::playback::MoveHiddenEntity(world.mover, x, y, z);
        replay::events::NotePlayerFollow(true);
        if (world.followLogs < 12 && now >= world.nextFollowAt)
        {
            ++world.followLogs;
            world.nextFollowAt = now + 5000;
            LogFile() << "TRAIL_PLAYER_FOLLOW,phase=moved,focus=" << (held ? "speech_hold" : focus ? "puppet" : "camera")
                      << ",mounted=" << (world.mover != player ? 1 : 0) << ",at=" << x << ';' << y << ';' << z
                      << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        }
    }

    // Where the weather audio's "is the world loaded about the player" gate looks
    // while the replay owns the world. The live player may stay hidden far away (a
    // mission skips the follow above), and the engine would freeze the wind on its
    // simple path. The detour reads the scene only for that one call
    // (wind_replay.h).
    // The scene is the streaming focus the replay set this tick (UpdateWorldFocus,
    // SET_FOCUS_POS_AND_VEL at the rendered or free camera), else the rendered
    // camera, and only then the trail focus puppet. Live, the player is the
    // engine's focus entity, so the gate asks about the focus; the answer depends
    // only on the map data loaded there (the position goes nowhere else in the
    // pre-update). The trail focus may be a puppet up to 250 m from the camera
    // (kNearestMeters) while the gate turns true only within ~165 m of the focus:
    // asked about such a puppet the gate would still say no and freeze the wind.
    void PublishWindScene()
    {
        if (!replay::windreplay::Installed())
            return;
        if (g_worldFocusTracker.owned && g_transport.Playing())
        {
            replay::windreplay::PublishScene(g_worldFocusAt.x, g_worldFocusAt.y, g_worldFocusAt.z,
                replay::windreplay::SceneSource::Focus, LogFile());
            return;
        }
        if (g_renderedCamera.valid)
        {
            replay::windreplay::PublishScene(g_renderedCamera.position.x, g_renderedCamera.position.y,
                g_renderedCamera.position.z, replay::windreplay::SceneSource::Camera, LogFile());
            return;
        }
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (replay::playback::TrailFocus(x, y, z))
        {
            replay::windreplay::PublishScene(x, y, z, replay::windreplay::SceneSource::Puppet, LogFile());
            return;
        }
        replay::windreplay::ClearScene();
    }

    // See PlayerLanding.
    void UpdatePlayerLanding(ULONGLONG now)
    {
        PlayerLanding& landing = g_playerLanding;
        if (!landing.active)
            return;
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
        {
            landing = PlayerLanding{};
            return;
        }
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        STREAMING::REQUEST_COLLISION_AT_COORD(at.x, at.y, at.z);
        const bool loaded = ENTITY::HAS_COLLISION_LOADED_AROUND_ENTITY(player) != FALSE;
        if (replay::landing::Decide(loaded, false, now, landing.deadline) == replay::landing::Verdict::Wait)
            return;
        ENTITY::FREEZE_ENTITY_POSITION(player, landing.frozen ? TRUE : FALSE);
        LogFile() << "TRAIL_PLAYER_FOLLOW,phase=landed,collision=" << (loaded ? 1 : 0)
                  << ",waited_ms=" << (now > landing.since ? now - landing.since : 0) << '\n';
        landing = PlayerLanding{};
    }
}

    void UpdateClipWorld(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.clip_world");
        UpdatePlayerLanding(now);
        replay::playback::UpdateWorldLanding();
        ClipWorld& world = g_clipWorld;
        UpdateMoonGiveBack(now); // after a clip, or after a reload in one
        if (!world.active)
        {
            replay::windreplay::ClearScene(); // only while the replay owns the world
            return;
        }
        // The map state at the playhead (a change crossed, a seek).
        if (!world.worldApplyPending)
            replay::world::UpdatePlayback(g_clip, static_cast<std::uint32_t>(g_transport.PlayheadMs()), now, LogFile());
        // Game scripts may show the player again (a mission, a cutscene).
        if (world.player && ENTITY::DOES_ENTITY_EXIST(world.player) && ENTITY::IS_ENTITY_VISIBLE(world.player))
            ENTITY::SET_ENTITY_VISIBLE(world.player, FALSE);
        UpdatePlayerFollow(world, now);
        PublishWindScene();
        if (g_clip.environment.recorded)
        {
            ApplyClipEnvironment(world, now); // the track at the playhead
            ApplyClipMoon(world, now); // after the clock
        }
        // Clouds "Live" in the editor leaves them to the engine.
        if (!g_clip.environment.sky.empty() && !g_clip.environment.edit.cloudsLive)
            ApplyClipSky(world, now); // the clouds at the playhead
        if (!g_clip.environment.timecycle.empty())
            ApplyClipTimecycle(world, now); // modifier and variations at the playhead
        // Product mode fades in once the world and the puppets are ready
        // (UpdateClipLoadWait); debug mode never fades.
        const bool jumpWait = world.loadPhase && std::strcmp(world.loadPhase, "jump") == 0;
        // The wait runs in debug mode too; only product mode fades.
        if (world.loadPhase && UpdateClipLoadWait(now) && world.fadeInPending)
        {
            CAM::DO_SCREEN_FADE_IN(jumpWait ? 300 : 500);
            world.fadeInPending = false;
        }
    }
    ExitLoadWait g_exitLoad;

    void StopExitLoadScene()
    {
        if (g_exitLoad.sceneActive)
            STREAMING::LOAD_SCENE_STOP();
        g_exitLoad.sceneActive = false;
    }

    void UpdateExitLoadWait(ULONGLONG now)
    {
        ExitLoadWait& wait = g_exitLoad;
        if (!wait.active)
            return;
        // A clip opened again: its own wait takes the black screen over. Its
        // LOAD_SCENE_START already replaced this wait's scene (the engine has
        // one): a stop now would end the clip's, which then never reports
        // loaded (QA logs: 6 of the 9 loaded=0 timeouts opened a clip in an
        // exit wait).
        if (g_clipWorld.active)
        {
            if (g_clipWorld.loadSceneActive)
                wait.sceneActive = false;
            StopExitLoadScene();
            wait = ExitLoadWait{};
            return;
        }
        now = std::max(now, wait.startedAt);
        const Vector3 at = CAM::GET_GAMEPLAY_CAM_COORD();
        if (!wait.sceneActive)
        {
            const Vector3 rotation = CAM::GET_GAMEPLAY_CAM_ROT(2);
            const float pitch = rotation.x * 3.14159265f / 180.0f;
            const float heading = rotation.z * 3.14159265f / 180.0f;
            STREAMING::LOAD_SCENE_START(at.x, at.y, at.z, -std::sin(heading) * std::cos(pitch), std::cos(heading) * std::cos(pitch),
                std::sin(pitch), 500.0f, 4);
            wait.sceneActive = true;
        }
        STREAMING::REQUEST_COLLISION_AT_COORD(at.x, at.y, at.z);
        const bool sceneLoaded = STREAMING::IS_LOAD_SCENE_LOADED() != FALSE;
        const std::uint64_t elapsed = now - wait.startedAt;
        const replay::clipload::Verdict verdict = replay::clipload::Evaluate(elapsed, sceneLoaded, true);
        if (!verdict.done)
        {
            DrawLoadingCaption(TR("hud.loading_world", "Loading..."));
            return;
        }
        StopExitLoadScene();
        CAM::DO_SCREEN_FADE_IN(500);
        _wremove(StateFile(kClipWorldStateFile).c_str()); // kept through the black (EndClipWorld)
        LogFile() << "CLIP_EXIT_WAIT,ms=" << elapsed << ",loaded=" << (sceneLoaded ? 1 : 0)
                  << ",timeout=" << (verdict.timedOut ? 1 : 0) << '\n';
        wait = ExitLoadWait{};
    }

    void EndClipWorld()
    {
        ClipWorld& world = g_clipWorld;
        if (!world.active)
            return;
        replay::windreplay::ClearScene(); // the gate asks about the live player again
        StopClipLoadScene();
        if (world.presentation)
            CAM::DO_SCREEN_FADE_OUT(0);
        // The map state goes back while the screen is dark.
        world.worldApplyPending = false;
        replay::world::EndPlayback(LogFile());
        if (g_clip.environment.recorded)
        {
            CLOCK::SET_CLOCK_TIME(world.hours, world.minutes, world.seconds);
            CLOCK::PAUSE_CLOCK(FALSE, 0);
            MISC::SET_CURR_WEATHER_STATE(world.weatherFrom, world.weatherTo, world.weatherMix, TRUE);
        }
        // The session's moon angles back once this clock jump back is consumed
        // (UpdateMoonGiveBack, a few frames).
        if (world.moonSaved)
        {
            g_moonGiveBack.Arm(world.moonSavedAngle, world.moonSavedOrbit, MISC::GET_FRAME_COUNT());
            g_moonGiveBackWhy = "end";
            LogFile() << "MOON_STATE,phase=end,angle=" << world.moonSavedAngle << ",orbit=" << world.moonSavedOrbit
                      << ",writes=" << world.moonWrites << ",write_fails=" << world.moonWriteFails
                      << ",source=" << (world.moonSource ? world.moonSource : "none") << '\n';
        }
        // The transition and the wind, rain and snow overrides the world had (only
        // what the replay wrote over).
        GiveBackTransition(world, "end");
        if (world.levelsForced)
        {
            RestoreWeatherOverrides(world.savedOverrides);
            world.levelsForced = false;
            LogFile() << "WEATHER_TRANSITION,phase=overrides_back,rain=" << world.savedOverrides.rain
                      << ",snow=" << world.savedOverrides.snow << '\n';
        }
        RestoreClipTimecycle(world);
        // Back where he stood, frozen until the collision there is in.
        if (world.playerMoved && world.mover && ENTITY::DOES_ENTITY_EXIST(world.mover))
        {
            replay::pose::playback::MoveHiddenEntity(world.mover, world.moverAt.x, world.moverAt.y, world.moverAt.z);
            ENTITY::SET_ENTITY_HEADING(world.mover, world.moverHeading);
            // A horse keeps the freeze of the hidden world, given back by the playback's
            // restore once the map collision at this place is in (world_landing.h).
            if (world.mover != world.player)
                ENTITY::_PAUSE_ENTITY_TRACKING(world.mover, FALSE);
            if (world.player && ENTITY::DOES_ENTITY_EXIST(world.player))
                ENTITY::_PAUSE_ENTITY_TRACKING(world.player, FALSE);
            STREAMING::REQUEST_COLLISION_AT_COORD(world.moverAt.x, world.moverAt.y, world.moverAt.z);
            g_playerLanding = PlayerLanding{true, world.playerFrozen, GetTickCount64() + replay::landing::kLandingTimeoutMs, GetTickCount64()};
            LogFile() << "TRAIL_PLAYER_FOLLOW,phase=returned\n";
        }
        RestorePlayer(world.playerVisible, world.playerFrozen || world.playerMoved, world.playerCollisionOff, world.playerInvincible);
        if (world.presentation)
        {
            StopExitLoadScene();
            g_exitLoad = ExitLoadWait{};
            g_exitLoad.active = true; // the fade in waits for the world (UpdateExitLoadWait)
            g_exitLoad.startedAt = GetTickCount64();
        }
        else
            _wremove(StateFile(kClipWorldStateFile).c_str());
        // In product mode the file stays until the exit wait fades in: a script
        // reload in that black finds it and fades in (no black screen left behind).
        LogFile() << "CLIP_WORLD,phase=end\n";
        world = ClipWorld{};
    }
