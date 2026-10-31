// script_theatre.cpp: theatre: the playback clock and world freeze, the world model checks, the theatre session.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"



    ULONGLONG PlaybackDurationMs()
    {
        return g_transport.DurationMs();
    }

    void LogTransportPaused(bool paused)
    {
        std::ofstream& log = LogFile();
        log << "PLAYBACK_TRANSPORT"
            << ",paused=" << (paused ? 1 : 0)
            << ",playhead_ms=" << g_transport.PlayheadMs()
            << ",actor_backend=pose_track_v2"
            << '\n';
        log.flush();
        ShowObjective(paused
            ? "Redemption Editor: timeline paused"
            : "Redemption Editor: timeline at +1x");
    }

    // The transport's clock. GetTickCount64 moves in 15.6 ms steps, so at 60 fps
    // the playhead would go 15/16 ms a frame with a 31/32 ms jump every ~15 frames
    // (visible on wagon wheels). Every transport call takes this clock, never the
    // tick count.
    std::uint64_t PlaybackClockMs()
    {
        static const double msPerTick = 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
        return static_cast<std::uint64_t>(static_cast<double>(replay::prof::Now()) * msPerTick);
    }
namespace
{

    // The paused replay stands still: the world (fire, particles, water, cloth,
    // wind, the ambient sound) freezes with the photo mode's freeze
    // (_REQUEST_PHOTO_MODE_FREEZE), which the time scale (at 1 while paused) never
    // does. It also holds the entities, so a moving playhead (scrub, frame step, a
    // marker jump) thaws it until the playhead has stood still for kFreezeSettleMs
    // and the puppets took the new pose. A file marks the freeze on disk: a script
    // reload in it finds the file and thaws the game (natives cannot run at unload).
    constexpr ULONGLONG kFreezeSettleMs = 250;
}
    const char* kWorldFreezeFile = "RedemptionEditor.world-freeze";
    // Written by an unload that could not see every detour or worker leave this
    // image; reported by the next load.
    const char* kUnloadReportFile = "RedemptionEditor.unload-report";
    WorldFreeze g_worldFreeze;

    void ThawWorld(const char* why)
    {
        WorldFreeze& freeze = g_worldFreeze;
        if (!freeze.active)
            return;
        ANIMSCENE::_REQUEST_PHOTO_MODE_DEFREEZE();
        _wremove(StateFile(kWorldFreezeFile).c_str());
        freeze.active = false;
        LogFile() << "WORLD_FREEZE,action=thaw,why=" << why << '\n';
    }

    void UpdateWorldFreeze(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.world_freeze");
        WorldFreeze& freeze = g_worldFreeze;
        if (!g_transport.Playing())
        {
            ThawWorld("stopped");
            freeze.lastPlayheadMs = ~0ull;
            return;
        }
        // A Ctrl+R may unload the mod before another tick: the world thaws now and
        // stays thawed for a few seconds (a key that reloaded nothing freezes it again
        // afterwards).
        const ULONGLONG reloadKeyAt = static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_reloadKeyAt, 0, 0));
        if (reloadKeyAt != 0 && now >= reloadKeyAt && now - reloadKeyAt < 5000)
        {
            ThawWorld("reload_key");
            freeze.thawUntil = now + kFreezeSettleMs;
            return;
        }
        const std::uint64_t playhead = g_transport.PlayheadMs();
        if (playhead != freeze.lastPlayheadMs || g_transport.Scrubbing())
            freeze.thawUntil = now + kFreezeSettleMs;
        freeze.lastPlayheadMs = playhead;
        // The freeze also holds the script cameras where they stand: the free camera
        // and Edit Camera never freeze, and a replay camera that moved (a marker
        // option, a new marker) thaws until it stands still again. A settle (a jump or
        // a scrub release) loading the target keeps the world running; it freezes once
        // the settle ends and stands still.
        if (g_freeCamEnabled || g_editingMarker || g_settle.Active() || ClipLoadWaiting())
            freeze.thawUntil = now + kFreezeSettleMs;
        // The freeze also holds the camera's DOF and near clip (the camera code returns
        // early while frozen, PhotoFreezeState), so a lens change thaws it like a
        // camera that moved; it freezes again after the last change, with the new
        // picture. 600 ms, not kFreezeSettleMs: the engine may ease its DOF toward the
        // new values over a few frames [hypothesis]; the freeze waits for it.
        constexpr ULONGLONG kLensThawMs = 600;
        const bool lensChanged = g_lensChangedAt != 0 && now >= g_lensChangedAt && now - g_lensChangedAt < kLensThawMs;
        if (lensChanged)
        {
            if (freeze.active && !g_lensHeldLogged && g_camLensLogs.Take(400))
            {
                g_lensHeldLogged = true;
                LogFile() << "CAMERA_DOF,phase=render_hold,freeze_state=" << PhotoFreezeState() << ",playhead_ms=" << playhead << '\n';
            }
            freeze.thawUntil = std::max(freeze.thawUntil, g_lensChangedAt + kLensThawMs);
        }
        else if (g_renderedCamera.valid)
        {
            const replay::clipload::Vec3& at = g_renderedCamera.position;
            const replay::clipload::Vec3& rotation = g_renderedCamera.rotation;
            if (replay::clipload::DistanceSq(at, freeze.lastCamera) > 1e-6f ||
                replay::clipload::DistanceSq(rotation, freeze.lastCameraRotation) > 1e-4f)
                freeze.thawUntil = now + kFreezeSettleMs;
            freeze.lastCamera = at;
            freeze.lastCameraRotation = rotation;
        }
        const bool want = g_recConfig.freezeWorldOnPause && g_transport.Paused() && now >= freeze.thawUntil;
        if (!want)
        {
            if (freeze.active && lensChanged)
            {
                ThawWorld("lens_changed");
                LogFile() << "CAMERA_DOF,phase=render_live,freeze_state=" << PhotoFreezeState() << ",playhead_ms=" << playhead << '\n';
                g_lensHeldLogged = false;
                return;
            }
            ThawWorld(g_transport.Paused() ? "playhead_moved" : "play");
            return;
        }
        if (freeze.active)
            return;
        ANIMSCENE::_REQUEST_PHOTO_MODE_FREEZE();
        if (FILE* file = _wfsopen(StateFile(kWorldFreezeFile).c_str(), L"w", _SH_DENYNO))
            std::fclose(file);
        freeze.active = true;
        if (++freeze.freezes <= 20)
            LogFile() << "WORLD_FREEZE,action=freeze,playhead_ms=" << playhead << ",count=" << freeze.freezes << '\n';
    }

    void SetPlaybackPaused(bool paused, ULONGLONG)
    {
        if (!g_transport.SetPaused(paused, PlaybackClockMs()))
            return;
        LogTransportPaused(paused);
    }

    // ---- the world model's engine access. Rung 2: the slot table, the load
    // operation and the queue count read under SEH; rung 3: the operation's
    // +0x10..+0x1C reset (an engine object no retail code uses: zero xrefs but its
    // exit destructor); rung 4: PushOnToSavegameQueue or LoadFromSlot called under
    // SEH. Every function, global and field of the path is found by signature
    // first (world_model.h; one missing disables the model for the session: no
    // clear, no reload).

    bool WorldModelChecks(std::string& error)
    {
        using namespace replay::worldmodel;
        if (g_world.checked)
        {
            error = g_world.checkError;
            return g_world.checksOk;
        }
        // The save reload through the menu's LoadFromSlot with the newest slot.
        const replay::sig::Signature* slotPathSignatures[] = {&kLoadFromSlot, &kMenuOpStatus, &kNewestSlot, &kSlotHeaders,
            &kSlotDateGetter, &kSlotDates};
        std::string failed;
        for (const replay::sig::Signature* signature : slotPathSignatures)
        {
            if (!replay::sig::Find(*signature))
            {
                failed = signature->name;
                break;
            }
        }
        g_world.checked = true;
        g_world.checksOk = failed.empty();
        g_world.checkError = failed.empty() ? "none" : failed;
        error = g_world.checkError;
        LogFile() << "WORLD_MODEL,phase=checks,path=slot,ok=" << (g_world.checksOk ? 1 : 0)
                  << ",failed=" << g_world.checkError << '\n';
        return g_world.checksOk;
    }

    // The engine's slot table as its newest-slot sort reads it.
    bool ReadWorldSlots(replay::worldmodel::SlotSummary& summary)
    {
        using namespace replay::worldmodel;
        // The tables where the sort and the date getter read them (the dates
        // address is the table's +0x2C already).
        const std::uintptr_t headers = replay::sig::Find(kSlotHeaders);
        const std::uintptr_t dates = replay::sig::Find(kSlotDates);
        if (!headers || !dates)
            return false;
        Slot slots[kSlotCount];
        for (std::uint32_t index = 0; index < kSlotCount; ++index)
        {
            std::uint8_t header[sizeof(kHeaderMagic)]{};
            std::int32_t date[6]{};
            if (!replay::mem::Copy(header, headers + index * kHeaderStride, sizeof(header)) ||
                !replay::mem::Copy(date, dates + index * kSlotStride, sizeof(date)))
                return false;
            slots[index] = SlotFromBytes(header, sizeof(header), date);
        }
        summary = Summarize(slots, kSlotCount);
        return true;
    }

    std::string WorldSlotDate(const replay::worldmodel::SlotSummary& summary)
    {
        if (summary.newest < 0)
            return "none";
        char text[48]{};
        std::snprintf(text, sizeof(text), "%04d-%02d-%02d_%02d:%02d:%02d", summary.date[0], summary.date[1], summary.date[2],
            summary.date[3], summary.date[4], summary.date[5]);
        return text;
    }

    std::uint32_t WorldPoolUsed(replay::pools::Kind kind)
    {
        const replay::pools::PoolState state = replay::pools::State(kind);
        return state.ok ? state.used : 0xFFFFFFFFu;
    }

    std::string WorldPoolCounts()
    {
        std::ostringstream text;
        const auto put = [&](const char* name, replay::pools::Kind kind) {
            const std::uint32_t used = WorldPoolUsed(kind);
            text << ',' << name << '=';
            if (used == 0xFFFFFFFFu)
                text << "na";
            else
                text << used;
        };
        put("peds", replay::pools::Kind::Ped);
        put("vehicles", replay::pools::Kind::Vehicle);
        put("objects", replay::pools::Kind::Object);
        return text.str();
    }
    TheatreSession g_theatre;
    ULONGLONG g_reloadPushedAtMs = 0; // survives the session reset: the reload's length
    bool g_reloadDoneLogged = true;

    // The ownership on disk, for the next copy in this process (a hot reload or an
    // unload with the world cleared: DllMain cannot run natives; theatre_exit.h).
    const char* kTheatreMarkerFile = "RedemptionEditor.theatre";
    replay::theatre::safeexit::Watchdog g_safeExit;
    std::uint32_t g_safeExitEditorCloses = 0; // added to sessionResetSequence: the add-on closes its screens
    bool g_tickException = false;
    bool g_scriptHookDev = false; // ScriptHookRDR2.dev in the game folder: Ctrl+R reloads the scripts
namespace
{
    std::string g_tickExceptionWhat;
    std::uint32_t g_tickExceptions = 0;
}

    void WriteTheatreMarker(bool owning, bool pushed, int slot)
    {
        replay::theatre::safeexit::Marker marker;
        marker.pid = GetCurrentProcessId();
        marker.owning = owning;
        marker.pushed = pushed;
        marker.slot = slot;
        const std::string line = replay::theatre::safeexit::FormatMarker(marker);
        FILE* file = nullptr;
        if (_wfopen_s(&file, StateFile(kTheatreMarkerFile).c_str(), L"w") == 0 && file)
        {
            std::fputs(line.c_str(), file);
            std::fclose(file);
        }
    }

    void RemoveTheatreMarker()
    {
        _wremove(StateFile(kTheatreMarkerFile).c_str());
    }

    // An exception left a tick (ScriptMain): the watchdog stops the replay
    // and reloads when the world is cleared; logged either way.
    void NoteTickException(const char* what)
    {
        g_tickException = true;
        g_tickExceptionWhat = what ? what : "";
        if (++g_tickExceptions <= 20)
        {
            LogFile() << "TICK_EXCEPTION,what=" << g_tickExceptionWhat << ",count=" << g_tickExceptions << '\n';
            LogFile().flush();
        }
    }

    // Every ped (and vehicle) of the world left now, and why: natives plus guarded
    // reads of the ped. The puppets are skipped.
    void TheatreCensus(const char* phase)
    {
        using namespace replay::theatre;
        constexpr int kCapacity = 1024;
        static int handles[kCapacity]{};
        const int count = (std::min)((std::max)(0, worldGetAllPeds(handles, kCapacity)), kCapacity);
        const Ped player = PLAYER::PLAYER_PED_ID();
        const int playerGroup = PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID());
        std::uint32_t byReason[static_cast<int>(LeftReason::Count)]{};
        std::uint32_t puppets = 0, world = 0, lines = 0;
        std::ofstream& log = LogFile();
        for (int index = 0; index < count; ++index)
        {
            const Ped ped = handles[index];
            if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
                continue;
            bool isPuppet = false;
            for (const replay::playback::Puppet& puppet : g_puppets.list)
                isPuppet = isPuppet || puppet.clone == ped;
            if (isPuppet)
            {
                ++puppets;
                continue;
            }
            PedFacts facts;
            facts.player = ped == player;
            facts.dead = PED::IS_PED_DEAD_OR_DYING(ped, TRUE) != FALSE;
            facts.animal = ENTITY::GET_IS_ANIMAL(ped) != FALSE;
            facts.perschar = PERSCHAR::_GET_PERSCHAR_INDEX_FROM_PED_INDEX(ped) != 0;
            facts.mission = ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped) != FALSE;
            Any scriptArgs[8]{};
            const Hash script = ENTITY::_GET_ENTITY_SCRIPT(ped, scriptArgs);
            facts.scripted = script != 0;
            facts.inVehicle = PED::IS_PED_IN_ANY_VEHICLE(ped, FALSE) != FALSE;
            facts.onMount = PED::IS_PED_ON_MOUNT(ped) != FALSE;
            facts.playerGroup = !facts.player && PED::IS_PED_GROUP_MEMBER(ped, playerGroup, FALSE) != FALSE;
            facts.population = ENTITY::GET_ENTITY_POPULATION_TYPE(ped);
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
            std::uint32_t f3f20 = 0, f3f30 = 0;
            std::uint8_t f1b8 = 0, f40 = 0;
            const bool read = address && replay::mem::Read(address + 0x3F20, f3f20) && replay::mem::Read(address + 0x3F30, f3f30) &&
                replay::mem::Read(address + 0x1B8, f1b8) && replay::mem::Read(address + 0x40, f40);
            facts.pendingDestroy = read && (f3f20 & (1u << 21)) != 0;
            facts.scene = read && (f3f30 & (1u << 28)) != 0;
            const LeftReason reason = ClassifyLeft(facts);
            ++byReason[static_cast<int>(reason)];
            if (!facts.player)
                ++world;
            if (!facts.player && lines++ < 60)
            {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE);
                log << "WORLD_LEFT,phase=" << phase << ",ped=" << ped << ",model=0x" << std::hex << ENTITY::GET_ENTITY_MODEL(ped)
                    << std::dec << ",reason=" << LeftReasonName(reason) << ",pop=" << facts.population
                    << ",mission=" << facts.mission << ",script=0x" << std::hex << script << std::dec
                    << ",perschar=" << facts.perschar << ",in_vehicle=" << facts.inVehicle << ",on_mount=" << facts.onMount
                    << ",group=" << facts.playerGroup << ",dead=" << facts.dead << ",animal=" << facts.animal
                    << ",pending=" << facts.pendingDestroy << ",scene=" << facts.scene << ",b1b8=" << (f1b8 & 1)
                    << ",b40=" << ((f40 >> 1) & 1) << ",read=" << read << ",at=" << static_cast<int>(at.x) << ':'
                    << static_cast<int>(at.y) << ':' << static_cast<int>(at.z) << '\n';
            }
        }
        static int vehicles[256]{};
        const int vehicleCount = (std::min)((std::max)(0, worldGetAllVehicles(vehicles, 256)), 256);
        std::uint32_t worldVehicles = 0, missionVehicles = 0;
        for (int index = 0; index < vehicleCount; ++index)
        {
            const Vehicle vehicle = vehicles[index];
            if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle))
                continue;
            bool isPuppet = false;
            for (const replay::playback::Puppet& puppet : g_puppets.list)
                isPuppet = isPuppet || puppet.clone == vehicle;
            if (isPuppet)
                continue;
            ++worldVehicles;
            missionVehicles += ENTITY::IS_ENTITY_A_MISSION_ENTITY(vehicle) ? 1 : 0;
            if (lines++ < 70)
                log << "WORLD_LEFT,phase=" << phase << ",vehicle=" << vehicle << ",model=0x" << std::hex
                    << ENTITY::GET_ENTITY_MODEL(vehicle) << std::dec << ",mission=" << ENTITY::IS_ENTITY_A_MISSION_ENTITY(vehicle)
                    << ",pop=" << ENTITY::GET_ENTITY_POPULATION_TYPE(vehicle) << '\n';
        }
        log << "WORLD_LEFT_SUMMARY,phase=" << phase << ",world_peds=" << world << ",world_vehicles=" << worldVehicles
            << ",mission_vehicles=" << missionVehicles << ",puppets=" << puppets;
        for (int r = 0; r < static_cast<int>(LeftReason::Count); ++r)
            if (byReason[r])
                log << ',' << LeftReasonName(static_cast<LeftReason>(r)) << '=' << byReason[r];
        log << ",perschar_held=" << PerscharHeld() << WorldPoolCounts() << '\n';
        log.flush();
    }
namespace
{

    void TheatreWorldSpeech(); // below
}

    // While the replay owns the world: the script threads paused every tick
    // (a thread launched since sleeps from its first tick on), and the
    // evidence rings drained into the log.
    bool FrontendScreenUp()
    {
        return HUD::IS_PAUSE_MENU_ACTIVE() || UISTICKYFEED::_UI_STICKY_FEED_IS_ALERT_SCREEN_ACTIVE();
    }

    void UpdateTheatre()
    {
        using namespace replay::theatre;
        if (!g_theatre.owning)
            return;
        // A frontend screen that waits for the player (the quit to desktop
        // alert, the pause menu). Its "Yes / No" prompts were hidden by the
        // per frame _UI_PROMPT_DISABLE_PROMPTS_THIS_FRAME of script.cpp, now
        // lifted while it is up (FrontendPrompts). Letting the game's scripts
        // run as well did not bring them back in game (
        // frontend_spare logged, prompts still missing), so it is a
        // developer key now (TheatreFrontendScripts, default 0).
        const bool frontend = g_recConfig.theatre.frontendScripts && FrontendScreenUp();
        if (frontend != g_theatre.frontendSpared)
        {
            g_theatre.frontendSpared = frontend;
            if (frontend)
            {
                ++g_theatre.frontendSpares;
                if (g_theatre.scriptsPaused)
                    ANIMSCENE::_PAUSE_SCRIPT_THREADS(FALSE);
            }
            LogFile() << "THEATRE,phase=frontend_" << (frontend ? "spare" : "end") << ",menu=" << (HUD::IS_PAUSE_MENU_ACTIVE() ? 1 : 0)
                      << ",alert=" << (UISTICKYFEED::_UI_STICKY_FEED_IS_ALERT_SCREEN_ACTIVE() ? 1 : 0)
                      << ",scripts_paused_before=" << (g_theatre.scriptsPaused ? 1 : 0) << ",spares=" << g_theatre.frontendSpares << '\n';
        }
        if (g_recConfig.theatre.pauseScripts && !frontend)
        {
            ANIMSCENE::_PAUSE_SCRIPT_THREADS(TRUE);
            const bool first = g_theatre.pauseCalls++ == 0;
            if (first)
                LogFile() << "THEATRE,phase=pause_scripts,first=1\n";
            g_theatre.scriptsPaused = true;
            // The native pauses every thread but ours, and ScriptHookRDR2 runs each ASI in
            // a thread of its own (another mod, e.g. a camera mod, would stop); those
            // threads run again in this same tick (world_theatre.h;
            // UnpauseForScriptHook).
            static std::vector<replay::theatre::ThreadView> census;
            census.clear();
            const replay::theatre::ThreadSpare spare = replay::theatre::SpareScriptHookThreads(first ? &census : nullptr);
            g_theatre.scriptHookUnpaused += spare.unpaused;
            g_theatre.scriptHookWriteFailed += spare.writeFailed;
            g_theatre.scriptHookThreads = spare.scriptHook;
            g_theatre.scriptHookError = spare.error;
            if (first)
            {
                std::ofstream& log = LogFile();
                log << "THEATRE_THREADS_SUMMARY,ok=" << (spare.ok ? 1 : 0) << ",error=" << spare.error
                    << ",threads=" << spare.threads
                    << ",paused_by_native=" << spare.pausedByNative << ",scripthook_threads=" << spare.scriptHook
                    << ",unpaused=" << spare.unpaused << ",write_failed=" << spare.writeFailed << ",list=";
                for (const replay::theatre::ThreadView& thread : census)
                    log << thread.id << ":0x" << std::hex << thread.scriptHash << std::dec << ":p" << static_cast<int>(thread.paused)
                        << (thread.scriptHash == replay::theatre::kScriptHookThreadHash ? ":sh" : "") << '|';
                log << '\n';
            }
        }
        TheatreWorldSpeech();
        static std::vector<SpawnCall> spawns;
        CollectSpawns(g_theatre.spawnCursor, spawns);
        for (const SpawnCall& call : spawns)
        {
            ++g_theatre.spawns;
            g_theatre.spawnsDenied += call.denied ? 1 : 0;
            ++g_theatre.spawnBySource[call.label];
            if (g_theatre.spawnLines++ < 40)
                LogFile() << "THEATRE_SPAWN,source=" << call.label << ",model_index=" << call.model << ",denied=" << call.denied
                          << ",caller=0x" << std::hex << call.caller << std::dec << ",at=" << static_cast<int>(call.position[0]) << ':'
                          << static_cast<int>(call.position[1]) << ':' << static_cast<int>(call.position[2]) << '\n';
        }
        static std::vector<SceneCall> scenes;
        CollectScenes(g_theatre.sceneCursor, scenes);
        for (const SceneCall& call : scenes)
        {
            ++g_theatre.scenes;
            ++g_theatre.sceneByHash[call.hash];
            if (g_theatre.sceneLines++ < 40)
                LogFile() << "THEATRE_SCENE,hash=0x" << std::hex << call.hash << ",caller=0x" << call.caller << std::dec << '\n';
        }
        static std::vector<RuleRefusal> refusals;
        CollectRefusals(g_theatre.refusalCursor, refusals);
        for (const RuleRefusal& refusal : refusals)
        {
            ++g_theatre.refusals;
            ++g_theatre.refusalByCode[refusal.code];
            bool puppet = false;
            for (const replay::playback::Puppet& p : g_puppets.list)
                puppet = puppet || (p.clone && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(p.clone)) == refusal.ped);
            if (g_theatre.refusalLines++ < 60)
                LogFile() << "SPEECH_STATE3_REFUSE,code=" << refusal.code << ",puppet=" << puppet << ",e3e0=0x" << std::hex
                          << static_cast<unsigned>(refusal.e3e0) << ",b2=0x" << static_cast<unsigned>(refusal.b2) << ",b3=0x"
                          << static_cast<unsigned>(refusal.b3) << ",caller=0x" << refusal.caller << std::dec << '\n';
        }
    }
namespace
{

    // Speech core calls on a speech entity that is not a puppet's while the replay
    // owns the world (nothing of the live world speaks): who it is (speech entity
    // +0x98 is its ped) and where from.
    void TheatreWorldSpeech()
    {
        static std::vector<replay::audio::SpeechCoreCall> calls;
        if (!g_theatre.coreCursorSet)
        {
            replay::audio::CollectSpeechCoreSince(g_theatre.coreCursor, calls); // skip what came before the clear
            g_theatre.coreCursorSet = true;
            return;
        }
        replay::audio::CollectSpeechCoreSince(g_theatre.coreCursor, calls);
        if (calls.empty())
            return;
        std::vector<std::uint64_t> puppets;
        for (const replay::playback::Puppet& p : g_puppets.list)
            if (p.clone && replay::entities::Exists(p.clone))
                puppets.push_back(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(p.clone)));
        const Ped player = PLAYER::PLAYER_PED_ID();
        const std::uint64_t playerAddress = player ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(player)) : 0;
        for (const replay::audio::SpeechCoreCall& call : calls)
        {
            std::uint64_t ped = 0;
            replay::mem::Read(static_cast<std::uintptr_t>(call.speechEntity + replay::theatre::kSpeechEntityPed), ped);
            bool puppet = false;
            for (const std::uint64_t address : puppets)
                puppet = puppet || address == ped;
            if (puppet)
                continue;
            ++g_theatre.worldSpeech;
            g_theatre.worldSpeechPlayer += ped && ped == playerAddress ? 1 : 0;
            if (g_theatre.worldSpeechLines++ < 20)
                LogFile() << "THEATRE_WORLD_SPEECH,player=" << (ped && ped == playerAddress) << ",ped=0x" << std::hex << ped
                          << ",entity=0x" << call.speechEntity << ",context=0x" << call.requestedContext << ",result=" << std::dec
                          << call.result << ",queued=" << call.queued << ",callers=0x" << std::hex << call.callers[0] << "/0x"
                          << call.callers[1] << "/0x" << call.callers[2] << std::dec << '\n';
        }
    }
}

    // The summary of the evidence and the end of the ownership (before the
    // reload's push, or when the session resets).
    void EndTheatre(const char* why)
    {
        if (!g_theatre.owning)
            return;
        UpdateTheatre();
        if (g_theatre.scriptsPaused)
            ANIMSCENE::_PAUSE_SCRIPT_THREADS(FALSE);
        // The persistent characters resumed before the load: with the manager's flag
        // left on, the load never ends. 0x2E957AA81F2C61C9 is the handler 0xFB151C that
        // writes [[0x4A35800]+0x10] = 0 (the GFM step 0x241BE68 does the same).
        if (g_theatre.perscharHeld)
        {
            PERSCHAR::_0x2E957AA81F2C61C9();
            LogFile() << "THEATRE,phase=perschar_resume,held_after=" << replay::theatre::PerscharHeld() << '\n';
        }
        replay::theatre::SetOwning(false, false);
        std::ofstream& log = LogFile();
        log << "THEATRE_SUMMARY,why=" << why << ",pause_calls=" << g_theatre.pauseCalls
            << ",frontend_spares=" << g_theatre.frontendSpares << ",scripthook_threads=" << g_theatre.scriptHookThreads << ",scripthook_unpaused=" << g_theatre.scriptHookUnpaused
            << ",scripthook_write_failed=" << g_theatre.scriptHookWriteFailed << ",scripthook_error=" << g_theatre.scriptHookError
            << ",spawns=" << g_theatre.spawns
            << ",spawns_denied=" << g_theatre.spawnsDenied << ",world_speech=" << g_theatre.worldSpeech
            << ",world_speech_player=" << g_theatre.worldSpeechPlayer << ",scenes=" << g_theatre.scenes << ",state3_refusals=" << g_theatre.refusals
            << ",by_source=";
        for (const auto& [source, n] : g_theatre.spawnBySource)
            log << source << ':' << n << '|';
        log << ",by_scene=";
        for (const auto& [hash, n] : g_theatre.sceneByHash)
            log << std::hex << "0x" << hash << std::dec << ':' << n << '|';
        log << ",by_code=";
        for (const auto& [code, n] : g_theatre.refusalByCode)
            log << code << ':' << n << '|';
        log << '\n';
        log.flush();
        g_theatre = TheatreSession{};
        // The world is still cleared (no reload yet): the next copy must still reload
        // it.
        if (g_world.cleared)
            WriteTheatreMarker(false, g_world.watching, g_world.clearedSlot);
    }
namespace
{

    // The engine's own clear, after the scripts' handshake and CLEAR_AREA:
    // persistent characters, vehicles (with the reuse pool) and trains, then every
    // ped but the player.
    void TheatreClear(Ped player)
    {
        using namespace replay::theatre;
        const Config& config = g_recConfig.theatre;
        const Checks& checks = RunChecks();
        std::ofstream& log = LogFile();
        log << "THEATRE,phase=checks,vehicles=" << checks.vehicles << ",peds=" << checks.peds << ",emitters=" << checks.emitters
            << ",perschar=" << checks.perschar << ",speech=" << checks.speech << ",failed=" << checks.failed
            << ",spawn_funnel=" << SpawnFunnelInstalled() << '\n';
        if (config.vehicles)
        {
            const std::string before = WorldPoolCounts();
            std::uint32_t code = 0;
            const CallResult result = RemoveAllVehicles(code);
            log << "THEATRE,phase=vehicles,result=" << CallResultName(result) << ",exception=0x" << std::hex << code << std::dec
                << ",before" << before << ",after" << WorldPoolCounts() << '\n';
            log.flush();
        }
        if (config.peds)
        {
            const std::string before = WorldPoolCounts();
            std::uint32_t code = 0;
            const std::uint64_t address = player ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(player)) : 0;
            const CallResult result = RemoveAllPeds(address, code);
            log << "THEATRE,phase=peds,mode=" << kRemovePedsForceAllRandomAndMission << ",result=" << CallResultName(result)
                << ",exception=0x" << std::hex << code << std::dec << ",before" << before << ",after" << WorldPoolCounts()
                << ",player_alive=" << (player && ENTITY::DOES_ENTITY_EXIST(player) ? 1 : 0) << '\n';
            log.flush();
        }
        // Whatever the kept player was saying stops.
        if (player && ENTITY::DOES_ENTITY_EXIST(player))
        {
            AUDIO::STOP_CURRENT_PLAYING_SPEECH(player, 0);
            AUDIO::STOP_CURRENT_PLAYING_AMBIENT_SPEECH(player, 0);
        }
        if (config.emitters)
        {
            std::uint32_t code = 0;
            const CallResult result = ResetStaticEmitters(code);
            log << "THEATRE,phase=emitters,result=" << CallResultName(result) << ",exception=0x" << std::hex << code << std::dec << '\n';
        }
    }
}

    // At the start of a playback, before the transport runs and before any puppet
    // exists (nothing the replay creates is in the world yet, so CLEAR_AREA cannot
    // take it). The first playback of a replay session clears; the next ones find
    // it cleared.
    void WorldModelEnter()
    {
        using namespace replay::worldmodel;
        if (g_recConfig.worldModel != Mode::Gta)
            return;
        EnterState state;
        state.mode = g_recConfig.worldModel;
        state.step = kStepReload;
        state.alreadyCleared = g_world.cleared;
        if (state.alreadyCleared)
        {
            LogFile() << "WORLD_MODEL,phase=clear,verdict=already_cleared,since_ms=" << (GetTickCount64() - g_world.clearedAt) << '\n';
            return;
        }
        state.inMission = MISC::GET_MISSION_FLAG() != FALSE;
        state.online = NETWORK::NETWORK_IS_GAME_IN_PROGRESS() != FALSE;
        SlotSummary slots;
        state.slotsRead = ReadWorldSlots(slots);
        state.newestSlot = state.slotsRead ? slots.newest : -1;
        std::string checkError;
        state.reloadReady = WorldModelChecks(checkError);
        const EnterVerdict verdict = DecideEnter(state);
        char flags[16]{};
        std::snprintf(flags, sizeof(flags), "0x%X", kDefaultClearFlags);
        if (verdict != EnterVerdict::Clear)
        {
            LogFile() << "WORLD_MODEL,phase=clear,verdict=" << EnterVerdictName(verdict) << ",mission=" << (state.inMission ? 1 : 0)
                      << ",online=" << (state.online ? 1 : 0) << ",slots_read=" << (state.slotsRead ? 1 : 0)
                      << ",slots_used=" << slots.used << ",slots_valid=" << slots.valid << ",newest=" << slots.newest
                      << ",newest_date=" << WorldSlotDate(slots) << ",checks=" << checkError
                      << (verdict == EnterVerdict::RefuseMission ? ",note=task20_d_missions_left_alone" : "") << '\n';
            // In a mission, a plain notice at every playback start; the other refusals
            // keep theirs, once (replay::worldmodel::EnterNotice).
            const std::string notice = EnterNotice(verdict, g_world.refusals++);
            if (!notice.empty())
                replay::hud::ShowNotice(notice.c_str());
            LogFile() << "WORLD_MODEL,phase=notice,verdict=" << EnterVerdictName(verdict) << ",shown=" << (notice.empty() ? 0 : 1)
                      << ",refusals=" << g_world.refusals << '\n';
            return;
        }
        if (!g_debugMode)
            CAM::DO_SCREEN_FADE_OUT(0); // the world is cleared behind a black screen
        const std::string before = WorldPoolCounts();
        const ULONGLONG started = GetTickCount64();
        // The world counts as cleared from here, before its first destructive step
        // (the steps below wait frames): on disk for the next copy (a hot reload in
        // the cleanup wait) and in the session for the watchdog (an exception half
        // way). theatre_exit.h EntryMarker.
        g_world.cleared = true;
        g_world.entering = true;
        g_world.clearedAt = started;
        g_world.clearedSlot = slots.newest;
        g_world.reloadStarted = false;
        g_world.idleSince = 0;
        {
            const replay::theatre::safeexit::Marker entry = replay::theatre::safeexit::EntryMarker(GetCurrentProcessId(), slots.newest);
            WriteTheatreMarker(entry.owning, entry.pushed, entry.slot);
        }
        int mainBefore = -1;
        int mainAfter = -1;
        {
            // The script cleanup handshake: the replay bit of FORCE_CLEANUP (the request is
            // queued, the scripts run their cleanup on their next update), then the
            // startup_sp's own test, the main script gone, up to 5 s.
            const Hash mainScript = MISC::GET_HASH_KEY("main");
            mainBefore = SCRIPTS::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(mainScript);
            PLAYER::FORCE_CLEANUP(kForceCleanupReplay);
            mainAfter = mainBefore;
            while (GetTickCount64() - started < kScriptCleanupWaitMs)
            {
                mainAfter = SCRIPTS::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(mainScript);
                if (mainAfter <= 0)
                    break;
                WAIT(0);
            }
        }
        const ULONGLONG waitedMs = GetTickCount64() - started;
        // Closed theater: two more frames after main is gone, so the scripts' entities
        // lose their script extension.
        for (int frame = 0; frame < 2; ++frame)
            WAIT(0);
        const Ped player = PLAYER::PLAYER_PED_ID();
        const bool playerInVehicle = player && ENTITY::DOES_ENTITY_EXIST(player) && PED::IS_PED_IN_ANY_VEHICLE(player, FALSE);
        int perscharBefore = replay::theatre::PerscharHeld();
        if (g_recConfig.theatre.perschar)
        {
            PERSCHAR::_0x92690B0822493CE0(); // persistent characters despawned and held (0xD1116C)
            g_theatre.perscharHeld = true;    // a release before the ownership resumes them (TheatreRelease)
        }
        const int perscharAfter = replay::theatre::PerscharHeld();
        // z = 0: the native looks for the ground only above a limit.
        MISC::CLEAR_AREA(0.0f, 0.0f, 0.0f, kClearRadius, static_cast<int>(kDefaultClearFlags));
        const bool trains = !playerInVehicle && (kDefaultClearFlags & kClearVehicles) != 0;
        if (!playerInVehicle)
            TheatreClear(player); // vehicles, peds, emitters through the engine
        if (trains)
            VEHICLE::DELETE_ALL_TRAINS(); // every train goes
        LogFile() << "THEATRE,phase=perschar,called=" << g_recConfig.theatre.perschar << ",held_before=" << perscharBefore
                  << ",held_after=" << perscharAfter << '\n';
        // The engine destroys deferred deletions over the next frames.
        WAIT(0);
        WAIT(0);
        TheatreCensus("enter");
        g_theatre = TheatreSession{};
        g_theatre.owning = true;
        g_theatre.perscharHeld = g_recConfig.theatre.perschar;
        replay::theatre::SetOwning(true, g_recConfig.theatre.denySpawn);
        const bool playerAlive = player && ENTITY::DOES_ENTITY_EXIST(player) && PLAYER::PLAYER_PED_ID() == player;
        g_world.cleared = true;
        g_world.clearedAt = GetTickCount64();
        g_world.clearedSlot = slots.newest;
        g_world.reloadStarted = false;
        g_world.idleSince = 0;
        g_world.entering = false;
        WriteTheatreMarker(true, false, slots.newest);
        LogFile() << "WORLD_MODEL,phase=clear,verdict=clear,flags=" << flags << ",radius=" << kClearRadius
                  << ",main_before=" << mainBefore
                  << ",main_after=" << mainAfter << ",wait_ms=" << waitedMs << ",trains=" << (trains ? 1 : 0)
                  << ",player_in_vehicle=" << (playerInVehicle ? 1 : 0) << ",player_kept=" << (playerAlive ? 1 : 0)
                  << ",slot=" << slots.newest << ",slot_date=" << WorldSlotDate(slots) << ",slots_valid=" << slots.valid
                  << ",before" << before << ",after" << WorldPoolCounts() << '\n';
        LogFile().flush();
        replay::hud::ShowNotice(
            TR("hud.world_cleared", "Redemption Editor: the world is cleared for the replay; leaving the replay loads your last save."));
    }
