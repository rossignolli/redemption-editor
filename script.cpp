#include "script.h"
#include "src/camera_track.h"
#include "src/i18n.h"
#include "src/camera_marker_shake.h"
#include "src/camera_bones.h"
#include "src/camera_spline.h"
#include "src/clip_file.h"
#include "src/clip_save_queue.h"
#include "src/clip_load_wait.h"
#include "src/buffer_coverage.h"
#include "src/clip_start_gate.h"
#include "src/world_landing.h"
#include "src/object_check.h"
#include "src/clip_compress.h"
#include "src/clip_compress_worker.h"
#include "src/zone_names.h"
#include "src/editor_bridge.h"
#include "src/playback_time.h"
#include "src/entity_appearance.h"
#include "src/entity_events.h"
#include "src/entity_lifecycle.h"
#include "src/entity_pools.h"
#include "src/entity_cache.h"
#include "src/rope_observer.h"
#include "src/skeleton_resolve.h"
#include "src/weapon_components.h"
#include "src/weapon_observer.h"
#include "src/reins_guard.h"
#include "src/anim_lod_hook.h"
#include "src/shader_vars_replay.h"
#include "src/entity_query.h"
#include "src/ped_audio_state.h"
#include "src/profiler.h"
#include "src/entity_selection.h"
#include "src/entity_transform.h"
#include "src/fx_observer.h"
#include "src/decal_observer.h"
#include "src/ped_gore.h"
#include "src/ped_blood.h"
#include "src/vegetation_speed.h"
#include "src/sky_state.h"
#include "src/weather_state.h"
#include "src/timecycle_state.h"
#include "src/sun_control.h"
#include "src/hook_batch.h"
#include "src/module_lifetime.h"
#include "src/event_ring.h" // the observer rings' counters (unload report)
#include "src/replay_buffer.h"
#include "src/replay_doors.h"
#include "src/replay_world.h"
#include "src/recording_window.h"
#include "src/frag_observer.h"
#include "src/water_observer.h"
#include "src/water_probe.h"
#include "src/export_channel.h" // render export: latency tag geometry
#include "src/export_probe.h" // render export
#include "src/fire_observer.h"
#include "src/audio_observer.h"
#include "src/sound_census.h"
#include "src/mix_probe.h"
#include "src/world_focus_core.h"
#include "src/game_listener.h"
#include "src/game_listener_core.h" // FollowsRecordedCamera
#include "src/mix_scene.h"
#include "src/speech_level_probe.h"
#include "src/volume_probe.h"
#include "src/world_sound_probe.h"
#include "src/wind_replay.h"
#include "src/category_state.h"
#include "src/speech_duck.h"
#include "src/sound_generic.h"
#include "src/soundfx_probe.h"
#include "src/capture_probe.h"
#include "src/render_target.h"
#include "src/pose_capture.h"
#include "src/pose_capture_rules.h" // kDefaultStallPollMs
#include "src/pose_fidelity.h"
#include "src/pose_playback.h"
#include "src/pose_segments.h"
#include "src/engine_hud.h"
#include "src/replay_bench.h"
#include "src/replay_capture.h"
#include "src/replay_context.h"
#include "src/burn_level.h"
#include "src/replay_events.h"
#include "src/horse_audio_session.h"
#include "src/replay_objects.h"
#include "src/object_look.h"
#include "src/replay_trains.h"
#include "src/vehicle_doors_engine.h"
#include "src/replay_playback.h"
#include "src/replay_puppets.h"
#include "src/replay_recorder.h"
#include "src/replay_recording.h"
#include "src/replay_ropes.h"
#include "src/replay_transport.h"
#include "src/replay_markers.h"
#include "src/replay_project.h"
#include "src/project_export_plan.h" // whole project export
#include "src/record_gate.h"
#include "src/rec_indicator.h"
#include "src/seek_settle.h"
#include "src/camera_cut.h"
#include "src/camera_overlay.h"
#include "src/free_cam.h"
#include "src/pose_stream.h"
#include "src/version.generated.h"
#include "src/log_policy.h"
#include "src/editor_keys_ini.h" // [Keys]
#include "src/deferred_log.h"
#include "src/mount_placement.h"
#include "src/puppet_collision.h"
#include "src/mod_clock.h"
#include "src/qa_report.h"
#include "src/qa_kit.h"
#include "src/qa_bundle.h"
#include "src/music_stop.h"
#include "src/qa_marks.h"
#include "src/session_reset.h"
#include "src/world_model.h"
#include "src/world_theatre_engine.h"
#include "src/theatre_exit.h" // the closed theater's safe exit
#include "src/thread_names.h"
#include "src/mod_identity.h" // Redemption Editor file names, old install migration
#include "src/storage_paths.h" // the base folder of every write (the game folder, else Documents)
#include "src/ini_public.h" // the shipped settings, written on the first run
#include "src/crash_dump_copy.h"
#include "src/crash_evidence_rules.h"
#include "src/guarded_memory.h"
#include "src/drawstore_probe.h"
#include "src/metaped_reader.h"

#include <algorithm>
#include <psapi.h>
#include <dbghelp.h> // MINIDUMP types (dbghelp.dll is loaded at run time)
#include <cstdarg>
#include <wincrypt.h> // SHA-256 of the ASI at load
#include <atomic>
#include <memory>
#include <thread>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <emmintrin.h>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    ULONGLONG PlaybackDurationMs();
    std::size_t PoseV2RecordedCount();
    std::size_t PoseV2PuppetCount();

    constexpr float kPi = 3.14159265358979323846f;
    constexpr ULONGLONG kMaxRecordingMs = 60000;
    constexpr Hash kInputLookLeftRight = 0xA987235Fu;
    constexpr Hash kInputLookUpDown = 0xD2047988u;
    constexpr float kFreeCamMouseSensitivity = 10.0f;
    constexpr float kFreeCamLookResponse = 18.0f;
    constexpr float kFreeCamMoveResponse = 7.0f;


    volatile LONG g_toggleRecordingRequested = 0;
    volatile LONG g_saveBufferRequested = 0; // Shift+F4
    volatile LONG g_togglePlaybackRequested = 0;
    volatile LONG g_toggleFreeCamRequested = 0;
    volatile LONG g_toggleOverlayRequested = 0;
    volatile LONG g_loadNextClipRequested = 0;
    volatile LONG g_qaReportRequested = 0; // Shift+F1
    // Debug mode (RedemptionEditor.ini [RedemptionEditor] Debug=1): the
    // development setup, the text HUD and the direct keys, with no product-only
    // steps (the editor's playback bar, the clip load presentation). Without it
    // the mod runs as the product: the editor is the only interface.
    bool g_debugMode = false;
    // RedemptionEditor.ini [RedemptionEditor] WaterShapeLog=1 installs the
    // read-only water disturbance observer (log only, off by default).
    bool g_waterShapeLog = false;
    bool g_cleanView = false; // bench camera preview: no HUD of any kind
    volatile bool g_editorCoversScreen = false; // the ImGui editor is open over the game (read by the key handler)
    // The editor draws the loading cue while the world loads (CommandType::SetLoadingCue,
    // sent every 2 s): its tick count, 0 never. Within 5 s of it the core's own
    // "Loading replay..." text is left out (the cue has the caption).
    volatile ULONGLONG g_editorLoadingCueAt = 0;
    bool EditorDrawsLoadingCue() { return g_editorLoadingCueAt != 0 && GetTickCount64() - g_editorLoadingCueAt < 5000; }
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
    // Simple camera: Insert adds a keyframe at the playhead from the free camera,
    // Delete removes the nearest one, End switches the track.
    volatile LONG g_cameraKeyAddRequested = 0;
    volatile LONG g_cameraKeyDeleteRequested = 0;
    volatile LONG g_cameraTrackToggleRequested = 0;
    // Playback speed: the marker speeds, [ and ] step through them. Below 1x the
    // world slows with it (SET_TIME_SCALE: particles, fire, the game's own
    // slow-motion audio), restored to 1 on pause, scrub, stop.
    constexpr double kPlaybackSpeeds[] = {0.05, 0.20, 0.35, 0.50, 1.00, 1.25, 1.50, 1.75, 2.00};
    constexpr int kNormalSpeedIndex = 4;
    int g_playbackSpeedIndex = kNormalSpeedIndex;
    volatile LONG g_speedStepRequested = 0; // -1 slower, +1 faster
    float g_worldTimeScaleApplied = 1.0f;
    volatile LONG g_togglePlayheadPauseRequested = 0;
    volatile LONG g_moveForward = 0;
    volatile LONG g_slowMove = 0;      // Ctrl held: the free camera moves slow
    float g_freeCamSpeedScale = 1.0f;  // the wheel's speed scale
    volatile LONG g_moveBackward = 0;
    volatile LONG g_moveLeft = 0;
    volatile LONG g_moveRight = 0;
    volatile LONG g_moveUp = 0;
    volatile LONG g_moveDown = 0;
    volatile LONG g_turnLeft = 0;
    volatile LONG g_turnRight = 0;
    volatile LONG g_turnUp = 0;
    volatile LONG g_turnDown = 0;
    volatile LONG g_rollLeft = 0;  // Z, the free camera's roll
    volatile LONG g_rollRight = 0; // C
    volatile LONG g_fovIn = 0;
    volatile LONG g_fovOut = 0;
    volatile LONG g_fastMove = 0;
    // The free camera's flight (free_cam.h: Classic, Drone, acrobatic, Walk,
    // collision, locked altitude), its target (follow, pick) and the markers drawn
    // in the world (camera_overlay.h). Keys and editor commands only set bits
    // here; the script thread takes them (ProcessFreeCamActions).
    replay::freecam::Settings g_fcSettings;   // FreeCam* keys of the ini and the editor's options
    replay::freecam::State g_fcState;         // momentum and orientation between ticks
    replay::freecam::Follow g_fcFollow = replay::freecam::Follow::Off;
    std::uint32_t g_fcTarget = 0;             // packed replay id, 0 none
    bool g_fcLocked = false;                  // the camera holds still (L)
    bool g_fcPadActive = false;               // the pad moved it last
    bool g_fcTargetLost = false;              // the target is not in the clip at the playhead
    replay::freecam::TargetFrame g_fcTargetFrame{};
    bool g_fcTargetFrameValid = false;
    volatile LONG g_fcActionBits = 0;
    enum FreeCamActionBit : LONG
    {
        kFcDrone = 1 << 0,
        kFcAcrobatic = 1 << 1,
        kFcWalk = 1 << 2,
        kFcCollision = 1 << 3,
        kFcLockAltitude = 1 << 4,
        kFcFollow = 1 << 5,
        kFcPick = 1 << 6,
        kFcPickNearest = 1 << 7,
        kFcLevel = 1 << 8,
        kFcBehind = 1 << 9,
        kFcLock = 1 << 10,
        kFcClearTarget = 1 << 11,
        kOverlayToggle = 1 << 12
    };
    bool g_camOverlayEnabled = true;       // CameraOverlay=1
    bool g_camOverlayWhilePlaying = false; // CameraOverlayWhilePlaying=0: shown paused, in Edit Camera and the free camera
    bool g_camOverlayTimeLabels = true;
    bool g_camOverlayTrail = true;
    // The Spline defaults (RedemptionEditor.ini Spline*; the editor's Spline
    // Defaults rows write them back). A marker's tuning overrides some fields.
    replay::editor::SplineTuning g_splineDefaults = replay::editor::DefaultSplineTuning();
    volatile LONG g_moduleDetaching = 0;
    // Set when another copy of the mod runs in this process (an old ReplayProbe.asi
    // or a second RedemptionEditor.asi). This copy then starts nothing: no hook, no
    // thread, no recording; it only says so in the log and on screen.
    volatile LONG g_refusedStart = 0;
    // The game restarted its scripts (ScriptMain ran again in this module) and
    // ShutdownSessionAfterRestart zeroed the session. The snapshot carries the
    // count (sessionResetSequence): the add-on closes its screens when it grows.
    // The notice for the player waits for the end of the loading screen.
    std::uint32_t g_sessionResets = 0;
    bool g_sessionRestarted = false; // this ScriptMain is a restart (set before the setup)
    std::string g_sessionResetNotice;
    // [Keys] values refused at load, said once the game is up.
    std::string g_keysNotice;
    // ReportIncludeClips=1 adds the open clip to the Shift+F1 report; by default
    // it carries only the clips the player marked (Shift+F2).
    bool g_reportIncludeClips = false;

    // The key the table holds for an action ("F7", "J / U"), or "unbound" (texts
    // of the game notices).
    std::string KeyText(replay::keys::Action action)
    {
        const std::string text = replay::keys::Keys(replay::keys::Current(), action);
        return text.empty() ? std::string("unbound") : text;
    }
    // The exit from the replay that reloads the save sets this before it pushes
    // the load, with the clip and the project already saved. The restart that
    // follows reads it (SESSION_RESET,why=own_reload): not a loss, nothing to tell
    // the player.
    replay::session::ExitReloadRequest g_exitReloadPending;
    // The world model's state for this session of the scripts (zeroed by
    // ShutdownSessionAfterRestart: the reload ends it).
    struct WorldModelSession
    {
        bool checked = false;      // the reload path's signatures and sites, once
        bool checksOk = false;
        std::string checkError;
        bool cleared = false;      // a playback cleared the world (b)
        ULONGLONG clearedAt = 0;
        int clearedSlot = -1;      // the newest slot seen at the clear
        bool reloadStarted = false; // pushed, or given up (c)
        ULONGLONG idleSince = 0;   // out of the replay since (0: in it)
        ULONGLONG pushWaitSince = 0;
        bool watching = false;     // the load's status is logged
        ULONGLONG pushedAt = 0;
        int lastStatus = -99;
        bool recordRefusedLogged = false;
        bool clearAfterLogged = false; // the pools 1 s after the clear (deletions the engine defers)
        std::uint32_t refusals = 0;
        // The safe exit (theatre_exit.h).
        bool forceReload = false;   // push now, in or out of the replay (the watchdog's verdict)
        bool reloadFailed = false;  // the last attempt failed (WorldReloadFailed)
        ULONGLONG reloadFailedAt = 0;
        bool recovered = false;     // cleared by a copy unloaded before (the marker)
        bool gaveUpNoticed = false;
        // WorldModelEnter between its first destructive step and the theater's
        // ownership (theatre_exit.h EntryMarker, UndoPartialEntry).
        bool entering = false;
    };
    WorldModelSession g_world;
    // The last HOOK_WARMUP (SESSION_CHECK after a restart).
    MH_STATUS g_lastWarmUpStatus = MH_OK;
    int g_lastWarmUpApplied = 0;
    // Tick count of the last Ctrl+R (ScriptHook's reload key with the .dev file).
    // The unload's DllMain cannot run natives, so a paused replay's world freeze
    // is lifted by the ticks that still run between the key and the unload
    // (UpdateWorldFreeze).
    volatile LONG64 g_reloadKeyAt = 0;
    std::uint64_t g_loggedEditorBridgeEpoch = 0;
    bool g_overlayEnabled = true;
    bool g_recording = false;
    std::uint32_t g_savedClipMs = 0; // length of the last clip written (the editor's notice)
    // How long the recording start took and the frame gaps of its first ticks.
    double TicksMs(std::uint64_t ticks)
    {
        return static_cast<double>(ticks) * 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
    }

    struct RecordingTiming
    {
        double startMs = 0;
        std::uint64_t lastTick = 0;
        int ticks = 0;
        double firstMaxMs = 0; // the first 180 ticks
        int firstOver33 = 0;
        int firstOver100 = 0;
        double maxMs = 0;      // the whole recording
        int over100 = 0;
    };
    RecordingTiming g_recTiming;
    int g_recordingsThisLoad = 0;

    // Recording modes: the always-on replay buffer (a ring of the last seconds,
    // saved on demand) and the manual recording (start, stop saves).
    // RedemptionEditor.ini [RedemptionEditor]:
    //   ReplayBuffer=1|0         the buffer runs whenever nothing plays (default on)
    //   ReplayBufferSeconds=60   the ring's window
    //   ReplayBufferMB=128       the ring's memory budget (the head moves on
    //                            when the recording holds more)
    //   ManualMaxSeconds=0       0: no time cap
    //   ManualMaxMB=900          a manual recording over this (32 to 960; with
    //                            the buffer on, over its budget when larger) is
    //                            saved as a part and continues in the next
    //                            (chained, buffer_coverage.h ManualPartBytes)
    // Clip compression (src/clip_compress.h):
    //   CompressClips=1          compress the clips not written nor opened
    //                            for CompressAfterDays days, in the background
    //   CompressAfterDays=1      1 to 60 (a save writes the clip already
    //                            packed with fast zstd blocks; the compressor
    //                            repacks it with LZMA after these days)
    //   CompressLevel=19         1 (fast) to 22 (smallest): the LZMA level of
    //                            the packed format (19 and up: 9), the zstd
    //                            level with CompressFormat=1
    //   CompressFormat=2         dev build only: 2 the packed container
    //                            (clip_pack.h; zstd clips are repacked), 1 the
    //                            zstd frame as before 1.0
    // The recording gate (src/record_gate.h):
    //   RecordCutscenes=0        0: the recording pauses while a cutscene
    //                            plays (the clip cuts past it); 1: kept
    //   LockCutsceneCamera=0     1: markers inside a recorded cutscene use
    //                            the recorded game camera
    //   RecordingIndicator=normal  the REC notice: normal, brief, hidden (out of
    //                            screen capture) or off (src/rec_indicator.h)
    // The clip's map state (src/replay_world.h):
    //   RestoreWorldState=1      apply the clip's IPLs and entity sets in its
    //                            area at playback and give them back on exit
    //                            (0: only the notice; the bench can turn it off)
    //   RestoreWorldStateInMission=0  1: apply even while a mission runs
    // The entity pools (src/entity_pools.h):
    //   EntityPoolReader=1       the recorder, the world clear and the object
    //                            listing read the engine's pools; 0: the
    //                            ScriptHook listings (and the nearby lists)
    // Trains (src/replay_trains.h):
    //   TrainPuppet=0            0: each car a vehicle puppet, an object when
    //                            the engine refuses or moves it; 1: vehicle
    //                            only; 2: object only
    //   TrainWorldClear=0        0: the world's trains hidden (the ambient
    //                            ones halted) while the replay plays; 1: also
    //                            DELETE_ALL_TRAINS at playback start
    // Effects, speech and puppets:
    //   FxOwnerRule=1            1: while a clip plays, a one-shot particle
    //                            effect the engine starts on its own on a
    //                            puppet or the hidden player is killed at its
    //                            start (the recorded one plays; blood,
    //                            explosion, fire, weapon sites exempt). The fx
    //                            check report (ReplayDebug\<clip>.fx-check.txt)
    //                            is always on.
    //   FireUntrackedInitial=0   1: a fire of the engine's list the start detour
    //                            never saw (lit before the mod loaded) and not
    //                            owned by a particle effect enters the clip as
    //                            an initial fire with a descriptor rebuilt from
    //                            the fire (at record start and when it comes
    //                            within 150 m); 0: counted and logged only
    //                            (POSE_V2_FIRE_UNTRACKED).
    //   FireAdoptLive=0          1: a live fire matching a recorded initial one
    //                            is kept (clip-aware clear) instead of the
    //                            native clear and the relight; 0 (default):
    //                            cleared and relit (an adopted molotov can go
    //                            out early).
    //   SpeechPlayerOnPlayer=1   1: the recorded player's lines are said by the
    //                            hidden real player (the engine plays the
    //                            player's speech louder than any ped's), only
    //                            while he stands within 3 m of the puppet (not
    //                            in a mission, where he stays put): farther,
    //                            the puppet says the line; 0: by his puppet.
    //   SpeechReplayRules=1      1: a line the replay asked passes the rules'
    //                            world context codes 28, 41 and 48 (rules
    //                            detour 0x2F972C4, speech_replay_rules.h); 0:
    //                            the engine's codes.
    //   FxWorldSiteRule=1        with FxOwnerRule, the engine's one-shots from
    //                            the sites of the clip's world one-shots are
    //                            killed while it plays (the fx track plays the
    //                            recorded ones).
    //   FxRegisteredSplit=1      1: every creation of a registered particle key
    //                            while recording is its own track (hoof dust
    //                            bursts); 0: one track per live key.
    //   Each of the following, at 0, turns its part off:
    //   FxMotionProbe=1          read-only evidence lines (FX_RELOCATE_PROBE,
    //                            FX_REG_RECREATE, FX_VELADD_PROBE, capped)
    //   FxPuppetNoWarp=1         clears the relocate mark the puppet placement
    //                            sets on our effects (small steps, normal play)
    //   FxPuppetNoWarpDetour=0   reserve: skips the marking (detour 0x143C180)
    //   FxRecordSettings=1       records the instance settings (FXSS, RGSS) and
    //                            the stretches without a call (with the next key)
    //   FxSettingsReplay=1       writes the recorded settings before the start
    //   FxRegisteredParentOnce=1 attached registered effect: parent at creation only
    //   FxRegisteredCallGaps=1   the registered key not called where the game
    //                            did not call it (stop and restart bursts)
    //   FxRegisteredOwnerReady=1 get or create only when the entity is ready
    //   FxRegisteredRecreateBackoffMs=250  wait after a recreation in a stretch
    //   FxPuppetVelocity=1       the puppet's motion as the particles' inherited
    //                            velocity (detour 0x1405BA4)
    //   FxWorldOwnerRule=1       the engine's own effect on a map object our
    //                            track rides is refused while ours lives
    //   FxOwnerListImmediate=1   a new puppet enters the refused owners at once
    //   WeaponFxEvents=1         muzzle and impact recorded as fx events
    //   FxApplyRecordedFlags=1   the recorded attach bits on attached one-shots
    //   ShotFxLegacyDedupe=1     old clips: a weapon effect two shots took plays once
    //   FxPauseFreeze=1          paused while the replay owns the world, the vfx
    //                            update gets a step of 0 (detour 0x145C17C)
    //   FxCheckMotion=1          the fx check's motion, restart, blink, settings
    //                            and weapon columns and flags
    //   FxRelocateClearStill=1   with FxPuppetNoWarp, the relocate mark of our
    //                            effect on an owner the playback did not place
    //                            this tick (a still prop, a map object) is
    //                            cleared when that owner moved less than the
    //                            instance's limit and 1 m
    //   WeaponFxIntent=1         records the weapon system's requests for each
    //                            shot's muzzle flash and shell (detours
    //                            0x14309A8, 0x14308F0, section WFXI) before
    //                            the engine's range and camera tests; the shot
    //                            audit names the shots the engine culled. A
    //                            clip with WFXI calls the same wrappers on the
    //                            puppets per request (the recorded muzzle
    //                            effects and shell tracks of those shots are
    //                            not played) and measures the sound around
    //                            each call (WEAPON_FX_INTENT_PLAY). 0: neither.
    //   EngineFeetInReplay=0     0: while a clip plays, the puppets' and the
    //                            hidden player's own footsteps and the
    //                            engine's foot puffs are kept out (the
    //                            recorded ones play); 1: kept in
    //   HorseAudioEvidence=0     1 logs the player's horse audio
    //                            (POSE_V2_HORSE_*), live horse against puppet
    //   FootMaterialReplay=1     the recorded standing material of each foot
    //                            set on the puppet's step container
    //                            (src/foot_material.h); 0: the puppet's own
    //                            ground probe decides
    //   FootLayerReplay=1        the recorded ground layer weights of each foot
    //                            (FOOT v6) on the puppet's foot before the
    //                            step plays; read at each playback start
    //   PuppetAudioLodRecorded=1 the puppets' audio LOD override set to the
    //                            recorded LOD of their steps instead of 0
    //   PuppetUnrecordedPersistent=1 a puppet's own persistent sound by ref
    //                            without any counterpart in the clip is refused
    //   SoundGenericCoreBByRef=1, SoundGenericKeepLoops=1  creator B packets by
    //                            ref; loops kept across a jump in play
    //   SoundGenericWorldCap=1, SoundGenericWorldCapCount=6  a world packet
    //                            counts the slot cap with its own sound, not
    //                            with every ownerless packet (0: the shared cap)
    //   SoundGenericVehicleBlock=1 a puppet vehicle's or train car's own create
    //                            of a sound the recording plays then is refused
    //                            at the census creators (0: its own copies play
    //                            over the packets)
    //   WorldSoundProbe=VolumeProbe  read only, PTFX_AUDIO_SLOT and GROUP_AUDIO
    //                            (rec and play)
    //   ObjectPosePlaces=0       capture places for objects with a skeleton
    //                            (the carried pelt, a drawer): 0 shares the
    //                            slots the bodies leave with the weapons in a
    //                            hand; 1..8 the old rule (that many places
    //                            given up by the ped class), for A/B only
    //   HatLook=1                (src/hat_look.h) a hat object records the
    //                            metaped layer it draws (read from its own
    //                            tags) and playback rebuilds it with that look:
    //                            the owner puppet knocks it off once the layer
    //                            is on, else a hidden helper ped wears and drops
    //                            it; a hat model without a recorded look (older
    //                            clips) is not drawn (the bare model is a grey
    //                            octagon). 0: the bare model.
    //   PedCapacity=56           peds and horses recorded at once, nearest
    //                            first (8 to 120; the vehicle places come on
    //                            top). 120 records a town's whole crowd out to
    //                            the 150 m enter radius at about twice the
    //                            capture and playback cost of 56
    //   SpawnedObjects=1         free objects the engine creates next to a
    //                            recorded ped (an animation's props, the pelt
    //                            rolled before it is in the hand) recorded once
    //                            they move or animate, posed when animated; 0:
    //                            logged only
    //   ObjectBurnLevel=1        the burn level of the recorded weapon objects,
    //                            props and scenery objects (the meat a camp's
    //                            script cooks with _SET_OBJECT_BURN_LEVEL) is
    //                            recorded (sections BLVL and SBLV) and written
    //                            on their puppets and stand-ins; 0: neither
    //   WeaponPassPose=1         the pose of a replay weapon (lever, cylinder,
    //                            hammer) written again right after the weapon
    //                            system's per-frame pass (detour 0x117D49C),
    //                            which writes the weapon object's own bones
    //                            over it; 0: not installed (WEAPON_PASS_POSE,
    //                            WEAPON_BONES_DRAWN measure either way)
    //   StillPoseEngine=1        an attached object with a skeleton whose pose
    //                            record never changed (a pelt on the horse:
    //                            constant track, detour_calls=0,
    //                            never_animated=1) is not pose-armed; the
    //                            engine poses the clone attached to the same
    //                            bone, as live (POSE_V2_OBJECT_STILL); 0: armed
    //   ObjectDrawnCheck=0       (evidence) each tick the pose the last frame
    //                            drew on a posed or still object against the
    //                            expected one and the bind pose
    //                            (POSE_V2_OBJECT_DRAWN,
    //                            POSE_V2_OBJECT_DRAWN_SUMMARY, write_checks)
    //   ObjectLook=1             the custom texture (document text: letters,
    //                            notes, telegrams, newspapers, posters, photos;
    //                            object +0x420/+0x424/+0x428) and the tint
    //                            (entity +0xB8 bits 3..8) of the recorded
    //                            weapon objects, props and scenery objects are
    //                            read (guarded) and recorded (section OLOK) and
    //                            set back on their puppets and stand-ins with
    //                            REQUEST_STREAMED_TXD,
    //                            SET_CUSTOM_TEXTURES_ON_OBJECT and
    //                            SET_OBJECT_TINT_INDEX; 0: neither
    //   ObjectLookEvidence=0     1: every read logged (OBJECT_LOOK_READ, first
    //                            read and changes per object, 256 lines); with
    //                            ObjectLook=0 only that (the layout check)
    //   SceneryHandoffTicks=1    a scenery or world object stand-in stays shown
    //                            until the prop that takes it over has been
    //                            posed for more than this many ticks (0..8; 0:
    //                            hidden in the tick the prop is ready, a one
    //                            frame flicker)
    //   SpawnedStaticScenery=1   such an object that stands still for a second
    //                            becomes a late scenery entry when its owner is
    //                            a script's, a temporary one or a prop set's
    //                            (a camp's tent, bedroll and grate) or it is a
    //                            pickup; 2: also any other owner that is not
    //                            the map's (mode 1 logs them as
    //                            would_scenery); 0: dropped as static after 20 s
    //   RefusePuppetOwnPersistent=1 a puppet's own persistent ped audio sound
    //                            (foley, clothing) from a call site the clip's
    //                            ESND track carries is refused (the recorded
    //                            one plays); 0: not refused
    //   PuppetPersistentByEntry=1 that refusal needs the same entity, call site
    //                            and sound entry in the clip (a loop the clip
    //                            never carried, such as the horse's tack loop,
    //                            plays from the puppet); 0: the call site alone
    //   RegisteredFxOwnerCheck=1 a registered effect's track takes no owner
    //                            from a new tenant of its instance address,
    //                            and an attached track recorded far from its
    //                            puppet is not created on it; 0: no check
    //   PedAudioLoopFollowsPuppet=1 a replayed ped audio loop with a recorded
    //                            position follows its puppet; 0: it stays where
    //                            it was created
    //   PedAudioKeepRecordedLength=1 a replayed short ped or train car audio
    //                            sound stops after its recorded length (the
    //                            horse's braking slide); 0: it plays its whole
    //                            sample
    //   AnimalVocalRetry=1       an animal line killed while loading is asked
    //                            again through the animal's own vocalization;
    //                            0: left silent
    //   AnimalBankHold=1         (src/animal_bank.h) the bank of the recorded
    //                            voice of an animal's next line is loaded
    //                            (AnimalBankLeadMs ahead, default 4000) and
    //                            held by the speech manager; a line asked with
    //                            its recorded voice goes by context while that
    //                            bank is not loaded. 0: the manager rotates
    //                            the banks on its own
    //   MountPreRender=0         (src/mount_placement.h) 1 sets a rider
    //                            puppet, at the ped pre-render, where his
    //                            mount puppet really is (the recorded
    //                            rider-in-mount offset on the mount's
    //                            transform then), so a horse the engine
    //                            pushed takes its rider along in the drawn
    //                            frame; 0: only measured (the same reads,
    //                            POSE_V2_MOUNT_PRERENDER_SUMMARY), no write
    //   JitterFix=1              the default of the two keys below; 0 turns
    //                            both off
    //   FrameEnd=1               every placed ped, horse and vehicle puppet
    //                            is set back to this tick's placement at its
    //                            pre-render (an end-of-frame write), and a ped
    //                            the pose finalizer skipped gets its pose there
    //                            (POSE_V2_PRERENDER_SUMMARY); 0: riders only,
    //                            as MountPreRender says
    //   PhysicsFilter=1          a walking puppet's active body collides only
    //                            with plants (its physics instance's include
    //                            flags ANDed with PuppetIncludeKeep), no
    //                            pushed hold (POSE_V2_COLLISION_FILTER_SUMMARY);
    //                            0: world collision, the other puppets kept
    //                            off pair by pair
    //   PuppetIncludeKeep=0x01E8C000 the include bits kept under PhysicsFilter
    //                            (env cloth 14, plant 15, foliage 19, query
    //                            classes 21 to 24); hex or decimal
    //   ProfileSlowTickMs=50     ticks this long get a PROFILE_SLOW_TICK line
    //                            with their largest zones (5 to 1000)
    // The sound census (src/sound_census.h):
    //   SoundCheck=Debug         the engine sound census in its cheap mode in
    //                            every recording and playback, and at each
    //                            playback's end ReplayDebug\<clip>.sound-check.txt
    //                            (sounds missing, fewer, extra, refused);
    //                            default 1 only with Debug=1
    //   SoundCensus=0            1: also a SOUND_CENSUS line per sound (bench)
    //   SoundCapture=1           sounds of recorded vehicles, train cars and
    //                            objects no track covers are recorded and
    //                            replayed on the puppet (2: every owner; 0: off)
    // The sound port probes (src/soundfx_probe.h):
    //   SoundFxProbe=0           1: read probes of the sound and particle
    //                            ports. A read-only detour per probed port
    //                            (signature checked) counts calls, threads,
    //                            callers and values; nothing is recorded or
    //                            changed. ReplayDebug\soundfx-probe-<date-
    //                            time>.txt every 60 s and at the unload
    //                            (tools/soundfx_probe_report.py reads it).
    //                            0: no detour of the probe is installed
    // The generic sound effects (src/sound_generic.h):
    //   SoundGeneric=1           1: the generic sound effect recorder. Every
    //                            top-level sound effect made while recording
    //                            becomes create, adjust and end packets in
    //                            the clip (section SGEN) through the sound
    //                            port detours (the probe's hooks, installed
    //                            for the recorder's ports with this key);
    //                            the player plays them. SOUND_GENERIC log
    //                            line every 60 s and at the stop;
    //                            ReplayDebug\<clip>.sound-generic.txt when
    //                            the clip is saved. 0: nothing runs
    //   SoundGenericUpdateHz=20  adjust samples per second per sound and
    //                            setting (a changed value only)
    //   SoundGenericMaxMBPerMin=8 clip bytes the track may add per minute:
    //                            past it the adjust rate halves per step,
    //                            then the adjusts stop, then the creates
    //                            and ends (BUDGET line of the report)
    //   SoundGenericPlay=1       0: the packets are counted in the
    //                            sound-check report, nothing plays
    //   SoundGenericExport=0     1 keeps the generic player on while an
    //                            export owns the clock; 0 (default) stops
    //                            it when the export starts and resumes it
    //                            when the export ends (packets due
    //                            meanwhile count as disabled)
    //   SoundGenericPreloadMs=500 a sound that waited for a load in the
    //                            recording is created and prepared (S10)
    //                            this early; 0: no preload
    //   SoundGenericPreloadDelayMs=100 the recorded wait that marks such a sound
    //   SoundGenericHoldMs=150   the wait after a packet's time for the
    //                            puppet (or another track) to make the
    //                            sound itself; then it is not played
    //   SoundGenericSlots=64     sounds the player holds at once (8 to 256)
    //   SoundGenericTake=        family:taker;... who plays a family in
    //                            the replay (takers generic, specific,
    //                            puppet; families world, owned, speech,
    //                            footstep, horse_step, movement, melee,
    //                            gunshot, explosion, collision,
    //                            entity_sound, sound_request, captured;
    //                            src/sound_owner_table.h has the defaults)
    //   SoundGenericMode=packets in the replay the engine's own sounds of the
    //                            puppets are refused at the creation ports
    //                            (S03, S04, S05, S06; the speech's sound
    //                            controller apart) and every recorded packet
    //                            of a replay entity plays from the clip;
    //                            speech, steps, melee, shots and explosions
    //                            keep their tracks. legacy: per-track rules
    //   SoundGenericBlock=packet the ports refuse a puppet's own create only
    //                            when the recording plays that sound then
    //                            (a packet of the same owner and sound alive,
    //                            or starting within 3 s) or another set at
    //                            the entry a recorded packet fills; the rest
    //                            is left to the puppet and counted. all:
    //                            every own create refused
    //   SoundGenericTrace=       owner@fromMs-toMs (0:1@45000-48000, or
    //                            *@... for every owner): one log line
    //                            SOUND_GENERIC_TRACE per packet event in the
    //                            window (at most 600 per playback)
    //   SoundGenericEnvGroup=1   a replayed create of an owner gets the
    //                            puppet's environment group where the game
    //                            gave one (every ped request, and the creates
    //                            recorded with one); 0: none
    //   SoundGenericEntityMode=a read at the load: an owner's entity_sound and
    //                            sound_request packets. a: created without
    //                            the update by the entity (flag 0x04), the
    //                            recorded packets drive it; b: with it, the
    //                            latest recorded variables, volume and pitch
    //                            written back every tick; c: through the
    //                            game's queue (S03) with the params whole
    //   SoundGenericSkip=        read at the load: what the generic player
    //                            does not apply, items by ';' (S16;S18;S19;
    //                            S21;S22;S23;S24;S38;stop;forget;release),
    //                            every family; a skipped item calls no port
    //                            and is counted (skipped_by_key in
    //                            SOUND_GENERIC_PLAYER_ENGINE; trace: skipped)
    //   SoundClock=1             SOUND_CLOCK lines (clip time against QPC,
    //                            recording and playing) for a game audio
    //                            capture on the same clock
    //   SoundGenericDirectParams=0  1 = the direct create (entity modes a and
    //                            b) takes the params whole from the track row
    //                            that kept them (the category at +0xD8
    //                            included), as mode c's queue does
    //   SoundGenericEntityDrivenSkip=S18,stop,forget,release  for the generic
    //                            packets whose sound updates with its entity
    //                            (flag 0x04, kept), what the player leaves to
    //                            the puppet's audio entity (SoundGenericSkip
    //                            items); empty: everything recorded applies
    //   SoundGenericMute=        owner@fromMs-toMs: those packets are not
    //                            created (ablation)
    //   SoundGenericPlayerRoute=0  1 = the recorded player's packets on the
    //                            hidden live player's audio entity, placed on
    //                            the puppet (no measured effect; bisection key)
    //   SoundCheckNotice=3       the sound check's finding on screen at the
    //                            playback's stop (Debug=0 too) when it has
    //                            at least this many missing and extra
    //                            sounds; 0: no notice
    // The capture probes (src/capture_probe.h):
    //   CaptureProbe=0           1: read probes 14 to 30 (camera, weather,
    //                            lightning, timecycle, water, vehicle fields,
    //                            map changes, pickups, explosion, glass, cloth,
    //                            lights, cutscene, audio scenes and streams,
    //                            RayFire, script screens). 30 read-only
    //                            detours plus polls by native and guarded
    //                            read; nothing is recorded or changed.
    //                            ReplayDebug\capture-probe-<date-time>.txt
    //                            every 60 s and at the unload
    //                            (tools/capture_probe_report.py reads it).
    //                            0: nothing is installed
    // Render targets (src/render_target.h):
    //   RenderTargetRecord=1     record named render targets (register, link
    //                            to a model, release) and the UI apps that
    //                            draw into them (open with entry, close):
    //                            4 detours that forward every call, a poll of
    //                            IS_UIAPP_ACTIVE_BY_HASH; clip section RTGT.
    //                            0: nothing installed
    //   RenderTargetPlayback=1   0 off; 1 re-register and relink at the
    //                            playhead; 2 also relaunch the UI app with
    //                            its entry during its span (experimental)
    //   RenderTargetUiApps=SHOP_BROWSING  the UI apps polled (names or 0x
    //                            hashes, comma separated), plus every app
    //                            the launch detour sees
    //   RenderTargetUiAppBlockInput=1  with playback 2: the frontend and game
    //                            menu actions disabled while the relaunched
    //                            app is open
    // The pose fidelity judge (src/pose_fidelity.h):
    //   PoseFidelity=0           1: a recording reads the engine pose of every
    //                            recorded entity (16 key bones and the world
    //                            matrix) and at its stop logs the tracks' error
    //                            against it (POSE_FIDELITY_SUMMARY, mm and
    //                            mrad). Reads only: nothing recorded changes
    //   PoseFidelityHz=10        its reads per second per entity (1 to 60)
    //   PoseFidelityAsync=1      the judge's evaluation at the stop runs on a
    //                            worker over copies of the tracks; the same
    //                            lines come a moment later (0: on the stop
    //                            tick, which can freeze the game for seconds
    //                            in a town)
    // The pose capture (src/pose_capture.h); the first two change what is
    // recorded when on:
    //   PoseStallPollMs=250      N (1 to 1000): the script copies a ped the
    //                            finalizer did not visit since the previous
    //                            tick only when the finalizer never visited
    //                            it or its last visit is at least N ms old (a
    //                            corpse, a ragdoll), through a gate of its
    //                            own, so the NPCs the engine animates a few
    //                            times per second keep the detour's samples;
    //                            0 (the default up to 1.0): whenever the
    //                            finalizer did not visit it, through the
    //                            detour's rate gate, which froze the poses of
    //                            the NPCs off the game camera (the buffer
    //                            still holds the previous pose on the frames
    //                            the finalizer skips)
    //   PoseLateBackoff=0        1: the end-of-frame check of a clean record
    //                            waits 1, 2, 4, then 8 s (a change: 250 ms,
    //                            then 1 s again), and a record's first check
    //                            waits a hash of its id (0 to 999 ms); an
    //                            engine write after the finalizer can be
    //                            seen up to 8 s later
    //   PoseWorkerPollMs=5       the pose worker's wait between two passes
    //                            over the capture rings (1 to 100 ms; each
    //                            pass drains every ready frame of a record,
    //                            and a buffer save drains the rings first).
    //                            Same recorded frames at any value
    std::wstring CurrentClipPathForCensus();
    struct RecordingConfig
    {
        bool buffer = true; // always recording by default
        std::uint32_t bufferSeconds = 60;
        std::uint32_t bufferMB = 128;
        std::uint32_t manualMaxSeconds = 0;
        std::uint32_t manualMaxMB = 900;
        // ReplayBufferAdaptive (0), ReplayBufferMaxMB (512), ReplayBufferReserveMB
        // (2048); buffer_coverage.h.
        bool bufferAdaptive = false;
        std::uint32_t bufferMaxMB = 512;
        std::uint32_t bufferReserveMB = 2048;
        bool replayDeadEye = false; // ReplayDeadEye=1: the recorded Dead Eye and Eagle Eye play (effect and slow motion)
        bool compressClips = true;
        std::uint32_t compressAfterDays = replay::clipcompress::kDefaultDays;
        int compressLevel = replay::clipcompress::kDefaultLevel;
        int compressFormat = 2; // CompressFormat (dev): 2 packed, 1 zstd
        bool recordCutscenes = false;
        bool lockCutsceneCamera = false;
        bool restoreWorldState = true;
        bool restoreWorldStateInMission = false;
        bool freezeWorldOnPause = true;          // FreezeWorldOnPause
        replay::recindicator::Mode recordingIndicator = replay::recindicator::Mode::Normal; // RecordingIndicator
        bool trailPlayerFollow = true;           // TrailPlayerFollow
        std::uint32_t musicStop = 1;             // MusicStop (0 off, 1 natives; 2 = plan B, not built)
        // WorldModel (off | gta), WorldReloadPath (queue | slot), WorldClearFlags
        // (CLEAR_AREA bits), WorldScriptsCleanup (FORCE_CLEANUP(512)).
        replay::worldmodel::Mode worldModel = replay::worldmodel::Mode::Off;
        replay::worldmodel::ReloadPath worldReloadPath = replay::worldmodel::ReloadPath::Queue;
        std::uint32_t worldClearFlags = replay::worldmodel::kDefaultClearFlags;
        bool worldScriptsCleanup = true;
        std::uint32_t worldModelStep = replay::worldmodel::kStepReload; // WorldModelStep (1 pool, 2 + clear, 3 + reload)
        // The closed theater: the engine's own functions, one key each.
        replay::theatre::Config theatre;
    };
    RecordingConfig g_recConfig;
    // The recording gate's state (UpdateRecording).
    replay::gate::Gate g_recordGate;
    replay::gate::Hysteresis g_spanScriptCam;
    replay::gate::Hysteresis g_spanControlsOff;
    std::uint32_t g_gateReasons = 0;      // the last tick's reasons
    std::uint32_t g_gatePauseReasons = 0; // every reason seen while the clock stands
    std::uint32_t g_gateSignals = 0;      // the last tick's raw signals
    bool g_gateLogged = false;
    std::uint32_t g_gateSignalLogs = 0;
    std::uint32_t g_gateSampleLogs = 0;
    ULONGLONG g_gateNextSampleAt = 0;
    enum class RecordKind { Manual, Buffer };
    RecordKind g_recordKind = RecordKind::Manual;
    std::uint32_t g_bufferHeadMs = 0;       // capture clock: the ring's first kept block
    bool g_manualInBuffer = false;          // F4 marked a manual start inside the buffer
    std::uint32_t g_manualStartMs = 0;
    ULONGLONG g_bufferNextBlockAt = 0;
    ULONGLONG g_bufferResumeAt = 0;         // restart after a playback or a pause
    // A clip loaded from disk waits for its playback: the buffer stopped for the
    // load would otherwise restart on the next tick (nothing is playing) and wipe
    // the loaded clip. Cleared when a playback or a recording starts.
    bool g_loadedClipHoldsBuffer = false;
    std::size_t g_bufferPeakBytes = 0;
    std::uint64_t g_bufferTrimmedPose = 0;
    std::uint64_t g_bufferTrimmedFx = 0;
    std::uint64_t g_bufferTrimmedFxTracks = 0; // emptied registered fx tracks removed by the trim
    int g_manualPart = 0;
    // A manual part being saved by the chain (not the stop that ends it): the
    // stop names the file with this part's number.
    bool g_manualSplitting = false;
    // The ring's budget this block (the configured one, or the adaptive one) and
    // its coverage verdict.
    std::size_t g_bufferBudgetBytes = 0;
    bool g_bufferBudgetMemLow = false;
    replay::buffercov::Limit g_bufferLimit = replay::buffercov::Limit::Start;
    std::uint64_t g_bufferLimitChanges = 0;
    // The clip start's world readiness log and the staggered puppet start
    // (clip_start_gate.h).
    replay::clipstart::Config g_clipStartConfig;
    replay::clipstart::Gate g_clipStart;
    std::uint32_t g_clipWorldLines = 0;
    std::uint64_t g_clipWorldLastLogMs = 0;
    std::uint32_t g_clipWorldLastBits = ~0u;
    std::uint64_t g_clipStartTogglesBefore = 0; // the IPL toggles when this clip's start began (before the stop)
    std::uint64_t g_clipStartBatchBefore = 0;
    std::vector<replay::window::PlaceSample> g_bufferPlaces; // the place over the ring
    constexpr std::uint32_t kBufferBlockMs = 2000; // the lifecycle block and pose keyframe interval
    // Playhead, pause, scrub and timeline length live in the transport.
    replay::transport::Transport g_transport;
    // Scrub: the cursor speed while the key is held.
    volatile LONG g_scrubBackHeld = 0;    // ','
    volatile LONG g_scrubForwardHeld = 0; // '.'
    bool g_freeCamEnabled = false;
    bool g_freeCamPlayerWasFrozen = false;
    ULONGLONG g_recordingStartedAt = 0;
    Ped g_freeCamPlayer = 0;
    Cam g_freeCam = 0;
    // The playback camera while the freecam is off: the camera keyframes when the
    // track has any, else the recorded game camera.
    Cam g_replayCam = 0;
    bool g_replayCamRendering = false;
    Vector3 g_freeCamPosition{};
    Vector3 g_freeCamRotation{};
    Vector3 g_freeCamVelocity{};
    float g_freeCamFov = 50.0f;
    float g_freeCamSmoothedLookX = 0.0f;
    float g_freeCamSmoothedLookY = 0.0f;
    ULONGLONG g_freeCamLastUpdateAt = 0;
    replay::camera::Track g_cameraTrack;
    bool g_cameraTrackEnabled = true;
    bool g_cameraTrackManualOverride = false;
    bool g_editorQueueOverflowed = false;

    // Per-tick sample of the player: the anchor of the entity scan and of
    // the diagnostic overlay.
    struct ProbeFrame
    {
        ULONGLONG timeMs = 0;
        Ped playerPed = 0;
        Vector3 position{};
        Vector3 cameraPosition{};
        Vector3 cameraRotation{};
        float cameraFov = 0.0f;
        Ped mount = 0;       // mount the player is fully on
        Ped mountTarget = 0; // mount being mounted, or ridden
    };

    float DegreesToRadians(float degrees)
    {
        return degrees * (kPi / 180.0f);
    }

    Vector3 RotationToDirection(const Vector3& rotation)
    {
        const float pitch = DegreesToRadians(rotation.x);
        const float yaw = DegreesToRadians(rotation.z);
        const float horizontal = std::fabs(std::cos(pitch));

        Vector3 direction{};
        direction.x = -std::sin(yaw) * horizontal;
        direction.y = std::cos(yaw) * horizontal;
        direction.z = std::sin(pitch);
        return direction;
    }

    bool IsPedFullyOnMount(Ped ped)
    {
        return PED::IS_PED_FULLY_ON_MOUNT(ped, TRUE) != FALSE;
    }

    void DrawText(const char* text, float x, float y, float scale, int r, int g, int b, int a)
    {
        replay::hud::DrawText(text, x, y, scale, r, g, b, a);
    }

    void ShowObjective(const char* message) { replay::hud::ShowObjective(message); }

    ProbeFrame CaptureFrame()
    {
        // The main loop's parts without a zone get one (otherwise over a millisecond
        // of the play tick falls outside every zone).
        REPLAY_PROFILE_ZONE("tick.capture_frame");
        ProbeFrame frame{};
        frame.timeMs = GetTickCount64();
        frame.playerPed = PLAYER::PLAYER_PED_ID();
        if (!ENTITY::DOES_ENTITY_EXIST(frame.playerPed))
            return frame;
        frame.position = ENTITY::GET_ENTITY_COORDS(frame.playerPed, TRUE, FALSE);
        frame.cameraPosition = CAM::GET_GAMEPLAY_CAM_COORD();
        frame.cameraRotation = CAM::GET_GAMEPLAY_CAM_ROT(2);
        frame.cameraFov = CAM::GET_GAMEPLAY_CAM_FOV();
        // IS_PED_ON_MOUNT turns true as the mount interaction starts;
        // IS_PED_FULLY_ON_MOUNT only once the rider settled in the saddle.
        const bool onMount = PED::IS_PED_ON_MOUNT(frame.playerPed) != FALSE;
        const Ped observedMount = onMount ? PED::GET_MOUNT(frame.playerPed) : 0;
        frame.mount = onMount && IsPedFullyOnMount(frame.playerPed) ? observedMount : 0;
        frame.mountTarget = observedMount;
        return frame;
    }

    // Log rotation: past 256 MB the log is renamed with the date when the mod loads
    // (never deleted: its telemetry is kept) and a new one starts. The bench host
    // takes its log offset at each run's start. The log is in the base folder
    // (storage_paths.h: the game folder, else Documents).
    std::wstring OpenedLogName()
    {
        constexpr std::uint64_t kRotateBytes = 256ull * 1024 * 1024;
        const std::wstring log = replay::storage::Path(replay::identity::AsciiW(replay::identity::kLogFile));
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &info) &&
            ((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) > kRotateBytes)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            char archived[64]{};
            std::snprintf(archived, sizeof(archived), "%s-%04u%02u%02u-%02u%02u%02u.log", replay::identity::kLogStem, now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
            MoveFileExW(log.c_str(), replay::storage::Path(replay::identity::AsciiW(archived)).c_str(), 0);
        }
        return log;
    }

    // While the always-on replay buffer records (no manual mark), the per-frame and
    // per-event evidence lines are dropped and counted: a buffer running for hours
    // would write ~1 MB a minute of them.
    class QuietFilter final : public std::streambuf
    {
    public:
        void Attach(std::streambuf* destination) { m_destination = destination; }
        void SetQuiet(bool quiet) { m_quiet = quiet; }
        std::uint64_t Dropped() const { return m_dropped; }

    protected:
        int_type overflow(int_type c) override
        {
            if (traits_type::eq_int_type(c, traits_type::eof()))
                return traits_type::not_eof(c);
            m_line.push_back(static_cast<char>(c));
            if (c == '\n')
                Emit();
            return c;
        }
        std::streamsize xsputn(const char* data, std::streamsize count) override
        {
            for (std::streamsize n = 0; n < count; ++n)
            {
                m_line.push_back(data[n]);
                if (data[n] == '\n')
                    Emit();
            }
            return count;
        }
        int sync() override { return m_destination ? m_destination->pubsync() : 0; }

    private:
        void Emit()
        {
            // The QA kit's health buffer sees every line, before the Debug=0 filter below
            // drops it.
            replay::qa::kit::OnLogLine(m_line);
            static const char* const kNoisy[] = {
                "POSE_V2_ROPE_FOLLOW", "POSE_V2_WEAPON_ATTACH_CHECK", "POSE_V2_FX_REGISTERED_STATE",
                "POSE_V2_ANIM_EVENT", "POSE_V2_ROPE_STATE", "POSE_V2_AUDIO_SPEECH_ALL",
                "POSE_V2_CAPTURE_STOP", "POSE_V2_SPEECH_ID", "POSE_V2_SPEECH_EVENT",
                "POSE_V2_DECAL_SAMPLE", "POSE_V2_PED_HELD_WEAPON",
                "POSE_V2_ENTITY_CREATE", "POSE_V2_ENTITY_DELETE", "POSE_V2_WEAPON_CREATE", "POSE_V2_SPEECH_STOP",
                "POSE_V2_PED_AUDIO", "POSE_V2_SOUND_EVENT", "POSE_V2_POSE_SEGMENT"};
            bool drop = false;
            // The recording gate's lines are always kept (the bench reads the pause
            // evidence from them).
            const bool always = m_line.compare(0, 12, "RECORD_PAUSE") == 0 || m_line.compare(0, 11, "RECORD_GATE") == 0;
            // Debug=0 keeps only the essential lines (replay::log::EssentialLine); the
            // heavy sites skip formatting through replay::log::Verbose().
            if (!replay::log::Verbose() && !replay::log::EssentialLine(m_line))
                drop = true;
            else if (m_quiet && !always)
            {
                for (const char* prefix : kNoisy)
                {
                    if (m_line.compare(0, std::strlen(prefix), prefix) == 0)
                    {
                        drop = true;
                        break;
                    }
                }
            }
            if (drop)
                ++m_dropped;
            else if (m_destination)
                m_destination->sputn(m_line.data(), static_cast<std::streamsize>(m_line.size()));
            m_line.clear();
        }
        std::streambuf* m_destination = nullptr;
        std::string m_line;
        bool m_quiet = false;
        std::uint64_t m_dropped = 0;
    };
    QuietFilter g_logFilter;

    // ReplayCameraCutMode (camera_cut.h).
    replay::camcut::Mode g_camCutMode = replay::camcut::Mode::Params;
    // The ini keys of the DOF, target and streaming work (CONFIG_CAMERA2).
    bool g_camDofRecreate = false;                 // ReplayCamDofRecreate=1: the DOF by recreating the camera
    replay::camera::FocusConfig g_camFocusConfig;  // ReplayCamDofBlend, ReplayCamFocusHysteresis, ReplayCamFocusSmooth
    bool g_camTargetRecorded = true;               // ReplayCamTargetRecorded
    bool g_camSmoothReframe = true;                // ReplayCamSmoothReframe
    bool g_camPrefetchOn = true;                   // ReplayCamPrefetch
    float g_camPrefetchMeters = replay::camera::kPrefetchMinMeters;   // ReplayCamPrefetchMeters
    std::uint32_t g_camPrefetchLeadMs = replay::camera::kPrefetchLeadMs; // ReplayCamPrefetchLeadMs
    // The view lent to another mod's script camera (ini ReplayCameraRelease,
    // camera_cut.h Release), and whether it is lent now.
    replay::camcut::Release g_camRelease = replay::camcut::Release::Paused;
    bool g_camLent = false;
    std::uint64_t g_camLends = 0;
    // The streaming focus follows the rendering camera every tick while the replay
    // owns the world (ini WorldFocus, 1; world_focus_core.h); CLEAR_FOCUS only when
    // the playback stops.
    bool g_worldFocus = true;
    replay::worldfocus::Tracker g_worldFocusTracker;
    bool g_replayCamPlacedThisTick = false; // UpdateReplayCamera placed the replay camera this tick
    bool g_replayCamKeyedThisTick = false;  // and it followed the camera markers
    Vector3 g_worldFocusAt{};

    std::ofstream& LogFile()
    {
        static std::ofstream file(OpenedLogName().c_str(), std::ios::out | std::ios::app);
        static const bool filtered = [] {
            g_logFilter.Attach(file.rdbuf());
            file.std::ios::rdbuf(&g_logFilter);
            return true;
        }();
        (void)filtered;
        return file;
    }

    // Crash evidence. A vectored handler writes one line per hardware fault to a
    // separate file with WriteFile (no stream shared with the script thread, whose
    // last lines may still sit in the ofstream buffer), then continues the search
    // so the game's own handlers still run. Logging only: no engine state is
    // touched. Guarded probes of the mod fault on purpose and appear here too; the
    // process dying right after a line is what tells a crash from a handled probe.
    // The handler must leave with the module: Windows keeps calling a registered
    // vectored handler for every exception in the process, so after a hot reload a
    // stale address would land in the new module's code. Removed in
    // OnModuleDetach, in-flight calls waited for.
    void* g_crashEvidenceHandle = nullptr;
    volatile LONG g_crashEvidenceInFlight = 0;
    struct CrashEvidenceInFlight
    {
        CrashEvidenceInFlight() { InterlockedIncrement(&g_crashEvidenceInFlight); }
        ~CrashEvidenceInFlight() { InterlockedDecrement(&g_crashEvidenceInFlight); }
    };

    // Module names for the handler without the loader lock: GetModuleFileNameA
    // takes it, and DllMain holds it while it removes this handler, so a fault in a
    // guarded read during a Ctrl+R would deadlock the unload. The table is filled
    // on the script thread; the handler only reads it.
    struct KnownModule
    {
        std::uintptr_t base = 0;
        std::uintptr_t size = 0;
        char name[48] = {};
    };
    KnownModule g_knownModules[256];
    volatile LONG g_knownModuleCount = 0;

    void RefreshKnownModules()
    {
        HMODULE modules[256]{};
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
            return;
        const int count = (std::min)(static_cast<int>(needed / sizeof(HMODULE)), 256);
        InterlockedExchange(&g_knownModuleCount, 0);
        int kept = 0;
        for (int n = 0; n < count; ++n)
        {
            MODULEINFO info{};
            char path[MAX_PATH]{};
            if (!K32GetModuleInformation(GetCurrentProcess(), modules[n], &info, sizeof(info)) ||
                !GetModuleFileNameA(modules[n], path, MAX_PATH))
                continue;
            const char* name = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/')
                    name = p + 1;
            KnownModule& known = g_knownModules[kept++];
            known.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
            known.size = info.SizeOfImage;
            strncpy_s(known.name, name, _TRUNCATE);
        }
        InterlockedExchange(&g_knownModuleCount, kept);
    }

    const char* KnownModuleName(std::uintptr_t address, std::uintptr_t& base)
    {
        const LONG count = InterlockedCompareExchange(&g_knownModuleCount, 0, 0);
        for (LONG n = 0; n < count; ++n)
        {
            const KnownModule& known = g_knownModules[n];
            if (address >= known.base && address < known.base + known.size)
            {
                base = known.base;
                return known.name;
            }
        }
        return "";
    }

    // ------------------------------------------------------------------
    // Crash evidence, per fatal first-chance exception (a stack captured inside the
    // handler itself stops at the bad RIP and loses the caller):
    //   EXCEPTION     the faulting address and stack line (the bench counts it),
    //                 plus the access (read/write/execute), every parameter, the
    //                 thread's name and whether a __try of this mod takes it
    //   CRASH_REGS    every general register of the faulting context
    //   CRASH_STACK   [RSP] and the 31 slots above it, module+offset when
    //                 they point into a loaded image (the return address of
    //                 a call that jumped into the heap is [RSP])
    //   CRASH_UNWIND  the faulting thread's own call stack, unwound from the
    //                 context (RtlVirtualUnwind; a frame without unwind data,
    //                 like code in the heap, is taken as a leaf: return at [RSP])
    // and, for a real crash (no __try of this mod on the faulting stack), a
    // minidump in %LOCALAPPDATA%\RedemptionEditor\crashes (the last 5 kept),
    // written by a thread made at load: the faulting thread only signals it
    // and waits (60 s at most). Nothing here allocates: static buffers under
    // a one-owner lock, snprintf into them, WriteFile. A guarded probe of the
    // mod (they fault on purpose) gets the first line only.
    // ------------------------------------------------------------------
    namespace crashdump
    {
        using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

        constexpr LONG kMaxDumpsPerLoad = 2;
        constexpr std::size_t kKeepDumps = 5;
        constexpr DWORD kDumpWaitMs = 60000;
        // dbghelp's own size choice: the stacks plus the memory their pointers reach
        // (1 KB around each), thread times and start addresses, the unloaded modules
        // (hot reloads). No data segments, no heap: tens of MB for RDR2's ~150
        // threads, not gigabytes. And the MemoryInfoList (MiniDumpWithFullMemoryInfo:
        // every region's base, allocation base, state, protection and type, no
        // contents; a few hundred KB), so a dump tells a freed block from a never
        // mapped one.
        constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules | MiniDumpIgnoreInaccessibleMemory | MiniDumpWithFullMemoryInfo);

        MiniDumpWriteDumpFn g_writeDump = nullptr;
        GetThreadDescriptionFn g_getDescription = nullptr;
        wchar_t g_folder[MAX_PATH] = {};
        HANDLE g_request = nullptr; // auto reset: the faulting thread asks
        HANDLE g_done = nullptr;    // auto reset: the dump thread answers
        HANDLE g_thread = nullptr;
        DWORD g_threadId = 0;
        std::atomic<bool> g_threadExited{false};
        volatile LONG g_stop = 0;
        volatile LONG g_busy = 0;
        volatile LONG g_written = 0;
        // The request's own copy of the exception (crash_dump_copy.h): the
        // faulting thread's records may be reused once its handler returned.
        replay::crash::OwnedException g_requestException = {};
        EXCEPTION_POINTERS* g_requestPointers = nullptr; // &g_requestException.pointers, or null
        DWORD g_requestThread = 0;
        char g_requestEvidence[12288] = {};
        int g_requestLength = 0;
    }

    // One owner at a time formats into the static buffers; a fault inside
    // the handler (a guarded read below) finds itself the owner and leaves.
    volatile LONG g_crashFormatOwner = 0;
    char g_crashText[12288] = {};
    CONTEXT g_crashUnwind = {};

    bool AcquireCrashFormat()
    {
        const LONG self = static_cast<LONG>(GetCurrentThreadId());
        for (int spin = 0; spin < 3000; ++spin)
        {
            const LONG owner = InterlockedCompareExchange(&g_crashFormatOwner, self, 0);
            if (owner == 0)
                return true;
            if (owner == self)
                return false;
            Sleep(spin < 100 ? 0 : 1);
        }
        return false;
    }

    void ReleaseCrashFormat() { InterlockedExchange(&g_crashFormatOwner, 0); }

    // The image's name from its export directory (an image loaded after the
    // last RefreshKnownModules): no loader lock, guarded reads.
    const char* ExportDirectoryName(std::uintptr_t base)
    {
        static std::uintptr_t bases[16] = {};
        static char names[16][40] = {};
        static volatile LONG next = 0;
        for (int n = 0; n < 16; ++n)
            if (bases[n] == base)
                return names[n];
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!replay::mem::Copy(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            !replay::mem::Copy(&nt, base + static_cast<std::uintptr_t>(dos.e_lfanew), sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
            return "";
        const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            return "";
        IMAGE_EXPORT_DIRECTORY exports{};
        if (!replay::mem::Copy(&exports, base + dir.VirtualAddress, sizeof(exports)) || !exports.Name)
            return "";
        const int slot = static_cast<int>(InterlockedIncrement(&next) & 15);
        char* name = names[slot];
        name[0] = 0;
        if (!replay::mem::Copy(name, base + exports.Name, sizeof(names[slot]) - 1))
            return "";
        name[sizeof(names[slot]) - 1] = 0;
        bases[slot] = base;
        return name;
    }

    // "RDR2.exe+0x1A2B" for an address inside a loaded image, "" otherwise.
    const char* ImageOf(std::uintptr_t address, std::uintptr_t& base)
    {
        base = 0;
        if (address < replay::mem::kLowest || address >= replay::mem::kUserEnd)
            return "";
        const char* name = KnownModuleName(address, base);
        if (*name)
            return name;
        PVOID header = nullptr;
        if (!RtlPcToFileHeader(reinterpret_cast<PVOID>(address), &header) || !header)
            return "";
        base = reinterpret_cast<std::uintptr_t>(header);
        name = ExportDirectoryName(base);
        return *name ? name : "image";
    }

    int FormatAddress(char* out, std::size_t size, std::uintptr_t address)
    {
        std::uintptr_t base = 0;
        const char* name = ImageOf(address, base);
        if (*name)
            return std::snprintf(out, size, "%.40s+0x%llX", name, static_cast<unsigned long long>(address - base));
        return std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
    }

    // Appends to g_crashText (never past its end).
    struct CrashText
    {
        int used = 0;
        void Add(const char* format, ...)
        {
            if (used >= static_cast<int>(sizeof(g_crashText)) - 1)
                return;
            va_list args;
            va_start(args, format);
            const int wrote = std::vsnprintf(g_crashText + used, sizeof(g_crashText) - static_cast<std::size_t>(used), format, args);
            va_end(args);
            if (wrote > 0)
                used = (std::min)(used + wrote, static_cast<int>(sizeof(g_crashText)) - 1);
        }
        void Address(std::uintptr_t address)
        {
            if (used >= static_cast<int>(sizeof(g_crashText)) - 1)
                return;
            const int wrote = FormatAddress(g_crashText + used, sizeof(g_crashText) - static_cast<std::size_t>(used), address);
            if (wrote > 0)
                used = (std::min)(used + wrote, static_cast<int>(sizeof(g_crashText)) - 1);
        }
    };

    // One unwind step of g_crashUnwind (the faulting thread's context, on its
    // own stack). `guarded`: this frame is in this module and inside one of
    // its __except scopes. False when the frame cannot be unwound. The
    // memory reads are guarded (a fault re-enters the handler, which leaves
    // at once: the format lock is this thread's).
    bool UnwindCrashFrame(std::uintptr_t stackLow, std::uintptr_t stackHigh, bool& guarded)
    {
        guarded = false;
        __try
        {
            CONTEXT& context = g_crashUnwind;
            const DWORD64 pc = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            if (!function)
            {
                // A leaf, or code without unwind data (the heap): return at [rsp].
                if (context.Rsp < stackLow || context.Rsp + 8 > stackHigh)
                    return false;
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
                return context.Rip != 0;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            const PEXCEPTION_ROUTINE handler =
                RtlVirtualUnwind(UNW_FLAG_EHANDLER, imageBase, pc, function, &context, &handlerData, &establisher, nullptr);
            const std::uintptr_t self = reinterpret_cast<std::uintptr_t>(&__ImageBase);
            if (handler && handlerData && imageBase == self)
            {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(self + __ImageBase.e_lfanew);
                const std::uint32_t count = *static_cast<const std::uint32_t*>(handlerData);
                const auto* records = reinterpret_cast<const replay::crash::ScopeRecord*>(static_cast<const std::uint8_t*>(handlerData) + 4);
                guarded = replay::crash::ScopeCoversPc(count, records, nt->OptionalHeader.SizeOfImage, static_cast<std::uint32_t>(pc - imageBase));
            }
            return context.Rsp >= stackLow && context.Rsp < stackHigh && context.Rip != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadStackSlot(std::uintptr_t address, std::uint64_t& value)
    {
        return replay::mem::Copy(&value, address, sizeof(value));
    }

    // The crash log's path in the base folder, set at load (the handler builds no string).
    wchar_t g_crashLogPath[MAX_PATH * 2]{};

    void AppendCrashLog(const char* text, int length)
    {
        if (length <= 0)
            return;
        HANDLE file = g_crashLogPath[0]
            ? CreateFileW(g_crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
            : CreateFileA(replay::identity::kCrashLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        DWORD wrote = 0;
        WriteFile(file, text, static_cast<DWORD>(length), &wrote, nullptr);
        CloseHandle(file);
    }

    // The dump thread (made at load, so the crash needs no thread creation).
    // It runs MiniDumpWriteDump for the thread that faulted while that one
    // waits on g_done: the dump then has the exception record and the
    // faulting thread's real context, and the work runs on a healthy stack
    // (a stack overflow has none left).
    void WriteRequestedDump()
    {
        using namespace crashdump;
        const ULONGLONG started = GetTickCount64();
        SYSTEMTIME now{};
        GetLocalTime(&now);
        // Keep the newest kKeepDumps - 1 of the old ones.
        {
            wchar_t pattern[MAX_PATH + 8]{};
            swprintf_s(pattern, L"%ls*.dmp", g_folder);
            WIN32_FIND_DATAW found{};
            wchar_t names[64][MAX_PATH]{};
            std::uint64_t stamps[64]{};
            std::size_t count = 0;
            HANDLE find = FindFirstFileW(pattern, &found);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (count < 64 && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    {
                        wcsncpy_s(names[count], found.cFileName, _TRUNCATE);
                        stamps[count] = (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
                        ++count;
                    }
                } while (FindNextFileW(find, &found));
                FindClose(find);
            }
            std::size_t remove[64]{};
            const std::size_t removing = replay::crash::DumpsToDelete(stamps, count, kKeepDumps, remove, 64);
            for (std::size_t n = 0; n < removing; ++n)
            {
                wchar_t path[MAX_PATH * 2]{};
                swprintf_s(path, L"%ls%ls", g_folder, names[remove[n]]);
                DeleteFileW(path);
                // its evidence text too
                wchar_t text[MAX_PATH * 2 + 8]{};
                swprintf_s(text, L"%ls.txt", path);
                DeleteFileW(text);
            }
        }
        wchar_t name[160]{};
        replay::crash::DumpFileName(name, 160, replay::build::kVersion, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            GetCurrentProcessId(), g_requestThread);
        wchar_t path[MAX_PATH * 2]{};
        swprintf_s(path, L"%ls%ls", g_folder, name);
        // The faulting thread's description (the editor core's threads and
        // the game's named threads have one; ours are in the table too).
        char description[96] = "none";
        if (g_getDescription)
        {
            if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_requestThread))
            {
                PWSTR text = nullptr;
                if (SUCCEEDED(g_getDescription(thread, &text)) && text)
                {
                    if (text[0])
                        WideCharToMultiByte(CP_UTF8, 0, text, -1, description, sizeof(description) - 1, nullptr, nullptr);
                    LocalFree(text);
                }
                CloseHandle(thread);
            }
        }
        DWORD error = 0;
        bool ok = false;
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            error = GetLastError();
        else
        {
            MINIDUMP_EXCEPTION_INFORMATION exception{};
            exception.ThreadId = g_requestThread;
            exception.ExceptionPointers = g_requestPointers;
            exception.ClientPointers = FALSE;
            ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, g_requestPointers ? &exception : nullptr, nullptr,
                     nullptr) != FALSE;
            if (!ok)
                error = GetLastError();
            CloseHandle(file);
            if (!ok)
                DeleteFileW(path);
        }
        LARGE_INTEGER size{};
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (ok && GetFileAttributesExW(path, GetFileExInfoStandard, &data))
            {
                size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                size.LowPart = data.nFileSizeLow;
            }
        }
        char pathUtf8[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, pathUtf8, sizeof(pathUtf8) - 1, nullptr, nullptr);
        char line[1024]{};
        const int length = std::snprintf(line, sizeof(line),
            "%04u-%02u-%02u %02u:%02u:%02u.%03u CRASH_DUMP,ok=%d,thread=%lu,thread_description=%s,path=%s,bytes=%lld,ms=%llu,error=%lu\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, ok ? 1 : 0,
            static_cast<unsigned long>(g_requestThread), description, ok ? pathUtf8 : "none", static_cast<long long>(size.QuadPart),
            static_cast<unsigned long long>(GetTickCount64() - started), static_cast<unsigned long>(error));
        AppendCrashLog(line, length);
        // The evidence lines and the thread names next to the dump.
        if (ok)
        {
            wchar_t textPath[MAX_PATH * 2 + 8]{};
            swprintf_s(textPath, L"%ls.txt", path);
            HANDLE text = CreateFileW(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (text != INVALID_HANDLE_VALUE)
            {
                DWORD wrote = 0;
                WriteFile(text, g_requestEvidence, static_cast<DWORD>(g_requestLength), &wrote, nullptr);
                WriteFile(text, line, static_cast<DWORD>(length > 0 ? length : 0), &wrote, nullptr);
                char names[2048]{};
                const int namesLength = std::snprintf(names, sizeof(names), "THREAD_NAMES,module=RedemptionEditor.asi,%s\n",
                    replay::threads::MapText().c_str());
                WriteFile(text, names, static_cast<DWORD>(namesLength > 0 ? namesLength : 0), &wrote, nullptr);
                CloseHandle(text);
            }
        }
    }

    DWORD WINAPI CrashDumpThreadMain(void*)
    {
        using namespace crashdump;
        replay::threads::Name("crash_dump");
        for (;;)
        {
            WaitForSingleObject(g_request, INFINITE);
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
                break;
            WriteRequestedDump();
            SetEvent(g_done);
        }
        replay::threads::Forget();
        g_threadExited.store(true, std::memory_order_release); // last statement (module_lifetime.h)
        return 0;
    }

    // From the faulting thread, under the format lock: takes the one
    // request slot (false: no dump thread, the load's dumps are used up, or
    // another thread's dump runs; this thread's stack is in that one).
    bool ClaimCrashDump(EXCEPTION_POINTERS* pointers, const char* evidence, int length)
    {
        using namespace crashdump;
        if (!g_thread || !g_writeDump || GetCurrentThreadId() == g_threadId)
            return false;
        if (InterlockedCompareExchange(&g_written, 0, 0) >= kMaxDumpsPerLoad)
            return false;
        if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
            return false;
        InterlockedIncrement(&g_written);
        // Copied before the dump thread is signalled; after a timeout the
        // handler returns and only these copies are read (g_busy keeps them).
        g_requestPointers = replay::crash::CopyException(pointers, g_requestException) ? &g_requestException.pointers : nullptr;
        g_requestThread = GetCurrentThreadId();
        g_requestLength = (std::min)(length, static_cast<int>(sizeof(g_requestEvidence)));
        if (g_requestLength > 0)
            std::memcpy(g_requestEvidence, evidence, static_cast<std::size_t>(g_requestLength));
        return true;
    }

    // Then, with the lock released: the dump thread writes, this one waits.
    void WaitCrashDump()
    {
        using namespace crashdump;
        SetEvent(g_request);
        if (WaitForSingleObject(g_done, kDumpWaitMs) == WAIT_OBJECT_0)
            InterlockedExchange(&g_busy, 0);
        // On a timeout g_busy stays set: the dump thread may still read the
        // request, nothing else may overwrite it.
    }

    LONG WINAPI CrashEvidenceHandler(EXCEPTION_POINTERS* pointers)
    {
        CrashEvidenceInFlight inFlight;
        if (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        static volatile LONG guardedLines = 0;
        static volatile LONG crashLines = 0;
        if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
            return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const DWORD code = record.ExceptionCode;
        const bool fatalClass = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_PRIV_INSTRUCTION ||
            code == EXCEPTION_IN_PAGE_ERROR || code == 0xC0000409 /* fail fast */;
        if (!fatalClass || (crashLines >= 16 && guardedLines >= 32))
            return EXCEPTION_CONTINUE_SEARCH;
        if (!AcquireCrashFormat())
            return EXCEPTION_CONTINUE_SEARCH; // a fault inside this handler, or another thread stuck in it
        const CONTEXT& context = *pointers->ContextRecord;
        const DWORD threadId = GetCurrentThreadId();
        const char* threadName = replay::threads::Lookup(threadId);
        // The faulting thread's stack (the handler runs on it; a fiber's TIB
        // holds the fiber's stack).
        const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const std::uintptr_t stackLow = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        const std::uintptr_t stackHigh = reinterpret_cast<std::uintptr_t>(tib->StackBase);

        // Unwind first (program counters only): it decides between a guarded
        // probe and a crash before anything is formatted.
        static std::uintptr_t pcs[40];
        int pcCount = 0;
        bool guarded = false;
        int guardFrame = -1;
        {
            std::memcpy(&g_crashUnwind, &context, sizeof(CONTEXT));
            for (int n = 0; n < 40; ++n)
            {
                pcs[pcCount++] = static_cast<std::uintptr_t>(g_crashUnwind.Rip);
                bool here = false;
                const bool more = UnwindCrashFrame(stackLow, stackHigh, here);
                if (here)
                {
                    guarded = true;
                    guardFrame = n;
                    break; // a __try of this mod takes it: the rest is not needed
                }
                if (!more)
                    break;
            }
        }
        if (guarded ? InterlockedIncrement(&guardedLines) > 32 : InterlockedIncrement(&crashLines) > 16)
        {
            ReleaseCrashFormat();
            return EXCEPTION_CONTINUE_SEARCH;
        }
        char frames[1600]{};
        if (!guarded)
        {
            int used = 0;
            for (int n = 0; n < pcCount && used < static_cast<int>(sizeof(frames)) - 64; ++n)
            {
                if (n)
                    frames[used++] = '|';
                const int wrote = FormatAddress(frames + used, sizeof(frames) - static_cast<std::size_t>(used), pcs[n]);
                if (wrote > 0)
                    used = (std::min)(used + wrote, static_cast<int>(sizeof(frames)) - 1);
            }
        }

        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        std::uintptr_t moduleBase = 0;
        const char* name = KnownModuleName(address, moduleBase);
        if (!*name)
            name = ImageOf(address, moduleBase);
        std::uintptr_t fault = 0;
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2)
            fault = static_cast<std::uintptr_t>(record.ExceptionInformation[1]);
        const char* access = replay::crash::AccessName(code, record.NumberParameters,
            record.NumberParameters ? static_cast<std::uint64_t>(record.ExceptionInformation[0]) : 0);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        // The handler's own stack (through the dispatcher).
        char handlerStack[640]{};
        {
            void* stack[24]{};
            const USHORT captured = RtlCaptureStackBackTrace(0, 24, stack, nullptr);
            int used = 0;
            for (USHORT n = 0; n < captured && used < static_cast<int>(sizeof(handlerStack)) - 48; ++n)
            {
                std::uintptr_t frameBase = 0;
                const char* frameName = KnownModuleName(reinterpret_cast<std::uintptr_t>(stack[n]), frameBase);
                const int wrote = std::snprintf(handlerStack + used, sizeof(handlerStack) - used, "%s%.12s+0x%llX", n ? "|" : "",
                    *frameName ? frameName : "?", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(stack[n]) - frameBase));
                if (wrote <= 0)
                    break;
                used += wrote;
            }
        }
        CrashText text;
        const unsigned long thread = static_cast<unsigned long>(threadId);
        text.Add("%04u-%02u-%02u %02u:%02u:%02u.%03u EXCEPTION,code=0x%08lX,address=0x%llX,module=%s,offset=0x%llX,fault_address=0x%llX,"
                 "access=%s,flags=0x%lX,params=%lu",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, static_cast<unsigned long>(code),
            static_cast<unsigned long long>(address), *name ? name : "unknown", static_cast<unsigned long long>(moduleBase ? address - moduleBase : 0),
            static_cast<unsigned long long>(fault), access, static_cast<unsigned long>(record.ExceptionFlags),
            static_cast<unsigned long>(record.NumberParameters));
        for (DWORD n = 0; n < record.NumberParameters && n < EXCEPTION_MAXIMUM_PARAMETERS; ++n)
            text.Add(",info%lu=0x%llX", static_cast<unsigned long>(n), static_cast<unsigned long long>(record.ExceptionInformation[n]));
        text.Add(",thread=%lu,thread_name=%s,guarded=%d,guard_frame=%d,build=%s,stack=%s\n", thread, *threadName ? threadName : "unknown",
            guarded ? 1 : 0, guardFrame, replay::build::kVersion, handlerStack);
        if (!guarded)
        {
            text.Add("CRASH_REGS,thread=%lu,rip=", thread);
            text.Address(static_cast<std::uintptr_t>(context.Rip));
            text.Add(",rsp=0x%llX,rbp=0x%llX,rax=0x%llX,rbx=0x%llX,rcx=0x%llX,rdx=0x%llX,rsi=0x%llX,rdi=0x%llX,r8=0x%llX,r9=0x%llX,"
                     "r10=0x%llX,r11=0x%llX,r12=0x%llX,r13=0x%llX,r14=0x%llX,r15=0x%llX,eflags=0x%lX,stack_low=0x%llX,stack_high=0x%llX\n",
                context.Rsp, context.Rbp, context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi, context.Rdi, context.R8, context.R9,
                context.R10, context.R11, context.R12, context.R13, context.R14, context.R15, static_cast<unsigned long>(context.EFlags),
                static_cast<unsigned long long>(stackLow), static_cast<unsigned long long>(stackHigh));
            // [RSP] first: after a call into a bad pointer it is the return
            // address of that call, i.e. the caller.
            text.Add("CRASH_STACK,thread=%lu,rsp=0x%llX", thread, context.Rsp);
            for (int slot = 0; slot < 32; ++slot)
            {
                const std::uintptr_t at = static_cast<std::uintptr_t>(context.Rsp) + static_cast<std::uintptr_t>(slot) * 8;
                std::uint64_t value = 0;
                text.Add(",+%02X=", slot * 8);
                if (at < stackLow || at + 8 > stackHigh || !ReadStackSlot(at, value))
                {
                    text.Add("unreadable");
                    continue;
                }
                text.Address(static_cast<std::uintptr_t>(value));
            }
            text.Add("\nCRASH_UNWIND,thread=%lu,frames=%s\n", thread, frames);
            // The page of the fault address as the system has it now (VirtualQuery: no
            // allocation, safe here). free = never mapped or released; reserve = an
            // allocation whose pages were decommitted; commit with PAGE_NOACCESS or a
            // guard = protected.
            if (fault)
            {
                MEMORY_BASIC_INFORMATION page{};
                const SIZE_T got = VirtualQuery(reinterpret_cast<const void*>(fault), &page, sizeof(page));
                const char* stateName = !got ? "unknown" : page.State == MEM_COMMIT ? "commit"
                    : page.State == MEM_RESERVE ? "reserve" : page.State == MEM_FREE ? "free" : "other";
                const char* typeName = !got || page.State == MEM_FREE ? "none" : page.Type == MEM_IMAGE ? "image"
                    : page.Type == MEM_MAPPED ? "mapped" : page.Type == MEM_PRIVATE ? "private" : "other";
                text.Add("CRASH_FAULT_PAGE,thread=%lu,fault_address=0x%llX,query=%d,base=0x%llX,allocation_base=0x%llX,"
                         "region_size=0x%llX,state=%s,state_raw=0x%lX,protect=0x%lX,allocation_protect=0x%lX,type=%s,type_raw=0x%lX\n",
                    thread, static_cast<unsigned long long>(fault), got ? 1 : 0,
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(page.BaseAddress)),
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(page.AllocationBase)),
                    static_cast<unsigned long long>(page.RegionSize), stateName, static_cast<unsigned long>(page.State),
                    static_cast<unsigned long>(page.Protect), static_cast<unsigned long>(page.AllocationProtect), typeName,
                    static_cast<unsigned long>(page.Type));
            }
            // The last DRAWSTORE_SLOT line (a copy kept by the script thread; no read of
            // the engine here).
            {
                char drawStore[1024];
                if (replay::drawstore::CopyLastLine(drawStore, static_cast<int>(sizeof(drawStore))) > 0)
                    text.Add("CRASH_DRAWSTORE,last=%s\n", drawStore);
            }
        }
        AppendCrashLog(g_crashText, text.used);
        // A real crash: the request (and its copy of these lines) is taken
        // under the format lock, the wait for the dump outside it.
        const bool requested = !guarded && ClaimCrashDump(pointers, g_crashText, text.used);
        ReleaseCrashFormat();
        if (requested)
            WaitCrashDump();
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The dump folder, dbghelp and the dump thread, once per load.
    void StartCrashDumps()
    {
        using namespace crashdump;
        if (g_thread)
            return;
        wchar_t local[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 40)
            return;
        swprintf_s(g_folder, L"%ls\\RedemptionEditor", local);
        CreateDirectoryW(g_folder, nullptr);
        swprintf_s(g_folder, L"%ls\\RedemptionEditor\\crashes\\", local);
        CreateDirectoryW(g_folder, nullptr);
        if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        g_getDescription = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
        if (!g_writeDump)
            return;
        g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_request || !g_done)
            return;
        InterlockedExchange(&g_stop, 0);
        g_threadExited.store(false, std::memory_order_release);
        g_thread = CreateThread(nullptr, 0, &CrashDumpThreadMain, nullptr, 0, &g_threadId);
    }

    void StopCrashDumps()
    {
        using namespace crashdump;
        if (g_thread)
        {
            InterlockedExchange(&g_stop, 1);
            SetEvent(g_request);
            if (!replay::lifetime::StopWait(g_thread, g_threadExited, 2000, "crash_dump"))
                replay::lifetime::NoteUnloadTimeout("crash_dump");
            CloseHandle(g_thread);
            g_thread = nullptr;
            g_threadId = 0;
        }
        if (g_request)
            CloseHandle(g_request);
        if (g_done)
            CloseHandle(g_done);
        g_request = nullptr;
        g_done = nullptr;
    }

    void InstallCrashEvidence()
    {
        if (!g_crashLogPath[0])
            wcsncpy_s(g_crashLogPath, replay::storage::Path(replay::identity::AsciiW(replay::identity::kCrashLogFile)).c_str(), _TRUNCATE);
        RefreshKnownModules();
        replay::threads::NoteForeign("script"); // the game's main thread (the script fiber runs on it)
        StartCrashDumps();
        if (!g_crashEvidenceHandle)
            g_crashEvidenceHandle = AddVectoredExceptionHandler(0, &CrashEvidenceHandler); // last in line: the game's handlers see the fault first
    }

    void RemoveCrashEvidence()
    {
        if (!g_crashEvidenceHandle)
            return;
        RemoveVectoredExceptionHandler(g_crashEvidenceHandle);
        g_crashEvidenceHandle = nullptr;
        for (int wait = 0; wait < 200 && InterlockedCompareExchange(&g_crashEvidenceInFlight, 0, 0) > 0; ++wait)
            Sleep(1);
        StopCrashDumps();
    }

    // The CRASH_DUMPS line of the log header: where the dumps go.
    std::string CrashDumpFolderUtf8()
    {
        char text[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, crashdump::g_folder, -1, text, sizeof(text) - 1, nullptr, nullptr);
        return text[0] ? text : "none";
    }

    // The ASI file this module was loaded from and its SHA-256 (CryptoAPI of
    // advapi32, read once at load), so a log and a report name the exact build.
    std::string g_asiSha256;
    std::wstring g_asiPath;
    std::string g_iniKeysText; // the [RedemptionEditor] section as read at load (report.txt)
    std::string Narrow(const std::wstring& text); // below, with the clip paths

    std::string FileSha256(const std::wstring& path, std::uint64_t& bytes)
    {
        bytes = 0;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return "unreadable";
        std::string text = "unavailable";
        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
            CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
        {
            std::vector<BYTE> buffer(1 << 16);
            DWORD read = 0;
            bool ok = true;
            while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
            {
                ok = CryptHashData(hash, buffer.data(), read, 0) != FALSE;
                bytes += read;
            }
            BYTE digest[32]{};
            DWORD digestSize = sizeof(digest);
            if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) && digestSize == sizeof(digest))
            {
                char hex[65]{};
                for (DWORD n = 0; n < digestSize; ++n)
                    std::snprintf(hex + n * 2, 3, "%02x", digest[n]);
                text = hex;
            }
        }
        if (hash)
            CryptDestroyHash(hash);
        if (provider)
            CryptReleaseContext(provider, 0);
        CloseHandle(file);
        return text;
    }

    // The [RedemptionEditor] section as read ("key=value;..."), so a report shows
    // the settings the session ran with.
    std::string IniSectionText(const std::string& configPath, bool& found)
    {
        found = GetFileAttributesA(configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        std::vector<char> section(8192);
        const DWORD length = replay::identity::IniSection(section.data(), static_cast<DWORD>(section.size()), configPath.c_str());
        std::string text;
        for (DWORD at = 0; at < length && section[at];)
        {
            const std::string pair(section.data() + at);
            text += (text.empty() ? "" : ";") + pair;
            at += static_cast<DWORD>(pair.size()) + 1;
        }
        return text;
    }

    // Extra idle time before the normal exit pushes the save reload (ini
    // TheatreReloadDelayMs, 0 = none): lets the render side finish destroying the
    // puppets before the load tears the world down.
    ULONGLONG g_theatreReloadDelayMs = 0;

    // The free camera's options, FreeCam* keys of [RedemptionEditor] (the editor's
    // Free Camera rows write them back) and SetFreeCamOption's fields.
    float g_fcNearClip = 0.1f; // FreeCamNearClip, the flying free camera's near clip
    struct FreeCamFieldInfo
    {
        const char* iniKey;
        float minimum;
        float maximum;
    };
    constexpr FreeCamFieldInfo kFreeCamFields[static_cast<std::size_t>(replay::editor::FreeCamField::Count)] = {
        {"FreeCamMotion", 0.0f, 1.0f},          // 0 Classic, 1 Drone
        {"", 0.0f, 1.0f},                       // Acrobatic: not in the ini (off at start; the N key and the menu, this session only)
        {"FreeCamWalk", 0.0f, 1.0f},
        {"FreeCamWalkHeight", 0.3f, 5.0f},
        {"FreeCamLockAltitude", 0.0f, 1.0f},
        {"FreeCamCollision", 0.0f, 1.0f},
        {"", 0.0f, 2.0f},                       // Follow: not saved (a target is per clip)
        {"FreeCamDroneDrag", 0.2f, 12.0f},
        {"FreeCamDroneAcceleration", 2.0f, 60.0f},
        {"FreeCamDroneGravity", 0.0f, 20.0f},
        {"FreeCamDroneBanking", 0.0f, 45.0f},
        {"FreeCamDroneRotSmoothing", 1.0f, 30.0f},
        {"FreeCamDroneFovSmoothing", 0.0f, 20.0f},
        {"FreeCamLookSensitivity", 2.0f, 30.0f},
        {"", 0.0f, 1.0f},                       // ResetDefaults
        {"FreeCamNearClip", 0.01f, 1.0f},       // metres
    };

    float FreeCamValue(replay::editor::FreeCamField field)
    {
        using F = replay::editor::FreeCamField;
        const replay::freecam::Settings& s = g_fcSettings;
        switch (field)
        {
        case F::Motion: return s.motion == replay::freecam::Motion::Drone ? 1.0f : 0.0f;
        case F::Acrobatic: return s.acrobatic ? 1.0f : 0.0f;
        case F::Walk: return s.walk ? 1.0f : 0.0f;
        case F::WalkHeight: return s.walkHeight;
        case F::LockAltitude: return s.lockAltitude ? 1.0f : 0.0f;
        case F::Collision: return s.collision ? 1.0f : 0.0f;
        case F::Follow: return static_cast<float>(g_fcFollow);
        case F::DroneDrag: return s.droneDrag;
        case F::DroneAcceleration: return s.droneAcceleration;
        case F::DroneGravity: return s.droneGravity;
        case F::DroneBanking: return s.droneBanking;
        case F::DroneRotSmoothing: return s.droneRotSmoothing;
        case F::DroneFovSmoothing: return s.droneFovSmoothing;
        case F::LookSensitivity: return s.lookSensitivity;
        case F::NearClip: return g_fcNearClip;
        default: return 0.0f;
        }
    }

    // The value held in the field's range; false when the field is unknown.
    bool SetFreeCamValue(replay::editor::FreeCamField field, float value)
    {
        using F = replay::editor::FreeCamField;
        const std::size_t index = static_cast<std::size_t>(field);
        if (index >= static_cast<std::size_t>(F::Count) || !std::isfinite(value))
            return false;
        if (field == F::ResetDefaults)
        {
            const float speedScale = g_fcSettings.speedScale;
            g_fcSettings = replay::freecam::Settings{};
            g_fcSettings.speedScale = speedScale;
            g_fcNearClip = replay::editor::kNearClipDefault;
            return true;
        }
        value = std::clamp(value, kFreeCamFields[index].minimum, kFreeCamFields[index].maximum);
        replay::freecam::Settings& s = g_fcSettings;
        const bool on = value >= 0.5f;
        switch (field)
        {
        case F::Motion: s.motion = on ? replay::freecam::Motion::Drone : replay::freecam::Motion::Classic; break;
        case F::Acrobatic: s.acrobatic = on; break;
        case F::Walk: s.walk = on; break;
        case F::WalkHeight: s.walkHeight = value; break;
        case F::LockAltitude: s.lockAltitude = on; break;
        case F::Collision: s.collision = on; break;
        case F::Follow:
            g_fcFollow = static_cast<replay::freecam::Follow>(static_cast<int>(value + 0.5f));
            g_fcTargetFrameValid = false;
            break;
        case F::DroneDrag: s.droneDrag = value; break;
        case F::DroneAcceleration: s.droneAcceleration = value; break;
        case F::DroneGravity: s.droneGravity = value; break;
        case F::DroneBanking: s.droneBanking = value; break;
        case F::DroneRotSmoothing: s.droneRotSmoothing = value; break;
        case F::DroneFovSmoothing: s.droneFovSmoothing = value; break;
        case F::LookSensitivity: s.lookSensitivity = value; break;
        case F::NearClip: g_fcNearClip = value; break;
        default: return false;
        }
        return true;
    }

    void LoadFreeCamOptions(const char* configPath)
    {
        for (std::size_t index = 0; index < static_cast<std::size_t>(replay::editor::FreeCamField::Count); ++index)
        {
            const FreeCamFieldInfo& info = kFreeCamFields[index];
            if (!info.iniKey[0])
                continue;
            char text[32]{};
            replay::identity::IniString(info.iniKey, "", text, sizeof(text), configPath);
            if (!text[0])
                continue;
            float value = 0.0f;
            if (_stricmp(text, "drone") == 0)
                value = 1.0f;
            else if (_stricmp(text, "classic") == 0)
                value = 0.0f;
            else
            {
                char* end = nullptr;
                value = std::strtof(text, &end);
                if (end == text)
                    continue;
            }
            SetFreeCamValue(static_cast<replay::editor::FreeCamField>(index), value);
        }
    }

    void SaveFreeCamOptions()
    {
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string path = replay::identity::IniPathIn(workingDirectory);
        for (std::size_t index = 0; index < static_cast<std::size_t>(replay::editor::FreeCamField::Count); ++index)
        {
            const FreeCamFieldInfo& info = kFreeCamFields[index];
            if (!info.iniKey[0])
                continue;
            const auto field = static_cast<replay::editor::FreeCamField>(index);
            char text[32]{};
            if (field == replay::editor::FreeCamField::Motion)
                std::snprintf(text, sizeof(text), "%s", g_fcSettings.motion == replay::freecam::Motion::Drone ? "drone" : "classic");
            else
                std::snprintf(text, sizeof(text), "%g", FreeCamValue(field));
            replay::identity::IniWrite(info.iniKey, text, path.c_str());
        }
    }

    void LoadSplineDefaults(const char* configPath)
    {
        namespace sp = replay::camera::spline;
        replay::editor::SplineTuning d = replay::editor::DefaultSplineTuning();
        for (std::size_t index = 0; index < sp::kIniKeyCount; ++index)
        {
            char text[32]{};
            replay::identity::IniString(sp::kIniKeys[index], "", text, sizeof(text), configPath);
            sp::IniRead(d, index, text);
        }
        g_splineDefaults = d;
    }

    void SaveSplineDefaults()
    {
        namespace sp = replay::camera::spline;
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string path = replay::identity::IniPathIn(workingDirectory);
        for (std::size_t index = 0; index < sp::kIniKeyCount; ++index)
            replay::identity::IniWrite(sp::kIniKeys[index], sp::IniValue(g_splineDefaults, index).c_str(), path.c_str());
    }

    void LogSplineDefaults(const char* why)
    {
        const replay::editor::SplineTuning& d = g_splineDefaults;
        LogFile() << "CAMERA_SPLINE,defaults,why=" << why << ",pacing="
                  << replay::camera::spline::PacingName(static_cast<replay::editor::SplinePacing>(d.pacing)) << ",alpha=" << d.alpha
                  << ",ease_in=" << d.easeIn << ",ease_out=" << d.easeOut << ",weight=" << d.weight
                  << ",channels=" << static_cast<int>(d.channels) << '\n';
    }

    void SaveOverlayOptions()
    {
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string path = replay::identity::IniPathIn(workingDirectory);
        replay::identity::IniWrite("CameraOverlay", g_camOverlayEnabled ? "1" : "0", path.c_str());
        replay::identity::IniWrite("CameraOverlayWhilePlaying", g_camOverlayWhilePlaying ? "1" : "0", path.c_str());
        replay::identity::IniWrite("CameraOverlayTimeLabels", g_camOverlayTimeLabels ? "1" : "0", path.c_str());
        replay::identity::IniWrite("CameraOverlayTrail", g_camOverlayTrail ? "1" : "0", path.c_str());
    }

    void LogFreeCamConfig(const char* why)
    {
        const replay::freecam::Settings& s = g_fcSettings;
        LogFile() << "FREECAM_CONFIG,why=" << why << ",motion=" << (s.motion == replay::freecam::Motion::Drone ? "drone" : "classic")
                  << ",acrobatic=" << (s.acrobatic ? 1 : 0) << ",walk=" << (s.walk ? 1 : 0) << ",walk_m=" << s.walkHeight
                  << ",lock_altitude=" << (s.lockAltitude ? 1 : 0) << ",collision=" << (s.collision ? 1 : 0)
                  << ",follow=" << static_cast<int>(g_fcFollow) << ",target=" << g_fcTarget << ",drag=" << s.droneDrag
                  << ",accel=" << s.droneAcceleration << ",gravity=" << s.droneGravity << ",banking=" << s.droneBanking
                  << ",rot_smooth=" << s.droneRotSmoothing << ",fov_smooth=" << s.droneFovSmoothing
                  << ",sensitivity=" << s.lookSensitivity << ",overlay=" << (g_camOverlayEnabled ? 1 : 0)
                  << ",overlay_playing=" << (g_camOverlayWhilePlaying ? 1 : 0) << ",overlay_labels=" << (g_camOverlayTimeLabels ? 1 : 0)
                  << ",overlay_trail=" << (g_camOverlayTrail ? 1 : 0) << '\n';
    }

    // The script timecycle modifier and the weather variations recorded
    // (TimecycleRecord) and played back (TimecycleReplay); both on by default
    // (timecycle_track.h).
    bool g_timecycleRecord = true;
    bool g_timecycleReplay = true;

    // The sky object's moon angles recorded (MoonRecord, section MOON) and, while
    // a clip sets the clock, kept from moving the session's moon and played back
    // from the clip (MoonReplay); both on by default (moon_track.h).
    bool g_moonRecord = true;
    bool g_moonReplay = true;

    // ---------------- Language (src/i18n.h) ----------------
    // RedemptionEditor.ini Language (auto: the game's language), and the game's
    // language read with GET_CURRENT_LANGUAGE every few seconds on this thread;
    // the editor gets it in the snapshot and loads the same file.
    std::string g_languageSetting;
    bool g_languageRead = false;
    std::uint32_t g_gameLanguage = 0; // GET_CURRENT_LANGUAGE + 1, 0 not read yet
    std::string g_languageCode = "en";
    ULONGLONG g_languageCheckedAt = 0;

    void UpdateLanguage(ULONGLONG now)
    {
        if (!g_languageRead)
        {
            g_languageRead = true;
            char workingDirectory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, workingDirectory);
            const std::string configPath = replay::identity::IniPathIn(workingDirectory);
            char value[32]{};
            replay::identity::IniString("Language", "auto", value, sizeof(value), configPath.c_str());
            g_languageSetting = value;
        }
        if (g_languageCheckedAt && now - g_languageCheckedAt < 2000)
            return;
        g_languageCheckedAt = now;
        g_gameLanguage = static_cast<std::uint32_t>(LOCALIZATION::GET_CURRENT_LANGUAGE()) + 1u;
        const std::string code = replay::i18n::Choose(g_languageSetting, g_gameLanguage);
        if (code == g_languageCode)
            return;
        g_languageCode = code;
        // RedemptionEditor\lang next to RDR2.exe.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring folder = exe;
        folder = folder.substr(0, folder.find_last_of(L"\\/") + 1) + L"RedemptionEditor\\lang\\";
        std::string loaded;
        std::unique_ptr<replay::i18n::Table> table = replay::i18n::Load(folder, code, &loaded);
        LogFile() << "LANGUAGE,code=" << code << ",game=" << static_cast<int>(g_gameLanguage) - 1 << ",setting=" << g_languageSetting
                  << ",file=" << (loaded.empty() ? "none" : loaded) << ",keys=" << (table ? table->Size() : 0u)
                  << ",problems=" << (table ? table->problems.size() : 0u) << '\n';
        if (table)
            for (const std::string& problem : table->problems)
                LogFile() << "LANGUAGE_PROBLEM,file=" << loaded << "," << problem << '\n';
        replay::i18n::SetActive(std::move(table));
    }

    void WriteLogHeader()
    {
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);

        const std::string configPath = replay::identity::IniPathIn(workingDirectory);
        g_debugMode = replay::identity::DevIniInt("Debug", 0, configPath.c_str()) != 0;
        UpdateLanguage(GetTickCount64()); // before the [Keys] notice is worded
        // [Keys], the same table the add-on loads (editor_keys.h); logged after the
        // header line.
        std::string keysLog;
        {
            const replay::keys::LoadResult keys = replay::keys::LoadFromIni(configPath.c_str());
            for (const replay::keys::Issue& issue : keys.issues)
                keysLog += replay::keys::LogLine(issue) + "\n";
            std::string table;
            for (const replay::keys::ActionInfo& info : replay::keys::kActions)
            {
                const replay::keys::Binding* bound = keys.table.Of(info.action);
                table += (table.empty() ? "" : ";") + std::string(info.iniKey) + ":";
                if (!bound[0].Valid() && !bound[1].Valid())
                    table += "None";
                for (std::size_t slot = 0; slot < replay::keys::kSlots; ++slot)
                {
                    if (bound[slot].Valid())
                        table += (slot ? "|" : "") + replay::keys::IniName(bound[slot]);
                }
            }
            keysLog += "KEYS_TABLE,configured=" + std::to_string(keys.configured) + ",refused=" +
                std::to_string(keys.issues.size()) + ",keys=" + table + "\n";
            g_keysNotice = replay::keys::NoticeText(keys.issues);
        }
        // FullLog=1 (the default) keeps the whole log; 0 only the essential lines.
        replay::log::SetVerbose(replay::identity::IniInt("FullLog", 1, configPath.c_str()) != 0);
        g_reportIncludeClips = replay::identity::IniInt("ReportIncludeClips", 0, configPath.c_str()) != 0; // release 1.0
        // FREC volume creation on puppets; 0 turns it off.
        replay::burn::SetCharCreate(replay::identity::IniInt("FireCharCreate", 1, configPath.c_str()) != 0);
        // 1 moves a rider puppet with his mount at the ped pre-render; 0 only measures.
        replay::mount::SetEnabled(replay::identity::DevIniInt("MountPreRender", 0, configPath.c_str()) != 0);
        // End-of-frame placement and the plants-only body.
        {
            const int jitterFix = replay::identity::DevIniInt("JitterFix", 1, configPath.c_str()) != 0 ? 1 : 0;
            const bool frameEnd = replay::identity::IniInt("FrameEnd", jitterFix, configPath.c_str()) != 0;
            const bool physicsFilter = replay::identity::IniInt("PhysicsFilter", jitterFix, configPath.c_str()) != 0;
            char keepText[40]{};
            replay::identity::DevIniString("PuppetIncludeKeep", "", keepText, sizeof(keepText), configPath.c_str());
            char* keepEnd = nullptr;
            const unsigned long long keepValue = std::strtoull(keepText, &keepEnd, 0);
            const std::uint64_t keep = keepText[0] && keepEnd && keepEnd != keepText && keepValue
                ? static_cast<std::uint64_t>(keepValue) : replay::collision::kDefaultKeep;
            replay::mount::SetFrameEnd(frameEnd);
            replay::collision::Configure(physicsFilter, keep);
            replay::mount::SetPhysicsFilterNote(physicsFilter);
        }
        g_waterShapeLog = replay::identity::DevIniInt("WaterShapeLog", 0, configPath.c_str()) != 0;
        replay::water::probe::Configure(configPath); // WaterGridProbe, evidence only
        replay::soundgeneric::Configure(configPath); // SoundGeneric, off by default
        replay::exportprobe::Configure(configPath); // render export sketch: [Export] Enabled, off by default
        g_recConfig.buffer = replay::identity::IniInt("ReplayBuffer", 1, configPath.c_str()) != 0; // on by default
        g_recConfig.bufferSeconds = (std::max)(10u, replay::identity::IniInt("ReplayBufferSeconds", 60, configPath.c_str()));
        g_recConfig.bufferMB = replay::buffercov::ClampBufferMB(replay::identity::IniInt("ReplayBufferMB", 128, configPath.c_str()));
        g_recConfig.manualMaxSeconds = replay::identity::IniInt("ManualMaxSeconds", 0, configPath.c_str());
        g_recConfig.manualMaxMB = replay::buffercov::ClampManualMB(replay::identity::IniInt("ManualMaxMB", 900, configPath.c_str()));
        // Off by default; the ring keeps ReplayBufferMB.
        g_recConfig.bufferAdaptive = replay::identity::DevIniInt("ReplayBufferAdaptive", 0, configPath.c_str()) != 0;
        // clip_start_gate.h.
        {
            replay::clipstart::Config& c = g_clipStartConfig;
            c.log = replay::identity::DevIniInt("ClipWorldLog", 1, configPath.c_str()) != 0;
            c.worldWait = replay::identity::DevIniInt("ClipWorldWait", 0, configPath.c_str()) != 0;
            c.worldCapMs = std::clamp(replay::identity::DevIniInt("ClipWorldWaitMs", 20000, configPath.c_str()), 3000u, 120000u);
            c.stagger = replay::identity::DevIniInt("ClipStartStagger", 0, configPath.c_str()) != 0;
            c.batch = std::clamp(replay::identity::DevIniInt("ClipStartBatch", 2, configPath.c_str()), 1u, 16u);
            c.settleMinMs = std::clamp(replay::identity::DevIniInt("ClipStartSettleMs", 500, configPath.c_str()), 0u, 10000u);
            c.settleCapMs = (std::max)(c.settleMinMs, static_cast<std::uint64_t>(3000));
            c.streamingMax = static_cast<int>(replay::identity::DevIniInt("ClipStartStreamingMax", 8, configPath.c_str()));
        }
        g_recConfig.bufferMaxMB = replay::buffercov::ClampBufferMB(replay::identity::DevIniInt("ReplayBufferMaxMB", 512, configPath.c_str()));
        g_recConfig.bufferReserveMB = std::clamp(replay::identity::DevIniInt("ReplayBufferReserveMB", 2048, configPath.c_str()), 256u, 65536u);
        g_recConfig.replayDeadEye = replay::identity::IniInt("ReplayDeadEye", 0, configPath.c_str()) != 0;
        g_recConfig.compressClips = replay::identity::IniInt("CompressClips", 1, configPath.c_str()) != 0;
        g_recConfig.compressAfterDays = std::clamp(
            replay::identity::IniInt("CompressAfterDays", static_cast<int>(replay::clipcompress::kDefaultDays), configPath.c_str()), 1u, 60u);
        g_recConfig.compressLevel = static_cast<int>(std::clamp(
            replay::identity::IniInt("CompressLevel", replay::clipcompress::kDefaultLevel, configPath.c_str()), 1u, 22u));
        g_recConfig.compressFormat = replay::identity::DevIniInt("CompressFormat", 2, configPath.c_str()) == 1 ? 1 : 2;
        g_recConfig.recordCutscenes = replay::identity::IniInt("RecordCutscenes", 0, configPath.c_str()) != 0;
        g_recConfig.lockCutsceneCamera = replay::identity::IniInt("LockCutsceneCamera", 0, configPath.c_str()) != 0;
        g_recConfig.restoreWorldState = replay::identity::IniInt("RestoreWorldState", 1, configPath.c_str()) != 0;
        g_recConfig.restoreWorldStateInMission =
            replay::identity::IniInt("RestoreWorldStateInMission", 0, configPath.c_str()) != 0;
        g_recConfig.freezeWorldOnPause = replay::identity::IniInt("FreezeWorldOnPause", 1, configPath.c_str()) != 0;
        {
            char value[32]{};
            replay::identity::IniString("RecordingIndicator", "normal", value, sizeof(value), configPath.c_str());
            g_recConfig.recordingIndicator = replay::recindicator::Parse(value);
        }
        g_recConfig.trailPlayerFollow = replay::identity::DevIniInt("TrailPlayerFollow", 1, configPath.c_str()) != 0;
        g_recConfig.musicStop = replay::identity::DevIniInt("MusicStop", 1, configPath.c_str());
        // The pool reader (entity_pools.h); 0 goes back to the listings.
        replay::pools::SetEnabled(replay::identity::DevIniInt("EntityPoolReader", 1, configPath.c_str()) != 0);
        // Trains (replay_trains.h).
        replay::trains::SetPuppetMode(static_cast<int>(replay::identity::DevIniInt("TrainPuppet", 0, configPath.c_str())));
        replay::trains::SetWorldClearMode(static_cast<int>(replay::identity::DevIniInt("TrainWorldClear", 0, configPath.c_str())));
        // The door ratios are read and driven by natives (vehicle_doors_engine.h);
        // this key writes the current ratio of the puppet's own door when the native's
        // step left it off (rung 3).
        replay::vehdoors::SetWrite(replay::identity::IniInt("VehicleDoorWrite", 1, configPath.c_str()) != 0);
        // The capture shortcuts and the streaming copy are always on. The stall polls
        // (250 ms unless set; 0: the plain per-tick polls) and the end-of-frame check
        // backoff (0: off).
        replay::pose::capture::SetStallPollMs(static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(replay::identity::DevIniInt("PoseStallPollMs",
                static_cast<int>(replay::pose::capture::kDefaultStallPollMs), configPath.c_str())),
            0, 1000)));
        replay::pose::capture::SetLateBackoff(replay::identity::DevIniInt("PoseLateBackoff", 0, configPath.c_str()) != 0);
        // The pose worker's poll period (5 ms unless set).
        replay::pose::capture::SetWorkerPollMs(replay::identity::DevIniInt("PoseWorkerPollMs", 5, configPath.c_str())); // clamped to 1..100
        // The pose fidelity judge (off by default).
        replay::pose::fidelity::Configure(replay::identity::DevIniInt("PoseFidelity", 0, configPath.c_str()) != 0,
            replay::identity::DevIniInt("PoseFidelityHz", static_cast<int>(replay::pose::fidelity::kDefaultHz),
                configPath.c_str()));
        // The judge's evaluation off the stop tick.
        replay::pose::fidelity::SetAsync(replay::identity::DevIniInt("PoseFidelityAsync", 1, configPath.c_str()) != 0);
        // The puppets' and the hidden player's own feet in the replay (0: kept out),
        // and the slow tick threshold of the profiler (PROFILE_SLOW_TICK, ms).
        replay::events::SetEngineFeetInReplay(
            replay::identity::DevIniInt("EngineFeetInReplay", 0, configPath.c_str()) != 0);
        replay::events::SetFxOwnerRule(
            replay::identity::DevIniInt("FxOwnerRule", 1, configPath.c_str()) != 0);
        replay::events::SetFxRegisteredSplit(
            replay::identity::DevIniInt("FxRegisteredSplit", 1, configPath.c_str()) != 0);
        replay::events::SetFxWorldSiteRule(
            replay::identity::DevIniInt("FxWorldSiteRule", 1, configPath.c_str()) != 0);
        {
            // The particle keys.
            const auto key = [&](const char* name, int fallback) {
                return replay::identity::IniInt(name, fallback, configPath.c_str());
            };
            // A developer key (the public build takes the default).
            const auto devKey = [&](const char* name, int fallback) {
                return replay::identity::DevIniInt(name, fallback, configPath.c_str());
            };
            replay::events::FxParticlesConfig fx;
            fx.motionProbe = devKey("FxMotionProbe", 1) != 0;
            fx.puppetNoWarp = key("FxPuppetNoWarp", 1) != 0;
            fx.recordSettings = devKey("FxRecordSettings", 1) != 0;
            fx.settingsReplay = key("FxSettingsReplay", 1) != 0;
            fx.registeredParentOnce = devKey("FxRegisteredParentOnce", 1) != 0;
            fx.callGaps = devKey("FxRegisteredCallGaps", 1) != 0;
            fx.ownerReady = devKey("FxRegisteredOwnerReady", 1) != 0;
            const int backoff = devKey("FxRegisteredRecreateBackoffMs", 250);
            fx.recreateBackoffMs = backoff < 0 ? 0u : backoff > 5000 ? 5000u : static_cast<std::uint32_t>(backoff);
            fx.puppetVelocity = key("FxPuppetVelocity", 1) != 0;
            fx.worldOwnerRule = devKey("FxWorldOwnerRule", 1) != 0;
            fx.ownerListImmediate = devKey("FxOwnerListImmediate", 1) != 0;
            fx.weaponFxEvents = devKey("WeaponFxEvents", 1) != 0;
            fx.applyRecordedFlags = devKey("FxApplyRecordedFlags", 1) != 0;
            fx.shotLegacyDedupe = devKey("ShotFxLegacyDedupe", 1) != 0;
            fx.pauseFreeze = key("FxPauseFreeze", 1) != 0;
            fx.checkMotion = devKey("FxCheckMotion", 1) != 0;
            fx.relocateClearStill = key("FxRelocateClearStill", 1) != 0;
            fx.weaponFxIntent = key("WeaponFxIntent", 1) != 0;
            replay::events::SetFxParticlesConfig(fx);
        }
        replay::events::SetSpeechPlayerOnPlayer( // the player's lines on the hidden player
            replay::identity::DevIniInt("SpeechPlayerOnPlayer", 1, configPath.c_str()) != 0);
        replay::events::SetFireUntrackedInitial(
            replay::identity::DevIniInt("FireUntrackedInitial", 0, configPath.c_str()) != 0);
        replay::events::SetFireAdoptLive( // an adopted fire can go out early
            replay::identity::DevIniInt("FireAdoptLive", 0, configPath.c_str()) != 0);
        // Evidence lines of the player's horse audio, record against playback.
        replay::horseaudio::SetEnabled(
            replay::identity::DevIniInt("HorseAudioEvidence", 0, configPath.c_str()) != 0);
        // The mix probe, read only (where each sound is heard from, record against
        // replay).
        replay::mixprobe::SetEnabled(replay::identity::DevIniInt("MixProbe", 0, configPath.c_str()) != 0);
        {
            // Named render targets and the UI apps drawing into them (the camp ledger).
            replay::rendertarget::Settings rt;
            rt.record = replay::identity::IniInt("RenderTargetRecord", 1, configPath.c_str()) != 0;
            // 2 (the UI app relaunched) is experimental: the developer build only;
            // the public build takes it as 1 (troubleshooting-keys.md lists 0 and 1).
            int rtPlayback = static_cast<int>(replay::identity::IniInt("RenderTargetPlayback", 1, configPath.c_str()));
            if constexpr (!replay::identity::kDevBuild)
                rtPlayback = (std::min)(rtPlayback, 1);
            rt.mode = replay::rendertarget::ModeFromInt(rtPlayback);
            char apps[256] = {};
            replay::identity::DevIniString("RenderTargetUiApps", "SHOP_BROWSING", apps, sizeof(apps), configPath.c_str());
            rt.uiApps = replay::rendertarget::ParseAppList(apps);
            rt.blockInput = replay::identity::DevIniInt("RenderTargetUiAppBlockInput", 1, configPath.c_str()) != 0;
            replay::rendertarget::Configure(rt);
        }
        // The streaming focus on the rendering camera.
        g_worldFocus = replay::identity::DevIniInt("WorldFocus", 1, configPath.c_str()) != 0;
        // Record the live engine volume listener (LSTN), put it back on the recorded
        // game camera (off by default), and the per second LISTENER line (defaults to
        // MixProbe).
        replay::gamelistener::Configure(replay::identity::DevIniInt("ListenerRecord", 1, configPath.c_str()) != 0,
            replay::identity::DevIniInt("ReplayGameListener", 0, configPath.c_str()) != 0,
            replay::identity::DevIniInt("GameListenerLog", replay::mixprobe::Enabled() ? 1 : 0, configPath.c_str()) != 0);
        {
            // LSTN v2 and the recorded microphone put back (game_listener.h).
            char hashText[32]{};
            replay::identity::DevIniString("ListenerMicFallbackHash", "0", hashText, sizeof(hashText), configPath.c_str());
            char matchText[32]{};
            replay::identity::DevIniString("ListenerCamMatchM", "1.0", matchText, sizeof(matchText), configPath.c_str());
            replay::gamelistener::ConfigureMic( // release 1.0: ListenerRecordV2 removed (v2 always)
                replay::identity::IniInt("ListenerMicReplay", 1, configPath.c_str()) != 0,
                static_cast<std::uint32_t>(std::strtoul(hashText, nullptr, 0)),
                static_cast<float>(std::strtod(matchText, nullptr)));
        }
        // The mixer scenes and mix groups recorded (MIXS, record only, default 1) and
        // replayed (default 0), and the speech level probe (read only).
        {
            replay::mixscene::Config mix;
            mix.record = replay::identity::DevIniInt("MixSceneRecord", 1, configPath.c_str()) != 0;
            mix.replay = replay::identity::DevIniInt("MixSceneReplay", 0, configPath.c_str()) != 0;
            mix.probe = replay::identity::DevIniInt("SpeechLevelProbe", 0, configPath.c_str()) != 0;
            // Which kinds are replayed (code scenes off by default), the scene variables,
            // and code scene hashes never replayed.
            mix.kinds = 0;
            if (replay::identity::DevIniInt("MixSceneReplayScript", 1, configPath.c_str()) != 0)
                mix.kinds |= replay::mixscene::kReplayScript;
            if (replay::identity::DevIniInt("MixSceneReplayConversation", 1, configPath.c_str()) != 0)
                mix.kinds |= replay::mixscene::kReplayConversation;
            if (replay::identity::DevIniInt("MixSceneReplayCode", 0, configPath.c_str()) != 0)
                mix.kinds |= replay::mixscene::kReplayCode;
            mix.vars = replay::identity::DevIniInt("MixSceneVars", 1, configPath.c_str()) != 0;
            {
                char skip[512]{};
                replay::identity::DevIniString("MixSceneCodeSkip", "", skip, sizeof(skip), configPath.c_str());
                mix.skip = replay::mixscene::ParseHashList(skip);
            }
            // The scene pool read directly (record the direct scenes and every open one,
            // MIXS v3; the engine's open scenes from the pool; the hidden player's state
            // scenes owned by the replay; the pool log).
            mix.poolRecord = replay::identity::DevIniInt("MixSceneRecordDirect", 1, configPath.c_str()) != 0;
            mix.engineOpenFromPool = replay::identity::DevIniInt("MixSceneEngineOpenFromPool", 1, configPath.c_str()) != 0;
            mix.ownPlayerState = replay::identity::DevIniInt("MixSceneOwnPlayerState", 1, configPath.c_str()) != 0;
            mix.poolLog = replay::identity::DevIniInt("MixScenePoolLog", 1, configPath.c_str()) != 0;
            replay::mixscene::Configure(mix);
            replay::speechlevel::SetEnabled(mix.probe);
        }
        {
            // The sound categories' controller values recorded (CATS) and put back while
            // the replay owns the world; the sliders, the music and BASE stay live.
            replay::catstate::Config cats;
            cats.record = replay::identity::DevIniInt("CategoryStateRecord", 1, configPath.c_str()) != 0;
            cats.replay = replay::identity::IniInt("CategoryStateReplay", 1, configPath.c_str()) != 0;
            char text[512]{};
            replay::identity::DevIniString("CategoryStateFields", "volume", text, sizeof(text), configPath.c_str());
            cats.fieldsAll = replay::catstate::ParseFieldsAll(text);
            replay::identity::DevIniString("CategoryStateKeepLive", replay::catstate::kDefaultKeepLiveText, text, sizeof(text),
                configPath.c_str());
            cats.keepLive = replay::mixscene::ParseHashList(text);
            cats.log = replay::identity::DevIniInt("CategoryStateLog", 0, configPath.c_str()) != 0;
            replay::identity::DevIniString("CategoryStateWatch", replay::catstate::kDefaultWatchText, text, sizeof(text),
                configPath.c_str());
            cats.watch = replay::mixscene::ParseHashList(text);
            replay::catstate::Configure(cats);
            // The engine's speech duck fed with the recorded inputs.
            replay::speechduck::Config duck;
            duck.replay = replay::identity::IniInt("SpeechDuckReplay", 1, configPath.c_str()) != 0;
            duck.derivedFlags = static_cast<std::uint8_t>(
                replay::identity::DevIniInt("SpeechDuckDerivedFlags", replay::speechduck::kDefaultDerivedFlags, configPath.c_str()) & 0x0F);
            duck.log = replay::identity::DevIniInt("SpeechDuckLog", 1, configPath.c_str()) != 0;
            replay::speechduck::Configure(duck);
            LogFile() << "CATEGORY_STATE,phase=config,record=" << (cats.record ? 1 : 0) << ",replay=" << (cats.replay ? 1 : 0)
                      << ",fields=" << (cats.fieldsAll ? "all" : "volume") << ",keep_live=" << cats.keepLive.size()
                      << ",log=" << (cats.log ? 1 : 0) << ",watch=" << cats.watch.size() << ",speech_duck=" << (duck.replay ? 1 : 0)
                      << ",derived_flags=" << static_cast<int>(duck.derivedFlags)
                      << ",pool_record=" << (replay::mixscene::GetConfig().poolRecord ? 1 : 0)
                      << '\n';
        }
        {
            // The volume probe, a measurement build (read only): every voice's gain by
            // stage and class, recording against replay.
            replay::volumeprobe::Config vp;
            vp.enabled = replay::identity::DevIniInt("VolumeProbe", 0, configPath.c_str()) != 0;
            vp.sampleMs = static_cast<std::uint32_t>((std::max)(20, static_cast<int>(replay::identity::DevIniInt("VolumeProbeSampleMs", 100, configPath.c_str()))));
            vp.windowMs = static_cast<std::uint32_t>((std::max)(100, static_cast<int>(replay::identity::DevIniInt("VolumeProbeWindowMs", 500, configPath.c_str()))));
            vp.categories = static_cast<std::uint32_t>((std::max)(0, static_cast<int>(replay::identity::DevIniInt("VolumeProbeCategories", 6, configPath.c_str()))));
            vp.top = replay::identity::DevIniInt("VolumeProbeTop", 1, configPath.c_str()) != 0;
            vp.world = static_cast<std::uint32_t>((std::max)(0, static_cast<int>(replay::identity::DevIniInt("VolumeProbeWorld", 8, configPath.c_str()))));
            vp.env = replay::identity::DevIniInt("VolumeProbeEnv", 1, configPath.c_str()) != 0;
            replay::volumeprobe::Configure(vp);
            // The world sound probe (read only): the engine's particle audio slots and
            // horse group audio, recording against replay.
            replay::worldsoundprobe::Config ws;
            ws.enabled = replay::identity::DevIniInt("WorldSoundProbe", vp.enabled ? 1 : 0, configPath.c_str()) != 0; // default: VolumeProbe
            ws.groupMs = static_cast<std::uint32_t>((std::max)(100, static_cast<int>(replay::identity::DevIniInt("WorldSoundProbeGroupMs", 1000, configPath.c_str()))));
            replay::worldsoundprobe::Configure(ws);
        }
        {
            // The weather audio's "is the world loaded about the player" gate asks about
            // the replay's scene while the replay owns the world (WindReplay), and the
            // recorded wind strength is logged against the replay's (WindReplayLog,
            // section WNDA).
            replay::windreplay::Config wind;
            wind.fix = replay::identity::IniInt("WindReplay", 1, configPath.c_str()) != 0;
            wind.log = replay::identity::DevIniInt("WindReplayLog", 1, configPath.c_str()) != 0;
            replay::windreplay::Configure(wind);
            LogFile() << "WIND_REPLAY,phase=config,fix=" << (wind.fix ? 1 : 0) << ",log=" << (wind.log ? 1 : 0) << '\n';
        }
        // Footstep surface.
        replay::audio::SetFootMaterialReplay(
            replay::identity::DevIniInt("FootMaterialReplay", 1, configPath.c_str()) != 0);
        // Object pose places.
        replay::objects::SetObjectPosePlaces(static_cast<std::uint32_t>((std::max)(0,
            static_cast<int>(replay::identity::DevIniInt("ObjectPosePlaces", 0, configPath.c_str())))));
        // The weapons' own bones. Our own logic, no ini key (keys only for engine
        // detours and calls).
        replay::objects::SetWeaponBonePose(true); // every weapon in a hand, the free capture slots
        // A detour, so a key.
        replay::objects::SetWeaponPassPose(replay::identity::IniInt("WeaponPassPose", 1, configPath.c_str()) != 0);
        // The weapon look is our logic and natives (no key); the rust and sixth wear
        // channel are plain writes on the replay's own weapon (rung 3), so a key.
        replay::objects::SetWeaponWearWrite(replay::identity::IniInt("WeaponWearWrite", 1, configPath.c_str()) != 0);
        // The reins hold mode detour (reins_guard.h), so a key.
        replay::reins::SetHoldGuard(replay::identity::IniInt("ReinsHoldGuard", 1, configPath.c_str()) != 0);
        replay::objects::SetAmmoScanEveryTick(true); // (C)
        // Hat looks.
        replay::objects::SetHatLook(replay::identity::DevIniInt("HatLook", 1, configPath.c_str()) != 0);
        // The world model.
        {
            char text[32]{};
            // The closed theater: the replay always takes the world over; WorldModel=off
            // turns the model off.
            replay::identity::IniString("WorldModel", "gta", text, sizeof(text), configPath.c_str());
            g_recConfig.worldModel = replay::worldmodel::ModeFromIni(text);
            char path[32]{};
            replay::identity::DevIniString("WorldReloadPath", "slot", path, sizeof(path), configPath.c_str());
            g_recConfig.worldReloadPath = replay::worldmodel::ReloadPathFromIni(path);
            char flags[32]{};
            replay::identity::DevIniString("WorldClearFlags", "", flags, sizeof(flags), configPath.c_str());
            char* end = nullptr;
            const unsigned long parsed = flags[0] ? std::strtoul(flags, &end, 0) : 0ul; // decimal or 0x hex
            g_recConfig.worldClearFlags = replay::worldmodel::SanitizeClearFlags(
                flags[0] && end != flags && parsed ? static_cast<std::uint32_t>(parsed) : replay::worldmodel::kDefaultClearFlags);
            g_recConfig.worldScriptsCleanup = replay::identity::DevIniInt("WorldScriptsCleanup", 1, configPath.c_str()) != 0;
            g_recConfig.worldModelStep = replay::worldmodel::StepFromIni(
                replay::identity::DevIniInt("WorldModelStep", static_cast<INT>(replay::worldmodel::kStepReload), configPath.c_str()));
            // Closed theater: each engine step has its own key (1 on, 0 off).
            replay::theatre::Config& theatre = g_recConfig.theatre;
            const auto key = [&](const char* name, bool fallback) {
                return replay::identity::IniInt(name, fallback ? 1 : 0, configPath.c_str()) != 0;
            };
            // A developer key (the public build takes the default).
            const auto devKey = [&](const char* name, bool fallback) {
                return replay::identity::DevIniInt(name, fallback ? 1 : 0, configPath.c_str()) != 0;
            };
            theatre.log = devKey("TheatreLog", theatre.log);
            theatre.perschar = key("TheatrePerschar", theatre.perschar);
            theatre.vehicles = key("TheatreVehicles", theatre.vehicles);
            theatre.peds = key("TheatrePeds", theatre.peds);
            theatre.pauseScripts = key("TheatrePauseScripts", theatre.pauseScripts);
            theatre.pauseScriptHook = devKey("TheatrePauseScriptHook", theatre.pauseScriptHook);
            theatre.denySpawn = key("TheatreDenySpawn", theatre.denySpawn);
            theatre.emitters = key("TheatreEmitters", theatre.emitters);
            theatre.hooks = static_cast<std::uint32_t>(replay::identity::IniInt("TheatreHooks", static_cast<INT>(theatre.hooks), configPath.c_str()));
            g_theatreReloadDelayMs = static_cast<ULONGLONG>((std::max)(0, (std::min)(30000,
                static_cast<int>(replay::identity::DevIniInt("TheatreReloadDelayMs", 0, configPath.c_str())))));
            replay::events::SetSpeechQueueAnyContext(devKey("SpeechQueueAnyContext", true));
            // The player's weapon bank, on by default (without it the player's shots play
            // with bank=absent). Natives only (GIVE_WEAPON_TO_PED, SET_CURRENT_PED_WEAPON
            // on the hidden live player, script thread); the key stays for an A/B.
            replay::events::SetPlayerWeaponBank(devKey("PlayerWeaponBank", true));
            replay::events::SetHumanOwnVoiceFallback(devKey("HumanOwnVoiceFallback", true));
            // Puppet LOD (puppet_lod.h).
            replay::lod::Config lod;
            lod.multiplier = std::min<std::uint32_t>(50u, static_cast<std::uint32_t>(replay::identity::DevIniInt("PuppetLodMultiplier", static_cast<INT>(lod.multiplier), configPath.c_str())));
            lod.prerender = devKey("PuppetAlwaysPrerender", lod.prerender);
            lod.forceUpdate = devKey("PuppetForceAnimUpdate", lod.forceUpdate);
            lod.scriptPose = key("PuppetScriptPose", lod.scriptPose);
            lod.animLod0 = devKey("PuppetAnimLod0", lod.animLod0);
            replay::playback::SetPuppetLod(lod);
            // The horses' muscles (shader_vars_replay.h): recorded (SHVR) and written
            // into the puppets' shader vars component before the shader reads them.
            replay::muscles::Config muscles;
            muscles.record = devKey("MuscleRecord", muscles.record);
            muscles.replay = devKey("MuscleReplay", muscles.replay);
            muscles.debugForce = static_cast<std::uint8_t>((std::max)(0, (std::min)(2,
                static_cast<int>(replay::identity::DevIniInt("MuscleDebugForce", 0, configPath.c_str())))));
            // DevIniInt returns UINT: the -1 default must go back through int (an earlier build
            // read it as 4294967295, so every horse puppet got SET_PED_SWEAT(4.29e7) each tick).
            muscles.debugSweat = static_cast<float>(static_cast<int>(
                replay::identity::DevIniInt("MuscleDebugSweatPct", -1, configPath.c_str()))) / 100.0f;
            if (muscles.debugSweat > 1.0f)
                muscles.debugSweat = -1.0f;
            muscles.debugOverwrite = devKey("MuscleDebugOverwrite", muscles.debugOverwrite);
            replay::muscles::SetConfig(muscles);
            replay::playback::SetHatFix(devKey("HatFix", true));
            // HideNeverAnimated 0 shows, 1 (default) hides a never animated body recorded
            // in its bind pose, 2 also when the bind pose cannot be read; TPoseWatch the
            // T-pose detector (default 1). See tpose_check.h.
            replay::playback::SetTPose(static_cast<int>(replay::identity::DevIniInt("HideNeverAnimated", 1, configPath.c_str())),
                devKey("TPoseWatch", true));
            // When the T-pose watch judges.
            replay::playback::SetTPoseGate(static_cast<std::uint32_t>((std::max)(0, static_cast<int>(
                                               replay::identity::DevIniInt("TPoseSettleMs", 1500, configPath.c_str())))),
                devKey("TPoseOnScreenOnly", true));
            // The puppets' "mount" sound chains through the replay's mount (entity_query.h).
            replay::entityquery::SetRelation(key("EntityQueryRelation", true));
            replay::entityquery::SetLogWanted(devKey("EntityQueryLog", true));
            // ... and the engine's answers recorded with the clip (EQRY), given back to the puppets.
            replay::entityquery::SetRecordAnswers(key("EntityQueryRecord", true));
            replay::entityquery::SetReplayAnswers(key("EntityQueryReplay", true));
            // The recorded foley variants on the puppets, the PED_VARS evidence, and the
            // read-only probe of the sound variable lookup (ped_audio_state.h).
            // PuppetFoleyVariants is off by default: every live ped, the player included,
            // reads -1/-1 at component +0xEC/+0xF0, so the recorded draw the write assumes
            // never exists live.
            replay::events::SetPedAudioState(devKey("PuppetFoleyVariants", false), devKey("PedAudioStateLog", true),
                devKey("PedVariablesReadProbe", false));
            // The puppets' PED_VARIABLES speed (a puppet reads 0 where the live ped reads
            // its speed): recorded (PSPD) and written back.
            replay::events::SetPedSpeed(devKey("PedSpeedRecord", true), key("PedSpeedReplay", true),
                devKey("PedSpeedReplayLog", true));
            // The secondary steps (the hoof that lifts or lands) recorded as events (FSEV)
            // and queued again on the puppets, and the movement component's own speed on
            // the puppets (the gait and the step volume read it).
            replay::events::SetFootEvents(devKey("FootEventRecord", true), key("FootEventReplay", true),
                devKey("FootEventLog", true));
            replay::events::SetMotionSpeed(key("MotionSpeedReplay", true), devKey("MotionStateLog", true));
            // The holder's environment group on weapon sounds.
            replay::events::SetShotEnvGroup(key("ShotEnvGroupReplay", true));
            // The movement volume result per step recorded (FMOV) and given back.
            replay::events::SetFootMovement(devKey("FootMovementRecord", true), key("FootMovementReplay", true));
            // The timecycle modifier and weather variations (section TCMD).
            g_timecycleRecord = devKey("TimecycleRecord", true);
            g_timecycleReplay = devKey("TimecycleReplay", true);
            // The moon's angles (section MOON, moon_track.h).
            g_moonRecord = devKey("MoonRecord", true);
            g_moonReplay = key("MoonReplay", true);
            {
                // replay_capture.h FullAnimConfig: experiment, off by default.
                replay::recorder::FullAnimConfig fullAnim;
                fullAnim.prerender = devKey("RecordPedPrerender", false);
                fullAnim.forceAnim = devKey("RecordPedForceAnim", false);
                fullAnim.lodMultiplier = std::min<std::uint32_t>(50u, static_cast<std::uint32_t>(replay::identity::DevIniInt("RecordPedLodMultiplier", 0, configPath.c_str())));
                fullAnim.radiusM = static_cast<float>(std::max(5, std::min(150, static_cast<int>(replay::identity::DevIniInt("RecordPedAnimRadiusM", 40, configPath.c_str())))));
                // The engine's "animate as if visible" byte (anim_visible.h) on every recorded
                // ped and animal, so hat fit, face and springs are computed off screen.
                fullAnim.animVisible = key("RecordPedAnimVisible", true);
                // ... and LOD 0 through the detour of 0x2BD410C (anim_lod_hook.h): faces far from the camera too.
                fullAnim.animLod0 = key("RecordPedAnimLod0", true);
                replay::recorder::SetFullAnim(fullAnim);
            }
            replay::doors::Config doors;
            doors.rebindMs = std::min<std::uint32_t>(5000u, static_cast<std::uint32_t>(replay::identity::DevIniInt("DoorRebindMs", static_cast<INT>(doors.rebindMs), configPath.c_str())));
            doors.keepOpenRatio = devKey("DoorKeepOpenRatio", doors.keepOpenRatio);
            doors.mapWaitMs = std::min<std::uint32_t>(8000u, static_cast<std::uint32_t>(replay::identity::DevIniInt("MapObjectsWaitMs", static_cast<INT>(doors.mapWaitMs), configPath.c_str())));
            replay::doors::Configure(doors);
            char cutMode[16]{};
            replay::identity::DevIniString("ReplayCameraCutMode", "bits", cutMode, sizeof(cutMode), configPath.c_str());
            g_camCutMode = replay::camcut::ModeFromIni(cutMode);
            // Camera DOF, target and streaming keys.
            g_camDofRecreate = devKey("ReplayCamDofRecreate", false);
            g_camFocusConfig.blend = devKey("ReplayCamDofBlend", true);
            g_camFocusConfig.hysteresis = devKey("ReplayCamFocusHysteresis", true);
            g_camFocusConfig.holdLast = g_camFocusConfig.hysteresis;
            g_camFocusConfig.smoothOmega = devKey("ReplayCamFocusSmooth", true) ? 12.0f : 0.0f;
            g_camTargetRecorded = devKey("ReplayCamTargetRecorded", true);
            g_camSmoothReframe = devKey("ReplayCamSmoothReframe", true);
            g_camPrefetchOn = devKey("ReplayCamPrefetch", true);
            g_camPrefetchMeters = static_cast<float>(std::clamp(static_cast<int>(replay::identity::DevIniInt("ReplayCamPrefetchMeters",
                static_cast<INT>(replay::camera::kPrefetchMinMeters), configPath.c_str())), 5, 2000));
            g_camPrefetchLeadMs = static_cast<std::uint32_t>(std::clamp(static_cast<int>(replay::identity::DevIniInt("ReplayCamPrefetchLeadMs", static_cast<INT>(replay::camera::kPrefetchLeadMs), configPath.c_str())), 200, 5000));
            LogFile() << "CONFIG_CAMERA2,dof_recreate=" << (g_camDofRecreate ? 1 : 0) << ",dof_blend="
                      << (g_camFocusConfig.blend ? 1 : 0) << ",focus_hysteresis=" << (g_camFocusConfig.hysteresis ? 1 : 0)
                      << ",focus_smooth=" << (g_camFocusConfig.smoothOmega > 0.0f ? 1 : 0)
                      << ",target_recorded=" << (g_camTargetRecorded ? 1 : 0) << ",smooth_reframe=" << (g_camSmoothReframe ? 1 : 0)
                      << ",prefetch=" << (g_camPrefetchOn ? 1 : 0) << ",prefetch_m=" << g_camPrefetchMeters
                      << ",prefetch_lead_ms=" << g_camPrefetchLeadMs << '\n';
            // The free camera's options (the editor saves them back) and the world overlay.
            LoadFreeCamOptions(configPath.c_str());
            g_camOverlayEnabled = key("CameraOverlay", true);
            g_camOverlayWhilePlaying = key("CameraOverlayWhilePlaying", false);
            g_camOverlayTimeLabels = key("CameraOverlayTimeLabels", true);
            g_camOverlayTrail = key("CameraOverlayTrail", true);
            LogFreeCamConfig("config");
            LoadSplineDefaults(configPath.c_str());
            LogSplineDefaults("config");
            char release[16]{};
            replay::identity::DevIniString("ReplayCameraRelease", "paused", release, sizeof(release), configPath.c_str());
            g_camRelease = replay::camcut::ReleaseFromIni(release);
            LogFile() << "CAMERA_RELEASE,phase=config,mode=" << replay::camcut::ReleaseName(g_camRelease) << '\n';
            // The skeleton change re-armed in its tick (segment_switch.h) and the whole
            // metaped look (metaped_state.h).
            const bool switchInTick = devKey("SegmentSwitchInTick", true);
            replay::playback::SetSegmentSwitchInTick(switchInTick);
            // No hidden puppet while a skeleton no record has is tried again
            // (no_record_fallback.h).
            const bool noRecordMapNow = devKey("NoRecordMapNow", true);
            const bool noRecordAlt = devKey("NoRecordAltAppearance", false);
            replay::playback::SetNoRecordFallback(noRecordMapNow, noRecordAlt);
            const bool metapedRecord = devKey("MetapedRecord", true);
            const bool metapedApply = devKey("MetapedApply", true);
            replay::metaped::engine::SetEnabled(metapedRecord, metapedApply);
            LogFile() << "CONFIG_APPEARANCE,segment_switch_in_tick=" << (switchInTick ? 1 : 0)
                      << ",no_record_map_now=" << (noRecordMapNow ? 1 : 0) << ",no_record_alt_appearance=" << (noRecordAlt ? 1 : 0)
                      << ",metaped_record=" << (metapedRecord ? 1 : 0) << ",metaped_apply=" << (metapedApply ? 1 : 0) << '\n';
            LogFile() << "CONFIG_LOD_DOORS_CAMERA,puppet_lod_multiplier=" << lod.multiplier << ",puppet_always_prerender="
                      << (lod.prerender ? 1 : 0) << ",puppet_force_anim_update=" << (lod.forceUpdate ? 1 : 0)
                      << ",puppet_script_pose=" << (lod.scriptPose ? 1 : 0)
                      << ",door_rebind_ms=" << doors.rebindMs << ",door_keep_open_ratio=" << (doors.keepOpenRatio ? 1 : 0)
                      << ",map_objects_wait_ms=" << doors.mapWaitMs << ",replay_camera_cut_mode="
                      << replay::camcut::ModeName(g_camCutMode) << '\n';
        }
        const bool wholePedPool = g_recConfig.worldModel == replay::worldmodel::Mode::Gta;
        // The ped class capacity (8 to 120, default 56), read here once, before any
        // recording. With WorldModel=gta up to the pool (150), default 148.
        const int requestedPedCapacity = static_cast<int>(replay::identity::DevIniInt("PedCapacity",
            static_cast<int>(replay::capacity::DefaultPedCapacity(wholePedPool)), configPath.c_str()));
        replay::capacity::SetPedCapacity(replay::capacity::ClampPedCapacity(requestedPedCapacity, wholePedPool));
        // The vehicle class: 8, or the vehicle pool less 1 with WorldModel=gta.
        replay::capacity::SetVehicleCapacity(replay::capacity::VehicleCapacityFor(wholePedPool));
        // Spawned free objects.
        replay::objects::SetSpawnedObjects(
            replay::identity::DevIniInt("SpawnedObjects", 1, configPath.c_str()) != 0);
        // The object burn level track.
        replay::objects::SetObjectBurnLevel(
            replay::identity::IniInt("ObjectBurnLevel", 1, configPath.c_str()) != 0);
        // The still rule and the drawn check of posed objects.
        replay::objects::SetStillPose(
            replay::identity::DevIniInt("StillPoseEngine", 1, configPath.c_str()) != 0,
            replay::identity::DevIniInt("ObjectDrawnCheck", 0, configPath.c_str()) != 0);
        // The object custom texture and tint, the evidence path, and the stand-in to
        // prop handoff overlap.
        replay::objlook::SetRecordAndPlay(replay::identity::IniInt("ObjectLook", 1, configPath.c_str()) != 0);
        replay::objlook::SetEvidence(replay::identity::DevIniInt("ObjectLookEvidence", 0, configPath.c_str()) != 0);
        {
            const long handoff = static_cast<long>(replay::identity::DevIniInt("SceneryHandoffTicks", 1, configPath.c_str()));
            replay::objects::SetSceneryHandoffTicks(static_cast<std::uint32_t>(handoff < 0 ? 0 : handoff));
        }
        // Still spawned objects of an owner that is not the map's become late scenery.
        replay::objects::SetSpawnedStaticScenery(
            static_cast<int>(replay::identity::DevIniInt("SpawnedStaticScenery", 1, configPath.c_str())));
        // The puppets' own persistent ped audio sounds.
        replay::audio::SetRefusePuppetOwnPersistent(
            replay::identity::IniInt("RefusePuppetOwnPersistent", 1, configPath.c_str()) != 0);
        // That refusal by entity, call site and entry; replayed ped audio loops with a
        // recorded position follow their puppet.
        replay::audio::SetPuppetPersistentByEntry(
            replay::identity::DevIniInt("PuppetPersistentByEntry", 1, configPath.c_str()) != 0);
        replay::events::SetPedAudioLoopFollowsPuppet(
            replay::identity::DevIniInt("PedAudioLoopFollowsPuppet", 1, configPath.c_str()) != 0);
        // Short ped audio sounds keep their recorded length; the animal line retry.
        replay::events::SetPedAudioKeepRecordedLength(
            replay::identity::DevIniInt("PedAudioKeepRecordedLength", 1, configPath.c_str()) != 0);
        replay::events::SetAnimalVocalRetry(
            replay::identity::DevIniInt("AnimalVocalRetry", 1, configPath.c_str()) != 0);
        // Owners of the registered effects.
        replay::events::SetRegisteredFxOwnerCheck(
            replay::identity::DevIniInt("RegisteredFxOwnerCheck", 1, configPath.c_str()) != 0);
        // SoundCheck, SoundCensus and SoundCapture are read by the census itself
        // (sound_census.h); the report needs the clip's path.
        replay::census::SetClipPathSource(&CurrentClipPathForCensus);
        replay::prof::SetSlowFrameUs(1000.0 *
            std::clamp(static_cast<int>(replay::identity::DevIniInt("ProfileSlowTickMs", 50, configPath.c_str())), 5, 1000));
        replay::hud::SetObjectivesEnabled(g_debugMode);

        std::ofstream& log = LogFile();
        log << "\n=== Redemption Editor loaded ===\n";
        log << "debugMode=" << (g_debugMode ? 1 : 0) << "\n";
        log << keysLog;
        log << "RECORDING_CONFIG,buffer=" << (g_recConfig.buffer ? 1 : 0) << ",buffer_s=" << g_recConfig.bufferSeconds
            << ",buffer_mb=" << g_recConfig.bufferMB << ",manual_max_s=" << g_recConfig.manualMaxSeconds
            << ",manual_max_mb=" << g_recConfig.manualMaxMB
            << ",buffer_adaptive=" << (g_recConfig.bufferAdaptive ? 1 : 0) << ",buffer_max_mb=" << g_recConfig.bufferMaxMB
            << ",buffer_reserve_mb=" << g_recConfig.bufferReserveMB
            << ",clip_world_log=" << (g_clipStartConfig.log ? 1 : 0) << ",clip_world_wait=" << (g_clipStartConfig.worldWait ? 1 : 0)
            << ",clip_world_wait_ms=" << g_clipStartConfig.worldCapMs << ",clip_start_stagger=" << (g_clipStartConfig.stagger ? 1 : 0)
            << ",clip_start_batch=" << g_clipStartConfig.batch << ",clip_start_settle_ms=" << g_clipStartConfig.settleMinMs
            << ",clip_start_streaming_max=" << g_clipStartConfig.streamingMax
            << ",compress_clips=" << (g_recConfig.compressClips ? 1 : 0)
            << ",compress_after_days=" << g_recConfig.compressAfterDays << ",compress_level=" << g_recConfig.compressLevel
            << ",compress_format=" << (g_recConfig.compressFormat == 1 ? "zstd" : "packed")
            << ",record_cutscenes=" << (g_recConfig.recordCutscenes ? 1 : 0)
            << ",lock_cutscene_camera=" << (g_recConfig.lockCutsceneCamera ? 1 : 0)
            << ",restore_world_state=" << (g_recConfig.restoreWorldState ? 1 : 0)
            << ",restore_world_state_in_mission=" << (g_recConfig.restoreWorldStateInMission ? 1 : 0)
            << ",recording_indicator=" << replay::recindicator::IniValue(g_recConfig.recordingIndicator) << "\n";
        // The capture capacity of this session.
        log << "POSE_V2_CAPACITY,requested=" << requestedPedCapacity << ",ped_capacity=" << replay::capacity::PedCapacity()
            << ",vehicle_capacity=" << replay::capacity::VehicleCapacity()
            << ",live_targets=" << replay::capacity::LiveTargets(replay::capacity::PedCapacity(), replay::capacity::VehicleCapacity())
            << ",slots=" << replay::capacity::kLiveTargetSlots
            << ",override_slots=" << replay::pose::capture::kMaxOverrides
            << ",weapon_capacity=" << replay::capacity::WeaponCapacity(replay::capacity::PedCapacity())
            << ",pool_ped_budget=" << replay::capacity::PoolPedBudget(replay::capacity::PedCapacity())
            << ",pool_vehicle_budget=" << replay::capacity::PoolVehicleBudget(replay::capacity::VehicleCapacity()) << "\n";
        // The world model of this session.
        {
            char flags[16]{};
            std::snprintf(flags, sizeof(flags), "0x%X", g_recConfig.worldClearFlags);
            log << "WORLD_MODEL,phase=config,mode=" << (wholePedPool ? "gta" : "off")
                << ",reload_path=" << replay::worldmodel::ReloadPathName(g_recConfig.worldReloadPath)
                << ",step=" << g_recConfig.worldModelStep << ",clear_flags=" << flags << ",scripts_cleanup=" << (g_recConfig.worldScriptsCleanup ? 1 : 0)
                << ",ped_capacity_limit=" << replay::capacity::PedCapacityLimit(wholePedPool) << "\n";
            const replay::theatre::Config& theatre = g_recConfig.theatre;
            log << "THEATRE_CONFIG,log=" << theatre.log << ",perschar=" << theatre.perschar << ",vehicles=" << theatre.vehicles
                << ",peds=" << theatre.peds << ",pause_scripts=" << theatre.pauseScripts
                << ",pause_scripthook=" << theatre.pauseScriptHook << ",deny_spawn=" << theatre.denySpawn
                << ",emitters=" << theatre.emitters << ",hooks=" << theatre.hooks << ",speech_queue_any_context=" << replay::events::SpeechQueueAnyContext() << "\n";
        }
        log << "buildVersion=" << replay::build::kVersion << "\n";
        // Public (the player's keys only) or dev (REDEMPTION_DEV: the research probes
        // and every ini key).
        log << "CONFIG_BUILD," << replay::identity::kBuildFlavor
            << ",report_include_clips=" << (g_reportIncludeClips ? 1 : 0) << "\n";
        {
            // What was loaded and with which settings.
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, MAX_PATH);
            g_asiPath = modulePath;
            std::uint64_t asiBytes = 0;
            g_asiSha256 = FileSha256(g_asiPath, asiBytes);
            log << "ASI_IDENTITY,version=" << replay::build::kVersion << ",sha256=" << g_asiSha256 << ",bytes=" << asiBytes
                << ",path=" << Narrow(g_asiPath) << "\n";
            // Where a crash's minidump goes, and the ids of the named threads
            // (thread_names.h; the workers made later are logged when the map changes,
            // THREADS lines).
            log << "CRASH_DUMPS,folder=" << CrashDumpFolderUtf8() << ",dbghelp=" << (crashdump::g_writeDump ? 1 : 0)
                << ",dump_thread=" << crashdump::g_threadId << ",keep=" << crashdump::kKeepDumps << "\n";
            log << "THREADS,module=RedemptionEditor.asi," << replay::threads::MapText() << "\n";
            bool iniFound = false;
            const std::string iniKeys = IniSectionText(configPath, iniFound);
            g_iniKeysText = iniKeys;
            log << "CONFIG_INI,path=" << configPath << ",found=" << (iniFound ? 1 : 0) << ",debug=" << (g_debugMode ? 1 : 0)
                << ",replay_buffer=" << (g_recConfig.buffer ? 1 : 0)
                << ",pose_fidelity=" << (replay::identity::DevIniInt("PoseFidelity", 0, configPath.c_str()) != 0 ? 1 : 0)
                << ",keys=" << (iniKeys.empty() ? "none" : iniKeys) << "\n";
        }
        log << "gameVersion=" << static_cast<int>(getGameVersion()) << "\n";
        // ASI_GAME (the game build against the one the mod's offline addresses come
        // from) and the mark key.
        replay::qa::kit::Configure(configPath, log, static_cast<int>(getGameVersion()));
        log << "workingDirectory=" << workingDirectory << "\n";
        // Where clips, projects, reports, the log and the changed settings go.
        log << replay::storage::LogLine() << "\n";
        if (!replay::storage::Current().userIni.empty())
            log << "STORAGE_SETTINGS,game_ini=" << configPath << ",user_ini=" << replay::storage::Utf8(replay::storage::Current().userIni)
                << ",user_ini_found=" << (replay::storage::Exists(replay::storage::Current().userIni) ? 1 : 0) << "\n";
        log.flush();
    }

    // Transport state for the overlay: play, pause, or the scrub in course.
    const char* TransportLabel()
    {
        if (g_transport.Scrubbing())
        {
            const replay::transport::ScrubState& scrub = g_transport.Scrub();
            if (scrub.fine)
                return scrub.direction < 0 ? "STEP-" : "STEP+";
            return scrub.direction < 0 ? "REW2x" : "FWD2x";
        }
        if (g_transport.Paused())
            return "PAUSE";
        static char label[16];
        if (g_playbackSpeedIndex == kNormalSpeedIndex)
            return "PLAY ";
        std::snprintf(label, sizeof(label), "PLAY %d%%", static_cast<int>(kPlaybackSpeeds[g_playbackSpeedIndex] * 100.0 + 0.5));
        return label;
    }

    void DrawOverlay(const ProbeFrame& f)
    {
        REPLAY_PROFILE_ZONE("tick.overlay");
        char line[256]{};
        float y = 0.025f;

        DrawText(replay::build::kDisplayName, 0.018f, y, 0.38f, 235, 195, 95, 235);
        y += 0.028f;

        std::snprintf(line, sizeof(line), "Ped %d | Pos %.2f  %.2f  %.2f | FOV %.1f | Mount %d",
            f.playerPed, f.position.x, f.position.y, f.position.z, f.cameraFov,
            f.mount ? f.mount : f.mountTarget);
        DrawText(line, 0.018f, y, 0.30f, 245, 245, 245, 225);
        y += 0.023f;

        std::snprintf(line, sizeof(line), "Recorded %u entities | puppets %u | timeline %.2f s",
            static_cast<unsigned int>(PoseV2RecordedCount()),
            static_cast<unsigned int>(PoseV2PuppetCount()),
            static_cast<double>(PlaybackDurationMs()) / 1000.0);
        DrawText(line, 0.018f, y, 0.30f, 245, 245, 245, 225);
        y += 0.023f;

        // Profiler: script tick p95 and the three largest zones of the last
        // report window (PROFILE_* lines in the log every 5 s).
        {
            const std::string perf = replay::prof::LastWindowLine();
            if (!perf.empty())
            {
                DrawText(perf.c_str(), 0.018f, y, 0.28f, 180, 230, 180, 225);
                y += 0.023f;
            }
        }
        y += 0.004f;

        DrawText(replay::keys::Fill("{key_editor} EDITOR | {key_record} REC | {key_stop} PLAY | {key_play_pause} pause | {key_frame_back} "
                                    "{key_frame_forward} scrub",
                     replay::keys::Current())
                     .c_str(),
            0.018f, y, 0.28f, 180, 190, 200, 210);
    }

    void DrawStatus(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.status");
        char text[224]{};

        if (g_recording)
        {
            const double elapsedSeconds = static_cast<double>(
                now >= g_recordingStartedAt
                    ? now - g_recordingStartedAt
                    : 0) / 1000.0;
            if (g_recordKind == RecordKind::Buffer)
            {
                const std::uint32_t nowMs = replay::pose::capture::NowMs();
                std::snprintf(text, sizeof(text), "BUFFER %us/%us%s | %u entities",
                    (nowMs - g_bufferHeadMs) / 1000u, g_recConfig.bufferSeconds, g_manualInBuffer ? " | REC" : "",
                    static_cast<unsigned int>(PoseV2RecordedCount()));
            }
            else
                std::snprintf(text, sizeof(text), "REC %05.2fs | %u entities", elapsedSeconds,
                    static_cast<unsigned int>(PoseV2RecordedCount()));
            DrawText(text, 0.650f, 0.035f, 0.34f, 240, 45, 45, 245);
        }
        else if (g_transport.Playing())
        {
            const double elapsedSeconds =
                static_cast<double>(g_transport.PlayheadMs()) / 1000.0;
            const double durationSeconds =
                static_cast<double>(PlaybackDurationMs()) / 1000.0;
            std::snprintf(
                text,
                sizeof(text),
                "%s %05.2f/%05.2fs | %u puppets",
                TransportLabel(),
                elapsedSeconds,
                durationSeconds,
                static_cast<unsigned int>(PoseV2PuppetCount()));
            DrawText(
                text,
                0.580f,
                0.035f,
                0.34f,
                g_transport.Paused() ? 245 : 80,
                g_transport.Paused() ? 195 : 210,
                g_transport.Paused() ? 75 : 245,
                245);

            {
                char cameraText[192]{};
                const std::string speedKeys = replay::keys::Fill("{key_speed_down} {key_speed_up}", replay::keys::Current());
                std::snprintf(cameraText, sizeof(cameraText), "camera: %u markers, track %s | Ins marker | Del delete | End on/off | %s speed",
                    static_cast<unsigned>(g_cameraTrack.Keyframes().size()), g_cameraTrackEnabled && !g_cameraTrackManualOverride ? "on" : "free",
                    speedKeys.c_str());
                DrawText(cameraText, 0.620f, 0.092f, 0.25f, 200, 220, 240, 230);
            }
            DrawText(
                replay::keys::Fill("{key_stop} stop | {key_play_pause} pause | {key_frame_back} {key_frame_forward} scrub (Shift fine) | "
                                   "{key_free_cam} freecam",
                    replay::keys::Current())
                    .c_str(),
                0.620f,
                0.066f,
                0.27f,
                195,
                205,
                215,
                225);
            if (g_freeCamEnabled)
            {
                DrawText(
                    "FREECAM mouse | WASD/QE | Shift fast | PgUp/PgDn FOV",
                    0.600f,
                    0.092f,
                    0.28f,
                    90,
                    220,
                    250,
                    235);
            }
        }
        else if (g_overlayEnabled)
        {
            DrawText(
                replay::keys::Fill("READY - {key_record} record | {key_stop} play | {key_play_pause} pause | {key_frame_back} {key_frame_forward} "
                                   "scrub | {key_free_cam} free camera | F9 overlay",
                    replay::keys::Current())
                    .c_str(),
                0.625f,
                0.035f,
                0.30f,
                135,
                225,
                155,
                225);
        }
    }
    void StopPlayback();
    bool StartPlayback(ULONGLONG now);
    void StartRecording(RecordKind kind = RecordKind::Manual);
    void StopRecording(bool save = true);
    extern std::uint32_t g_editingMarker;
    // Edit Camera's view is resolved again once the puppets stand at the marker's
    // time (UpdateFreeCam).
    bool g_editResolvePending = false;
    bool g_freeCamBeforeEdit = false; // restored when Edit Camera ends
    void LeaveMarkerCameraEdit(bool keep);
    extern bool g_trimReady;
    void ApplyMarkerEffects(bool active);
    void ApplyAbilityEffect(bool playing);
    void StopOwnScreenEffects(const char* why);

    // PoseTrack v2 entity registry, recording side. Every recorded entity gets a
    // ReplayId (slot + instance). Its create and delete packets go to the
    // lifecycle track, which opens every block with a full create of the live
    // entities, and its pose record in replay::pose::capture is keyed by the same
    // id. Entities enter and leave by the proximity scan below, and also leave when
    // they stop existing or their skeleton is replaced (a new entity for the replay).
    constexpr std::uint32_t kPoseV2SampleHz = replay::recorder::kSampleHz;

    // The class capacities are replay::capacity::PedCapacity() (ini PedCapacity)
    // and replay::capacity::VehicleCapacity() (8, or the vehicle pool less 1
    // with WorldModel=gta).

    // The clip in memory (src/replay_recording.h): the recorder fills it,
    // playback reads it.
    replay::Recording g_clip;
    // The recorder's registry (src/replay_recorder.h): engine entities
    // being recorded and what the recorder remembers about them.
    replay::recorder::Recorder g_recorder;
    // The puppet registry (src/replay_puppets.h) and the context the
    // subsystem modules work on.
    replay::playback::Puppets g_puppets;
    replay::Context g_context{g_clip, g_recorder, g_puppets, g_transport, LogFile()};
    std::size_t PoseV2PuppetCount() { return g_puppets.Count(); }
    std::size_t PoseV2RecordedCount() { return g_recorder.entities.size(); }


    // Clips on disk (src/clip_file.h): every recording is saved to the
    // ReplayClips folder when it stops; F8 loads the clips of the folder in turn, newest first. The
    // file carries no session pointer; image offsets are rebased with this module.
    const wchar_t* kClipFolder = L"ReplayClips";
    std::vector<replay::editor::ClipInfo> g_editorClips;
    // The list newest first as read (g_editorClips is it sorted for the gallery; a
    // sort change re-sorts this one, no disk read).
    std::vector<replay::editor::ClipInfo> g_editorClipsByDate;
    // The clip loads, buffer plays and recordings (the editor tells another clip
    // from a rename or a first save of the same one).
    std::uint32_t g_clipLoadSequence = 0;
    // A gallery load waiting for the save of the clip being left (no freeze in
    // FinishPendingSave while it is written).
    std::wstring g_pendingLoadPath;
    // The editor's window over the sorted list (the snapshot holds 64 clips; the
    // folder holds hundreds) and its sort.
    std::uint32_t g_clipWindowFirst = 0;
    std::uint32_t g_clipSort = 0; // 0 newest first, 1 longest first
    std::uint64_t g_editorClipListSequence = 0;
    std::wstring g_currentClipPath;
    // The sound report goes next to the open clip (sound_census.h).
    std::wstring CurrentClipPathForCensus() { return g_currentClipPath; }
    // The file that holds the clip in memory (loaded, or written from it), empty
    // for a buffer window played before it was saved: the editor's save writes
    // there (g_currentClipPath also names the last buffer window saved, another
    // clip). g_clipEdited: markers changed since.
    std::wstring g_memoryClipPath;
    bool g_clipEdited = false;
    ULONGLONG g_loadFailedUntil = 0; // SnapshotLoadFailed
    ULONGLONG g_renameFailedUntil = 0; // SnapshotRenameFailed
    // The markers before each change (Ctrl+Z) and the changes undone (Ctrl+Y). A
    // run of the same change on the same marker within a second (a drag, a held
    // arrow) is one step.
    struct MarkerHistory
    {
        std::vector<replay::camera::Track> undo;
        std::vector<replay::camera::Track> redo;
        std::uint32_t lastType = 0;
        std::uint32_t lastMarker = 0;
        ULONGLONG lastAt = 0;
    };
    MarkerHistory g_markerHistory;
    constexpr std::size_t kMarkerHistoryDepth = 64;

    // `before`, the markers as they were before the change (the caller changed
    // them and compared: only a real change is a step).
    void PushMarkerHistory(std::uint32_t type, std::uint32_t marker, const replay::camera::Track* before = nullptr)
    {
        MarkerHistory& history = g_markerHistory;
        const ULONGLONG now = GetTickCount64();
        // Only a drag (moves) and a held arrow (one option) merge: two adds or
        // two deletes are two steps.
        const bool mergeable = type == static_cast<std::uint32_t>(replay::editor::CommandType::MoveCameraKeyframe) || (type & 0x10000u);
        const bool run = mergeable && type == history.lastType && marker == history.lastMarker && now - history.lastAt < 1000 &&
            !history.undo.empty();
        history.lastType = type;
        history.lastMarker = marker;
        history.lastAt = now;
        if (run)
            return;
        history.undo.push_back(before ? *before : g_cameraTrack);
        if (history.undo.size() > kMarkerHistoryDepth)
            history.undo.erase(history.undo.begin());
        history.redo.clear();
    }
    void RebuildEditorClipList();
    void RecordEnvironment(std::uint32_t nowMs, bool force);
    bool SavePending();
    void FinishPendingSave(bool wait);
    bool CopyQueuedClip(const std::wstring& path, replay::Recording& recording, std::vector<replay::pose::PoseRecordExport>& poses,
        replay::camera::Track& camera, std::uint64_t& durationMs, std::string& error);
    std::size_t HeldClipSaves(); // saves that failed and wait for a retry
    // All the clips of the folder, as on disk and uncompressed.
    std::uint64_t g_clipsDiskBytes = 0;
    std::uint64_t g_clipsBytes = 0;
    std::uint32_t g_compressedClips = 0;

    void ImageRange(std::uintptr_t& base, std::size_t& size)
    {
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
        replay::clipfile::SetBuildIdentity(nt->FileHeader.TimeDateStamp, size);
    }

    // A clip file of the base folder (where new clips go).
    std::wstring ClipPath(const std::wstring& name)
    {
        return replay::storage::Folder(kClipFolder) + L"\\" + name + L".rpclip";
    }

    // Where the clip file `fileName` ("042-Valentine.rpclip") is: the base
    // folder, else the game folder (clips of an earlier start), else the base
    // path a new one would take.
    std::wstring FindClipFile(const std::wstring& fileName) { return replay::storage::Find(std::wstring(kClipFolder) + L"\\" + fileName); }

    // A clip opened from a folder that takes no writes (a Program Files game
    // folder) is saved to the base folder under the same name.
    std::wstring SavableClipPath(const std::wstring& path)
    {
        if (replay::storage::Current().gameWritable)
            return path;
        replay::storage::Folder(kClipFolder);
        return replay::storage::WritablePath(path);
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (const wchar_t c : text)
            out += c < 128 ? static_cast<char>(c) : '?';
        return out;
    }

    // The last clip written, for the editor's thumbnail.
    std::string g_savedClipName;
    std::string g_lastWrittenClipName; // any save that reached the disk
    std::uint64_t g_savedClipSequence = 0;

    bool WriteClipFile(const std::wstring& name, std::string& error)
    {
        if (g_recording)
        {
            error = "recording";
            return false;
        }
        if (g_transport.DurationMs() == 0)
        {
            error = "no_clip";
            return false;
        }
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        const std::wstring path = ClipPath(name);
        const ULONGLONG startedAt = GetTickCount64();
        // A legacy shake still waiting is folded first (a save writes only the
        // markers' shake).
        if (g_cameraTrack.HasLegacyShake())
            replay::camera::mshake::Migrate(g_cameraTrack);
        const bool ok = replay::clipfile::Save(g_clip, replay::pose::capture::ExportRecords(), g_cameraTrack,
            g_transport.DurationMs(), base, size, path, error);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const bool sized = ok && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info);
        if (ok)
        {
            g_currentClipPath = path;
            g_memoryClipPath = path;
            g_clipEdited = false;
            g_savedClipMs = static_cast<std::uint32_t>(g_transport.DurationMs());
            g_savedClipName = Narrow(path.substr(path.find_last_of(L'\\') + 1));
            ++g_savedClipSequence;
            RebuildEditorClipList();
        }
        LogFile() << "CLIP_SAVE,path=" << Narrow(path) << ",ok=" << ok << ",bytes="
                  << (sized ? (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow : 0)
                  << ",ms=" << (GetTickCount64() - startedAt) << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        return ok;
    }

    // A gallery load's file read, decompressed and decoded on a thread (the script
    // thread would freeze for the whole load of a large clip). The script thread
    // then imports the poses and publishes the clip (ReadClipFile with it):
    // ImportRecords stays on this thread.
    struct ClipRead
    {
        std::wstring path;
        std::thread worker;
        std::atomic<bool> done{false};
        bool ok = false;
        std::string error;
        replay::Recording recording;
        std::vector<replay::pose::PoseRecordExport> poses;
        replay::camera::Track camera;
        std::uint64_t durationMs = 0;
        replay::clipfile::LoadInfo info;
        ULONGLONG startedAt = 0;
        // Ctrl+R never runs ScriptMain's exit branch: a finished read not
        // taken yet is destroyed with the module's globals, and a joinable
        // std::thread there ends the process. The thread pins the module
        // while it runs, so by then it has ended.
        ~ClipRead()
        {
            if (worker.joinable())
                worker.detach();
        }
    };
    std::unique_ptr<ClipRead> g_clipRead;
    // The next clip of a project playing as a whole, read ahead on a thread while
    // the one before it plays. Handed to g_clipRead when its turn comes; until
    // then the compressor holds as for any read.
    std::unique_ptr<ClipRead> g_projectPreload;
    // Read aheads not wanted any more, kept until their thread ends (joining one in
    // flight would hold the script thread for the whole read).
    std::vector<std::unique_ptr<ClipRead>> g_projectPreloadDropped;
    bool ClipReadPending() { return g_clipRead != nullptr || g_projectPreload != nullptr || !g_projectPreloadDropped.empty(); }

    // The read itself, for a load (openClip: the compressor stays off that file
    // from now on) or a project preload (the clip open now stays the compressor's
    // open clip until the preloaded one is published).
    std::unique_ptr<ClipRead> BeginClipRead(const std::wstring& path, bool openClip)
    {
        auto read = std::make_unique<ClipRead>();
        read->path = path;
        read->startedAt = GetTickCount64();
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        // The compressor stays off this file and off the disk meanwhile
        // (ServiceClipCompressor keeps the hold while the read runs).
        if (openClip)
            replay::clipcompress::worker::SetOpenClip(path);
        replay::clipcompress::worker::SetHold(true);
        ClipRead* raw = read.get();
        raw->worker = std::thread([raw, base, size]() {
            replay::threads::Scope threadName("clip_read");
            // Load reports bad_alloc itself; anything else leaving the thread would be
            // std::terminate (the game closes).
            try
            {
                raw->ok = replay::clipfile::Load(raw->path, base, size, raw->recording, raw->poses, raw->camera,
                    raw->durationMs, raw->error, &raw->info);
            }
            catch (const std::exception& exception)
            {
                raw->ok = false;
                raw->error = std::string("exception_") + exception.what();
            }
            catch (...)
            {
                raw->ok = false;
                raw->error = "unknown_exception";
            }
            raw->done.store(true);
        });
        LogFile() << "CLIP_READ_START,path=" << Narrow(path) << (openClip ? "" : ",preload=1") << '\n';
        return read;
    }

    void StartClipRead(const std::wstring& path)
    {
        g_clipRead = BeginClipRead(path, true);
    }

    // The read in flight, waited for (unload, or a read no longer wanted).
    void CancelClipRead()
    {
        if (!g_clipRead)
            return;
        g_clipRead->worker.join();
        g_clipRead.reset();
    }

    // `preloaded` (may be null): that file already read on a thread
    // (StartClipRead); used when it is this path.
    bool ReadClipFile(const std::wstring& path, std::string& error, ClipRead* preloaded = nullptr)
    {
        FinishPendingSave(true); // a clip being written is read once it is whole
        if (g_recording && g_recordKind == RecordKind::Buffer)
            StopRecording(false); // the buffer gives way to the clip; it resumes after
        if (g_recording)
        {
            error = "recording";
            return false;
        }
        if (g_transport.Playing())
            StopPlayback();
        // After the stop: leaving the open clip starts its autosave, and this may be
        // that same file.
        FinishPendingSave(true);
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        const ULONGLONG startedAt = preloaded ? preloaded->startedAt : GetTickCount64();
        replay::Recording loaded;
        std::vector<replay::pose::PoseRecordExport> poses;
        replay::camera::Track camera;
        std::uint64_t durationMs = 0;
        // The background compressor stays off this file (and off the disk while it is
        // read); a compressed clip is decompressed in memory by Load.
        replay::clipcompress::worker::SetOpenClip(path);
        replay::clipcompress::worker::SetHold(true);
        replay::clipfile::LoadInfo loadInfo;
        // A file with a save not written yet (held after a failure, or queued) opens
        // from that save's snapshot: it is newer than the file.
        bool fromQueue = false;
        bool ok = false;
        if (CopyQueuedClip(path, loaded, poses, camera, durationMs, error))
            ok = fromQueue = true;
        else if (error.empty() && preloaded && preloaded->path == path)
        {
            // Read on a thread.
            ok = preloaded->ok;
            error = preloaded->error;
            loadInfo = preloaded->info;
            loaded = std::move(preloaded->recording);
            poses = std::move(preloaded->poses);
            camera = std::move(preloaded->camera);
            durationMs = preloaded->durationMs;
        }
        else if (error.empty())
            ok = replay::clipfile::Load(path, base, size, loaded, poses, camera, durationMs, error, &loadInfo);
        ok = ok && replay::pose::capture::ImportRecords(poses, error);
        replay::clipcompress::worker::SetHold(SavePending() || ClipReadPending());
        if (loadInfo.compressed)
            LogFile() << "CLIP_DECOMPRESS,ms=" << loadInfo.decompressMs << ",bytes=" << loadInfo.clipBytes
                      << ",file_bytes=" << loadInfo.fileBytes << ",format=" << (loadInfo.packed ? "packed" : "zstd")
                      << ",path=" << Narrow(path) << '\n';
        if (ok)
        {
            // The age rule reads the last access; NTFS may not keep it.
            if (!fromQueue)
                replay::clipcompress::StampAccess(path);
            g_currentClipPath = path;
            g_memoryClipPath = path;
            g_clipEdited = false;
            g_markerHistory = {};
            ++g_clipLoadSequence;
            g_clip = std::move(loaded);
            g_clip.fromDisk = true;
            g_cameraTrack = std::move(camera);
            g_cameraTrackEnabled = true;
            g_cameraTrackManualOverride = g_cameraTrack.Empty();
            g_transport.SetDurationMs(durationMs);
            g_loadedClipHoldsBuffer = true;
        }
        // The optional sections the reader left out (the clip opened without them).
        for (const replay::clipfile::SkippedSection& section : loadInfo.skipped)
            LogFile() << "CLIP_SECTION_SKIPPED,section=" << section.section << ",reason=" << section.reason
                      << ",path=" << Narrow(path) << '\n';
        LogFile() << "CLIP_LOAD,path=" << Narrow(path) << ",ok=" << ok << ",duration_ms=" << durationMs
                  << ",entities=" << (ok ? g_clip.created.size() : 0) << ",poses=" << poses.size()
                  << ",ms=" << (GetTickCount64() - startedAt) << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        // A buffer window saved while the recording ran, its environment flag set
        // from its track at the load (clip_file.cpp).
        if (ok && g_clip.environment.repairedOnLoad)
            LogFile() << "CLIP_ENV_REPAIRED,path=" << Narrow(path) << ",clock_start_s=" << g_clip.environment.clockStartS
                      << ",clock_end_s=" << g_clip.environment.clockEndS << ",env_samples=" << g_clip.environment.track.size()
                      << '\n';
        return ok;
    }

    // The clip files of the folder, newest first (by the file's write time;
    // names typed by the player carry no date).
    // Every read root's clips (storage_paths.h), a name in two of them once.
    std::vector<std::wstring> ListClipFiles()
    {
        std::vector<std::pair<std::uint64_t, std::wstring>> found;
        std::vector<std::wstring> folders;
        for (const replay::storage::Entry& entry : replay::storage::List(kClipFolder, L"*.rpclip", &folders))
            found.emplace_back(entry.stamp, folders[entry.root] + L"\\" + entry.name);
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::wstring> names;
        for (auto& item : found)
            names.push_back(std::move(item.second));
        return names;
    }

    // Clip list for the editor gallery: rebuilt on request and after a save, a
    // load or a delete; only the file header is read.

    // Weather by name: the game's weather type names, hashed at run time and
    // compared with the recorded hash.
    std::string WeatherName(std::uint32_t hash)
    {
        static const std::pair<const char*, const char*> kNames[] = {
            {"SUNNY", "Sunny"}, {"HIGHPRESSURE", "Clear"}, {"CLOUDS", "Cloudy"}, {"OVERCAST", "Overcast"},
            {"OVERCASTDARK", "Dark overcast"}, {"RAIN", "Rain"}, {"THUNDERSTORM", "Thunderstorm"}, {"THUNDER", "Thunder"},
            {"DRIZZLE", "Drizzle"}, {"SHOWER", "Showers"}, {"FOG", "Fog"}, {"MISTY", "Mist"}, {"SLEET", "Sleet"},
            {"HAIL", "Hail"}, {"SNOW", "Snow"}, {"SNOWLIGHT", "Light snow"}, {"SNOWCLEARING", "Snow clearing"},
            {"BLIZZARD", "Blizzard"}, {"GROUNDBLIZZARD", "Ground blizzard"}, {"WHITEOUT", "Whiteout"},
            {"HURRICANE", "Hurricane"}, {"SANDSTORM", "Sandstorm"}};
        for (const auto& [key, label] : kNames)
        {
            if (static_cast<std::uint32_t>(MISC::GET_HASH_KEY(key)) == hash)
                return label;
        }
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", hash);
        return text;
    }

    // A zone's name for people: the community name whose hash is the zone's
    // (computed with GET_HASH_KEY), map text prefixes dropped
    // ("P_4_THIEVES_LANDING" -> "Thieves Landing", "HennigansStead" ->
    // "Hennigans Stead"). Empty when the zone is not in the table. Hashed once
    // (285 names), then looked up.
    const char* FindZoneName(std::uint32_t hash)
    {
        static std::unordered_map<std::uint32_t, const char*> table;
        if (table.empty())
        {
            const auto add = [](const auto& names) {
                for (const char* name : names)
                    table.emplace(static_cast<std::uint32_t>(MISC::GET_HASH_KEY(name)), name);
            };
            add(replay::zones::kPrinted);
            add(replay::zones::kWritten);
            add(replay::zones::kTowns);
            add(replay::zones::kDistricts);
            add(replay::zones::kStates);
        }
        const auto found = table.find(hash);
        return found == table.end() ? nullptr : found->second;
    }

    std::string ZoneDisplayName(std::uint32_t hash)
    {
        if (!hash)
            return {};
        const char* name = FindZoneName(hash);
        if (!name)
            return {};
        std::string text(name);
        // Map text names carry a "P_4_" or "W_4_" style prefix.
        if (text.size() > 4 && (text[0] == 'P' || text[0] == 'p' || text[0] == 'W' || text[0] == 'w') && text[1] == '_')
        {
            const std::size_t second = text.find('_', 2);
            if (second != std::string::npos)
                text = text.substr(second + 1);
        }
        std::string out;
        bool newWord = true;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '_')
            {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
                newWord = true;
                continue;
            }
            const bool upper = c >= 'A' && c <= 'Z';
            const bool prevLower = i > 0 && text[i - 1] >= 'a' && text[i - 1] <= 'z';
            if (upper && prevLower && !out.empty() && out.back() != ' ')
            {
                out += ' ';
                newWord = true;
            }
            if (newWord && c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            else if (!newWord && upper)
                c = static_cast<char>(c - 'A' + 'a');
            out += c;
            newWord = false;
        }
        return out;
    }

    // The zone part, also sampled over a running buffer (g_bufferPlaces) so a
    // saved window is named after where it starts.
    bool ReadPlaceZones(replay::Place& place)
    {
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return false;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        // The zone hash is not a text label (GET_STRING_FROM_HASH_KEY gives empty
        // text): the name comes from the zone table.
        const auto zoneText = [&](int type, std::uint32_t& hash, char* out, std::size_t size) {
            hash = static_cast<std::uint32_t>(ZONE::_GET_MAP_ZONE_AT_COORDS(at.x, at.y, at.z, type));
            std::snprintf(out, size, "%s", ZoneDisplayName(hash).c_str());
        };
        // The place as the game's HUD names it: printed map text (a settlement
        // like Thieves Landing), else written map text, else the town.
        zoneText(11, place.zoneTown, place.town, sizeof(place.town));
        if (!place.zoneTown)
            zoneText(12, place.zoneTown, place.town, sizeof(place.town));
        if (!place.zoneTown)
            zoneText(1, place.zoneTown, place.town, sizeof(place.town));
        zoneText(10, place.zoneDistrict, place.district, sizeof(place.district));
        zoneText(0, place.zoneState, place.state, sizeof(place.state));
        place.recorded = true;
        return true;
    }

    // Where and who at the recording's start: the map zones at the player with the
    // game's own text for them, the character, and the weather by name.
    void RecordPlace()
    {
        replay::Place& place = g_clip.place;
        place = replay::Place{};
        if (!ReadPlaceZones(place))
        {
            place = replay::Place{};
            return;
        }
        const Ped player = PLAYER::PLAYER_PED_ID();
        place.playerModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(player));
        const char* character = place.playerModel == static_cast<std::uint32_t>(MISC::GET_HASH_KEY("player_zero")) ? "Arthur"
            : place.playerModel == static_cast<std::uint32_t>(MISC::GET_HASH_KEY("player_three")) ? "John" : "Other";
        std::snprintf(place.character, sizeof(place.character), "%s", character);
        const replay::Environment& environment = g_clip.environment;
        const std::string weather = WeatherName(environment.weatherMix >= 0.5f ? environment.weatherTo : environment.weatherFrom);
        std::snprintf(place.weather, sizeof(place.weather), "%s", weather.c_str());
        place.recorded = true;
        LogFile() << "CLIP_PLACE,town=0x" << std::hex << place.zoneTown << ":" << place.town << ",district=0x" << place.zoneDistrict
                  << ":" << place.district << ",state=0x" << place.zoneState << ":" << place.state << std::dec
                  << ",character=" << place.character << ",weather=" << place.weather << '\n';
    }

    // The gallery's details of a clip, read from its sections without loading it:
    // the environment and place sections of the clip's summary
    // (clipcompress::ReadHead; for a plain file the section payloads are skipped
    // on disk, a compressed one carries its summary).
    void ReadClipListDetails(const std::vector<std::uint8_t>& summary, replay::editor::ClipInfo& info)
    {
        std::uint32_t environmentTag = 0, placeTag = 0, worldInfoTag = 0;
        for (const replay::clipfile::SectionInfo& section : replay::clipfile::Sections())
        {
            if (std::strcmp(section.name, "environment") == 0)
                environmentTag = section.tag;
            if (std::strcmp(section.name, "place") == 0)
                placeTag = section.tag;
            if (std::strcmp(section.name, "world info") == 0)
                worldInfoTag = section.tag;
        }
        info.chapter = replay::world::kChapterUnknown;
        info.camp = replay::world::kCampUnknown;
        std::size_t at = 20; // the clip header
        for (int guard = 0; guard < 64 && at + 14 <= summary.size(); ++guard)
        {
            const std::uint8_t* header = summary.data() + at;
            std::uint32_t tag = 0;
            std::uint64_t length = 0;
            std::memcpy(&tag, header, 4);
            std::memcpy(&length, header + 6, 8);
            if (length > 4096 || at + 14 + length + 8 > summary.size())
                return;
            const std::uint8_t* payload = header + 14;
            at += 14 + static_cast<std::size_t>(length) + 8;
            if (tag == environmentTag && length >= 21 && payload[0])
            {
                std::uint32_t from = 0, to = 0;
                float mix = 0.0f;
                std::memcpy(&info.clockStartS, payload + 1, 4);
                std::memcpy(&from, payload + 9, 4);
                std::memcpy(&to, payload + 13, 4);
                std::memcpy(&mix, payload + 17, 4);
                info.hasClock = 1;
                if (!info.weather[0])
                    std::snprintf(info.weather, sizeof(info.weather), "%s", WeatherName(mix >= 0.5f ? to : from).c_str());
            }
            // World info (WINF): u8 recorded, u8 camp, u8 chapter, ...
            if (tag == worldInfoTag && length >= 20 && payload[0])
            {
                info.camp = payload[1];
                info.chapter = payload[2] <= replay::world::kChapterEpilogue2 ? payload[2] : replay::world::kChapterUnknown;
            }
            if (tag == placeTag && length >= 1 + 16 + 48 * 3 + 16 + 24 && payload[0])
            {
                const char* town = reinterpret_cast<const char*>(payload + 17);
                const char* district = town + 48;
                const char* state = district + 48;
                std::uint32_t hashes[3] = {};
                std::memcpy(hashes, payload + 1, sizeof(hashes));
                std::string region;
                const char* parts[3] = {town, district, state};
                for (int n = 0; n < 3; ++n)
                {
                    // Older clips have the hash but no text.
                    std::string text(parts[n], strnlen(parts[n], 48));
                    if (text.empty())
                        text = ZoneDisplayName(hashes[n]);
                    if (!text.empty())
                        region += (region.empty() ? "" : ", ") + text;
                }
                std::snprintf(info.region, sizeof(info.region), "%s", region.c_str());
                std::snprintf(info.character, sizeof(info.character), "%.15s", reinterpret_cast<const char*>(payload + 17 + 48 * 3));
                std::snprintf(info.weather, sizeof(info.weather), "%.23s", reinterpret_cast<const char*>(payload + 17 + 48 * 3 + 16));
            }
        }
    }

    // The folder is read on a thread (the headers of hundreds of clips, on the
    // script thread, would stall the game each time the gallery opens and lose
    // the keys pressed meanwhile). The thread only reads files; the details
    // (weather and zone names need natives) and the sort are made on the script
    // thread when it is done.
    struct ClipListRead
    {
        std::thread worker;
        std::atomic<bool> done{false};
        std::vector<std::wstring> paths;
        std::vector<replay::clipcompress::FileHead> heads;
        std::vector<std::uint8_t> read;
        ULONGLONG startedAt = 0;
        std::string error; // an exception on the thread (the list is then empty)
    };
    std::unique_ptr<ClipListRead> g_clipListRead;
    bool g_clipListAgain = false; // asked again while a read ran (a save, a delete)
    bool ClipListLoading() { return g_clipListRead != nullptr; }

    // The sorted list for the gallery from the list as read.
    void ApplyClipSort()
    {
        g_editorClips = g_editorClipsByDate;
        if (g_clipSort == 1)
            std::stable_sort(g_editorClips.begin(), g_editorClips.end(),
                [](const replay::editor::ClipInfo& a, const replay::editor::ClipInfo& b) { return a.durationMs > b.durationMs; });
        for (std::size_t index = 0; index < g_editorClips.size(); ++index)
            g_editorClips[index].listIndex = static_cast<std::uint32_t>(index);
        ++g_editorClipListSequence;
    }

    void RebuildEditorClipList()
    {
        if (g_clipListRead)
        {
            g_clipListAgain = true;
            return;
        }
        g_clipListRead = std::make_unique<ClipListRead>();
        ClipListRead* read = g_clipListRead.get();
        read->startedAt = GetTickCount64();
        read->worker = std::thread([read]() {
            replay::threads::Scope threadName("clip_list_read");
            // An exception leaving a std::thread is std::terminate, which closes the game
            // (e.g. a bad_alloc with a large clip folder); it is reported instead.
            try
            {
                read->paths = ListClipFiles();
                read->heads.resize(read->paths.size());
                read->read.resize(read->paths.size());
                for (std::size_t n = 0; n < read->paths.size(); ++n)
                {
                    if (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) != 0)
                        break;
                    read->read[n] = replay::clipcompress::ReadHead(read->paths[n], read->heads[n]) ? 1 : 0;
                }
            }
            catch (const std::exception& exception)
            {
                read->error = exception.what();
            }
            catch (...)
            {
                read->error = "unknown_exception";
            }
            if (!read->error.empty())
            {
                read->paths.clear();
                read->heads.clear();
                read->read.clear();
            }
            read->done.store(true);
        });
    }

    // Each tick (and with `wait` on unload): the read list goes in.
    void ServiceClipListRead(bool wait)
    {
        if (!g_clipListRead || (!wait && !g_clipListRead->done.load()))
            return;
        g_clipListRead->worker.join();
        ClipListRead& read = *g_clipListRead;
        std::vector<replay::editor::ClipInfo> clips;
        std::uint64_t diskBytes = 0, clipBytes = 0;
        std::uint32_t compressedClips = 0;
        for (std::size_t n = 0; n < read.paths.size(); ++n)
        {
            const std::wstring& path = read.paths[n];
            const replay::clipcompress::FileHead& head = read.heads[n];
            replay::editor::ClipInfo info{};
            const std::string name = Narrow(path.substr(path.find_last_of(L'\\') + 1));
            std::snprintf(info.name, sizeof(info.name), "%s", name.c_str());
            info.bytes = head.diskBytes;
            info.clipBytes = head.clipBytes;
            info.compressed = head.compressed ? 1u : 0u;
            diskBytes += head.diskBytes;
            clipBytes += head.clipBytes ? head.clipBytes : head.diskBytes;
            compressedClips += head.compressed ? 1u : 0u;
            if (read.read[n] && head.summary.size() >= 20 &&
                std::memcmp(head.summary.data(), replay::clipfile::kMagic, sizeof(replay::clipfile::kMagic)) == 0)
            {
                std::memcpy(&info.formatVersion, head.summary.data() + 8, 4);
                std::memcpy(&info.durationMs, head.summary.data() + 12, 8);
                info.loadable = info.formatVersion == replay::clipfile::kFormatVersion ? 1u : 0u;
                if (info.loadable)
                    ReadClipListDetails(head.summary, info);
            }
            clips.push_back(info);
        }
        const ULONGLONG tookMs = GetTickCount64() - read.startedAt;
        const std::string listError = read.error.empty() ? std::string("none") : read.error;
        g_clipListRead.reset();
        g_editorClipsByDate = std::move(clips);
        g_clipsDiskBytes = diskBytes;
        g_clipsBytes = clipBytes;
        g_compressedClips = compressedClips;
        ApplyClipSort();
        LogFile() << "CLIP_LIST,clips=" << g_editorClipsByDate.size() << ",ms=" << tookMs
                  << ",error=" << listError << '\n';
        if (g_clipListAgain && !wait)
        {
            g_clipListAgain = false;
            RebuildEditorClipList();
        }
    }


    // Shift+F1 saves a report in the game folder: ReplayReports\report-YYYYMMDD-
    // HHMMSS\ with copies of the mod log, the crash log, the ScriptHook log and the
    // ini, and report.txt with the build and the last clip's name (qa_report.h).
    // The log is flushed on the script thread; the copies run on a std::thread
    // like the clip list reader (it pins the module, so Ctrl+R waits for it) and
    // the notice is shown here when it is done. The logs are open for writing
    // (this mod's stream, ScriptHook's), so they are read with every share flag:
    // CopyFile asks for read sharing only and fails on them.
    std::string ClipFileName(const std::wstring& path); // below, with the clip saves

    struct QaReportJob
    {
        std::thread worker;
        std::atomic<bool> done{false};
        std::wstring folder;     // ReplayReports\report-...
        std::string summary;     // report.txt, made on the script thread
        std::string copied;      // "RedemptionEditor.log:12.3MB;..." for the log
        std::uint64_t bytes = 0;
        int files = 0;
        std::string error;       // the first failure ("" none)
        ULONGLONG startedAt = 0;
        replay::qa::bundle::Input bundleInput;   // marks, clips, dumps, sysinfo, zip
        replay::qa::bundle::Result bundle;
    };
    std::unique_ptr<QaReportJob> g_qaReport;
    ULONGLONG g_qaReportWaitSince = 0; // Shift+F1 waits for an open mark
    bool g_qaReportWaitSaid = false;

    bool CopySharedFile(const std::wstring& from, const std::wstring& to, std::uint64_t& bytes, std::string& error)
    {
        bytes = 0;
        HANDLE source = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (source == INVALID_HANDLE_VALUE)
        {
            error = "open_" + std::to_string(GetLastError());
            return false;
        }
        HANDLE target = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (target == INVALID_HANDLE_VALUE)
        {
            error = "create_" + std::to_string(GetLastError());
            CloseHandle(source);
            return false;
        }
        std::vector<char> buffer(1 << 20);
        bool ok = true;
        DWORD read = 0;
        while (ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
        {
            DWORD wrote = 0;
            if (!WriteFile(target, buffer.data(), read, &wrote, nullptr) || wrote != read)
            {
                error = "write_" + std::to_string(GetLastError());
                ok = false;
                break;
            }
            bytes += read;
            if (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) != 0)
            {
                error = "unload";
                ok = false;
                break;
            }
        }
        CloseHandle(target);
        CloseHandle(source);
        return ok;
    }

    void StartQaReport()
    {
        if (g_qaReport)
        {
            replay::hud::ShowNotice(TR("hud.report_busy", "Redemption Editor: a report is still being saved."));
            return;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const std::string folderName = replay::qareport::FolderName(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        auto job = std::make_unique<QaReportJob>();
        job->startedAt = GetTickCount64();
        // In the base folder (storage_paths.h).
        job->folder = replay::storage::Path(replay::storage::kReports) + L"\\" + std::wstring(folderName.begin(), folderName.end());
        replay::qareport::Summary summary;
        summary.version = replay::build::kVersion;
        summary.asiSha256 = g_asiSha256;
        char time[32]{};
        std::snprintf(time, sizeof(time), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond);
        summary.time = time;
        summary.lastClip = g_lastWrittenClipName;
        summary.openClip = g_currentClipPath.empty() ? std::string() : ClipFileName(g_currentClipPath);
        summary.state = g_recording ? (g_recordKind == RecordKind::Buffer ? (g_manualInBuffer ? "recording (buffer, manual mark)" : "recording (buffer)")
                                                                            : "recording (manual)")
                                    : g_transport.Playing() ? "playing" : "idle";
        summary.iniKeys = g_iniKeysText;
        {
            // The marks of this session go in (the folders of older sessions too: the
            // report thread moves every one it finds and rewrites the "marks:" line from
            // them, so a report saved after a crash lists them).
            const std::vector<std::string> marks = replay::qa::kit::PendingMarkFolders();
            for (const std::string& mark : marks)
                summary.marks += (summary.marks.empty() ? "" : ";") + mark;
            if (!marks.empty())
                summary.marks = std::to_string(marks.size()) + " (" + summary.marks + ")";
            replay::qa::bundle::Input& input = job->bundleInput;
            input.reportFolder = job->folder;
            input.reportName = folderName;
            // The clips the player marked; the open clip only with ReportIncludeClips=1.
            summary.includeOpenClip = g_reportIncludeClips;
            input.clips = replay::qareport::ReportClips(replay::qa::kit::MarkedClips(), summary.openClip, g_reportIncludeClips);
            input.zipClip = !input.clips.empty() ? input.clips.front() : summary.lastClip;
            input.crashFolder = crashdump::g_folder;
            input.dataRoot = replay::storage::Current().base;
            input.readRoots = replay::storage::Current().roots;
            input.asiVersion = replay::build::kVersion;
            input.asiSha256 = g_asiSha256;
            input.asiPath = g_asiPath;
            input.scriptHookGameVersion = static_cast<int>(getGameVersion());
            input.cancel = &replay::qa::kit::CancelFlag();
            replay::qa::kit::MarksReported();
        }
        job->summary = replay::qareport::SummaryText(summary);
        job->bundleInput.reportText = job->summary; // the marks line is rewritten from the folders moved
        // The start line goes into the copied log.
        LogFile() << "REPORT_SAVE,phase=start,dir=" << Narrow(job->folder) << ",last_clip=" << (summary.lastClip.empty() ? "none" : summary.lastClip)
                  << ",state=" << summary.state << ",clips=" << job->bundleInput.clips.size()
                  << ",clips_policy=" << (g_reportIncludeClips ? "marked_and_open" : "marked") << '\n';
        LogFile().flush();
        QaReportJob* raw = job.get();
        raw->worker = std::thread([raw]() {
            replay::threads::Scope threadName("report_copy");
            // An exception leaving a std::thread is std::terminate.
            try
            {
                replay::storage::Folder(replay::storage::kReports);
                if (!CreateDirectoryW(raw->folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    raw->error = "mkdir_" + std::to_string(GetLastError());
                const replay::storage::Layout& storage = replay::storage::Current();
                std::string listing = "storage: " + replay::storage::Utf8(storage.base) + " (game_folder_writable=" +
                    (storage.gameWritable ? "1" : "0") + ", " + storage.reason + ")\r\n";
                // The game folder's settings, and the player's own file over them when the
                // game folder takes no writes (as RedemptionEditor.user.ini).
                const std::wstring userIni = storage.userIni;
                std::vector<const char*> files(std::begin(replay::qareport::kFiles), std::end(replay::qareport::kFiles));
                if (!userIni.empty())
                    files.push_back("RedemptionEditor.user.ini");
                for (const char* file : files)
                {
                    if (!raw->error.empty())
                        break;
                    const std::wstring name(file, file + std::strlen(file));
                    // The log and the crash log in the base folder; the ini, ScriptHook's and
                    // ReShade's logs in the game folder.
                    const bool userCopy = !userIni.empty() && name == L"RedemptionEditor.user.ini";
                    const std::wstring source = userCopy ? userIni
                        : name == replay::identity::kIniFileW ? storage.game + name
                                                              : replay::storage::Find(name);
                    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES)
                    {
                        listing += std::string(file) + ": missing\r\n";
                        continue;
                    }
                    std::uint64_t bytes = 0;
                    std::string error;
                    const bool ok = CopySharedFile(source, raw->folder + L"\\" + name, bytes, error);
                    char size[32]{};
                    std::snprintf(size, sizeof(size), "%.1fMB", static_cast<double>(bytes) / (1024.0 * 1024.0));
                    listing += std::string(file) + ": " + (ok ? size : "failed " + error) + "\r\n";
                    raw->copied += (raw->copied.empty() ? "" : ";") + std::string(file) + ":" + (ok ? size : "failed_" + error);
                    raw->bytes += bytes;
                    raw->files += ok ? 1 : 0;
                    if (!ok && error == "unload")
                        raw->error = error;
                }
                if (raw->error.empty())
                {
                    const std::string text = raw->summary + "\r\nfiles:\r\n" + listing;
                    raw->bundleInput.reportText = text; // the bundle rewrites its marks line from the folders it moves
                    HANDLE file = CreateFileW((raw->folder + L"\\report.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    DWORD wrote = 0;
                    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr))
                        raw->error = "report_txt_" + std::to_string(GetLastError());
                    if (file != INVALID_HANDLE_VALUE)
                        CloseHandle(file);
                }
                // Marks, clips, dumps, sysinfo and the zip.
                if (raw->error.empty())
                    replay::qa::bundle::Extend(raw->bundleInput, raw->bundle);
            }
            catch (const std::exception& exception)
            {
                raw->error = std::string("exception_") + exception.what();
            }
            catch (...)
            {
                raw->error = "unknown_exception";
            }
            raw->done.store(true);
        });
        g_qaReport = std::move(job);
    }

    // Each tick (and with `wait` on unload): the finished report is said.
    void ServiceQaReport(bool wait)
    {
        if (!g_qaReport || (!wait && !g_qaReport->done.load()))
            return;
        g_qaReport->worker.join();
        const std::unique_ptr<QaReportJob> job = std::move(g_qaReport);
        const bool ok = job->error.empty();
        char mb[24]{};
        std::snprintf(mb, sizeof(mb), "%.1f", static_cast<double>(job->bytes) / (1024.0 * 1024.0));
        const replay::qa::bundle::Result& bundle = job->bundle;
        LogFile() << "REPORT_SAVE,phase=done,ok=" << (ok ? 1 : 0) << ",dir=" << Narrow(job->folder) << ",files=" << job->files
                  << ",mb=" << mb << ",ms=" << (GetTickCount64() - job->startedAt) << ",copied=" << (job->copied.empty() ? "none" : job->copied)
                  << ",error=" << (ok ? "none" : job->error.c_str()) << ",marks=" << bundle.marks << ",debug_files=" << bundle.debugFiles << ",clips=" << bundle.clips
                  << ",clips_compressed=" << bundle.clipsCompressed << ",dumps=" << bundle.dumps
                  << ",zip=" << (bundle.zipped ? Narrow(bundle.zipPath) : std::string("none")) << ",zip_mb="
                  << replay::qa::MegabytesText(bundle.zipBytes) << ",zip_entries=" << bundle.zipEntries << ",zip_deflated=" << bundle.zipDeflated
                  << ",zlib=" << (bundle.zlib.empty() ? "none" : bundle.zlib) << ",zip_error=" << (bundle.zipError.empty() ? "none" : bundle.zipError)
                  << ",bundle_error=" << (bundle.error.empty() ? "none" : bundle.error) << ",game_exe=" << (bundle.gameExeVersion.empty() ? "?" : bundle.gameExeVersion)
                  << ",game_build_match=" << bundle.gameBuildMatches << '\n';
        if (ok && bundle.zipped)
            replay::hud::ShowNotice(TRF("hud.report_saved", "Report saved: {path} ({mb} MB)",
                {{"path", Narrow(bundle.zipPath)}, {"mb", replay::qa::MegabytesText(bundle.zipBytes)}}).c_str());
        else if (ok)
            replay::hud::ShowNotice(TRF("hud.report_saved_folder", "Report saved: {path} (folder; the zip failed: {error})",
                {{"path", Narrow(job->folder)}, {"error", bundle.zipError.empty() ? std::string("?") : bundle.zipError}}).c_str());
        else
            replay::hud::ShowNotice(TRF("hud.report_failed", "Redemption Editor: the report could not be saved ({error}).", {{"error", job->error}}).c_str());
    }

    // Clip picker: F8 opens the list of ReplayClips (newest first), each F8 moves
    // down, and 1.5 s without a press loads the one selected.
    bool g_clipPickerOpen = false;
    std::vector<std::wstring> g_clipPickerFiles;
    std::size_t g_clipPickerIndex = 0;
    ULONGLONG g_clipPickerLastPress = 0;
    constexpr ULONGLONG kClipPickerIdleMs = 1500;

    void PressClipPicker(ULONGLONG now)
    {
        if (!g_clipPickerOpen)
        {
            g_clipPickerFiles = ListClipFiles();
            if (g_clipPickerFiles.empty())
            {
                ShowObjective("Redemption Editor: no clips in ReplayClips");
                return;
            }
            g_clipPickerOpen = true;
            g_clipPickerIndex = 0;
        }
        else
            g_clipPickerIndex = (g_clipPickerIndex + 1) % g_clipPickerFiles.size();
        g_clipPickerLastPress = now;
        LogFile() << "CLIP_PICKER,press,index=" << g_clipPickerIndex << ",files=" << g_clipPickerFiles.size() << ",now=" << now << '\n';
    }

    void UpdateClipPicker(ULONGLONG now)
    {
        if (!g_clipPickerOpen)
            return;
        const std::size_t count = g_clipPickerFiles.size();
        const std::size_t first = g_clipPickerIndex >= 4 ? g_clipPickerIndex - 4 : 0;
        float y = 0.30f;
        DrawText("Clips (F8 next, release to load)", 0.018f, y, 0.32f, 235, 195, 95, 240);
        y += 0.030f;
        for (std::size_t n = first; n < count && n < first + 10; ++n)
        {
            const std::wstring& path = g_clipPickerFiles[n];
            const std::string name = Narrow(path.substr(path.find_last_of(L'\\') + 1));
            const bool selected = n == g_clipPickerIndex;
            DrawText(((selected ? "> " : "  ") + name).c_str(), 0.018f, y, 0.30f, selected ? 255 : 200, selected ? 235 : 200,
                selected ? 140 : 200, 235);
            y += 0.026f;
        }
        if (now - g_clipPickerLastPress < kClipPickerIdleMs)
            return;
        g_clipPickerOpen = false;
        const std::wstring path = g_clipPickerFiles[g_clipPickerIndex];
        std::string error;
        const bool ok = ReadClipFile(path, error);
        const std::string shown = Narrow(path.substr(path.find_last_of(L'\\') + 1));
        ShowObjective(ok ? ("Redemption Editor: clip loaded " + shown + " (" + KeyText(replay::keys::Action::Stop) + " plays)").c_str()
                         : ("Redemption Editor: could not load " + shown + ": " + error).c_str());
    }

    // The pose finalizer belongs to one mod at a time (the Pose Editor shares it):
    // false, with a notice, when the other has it.
    bool TakePoseFinalizer()
    {
        std::string error;
        if (replay::pose::capture::AcquireFinalizer(error))
            return true;
        LogFile() << "POSE_FINALIZER,refused=" << error << '\n';
        replay::hud::ShowNotice(TR("hud.pose_editor", "Redemption Editor: close the Pose Editor first (it is using the character poses)."));
        return false;
    }

    // Every detour of the mod installed at load with a single freeze
    // (MH_ApplyQueued): enabled one by one they would cost 60 to 160 ms of frozen
    // game each.
    void WarmUpHooks()
    {
        const std::uint64_t startedAt = replay::prof::Now();
        // The batch opened before the modules attached (fire and decal
        // install there).
        if (!replay::hooks::BatchOpen())
            replay::hooks::BeginBatch();
        replay::events::WarmUp();
        replay::objects::WarmUp();
        std::string vegetationError;
        const bool vegetation = replay::vegetation::Install(vegetationError);
        // The weather audio gate.
        std::string windError;
        const bool wind = replay::windreplay::Install(windError);
        // The category resolver and the speech duck driver.
        std::string catsError;
        const bool cats = replay::catstate::Install(catsError);
        std::string duckError;
        const bool duck = replay::speechduck::Install(duckError);
        std::string finalizerError;
        const bool finalizer = replay::pose::capture::WarmUpFinalizer(finalizerError);
        std::string vehicleError;
        const bool vehicle = replay::pose::capture::WarmUpVehiclePreRender(vehicleError);
        // The ped pre-render detour (rider follows mount); installed whatever
        // MountPreRender says (0 measures).
        std::string pedPreRenderError;
        const bool pedPreRender = replay::mount::Install(pedPreRenderError);
        // Evidence only, behind the ini flag.
        std::string waterError = "off";
        const bool water = g_waterShapeLog && replay::water::Install(waterError);
        // Closed theater: spawn funnel, mix scenes, speech rules (forward, record; deny only while owning).
        std::string theatreReport;
        replay::theatre::Install(g_recConfig.theatre.hooks, theatreReport);
        // The animal bank picker (animal_bank.h); it only forwards until a playback
        // publishes holds (AnimalBankHold=1).
        replay::theatre::InstallAnimalBank(theatreReport);
        int applied = 0;
        MH_STATUS status = MH_OK;
        const double freezeMs = replay::hooks::EndBatch(applied, status);
        g_lastWarmUpStatus = status; // SESSION_CHECK
        g_lastWarmUpApplied = applied;
        LogFile() << "HOOK_WARMUP,applied=" << applied << ",status=" << MH_StatusToString(status)
            << ",freeze_ms=" << freezeMs << ",total_ms=" << TicksMs(replay::prof::Now() - startedAt)
            << ",vegetation=" << (vegetation ? 1 : 0) << ",finalizer=" << (finalizer ? "held" : finalizerError.c_str())
            << ",wind_gate=" << (wind ? "installed" : windError.c_str())
            << ",category_resolver=" << (cats ? "installed" : catsError.c_str())
            << ",speech_duck=" << (duck ? "installed" : duckError.c_str())
            << ",vehicle_prerender=" << (vehicle ? "installed" : vehicleError.c_str())
            << ",ped_prerender=" << (pedPreRender ? "installed" : pedPreRenderError.c_str())
            << ",mount_prerender=" << (replay::mount::Enabled() ? 1 : 0)
            << ",water_shapes=" << (water ? "installed" : waterError.c_str())
            << ",theatre=" << theatreReport
            << '\n';
        LogFile() << replay::pools::Describe() << '\n';
        LogFile() << replay::entities::Describe() << '\n';
        LogFile() << replay::trains::Describe() << '\n';
        LogFile() << replay::vehdoors::Describe() << '\n';
    }

    // The Pose Editor asks for the pose finalizer through the shared slot;
    // it gets it while nothing is recorded or played, and the replay takes
    // it back when the Pose Editor lets it go.
    void ServicePoseFinalizer()
    {
        // The always-on buffer is not a reason to keep the Pose Editor out:
        // it stops (no file) and resumes once the Pose Editor let go.
        if (g_recording && g_recordKind == RecordKind::Buffer && replay::pose::capture::FinalizerWanted())
        {
            LogFile() << "BUFFER_PAUSE,reason=pose_editor\n";
            StopRecording(false);
            g_bufferResumeAt = GetTickCount64() + 3000;
        }
        const bool busy = g_recording || g_transport.Playing() || !g_context.puppets.list.empty();
        std::string error;
        switch (replay::pose::capture::ServiceFinalizer(busy, error))
        {
        case replay::pose::capture::FinalizerEvent::Yielded:
            LogFile() << "POSE_FINALIZER,event=yielded_to_pose_editor\n";
            break;
        case replay::pose::capture::FinalizerEvent::Reclaimed:
            LogFile() << "POSE_FINALIZER,event=reclaimed\n";
            break;
        case replay::pose::capture::FinalizerEvent::Failed:
            LogFile() << "POSE_FINALIZER,event=reclaim_failed,error=" << error << '\n';
            break;
        default:
            break;
        }
    }

    std::size_t RecordingBytes()
    {
        const std::size_t bytes = replay::pose::capture::TrackBytes() + replay::buffer::ApproxBytes(g_clip);
        g_bufferPeakBytes = (std::max)(g_bufferPeakBytes, bytes);
        return bytes;
    }

    std::size_t ApplyBufferHead(std::uint32_t headMs)
    {
        if (headMs > g_bufferHeadMs)
        {
            g_bufferTrimmedFx += replay::buffer::TrimBefore(g_clip, headMs).fxFrames;
            // The registered fx tracks the trim emptied go too (their count is
            // capped, entity::kMaxRegisteredFxTracks; the live keys keep theirs).
            g_bufferTrimmedFxTracks += replay::events::DropTrimmedRegisteredFx(replay::buffer::ExportableBaseMs(g_clip, headMs));
            // The pose chunks keep from the same base as the other tracks (the block a
            // saved window starts at).
            g_bufferTrimmedPose += replay::pose::capture::DropBefore(replay::buffer::ExportableBaseMs(g_clip, headMs));
            g_bufferHeadMs = headMs;
            replay::window::DropPlacesBefore(g_bufferPlaces, replay::buffer::ExportableBaseMs(g_clip, headMs));
        }
        return RecordingBytes();
    }

    bool SaveRecordingWindow(std::uint32_t fromMs, std::uint32_t toMs, const char* why);

    // A part saved at its limit while the manual recording goes on: said on
    // screen, not only in the log (testers took the split for a 1:30 cap).
    void ManualPartNotice(bool timeLimit)
    {
        const std::string part = std::to_string(g_manualPart);
        const std::string next = std::to_string(g_manualPart + 1);
        const std::string text = timeLimit
            ? TRF("hud.manual_part_time", "Redemption Editor: the recording reached its time limit, so part {part} is saved as its own clip "
                  "and the recording goes on in part {next}.", {{"part", part}, {"next", next}})
            : TRF("hud.manual_part_size", "Redemption Editor: the recording reached its size limit, so part {part} is saved as its own clip "
                  "and the recording goes on in part {next}.", {{"part", part}, {"next", next}});
        replay::hud::ShowNotice(text.c_str());
        LogFile() << "MANUAL_CHAIN,notice=" << (timeLimit ? "time" : "size") << ",part=" << g_manualPart << '\n';
    }

    // A manual recording inside the buffer at its part limit: the part from the
    // mark to now is saved and the next one starts now (chained recordings).
    void ChainManualInBuffer(std::uint32_t nowMs, bool timeLimit, std::size_t bytes, std::size_t ringBudget, std::size_t partBytes)
    {
        ++g_manualPart;
        LogFile() << "MANUAL_CHAIN,part=" << g_manualPart << ",reason=" << (timeLimit ? "time" : "memory") << ",buffer=1,from_ms="
                  << g_manualStartMs << ",to_ms=" << nowMs << ",mb=" << bytes / (1024 * 1024) << ",part_mb=" << partBytes / (1024 * 1024)
                  << ",manual_max_mb=" << g_recConfig.manualMaxMB << ",ring_budget_mb=" << ringBudget / (1024 * 1024) << '\n';
        // The part's job owns its cut before the head moves past it. A cut that
        // failed, or saves already failing, end the manual recording here (said, not
        // silent): the ring then trims as usual.
        const bool saved = SaveRecordingWindow(g_manualStartMs, nowMs, "manual_part");
        g_manualStartMs = nowMs;
        if (!saved || HeldClipSaves() > 0)
        {
            g_manualInBuffer = false;
            LogFile() << "MANUAL_CHAIN,stopped=" << (saved ? "saves_failing" : "cut_failed") << ",part=" << g_manualPart
                      << ",held=" << HeldClipSaves() << '\n';
            replay::hud::ShowNotice(saved ? TR("hud.saves_failing_manual", "Redemption Editor: saving is failing, so the manual recording stopped here. "
                                            "The parts not saved are kept in memory and saving is retried.")
                                          : TR("hud.cut_failed", "Redemption Editor: this part of the recording could not be cut, so the manual recording stopped."));
        }
        else
            ManualPartNotice(timeLimit);
    }

    // Every block (2 s): the manual part's limits (ManualMaxSeconds,
    // ManualMaxMB: a full part is saved and the recording goes on in the
    // next), and the buffer's ring (the head follows the window, and moves on
    // while over the budget; a manual mark inside it splits like a manual
    // recording, at the larger of ManualMaxMB and the ring's budget).
    void UpdateRecordingLimits()
    {
        const ULONGLONG now = GetTickCount64();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (g_recordKind == RecordKind::Manual)
        {
            const bool timeCap = replay::buffercov::ManualTimeUp(g_recConfig.manualMaxSeconds, nowMs, 0);
            if (now < g_bufferNextBlockAt && !timeCap)
                return;
            g_bufferNextBlockAt = now + kBufferBlockMs;
            const std::size_t bytes = RecordingBytes();
            const std::size_t partBytes = replay::buffercov::ManualPartBytes(g_recConfig.manualMaxMB, 0);
            if (timeCap || bytes > partBytes)
            {
                ++g_manualPart;
                LogFile() << "MANUAL_CHAIN,part=" << g_manualPart << ",reason=" << (timeCap ? "time" : "memory") << ",buffer=0"
                          << ",now_ms=" << nowMs << ",mb=" << bytes / (1024 * 1024) << ",part_mb=" << partBytes / (1024 * 1024) << '\n';
                g_manualSplitting = true;
                StopRecording(true);
                g_manualSplitting = false;
                // While an earlier part failed to save (held in memory, retried), no next part
                // starts: the parts would pile up in memory. The player is told.
                if (HeldClipSaves() > 0)
                {
                    LogFile() << "MANUAL_CHAIN,stopped=saves_failing,held=" << HeldClipSaves() << '\n';
                    replay::hud::ShowNotice(TR("hud.saves_failing", "Redemption Editor: saving is failing, so the recording stopped here. "
                        "The parts not saved are kept in memory and saving is retried; free some disk space."));
                }
                else
                {
                    StartRecording(RecordKind::Manual);
                    if (g_recording)
                        ManualPartNotice(timeCap); // after the start's REC notice
                    else
                        LogFile() << "MANUAL_CHAIN,stopped=start_refused,part=" << g_manualPart << '\n';
                }
            }
            return;
        }
        if (now < g_bufferNextBlockAt)
            return;
        g_bufferNextBlockAt = now + kBufferBlockMs;
        {
            replay::Place here;
            if (ReadPlaceZones(here))
                replay::window::AddPlaceSample(g_bufferPlaces, nowMs, here);
        }
        const std::uint32_t windowMs = g_recConfig.bufferSeconds * 1000u;
        std::uint32_t head = nowMs > windowMs ? nowMs - windowMs : 0;
        if (g_manualInBuffer)
            head = (std::min)(head, g_manualStartMs);
        std::size_t bytes = ApplyBufferHead(head);
        // The budget, adaptive when ReplayBufferAdaptive=1 (up to ReplayBufferMaxMB
        // while the machine keeps ReplayBufferReserveMB of physical memory free).
        std::size_t budget = static_cast<std::size_t>(g_recConfig.bufferMB) * 1024 * 1024;
        {
            replay::buffercov::AdaptiveConfig adaptive;
            adaptive.enabled = g_recConfig.bufferAdaptive;
            adaptive.baseMB = g_recConfig.bufferMB;
            adaptive.maxMB = g_recConfig.bufferMaxMB;
            adaptive.reserveMB = g_recConfig.bufferReserveMB;
            std::uint64_t availMB = 0;
            if (adaptive.enabled)
            {
                MEMORYSTATUSEX status{};
                status.dwLength = sizeof(status);
                availMB = GlobalMemoryStatusEx(&status) ? status.ullAvailPhys / (1024 * 1024) : 0;
            }
            const replay::buffercov::Budget chosen = replay::buffercov::AdaptiveBudget(adaptive, bytes, availMB);
            const bool memLowBefore = g_bufferBudgetMemLow;
            const std::size_t budgetBefore = g_bufferBudgetBytes;
            budget = chosen.bytes;
            g_bufferBudgetBytes = budget;
            g_bufferBudgetMemLow = chosen.memoryLow;
            if (adaptive.enabled && (budgetBefore / (1024 * 1024) != budget / (1024 * 1024) || memLowBefore != chosen.memoryLow))
            {
                static int adaptiveLogs = 0;
                if (adaptiveLogs++ < 200)
                    LogFile() << "BUFFER_ADAPTIVE,budget_mb=" << budget / (1024 * 1024) << ",base_mb=" << adaptive.baseMB
                              << ",max_mb=" << adaptive.maxMB << ",avail_phys_mb=" << availMB << ",reserve_mb=" << adaptive.reserveMB
                              << ",used_mb=" << bytes / (1024 * 1024) << ",mem_low=" << (chosen.memoryLow ? 1 : 0) << '\n';
            }
        }
        // A manual mark holds its part to the same limits as a manual recording
        // without the buffer: ManualMaxSeconds, and ManualMaxMB (or the ring's budget
        // when that is larger). The seconds before the mark stay the ring's, trimmed
        // to its own budget.
        const std::size_t partBytes = replay::buffercov::ManualPartBytes(g_recConfig.manualMaxMB, budget);
        if (g_manualInBuffer && replay::buffercov::ManualTimeUp(g_recConfig.manualMaxSeconds, nowMs, g_manualStartMs))
            ChainManualInBuffer(nowMs, true, bytes, budget, partBytes);
        int steps = 0;
        while (g_bufferHeadMs + 2 * kBufferBlockMs < nowMs && steps++ < 64)
        {
            const bool atMark = replay::buffercov::HeadAtManualMark(g_manualInBuffer, g_bufferHeadMs, g_manualStartMs);
            if (bytes <= replay::buffercov::RingLimitBytes(atMark, budget, partBytes))
                break;
            if (atMark)
                ChainManualInBuffer(nowMs, false, bytes, budget, partBytes);
            bytes = ApplyBufferHead(g_bufferHeadMs + kBufferBlockMs);
        }
        // What the ring really covers, and why it is short; a change of the reason is
        // logged at once.
        const replay::buffercov::Coverage coverage =
            replay::buffercov::Evaluate(nowMs, g_bufferHeadMs, g_recConfig.bufferSeconds, g_manualInBuffer, g_manualStartMs);
        if (coverage.limit != g_bufferLimit)
        {
            g_bufferLimit = coverage.limit;
            if (++g_bufferLimitChanges <= 400)
                LogFile() << "BUFFER_COVERAGE,phase=change,covered_ms=" << coverage.coveredMs << ",target_ms=" << coverage.targetMs
                          << ",limit=" << replay::buffercov::LimitText(coverage.limit) << ",mb=" << bytes / (1024 * 1024)
                          << ",budget_mb=" << budget / (1024 * 1024) << ",adaptive=" << (g_recConfig.bufferAdaptive ? 1 : 0) << '\n';
        }
        static ULONGLONG nextLog = 0;
        if (now >= nextLog)
        {
            nextLog = now + 10000;
            // The pose records (all, still visited by the worker, sealed, removed so far)
            // and the process's private bytes, so a long buffer session shows whether
            // memory grows with the entities that left it.
            const replay::pose::capture::RecordCounts poseRecords = replay::pose::capture::RecordCountsSnapshot();
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            const std::uint64_t privateMb = K32GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))
                ? static_cast<std::uint64_t>(memory.PrivateUsage) / (1024 * 1024) : 0;
            LogFile() << "BUFFER_STATE,now_ms=" << nowMs << ",head_ms=" << g_bufferHeadMs
                      << ",window_ms=" << (nowMs - g_bufferHeadMs) << ",mb=" << bytes / (1024 * 1024)
                      << ",pose_mb=" << replay::pose::capture::TrackBytes() / (1024 * 1024)
                      << ",peak_mb=" << g_bufferPeakBytes / (1024 * 1024)
                      << ",trimmed_pose_chunks=" << g_bufferTrimmedPose << ",trimmed_fx_frames=" << g_bufferTrimmedFx
                      << ",fx_tracks=" << g_clip.registeredFx.size() << ",trimmed_fx_tracks=" << g_bufferTrimmedFxTracks
                      << ",manual=" << (g_manualInBuffer ? 1 : 0) << ",quiet_dropped_lines=" << g_logFilter.Dropped()
                      << ",pose_records=" << poseRecords.records << ",pose_drain=" << poseRecords.drain
                      << ",pose_sealed=" << poseRecords.sealed << ",pose_pruned=" << poseRecords.pruned
                      // Rope records in use, freed by the ring, refused full.
                      << ",rope_records=" << g_clip.ropes.Records().size() << ",rope_freed=" << g_clip.ropes.FreedRecords()
                      << ",rope_refused_full=" << g_clip.ropes.RefusedFull() << ",rope_still_samples=" << g_clip.ropes.StillSamples()
                      << ",private_mb=" << privateMb
                      // The real window and its limit.
                      << ",covered_s=" << coverage.coveredMs / 1000u << ",target_s=" << coverage.targetMs / 1000u
                      << ",limit=" << replay::buffercov::LimitText(coverage.limit) << ",budget_mb=" << budget / (1024 * 1024)
                      << ",adaptive=" << (g_recConfig.bufferAdaptive ? 1 : 0) << ",mem_low=" << (g_bufferBudgetMemLow ? 1 : 0) << '\n';
        }
    }

    // F4 (and the editor's record button): without the buffer, start and stop;
    // with it, F4 marks a manual start inside the running ring and the second F4
    // saves from the mark to now.
    void PressRecord()
    {
        if (g_recording && g_recordKind == RecordKind::Buffer)
        {
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            if (!g_manualInBuffer)
            {
                g_manualInBuffer = true;
                g_manualStartMs = nowMs;
                g_manualPart = 0;
                LogFile() << "MANUAL_MARK,at_ms=" << nowMs << '\n';
            }
            else
            {
                g_manualInBuffer = false;
                // The chain's final save is the next part.
                const int partsSaved = g_manualPart;
                g_manualPart = replay::buffercov::FinalPartNumber(partsSaved);
                if (partsSaved > 0)
                    LogFile() << "MANUAL_CHAIN,final_part=" << g_manualPart << ",parts=" << g_manualPart << ",buffer=1,from_ms=" << g_manualStartMs
                              << ",to_ms=" << nowMs << ",part_mb=" << replay::buffercov::ManualPartBytes(g_recConfig.manualMaxMB, g_bufferBudgetBytes) / (1024 * 1024)
                              << ",manual_max_mb=" << g_recConfig.manualMaxMB << ",ring_budget_mb=" << g_bufferBudgetBytes / (1024 * 1024) << '\n';
                SaveRecordingWindow(g_manualStartMs, nowMs, "manual");
                g_manualPart = 0; // a later Shift+F4 or chain starts unnumbered
            }
            return;
        }
        if (g_recording)
            StopRecording();
        else
        {
            g_manualPart = 0;
            StartRecording();
        }
    }

    // The editor's Replay Settings: applied now and kept in RedemptionEditor.ini
    // for the next load.
    void SetReplayBuffer(bool enabled, std::uint32_t seconds, std::uint32_t megabytes)
    {
        g_recConfig.buffer = enabled;
        g_recConfig.bufferSeconds = std::clamp(seconds, 10u, 600u);
        g_recConfig.bufferMB = replay::buffercov::ClampBufferMB(megabytes); // under the clip file's 1 GiB
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string configPath = replay::identity::IniPathIn(workingDirectory);
        replay::identity::IniWrite("ReplayBuffer", enabled ? "1" : "0", configPath.c_str());
        replay::identity::IniWrite("ReplayBufferSeconds", std::to_string(g_recConfig.bufferSeconds).c_str(), configPath.c_str());
        replay::identity::IniWrite("ReplayBufferMB", std::to_string(g_recConfig.bufferMB).c_str(), configPath.c_str());
        if (!enabled && g_recording && g_recordKind == RecordKind::Buffer)
            StopRecording(false);
        LogFile() << "RECORDING_CONFIG,buffer=" << (enabled ? 1 : 0) << ",buffer_s=" << g_recConfig.bufferSeconds
                  << ",buffer_mb=" << g_recConfig.bufferMB << ",from=editor\n";
    }

    // Clip compression from the editor's Replay Settings, saved to the ini like the
    // buffer's settings.
    replay::clipcompress::worker::Settings CompressSettings()
    {
        replay::clipcompress::worker::Settings settings;
        settings.automatic = g_recConfig.compressClips;
        settings.days = g_recConfig.compressAfterDays;
        settings.level = g_recConfig.compressLevel;
        settings.format = g_recConfig.compressFormat == 1 ? replay::clipcompress::Format::Zstd : replay::clipcompress::Format::Packed;
        settings.upgrade = settings.format == replay::clipcompress::Format::Packed;
        return settings;
    }

    void SetClipCompression(bool enabled, std::uint32_t days)
    {
        g_recConfig.compressClips = enabled;
        g_recConfig.compressAfterDays = std::clamp(days, 1u, 60u);
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string configPath = replay::identity::IniPathIn(workingDirectory);
        replay::identity::IniWrite("CompressClips", enabled ? "1" : "0", configPath.c_str());
        replay::identity::IniWrite("CompressAfterDays", std::to_string(g_recConfig.compressAfterDays).c_str(),
            configPath.c_str());
        replay::clipcompress::worker::Configure(CompressSettings());
        LogFile() << "RECORDING_CONFIG,compress_clips=" << (enabled ? 1 : 0) << ",compress_after_days="
                  << g_recConfig.compressAfterDays << ",from=editor\n";
    }

    // The compressor's side of the script tick: its log lines, the clip
    // list after a file was replaced, and the saves and the open clip it
    // must stay off.
    void ServiceClipCompressor()
    {
        replay::clipcompress::worker::SetHold(SavePending() || ClipReadPending()); // and a clip read
        static std::wstring openClip;
        // The open clip is the one playing (after Back to Game no clip stays "open":
        // it can be compressed, and the gallery's C works on it). A save of it holds
        // the compressor.
        const std::wstring open = g_transport.Playing() ? g_currentClipPath : std::wstring();
        if (openClip != open)
        {
            openClip = open;
            replay::clipcompress::worker::SetOpenClip(openClip);
        }
        std::vector<std::string> lines;
        replay::clipcompress::worker::DrainLog(lines);
        for (const std::string& line : lines)
            LogFile() << line << '\n';
        static std::uint64_t changes = 0;
        const std::uint64_t now = replay::clipcompress::worker::ChangeSequence();
        if (now != changes)
        {
            changes = now;
            RebuildEditorClipList();
        }
    }

    // Shift+F4: the buffer's whole window to a clip, the recording goes on.
    void SaveBuffer()
    {
        if (!g_recording || g_recordKind != RecordKind::Buffer)
        {
            replay::hud::ShowNotice(TR("hud.buffer_off", "Redemption Editor: the replay buffer is off."));
            return;
        }
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        // The save says what it covers.
        const replay::buffercov::Coverage coverage =
            replay::buffercov::Evaluate(nowMs, g_bufferHeadMs, g_recConfig.bufferSeconds, g_manualInBuffer, g_manualStartMs);
        const std::size_t bytes = RecordingBytes();
        const double mbPerSecond = coverage.coveredMs
            ? static_cast<double>(bytes) / (1024.0 * 1024.0) / (static_cast<double>(coverage.coveredMs) / 1000.0) : 0.0;
        const bool shortWindow = replay::buffercov::ShortWindowNotice(coverage);
        LogFile() << "BUFFER_COVERAGE,phase=save,covered_ms=" << coverage.coveredMs << ",target_ms=" << coverage.targetMs
                  << ",limit=" << replay::buffercov::LimitText(coverage.limit) << ",mb=" << bytes / (1024 * 1024)
                  << ",budget_mb=" << g_bufferBudgetBytes / (1024 * 1024) << ",mb_per_s=" << mbPerSecond
                  << ",mb_for_target=" << replay::buffercov::MegabytesFor(g_recConfig.bufferSeconds, mbPerSecond)
                  << ",adaptive=" << (g_recConfig.bufferAdaptive ? 1 : 0) << ",notice=" << (shortWindow ? 1 : 0) << '\n';
        if (SaveRecordingWindow(g_bufferHeadMs, nowMs, "buffer") && shortWindow)
        {
            const std::string text = TRF("hud.buffer_short", "Redemption Editor: saved the last {saved} seconds, not {asked}: the replay buffer memory ({mb} MB) "
                "fills faster in busy places. Raise Memory in Replay Settings to keep more.",
                {{"saved", std::to_string(coverage.coveredMs / 1000u)}, {"asked", std::to_string(coverage.targetMs / 1000u)},
                    {"mb", std::to_string(static_cast<unsigned>(g_bufferBudgetBytes / (1024 * 1024)))}});
            replay::hud::ShowNotice(text.c_str());
        }
    }

    void UpdateBufferAutoStart(ULONGLONG now)
    {
        if (!g_recConfig.buffer || g_recording || g_transport.Playing() || !g_context.puppets.list.empty() || g_world.cleared ||
            now < g_bufferResumeAt || g_loadedClipHoldsBuffer || !g_pendingLoadPath.empty()) // nor while a clip opens
            return;
        const Player player = PLAYER::PLAYER_ID();
        if (!PLAYER::IS_PLAYER_PLAYING(player) || CAM::IS_SCREEN_FADED_OUT() || CAM::IS_SCREEN_FADING_OUT())
        {
            g_bufferResumeAt = now + 1000;
            return;
        }
        if (replay::pose::capture::FinalizerWanted())
        {
            g_bufferResumeAt = now + 2000;
            return;
        }
        std::string error;
        if (!replay::pose::capture::AcquireFinalizer(error))
        {
            g_bufferResumeAt = now + 5000; // the Pose Editor has it
            return;
        }
        StartRecording(RecordKind::Buffer);
    }

    // A window of the running recording to a clip: cut from the lifecycle block
    // holding fromMs, times from 0, saved while the recording goes on. The file
    // write runs on a worker thread; the cut itself is a copy. Every save is a job
    // that owns its snapshot of the clip until the file is written
    // (clip_save_queue.h): a failed write keeps it and tries again (5 s, 15 s,
    // 30 s, then every minute, at once when a save is asked again); it goes only
    // once written, replaced by a newer save of the same file, or with its file
    // deleted. A load of that file reads the snapshot.
    struct ClipSaveData
    {
        std::shared_ptr<const replay::Recording> recording;
        std::shared_ptr<const std::vector<replay::pose::PoseRecordExport>> poses;
        std::shared_ptr<const replay::camera::Track> camera;
        std::uint32_t durationMs = 0;
        std::string why;
        bool editorSave = false;   // the open clip's own file
        bool newRecording = false; // the editor's thumbnail frames are this clip's
        // The generic sound recorder's stats of the recording this save comes from (a
        // new recording's save writes ReplayDebug\<clip>.sound-generic.txt beside the
        // clip, on the save's thread); null: no report.
        std::shared_ptr<const replay::soundgeneric::Stats> soundGenericStats;
        ULONGLONG startedAt = 0;
        ULONGLONG cutMs = 0;
        std::uint32_t fromMs = 0, toMs = 0, baseMs = 0;
    };
    replay::savequeue::Queue<ClipSaveData> g_clipSaves;

    // The job being written on a worker thread (one file at a time).
    struct PendingSave
    {
        std::uint64_t jobId = 0;
        ClipSaveData data; // shares the job's snapshot while it is written
        std::uint32_t attempt = 1;
        std::thread worker;
        std::atomic<bool> done{false};
        bool ok = false;
        std::string error;
        std::wstring path;
        // The sound-generic report of this save (empty: none asked).
        std::wstring reportPath;
        std::string reportError;
        bool reportOk = false;
        // The file's pack (clipfile::SaveOptions::pack): a clip saved here is
        // written already packed (zstd blocks), plain when the pack fails.
        replay::clipfile::SaveInfo saveInfo;
        ~PendingSave()
        {
            if (worker.joinable())
                worker.detach(); // as ClipRead (OnModuleDetach also waits and detaches it)
        }
    };
    std::unique_ptr<PendingSave> g_pendingSave;
    bool SavePending() { return g_pendingSave != nullptr; }
    std::size_t HeldClipSaves() { return g_clipSaves.HeldCount(); }

    std::string ClipFileName(const std::wstring& path)
    {
        return Narrow(path.substr(path.find_last_of(L'\\') + 1));
    }

    // The next due job on its thread (none while one is written).
    void StartNextClipSave(ULONGLONG now)
    {
        if (g_pendingSave)
            return;
        const auto* job = g_clipSaves.Begin(now);
        if (!job)
            return;
        auto pending = std::make_unique<PendingSave>();
        pending->jobId = job->id;
        pending->data = job->payload;
        pending->attempt = job->failures + 1;
        pending->path = job->path;
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        // Bench only: RedemptionEditor.ini ClipSaveFailTest=N fails the first N writes
        // of this load (the held save, its retry and the notices checked without a
        // full disk). 0 by default; read only with Debug=1, the product never fails a
        // save on purpose.
        static int failTest = g_debugMode ? -1 : 0;
        if (failTest < 0)
        {
            char workingDirectory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, workingDirectory);
            const std::string configPath = replay::identity::IniPathIn(workingDirectory);
            failTest = static_cast<int>(replay::identity::DevIniInt("ClipSaveFailTest", 0, configPath.c_str()));
        }
        const bool fail = failTest > 0;
        if (fail)
            --failTest;
        PendingSave* save = pending.get();
        save->worker = std::thread([save, base, size, fail]() {
            replay::threads::Scope threadName("clip_save");
            const ClipSaveData& data = save->data;
            if (fail)
                save->error = "ClipSaveFailTest";
            else
            {
                // As the read above; a failed save is retried by the save queue.
                try
                {
                    replay::clipfile::SaveOptions options;
                    options.pack = true;
                    options.zstdLevel = replay::clipcompress::kSavePackLevel;
                    options.threads = replay::clipcompress::kSavePackThreads;
                    save->ok = replay::clipfile::Save(*data.recording, *data.poses, *data.camera, data.durationMs, base,
                        size, save->path, save->error, options, &save->saveInfo);
                }
                catch (const std::exception& exception)
                {
                    save->ok = false;
                    save->error = std::string("exception_") + exception.what();
                }
                catch (...)
                {
                    save->ok = false;
                    save->error = "unknown_exception";
                }
                // The generic sound report beside a saved new recording (pure formatting and
                // one file; no engine).
                if (save->ok && data.soundGenericStats && data.recording)
                {
                    try
                    {
                        save->reportOk = replay::soundgeneric::WriteReport(*data.recording, *data.soundGenericStats, save->path,
                            data.durationMs, save->reportPath, save->reportError);
                    }
                    catch (...)
                    {
                        save->reportOk = false;
                        save->reportError = "exception";
                    }
                }
            }
            save->done.store(true);
        });
        g_pendingSave = std::move(pending);
    }

    // A save asked for: its job owns `data` until the file is written.
    void QueueClipSave(const std::wstring& path, ClipSaveData data)
    {
        g_clipSaves.Add(path, std::move(data));
        StartNextClipSave(GetTickCount64());
    }

    // True with the snapshot of the newest save of `path` not written yet;
    // false with no error when there is none (the file is read).
    bool CopyQueuedClip(const std::wstring& path, replay::Recording& recording, std::vector<replay::pose::PoseRecordExport>& poses,
        replay::camera::Track& camera, std::uint64_t& durationMs, std::string& error)
    {
        error.clear();
        const auto* job = g_clipSaves.Find(path);
        if (!job)
            return false;
        try
        {
            recording = *job->payload.recording;
            poses = *job->payload.poses;
            camera = *job->payload.camera;
        }
        catch (const std::bad_alloc&)
        {
            error = "not enough memory to open the clip";
            return false;
        }
        durationMs = job->payload.durationMs;
        LogFile() << "CLIP_LOAD_FROM_SAVE,path=" << Narrow(path) << ",failures=" << job->failures << '\n';
        return true;
    }

    // wait: the write in progress is done when this returns (and no other
    // starts); otherwise a finished write is taken and the next due job
    // starts.
    void FinishPendingSave(bool wait)
    {
        if (g_pendingSave && (wait || g_pendingSave->done.load()))
        {
            g_pendingSave->worker.join();
            const std::unique_ptr<PendingSave> finished = std::move(g_pendingSave);
            const PendingSave& save = *finished;
            const ClipSaveData& data = save.data;
            replay::savequeue::Queue<ClipSaveData>::Job job;
            const replay::savequeue::Outcome outcome = g_clipSaves.Finish(save.jobId, save.ok, save.error, GetTickCount64(), &job);
            const std::string name = ClipFileName(save.path);
            // A failed save of the open clip leaves its changes unsaved (the flag was
            // cleared when the save began).
            if (!save.ok && (data.why == "editor" || data.why == "leave") && g_memoryClipPath == save.path)
                g_clipEdited = true;
            if (outcome == replay::savequeue::Outcome::Saved)
            {
                g_lastWrittenClipName = name;
                // Only a new recording's first write takes the editor's frames as its
                // thumbnail; an edit of a clip keeps its own (the frames kept are another
                // recording's), and so does a retried one.
                if (data.newRecording && save.attempt == 1)
                {
                    g_savedClipMs = data.durationMs;
                    g_savedClipName = name;
                    ++g_savedClipSequence;
                }
                if (save.attempt > 1)
                    replay::hud::ShowNotice(TRF("hud.saved_now", "Redemption Editor: {name} is saved now.", {{"name", name}}).c_str());
                RebuildEditorClipList();
            }
            else if (outcome == replay::savequeue::Outcome::Held && job.failures == 1)
            {
                // The notice is the editor's; the game's text line says a failure.
                replay::hud::ShowNotice(TRF("hud.save_held", "Redemption Editor: could not save {name} ({error}). The replay is kept in memory and saving is retried; "
                    "free some disk space.", {{"name", name}, {"error", save.error}}).c_str());
            }
            // The clip's entities and size on disk.
            WIN32_FILE_ATTRIBUTE_DATA written{};
            const double savedMb = save.ok && GetFileAttributesExW(save.path.c_str(), GetFileExInfoStandard, &written)
                ? static_cast<double>((static_cast<std::uint64_t>(written.nFileSizeHigh) << 32) | written.nFileSizeLow) / (1024.0 * 1024.0)
                : 0.0;
            char savedMbText[24]{};
            std::snprintf(savedMbText, sizeof(savedMbText), "%.1f", savedMb);
            if (data.soundGenericStats && save.ok)
                LogFile() << "SOUND_GENERIC_REPORT,clip=" << name << ",ok=" << (save.reportOk ? 1 : 0) << ",path="
                          << (save.reportOk ? Narrow(save.reportPath) : std::string("none")) << ",error="
                          << (save.reportError.empty() ? "none" : save.reportError.c_str()) << '\n';
            LogFile() << "BUFFER_SAVE,why=" << data.why << ",from_ms=" << data.fromMs << ",to_ms=" << data.toMs
                      << ",base_ms=" << data.baseMs << ",duration_ms=" << data.durationMs << ",ok=" << (save.ok ? 1 : 0)
                      << ",entities=" << (data.recording ? data.recording->created.size() : 0) << ",mb=" << savedMbText
                      << ",cut_ms=" << data.cutMs << ",total_ms=" << (GetTickCount64() - data.startedAt)
                      << ",packed=" << (save.saveInfo.packed ? 1 : 0) << ",clip_bytes=" << save.saveInfo.clipBytes
                      << ",encode_ms=" << save.saveInfo.encodeMs << ",pack_ms=" << save.saveInfo.packMs
                      << ",verify_ms=" << save.saveInfo.verifyMs << ",write_ms=" << save.saveInfo.writeMs
                      << ",pack_fallback=" << (save.saveInfo.fallback.empty() ? "none" : save.saveInfo.fallback.c_str())
                      << ",attempt=" << save.attempt << ",outcome=" << static_cast<int>(outcome)
                      << ",held=" << g_clipSaves.HeldCount() << ",queued=" << g_clipSaves.Size()
                      << ",path=" << Narrow(save.path) << ",error=" << (save.ok ? "none" : save.error.c_str()) << '\n';
        }
        if (!wait)
            StartNextClipSave(GetTickCount64());
    }

    // Unload: the saves not written yet (held ones too) get one last try each on
    // this thread; the data goes with the module.
    void FlushClipSaves()
    {
        FinishPendingSave(true);
        if (g_clipSaves.Empty())
            return;
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        for (const auto& job : g_clipSaves.Jobs())
        {
            std::string error;
            const ClipSaveData& data = job.payload;
            const bool ok = replay::clipfile::Save(*data.recording, *data.poses, *data.camera, data.durationMs, base, size, job.path, error);
            LogFile() << "CLIP_SAVE_FLUSH,path=" << Narrow(job.path) << ",ok=" << (ok ? 1 : 0) << ",failures_before=" << job.failures
                      << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        }
        g_clipSaves = {};
    }

    // Clip names "NNN-Place": the highest counter given out in this session; a
    // save not written yet is not on disk for the scan.
    std::uint32_t g_lastClipNumber = 0;

    // A new clip's file, with -2, -3... only when that name is on disk or held by
    // a save not written yet (two saves in one second). NNN-Place (042-Valentine,
    // the part's -pN after it): the counter is the highest NNN of the folder plus
    // one (clip-* names do not count), the place is the clip's PLAC text where the
    // recording started (clipfile::ClipPlaceTag, Wild without one).
    std::wstring NewClipPath(const replay::Place& place, const wchar_t* suffix = L"")
    {
        std::vector<std::wstring> names; // every read root's: the numbers go on after the game folder's clips
        for (const replay::storage::Entry& entry : replay::storage::List(kClipFolder, L"*.rpclip"))
            names.push_back(entry.name);
        const std::uint32_t number = (std::max)(replay::clipfile::NextClipNumber(names), g_lastClipNumber + 1);
        g_lastClipNumber = number;
        const std::wstring base = replay::clipfile::NumberedClipBase(number,
            replay::clipfile::ClipPlaceTag(place.town, place.district, place.state), suffix);
        std::wstring name = replay::clipfile::UniqueClipName(base, [](const std::wstring& candidate) {
            const std::wstring path = ClipPath(candidate);
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ||
                replay::storage::Exists(FindClipFile(candidate + L".rpclip")) ||
                GetFileAttributesW((path + L".tmp").c_str()) != INVALID_FILE_ATTRIBUTES || g_clipSaves.Holds(path);
        });
        if (name.empty())
            name = std::wstring(base) + L"-" + std::to_wstring(GetTickCount64());
        return ClipPath(name);
    }

    // A window of the running recording to a clip: cut from the lifecycle block
    // holding fromMs, times from 0, saved while the recording goes on.
    bool SaveRecordingWindow(std::uint32_t fromMs, std::uint32_t toMs, const char* why)
    {
        // No wait for the save before this one: the cut is a job that follows it.
        const ULONGLONG startedAt = GetTickCount64();
        ClipSaveData data;
        replay::window::Result result;
        bool cutOk = false;
        try
        {
            // The frames still in the capture rings first (PoseWorkerPollMs above 5 leaves
            // them there longer); a stopped recording was drained by StopAll.
            replay::pose::capture::FlushRings();
            const std::vector<replay::pose::PoseRecordExport> poses = replay::pose::capture::ExportRecords();
            auto cut = std::make_shared<replay::Recording>();
            auto cutPoses = std::make_shared<std::vector<replay::pose::PoseRecordExport>>();
            cutOk = replay::window::Cut(g_clip, poses, toMs, fromMs, toMs, *cut, *cutPoses, result);
            // The PLAC (and the name) of the window's start, not of the buffer's.
            if (cutOk && g_recordKind == RecordKind::Buffer)
                replay::window::ApplyWindowPlace(cut->place, g_bufferPlaces, fromMs);
            data.recording = std::move(cut);
            data.poses = std::move(cutPoses);
            data.camera = std::make_shared<const replay::camera::Track>();
        }
        catch (const std::bad_alloc&)
        {
            cutOk = false;
            result.error = "not enough memory for the cut";
        }
        if (!cutOk)
        {
            LogFile() << "BUFFER_SAVE,why=" << why << ",ok=0,error=" << result.error << '\n';
            replay::hud::ShowNotice(TR("hud.save_failed", "Redemption Editor: could not save the replay."));
            return false;
        }
        data.durationMs = result.durationMs;
        data.why = why;
        data.newRecording = true;
        data.startedAt = startedAt;
        data.cutMs = GetTickCount64() - startedAt;
        data.fromMs = fromMs;
        data.toMs = toMs;
        data.baseMs = result.baseMs;
        data.soundGenericStats = replay::soundgeneric::StatsSnapshot();
        // The name's place is the window's own PLAC (the cut copies it). Only a manual
        // chain's saves carry the part (a Shift+F4 during a chain does not take the
        // part's name).
        const bool manualSave = std::strcmp(why, "manual") == 0 || std::strcmp(why, "manual_part") == 0;
        const std::wstring path = NewClipPath(data.recording->place,
            manualSave && g_manualPart > 0 ? (L"-p" + std::to_wstring(g_manualPart)).c_str() : L"");
        QueueClipSave(path, std::move(data));
        return true;
    }

    // The manual recording that just stopped, written on a thread like the
    // buffer's windows. The clip stays in memory for playback; the job owns a copy.
    void SaveStoppedRecording(const std::wstring& path)
    {
        const ULONGLONG startedAt = GetTickCount64();
        ClipSaveData data;
        std::uint64_t copyT[3] = {replay::prof::Now(), 0, 0}; // the copy's parts
        try
        {
            data.recording = std::make_shared<const replay::Recording>(g_clip);
            copyT[1] = replay::prof::Now();
            data.poses = std::make_shared<const std::vector<replay::pose::PoseRecordExport>>(replay::pose::capture::ExportRecords());
            copyT[2] = replay::prof::Now();
            data.camera = std::make_shared<const replay::camera::Track>(g_cameraTrack);
            LogFile() << "BUFFER_SAVE_COPY,why=manual_stop,recording_ms=" << static_cast<long long>(TicksMs(copyT[1] - copyT[0]))
                      << ",poses_ms=" << static_cast<long long>(TicksMs(copyT[2] - copyT[1])) << ",camera_ms=" << static_cast<long long>(TicksMs(replay::prof::Now() - copyT[2]))
                      << ",pose_records=" << data.poses->size() << '\n';
        }
        catch (const std::bad_alloc&)
        {
            // The copy did not fit; the recording stays in memory (playback, then the
            // editor's Ctrl+S writes it).
            LogFile() << "BUFFER_SAVE,why=manual_stop,ok=0,error=not enough memory for the copy\n";
            replay::hud::ShowNotice(replay::keys::Fill(TR("hud.no_memory_replay", "Redemption Editor: not enough memory to save the replay. Play it "
                                                                                  "and save it from the editor ({key_save})."),
                replay::keys::Current()).c_str());
            return;
        }
        data.why = "manual_stop";
        data.newRecording = true;
        data.durationMs = static_cast<std::uint32_t>(g_transport.DurationMs());
        data.soundGenericStats = replay::soundgeneric::StatsSnapshot();
        data.startedAt = startedAt;
        data.cutMs = GetTickCount64() - startedAt; // the copy
        data.toMs = data.durationMs;
        g_currentClipPath = path;
        g_memoryClipPath = path;
        g_clipEdited = false;
        QueueClipSave(path, std::move(data));
    }

    // The editor's save (Ctrl+S, and on leaving a clip with changed markers): the
    // clip in memory and its markers over its own file, or a new file for a buffer
    // window never saved, on a thread like the other saves (the file is written
    // aside and renamed: a failed write leaves the old clip whole).
    bool ProjectOwnsPlayback();
    void SyncProjectMarkers();

    void SaveEditedClip(const char* why)
    {
        if (!g_transport.Playing() || g_transport.DurationMs() == 0)
            return;
        // A project's clip: its markers are the project's (edit markers live in the
        // montage, never in the clip file).
        if (ProjectOwnsPlayback())
        {
            SyncProjectMarkers();
            g_clipEdited = false;
            LogFile() << "CLIP_EDIT_SAVE,why=" << why << ",skipped=project_clip\n";
            return;
        }
        // A save asked for retries the held ones now (the disk may have room again);
        // no wait for the save before it.
        g_clipSaves.RetryNow();
        const bool ownFile = !g_memoryClipPath.empty();
        const std::wstring path = ownFile ? SavableClipPath(g_memoryClipPath) : NewClipPath(g_clip.place);
        const ULONGLONG startedAt = GetTickCount64();
        ClipSaveData data;
        try
        {
            data.recording = std::make_shared<const replay::Recording>(g_clip);
            data.poses = std::make_shared<const std::vector<replay::pose::PoseRecordExport>>(replay::pose::capture::ExportRecords());
            data.camera = std::make_shared<const replay::camera::Track>(g_cameraTrack);
        }
        catch (const std::bad_alloc&)
        {
            LogFile() << "CLIP_EDIT_SAVE,why=" << why << ",ok=0,error=not enough memory for the copy\n";
            replay::hud::ShowNotice(TR("hud.no_memory_changes", "Redemption Editor: not enough memory to save the changes. They stay in the open clip."));
            return; // the changes stay flagged
        }
        data.editorSave = ownFile;
        data.newRecording = !ownFile;
        data.why = why;
        data.durationMs = static_cast<std::uint32_t>(g_transport.DurationMs());
        data.startedAt = startedAt;
        data.cutMs = GetTickCount64() - startedAt;
        data.toMs = data.durationMs;
        g_memoryClipPath = path;
        g_currentClipPath = path;
        g_clipEdited = false;
        replay::clipcompress::worker::SetOpenClip(path); // never compressed while written
        QueueClipSave(path, std::move(data));
        LogFile() << "CLIP_EDIT_SAVE,why=" << why << ",path=" << Narrow(path) << ",markers=" << g_cameraTrack.Keyframes().size() << '\n';
    }

    // F7 or the editor's play with the buffer running: the buffer stops
    // (no file) and its window becomes the clip in memory.
    void StopBufferForPlayback()
    {
        const std::uint32_t headMs = g_bufferHeadMs;
        StopRecording(false);
        const std::uint64_t durationMs = g_transport.DurationMs();
        if (durationMs == 0)
            return;
        replay::Recording cut;
        std::vector<replay::pose::PoseRecordExport> cutPoses;
        replay::window::Result result;
        std::string error;
        const bool ok = replay::window::Cut(g_clip, replay::pose::capture::ExportRecords(), durationMs, headMs,
                            static_cast<std::uint32_t>(durationMs), cut, cutPoses, result) &&
            replay::pose::capture::ImportRecords(cutPoses, error);
        if (ok)
        {
            g_clip = std::move(cut);
            g_clip.fromDisk = true; // the live world's objects are not the recording's leftovers
            g_memoryClipPath.clear(); // no file yet
            g_currentClipPath.clear(); // the open clip is no file's (T, C, Delete looked at the last one)
            g_clipEdited = false;
            g_markerHistory = {};
            ++g_clipLoadSequence;
            g_cameraTrack = {};
            g_cameraTrackManualOverride = true;
            g_transport.SetDurationMs(result.durationMs);
        }
        // base_ms, the capture clock at the clip's 0 (the block that holds head_ms), so
        // a log reader can line the recording's times up with the playback's
        // (tools/volume_probe_diff.py).
        LogFile() << "BUFFER_TO_PLAYBACK,head_ms=" << headMs << ",base_ms=" << (ok ? result.baseMs : 0)
                  << ",duration_ms=" << (ok ? result.durationMs : 0) << ",ok=" << (ok ? 1 : 0) << ",error=" << (ok ? "none" : (error.empty() ? result.error : error).c_str()) << '\n';
    }

    void ResetRecordGate(); // below UpdateRecording

    void StartRecording(RecordKind kind)
    {
        // The cleared world only waits for the save reload (no recording in the
        // editor's world).
        if (g_world.cleared)
        {
            if (!g_world.recordRefusedLogged)
            {
                g_world.recordRefusedLogged = true;
                LogFile() << "WORLD_MODEL,phase=record_refused,kind=" << (kind == RecordKind::Buffer ? "buffer" : "manual") << '\n';
            }
            if (kind != RecordKind::Buffer)
                replay::hud::ShowNotice(TR("hud.leave_replay", "Redemption Editor: leave the replay first; the world comes back with your last save."));
            return;
        }
        const std::uint64_t startedAt = replay::prof::Now();
        g_loadedClipHoldsBuffer = false; // a new recording replaces the loaded clip
        if (g_transport.Playing())
            StopPlayback();
        if (!TakePoseFinalizer())
            return;
        replay::prof::ResetSession();

        g_cameraTrack.Clear();
        g_memoryClipPath.clear();
        g_clipEdited = false;
        g_markerHistory = {};
        ++g_clipLoadSequence;
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;


        g_recordingStartedAt = GetTickCount64();
        g_transport.SetDurationMs(0);
        g_recording = true;
        ResetRecordGate();

        std::ofstream& log = LogFile();
        log << "CAPTURE_MODE"
            << ",role=pose_track_v2"
            << ",task_native_actor_calls=0"
            << ",transform_actor_calls=0"
            << ",pose_track=v2_capture"
            << '\n';
        log.flush();
        replay::recorder::Start();
        replay::doors::BeginRecording(g_clip);
        // The clip's world: clock and weather at the start.
        g_clip.environment.clockStartS = static_cast<std::uint32_t>(
            CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS());
        MISC::GET_CURR_WEATHER_STATE(reinterpret_cast<Hash*>(&g_clip.environment.weatherFrom),
            reinterpret_cast<Hash*>(&g_clip.environment.weatherTo), &g_clip.environment.weatherMix);
        RecordPlace();
        replay::world::BeginRecording(g_clip, log); // the map state at the start

        g_recordKind = kind;
        replay::pose::fidelity::SetBufferMode(kind == RecordKind::Buffer); // keeps its last 120 s
        g_bufferHeadMs = 0;
        g_bufferPlaces.clear();
        if (g_clip.place.recorded)
            replay::window::AddPlaceSample(g_bufferPlaces, 0, g_clip.place);
        g_manualInBuffer = false;
        g_bufferNextBlockAt = GetTickCount64() + kBufferBlockMs;
        g_bufferPeakBytes = 0;
        g_bufferTrimmedPose = g_bufferTrimmedFx = g_bufferTrimmedFxTracks = 0;
        g_recTiming = {};
        g_recTiming.startMs = TicksMs(replay::prof::Now() - startedAt);
        ++g_recordingsThisLoad;
        const replay::hooks::Stats hooks = replay::hooks::Snapshot();
        log << "REC_START_TIMING,ms=" << g_recTiming.startMs << ",recording=" << g_recordingsThisLoad
            << ",single_enables=" << hooks.enables << ",single_enable_ms=" << hooks.enableMsTotal
            << ",batched=" << hooks.queued << '\n';

        log << "RECORDING_START,kind=" << (kind == RecordKind::Buffer ? "buffer" : "manual") << ",part=" << g_manualPart << '\n';
        if (kind == RecordKind::Manual)
            ShowObjective(("Redemption Editor: REC (" + KeyText(replay::keys::Action::Record) + " stops)").c_str());
    }

    void StopRecording(bool save)
    {
        if (!g_recording)
            return;
        // The buffer never becomes a file by stopping (the bench, a clip
        // load, a playback stop it); only Shift+F4 or a manual mark saves.
        if (g_recordKind == RecordKind::Buffer)
            save = false;
        // Where the stop tick goes (a long clip's stop can hold the game for hundreds
        // of ms): report, recorder stop, save copy.
        const std::uint64_t stopT0 = replay::prof::Now();
        {
            std::ofstream& log = LogFile();
            replay::prof::WriteSessionReport(log, "record");
            log.flush();
        }
        const std::uint64_t stopT1 = replay::prof::Now();

        g_recording = false;
        // The timeline is the clock every record is stamped with (the capture clock),
        // capped like the recording itself. The wall clock started earlier: a
        // recording start that stalls (observer installs, 1.0 to 1.4 s) would make the
        // timeline that much longer than the tracks, and playback would freeze for the
        // difference at the end.
        const ULONGLONG wallMs = GetTickCount64() - g_recordingStartedAt;
        const ULONGLONG elapsedMs = replay::pose::capture::NowMs();
        g_transport.SetDurationMs(elapsedMs);

        g_clip.environment.clockEndS = static_cast<std::uint32_t>(
            CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS());
        g_clip.environment.recorded = true;
        // With the recording clock paused (a mission's fade, the pause menu) the state
        // now is not the state at the end of the recorded timeline (a sample taken
        // after a fade can sweep the sun over the clip's last second). The last sample
        // before the pause stays the state at the end.
        if (!replay::pose::capture::IsClockPaused())
            RecordEnvironment(static_cast<std::uint32_t>(elapsedMs), true); // the state at the end
        else
            LogFile() << "ENV_TRACK,phase=record,why=stop_skipped_paused,time_ms=" << elapsedMs
                      << ",samples=" << g_clip.environment.track.size() << '\n';

        std::ofstream& log = LogFile();
        log << "REC_FRAME_GAPS,recording=" << g_recordingsThisLoad << ",ticks=" << g_recTiming.ticks
            << ",first_max_ms=" << g_recTiming.firstMaxMs << ",first_over33=" << g_recTiming.firstOver33
            << ",first_over100=" << g_recTiming.firstOver100 << ",max_ms=" << g_recTiming.maxMs
            << ",over100=" << g_recTiming.over100 << ",start_ms=" << g_recTiming.startMs << '\n';
        if (g_recordKind == RecordKind::Buffer)
            log << "BUFFER_STOP,save=" << (save ? 1 : 0) << ",head_ms=" << g_bufferHeadMs << ",now_ms=" << elapsedMs
                << ",peak_mb=" << g_bufferPeakBytes / (1024 * 1024) << ",trimmed_pose_chunks=" << g_bufferTrimmedPose
                << ",trimmed_fx_frames=" << g_bufferTrimmedFx << ",trimmed_fx_tracks=" << g_bufferTrimmedFxTracks << '\n';
        const std::uint64_t stopT2 = replay::prof::Now();
        replay::recorder::Stop(elapsedMs);
        const std::uint64_t stopT3 = replay::prof::Now();
        replay::doors::EndRecording(g_clip, log);
        replay::world::EndRecording(g_clip, log);
        for (replay::AbilitySpan& span : g_clip.abilities)
            span.open = false;
        replay::gate::CloseSpans(g_clip.spans, static_cast<std::uint32_t>(elapsedMs));
        log << "CAPTURE_STOP"
            << ",duration_ms=" << elapsedMs
            << ",wall_ms=" << wallMs
            << ",entities=" << PoseV2RecordedCount()
            << '\n';
        // The rope records of this recording, with Debug=0 too.
        // records: every rope recorded (none is given away any more); live_cap: the
        // engine's rope pool, the ropes alive at once; refused_full: ropes refused
        // with that many records live; freed: records of ropes the buffer's ring left.
        log << "POSE_V2_ROPE_RECORDS_SUMMARY,records=" << g_clip.ropes.Records().size()
            << ",live_cap=" << replay::ropes::kEngineRopePoolCapacity << ",max_live=" << g_clip.ropes.MaxLiveRecords()
            << ",refused_full=" << g_clip.ropes.RefusedFull() << ",freed=" << g_clip.ropes.FreedRecords()
            << ",still_samples=" << g_clip.ropes.StillSamples() << ",kb=" << replay::ropes::RopeRecordsBytes(g_clip.ropes.Records()) / 1024
            << '\n';
        log.flush();
        const std::uint64_t stopT4 = replay::prof::Now();
        if (save)
        {
            // A free name: NNN-Place. The parts of a manual recording saved in parts
            // end in -p1, -p2... like a chain inside the buffer: a split saves part
            // g_manualPart, the stop that ends the chain the next one.
            const int part = g_manualSplitting ? g_manualPart : replay::buffercov::FinalPartNumber(g_manualPart);
            SaveStoppedRecording(NewClipPath(g_clip.place, part > 0 ? (L"-p" + std::to_wstring(part)).c_str() : L""));
        }
        const std::uint64_t stopT5 = replay::prof::Now();
        g_manualInBuffer = false;
        if (!g_manualSplitting)
            g_manualPart = 0; // the chain ended; the next recording starts unnumbered
        replay::pose::capture::ReleaseFinalizer(); // the Pose Editor may take it now
        const auto stopMs = [](std::uint64_t ticks) { return static_cast<long long>(TicksMs(ticks)); };
        LogFile() << "REC_STOP_TIMING,save=" << (save ? 1 : 0) << ",report_ms=" << stopMs(stopT1 - stopT0)
                  << ",before_recorder_ms=" << stopMs(stopT2 - stopT1) << ",recorder_stop_ms=" << stopMs(stopT3 - stopT2)
                  << ",world_ms=" << stopMs(stopT4 - stopT3) << ",save_copy_ms=" << stopMs(stopT5 - stopT4)
                  << ",total_ms=" << stopMs(replay::prof::Now() - stopT0) << '\n';
    }

    // Dead Eye and Eagle Eye spans: the player's special abilities as the game
    // reports them, one span per activation.
    void RecordAbilities(std::uint32_t nowMs)
    {
        const Player player = PLAYER::PLAYER_ID();
        const bool active[2] = {PLAYER::_IS_SPECIAL_ABILITY_ACTIVE(player) != FALSE,
            PLAYER::_IS_SECONDARY_SPECIAL_ABILITY_ACTIVE(player) != FALSE};
        for (int kind = 0; kind < 2; ++kind)
        {
            replay::AbilitySpan* open = nullptr;
            for (replay::AbilitySpan& span : g_clip.abilities)
                if (span.open && static_cast<int>(span.kind) == kind)
                    open = &span;
            if (active[kind] && !open)
            {
                g_clip.abilities.push_back({nowMs, nowMs, static_cast<replay::AbilityKind>(kind), true});
                LogFile() << "ABILITY_RECORD,kind=" << (kind ? "eagle_eye" : "dead_eye") << ",start_ms=" << nowMs << '\n';
            }
            else if (open)
            {
                open->endMs = nowMs;
                if (!active[kind])
                {
                    open->open = false;
                    LogFile() << "ABILITY_RECORD,kind=" << (kind ? "eagle_eye" : "dead_eye") << ",end_ms=" << nowMs << '\n';
                }
            }
        }
    }

    // The weather manager's transition over time (weather_state.h), which
    // GET_CURR_WEATHER_STATE does not return: guarded reads at signature-checked
    // addresses. A mismatch (logged once a load) leaves the samples' transition
    // unknown, and the replay then leaves the engine's alone.
    bool g_weatherResolveLogged = false;
    bool WeatherStateReady()
    {
        std::string error;
        const bool ok = replay::weatherstate::Resolve(error);
        if (!g_weatherResolveLogged)
        {
            g_weatherResolveLogged = true;
            LogFile() << "WEATHER_TRANSITION,phase=resolve,ok=" << (ok ? 1 : 0) << ",error=" << (ok ? "none" : error.c_str())
                      << '\n';
        }
        return ok;
    }

    void ReadWeatherTransition(replay::EnvironmentSample& sample)
    {
        replay::weatherstate::Transition transition;
        if (!WeatherStateReady() || !replay::weatherstate::ReadTransition(transition) || !std::isfinite(transition.totalMs) ||
            !std::isfinite(transition.leftMs) || transition.totalMs < 0.0f)
            return;
        sample.transitionKnown = true;
        sample.transitionTo = transition.hash;
        sample.transitionTotalMs = transition.hash ? transition.totalMs : 0.0f;
        sample.transitionLeftMs = transition.hash ? transition.leftMs : 0.0f;
        sample.transitionFlag = transition.hash && transition.flag ? 1 : 0;
    }

    // The environment now (environment_track.h), for the track.
    replay::EnvironmentSample ReadEnvironmentNow(std::uint32_t nowMs)
    {
        replay::EnvironmentSample sample;
        sample.timeMs = nowMs;
        Hash from = 0, to = 0;
        float mix = 0.0f;
        MISC::GET_CURR_WEATHER_STATE(&from, &to, &mix);
        sample.weatherFrom = static_cast<std::uint32_t>(from);
        sample.weatherTo = static_cast<std::uint32_t>(to);
        sample.weatherMix = std::isfinite(mix) ? mix : 0.0f;
        sample.clockS = static_cast<std::uint32_t>(
            CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS()) %
            replay::envtrack::kDaySeconds;
        const float wind = MISC::GET_WIND_SPEED();
        sample.windSpeed = std::isfinite(wind) ? wind : 0.0f;
        const Vector3 direction = MISC::GET_WIND_DIRECTION();
        const float heading = std::atan2(direction.y, direction.x);
        sample.windDirection = std::isfinite(heading) ? heading : 0.0f;
        const float rain = MISC::GET_RAIN_LEVEL();
        const float snow = MISC::GET_SNOW_LEVEL();
        sample.rain = std::isfinite(rain) ? rain : 0.0f;
        sample.snow = std::isfinite(snow) ? snow : 0.0f;
        ReadWeatherTransition(sample);
        return sample;
    }

    // The clouds' accumulators at 10 Hz, every sample kept (they move all the
    // time), and their periods with the first. Guarded reads at signature-checked
    // addresses (sky_state.h); a mismatch leaves the clip without the track
    // (logged once a load).
    std::uint32_t g_skyNextSampleMs = 0;
    bool g_skyResolveLogged = false;
    replay::log::LogLimit g_skyRecordLogs;
    std::string SkyValuesText(const replay::SkySample& s)
    {
        using namespace replay::sky;
        std::ostringstream text;
        text << "noise=" << s.value[kNoiseX] << ';' << s.value[kNoiseY] << ';' << s.value[kNoiseZ]
             << ",position=" << s.value[kPositionX] << ';' << s.value[kPositionY] << ';' << s.value[kPositionZ]
             << ",layer0=" << s.value[kLayer0X] << ';' << s.value[kLayer0Y] << ",layer1=" << s.value[kLayer1X] << ';'
             << s.value[kLayer1Y] << ",drift=" << s.value[kDriftX] << ';' << s.value[kDriftY] << ",region=" << s.value[kRegionX]
             << ';' << s.value[kRegionY] << ",unpinned=" << s.unpinned[0] << ';' << s.unpinned[1] << ';' << s.unpinned[2];
        return text.str();
    }
    void RecordSky(std::uint32_t nowMs, bool force)
    {
        replay::Environment& environment = g_clip.environment;
        if (environment.sky.empty())
            g_skyNextSampleMs = 0;
        if (!force && nowMs < g_skyNextSampleMs)
            return;
        g_skyNextSampleMs = nowMs + replay::skytrack::kSampleMs;
        std::string error;
        if (!replay::skystate::Resolve(error))
        {
            if (!g_skyResolveLogged)
            {
                g_skyResolveLogged = true;
                LogFile() << "SKY_TRACK,phase=resolve,ok=0,error=" << error << '\n';
            }
            return;
        }
        if (environment.sky.empty() && !replay::skystate::ReadPeriods(environment.skyPeriods))
        {
            if (g_skyRecordLogs.Take(5))
                LogFile() << "SKY_TRACK,phase=record,error=period_read\n";
            return;
        }
        replay::SkySample sample;
        if (!replay::skystate::Read(sample))
        {
            if (g_skyRecordLogs.Take(5))
                LogFile() << "SKY_TRACK,phase=record,error=read,time_ms=" << nowMs << '\n';
            return;
        }
        sample.timeMs = nowMs;
        const std::size_t before = environment.sky.size();
        replay::skytrack::Add(environment.sky, sample);
        if ((before == 0 || force) && g_skyRecordLogs.Take(20))
        {
            const replay::SkyPeriods& p = environment.skyPeriods;
            LogFile() << "SKY_TRACK,phase=record,why=" << (before == 0 ? "first" : "stop") << ",time_ms=" << nowMs << ','
                      << SkyValuesText(sample) << ",period_noise=" << p.value[replay::sky::kPeriodNoise]
                      << ",period_position=" << p.value[replay::sky::kPeriodPosition]
                      << ",period_layer=" << p.value[replay::sky::kPeriodLayer]
                      << ",period_region=" << p.value[replay::sky::kPeriodRegion] << ",samples=" << environment.sky.size()
                      << '\n';
        }
    }

    // The script timecycle modifier and the weather variations, polled at 10 Hz
    // and kept when they move (always the first and the last). Guarded reads at
    // signature-checked addresses (timecycle_state.h); a mismatch leaves the clip
    // without the track (logged once a load).
    std::uint32_t g_tcNextSampleMs = 0;
    bool g_tcResolveLogged = false;
    replay::tctrack::Recorder g_tcRecorder;
    replay::log::LogLimit g_tcRecordLogs;
    std::string HashText(std::uint32_t hash)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", hash);
        return text;
    }
    std::string TimecycleText(const replay::TimecycleSample& s)
    {
        std::ostringstream text;
        text << "mod=" << HashText(s.modifierKey) << ",mod_index=" << s.modifierIndex << ",strength=" << s.strength
             << ",trans=" << HashText(s.transitionKey) << ",trans_index=" << s.transitionIndex << ",progress=" << s.progress
             << ",speed=" << s.speed << ",fade=" << static_cast<int>(s.fadeOut) << ",variations=";
        if (!s.variationCount)
            text << "none";
        for (std::size_t n = 0; n < s.variationCount; ++n)
            text << (n ? ";" : "") << WeatherName(s.variations[n].type) << ':' << HashText(s.variations[n].variation);
        return text.str();
    }
    void RecordTimecycle(std::uint32_t nowMs, bool force)
    {
        if (!g_timecycleRecord)
            return;
        std::vector<replay::TimecycleSample>& track = g_clip.environment.timecycle;
        if (track.empty())
        {
            g_tcNextSampleMs = 0;
            g_tcRecorder.Reset();
        }
        if (!force && nowMs < g_tcNextSampleMs)
            return;
        g_tcNextSampleMs = nowMs + replay::tctrack::kSampleMs;
        std::string error;
        if (!replay::tcstate::Resolve(error))
        {
            if (!g_tcResolveLogged)
            {
                g_tcResolveLogged = true;
                LogFile() << "TIMECYCLE_TRACK,phase=resolve,ok=0,error=" << error << '\n';
            }
            return;
        }
        replay::TimecycleSample sample;
        bool overflow = false;
        if (!replay::tcstate::Read(sample, &overflow))
        {
            if (g_tcRecordLogs.Take(5))
                LogFile() << "TIMECYCLE_TRACK,phase=record,error=read,time_ms=" << nowMs << '\n';
            return;
        }
        sample.timeMs = nowMs;
        const std::size_t before = track.size();
        const bool keysMoved = !track.empty() && !replay::tctrack::SameKeys(track.back(), sample);
        const bool kept = force ? g_tcRecorder.Finish(track, sample) : g_tcRecorder.Poll(track, sample);
        if (kept && (before == 0 || keysMoved || force || overflow) && g_tcRecordLogs.Take(40))
        {
            LogFile() << "TIMECYCLE_TRACK,phase=record,why=" << (before == 0 ? "first" : force ? "stop" : "change")
                      << ",time_ms=" << nowMs << ',' << TimecycleText(sample) << ",overflow=" << (overflow ? 1 : 0)
                      << ",modifiers=" << replay::tcstate::ModifierCount() << ",samples=" << track.size() << '\n';
        }
    }

    // The sky object's two moon angles, the drawn sun and moon and the clock date,
    // polled at 4 Hz and kept when an angle or the date moved (always the first
    // and the last). Guarded reads at the signature-checked object
    // (sun_control.h, no detour); a mismatch leaves the clip without the track
    // (logged once a load).
    std::uint32_t g_moonNextSampleMs = 0;
    bool g_moonResolveLogged = false;
    replay::moontrack::Recorder g_moonRecorder;
    replay::log::LogLimit g_moonRecordLogs;
    std::uint32_t LiveClockS()
    {
        return static_cast<std::uint32_t>(CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS());
    }
    std::string MoonDateText(const replay::MoonSample& s)
    {
        std::ostringstream text;
        text << s.year << '-' << static_cast<int>(s.month) << '-' << static_cast<int>(s.day);
        return text.str();
    }
    std::string MoonText(const replay::MoonSample& s)
    {
        std::ostringstream text;
        text << "angle=" << s.angle << ",orbit=" << s.orbit << ",date=" << MoonDateText(s)
             << ",sun_el=" << replay::moontrack::ElevationDeg(s.sun) << ",moon_el=" << replay::moontrack::ElevationDeg(s.moon)
             << ",moon=" << s.moon[0] << ';' << s.moon[1] << ';' << s.moon[2];
        return text.str();
    }
    // The moon now (guarded reads; the date by the clock natives).
    bool ReadMoonNow(replay::MoonSample& sample)
    {
        replay::sunctl::MoonState state;
        if (!replay::sunctl::ReadMoon(state))
            return false;
        sample.angle = state.angle;
        sample.orbit = state.orbit;
        if (state.directions)
        {
            sample.sun[0] = state.sun.x;
            sample.sun[1] = state.sun.y;
            sample.sun[2] = state.sun.z;
            sample.moon[0] = state.moon.x;
            sample.moon[1] = state.moon.y;
            sample.moon[2] = state.moon.z;
        }
        sample.year = static_cast<std::uint16_t>(std::max(0, std::min(65535, CLOCK::GET_CLOCK_YEAR())));
        sample.month = static_cast<std::uint8_t>(std::max(0, std::min(12, CLOCK::GET_CLOCK_MONTH())));
        sample.day = static_cast<std::uint8_t>(std::max(0, std::min(31, CLOCK::GET_CLOCK_DAY_OF_MONTH())));
        return true;
    }
    void RecordMoon(std::uint32_t nowMs, bool force)
    {
        if (!g_moonRecord)
            return;
        std::vector<replay::MoonSample>& track = g_clip.environment.moon;
        if (track.empty())
        {
            g_moonNextSampleMs = 0;
            g_moonRecorder.Reset();
        }
        if (!force && nowMs < g_moonNextSampleMs)
            return;
        g_moonNextSampleMs = nowMs + replay::moontrack::kSampleMs;
        const char* resolveError = replay::sunctl::ResolveError();
        if (std::strcmp(resolveError, "none") != 0)
        {
            if (!g_moonResolveLogged)
            {
                g_moonResolveLogged = true;
                LogFile() << "MOON_TRACK,phase=resolve,ok=0,error=" << resolveError << '\n';
            }
            return;
        }
        replay::MoonSample sample;
        if (!ReadMoonNow(sample))
        {
            if (g_moonRecordLogs.Take(5))
                LogFile() << "MOON_TRACK,phase=record,error=read,time_ms=" << nowMs << '\n';
            return;
        }
        sample.timeMs = nowMs;
        const std::size_t before = track.size();
        // A clock jump while recording (a mission's SET_CLOCK_TIME in the dark).
        const bool jumped = before && (std::fabs(replay::moontrack::WrapDelta(track.back().angle, sample.angle)) > replay::moontrack::kJump ||
                                          std::fabs(replay::moontrack::WrapDelta(track.back().orbit, sample.orbit)) > replay::moontrack::kJump);
        const bool kept = force ? g_moonRecorder.Finish(track, sample) : g_moonRecorder.Poll(track, sample);
        if (kept && (before == 0 || force || jumped) && g_moonRecordLogs.Take(40))
        {
            LogFile() << "MOON_TRACK,phase=record,why=" << (before == 0 ? "first" : force ? "stop" : "jump") << ",time_ms=" << nowMs
                      << ",clock_s=" << LiveClockS() << ',' << MoonText(sample) << ",samples=" << track.size() << '\n';
        }
    }

    // 4 Hz while the recording clock runs; a sample is kept when something moved
    // (and always the first and the last).
    std::uint32_t g_envNextSampleMs = 0;
    replay::log::LogLimit g_envRecordLogs; // per session
    void RecordEnvironment(std::uint32_t nowMs, bool force)
    {
        RecordSky(nowMs, force);
        RecordTimecycle(nowMs, force);
        RecordMoon(nowMs, force);
        std::vector<replay::EnvironmentSample>& track = g_clip.environment.track;
        if (track.empty())
            g_envNextSampleMs = 0;
        if (!force && nowMs < g_envNextSampleMs)
            return;
        g_envNextSampleMs = nowMs + replay::envtrack::kSampleMs;
        const replay::EnvironmentSample sample = ReadEnvironmentNow(nowMs);
        const std::size_t before = track.size();
        const bool weatherChanged = !track.empty() &&
            (track.back().weatherFrom != sample.weatherFrom || track.back().weatherTo != sample.weatherTo);
        // A transition started, ended or strayed.
        const bool transitionChanged = !track.empty() && replay::envtrack::TransitionChanged(track.back(), sample);
        if (!replay::envtrack::Add(track, sample) && force && !track.empty() && track.back().timeMs < nowMs)
            track.push_back(sample); // the last state at the stop
        if (track.size() != before && (before == 0 || weatherChanged || transitionChanged || force) && g_envRecordLogs.Take(40))
        {
            LogFile() << "ENV_TRACK,phase=record,time_ms=" << nowMs << ",from=" << WeatherName(sample.weatherFrom)
                      << ",to=" << WeatherName(sample.weatherTo) << ",mix=" << sample.weatherMix << ",clock_s=" << sample.clockS
                      << ",wind=" << sample.windSpeed << ",wind_dir=" << sample.windDirection << ",rain=" << sample.rain
                      << ",snow=" << sample.snow << ",tr_known=" << (sample.transitionKnown ? 1 : 0)
                      << ",tr_to=" << (sample.transitionTo ? WeatherName(sample.transitionTo) : std::string("none"))
                      << ",tr_total_ms=" << sample.transitionTotalMs << ",tr_left_ms=" << sample.transitionLeftMs
                      << ",tr_flag=" << static_cast<int>(sample.transitionFlag) << ",samples=" << track.size()
                      << ",why=" << (before == 0 ? "first" : force ? "stop" : weatherChanged ? "weather" : "transition") << '\n';
        }
    }

    // The recording gate's raw signals this tick (record_gate.h; every native
    // checked in inc/natives.h). A script camera counts only when it is not one of
    // ours (free camera, replay camera, the bench's).
    std::uint32_t ReadGateSignals()
    {
        using namespace replay::gate;
        std::uint32_t bits = 0;
        const Player player = PLAYER::PLAYER_ID();
        const Ped playerPed = PLAYER::PLAYER_PED_ID();
        if (HUD::IS_PAUSE_MENU_ACTIVE())
            bits |= SignalPauseMenu;
        if (CAM::_IS_ANIM_SCENE_CAM_ACTIVE())
            bits |= SignalAnimSceneCam;
        const Cam rendering = CAM::GET_RENDERING_CAM();
        if (rendering > 0 && CAM::DOES_CAM_EXIST(rendering) && rendering != g_freeCam && rendering != g_replayCam &&
            rendering != replay::bench::OwnCamera())
            bits |= SignalScriptCam;
        if (CAM::IS_GAMEPLAY_CAM_RENDERING())
            bits |= SignalGameplayCam;
        if (ANIMSCENE::IS_ENTITY_PLAYING_ANIM_SCENE(playerPed, -1))
            bits |= SignalPlayerAnimScene;
        if (CAM::HAS_LETTER_BOX())
            bits |= SignalLetterbox;
        if (!PLAYER::IS_PLAYER_CONTROL_ON(player))
            bits |= SignalControlOff;
        if (CAM::IS_SCREEN_FADED_OUT())
            bits |= SignalFadedOut;
        if (DLC::GET_IS_LOADING_SCREEN_ACTIVE() || SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
            bits |= SignalLoading;
        if (CAM::IS_CINEMATIC_CAM_RENDERING())
            bits |= SignalCinematicCam;
        if (MISC::GET_MISSION_FLAG())
            bits |= SignalMission;
        return bits;
    }

    std::string GateSignalText(std::uint32_t bits)
    {
        using namespace replay::gate;
        char text[256]{};
        std::snprintf(text, sizeof(text),
            "bits=0x%x,menu=%u,anim_scene_cam=%u,script_cam=%u,gameplay_cam=%u,player_anim_scene=%u,letterbox=%u,"
            "control_off=%u,faded_out=%u,loading=%u,cinematic_cam=%u,mission=%u",
            bits, (bits & SignalPauseMenu) ? 1u : 0u, (bits & SignalAnimSceneCam) ? 1u : 0u, (bits & SignalScriptCam) ? 1u : 0u,
            (bits & SignalGameplayCam) ? 1u : 0u, (bits & SignalPlayerAnimScene) ? 1u : 0u, (bits & SignalLetterbox) ? 1u : 0u,
            (bits & SignalControlOff) ? 1u : 0u, (bits & SignalFadedOut) ? 1u : 0u, (bits & SignalLoading) ? 1u : 0u,
            (bits & SignalCinematicCam) ? 1u : 0u, (bits & SignalMission) ? 1u : 0u);
        return text;
    }

    void ResetRecordGate()
    {
        g_recordGate.Reset();
        g_spanScriptCam.Reset();
        g_spanControlsOff.Reset();
        g_gateReasons = g_gatePauseReasons = g_gateSignals = 0;
        g_gateLogged = false;
        g_gateSignalLogs = g_gateSampleLogs = 0;
        g_gateNextSampleAt = 0;
    }

    // One tick of the recording gate. The clock stops for the pause menu, a faded
    // out screen, a loading screen and (unless RecordCutscenes=1) a cutscene; it
    // goes on with a cut when the pause was more than the menu. The
    // restricted-camera spans are kept while the clock runs.
    void UpdateRecordGate()
    {
        using namespace replay::gate;
        const std::uint32_t signals = ReadGateSignals();
        const Decision decision = g_recordGate.Update(signals, g_recConfig.recordCutscenes);
        const bool wasPaused = replay::pose::capture::IsClockPaused();
        if (decision.paused)
            g_gatePauseReasons |= decision.reasons;
        std::ofstream& log = LogFile();
        bool cut = false;
        if (!decision.paused && wasPaused)
        {
            cut = PauseMakesCut(g_gatePauseReasons);
            if (cut)
            {
                replay::pose::capture::MarkCut();
                AddCut(g_clip.cuts, replay::pose::capture::NowMs());
            }
        }
        if (decision.paused != wasPaused)
            replay::pose::capture::SetClockPaused(decision.paused);
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (decision.paused != wasPaused || decision.reasons != g_gateReasons || !g_gateLogged)
        {
            g_gateLogged = true;
            log << "RECORD_PAUSE,paused=" << (decision.paused ? 1 : 0)
                << ",reason=" << ReasonName(decision.paused ? decision.reasons : (wasPaused ? g_gatePauseReasons : decision.reasons))
                << ",reasons=0x" << std::hex << decision.reasons << std::dec << ",cutscene=" << (decision.cutscene ? 1 : 0)
                << ",cut=" << (cut ? 1 : 0) << "," << GateSignalText(signals) << ",record_cutscenes="
                << (g_recConfig.recordCutscenes ? 1 : 0) << ",now_ms=" << nowMs
                << ",paused_total_ms=" << replay::pose::capture::PausedMsTotal() << '\n';
        }
        // The signal changes and the calibration samples are evidence for the bench
        // (Debug=1); RECORD_PAUSE stays in both.
        else if (signals != g_gateSignals && replay::log::Verbose() && g_gateSignalLogs < 200)
        {
            ++g_gateSignalLogs;
            log << "RECORD_GATE_SIGNALS," << GateSignalText(signals) << ",paused=" << (decision.paused ? 1 : 0)
                << ",now_ms=" << nowMs << '\n';
        }
        // Calibration: raw bits while a script camera renders, capped.
        const ULONGLONG wall = GetTickCount64();
        if ((signals & SignalScriptCam) && replay::log::Verbose() && wall >= g_gateNextSampleAt && g_gateSampleLogs < 120)
        {
            ++g_gateSampleLogs;
            g_gateNextSampleAt = wall + 1000;
            log << "RECORD_GATE_SAMPLE," << GateSignalText(signals) << ",cutscene=" << (decision.cutscene ? 1 : 0)
                << ",paused=" << (decision.paused ? 1 : 0) << ",now_ms=" << nowMs << '\n';
        }
        if (!decision.paused && wasPaused)
            g_gatePauseReasons = 0;
        g_gateReasons = decision.reasons;
        g_gateSignals = signals;

        // Spans: hysteresis every tick, spans only while the clock runs.
        g_spanScriptCam.Update((signals & SignalScriptCam) && !(signals & SignalGameplayCam));
        g_spanControlsOff.Update((signals & SignalControlOff) != 0);
        if (!decision.paused)
        {
            UpdateSpan(g_clip.spans, SpanKind::Cutscene, decision.cutscene && g_recConfig.recordCutscenes, nowMs);
            UpdateSpan(g_clip.spans, SpanKind::ScriptCamera, g_spanScriptCam.active, nowMs);
            UpdateSpan(g_clip.spans, SpanKind::ControlsOff, g_spanControlsOff.active, nowMs);
        }
    }

    // A tick that comes a second or more after the previous one means the game did
    // not run the script thread (its pause menu freezes it, so the gate never sees
    // the menu): that time leaves the recording clock as a pause. First thing in
    // the tick, before a save or a mark reads the clock.
    void ExcludeRecordingStall()
    {
        if (!g_recording)
            return;
        constexpr std::uint32_t kScriptStallMs = 1000;
        const std::uint32_t stallMs = replay::pose::capture::ExcludeScriptStall(kScriptStallMs);
        if (!stallMs)
            return;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        const std::uint32_t pausedTotal = replay::pose::capture::PausedMsTotal();
        std::ofstream& log = LogFile();
        log << "RECORD_PAUSE,paused=1,reason=script_stall,stall_ms=" << stallMs << ",now_ms=" << nowMs
            << ",paused_total_ms=" << pausedTotal << '\n';
        log << "RECORD_PAUSE,paused=0,reason=script_stall,stall_ms=" << stallMs << ",now_ms=" << nowMs
            << ",paused_total_ms=" << pausedTotal << '\n';
    }

    // A first person scope (binoculars, a rifle scope, the camera kit) looked
    // through from third person play puts the rendered camera inside the
    // character's head; the clip records a third person camera there instead
    // (camera::ThirdPersonDuringScope). The game's own scripts test the binoculars
    // as the kit weapon plus IS_FIRST_PERSON_AIM_CAM_ACTIVE; full first person play
    // is left as recorded. The scope's blend in and out (the camera nearer the
    // head than kScopeHeadClearance) counts as scope too.
    replay::camera::ScopeAnchor g_scopeAnchor;

    void RecordThirdPersonCamera(replay::editor::CameraState& rendered, std::uint32_t cameraMs)
    {
        if (g_clip.gameCamera.empty())
            g_scopeAnchor = {}; // a new recording
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return;
        const Vector3 root = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        const Vector3 head = PED::GET_PED_BONE_COORDS(player, 21030, 0.0f, 0.0f, 0.0f); // SKEL_Head
        const float dx = rendered.position.x - head.x, dy = rendered.position.y - head.y, dz = rendered.position.z - head.z;
        const bool nearHead = std::sqrt(dx * dx + dy * dy + dz * dz) < replay::camera::kScopeHeadClearance;
        const bool scope = CAM::IS_FIRST_PERSON_AIM_CAM_ACTIVE() && !CAM::_IS_IN_FULL_FIRST_PERSON_MODE();
        const replay::editor::Vec3 at{root.x, root.y, root.z};
        const bool wasInScope = g_scopeAnchor.inScope;
        if (scope || (wasInScope && nearHead))
        {
            const bool anchored = g_scopeAnchor.valid;
            rendered = replay::camera::ThirdPersonDuringScope(g_scopeAnchor, rendered, at);
            if (!wasInScope)
                LogFile() << "GAME_CAMERA_SCOPE,phase=start,ms=" << cameraMs << ",anchor=" << (anchored ? 1 : 0) << '\n';
            return;
        }
        if (wasInScope)
            LogFile() << "GAME_CAMERA_SCOPE,phase=end,ms=" << cameraMs << '\n';
        if (!nearHead)
            replay::camera::TrackThirdPerson(g_scopeAnchor, rendered, at);
        else
            g_scopeAnchor.inScope = false;
    }

    void UpdateRecording(const ProbeFrame& frame)
    {
        REPLAY_PROFILE_ZONE("tick.recording");
        if (!g_recording)
            return;

        // The recording gate (pause menu, cutscene, fade, loading screen). While it
        // holds, the recording clock and every observer stop (one check:
        // pose::capture::IsClockPaused); the detour queues are drained and discarded.
        UpdateRecordGate();

        replay::recorder::Update(frame.mount);
        if (!replay::pose::capture::IsClockPaused())
        {
            {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                replay::doors::UpdateRecording(g_clip, replay::pose::capture::NowMs(), at.x, at.y, at.z);
                // The map state, a slice of the candidates per tick.
                const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const float player[3] = {at.x, at.y, at.z};
                const float camera[3] = {cam.x, cam.y, cam.z};
                replay::world::UpdateRecording(g_clip, replay::pose::capture::NowMs(), player, camera, LogFile());
            }
            RecordAbilities(replay::pose::capture::NowMs());
            RecordEnvironment(replay::pose::capture::NowMs(), false);

            // The rendered camera, one sample per captured frame: the clip's game camera.
            const std::uint32_t cameraMs = replay::pose::capture::NowMs();
            if (g_clip.gameCamera.empty() || g_clip.gameCamera.back().timeMs < cameraMs)
            {
                const Vector3 position = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const Vector3 rotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
                replay::editor::CameraState rendered{{position.x, position.y, position.z},
                    {rotation.x, rotation.y, rotation.z}, CAM::GET_FINAL_RENDERED_CAM_FOV()};
                RecordThirdPersonCamera(rendered, cameraMs);
                g_clip.gameCamera.push_back({cameraMs, rendered.position, rendered.rotation, rendered.fov});
            }
        }

        UpdateRecordingLimits();
    }
    float Lerp(float from, float to, float amount)
    {
        return from + (to - from) * amount;
    }
    void StopFreeCam()
    {
        g_freeCamVelocity = {};
        g_freeCamSmoothedLookX = 0.0f;
        g_freeCamSmoothedLookY = 0.0f;
        if (!g_freeCamEnabled && !g_freeCam)
            return;

        CAM::RENDER_SCRIPT_CAMS(FALSE, TRUE, 250, TRUE, TRUE, 0);
        if (g_freeCam && CAM::DOES_CAM_EXIST(g_freeCam))
            CAM::DESTROY_CAM(g_freeCam, FALSE);

        if (g_freeCamPlayer && ENTITY::DOES_ENTITY_EXIST(g_freeCamPlayer) && !g_freeCamPlayerWasFrozen)
            ENTITY::FREEZE_ENTITY_POSITION(g_freeCamPlayer, FALSE);

        g_freeCam = 0;
        g_freeCamPlayer = 0;
        g_freeCamEnabled = false;
        g_freeCamPlayerWasFrozen = false;
        g_cameraTrackManualOverride = false;
    }

    // The clip's world in product mode: black screen while the puppets are built,
    // the clip's clock (following the playhead) and weather, the real player
    // hidden, frozen and without control; stop puts the world back. Debug mode
    // (bench, tests) leaves the world alone. Not done: preloading the clip's
    // particle assets (the asset of an effect is not known yet) and the wind.
    struct ClipWorld
    {
        bool active = false;
        bool presentation = false; // product mode: black screen, player hidden
        bool fadeInPending = false;
        // The load wait (product mode). The screen stays black and the transport held
        // until the world at the rendered camera and the puppets are ready.
        const char* loadPhase = nullptr; // "start" or "jump" while waiting
        ULONGLONG loadStartedAt = 0;
        bool loadSceneActive = false;
        replay::clipload::Vec3 loadSceneAt{};
        int loadRetargets = 0;
        float jumpMeters = 0.0f;
        int hours = 0, minutes = 0, seconds = 0;
        Hash weatherFrom = 0, weatherTo = 0;
        float weatherMix = 0.0f;
        Ped player = 0;
        bool playerVisible = true;
        bool playerFrozen = false;
        bool playerCollisionOff = false;
        bool playerInvincible = false;
        ULONGLONG nextWeatherAt = 0;
        // The environment track's last applied weather state.
        bool envApplied = false;
        Hash envFrom = 0, envTo = 0;
        float envMix = 0.0f;
        ULONGLONG envLogAt = 0;
        std::uint32_t envLogs = 0;
        // The weather transition and the wind, rain and snow overrides the world had
        // when the clip took it (given back at the end), and what the replay wrote
        // over them.
        bool transitionSaved = false;
        replay::weatherstate::Transition savedTransition;
        bool overridesSaved = false;
        replay::weatherstate::Overrides savedOverrides;
        bool transitionWritten = false;
        replay::weatherstate::Transition lastTransition; // the last write
        bool levelsForced = false;
        std::uint32_t transitionErrors = 0;
        // The clouds last pinned (ApplyClipSky).
        bool skyApplied = false;
        replay::SkySample skyLast;
        ULONGLONG skyLogAt = 0;
        std::uint32_t skyLogs = 0;
        std::uint64_t skyWrites = 0;
        // The timecycle modifier and the variations the session had (given back by
        // EndClipWorld once touched), and the playback counters for the log.
        bool tcBeforeValid = false;
        replay::tctrack::ModifierFields tcBeforeFields;
        replay::TimecycleSample tcBefore;
        bool tcTouched = false;
        // The variations were written at least once (weather not "Game (live)"); only
        // then does the end give them back.
        bool tcVariationsTouched = false;
        ULONGLONG tcLogAt = 0;
        std::uint32_t tcLogs = 0;
        std::uint64_t tcWrites = 0;
        std::uint64_t tcWriteFails = 0;
        std::uint64_t tcUnresolved = 0;
        std::uint32_t tcVariationCalls = 0;
        std::uint32_t tcVariationMissing = 0;
        // The session's moon angles when the clip took the clock (given back after it,
        // moon_track.h), the last clock row seen by ApplyClipEnvironment, and the
        // playback counters.
        bool moonSaved = false;
        double moonSavedAngle = 0.0;
        double moonSavedOrbit = 0.0;
        bool clockLive = false;
        const char* moonSource = nullptr;
        ULONGLONG moonLogAt = 0;
        std::uint32_t moonLogs = 0;
        std::uint64_t moonWrites = 0;
        std::uint64_t moonWriteFails = 0;
        // Product mode applies the clip's map state once the screen is dark, in the
        // load wait before the load scene starts.
        bool worldApplyPending = false;
        // Where the gameplay camera stood when the clip opened. Debug mode waits for
        // the world too (no black screen) unless the clip's first camera is this close
        // (the bench plays where it recorded): a clip opened with the player across
        // the map needs its load scene.
        replay::clipload::Vec3 openedAt{};
        bool openedAtValid = false;
        // Where the hidden player stood, and whether the replay moved him
        // (TrailPlayerFollow).
        replay::clipload::Vec3 playerAt{};
        float playerHeading = 0.0f;
        bool playerAtValid = false;
        bool playerMoved = false;
        Ped mover = 0; // the player, or his horse when mounted
        replay::clipload::Vec3 moverAt{};
        float moverHeading = 0.0f;
        ULONGLONG nextFollowAt = 0;
        std::uint32_t followLogs = 0;
    };

    // After a clip that moved the hidden player, he stays frozen back at his place
    // until the collision there has loaded (a debug-mode exit has no load wait:
    // unfrozen at once he could fall through). The world's hidden entities wait
    // the same way (world_landing.h, replay::playback::UpdateWorldLanding); the
    // verdict is the shared replay::landing::Decide.
    struct PlayerLanding
    {
        bool active = false;
        bool frozen = false;
        ULONGLONG deadline = 0;
        ULONGLONG since = 0;
    };
    PlayerLanding g_playerLanding;

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
    ClipWorld g_clipWorld;

    // The session's moon angles being written back after a clip (or after a script
    // reload in one), and the check one second after the last write
    // (moon_track.h).
    replay::moontrack::GiveBack g_moonGiveBack;
    const char* g_moonGiveBackWhy = "end";
    std::uint64_t g_moonGiveBackFails = 0;
    struct MoonCheck
    {
        bool pending = false;
        ULONGLONG at = 0;
        double angle = 0.0;
        double orbit = 0.0;
        std::uint32_t clockS = 0;
        const char* why = "";
    };
    MoonCheck g_moonCheck;

    // Where the replay camera renders this tick (set by UpdateReplayCamera): the
    // load scene's target and the long jump test.
    struct RenderedCamera
    {
        bool valid = false;
        replay::clipload::Vec3 position{};
        replay::clipload::Vec3 rotation{}; // degrees, rotation order 2
        std::uint64_t playheadMs = 0;
    };
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
    constexpr std::uint32_t kJumpLoadLogCap = 300;

    // RedemptionEditor.ini RestoreWorldState and RestoreWorldStateInMission.
    replay::world::PlaybackConfig WorldConfig()
    {
        replay::world::PlaybackConfig config;
        config.restore = g_recConfig.restoreWorldState;
        config.inMission = g_recConfig.restoreWorldStateInMission;
        return config;
    }

    void StopClipLoadScene()
    {
        if (g_clipWorld.loadSceneActive)
            STREAMING::LOAD_SCENE_STOP();
        g_clipWorld.loadSceneActive = false;
    }

    // The load scene started ahead of a marker cut far away (the same frustum load
    // as the clip's), stopped once the cut is past and loaded, or as soon as
    // anything else needs the load scene (camera_track.h PlanPrefetch).
    struct CamPrefetch
    {
        bool active = false;
        std::uint64_t cutMs = 0;
        float meters = 0.0f;
        ULONGLONG startedAt = 0;
        bool cutSeen = false;
        ULONGLONG cutAt = 0;
    };
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
        STREAMING::LOAD_SCENE_START(camera.position.x, camera.position.y, camera.position.z, dirX, dirY, dirZ, 500.0f, 4);
        g_clipWorld.loadSceneActive = true;
        g_clipWorld.loadSceneAt = camera.position;
    }

    // The start's wait for the map objects the closed theater's clear turned into
    // dummies (doors included): the object pool count holding and the moving doors
    // near the first camera found, bounded by MapObjectsWaitMs (door_rebind.h).
    std::uint32_t WorldPoolUsed(replay::pools::Kind kind);
    replay::doors::MapSettle g_mapSettle;
    bool g_objectStandsWaitLogged = false; // the scenery wait's line, once per wait
    constexpr float kMapSettleDoorRadiusM = 60.0f;
    constexpr ULONGLONG kMapSettleAfterClearMs = 20000; // a clear this recent is this playback's

    void BeginClipLoadWait(const char* phase, ULONGLONG now)
    {
        g_clipWorld.loadPhase = phase;
        g_clipWorld.loadStartedAt = now;
        g_clipWorld.loadRetargets = 0;
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
                  << ",stagger=" << (g_clipStartConfig.stagger ? 1 : 0) << ",entities=" << g_clip.created.size()
                  << ",bodies=" << bodies << ",objects=" << objects << ",models=" << distinct(models)
                  << ",opening_entities=" << opening << ",opening_models=" << distinct(openingModels)
                  << ",ipl_toggles=" << (pressure.iplToggles - g_clipStartTogglesBefore)
                  << ",ipl_restored=" << pressure.lastRestoreIpls << ",ipl_on=" << pressure.lastApplyOn
                  << ",ipl_off=" << pressure.lastApplyOff << ",sets=" << pressure.lastApplySets
                  << ",ms_since_ipl_apply=" << (pressure.lastApplyAtMs && now >= pressure.lastApplyAtMs
                         ? static_cast<long long>(now - pressure.lastApplyAtMs) : -1LL)
                  << ",streaming_requests=" << STREAMING::GET_NUMBER_OF_STREAMING_REQUESTS()
                  << ",pool_peds=" << static_cast<int>(peds) << ",pool_vehicles=" << static_cast<int>(vehicles)
                  << ",pool_objects=" << static_cast<int>(poolObjects)
                  << ",ms_since_start=" << (g_clipStart.BeganAt() && now >= g_clipStart.BeganAt() ? now - g_clipStart.BeganAt() : 0)
                  << ",batch=" << (g_clipStartConfig.stagger ? g_clipStartConfig.batch : 0) << '\n';
    }

    // ClipStartStagger: the puppets start now (the world is ready, or the
    // cap ran out), then build in batches (BeginStartBatchTick each tick).
    void ReleaseClipStart(ULONGLONG now, const char* reason, const replay::clipstart::WorldFacts& facts)
    {
        LogFile() << "CLIP_START_STAGGER,phase=release,reason=" << reason << ",wait_ms=" << (now - g_clipStart.BeganAt())
                  << ",settle_ms=" << (g_clipStart.ReadyAt() ? static_cast<long long>(now - g_clipStart.ReadyAt()) : -1LL)
                  << ",timed_out=" << (g_clipStart.TimedOut() ? 1 : 0) << ",ipls_applied=" << (facts.iplsApplied ? 1 : 0)
                  << ",scene_loaded=" << (facts.sceneLoaded ? 1 : 0) << ",collision=" << (facts.collisionLoaded ? 1 : 0)
                  << ",interiors=" << (facts.interiorsReady ? 1 : 0) << ",map_settled=" << (facts.mapSettled ? 1 : 0)
                  << ",streaming=" << facts.streamingRequests << ",batch=" << g_clipStartConfig.batch << '\n';
        g_clipStartBatchBefore = replay::playback::g_startBatchCreates;
        replay::playback::Start();
        LogStreamPressure("release", reason, now);
    }

    // Every tick before the playback's update: the stagger's fallbacks (no
    // load wait left to feed it) and the shared create budget while it
    // batches; done once the opening census is built.
    void UpdateClipStart(ULONGLONG now)
    {
        if (g_clipStart.Holding() && (!g_transport.Playing() || !ClipLoadWaiting()))
        {
            replay::clipstart::WorldFacts facts;
            facts.iplsApplied = !g_clipWorld.worldApplyPending;
            g_clipStart.Feed(now, facts); // marks the phases for the log; the release is unconditional here
            if (g_transport.Playing())
                ReleaseClipStart(now, "no_wait", facts);
            else
                g_clipStart.Reset();
        }
        if (g_clipStart.Batching() && g_transport.Playing() && !replay::playback::Preparing())
        {
            g_clipStart.PuppetsBuilt();
            LogFile() << "CLIP_START_STAGGER,phase=done,build_ms=" << (now - g_clipStart.ReleasedAt())
                      << ",creates=" << (replay::playback::g_startBatchCreates - g_clipStartBatchBefore)
                      << ",total_ms=" << (now - g_clipStart.BeganAt()) << '\n';
        }
        replay::playback::BeginStartBatchTick(g_clipStart.Batching() ? g_clipStartConfig.batch : 0u);
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
            if (replay::clipload::NeedsRetarget(world.loadSceneActive, world.loadSceneAt, camera.position))
            {
                world.loadRetargets += world.loadSceneActive ? 1 : 0;
                StartClipLoadScene(camera);
            }
            STREAMING::REQUEST_COLLISION_AT_COORD(camera.position.x, camera.position.y, camera.position.z);
        }
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
        bool worldReady = replay::world::Ready(); // interiors whose entity sets changed
        if (g_mapSettle.Active())
        {
            // The map objects back and the doors near the camera found before the
            // playhead runs.
            const std::uint32_t objects = WorldPoolUsed(replay::pools::Kind::Object);
            const std::uint32_t doors = camera.valid ? replay::doors::MissingNear(g_clip, camera.position.x,
                camera.position.y, camera.position.z, kMapSettleDoorRadiusM) : 0u;
            const replay::doors::MapSettle::Verdict map =
                g_mapSettle.Feed(now, objects == 0xFFFFFFFFu ? 0u : objects, doors);
            if (map.done)
                LogFile() << "WORLD_MODEL,phase=map_objects,ms=" << g_mapSettle.EndMs() << ",objects=" << g_mapSettle.LastObjects()
                          << ",doors_missing=" << g_mapSettle.DoorsMissing() << ",timeout=" << (map.timedOut ? 1 : 0)
                          << ",cap_ms=" << replay::doors::GetConfig().mapWaitMs << '\n';
            else
                worldReady = false;
        }
        // The stand-ins of the world objects and of the scenery due at the playhead
        // built before it runs (object_check.h SceneryReady), like the doors above,
        // bounded by the same MapObjectsWaitMs.
        if (worldReady && !g_objectStandsWaitLogged && !g_clipStart.Holding())
        {
            const std::uint32_t pending = replay::objects::PendingObjectStands(static_cast<std::uint32_t>(g_transport.PlayheadMs()));
            // The stand-ins are entities the stagger holds; their wait counts from the
            // release.
            const std::uint64_t waitFrom = (std::max<std::uint64_t>)(world.loadStartedAt, g_clipStart.ReleasedAt());
            const std::uint64_t waited = now > waitFrom ? now - waitFrom : 0;
            if (replay::objcheck::SceneryReady(pending, waited, replay::doors::GetConfig().mapWaitMs))
            {
                g_objectStandsWaitLogged = true;
                LogFile() << "WORLD_MODEL,phase=object_stands,ms=" << waited << ",pending=" << pending
                          << ",timeout=" << (pending ? 1 : 0) << ",phase_name=" << world.loadPhase << '\n';
            }
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
            facts.streamingRequests = STREAMING::GET_NUMBER_OF_STREAMING_REQUESTS();
            if (g_clipStart.Holding())
            {
                const replay::clipstart::Step step = g_clipStart.Feed(now, facts);
                if (step.release)
                    ReleaseClipStart(now, step.timedOut ? "world_cap" : "world_ready", facts);
            }
            if (g_clipStartConfig.worldWait)
                waitWorldReady = waitWorldReady && collisionLoaded;
            waitConfig.timeoutMs = replay::clipstart::StartWaitTimeoutMs(g_clipStartConfig, waitConfig.timeoutMs);
            const std::uint32_t bits = replay::clipstart::FactsBits(facts);
            if (g_clipStartConfig.log &&
                replay::clipstart::LogThisTick(g_clipWorldLines, bits != g_clipWorldLastBits, now, g_clipWorldLastLogMs))
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
                          << ",gate=" << replay::clipstart::PhaseText(g_clipStart.CurrentPhase())
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
        // The held puppets are not ready, and a release in this tick has just started
        // them (Preparing read again).
        const bool puppetsReadyNow = !replay::playback::Preparing() && !g_clipStart.Holding();
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
                  << ",cap_ms=" << waitConfig.timeoutMs << ",gate=" << replay::clipstart::PhaseText(g_clipStart.CurrentPhase()) << '\n';
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
    // A state file of the base folder (storage_paths.h), for the next load.
    std::wstring StateFile(const char* name) { return replay::storage::Path(replay::identity::AsciiW(name)); }

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
        if (FILE* file = _wfopen(StateFile(kClipWorldStateFile).c_str(), L"w"))
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

    // `afterRestart`: the game restarted its scripts (a save loaded, the mod's own
    // reload): the clock, the weather and the place in the file are the old
    // session's and the load brings the new one's, and the load scene, focus and
    // fade are the game's own during its loading screen. Only the player's flags
    // go back (visible, not frozen, collision, control: what the replay changed on
    // him).
    void RestoreLeftClipWorld(bool afterRestart = false)
    {
        FILE* file = _wfopen(StateFile(kClipWorldStateFile).c_str(), L"r");
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
        const int read = std::fscanf(file,
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
            if (std::fscanf(file, "%d", &count) == 1 && count >= 0 && count <= static_cast<int>(replay::tctrack::kMaxVariations))
            {
                tcVariationsRead = true;
                for (int n = 0; n < count && tcVariationsRead; ++n)
                {
                    unsigned type = 0, variation = 0;
                    if (std::fscanf(file, "%u %u", &type, &variation) == 2)
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
            while (std::fscanf(file, "%15s", token) == 1)
            {
                if (std::strcmp(token, "moon") != 0)
                    continue;
                if (std::fscanf(file, "%d %lf %lf", &moonValid, &moonAngle, &moonOrbit) != 3 || !std::isfinite(moonAngle) ||
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

    void ApplyClipWeather()
    {
        const replay::Environment& environment = g_clip.environment;
        MISC::SET_CURR_WEATHER_STATE(environment.weatherFrom, environment.weatherTo, environment.weatherMix, TRUE);
    }

    // The clip's environment at the playhead: the clock and the weather pair and
    // mix of the track, interpolated. The weather is set again when it moved or
    // once a second (the weather system moves on by itself). Where the clip starts
    // for the player (the trim's in for a project clip): the override clock and
    // the weather blend count from there.
    std::uint64_t EnvironmentOriginMs();

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
        if (!g_timecycleReplay || g_clip.environment.timecycle.empty())
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

    // The clip's clock and weather apply in both modes (they are the clip's own
    // data); the black screen and the hidden player are the product's presentation
    // only.
    // ---- the game's score off while the replay owns the world ----
    // (src/music_stop.h). Rung 1 only: public natives on the script thread.
    replay::music::Guard g_music;

    const char* MusicModeText(std::uint32_t value)
    {
        return value == 0 ? "0" : value == 1 ? "1" : "1_plan_b_not_built";
    }

    void ApplyMusicPlan(const replay::music::Plan& plan)
    {
        if (plan.setFlag)
            AUDIO::SET_AUDIO_FLAG(replay::music::kFlagName, plan.flagValue ? TRUE : FALSE);
        if (plan.stopScenes)
            AUDIO::STOP_AUDIO_SCENES(); // this script's slot only (the scenes this script started)
    }

    // The replay takes the world (end of BeginClipWorld, the screen going black in
    // product mode). With the save reload this goes after the scripts' cleanup:
    // their FAIL/STOP events would play over a stop made before it.
    void MusicEnter()
    {
        const replay::music::Mode mode = replay::music::ModeFromIni(g_recConfig.musicStop);
        const bool playingBefore = AUDIO::AUDIO_IS_MUSIC_PLAYING() != FALSE;
        const int playtime = AUDIO::GET_MUSIC_PLAYTIME();
        const replay::music::Plan plan = g_music.Enter(mode, GetTickCount64());
        if (mode == replay::music::Mode::Off)
        {
            LogFile() << "MUSIC_STOP,phase=enter,mode=0,playing_before=" << (playingBefore ? 1 : 0) << ",action=none\n";
            return;
        }
        if (plan.already)
        {
            LogFile() << "MUSIC_STOP,phase=enter,mode=" << MusicModeText(g_recConfig.musicStop) << ",already=1\n";
            return;
        }
        ApplyMusicPlan(plan);
        const bool eventOk = plan.triggerStop && AUDIO::TRIGGER_MUSIC_EVENT(replay::music::kStopEvent) != FALSE;
        LogFile() << "MUSIC_STOP,phase=enter,mode=" << MusicModeText(g_recConfig.musicStop)
                  << ",playing_before=" << (playingBefore ? 1 : 0) << ",playtime_ms=" << playtime
                  << ",flag=1,event=" << replay::music::kStopEvent << ",event_ok=" << (eventOk ? 1 : 0)
                  << ",internal=skip\n";
    }

    // The replay gives the world back: StopPlayback (every stop, also the one at
    // the start of each playback and a Stop in the middle) and the unload. Does
    // nothing without an entry. With the save reload, call this before the load
    // is pushed (reload=1); if the mod's script dies in the load the engine frees
    // its slot and the flag falls anyway, the FALSE covers the case where it
    // survives.
    void MusicExit(const char* why, bool reload = false)
    {
        const replay::music::Plan plan = g_music.Exit();
        if (plan.already)
            return;
        ApplyMusicPlan(plan);
        LogFile() << "MUSIC_STOP,phase=exit,why=" << why << ",reload=" << (reload ? 1 : 0)
                  << ",flag=0,scenes_stopped=" << (plan.stopScenes ? 1 : 0) << ",restops=" << g_music.Restops() << '\n';
    }

    // Every tick: the sensor once a second while the flag is on.
    void MusicWatch(ULONGLONG now)
    {
        const replay::music::WatchResult result =
            g_music.Watch(now, [] { return AUDIO::AUDIO_IS_MUSIC_PLAYING() != FALSE; });
        if (result == replay::music::WatchResult::Idle)
            return;
        const std::uint64_t since = g_music.SinceEntryMs(now);
        if (result == replay::music::WatchResult::Changed || g_debugMode)
            LogFile() << "MUSIC_WATCH,t_ms=" << since << ",playing=" << (g_music.Playing() ? 1 : 0) << '\n';
        if (result != replay::music::WatchResult::Restop)
            return;
        const bool eventOk = AUDIO::TRIGGER_MUSIC_EVENT(replay::music::kStopEvent) != FALSE;
        AUDIO::SET_AUDIO_FLAG(replay::music::kFlagName, TRUE); // same bit: counted once
        LogFile() << "MUSIC_STOP,phase=restop,why=playing_again,t_ms=" << since << ",event_ok=" << (eventOk ? 1 : 0) << '\n';
    }

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
        if (!g_recConfig.trailPlayerFollow || !world.playerAtValid)
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
        if (g_worldFocus && g_worldFocusTracker.owned && g_transport.Playing())
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
        if (g_timecycleReplay && !g_clip.environment.timecycle.empty())
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

    // Back to the game behind a loading screen too: the screen stays black while
    // the world around the gameplay camera streams (the load scene, the same rules
    // as the clip load: minimum black, timeout), then fades in.
    struct ExitLoadWait
    {
        bool active = false;
        bool sceneActive = false;
        ULONGLONG startedAt = 0;
    };
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
        // A clip opened again: its own wait takes the black screen over.
        if (g_clipWorld.active)
        {
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

    // A camera target's puppet: where it stands and where it faces.
    bool TargetState(std::uint32_t target, Vector3& position, float& heading)
    {
        const replay::playback::Puppet* puppet = g_puppets.Find(replay::entity::ReplayId::FromPacked(target));
        if (!target || !puppet || !puppet->clone || !ENTITY::DOES_ENTITY_EXIST(puppet->clone))
            return false;
        position = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
        heading = ENTITY::GET_ENTITY_HEADING(puppet->clone);
        return true;
    }

    // A Follow keyframe keeps the offset it had from its target at its own
    // time: the command seeks there, and the offset is read once the
    // puppets stand at that time (after the playback update of the tick).
    std::uint32_t g_pendingFollowKeyframe = 0;

    void ResolvePendingFollow()
    {
        if (!g_pendingFollowKeyframe)
            return;
        const replay::camera::Keyframe* key = g_cameraTrack.Find(g_pendingFollowKeyframe);
        Vector3 at{};
        float heading = 0.0f;
        if (key && !replay::playback::Preparing() && TargetState(key->target, at, heading))
        {
            const replay::editor::Vec3 world{key->pose.position.x - at.x, key->pose.position.y - at.y, key->pose.position.z - at.z};
            const replay::editor::Vec3 offset = replay::camera::ToHeadingFrame(world, heading);
            g_cameraTrack.SetMode(key->id, replay::editor::CameraMode::Follow, key->target, offset);
            LogFile() << "CAMERA_KEY,action=follow,id=" << key->id << ",offset=" << offset.x << '/' << offset.y << '/' << offset.z << '\n';
            g_pendingFollowKeyframe = 0;
        }
        else if (!key)
            g_pendingFollowKeyframe = 0;
    }

    // Camera markers: the evaluator is pure (camera_track); the engine gives it the
    // puppets.
    constexpr int kHeadBone = 21030; // SKEL_Head

    // The live puppet read by natives (the fallback).
    bool LiveMarkerEntity(const replay::playback::Puppet* puppet, replay::camera::EntityState& out)
    {
        if (!puppet || !puppet->clone || !ENTITY::DOES_ENTITY_EXIST(puppet->clone))
            return false;
        const Vector3 p = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
        const Vector3 r = ENTITY::GET_ENTITY_ROTATION(puppet->clone, 2);
        out.position = {p.x, p.y, p.z};
        out.pitch = r.x;
        out.roll = r.y;
        out.heading = r.z;
        out.hasHead = false;
        if (puppet->packet.kind == replay::entity::EntityKind::Ped || puppet->packet.kind == replay::entity::EntityKind::Horse)
        {
            const Vector3 head = PED::GET_PED_BONE_COORDS(puppet->clone, kHeadBone, 0.0f, 0.0f, 0.0f);
            if (head.x != 0.0f || head.y != 0.0f || head.z != 0.0f)
            {
                out.hasHead = true;
                out.head = {head.x, head.y, head.z};
            }
        }
        return true;
    }

    // The camera follows the target's RECORDED root at the asked time
    // (replay_playback.h RecordedBodyAt), and its head from the recorded pose.
    // ReplayCamTargetRecorded=0: the live puppet. The head bone index is the
    // engine's (GET_PED_BONE_INDEX on the puppet) and is checked against the live
    // head at the playhead: three misses over 0.5 m with no match turn it off for
    // that entity and record (the head then rides the recorded root with the live
    // offset).
    struct CamTargetHead
    {
        std::uint32_t id = 0;
        std::uint32_t record = 0;
        int bone = -1;      // -1: not asked yet; -2: no head bone
        std::uint8_t good = 0;
        std::uint8_t bad = 0;
        bool off = false;
    };
    std::vector<CamTargetHead> g_camTargetHeads;
    struct CamTargetCache
    {
        std::uint32_t id = 0;
        std::uint64_t timeMs = 0;
        replay::camera::EntityState state;
    };
    CamTargetCache g_camTargetCache[8];
    std::uint32_t g_camTargetCacheNext = 0;
    // Per CAMERA_CUT_RATE second.
    std::uint32_t g_camTargetRecordedHits = 0;
    std::uint32_t g_camTargetLiveHits = 0;
    std::uint32_t g_camTargetDriftOver5cm = 0;
    float g_camTargetDriftMaxM = 0.0f;
    replay::log::LogLimit g_camTargetLogs;

    // The bone a marker is attached to or looks at. A named bone becomes the
    // skeleton index once per puppet (GET_ENTITY_BONE_INDEX_BY_NAME on its clone,
    // the index of the recorded pose's object space bones, as the head above); the
    // bone at a time is the recorded root times the recorded bone
    // (camera::EntityFromBone), the live clone only without a record there. Caches
    // keyed on the clone handle: a puppet built again asks again.
    struct CamBoneIndex
    {
        std::uint32_t id = 0;
        Entity clone = 0;
        std::uint32_t ref = 0;
        int index = -1;
    };
    std::vector<CamBoneIndex> g_camBoneIndex;
    struct CamBoneCache
    {
        std::uint32_t id = 0;
        std::uint32_t ref = 0;
        std::uint64_t timeMs = 0;
        replay::camera::EntityState state;
    };
    CamBoneCache g_camBoneCache[8];
    std::uint32_t g_camBoneCacheNext = 0;
    struct CamBoneNote
    {
        std::uint32_t id = 0;
        std::uint32_t ref = 0;
        std::uint8_t checks = 0;
        bool missingLogged = false;
    };
    std::vector<CamBoneNote> g_camBoneNotes;
    // The editor's per target bone facts (family, named bones it has, the
    // recorded skeleton's bone count), asked once per clone.
    struct CamTargetBones
    {
        std::uint32_t id = 0;
        Entity clone = 0;
        std::uint32_t family = 0;
        std::uint64_t mask = 0; // 64 named bones
        std::uint32_t count = 0;
        ULONGLONG countAskedAt = 0;
    };
    std::vector<CamTargetBones> g_camTargetBones;
    // Per CAMERA_CUT_RATE second.
    std::uint32_t g_camBoneRecorded = 0;
    std::uint32_t g_camBoneLive = 0;
    std::uint32_t g_camBoneMissing = 0;
    replay::log::LogLimit g_camBoneLogs;

    void ForgetCamTargets()
    {
        g_camBoneIndex.clear();
        for (CamBoneCache& entry : g_camBoneCache)
            entry = CamBoneCache{};
        g_camBoneNotes.clear();
        g_camTargetBones.clear();
        g_camTargetHeads.clear();
        for (CamTargetCache& entry : g_camTargetCache)
            entry = CamTargetCache{};
        g_camTargetCacheNext = 0;
    }

    CamTargetHead& CamTargetHeadFor(std::uint32_t id, std::uint32_t record)
    {
        for (CamTargetHead& head : g_camTargetHeads)
            if (head.id == id && head.record == record)
                return head;
        if (g_camTargetHeads.size() >= 256)
            g_camTargetHeads.erase(g_camTargetHeads.begin());
        g_camTargetHeads.push_back(CamTargetHead{id, record});
        return g_camTargetHeads.back();
    }

    bool MarkerEntity(std::uint32_t id, std::uint64_t timeMs, replay::camera::EntityState& out)
    {
        const replay::playback::Puppet* puppet = id ? g_puppets.Find(replay::entity::ReplayId::FromPacked(id)) : nullptr;
        if (!id || !puppet)
            return false;
        if (!g_camTargetRecorded)
            return LiveMarkerEntity(puppet, out);
        for (const CamTargetCache& entry : g_camTargetCache)
        {
            if (entry.id == id && entry.timeMs == timeMs)
            {
                out = entry.state;
                ++g_camTargetRecordedHits;
                return true;
            }
        }
        const bool body = puppet->packet.kind == replay::entity::EntityKind::Ped ||
            puppet->packet.kind == replay::entity::EntityKind::Horse;
        const bool clone = puppet->clone && ENTITY::DOES_ENTITY_EXIST(puppet->clone);
        // The head bone of the record at that time (asked once per record).
        replay::playback::RecordedBody recorded;
        // Before the record's first sample the first one (holdFirst), as for the bones.
        if (!replay::playback::RecordedBodyAt(id, static_cast<std::uint32_t>(std::min<std::uint64_t>(timeMs, 0xFFFFFFFFull)),
                -1, recorded, true))
        {
            ++g_camTargetLiveHits;
            return LiveMarkerEntity(puppet, out); // no pose record there: the live puppet
        }
        CamTargetHead* head = nullptr;
        if (body)
        {
            head = &CamTargetHeadFor(id, recorded.recordId);
            if (head->bone == -1 && clone)
            {
                const int bone = PED::GET_PED_BONE_INDEX(puppet->clone, kHeadBone);
                head->bone = bone >= 0 ? bone : -2;
            }
            replay::playback::RecordedBody withHead;
            if (head->bone >= 0 && !head->off &&
                replay::playback::RecordedBodyAt(id, static_cast<std::uint32_t>(timeMs), head->bone, withHead, true))
                recorded = withHead;
        }
        const replay::editor::Vec3 position{recorded.position[0], recorded.position[1], recorded.position[2]};
        const replay::editor::Vec3 headAt{recorded.head[0], recorded.head[1], recorded.head[2]};
        out = replay::camera::EntityFromRecorded(position, recorded.rotation, recorded.hasHead ? &headAt : nullptr);
        // At the playhead the live puppet is there to compare with (the drift the
        // camera does not take) and to check the head bone.
        replay::camera::EntityState live{};
        const bool atPlayhead = timeMs == g_transport.PlayheadMs() && clone && LiveMarkerEntity(puppet, live);
        if (atPlayhead)
        {
            const float dx = live.position.x - position.x, dy = live.position.y - position.y, dz = live.position.z - position.z;
            const float drift = std::sqrt(dx * dx + dy * dy + dz * dz);
            g_camTargetDriftMaxM = std::max(g_camTargetDriftMaxM, drift);
            if (drift > 0.05f)
                ++g_camTargetDriftOver5cm;
            if (head && recorded.hasHead && live.hasHead && head->good < 3 && head->bad < 3)
            {
                const float hx = live.head.x - headAt.x, hy = live.head.y - headAt.y, hz = live.head.z - headAt.z;
                const float gap = std::sqrt(hx * hx + hy * hy + hz * hz);
                if (gap <= 0.5f + drift)
                    ++head->good;
                else if (++head->bad >= 3 && head->good == 0)
                {
                    head->off = true;
                    for (CamTargetCache& entry : g_camTargetCache)
                        if (entry.id == id)
                            entry = CamTargetCache{};
                }
                if ((head->good == 3 || head->off) && g_camTargetLogs.Take(64))
                    LogFile() << "CAMERA_TARGET,phase=head_check,id=" << id << ",record=0x" << std::hex << recorded.recordId
                              << std::dec << ",bone=" << head->bone << ",result=" << (head->off ? "off" : "ok")
                              << ",gap_m=" << gap << ",drift_m=" << drift << ",playhead_ms=" << timeMs << '\n';
            }
        }
        if (body && (!out.hasHead || (head && head->off)))
        {
            // No recorded head: the live head's offset from the live root,
            // carried by the recorded root (its heading frame).
            replay::camera::EntityState offsetFrom{};
            const bool haveLive = atPlayhead ? true : (clone && LiveMarkerEntity(puppet, offsetFrom));
            const replay::camera::EntityState& source = atPlayhead ? live : offsetFrom;
            if (haveLive && source.hasHead)
            {
                const replay::editor::Vec3 rel = replay::camera::ToHeadingFrame(
                    {source.head.x - source.position.x, source.head.y - source.position.y, source.head.z - source.position.z},
                    source.heading);
                const replay::editor::Vec3 back = replay::camera::FromHeadingFrame(rel, out.heading);
                out.hasHead = true;
                out.head = {position.x + back.x, position.y + back.y, position.z + back.z};
            }
        }
        ++g_camTargetRecordedHits;
        CamTargetCache& slot = g_camTargetCache[g_camTargetCacheNext++ % 8];
        slot.id = id;
        slot.timeMs = timeMs;
        slot.state = out;
        return true;
    }

    CamBoneNote& CamBoneNoteFor(std::uint32_t id, std::uint32_t ref)
    {
        for (CamBoneNote& note : g_camBoneNotes)
            if (note.id == id && note.ref == ref)
                return note;
        if (g_camBoneNotes.size() >= 256)
            g_camBoneNotes.erase(g_camBoneNotes.begin());
        g_camBoneNotes.push_back(CamBoneNote{id, ref});
        return g_camBoneNotes.back();
    }

    replay::camera::bones::Family CamFamilyOf(const replay::playback::Puppet& puppet, bool clone)
    {
        using replay::camera::bones::Family;
        switch (puppet.packet.kind)
        {
        case replay::entity::EntityKind::Ped:
            return clone && !PED::IS_PED_HUMAN(puppet.clone) ? Family::Animal : Family::Human;
        case replay::entity::EntityKind::Horse: return Family::Horse;
        case replay::entity::EntityKind::Weapon: return Family::Weapon;
        case replay::entity::EntityKind::Vehicle:
        case replay::entity::EntityKind::TrainCar: return Family::Vehicle;
        default: return Family::None;
        }
    }

    // The skeleton index of a named bone on this clone (first engine name
    // found), -1 when it has none of them.
    int NamedBoneIndex(Entity clone, std::uint32_t named)
    {
        if (named >= replay::camera::bones::kNamedCount)
            return -1;
        for (const char* name : replay::camera::bones::kNamed[named].names)
        {
            if (!name)
                break;
            const int index = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(clone, name);
            if (index >= 0)
                return index;
        }
        return -1;
    }

    const CamTargetBones& TargetBonesFor(const replay::playback::Puppet& puppet)
    {
        const std::uint32_t id = puppet.packet.id.Packed();
        const bool clone = puppet.clone && ENTITY::DOES_ENTITY_EXIST(puppet.clone);
        const ULONGLONG now = GetTickCount64();
        CamTargetBones* entry = nullptr;
        for (CamTargetBones& known : g_camTargetBones)
            if (known.id == id)
                entry = &known;
        if (entry && entry->clone == (clone ? puppet.clone : 0) && (entry->count || now - entry->countAskedAt < 1000))
            return *entry;
        if (!entry)
        {
            if (g_camTargetBones.size() >= 256)
                g_camTargetBones.erase(g_camTargetBones.begin());
            g_camTargetBones.push_back(CamTargetBones{});
            entry = &g_camTargetBones.back();
        }
        const bool sameClone = entry->id == id && entry->clone == (clone ? puppet.clone : 0);
        entry->id = id;
        if (!sameClone)
        {
            entry->clone = clone ? puppet.clone : 0;
            const replay::camera::bones::Family family = CamFamilyOf(puppet, clone);
            entry->family = static_cast<std::uint32_t>(family);
            entry->mask = 0;
            int found[replay::camera::bones::kNamedCount];
            for (std::uint32_t n = 0; n < replay::camera::bones::kNamedCount; ++n)
            {
                found[n] = clone && replay::camera::bones::kNamed[n].family == family ? NamedBoneIndex(puppet.clone, n) : -1;
                if (found[n] >= 0)
                    entry->mask |= 1ull << n;
            }
            // What each weapon model answered for the engine's weapon bone names (the
            // engine hashes the name and finds its tag on this skeleton), once per model:
            // the evidence of the names.
            static std::vector<std::uint32_t> loggedModels;
            if (clone && family == replay::camera::bones::Family::Weapon &&
                std::find(loggedModels.begin(), loggedModels.end(), puppet.packet.modelHash) == loggedModels.end() &&
                loggedModels.size() < 128)
            {
                loggedModels.push_back(puppet.packet.modelHash);
                replay::playback::RecordedBody counted;
                const bool recorded = replay::playback::RecordedBodyAt(id,
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(g_transport.PlayheadMs(), 0xFFFFFFFFull)), -1, counted);
                std::ostringstream names;
                std::uint32_t named = 0;
                for (std::uint32_t n = 0; n < replay::camera::bones::kNamedCount; ++n)
                    if (found[n] >= 0)
                    {
                        names << (named++ ? ";" : "") << replay::camera::bones::kNamed[n].names[0] << '=' << found[n];
                    }
                LogFile() << "WEAPON_BONE_NAMES,id=" << id << ",model=0x" << std::hex << puppet.packet.modelHash << ",weapon=0x"
                          << puppet.packet.weaponHash << std::dec << ",bones=" << (recorded ? counted.boneCount : 0)
                          << ",named=" << named << ",names=" << (named ? names.str() : std::string("-")) << '\n';
            }
        }
        replay::playback::RecordedBody body;
        entry->count = replay::playback::RecordedBodyAt(id,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(g_transport.PlayheadMs(), 0xFFFFFFFFull)), -1, body) ? body.boneCount : 0;
        entry->countAskedAt = now;
        return *entry;
    }

    int CamBoneIndexFor(const replay::playback::Puppet& puppet, bool clone, std::uint32_t ref)
    {
        namespace bones = replay::camera::bones;
        if (bones::ModeOf(ref) == bones::kModeIndex)
            return static_cast<int>(bones::ValueOf(ref));
        if (bones::ModeOf(ref) != bones::kModeNamed || !clone)
            return -1;
        const std::uint32_t id = puppet.packet.id.Packed();
        for (const CamBoneIndex& known : g_camBoneIndex)
            if (known.id == id && known.clone == puppet.clone && known.ref == ref)
                return known.index;
        if (g_camBoneIndex.size() >= 512)
            g_camBoneIndex.erase(g_camBoneIndex.begin());
        const int index = NamedBoneIndex(puppet.clone, bones::ValueOf(ref));
        g_camBoneIndex.push_back(CamBoneIndex{id, puppet.clone, ref, index});
        return index;
    }

    bool MarkerBone(std::uint32_t id, std::uint32_t ref, std::uint64_t timeMs, replay::camera::EntityState& out)
    {
        namespace bones = replay::camera::bones;
        const replay::playback::Puppet* puppet = id ? g_puppets.Find(replay::entity::ReplayId::FromPacked(id)) : nullptr;
        if (!puppet || !ref || !bones::Valid(ref))
            return false;
        for (const CamBoneCache& entry : g_camBoneCache)
            if (entry.id == id && entry.ref == ref && entry.timeMs == timeMs)
            {
                out = entry.state;
                return true;
            }
        const bool clone = puppet->clone && ENTITY::DOES_ENTITY_EXIST(puppet->clone);
        const int index = CamBoneIndexFor(*puppet, clone, ref);
        const std::uint32_t at = static_cast<std::uint32_t>(std::min<std::uint64_t>(timeMs, 0xFFFFFFFFull));
        replay::playback::RecordedBody body;
        bool found = false;
        const char* source = "missing";
        if (index >= 0 && g_camTargetRecorded && replay::playback::RecordedBodyAt(id, at, index, body, true) &&
            static_cast<std::uint32_t>(index) < body.boneCount)
        {
            // Before the record's first sample the camera holds the first one; the live
            // branch below would mix the clone's entity rotation in.
            if (body.heldFirst && g_camBoneLogs.Take(64))
                LogFile() << "CAMERA_BONE,phase=hold_first,id=" << id << ",ref=0x" << std::hex << ref << std::dec << ",index=" << index
                          << ",time_ms=" << timeMs << '\n';
            const replay::editor::Vec3 root{body.position[0], body.position[1], body.position[2]};
            out = replay::camera::EntityFromBone(root, body.rotation, body.boneTranslation, body.boneRotation);
            found = std::isfinite(out.position.x) && std::isfinite(out.position.y) && std::isfinite(out.position.z);
            source = "recorded";
            if (found)
                ++g_camBoneRecorded;
        }
        else if (index >= 0 && clone)
        {
            // No pose record there: the live clone, and only for a named bone (its index
            // came from this clone's own skeleton). An index picked from the recorded list
            // is never asked of the engine: the clone's skeleton may have fewer bones than
            // the record and the native's bounds are not proven.
            const bool safe = bones::ModeOf(ref) == bones::kModeNamed;
            const Vector3 p = safe ? ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(puppet->clone, index) : Vector3{};
            if (safe && (p.x != 0.0f || p.y != 0.0f || p.z != 0.0f))
            {
                const Vector3 r = ENTITY::GET_ENTITY_ROTATION(puppet->clone, 2);
                out = replay::camera::EntityState{};
                out.position = {p.x, p.y, p.z};
                out.pitch = r.x; // the clone's rotation: no bone rotation native in RDR2
                out.roll = r.y;
                out.heading = r.z;
                out.hasHead = true;
                out.head = out.position;
                found = true;
                source = "live";
                ++g_camBoneLive;
            }
        }
        CamBoneNote& note = CamBoneNoteFor(id, ref);
        if (!found)
        {
            ++g_camBoneMissing;
            if (!note.missingLogged && g_camBoneLogs.Take(64))
            {
                note.missingLogged = true;
                LogFile() << "CAMERA_BONE,phase=missing,id=" << id << ",ref=0x" << std::hex << ref << std::dec << ",index=" << index
                          << ",clone=" << (clone ? 1 : 0) << ",bones=" << body.boneCount << ",time_ms=" << timeMs << '\n';
            }
            return false;
        }
        // At the playhead, the recorded bone against the clone's (the bone
        // index and the object space read are checked, three times).
        if (clone && note.checks < 3 && timeMs == g_transport.PlayheadMs() && std::strcmp(source, "recorded") == 0 &&
            bones::ModeOf(ref) == bones::kModeNamed) // the same rule: an index from this clone only
        {
            const Vector3 live = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(puppet->clone, index);
            if (live.x != 0.0f || live.y != 0.0f || live.z != 0.0f)
            {
                ++note.checks;
                const float dx = live.x - out.position.x, dy = live.y - out.position.y, dz = live.z - out.position.z;
                if (g_camBoneLogs.Take(64))
                    LogFile() << "CAMERA_BONE,phase=check,id=" << id << ",ref=0x" << std::hex << ref << std::dec << ",index=" << index
                              << ",gap_m=" << std::sqrt(dx * dx + dy * dy + dz * dz) << ",bones=" << body.boneCount
                              << ",playhead_ms=" << timeMs << '\n';
            }
        }
        CamBoneCache& slot = g_camBoneCache[g_camBoneCacheNext++ % 8];
        slot.id = id;
        slot.ref = ref;
        slot.timeMs = timeMs;
        slot.state = out;
        return true;
    }

    // The recording's main character: Arthur's or John's puppet.
    std::uint32_t MainCharacterId()
    {
        static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        for (const replay::playback::Puppet& puppet : g_puppets.list)
        {
            if (puppet.packet.kind == replay::entity::EntityKind::Ped &&
                (puppet.packet.modelHash == arthur || puppet.packet.modelHash == john))
                return puppet.packet.id.Packed();
        }
        return 0;
    }

    // The Custom shake's phase memo (a clip change drops it; the markers and
    // events are in its signature).
    replay::camera::ShakePhaseCache g_shakePhaseCache;
    // Ticks to wait before folding a legacy shake left in the track (the bench's
    // marker and event steps come one a tick; TickShakeMigration).
    int g_shakeFoldHold = 0;

    // The Spline camera weight applies while the clip plays forward (normal play,
    // not in Edit Camera) or exports; paused, scrubbed or framing a marker the
    // camera sits where the markers are (a view saved from a weighted frame would
    // get the weight again on play).
    bool SplineWeightActive()
    {
        if (replay::exportprobe::Active())
            return true;
        return g_transport.Playing() && !g_transport.Paused() && replay::playback::NormalPlay() && g_editingMarker == 0;
    }

    replay::camera::Scene MarkerScene()
    {
        static std::uint32_t phaseClip = ~0u;
        if (phaseClip != g_clipLoadSequence)
        {
            phaseClip = g_clipLoadSequence;
            g_shakePhaseCache.Clear();
        }
        replay::camera::Scene scene;
        scene.shakePhase = &g_shakePhaseCache;
        scene.splineDefaults = g_splineDefaults;
        scene.splineWeight = SplineWeightActive();
        scene.lookup = MarkerEntity;
        scene.boneLookup = MarkerBone;
        scene.game = &g_clip.gameCamera;
        int width = 0, height = 0;
        GRAPHICS::GET_SCREEN_RESOLUTION(&width, &height);
        scene.aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f;
        scene.mainCharacter = MainCharacterId();
        // No blend or game camera interpolation across a cut; LockCutsceneCamera gives
        // a recorded cutscene to the game camera.
        scene.cuts = &g_clip.cuts;
        if (g_recConfig.lockCutsceneCamera)
            scene.gameOnly = [](std::uint64_t timeMs) {
                return replay::gate::SpanAt(g_clip.spans, replay::gate::SpanKind::Cutscene, timeMs) != nullptr;
            };
        return scene;
    }

    replay::camera::SpringState g_cameraSpring;

    // A shake migration in the log, once, as SHAKE_MIGRATION (its counts in
    // both builds). The proof (the legacy track and the migrated one compared at
    // every 30 Hz frame of the clip with this clip's scene: the shake and the
    // camera path) runs in the developer build only: its cost is the declared
    // duration's (a small legacy clip may declare 49 days), the public build
    // logs proof=skipped. The track kept for it is let go first, so a failed
    // proof (out of memory) is not tried again every tick.
    void LogShakeMigration(const char* source)
    {
        replay::camera::ShakeMigrationReport& report = g_cameraTrack.Migration();
        if (!report.before)
            return;
        const std::shared_ptr<const replay::camera::Track> before = std::move(report.before);
        report.before.reset();
        const ULONGLONG startedAt = GetTickCount64();
        replay::camera::mshake::Proof proof;
        proof.skipped = true;
        const char* proofError = nullptr;
        if constexpr (replay::identity::kDevBuild)
        {
            const std::uint64_t duration = std::max<std::uint64_t>(g_transport.DurationMs(),
                g_cameraTrack.Keyframes().empty() ? 0 : g_cameraTrack.Keyframes().back().timeMs);
            try
            {
                proof = replay::camera::mshake::Verify(*before, g_cameraTrack, MarkerScene(), duration, 0);
            }
            catch (const std::exception&)
            {
                proof = {};
                proof.skipped = true;
                proofError = "no_memory";
            }
        }
        LogFile() << replay::camera::mshake::FormatLine(report, proof) << (proofError ? ",proof_error=" : "")
                  << (proofError ? proofError : "") << ",source=" << source
                  << ",verify_ms=" << (GetTickCount64() - startedAt) << ",clip=" << Narrow(g_currentClipPath) << '\n';
    }

    // Once a tick: a migrated clip's proof; a legacy shake still waiting in the
    // track (the bench's steps, a director's line) folded once no marker or event
    // step came for two ticks.
    void TickShakeMigration()
    {
        if (g_shakeFoldHold > 0)
        {
            --g_shakeFoldHold;
            return;
        }
        if (g_cameraTrack.HasLegacyShake())
        {
            replay::camera::mshake::Migrate(g_cameraTrack);
            g_shakePhaseCache.Clear();
            LogShakeMigration("fold");
            return;
        }
        if (g_cameraTrack.Migration().before)
            LogShakeMigration("load");
    }

    bool EvaluateCameraTrack(std::uint64_t timeMs, replay::editor::CameraState& state, float dtSeconds,
        replay::camera::SegmentPlan* plan)
    {
        if (g_cameraTrack.Empty())
            return false;
        g_cameraSpring.reframe = g_camSmoothReframe;
        return replay::camera::EvaluateMarkers(g_cameraTrack, timeMs, MarkerScene(), state, &g_cameraSpring, dtSeconds, plan);
    }

    // Every camera of the editor is placed through here (src/camera_cut.h). Params
    // mode: one SET_CAM_PARAMS whose cut flag is set only on a real cut (the TAA
    // keeps its history); legacy mode (ReplayCameraCutMode=legacy, or the first
    // placements did not take): SET_CAM_COORD + SET_CAM_ROT + SET_CAM_FOV, a cut
    // every frame.
    bool g_camCutParamsFailed = false;
    bool g_camCutVerifiedLogged = false;
    replay::camcut::Tracker g_replayCamCut;
    replay::camcut::Tracker g_freeCamCut;
    std::uint32_t g_camPlacedCutsThisTick = 0;
    std::uint32_t g_camCutLogs = 0;
    std::uint32_t g_camDofRecreations = 0; // ForgetReplayCamDof with a camera made (CAMERA_CUT_RATE)
    std::uint32_t g_lastPlaceWhy = 0;       // PlaceCam's last cut decision (camcut::CutWhy)

    // The camera object behind a handle through the engine's own lookup 0x1739DC
    // (rung 4, its body compared once), its cut bits cleared after SET_CAM_COORD /
    // SET_CAM_ROT when the placement is not a real cut (rung 3: a plain field of
    // the replay's own camera, written only after the object is shown to be that
    // camera: the position just set at +0x50 and the position bit up). Any
    // failure: the plain natives for good, logged.
    bool g_camBitsChecked = false;
    bool g_camBitsUsable = false;
    bool g_camBitsFailed = false;
    bool g_camBitsVerifiedLogged = false;
    std::uint32_t g_camBitsCleared = 0;     // placements whose bits were cleared (this second)
    std::uint32_t g_camBitsSeenBefore = 0;  // ... whose camera still had cut bits from the frame before
    std::uint32_t g_camBitsClean = 0;       // clean clears in a row (verification)
    using CamLookupFn = void*(__fastcall*)(int handle);

    void* CallCamLookup(std::uintptr_t function, int handle, std::uint32_t& code)
    {
        __try
        {
            return reinterpret_cast<CamLookupFn>(function)(handle);
        }
        __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    std::uintptr_t CamObject(Cam cam, std::uint32_t& code)
    {
        static std::uintptr_t base = 0;
        if (!base)
            base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_camBitsChecked)
        {
            g_camBitsChecked = true;
            g_camBitsUsable = replay::mem::Matches(reinterpret_cast<const void*>(base + replay::camcut::kCamLookupRva),
                replay::camcut::kCamLookup, sizeof(replay::camcut::kCamLookup));
            LogFile() << "CAMERA_CUT_MODE,phase=bits_config,lookup=" << (g_camBitsUsable ? "ok" : "signature_mismatch")
                      << ",rva=0x" << std::hex << replay::camcut::kCamLookupRva << std::dec << '\n';
        }
        if (!g_camBitsUsable)
            return 0;
        return reinterpret_cast<std::uintptr_t>(CallCamLookup(base + replay::camcut::kCamLookupRva, cam, code));
    }

    void CamBitsFallback(const char* why, Cam cam, std::uint32_t code)
    {
        g_camBitsFailed = true;
        LogFile() << "CAMERA_CUT_MODE,phase=fallback,mode=legacy,why=" << why << ",cam=" << cam << ",code=0x" << std::hex
                  << code << std::dec << '\n';
    }

    // After SET_CAM_COORD + SET_CAM_ROT on `cam` at (px, py, pz): the cut
    // bits off unless `cut`. Also counts a camera that still had the bits
    // from the frame before (they were not the engine's to clear).
    void ClearCamCutBits(Cam cam, float px, float py, float pz, bool cut)
    {
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        if (!object)
        {
            CamBitsFallback(g_camBitsUsable ? (code ? "lookup_exception" : "lookup_null") : "lookup_signature", cam, code);
            return;
        }
        float position[3]{};
        std::uint16_t flags = 0;
        if (!replay::mem::Copy(position, object + replay::camcut::kCamPositionOffset, sizeof(position)) ||
            !replay::mem::Read(object + replay::camcut::kCamFlagsOffset, flags))
        {
            CamBitsFallback("read_fault", cam, 0);
            return;
        }
        const float want[3] = {px, py, pz};
        if (!replay::camcut::CamObjectIsOurs(position, want, flags))
        {
            CamBitsFallback("not_our_camera", cam, flags);
            return;
        }
        if (cut)
            return; // a real cut keeps its bits
        const std::uint16_t cleared = replay::camcut::ClearedCutBits(flags);
        std::uint16_t back = 0xFFFF;
        if (!replay::mem::Put(object + replay::camcut::kCamFlagsOffset, &cleared, sizeof(cleared)) ||
            !replay::mem::Read(object + replay::camcut::kCamFlagsOffset, back) || back != cleared)
        {
            CamBitsFallback("write_fault", cam, back);
            return;
        }
        ++g_camBitsCleared;
        if (++g_camBitsClean == 3 && !g_camBitsVerifiedLogged)
        {
            g_camBitsVerifiedLogged = true;
            LogFile() << "CAMERA_CUT_MODE,phase=verified,mode=bits,flags_before=0x" << std::hex << flags << ",flags_after=0x"
                      << back << std::dec << '\n';
        }
    }

    // The last view PlaceCam set (the replay camera's or the free camera's: one of
    // them renders), for the world overlay.
    struct PlacedView
    {
        bool valid = false;
        replay::editor::CameraState view{};
    };
    PlacedView g_lastPlacedView;

    void PlaceCam(Cam cam, replay::camcut::Tracker& tracker, const char* which, float px, float py, float pz, float rx,
        float ry, float rz, float fov, float realDt, bool forceCut)
    {
        replay::camcut::Placement placement;
        placement.camera = cam;
        placement.position[0] = px; placement.position[1] = py; placement.position[2] = pz;
        placement.rotation[0] = rx; placement.rotation[1] = ry; placement.rotation[2] = rz;
        placement.fov = fov;
        placement.realDt = realDt;
        placement.forceCut = forceCut;
        const std::uint32_t why = replay::camcut::Decide(tracker, placement);
        g_lastPlaceWhy = why; // the shot's memory (focus) starts over on a cut
        // The view the world overlay is projected with.
        g_lastPlacedView.valid = true;
        g_lastPlacedView.view = {{px, py, pz}, {rx, ry, rz}, fov};
        // The cut bits mode (the default), and where a params placement did not take.
        const bool bits = (g_camCutMode == replay::camcut::Mode::Bits ||
                              (g_camCutMode == replay::camcut::Mode::Params && g_camCutParamsFailed)) &&
            !g_camBitsFailed;
        if (bits)
        {
            {
                // The camera's word before this placement: cut bits left from
                // the frame before mean the engine did not clear them.
                std::uint32_t code = 0;
                const std::uintptr_t object = CamObject(cam, code);
                std::uint16_t before = 0;
                if (object && replay::mem::Read(object + replay::camcut::kCamFlagsOffset, before) &&
                    (before & replay::camcut::kCutBits))
                    ++g_camBitsSeenBefore;
            }
            CAM::SET_CAM_COORD(cam, px, py, pz);
            CAM::SET_CAM_ROT(cam, rx, ry, rz, 2);
            CAM::SET_CAM_FOV(cam, fov);
            const bool cut = why != replay::camcut::kWhyNone;
            ClearCamCutBits(cam, px, py, pz, cut);
            if (cut || g_camBitsFailed)
                ++g_camPlacedCutsThisTick;
            if (cut && replay::log::Verbose() && g_camCutLogs < 200)
            {
                ++g_camCutLogs;
                LogFile() << "CAMERA_CUT,cam=" << which << ",why=0x" << std::hex << why << std::dec
                          << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
            return;
        }
        if (g_camCutMode == replay::camcut::Mode::Legacy || g_camCutParamsFailed)
        {
            CAM::SET_CAM_COORD(cam, px, py, pz);
            CAM::SET_CAM_ROT(cam, rx, ry, rz, 2);
            CAM::SET_CAM_FOV(cam, fov);
            ++g_camPlacedCutsThisTick;
            return;
        }
        const bool cut = why != replay::camcut::kWhyNone;
        CAM::SET_CAM_PARAMS(cam, px, py, pz, rx, ry, rz, fov, 0, 0, 0, 2, cut ? 1 : 0, 0);
        if (cut)
        {
            ++g_camPlacedCutsThisTick;
            if (replay::log::Verbose() && g_camCutLogs < 200)
            {
                ++g_camCutLogs;
                LogFile() << "CAMERA_CUT,cam=" << which << ",why=0x" << std::hex << why << std::dec
                          << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
        }
        if (tracker.placements <= 3)
        {
            // SET_CAM_PARAMS applies the rotation only when [cam+0x538] == 3 and the
            // position only when [cam+0x498] == 0 (camera_cut.h): read back, and the plain
            // natives for good when it did not take.
            const Vector3 gotPos = CAM::GET_CAM_COORD(cam);
            const Vector3 gotRot = CAM::GET_CAM_ROT(cam, 2);
            const float got[3] = {gotPos.x, gotPos.y, gotPos.z};
            const float gotR[3] = {gotRot.x, gotRot.y, gotRot.z};
            if (!replay::camcut::Took(placement.position, placement.rotation, got, gotR))
            {
                g_camCutParamsFailed = true;
                // The fields SET_CAM_PARAMS tests (camera_cut.h), read off the camera object
                // for the evidence.
                std::uint64_t attach = ~0ull, point = ~0ull;
                std::uint32_t camMode = ~0u;
                std::uint8_t frameBit = 0xFF;
                {
                    std::uint32_t code = 0;
                    const std::uintptr_t object = CamObject(cam, code);
                    if (object)
                    {
                        replay::mem::Read(object + replay::camcut::kCamAttachOffset, attach);
                        replay::mem::Read(object + replay::camcut::kCamPointOffset, point);
                        replay::mem::Read(object + replay::camcut::kCamModeOffset, camMode);
                        replay::mem::Read(object + replay::camcut::kCamFrameOffset, frameBit);
                    }
                }
                char line[420]{};
                std::snprintf(line, sizeof(line),
                    "CAMERA_CUT_MODE,phase=fallback,mode=%s,why=params_not_applied,cam=%s,placement=%u,"
                    "want=%.2f;%.2f;%.2f;%.1f;%.1f;%.1f,got=%.2f;%.2f;%.2f;%.1f;%.1f;%.1f,cam_498=0x%llx,cam_520=0x%llx,"
                    "cam_538=%d,cam_4b4=0x%x\n",
                    g_camBitsFailed ? "legacy" : "bits", which, tracker.placements, px, py, pz, rx, ry, rz, got[0], got[1],
                    got[2], gotR[0], gotR[1], gotR[2], static_cast<unsigned long long>(attach),
                    static_cast<unsigned long long>(point), static_cast<int>(camMode), static_cast<unsigned>(frameBit));
                LogFile() << line;
                CAM::SET_CAM_COORD(cam, px, py, pz);
                CAM::SET_CAM_ROT(cam, rx, ry, rz, 2);
                CAM::SET_CAM_FOV(cam, fov);
            }
            else if (tracker.placements == 3 && !g_camCutVerifiedLogged)
            {
                g_camCutVerifiedLogged = true;
                LogFile() << "CAMERA_CUT_MODE,phase=verified,mode=params,cam=" << which << '\n';
            }
        }
    }

    replay::camcut::MarkerFrame MarkerFrameOf(bool keyed, const replay::camera::SegmentPlan& plan)
    {
        replay::camcut::MarkerFrame frame;
        frame.valid = keyed && plan.valid;
        frame.current = plan.current;
        frame.next = plan.next;
        frame.blending = plan.blending;
        return frame;
    }

    // Depth of field: 0xE4B7945EF4F1BFB2 takes 10 x 8-byte slots (f-stop, near,
    // far, lens mm, lens min, lens max, 4 flags) and _SET_CAM_FOCUS_DISTANCE the
    // focus. The block is an object of 0x40 bytes the camera owns at [cam+0x470]
    // (0x4210DC allocates it on the first _SET_CAM_DOF_PARAMS and copies into it).
    // The CAM native 0x1FC6C727D30FFDDE (handler 0xF5ED6C > 0xF6850C: the camera
    // lookup, the script owner check, then 0x4256B8) deletes it through its
    // virtual destructor and nulls [cam+0x470], the state of a fresh camera: DOF
    // back to Default without making a camera. _PAUSE_CAMERA_FOCUS (0xF6329C >
    // 0xF721D4) only sets or clears bit 0x4000 of the frame flags [cam+0xEC]:
    // leaving the engine's autofocus is that bit put back. Recreating the camera
    // (ForgetReplayCamDof) is kept with ReplayCamDofRecreate=1 and as the fallback.
    // What the replay sent to a script camera's near clip and focus distance
    // (ApplyNearClip, SendFocusDistance below); a new camera starts over
    // (NewLensCamera), keeping the counts.
    struct LensState
    {
        float nearSent = -1.0f;
        float focusSent = -1.0f;
        std::uint32_t nearOverwrites = 0;
        std::uint32_t focusOverwrites = 0;
        std::uint32_t focusOverwriteRun = 0;
        bool pauseForDistance = false;
        // The free camera's own DOF state (the replay camera keeps its globals).
        bool dof = false;
        bool engineFocus = false;
        int pausedBefore = -1;
    };
    LensState g_replayLens;
    LensState g_freeLens;
    // When the near clip or the DOF sent last changed (ApplyNearClip,
    // NoteDofReadback): UpdateWorldFreeze thaws the photo mode freeze for it.
    ULONGLONG g_lensChangedAt = 0;
    bool g_lensHeldLogged = false;

    // The photo mode freeze's state (rung 2, read only).
    // _REQUEST_PHOTO_MODE_FREEZE 0x7C709C01D43D94CD is the wrapper 0xF63B4C: mov
    // rcx, [rip + d] (the singleton pointer at RVA 0x3F02448), xor edx, edx, jmp
    // 0x71E260 (state 0 -> 1, the request). 0x73FFBC is the engine's "frozen" test
    // (state - 2 <= 1: states 2 and 3), and the camera code returns early on it
    // (0x44B619, 0x44B66E, 0x44B6D0). -1 when the signature or the read fails.
    int PhotoFreezeState()
    {
        static bool checked = false;
        static std::uintptr_t global = 0;
        if (!checked)
        {
            checked = true;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            constexpr std::uintptr_t kWrapperRva = 0xF63B4C;
            constexpr std::uintptr_t kGlobalRva = 0x3F02448;
            std::uint8_t code[12]{};
            std::int32_t disp = 0;
            const bool read = replay::mem::Copy(code, base + kWrapperRva, sizeof(code));
            if (read)
                std::memcpy(&disp, code + 3, sizeof(disp));
            const bool ok = read && code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x0D && code[7] == 0x33 && code[8] == 0xD2 &&
                code[9] == 0xE9 && kWrapperRva + 7 + static_cast<std::intptr_t>(disp) == kGlobalRva;
            global = ok ? base + kGlobalRva : 0;
            LogFile() << "CAMERA_FREEZE_PROBE,lookup=" << (ok ? "ok" : read ? "signature_mismatch" : "read_fault") << ",rva=0x" << std::hex
                      << kGlobalRva << std::dec << '\n';
        }
        std::uint64_t object = 0;
        std::uint32_t state = 0;
        if (!global || !replay::mem::Read(global, object) || !object || !replay::mem::Read(static_cast<std::uintptr_t>(object), state))
            return -1;
        return static_cast<int>(state);
    }
    void NewLensCamera(LensState& lens)
    {
        lens.nearSent = -1.0f;
        lens.focusSent = -1.0f;
        lens.focusOverwriteRun = 0;
        lens.pauseForDistance = false;
        lens.dof = false;
        lens.engineFocus = false;
        lens.pausedBefore = -1;
    }
    bool g_replayCamDof = false;
    // The camera's autofocus was unpaused (_PAUSE_CAMERA_FOCUS false, the engine
    // focus).
    bool g_replayCamEngineFocus = false;
    float g_replayCamBlur = 0.0f; // this frame's motion blur strength
    replay::log::LogLimit g_markerFocusLogs; // MARKER_FOCUS lines (capped per session)
    // The shot's focus memory (hysteresis, held distance, distance spring), reset
    // on every cut of the replay camera.
    replay::camera::FocusMemory g_camFocusMemory;
    // Bit 0x4000 of [cam+0xEC] read just before the autofocus was let in
    // (0 or 1), -1 when it could not be read (then the fallback recreates).
    int g_replayCamFocusPausedBefore = -1;
    bool g_camDofClearFailed = false;          // the clear did not null the block: the recreate path for good
    std::uint32_t g_camDofClearChecks = 0;
    std::uint32_t g_camDofClears = 0;          // DOF dropped by the engine's clear (no camera made)
    std::uint32_t g_camFocusRestores = 0;      // autofocus left by the pause bit (no camera made)
    std::uint32_t g_camDofBlendFrames = 0;     // ticks with the DOF interpolated inside a blend
    std::uint32_t g_camFocusEngineFlips = 0;   // inside a shot: entity <-> engine autofocus flips
    std::uint32_t g_camFocusSourceChanges = 0; // inside a shot: any change of focus source
    replay::log::LogLimit g_camDofLogs;
    constexpr std::uint32_t kCamDofBlockOffset = 0x470; // qword, the DOF object (null: none)
    constexpr std::uint16_t kCamFocusPausedBit = 0x4000;

    // A fresh camera without the DOF block (the block stays on a camera once set).
    // The new camera takes over in the same frame while the old one still renders
    // (otherwise the script cameras stop rendering for a frame and the gameplay
    // camera flashes on each switch); false when it cannot be made (then the next
    // tick creates one).
    bool ForgetReplayCamDof(const replay::editor::CameraState& state)
    {
        const Cam fresh = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", state.position.x, state.position.y,
            state.position.z, state.rotation.x, state.rotation.y, state.rotation.z, state.fov, FALSE, 2);
        const bool made = fresh && CAM::DOES_CAM_EXIST(fresh);
        if (made && g_replayCamRendering)
        {
            CAM::SET_CAM_NEAR_CLIP(fresh, 0.1f);
            CAM::SET_CAM_ACTIVE(fresh, TRUE);
        }
        if (made)
        {
            // The camera that renders this frame is the fresh one: it gets the placement,
            // with the cut (a new camera starts its springs over), in this same tick.
            PlaceCam(fresh, g_replayCamCut, "replay_dof", state.position.x, state.position.y, state.position.z,
                state.rotation.x, state.rotation.y, state.rotation.z, state.fov, 0.0f, true);
            ++g_camDofRecreations;
            g_camFocusMemory.ResetShot(); // a new camera is a cut
        }
        else if (g_replayCamRendering)
            CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, TRUE, 0);
        CAM::DESTROY_CAM(g_replayCam, FALSE);
        g_replayCam = made ? fresh : 0;
        g_replayCamRendering = made && g_replayCamRendering;
        NewLensCamera(g_replayLens);
        g_replayCamDof = false;
        g_replayCamEngineFocus = false;
        g_replayCamFocusPausedBefore = -1;
        return made;
    }

    // The camera's word at +0xEC (rung 2, the plan B lookup); false when the
    // lookup or the read failed.
    bool ReadCamFlags(Cam cam, std::uint16_t& flags)
    {
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        return object && replay::mem::Read(object + replay::camcut::kCamFlagsOffset, flags);
    }

    // DOF back to Default on the same camera. The first clears of the session read
    // [cam+0x470] back (rung 2): still set means the clear did not take, and the
    // camera is recreated from then on.
    bool DropReplayCamDof(const replay::editor::CameraState& state)
    {
        if (g_camDofClearFailed)
            return ForgetReplayCamDof(state);
        std::uint64_t before = ~0ull, after = ~0ull;
        std::uintptr_t object = 0;
        if (g_camDofClearChecks < 3)
        {
            std::uint32_t code = 0;
            object = CamObject(g_replayCam, code);
            if (object)
                replay::mem::Read(object + kCamDofBlockOffset, before);
        }
        CAM::_0x1FC6C727D30FFDDE(g_replayCam);
        if (object)
        {
            ++g_camDofClearChecks;
            const bool read = replay::mem::Read(object + kCamDofBlockOffset, after);
            const bool took = read && after == 0;
            if (g_camDofLogs.Take(16))
                LogFile() << "CAMERA_DOF,phase=clear_check,block_before=0x" << std::hex << before << ",block_after=0x" << after
                          << std::dec << ",result=" << (took ? "ok" : "not_cleared") << '\n';
            if (!took)
            {
                g_camDofClearFailed = true;
                return ForgetReplayCamDof(state);
            }
        }
        ++g_camDofClears;
        g_replayCamDof = false;
        return true;
    }

    // Out of the engine's autofocus on the same camera: the pause bit back to what
    // it was before the autofocus (read back). Unknown or not taken: the camera is
    // recreated.
    bool LeaveEngineFocus(const replay::editor::CameraState& state)
    {
        if (g_replayCamFocusPausedBefore >= 0)
        {
            const bool paused = g_replayCamFocusPausedBefore != 0;
            CAM::_PAUSE_CAMERA_FOCUS(g_replayCam, paused ? TRUE : FALSE);
            std::uint16_t flags = 0;
            if (ReadCamFlags(g_replayCam, flags) && ((flags & kCamFocusPausedBit) != 0) == paused)
            {
                ++g_camFocusRestores;
                g_replayCamEngineFocus = false;
                return true;
            }
        }
        if (g_camDofLogs.Take(16))
            LogFile() << "CAMERA_DOF,phase=fallback,why=" << (g_replayCamFocusPausedBefore < 0 ? "focus_bit_unknown" : "focus_bit_not_restored")
                      << '\n';
        return ForgetReplayCamDof(state);
    }

    // Near clip and DOF sent to a script camera and read back from the camera
    // object (rung 2, the cut bits lookup CamObject; build 1491.50, no write):
    //   [cam+0x84]  near clip (SET_CAM_NEAR_CLIP 0xF64210 > 0xF753A8: maxss
    //               with 0.01, movss [rbx+0x84]);
    //   [cam+0x470] the DOF block (0xF7583C > 0x4210DC): +0x20 f-stop,
    //               +0x34 lens mm, +0x28 "use the f-stop" (0x176678 takes
    //               +0x20 when set), +0x38 "the lens drives the FOV" (0x428370:
    //               FOV = 2 atan(12 / lens));
    //   [cam+0x54C] focus distance (_SET_CAM_FOCUS_DISTANCE 0xF641E0 > 0xF75258).
    // Each tick reads the value before writing: one that is not what the replay
    // wrote the tick before was changed by the engine in between (counted,
    // logged). The focus distance overwritten three ticks in a row pauses the
    // camera's autofocus (_PAUSE_CAMERA_FOCUS, bit 0x4000 of [cam+0xEC], read
    // back) while a distance is asked.
    constexpr std::uint32_t kCamNearClipOffset = 0x84;
    constexpr std::uint32_t kCamFocusDistanceOffset = 0x54C;
    constexpr std::uint32_t kDofFStopOffset = 0x20;
    constexpr std::uint32_t kDofLensOffset = 0x34;
    constexpr std::uint32_t kDofUseFStopOffset = 0x28;
    constexpr std::uint32_t kDofLensFovOffset = 0x38;
    replay::editor::CameraLiveInfo g_cameraLive{};
    replay::log::LogLimit g_camLensLogs;

    bool ReadCamFloat(Cam cam, std::uint32_t offset, float& out)
    {
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        return object && replay::mem::Read(object + offset, out) && std::isfinite(out);
    }

    struct DofReadback
    {
        bool ok = false;
        float fstop = 0.0f;
        float lens = 0.0f;
        std::uint8_t useFStop = 0;
        std::uint8_t lensFov = 0;
        float focus = 0.0f;
        bool focusOk = false;
    };

    DofReadback ReadCamDof(Cam cam)
    {
        DofReadback out;
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        if (!object)
            return out;
        std::uint64_t block = 0;
        if (replay::mem::Read(object + kCamDofBlockOffset, block) && block)
            out.ok = replay::mem::Read(block + kDofFStopOffset, out.fstop) && replay::mem::Read(block + kDofLensOffset, out.lens) &&
                replay::mem::Read(block + kDofUseFStopOffset, out.useFStop) && replay::mem::Read(block + kDofLensFovOffset, out.lensFov);
        out.focusOk = replay::mem::Read(object + kCamFocusDistanceOffset, out.focus);
        return out;
    }

    // The near clip on `cam` (held to 0.01..1 m), read back.
    void ApplyNearClip(Cam cam, LensState& lens, float value, const char* which)
    {
        value = std::clamp(std::isfinite(value) ? value : replay::editor::kNearClipDefault, replay::editor::kNearClipMin,
            replay::editor::kNearClipMax);
        float before = 0.0f;
        if (lens.nearSent > 0.0f && ReadCamFloat(cam, kCamNearClipOffset, before) &&
            std::fabs(before - std::max(lens.nearSent, replay::editor::kNearClipMin)) > 1e-4f)
        {
            ++lens.nearOverwrites;
            if (g_camLensLogs.Take(400))
                LogFile() << "CAMERA_NEARCLIP,phase=overwritten,cam=" << which << ",sent=" << lens.nearSent << ",read=" << before
                          << ",overwrites=" << lens.nearOverwrites << '\n';
        }
        CAM::SET_CAM_NEAR_CLIP(cam, value);
        float after = 0.0f;
        const bool read = ReadCamFloat(cam, kCamNearClipOffset, after);
        if (lens.nearSent > 0.0f && std::fabs(value - lens.nearSent) > 1e-4f)
            g_lensChangedAt = GetTickCount64(); // shown through the freeze
        if (std::fabs(value - lens.nearSent) > 1e-4f && g_camLensLogs.Take(400))
            LogFile() << "CAMERA_NEARCLIP,cam=" << which << ",set=" << value << ",read=" << (read ? after : -1.0f)
                      << ",match=" << (read && std::fabs(after - value) < 1e-4f ? 1 : 0) << ",overwrites=" << lens.nearOverwrites
                      << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        lens.nearSent = value;
        g_cameraLive.nearSet = value;
        g_cameraLive.nearRead = read ? after : 0.0f;
        if (read)
            g_cameraLive.flags |= replay::editor::CameraLiveNearRead;
    }

    // The DOF block the way the game's DEFAULT_SCRIPTED_CAMERA scripts send it
    // (shop_barber, property_use_core: slot 6 on, 7 to 9 off): the f-stop used as
    // given (+0x28), the lens free of the FOV (+0x38 off: the FOV stays the
    // camera's), so the lens can be a focal length multiplier on the FOV's lens.
    // With slots 8 and 9 on (the camera kit's block) the lens drives the FOV, so it
    // must be the FOV's own lens (a 29 mm at FOV 45: f/3.3 keeps almost everything
    // sharp).
    float SendDofBlock(Cam cam, const replay::camera::FocusPlan& focus, float fov)
    {
        const float lens = replay::camera::DofLensMm(fov, focus.intensity, focus.lensMm);
        alignas(8) std::uint8_t block[10 * 8]{};
        const float floats[6] = {focus.fstop, 1.0f, 64.0f, lens, std::min(6.0f, lens), std::max(120.0f, lens)};
        for (int n = 0; n < 6; ++n)
            std::memcpy(block + n * 8, &floats[n], 4);
        const std::int32_t flags[4] = {1, 0, 0, 0};
        for (int n = 0; n < 4; ++n)
            std::memcpy(block + (6 + n) * 8, &flags[n], 4);
        CAM::_SET_CAM_DOF_PARAMS(cam, reinterpret_cast<Any*>(block));
        return lens;
    }

    // The focus distance, after checking whether the engine changed the
    // last one (its autofocus); paused for good on this camera once it did
    // three ticks in a row.
    void SendFocusDistance(Cam cam, LensState& lens, float distance, const char* which)
    {
        float before = 0.0f;
        if (lens.focusSent > 0.0f && ReadCamFloat(cam, kCamFocusDistanceOffset, before))
        {
            if (std::fabs(before - lens.focusSent) > std::max(0.01f, 0.01f * lens.focusSent))
            {
                ++lens.focusOverwrites;
                ++lens.focusOverwriteRun;
                if (g_camLensLogs.Take(400))
                    LogFile() << "CAMERA_DOF,phase=focus_overwritten,cam=" << which << ",sent=" << lens.focusSent << ",read=" << before
                              << ",run=" << lens.focusOverwriteRun << ",overwrites=" << lens.focusOverwrites << '\n';
            }
            else
                lens.focusOverwriteRun = 0;
        }
        if (!lens.pauseForDistance && lens.focusOverwriteRun >= 3)
        {
            lens.pauseForDistance = true;
            if (g_camLensLogs.Take(400))
                LogFile() << "CAMERA_DOF,phase=pause_for_distance,cam=" << which << '\n';
        }
        if (lens.pauseForDistance)
        {
            std::uint16_t flags = 0;
            if (ReadCamFlags(cam, flags) && (flags & kCamFocusPausedBit) == 0)
                CAM::_PAUSE_CAMERA_FOCUS(cam, TRUE);
            g_cameraLive.flags |= replay::editor::CameraLiveFocusPaused;
        }
        CAM::_SET_CAM_FOCUS_DISTANCE(cam, distance);
        lens.focusSent = distance;
        if (lens.focusOverwriteRun)
            g_cameraLive.flags |= replay::editor::CameraLiveFocusOverwritten;
    }

    // What the camera holds now, into the editor's live info and, on a
    // change, the log (requested against read back).
    void NoteDofReadback(Cam cam, const char* which, const replay::camera::FocusPlan& focus, float lens, float focusSent,
        std::uint32_t markerId)
    {
        const DofReadback back = ReadCamDof(cam);
        g_cameraLive.flags |= replay::editor::CameraLiveDofActive | (back.ok ? replay::editor::CameraLiveDofRead : 0u);
        g_cameraLive.focusSource = static_cast<std::uint32_t>(focus.source);
        g_cameraLive.dofFStopSet = focus.fstop;
        g_cameraLive.dofFStopRead = back.ok ? back.fstop : 0.0f;
        g_cameraLive.dofLensSet = lens;
        g_cameraLive.dofLensRead = back.ok ? back.lens : 0.0f;
        g_cameraLive.focusSet = focusSent;
        g_cameraLive.focusRead = back.focusOk ? back.focus : 0.0f;
        replay::camera::InFocusRange(focus.fstop, lens, focusSent > 0.0f ? focusSent : focus.distance, g_cameraLive.inFocusNear,
            g_cameraLive.inFocusFar);
        static std::uint32_t lastMarker = ~0u;
        static replay::camera::FocusSource lastSource = replay::camera::FocusSource::Default;
        static float lastFStop = -1.0f, lastLens = -1.0f, lastFocus = -1.0f;
        static const char* lastWhich = "";
        const bool changed = markerId != lastMarker || focus.source != lastSource || which != lastWhich ||
            std::fabs(focus.fstop - lastFStop) > 0.05f * std::max(0.1f, lastFStop) || std::fabs(lens - lastLens) > 0.5f ||
            std::fabs(focusSent - lastFocus) > 0.1f * std::max(0.2f, lastFocus);
        const int freezeState = PhotoFreezeState();
        if (freezeState == 2 || freezeState == 3)
            g_cameraLive.flags |= replay::editor::CameraLiveFrozen;
        // Any change of the rendered camera's DOF is shown through the freeze, not
        // only the same marker's change: a marker whose DOF was Default would read its
        // first change to Custom as a marker switch and the frozen frame would stay.
        if (changed)
            g_lensChangedAt = GetTickCount64();
        if (!changed)
            return;
        // The last values follow every change (not only the logged ones: a
        // full log cap must not read as a change every tick).
        lastMarker = markerId;
        lastSource = focus.source;
        lastFStop = focus.fstop;
        lastLens = lens;
        lastFocus = focusSent;
        lastWhich = which;
        if (!g_camLensLogs.Take(400))
            return;
        LogFile() << "CAMERA_DOF,phase=readback,cam=" << which << ",marker=" << markerId
                  << ",source=" << replay::camera::FocusSourceName(focus.source) << ",fstop_set=" << focus.fstop
                  << ",fstop_read=" << (back.ok ? back.fstop : -1.0f) << ",lens_set=" << lens << ",lens_read=" << (back.ok ? back.lens : -1.0f)
                  << ",use_fstop=" << static_cast<int>(back.useFStop) << ",lens_fov=" << static_cast<int>(back.lensFov)
                  << ",focus_set=" << focusSent << ",focus_read=" << (back.focusOk ? back.focus : -1.0f)
                  << ",in_focus_near_m=" << g_cameraLive.inFocusNear << ",in_focus_far_m=" << g_cameraLive.inFocusFar
                  << ",block=" << (back.ok ? 1 : 0) << ",freeze_state=" << freezeState << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // PlanFocus picks the entity (Target, else the main character; Auto: Look At,
    // attach, main character on screen) and the focus plane depth along the view
    // axis to its head; the distance is sent every frame on an unpaused camera
    // (pausing the focus before _SET_CAM_FOCUS_DISTANCE is not a measured
    // combination); the engine autofocus only when no entity is there. The DOF of
    // the instant is BlendFocus (interpolated inside a blend, hysteresis on the
    // main character, the shot's last distance held) and the distance goes through
    // a spring; the memory starts over on a cut (`cut`). Nothing here makes a
    // camera unless a fallback says so (CAMERA_DOF).
    void ApplyMarkerDof(const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& state,
        std::uint64_t playheadMs, bool cut, float dtSeconds)
    {
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        const replay::camera::Keyframe* marker =
            plan.valid && plan.dofIndex >= 0 && plan.dofIndex < static_cast<int>(keys.size()) ? &keys[plan.dofIndex] : nullptr;
        if (cut)
            g_camFocusMemory.ResetShot();
        replay::camera::FocusPlan focus;
        bool blended = false;
        if (g_camDofRecreate)
            focus = replay::camera::PlanFocus(marker, state, MarkerScene(), playheadMs);
        else
        {
            const replay::camera::BlendedFocus mixed = replay::camera::BlendFocus(g_cameraTrack, plan, state, MarkerScene(),
                playheadMs, &g_camFocusMemory, g_camFocusConfig);
            focus = mixed.plan;
            blended = mixed.blended;
            if (blended)
                ++g_camDofBlendFrames;
        }
        {
            // Evidence for both paths: changes of focus source inside a shot.
            static replay::camera::FocusSource shotSource = replay::camera::FocusSource::Default;
            using replay::camera::FocusSource;
            if (!cut && focus.source != shotSource && shotSource != FocusSource::Default &&
                focus.source != FocusSource::Default)
            {
                ++g_camFocusSourceChanges;
                if ((shotSource == FocusSource::Engine) != (focus.source == FocusSource::Engine))
                    ++g_camFocusEngineFlips;
            }
            shotSource = focus.source;
        }
        if (focus.source == replay::camera::FocusSource::Default)
        {
            g_replayLens.focusSent = -1.0f; // nothing of ours to compare with
            if (g_replayCamDof)
            {
                g_lensChangedAt = GetTickCount64(); // Custom back to Default, shown through the freeze too
                if (g_camDofRecreate)
                    ForgetReplayCamDof(state); // Default: a fresh camera drops the block
                else
                    DropReplayCamDof(state);
            }
            return;
        }
        const bool engineFocus = focus.source == replay::camera::FocusSource::Engine;
        if (g_replayCamEngineFocus && !engineFocus)
        {
            if (g_camDofRecreate ? !ForgetReplayCamDof(state) : !LeaveEngineFocus(state))
                return; // recreated next tick, then focused by distance
            if (!g_replayCam)
                return;
        }
        // The shop scripts' block, the lens from the marker (SendDofBlock).
        const float lens = SendDofBlock(g_replayCam, focus, state.fov);
        float sentDistance = focus.distance;
        if (engineFocus)
        {
            if (!g_replayCamEngineFocus)
            {
                std::uint16_t word = 0;
                g_replayCamFocusPausedBefore = ReadCamFlags(g_replayCam, word) ? ((word & kCamFocusPausedBit) ? 1 : 0) : -1;
                CAM::_PAUSE_CAMERA_FOCUS(g_replayCam, FALSE);
            }
            g_replayCamEngineFocus = true;
            g_replayLens.focusSent = -1.0f;
        }
        else if (focus.source != replay::camera::FocusSource::Sharp)
        {
            if (!g_camDofRecreate && g_camFocusConfig.smoothOmega > 0.0f)
                sentDistance = replay::camera::SmoothFocusDistance(g_camFocusMemory, focus.distance, cut ? 0.0f : dtSeconds,
                    g_camFocusConfig.smoothOmega);
            SendFocusDistance(g_replayCam, g_replayLens, sentDistance, "replay"); // checked, read back
        }
        else
            g_replayLens.focusSent = -1.0f;
        CAM::ALLOW_MOTION_BLUR_DECAY(g_replayCam, FALSE);
        g_replayCamDof = true;
        NoteDofReadback(g_replayCam, "replay", focus, lens, engineFocus || focus.source == replay::camera::FocusSource::Sharp ? 0.0f : sentDistance,
            marker ? marker->id : 0);

        // One line when the source, the entity or the marker changes, or the
        // distance moves more than 10 %, at most every 250 ms; capped.
        static replay::camera::FocusSource lastSource = replay::camera::FocusSource::Default;
        static std::uint32_t lastEntity = ~0u, lastMarker = ~0u;
        static float lastDistance = -1.0f;
        static ULONGLONG lastAt = 0;
        const ULONGLONG now = GetTickCount64();
        const std::uint32_t markerId = marker ? marker->id : 0;
        const bool changed = focus.source != lastSource || focus.entity != lastEntity || markerId != lastMarker;
        const bool moved = lastDistance > 0.0f && std::fabs(focus.distance - lastDistance) > 0.1f * lastDistance && now - lastAt >= 250;
        // Camera evidence (up to 4 lines a second while a clip plays), Debug=1 only.
        if ((changed || moved) && replay::log::Verbose() && g_markerFocusLogs.Take(400))
        {
            LogFile() << "MARKER_FOCUS,marker=" << markerId << ",source=" << replay::camera::FocusSourceName(focus.source)
                      << ",engine_autofocus=" << (engineFocus ? 1 : 0) << ",entity=" << focus.entity
                      << ",focus_depth_m=" << (engineFocus ? 0.0f : focus.distance) << ",sent_m=" << (engineFocus ? 0.0f : sentDistance)
                      << ",target_distance_m=" << focus.straightMeters << ",fstop=" << focus.fstop << ",lens_mm=" << lens
                      << ",blended=" << (blended ? 1 : 0) << ",phase=" << plan.phase
                      << ",intensity=" << (marker ? marker->dofIntensity : 0.0f)
                      << ",focus_mode=" << (marker ? static_cast<int>(marker->focus) : -1)
                      << ",focus_target=" << (marker ? marker->focusTarget : 0)
                      << ",look_at=" << (marker ? marker->lookAtTarget : 0) << ",attach=" << (marker ? marker->attachTarget : 0)
                      << ",missing=" << (focus.targetMissing ? 1 : 0) << ",playhead_ms=" << playheadMs << '\n';
            lastSource = focus.source;
            lastEntity = focus.entity;
            lastMarker = markerId;
            lastDistance = focus.distance;
            lastAt = now;
        }
    }

    // CAMERA_FOLLOW, twice a second while a marker with an attach or Look At target
    // plays (and on each marker change), capped. follow_err_m: the camera against
    // the marker's own attach pose at this time (0 when it follows exactly; a
    // blend or a shake moves it on purpose, see blending and smooth_lag_m). rel:
    // camera minus target in the target's heading frame (constant while an attach
    // Default follows). frame_err: where the Look At target sits on screen against
    // the marker's framing. dt_game is GET_FRAME_TIME, dt_real the frame, dt_clip
    // what the springs took.
    replay::log::LogLimit g_cameraFollowLogs; // per session
    double MarkerSpeedAt(std::uint64_t timeMs); // below, with the marker speed

    void LogCameraFollow(const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& state,
        std::uint64_t playheadMs, float realDt, float clipDt)
    {
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        if (!plan.valid || plan.current < 0 || plan.current >= static_cast<int>(keys.size()) || g_cameraFollowLogs.Seen() >= 400)
            return;
        const replay::camera::Keyframe& marker = keys[plan.current];
        const std::uint32_t target = marker.attachTarget ? marker.attachTarget : marker.lookAtTarget;
        if (!target || marker.cameraMode != replay::camera::CameraMode::Free)
            return;
        static std::uint32_t lastMarker = 0;
        static ULONGLONG lastAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (marker.id == lastMarker && now - lastAt < 500)
            return;
        lastMarker = marker.id;
        lastAt = now;
        (void)g_cameraFollowLogs.Take(400);
        const replay::camera::Scene scene = MarkerScene();
        replay::camera::EntityState entity{};
        bool boneMissing = false; // the attach (else Look At) target through its bone
        const bool found = marker.attachTarget ? replay::camera::LookupAttach(marker, playheadMs, scene, entity, &boneMissing)
                                               : replay::camera::LookupLookAt(marker, playheadMs, scene, entity, &boneMissing);
        const replay::editor::Vec3 aim = entity.hasHead ? entity.head
            : replay::editor::Vec3{entity.position.x, entity.position.y, entity.position.z + replay::camera::kLookAtAimHeight};
        const float ax = aim.x - state.position.x, ay = aim.y - state.position.y, az = aim.z - state.position.z;
        const replay::editor::Vec3 rel = replay::camera::ToHeadingFrame(
            {state.position.x - entity.position.x, state.position.y - entity.position.y, state.position.z - entity.position.z},
            entity.heading);
        const replay::camera::LivePose own = replay::camera::Resolve(marker, playheadMs, scene);
        const float ex = state.position.x - own.pose.position.x, ey = state.position.y - own.pose.position.y,
                    ez = state.position.z - own.pose.position.z;
        float frameErr = -1.0f;
        if (found && marker.lookAtTarget)
        {
            replay::editor::Vec2 screen{};
            if (replay::camera::ProjectToScreen(state, aim, scene.aspect, screen))
                frameErr = std::sqrt((screen.x - marker.lookAtOffset.x) * (screen.x - marker.lookAtOffset.x) +
                    (screen.y - marker.lookAtOffset.y) * (screen.y - marker.lookAtOffset.y));
        }
        LogFile() << "CAMERA_FOLLOW,marker=" << marker.id << ",target=" << target << ",found=" << (found ? 1 : 0)
                  << ",attach=" << marker.attachTarget << ",attach_type=" << static_cast<int>(marker.attachType)
                  << ",look_at=" << marker.lookAtTarget << ",attach_bone=0x" << std::hex << marker.attachBone
                  << ",look_bone=0x" << marker.lookAtBone << std::dec << ",bone_missing=" << (boneMissing ? 1 : 0)
                  << ",blend=" << static_cast<int>(marker.blend)
                  << ",blending=" << (plan.blending ? 1 : 0) << ",smooth=" << (plan.smooth ? 1 : 0)
                  << ",smooth_anchor=" << plan.smoothAnchor << ",smooth_lag_m=" << plan.smoothLagMeters
                  << ",follow_err_m=" << std::sqrt(ex * ex + ey * ey + ez * ez)
                  << ",target_m=" << std::sqrt(ax * ax + ay * ay + az * az)
                  << ",rel=" << rel.x << '/' << rel.y << '/' << rel.z << ",frame_err=" << frameErr
                  << ",speed_pct=" << static_cast<int>(MarkerSpeedAt(playheadMs) * 100.0 + 0.5)
                  << ",rate=" << g_transport.Rate() << ",time_scale=" << g_worldTimeScaleApplied
                  << ",dt_game=" << MISC::GET_FRAME_TIME() << ",dt_real=" << realDt << ",dt_clip=" << clipDt
                  << ",shake=" << static_cast<int>(replay::camera::mshake::SummaryType(replay::camera::mshake::HeldAt(g_cameraTrack, playheadMs)))
                  << ",playhead_ms=" << playheadMs << '\n';
    }

    // Once a tick: the final rendered frame's cut bits (rung 2, guarded
    // read at the address the signature of 0x174AE8 gives), one
    // CAMERA_CUT_RATE line a second while a clip is open (every 10 s
    // outside it, the game's own baseline).
    std::uint32_t g_camCutFlagsRva = 0;
    bool g_camCutFlagsChecked = false;
    replay::camcut::Rate g_camCutRate;
    std::uint32_t g_camCutGameSeconds = 0;

    void UpdateCameraCutRate(ULONGLONG now)
    {
        static std::uintptr_t base = 0;
        if (!base)
            base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_camCutFlagsChecked)
        {
            g_camCutFlagsChecked = true;
            std::uint8_t code[replay::camcut::kCutTestBytes]{};
            const bool read = replay::mem::Copy(code, base + replay::camcut::kFinalFrameCutTestRva, sizeof(code));
            const std::uint32_t rva = read ? replay::camcut::FlagsRvaFromCode(code, replay::camcut::kFinalFrameCutTestRva) : 0u;
            g_camCutFlagsRva = rva == replay::camcut::kFinalFrameFlagsRva ? rva : 0u;
            LogFile() << "CAMERA_CUT_MODE,phase=config,mode=" << replay::camcut::ModeName(g_camCutMode)
                      << ",rate_probe=" << (g_camCutFlagsRva ? "ok" : read ? "signature_mismatch" : "read_fault")
                      << ",flags_rva=0x" << std::hex << rva << std::dec << '\n';
        }
        const std::uint32_t placed = g_camPlacedCutsThisTick;
        g_camPlacedCutsThisTick = 0;
        if (!g_camCutFlagsRva)
            return;
        std::uint16_t flags = 0;
        if (!replay::mem::Read(base + g_camCutFlagsRva, flags))
            return;
        if (!g_camCutRate.Add(now, (flags & replay::camcut::kCutBits) != 0, placed != 0))
            return;
        const std::uint32_t bitsCleared = g_camBitsCleared, bitsBefore = g_camBitsSeenBefore;
        g_camBitsCleared = 0;
        g_camBitsSeenBefore = 0;
        const bool open = g_transport.Playing();
        if (!open && ++g_camCutGameSeconds % 10 != 0)
            return;
        const char* mode = !open ? "game" : g_freeCamEnabled ? "freecam" : g_transport.Paused() ? "pause" : "play";
        const char* cutMode = g_camBitsFailed && g_camCutMode != replay::camcut::Mode::Legacy ? "legacy_fallback"
            : g_camCutMode == replay::camcut::Mode::Params && g_camCutParamsFailed            ? "bits_fallback"
                                                                                            : replay::camcut::ModeName(g_camCutMode);
        LogFile() << "CAMERA_CUT_RATE,mode=" << mode << ",cut_mode=" << cutMode
                  << ",frames=" << g_camCutRate.frames << ",cut_frames=" << g_camCutRate.cuts
                  << ",placed_cuts=" << g_camCutRate.placedCuts << ",bits_cleared=" << bitsCleared
                  << ",bits_left_before=" << bitsBefore << ",dof_recreations=" << g_camDofRecreations
                  // Session totals, but the target drift (this second).
                  << ",dof_clears=" << g_camDofClears << ",focus_restores=" << g_camFocusRestores
                  << ",dof_blend_frames=" << g_camDofBlendFrames << ",focus_engine_flips=" << g_camFocusEngineFlips
                  << ",focus_source_changes=" << g_camFocusSourceChanges << ",target_recorded=" << g_camTargetRecordedHits
                  << ",target_live=" << g_camTargetLiveHits << ",target_drift_max_m=" << g_camTargetDriftMaxM
                  << ",target_drift_over_5cm=" << g_camTargetDriftOver5cm << ",smooth_reframes=" << g_cameraSpring.reframes
                  << ",smooth_resets=" << g_cameraSpring.resets << ",prefetch_started=" << g_camPrefetchStarted
                  << ",prefetch_loaded_at_cut=" << g_camPrefetchLoadedAtCut
                  << ",prefetch_not_loaded_at_cut=" << g_camPrefetchNotLoadedAtCut
                  << ",prefetch_cancelled=" << g_camPrefetchCancelled
                  // Bone lookups (this second) and engine overwrites (session).
                  << ",bone_recorded=" << g_camBoneRecorded << ",bone_live=" << g_camBoneLive << ",bone_missing=" << g_camBoneMissing
                  << ",near_overwrites=" << (g_replayLens.nearOverwrites + g_freeLens.nearOverwrites)
                  << ",focus_overwrites=" << (g_replayLens.focusOverwrites + g_freeLens.focusOverwrites)
                  << ",playhead_ms=" << (open ? g_transport.PlayheadMs() : 0) << '\n';
        g_camTargetDriftMaxM = 0.0f;
        g_camTargetDriftOver5cm = 0;
        g_camBoneRecorded = g_camBoneLive = g_camBoneMissing = 0;
    }

    void UpdateCamPrefetch(bool keyed, const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& camera,
        std::uint64_t playheadMs, bool normalPlay)
    {
        const bool busy = g_clipWorld.loadSceneActive || g_exitLoad.active || g_settle.Active() || ClipLoadWaiting();
        const ULONGLONG now = GetTickCount64();
        if (g_camPrefetch.active)
        {
            if (busy || !normalPlay || !keyed || !g_camPrefetchOn)
            {
                StopCamPrefetch(busy ? "load_busy" : !normalPlay ? "not_playing" : "no_markers");
                return;
            }
            const bool loaded = STREAMING::IS_LOAD_SCENE_LOADED() != FALSE;
            if (!g_camPrefetch.cutSeen && playheadMs >= g_camPrefetch.cutMs)
            {
                g_camPrefetch.cutSeen = true;
                g_camPrefetch.cutAt = now;
                ++(loaded ? g_camPrefetchLoadedAtCut : g_camPrefetchNotLoadedAtCut);
                if (g_camPrefetchLogs.Take(200))
                    LogFile() << "CAMERA_PREFETCH,phase=cut,cut_ms=" << g_camPrefetch.cutMs << ",loaded=" << (loaded ? 1 : 0)
                              << ",lead_real_ms=" << (now - g_camPrefetch.startedAt) << ",meters=" << g_camPrefetch.meters
                              << ",rendered_scene_loaded=" << (STREAMING::IS_RENDERED_SCENE_LOADED() ? 1 : 0)
                              << ",playhead_ms=" << playheadMs << '\n';
            }
            if (g_camPrefetch.cutSeen && (loaded || now - g_camPrefetch.cutAt > 2000))
                StopCamPrefetch(loaded ? "loaded" : "timeout");
            return;
        }
        if (!normalPlay)
            g_camPrefetchDoneCutMs = ~0ull; // a seek or a pause: the same cut may be served again
        if (!g_camPrefetchOn || busy || !normalPlay || !keyed || STREAMING::IS_LOAD_SCENE_ACTIVE())
            return;
        const replay::camera::PrefetchTarget target = replay::camera::PlanPrefetch(g_cameraTrack, plan, playheadMs, camera,
            MarkerScene(), g_camPrefetchLeadMs, g_camPrefetchMeters);
        if (!target.want || target.cutMs == g_camPrefetchDoneCutMs)
            return;
        const float pitch = target.view.rotation.x * 3.14159265f / 180.0f;
        const float heading = target.view.rotation.z * 3.14159265f / 180.0f;
        g_camPrefetchDoneCutMs = target.cutMs;
        if (!STREAMING::LOAD_SCENE_START(target.view.position.x, target.view.position.y, target.view.position.z,
                -std::sin(heading) * std::cos(pitch), std::cos(heading) * std::cos(pitch), std::sin(pitch), 500.0f, 4))
        {
            if (g_camPrefetchLogs.Take(200))
                LogFile() << "CAMERA_PREFETCH,phase=refused,cut_ms=" << target.cutMs << ",meters=" << target.meters
                          << ",playhead_ms=" << playheadMs << '\n';
            return;
        }
        g_camPrefetch.active = true;
        g_camPrefetch.cutMs = target.cutMs;
        g_camPrefetch.meters = target.meters;
        g_camPrefetch.startedAt = now;
        ++g_camPrefetchStarted;
        if (g_camPrefetchLogs.Take(200))
            LogFile() << "CAMERA_PREFETCH,phase=start,cut_ms=" << target.cutMs << ",meters=" << target.meters
                      << ",at=" << target.view.position.x << '/' << target.view.position.y << '/' << target.view.position.z
                      << ",playhead_ms=" << playheadMs << '\n';
    }

    // The detector for the corner at a marker. Each time normal play crosses a
    // marker inside a move (the segment before and after both blend), one
    // CAMERA_MARKER_PASS line: the camera's speed (metres per clip second) on the
    // tick before and the tick after the marker, the jump between them and how far
    // the direction turned. A Linear chain turns a corner there; a Spline one
    // should not.
    replay::log::LogLimit g_markerPassLogs;
    replay::log::LogLimit g_cameraFxLogs; // CAMERA_FX

    void DetectMarkerPass(bool keyed, const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& state,
        std::uint64_t playheadMs, bool normalPlay)
    {
        static replay::editor::Vec3 lastPosition{};
        static std::uint64_t lastPlayhead = ~0ull;
        static int lastCurrent = -1;
        static bool lastBlending = false;
        static float lastSpeed = 0.0f;
        static replay::editor::Vec3 lastDirection{};
        static bool pending = false;
        static std::uint32_t pendingMarker = 0;
        static float pendingSpeed = 0.0f;
        static replay::editor::Vec3 pendingDirection{};
        static int pendingBlend = 0;
        if (!keyed || !normalPlay || lastPlayhead == ~0ull || playheadMs <= lastPlayhead || playheadMs - lastPlayhead > 200)
        {
            pending = false;
            lastSpeed = 0.0f;
        }
        else
        {
            const float clipDt = static_cast<float>(playheadMs - lastPlayhead) / 1000.0f;
            const float dx = state.position.x - lastPosition.x, dy = state.position.y - lastPosition.y,
                        dz = state.position.z - lastPosition.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float speed = distance / clipDt;
            const replay::editor::Vec3 direction = distance > 1e-4f ? replay::editor::Vec3{dx / distance, dy / distance, dz / distance}
                                                                    : replay::editor::Vec3{};
            if (pending)
            {
                pending = false;
                const float dot = direction.x * pendingDirection.x + direction.y * pendingDirection.y + direction.z * pendingDirection.z;
                const float turn = std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.29578f;
                const float jump = std::max(speed, pendingSpeed) > 0.05f
                    ? std::fabs(speed - pendingSpeed) / std::max(speed, pendingSpeed) * 100.0f : 0.0f;
                // The marker's Spline pacing and the weight in force.
                replay::editor::SplineTuning tuning = g_splineDefaults;
                if (const replay::camera::Keyframe* key = g_cameraTrack.Find(pendingMarker))
                    tuning = replay::camera::spline::Effective(key->spline, g_splineDefaults);
                if (g_markerPassLogs.Take(400))
                    LogFile() << "CAMERA_MARKER_PASS,marker=" << pendingMarker << ",blend=" << pendingBlend << ",speed_in=" << pendingSpeed
                              << ",speed_out=" << speed << ",jump_pct=" << jump << ",turn_deg=" << turn
                              << ",pacing=" << replay::camera::spline::PacingName(static_cast<replay::editor::SplinePacing>(tuning.pacing))
                              << ",ease=" << tuning.easeIn << '/' << tuning.easeOut
                              << ",weight=" << (SplineWeightActive() ? tuning.weight : 0.0f) << ",playhead_ms=" << playheadMs << '\n';
            }
            if (plan.valid && plan.current >= 0 && plan.current != lastCurrent && lastCurrent >= 0 && lastBlending && plan.blending &&
                plan.current < static_cast<int>(g_cameraTrack.Keyframes().size()))
            {
                pending = true;
                pendingMarker = g_cameraTrack.Keyframes()[plan.current].id;
                pendingSpeed = lastSpeed;
                pendingDirection = lastDirection;
                pendingBlend = static_cast<int>(g_cameraTrack.Keyframes()[plan.current].blend);
            }
            lastSpeed = speed;
            lastDirection = direction;
        }
        lastPosition = state.position;
        lastPlayhead = playheadMs;
        lastCurrent = plan.valid ? plan.current : -1;
        lastBlending = plan.valid && plan.blending;
    }

    void StopReplayCamera()
    {
        if (g_replayCam && CAM::DOES_CAM_EXIST(g_replayCam))
        {
            if (g_replayCamRendering && !g_freeCamEnabled)
                CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, TRUE, 0);
            CAM::DESTROY_CAM(g_replayCam, FALSE);
        }
        // With WorldFocus the focus stays set while a clip plays (another camera takes
        // it this tick); StopPlayback clears it.
        if (g_replayCam && !(g_worldFocus && g_transport.Playing()))
            STREAMING::CLEAR_FOCUS();
        g_renderedCamera.valid = false;
        if (!g_freeCamEnabled)
            g_lastPlacedView.valid = false;
        g_replayCam = 0;
        g_replayCamRendering = false;
        g_replayCamDof = false;
        g_replayCamEngineFocus = false;
        g_replayCamFocusPausedBefore = -1;
        StopCamPrefetch("camera_stopped");
        ForgetCamTargets();
    }

    // Once per tick after the playhead moved. The freecam, when on, owns the
    // view; turning it off hands the view back here on the next tick.
    void UpdateReplayCamera()
    {
        REPLAY_PROFILE_ZONE("tick.replaycam");
        if (!g_transport.Playing())
        {
            StopReplayCamera();
            return;
        }
        if (g_freeCamEnabled)
        {
            g_replayCamRendering = false;
            g_replayCamCut = {}; // back from the freecam: a cut
            g_renderedCamera.valid = false; // the freecam renders
            return;
        }
        replay::editor::CameraState state{};
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        // The real frame time (the performance counter; the game's GET_FRAME_TIME
        // follows SET_TIME_SCALE, 0.05 under a 5 % marker) and the clip time it played
        // (real x the playhead rate). The Smooth springs take the clip time: the same
        // rounding at every marker speed. The motion blur takes the real time: it is
        // how fast the picture moves on screen.
        static std::uint64_t lastTick = 0;
        const std::uint64_t tickNow = replay::prof::Now();
        float realDt = lastTick && tickNow > lastTick
            ? static_cast<float>(static_cast<double>(tickNow - lastTick) / static_cast<double>(replay::prof::TicksPerSecond()))
            : 0.0f;
        lastTick = tickNow;
        realDt = std::min(realDt, 0.2f); // a stalled frame: the transport's kMaxNormalStepMs
        // The Smooth blend's springs only run in normal forward play:
        // paused, scrubbing or stepping, dt 0 resets them.
        const bool normalPlay = !g_transport.Paused() && replay::playback::NormalPlay();
        const float dt = normalPlay ? replay::camera::ClipStepSeconds(realDt, g_transport.Rate()) : 0.0f;
        const float blurDt = normalPlay ? realDt : 0.0f;
        replay::camera::SegmentPlan plan;
        const bool keyed = g_cameraTrackEnabled && EvaluateCameraTrack(playheadMs, state, dt, &plan);
        if (!keyed && !replay::camera::EvaluateGame(g_clip.gameCamera, playheadMs, state, &g_clip.cuts))
        {
            StopReplayCamera(); // a clip without a game camera: the live game camera
            return;
        }
        if (!g_replayCam || !CAM::DOES_CAM_EXIST(g_replayCam))
        {
            g_replayCam = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA",
                state.position.x, state.position.y, state.position.z,
                state.rotation.x, state.rotation.y, state.rotation.z, state.fov, FALSE, 2);
            g_replayCamRendering = false;
            NewLensCamera(g_replayLens);
            g_replayCamDof = false;               // a fresh camera has no block
            g_replayCamEngineFocus = false;
            g_replayCamFocusPausedBefore = -1;
            if (!g_replayCam || !CAM::DOES_CAM_EXIST(g_replayCam))
            {
                g_replayCam = 0;
                return;
            }
            LogFile() << "PLAYBACK_CAMERA,created=1,samples=" << g_clip.gameCamera.size()
                      << ",keyframes=" << g_cameraTrack.Keyframes().size() << '\n';
        }
        // Another mod's script camera renders (camera_cut.h LendView): the replay lends
        // it the view (no placing, no activation, no focus) and takes it back when that
        // camera stops or the replay plays.
        {
            const Cam rendering = CAM::GET_RENDERING_CAM();
            const bool other = rendering != 0 && rendering != -1 && rendering != g_replayCam &&
                (!g_freeCam || rendering != g_freeCam);
            const bool lend = replay::camcut::LendView(g_camRelease, g_transport.Paused(), other);
            if (lend)
            {
                if (!g_camLent)
                {
                    g_camLent = true;
                    ++g_camLends;
                    // With WorldFocus the focus follows the lent camera (UpdateWorldFocus);
                    // clearing it would hand the world to the hidden live player, maybe far away.
                    if (!g_worldFocus)
                        STREAMING::CLEAR_FOCUS();
                    LogFile() << "CAMERA_RELEASE,phase=lend,focus=" << (g_worldFocus ? "follows" : "cleared")
                              << ",mode=" << replay::camcut::ReleaseName(g_camRelease)
                              << ",cam=" << rendering << ",paused=" << (g_transport.Paused() ? 1 : 0) << ",playhead_ms=" << playheadMs
                              << ",lends=" << g_camLends << '\n';
                }
                g_replayCamRendering = false; // activated and rendered again when taken back
                g_replayCamCut = {};          // and placed with a cut
                return;
            }
            if (g_camLent)
            {
                g_camLent = false;
                LogFile() << "CAMERA_RELEASE,phase=take_back,other_rendering=" << (other ? 1 : 0)
                          << ",paused=" << (g_transport.Paused() ? 1 : 0) << ",playhead_ms=" << playheadMs << '\n';
            }
        }
        // The rendered camera is the load scene's target. A jump's camera test (in the
        // jump's own tick): the camera moved more than 100 m, or more than 1 m onto
        // ground whose collision is not loaded, makes the jump far: the settle freezes
        // the picture (no black screen) and the load scene streams the new place (the
        // "jump" wait). A frame step never tests the camera.
        {
            const replay::clipload::Vec3 position{state.position.x, state.position.y, state.position.z};
            if (g_settleCameraCheck)
            {
                g_settleCameraCheck = false;
                if (g_clipWorld.active && g_settle.Active() && g_settleCameraFromValid)
                {
                    replay::settle::JumpFacts facts;
                    facts.windowCentreMs = facts.targetMs = playheadMs; // the window was tested at the jump
                    facts.cameraValid = true;
                    facts.cameraMovedMeters = std::sqrt(replay::clipload::DistanceSq(g_settleCameraFrom, position));
                    if (facts.cameraMovedMeters > g_settleConfig.collisionCheckMeters)
                    {
                        STREAMING::REQUEST_COLLISION_AT_COORD(position.x, position.y, position.z);
                        facts.collisionLoaded = ENTITY::HAS_COLLISION_LOADED_AROUND_POSITION(position.x, position.y, position.z) != FALSE;
                    }
                    const std::uint32_t bits = replay::settle::FarBitsOf(facts, g_settleConfig);
                    if (bits)
                    {
                        const bool wasFar = g_settle.Far();
                        g_settle.AddFar(bits);
                        if (!wasFar)
                            ++g_seekLoadSequence;
                        g_clipWorld.jumpMeters = facts.cameraMovedMeters;
                        if (!ClipLoadWaiting())
                        {
                            BeginClipLoadWait("jump", GetTickCount64());
                            g_settleJumpScene = true;
                        }
                        if (g_jumpLoadLogs.Take(kJumpLoadLogCap))
                        {
                            LogFile() << "JUMP_LOAD,phase=camera,reason=" << g_settle.Reason() << ",far="
                                      << replay::settle::FarReasons(g_settle.FarBits()) << ",camera_m=" << facts.cameraMovedMeters
                                      << ",collision=" << (facts.collisionLoaded ? 1 : 0) << ",target_ms=" << playheadMs << '\n';
                        }
                    }
                }
            }
            g_renderedCamera.valid = true;
            g_renderedCamera.position = position;
            g_renderedCamera.rotation = {state.rotation.x, state.rotation.y, state.rotation.z};
            g_renderedCamera.playheadMs = playheadMs;
        }
        // A script camera brings the game's cinematic letterbox; the replay's view has
        // none.
        CAM::SET_WIDESCREEN_BORDERS(FALSE, 0);
        bool placedCut = false;
        {
            // A cut only on a real one (camera_cut.h): the playhead jumped (seek, scrub,
            // frame step), the settle or the load wait holds the picture, the marker
            // changed without a blend, the view switched between markers and the game
            // camera, or a recorded cut was crossed. Paused, the frame converges.
            static std::uint64_t lastPlayhead = ~0ull;
            static replay::camcut::MarkerFrame lastMarker;
            static bool lastKeyed = false;
            bool forceCut = lastPlayhead == ~0ull;
            if (!normalPlay && playheadMs != lastPlayhead)
                forceCut = true;
            if (g_settle.Active() || ClipLoadWaiting())
                forceCut = true;
            if (keyed != lastKeyed)
                forceCut = true;
            // Cut when the frame before was not blending into the marker now current
            // (camera_cut.h MarkerBoundaryCut).
            const replay::camcut::MarkerFrame marker = MarkerFrameOf(keyed, plan);
            if (replay::camcut::MarkerBoundaryCut(lastMarker, marker))
                forceCut = true;
            if (normalPlay && lastPlayhead != ~0ull && playheadMs > lastPlayhead)
                for (const std::uint32_t c : g_clip.cuts)
                    if (c > lastPlayhead && c <= playheadMs)
                        forceCut = true;
            lastPlayhead = playheadMs;
            lastMarker = marker;
            lastKeyed = keyed;
            PlaceCam(g_replayCam, g_replayCamCut, "replay", state.position.x, state.position.y, state.position.z,
                state.rotation.x, state.rotation.y, state.rotation.z, state.fov, realDt, forceCut);
            placedCut = g_lastPlaceWhy != replay::camcut::kWhyNone;
        }
        // Streaming ahead of a cut far away.
        UpdateCamPrefetch(keyed, plan, state, playheadMs, normalPlay);
        // The markers' near clip (default 0.1), read back. The live info starts over
        // each tick (the rendered camera fills it).
        g_cameraLive = {};
        ApplyNearClip(g_replayCam, g_replayLens, keyed ? replay::camera::NearClipAt(g_cameraTrack, plan) : replay::editor::kNearClipDefault,
            "replay");
        {
            // Motion blur from the camera's own speed: still is sharp, a fast move smears,
            // as a real shutter does. Only in normal play (a scrub or a jump is not
            // movement).
            static replay::editor::Vec3 lastPosition{};
            static float blur = 0.0f;
            // Per real second (blurDt): the game's frame time under a 5 % marker would make
            // a slow camera count 20 times faster and smear the slow motion.
            float target = 0.0f;
            if (blurDt > 0.0f)
            {
                const float dx = state.position.x - lastPosition.x, dy = state.position.y - lastPosition.y,
                            dz = state.position.z - lastPosition.z;
                const float speedMs = std::sqrt(dx * dx + dy * dy + dz * dz) / blurDt;
                target = speedMs < 50.0f ? std::clamp((speedMs - 1.0f) / 15.0f, 0.0f, 0.6f) : 0.0f; // a cut is not speed
            }
            blur += (target - blur) * std::min(1.0f, blurDt * 8.0f);
            lastPosition = state.position;
            g_replayCamBlur = blurDt > 0.0f ? blur : 0.0f;
        }
        if (keyed)
            LogCameraFollow(plan, state, playheadMs, realDt, dt);
        DetectMarkerPass(keyed, plan, state, playheadMs, normalPlay);
        {
            // The shake marker in force, the markers' shake folded to the playhead
            // (CAMERA_SHAKE, evidence).
            static int lastShakeMarker = -2;
            const replay::camera::mshake::Resolved r = keyed ? replay::camera::mshake::Resolve(g_cameraTrack, playheadMs)
                                                             : replay::camera::mshake::Resolved{};
            if (r.marker != lastShakeMarker)
            {
                lastShakeMarker = r.marker;
                const std::uint32_t id = r.marker >= 0 ? g_cameraTrack.Keyframes()[r.marker].id : 0u;
                if (g_cameraFxLogs.Take(400))
                    LogFile() << "CAMERA_SHAKE,action=in_force,marker=" << id << ",active=" << (r.active ? 1 : 0)
                              << ",profile=" << replay::camera::fx::ShakeName(static_cast<replay::editor::ShakeType>(r.profile))
                              << ",gain=" << r.gain << ",amp=" << r.custom.amp
                              << ",hz=" << r.custom.hz << ",spdamp=" << r.custom.speedAmp << ",spdfreq=" << r.custom.speedHz
                              << ",rot=" << r.custom.rotWeight << ",pos=" << r.custom.posWeight
                              << ",still=" << static_cast<int>(r.custom.still) << ",seed=" << r.custom.seed
                              << ",blending=" << (r.blending ? 1 : 0) << ",cycles=" << r.cycles << ",playhead_ms=" << playheadMs << '\n';
            }
        }
        ApplyMarkerDof(keyed ? plan : replay::camera::SegmentPlan{}, state, playheadMs, placedCut, dt);
        if (!g_replayCam)
            return; // recreated next tick without the DOF block
        CAM::SET_CAM_MOTION_BLUR_STRENGTH(g_replayCam, g_replayCamBlur); // a camera ApplyMarkerDof replaced
        // The world streams around the camera, not around the player left where the
        // playback started. With WorldFocus the focus is set once per tick for every
        // camera by UpdateWorldFocus.
        g_replayCamPlacedThisTick = true;
        g_replayCamKeyedThisTick = keyed;
        if (!g_worldFocus)
            STREAMING::SET_FOCUS_POS_AND_VEL(state.position.x, state.position.y, state.position.z, 0.0f, 0.0f, 0.0f);
        if (!g_replayCamRendering)
        {
            CAM::SET_CAM_ACTIVE(g_replayCam, TRUE);
            CAM::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, TRUE, 0);
            g_replayCamRendering = true;
        }
    }

    // Once per tick after UpdateFreeCam and UpdateReplayCamera (world_focus_core.h).
    // While a clip plays (the replay owns the world) the streaming focus is put on
    // the camera that renders, whatever it is: the replay camera, the free camera,
    // Edit Camera, another mod's camera the view was lent to, or the live game
    // camera. Otherwise the focus would stay on the hidden live player, maybe
    // hundreds of metres away, and the puppets' hooves would never be created.
    // Rung 1 (natives). FOCUS on each owner change, FOCUS_SAMPLE once per second
    // (focus to camera distance, also with WorldFocus=0).
    void UpdateWorldFocus(ULONGLONG now)
    {
        using namespace replay::worldfocus;
        Inputs in;
        in.playing = g_transport.Playing();
        in.renderingCam = in.playing ? CAM::GET_RENDERING_CAM() : 0;
        in.replayCam = (g_replayCam && CAM::DOES_CAM_EXIST(g_replayCam)) ? g_replayCam : 0;
        in.freeCam = (g_freeCam && CAM::DOES_CAM_EXIST(g_freeCam)) ? g_freeCam : 0;
        in.freeCamEnabled = g_freeCamEnabled;
        in.editingMarker = g_editingMarker != 0;
        in.keyed = g_replayCamKeyedThisTick;
        in.replayCamPlaced = g_replayCamPlacedThisTick;
        g_replayCamPlacedThisTick = false;
        g_replayCamKeyedThisTick = false;
        if (!in.playing)
            return;
        const Source source = Choose(in);
        // The live listener put back while the replay camera renders the recorded game
        // camera (game_listener.h): a marker camera counts when it stands within
        // ListenerCamMatchM of the recorded game camera at the playhead.
        {
            float camErrM = -1.0f;
            if (source == Source::Marker || source == Source::Replay)
            {
                replay::editor::CameraState recorded{};
                if (replay::camera::EvaluateGame(g_clip.gameCamera, g_transport.PlayheadMs(), recorded, &g_clip.cuts))
                {
                    const Vector3 live = CAM::GET_FINAL_RENDERED_CAM_COORD();
                    const float rx = g_renderedCamera.valid ? g_renderedCamera.position.x : live.x;
                    const float ry = g_renderedCamera.valid ? g_renderedCamera.position.y : live.y;
                    const float rz = g_renderedCamera.valid ? g_renderedCamera.position.z : live.z;
                    const float dx = rx - recorded.position.x, dy = ry - recorded.position.y, dz = rz - recorded.position.z;
                    camErrM = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
            const bool follows = replay::gamelistener::FollowsRecordedCamera(source == Source::Replay,
                source == Source::Marker, camErrM, replay::gamelistener::CamMatchM());
            replay::gamelistener::Play(g_clip, static_cast<std::uint32_t>(g_transport.PlayheadMs()), follows, LogFile(),
                camErrM, replay::worldfocus::SourceName(source));
        }
        const Vector3 rendered = CAM::GET_FINAL_RENDERED_CAM_COORD();
        Vector3 at = rendered;
        if (source == Source::Replay || source == Source::Marker)
        {
            if (g_renderedCamera.valid)
            {
                at.x = g_renderedCamera.position.x;
                at.y = g_renderedCamera.position.y;
                at.z = g_renderedCamera.position.z;
            }
        }
        else if (source == Source::FreeCam || source == Source::Edit)
        {
            at = g_freeCamPosition;
        }
        if (g_worldFocus)
        {
            STREAMING::SET_FOCUS_POS_AND_VEL(at.x, at.y, at.z, 0.0f, 0.0f, 0.0f);
            g_worldFocusAt = at;
            g_worldFocusTracker.owned = true;
        }
        else if (in.replayCamPlaced)
        {
            g_worldFocusAt = at; // only the replay camera sets it
        }
        const Ped player = PLAYER::PLAYER_PED_ID();
        const bool playerExists = player && ENTITY::DOES_ENTITY_EXIST(player);
        const Vector3 playerAt = playerExists ? ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE) : Vector3{};
        auto dist = [](const Vector3& a, const Vector3& b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const bool changed = Update(g_worldFocusTracker, source);
        const bool sample = SampleDue(g_worldFocusTracker, now);
        if (!changed && !sample)
            return;
        const bool playerIsFocus = playerExists && STREAMING::IS_ENTITY_FOCUS(player) != FALSE;
        std::ofstream& log = LogFile();
        log << (changed ? "FOCUS" : "FOCUS_SAMPLE") << ",source=" << SourceName(source)
            << ",set=" << (g_worldFocus ? 1 : 0) << ",cam=" << in.renderingCam
            << ",at=" << at.x << ';' << at.y << ';' << at.z
            << ",focus_cam_m=" << dist(g_worldFocusAt, rendered)
            << ",player_cam_m=" << (playerExists ? dist(playerAt, rendered) : -1.0f)
            << ",player_is_focus=" << (playerIsFocus ? 1 : 0)
            << ",lent=" << (g_camLent ? 1 : 0) << ",paused=" << (g_transport.Paused() ? 1 : 0)
            << ",playhead_ms=" << g_transport.PlayheadMs();
        if (changed)
            log << ",changes=" << g_worldFocusTracker.changes;
        log << '\n';
    }

    // The focus the replay set does not outlive the clip.
    void ReleaseWorldFocus(const char* why)
    {
        const bool owned = g_worldFocusTracker.owned;
        if (owned)
            STREAMING::CLEAR_FOCUS();
        if (owned || g_worldFocusTracker.changes)
            LogFile() << "FOCUS,source=none,why=" << why << ",cleared=" << (owned ? 1 : 0)
                      << ",changes=" << g_worldFocusTracker.changes << ",samples=" << g_worldFocusTracker.samples << '\n';
        replay::worldfocus::Reset(g_worldFocusTracker);
        g_replayCamPlacedThisTick = false;
        g_replayCamKeyedThisTick = false;
    }

    bool StartFreeCam(ULONGLONG now)
    {
        if (!g_transport.Playing())
        {
            ShowObjective("Redemption Editor: the free camera can only be used during PLAY");
            return false;
        }

        StopFreeCam();
        g_freeCamPosition = CAM::GET_FINAL_RENDERED_CAM_COORD();
        g_freeCamRotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
        g_freeCamFov = CAM::GET_FINAL_RENDERED_CAM_FOV();
        g_freeCam = CAM::CREATE_CAM_WITH_PARAMS(
            "DEFAULT_SCRIPTED_CAMERA",
            g_freeCamPosition.x,
            g_freeCamPosition.y,
            g_freeCamPosition.z,
            g_freeCamRotation.x,
            g_freeCamRotation.y,
            g_freeCamRotation.z,
            g_freeCamFov,
            TRUE,
            2);

        if (!g_freeCam || !CAM::DOES_CAM_EXIST(g_freeCam))
        {
            g_freeCam = 0;
            ShowObjective("Redemption Editor: could not create the free camera");
            return false;
        }

        g_freeCamPlayer = PLAYER::PLAYER_PED_ID();
        if (g_freeCamPlayer && ENTITY::DOES_ENTITY_EXIST(g_freeCamPlayer))
        {
            g_freeCamPlayerWasFrozen = ENTITY::_IS_ENTITY_FROZEN(g_freeCamPlayer) != FALSE;
            if (!g_freeCamPlayerWasFrozen)
                ENTITY::FREEZE_ENTITY_POSITION(g_freeCamPlayer, TRUE);
        }

        CAM::SET_CAM_ACTIVE(g_freeCam, TRUE);
        CAM::RENDER_SCRIPT_CAMS(TRUE, TRUE, 250, TRUE, TRUE, 0);
        NewLensCamera(g_freeLens);
        g_freeCamLastUpdateAt = now;
        g_freeCamEnabled = true;
        g_cameraTrackManualOverride = g_cameraTrack.Empty();
        ShowObjective("Redemption Editor: free camera on");
        return true;
    }

    // Where Edit Camera starts: the marker's live view. A Free marker still on the
    // placeholder pose (the world origin, saved in older clips) starts from the
    // recorded game camera at its time instead of far away.
    replay::camera::LivePose EditStartPose(const replay::camera::Keyframe& key)
    {
        replay::camera::LivePose live = replay::camera::Resolve(key, key.timeMs, MarkerScene());
        const replay::editor::Vec3& at = key.pose.position;
        const bool placeholder = key.cameraMode == replay::editor::CameraMode::Free && key.attachTarget == 0 &&
            at.x == 0.0f && at.y == 0.0f && at.z == 0.0f;
        replay::editor::CameraState game{};
        if (placeholder && replay::camera::EvaluateGame(g_clip.gameCamera, key.timeMs, game, &g_clip.cuts))
        {
            live.pose.position = game.position;
            const replay::editor::Vec3 rotation = live.hasLookAt
                ? replay::camera::LookAtWithScreenOffset(live.lookAtPoint, game.position, live.lookAtOffset, live.pose.fov,
                      live.lookAtRoll, MarkerScene().aspect)
                : game.rotation;
            live.pose.orientation = replay::camera::Track::FromEulerDegrees(rotation);
            LogFile() << "CAMERA_MARKER,action=edit_placeholder,id=" << key.id << '\n';
        }
        return live;
    }

    // Edit Camera on a marker with a Look At target (an orbit): the camera circles
    // the target's head. A/D yaw, W/S pitch, arrows up/down distance, Q/E the aim
    // height, the mouse the framing (where the target sits on screen), Z/C roll,
    // PgUp/PgDn zoom.
    struct OrbitEdit
    {
        std::uint32_t marker = 0;
        float yaw = 0.0f;   // degrees, heading frame (0 faces +Y)
        float pitch = 0.0f; // degrees, camera above the point when > 0
        float distance = 3.0f;
        float height = 0.0f;
        replay::editor::Vec2 offset{};
        float roll = 0.0f;
    };
    OrbitEdit g_orbit;

    bool UpdateOrbitEdit(float deltaSeconds, float lookX, float lookY)
    {
        const replay::camera::Keyframe* key = g_editingMarker ? g_cameraTrack.Find(g_editingMarker) : nullptr;
        replay::camera::EntityState target{};
        if (!key || !key->lookAtTarget || key->cameraMode != replay::editor::CameraMode::Free ||
            !replay::camera::LookupLookAt(*key, key->timeMs, MarkerScene(), target)) // around the bone
        {
            g_orbit.marker = 0;
            return false;
        }
        const replay::editor::Vec3 base = target.hasHead ? target.head
            : replay::editor::Vec3{target.position.x, target.position.y, target.position.z + replay::camera::kLookAtAimHeight};
        constexpr float kDeg = 3.14159265f / 180.0f;
        if (g_orbit.marker != key->id)
        {
            // Start from where the camera is now around the target.
            g_orbit = {};
            g_orbit.marker = key->id;
            const float dx = g_freeCamPosition.x - base.x, dy = g_freeCamPosition.y - base.y, dz = g_freeCamPosition.z - base.z;
            g_orbit.distance = std::max(0.5f, std::sqrt(dx * dx + dy * dy + dz * dz));
            g_orbit.yaw = std::atan2(-dx, dy) / kDeg;
            g_orbit.pitch = std::asin(std::clamp(dz / g_orbit.distance, -1.0f, 1.0f)) / kDeg;
            g_orbit.offset = key->lookAtOffset;
            g_orbit.roll = key->lookAtRoll;
        }
        const bool fast = InterlockedCompareExchange(&g_fastMove, 0, 0) != 0;
        const float speed = fast ? 3.0f : 1.0f;
        if (InterlockedCompareExchange(&g_moveLeft, 0, 0)) g_orbit.yaw -= 60.0f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveRight, 0, 0)) g_orbit.yaw += 60.0f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveForward, 0, 0)) g_orbit.pitch += 40.0f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveBackward, 0, 0)) g_orbit.pitch -= 40.0f * speed * deltaSeconds;
        g_orbit.pitch = std::clamp(g_orbit.pitch, -80.0f, 85.0f);
        if (InterlockedCompareExchange(&g_turnUp, 0, 0)) g_orbit.distance *= 1.0f - 0.8f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_turnDown, 0, 0)) g_orbit.distance *= 1.0f + 0.8f * speed * deltaSeconds;
        g_orbit.distance = std::clamp(g_orbit.distance, 0.4f, 150.0f);
        if (InterlockedCompareExchange(&g_moveUp, 0, 0)) g_orbit.height += 0.8f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveDown, 0, 0)) g_orbit.height -= 0.8f * speed * deltaSeconds;
        g_orbit.height = std::clamp(g_orbit.height, -2.0f, 3.0f);
        g_orbit.offset.x = std::clamp(g_orbit.offset.x + lookX * 0.6f * deltaSeconds * 10.0f, -1.0f, 1.0f);
        g_orbit.offset.y = std::clamp(g_orbit.offset.y - lookY * 0.6f * deltaSeconds * 10.0f, -1.0f, 1.0f);
        if (InterlockedCompareExchange(&g_rollLeft, 0, 0)) g_orbit.roll -= 30.0f * deltaSeconds;
        if (InterlockedCompareExchange(&g_rollRight, 0, 0)) g_orbit.roll += 30.0f * deltaSeconds;
        const float fovAmount = 30.0f * deltaSeconds;
        if (InterlockedCompareExchange(&g_fovIn, 0, 0)) g_freeCamFov -= fovAmount;
        if (InterlockedCompareExchange(&g_fovOut, 0, 0)) g_freeCamFov += fovAmount;
        g_freeCamFov = std::clamp(g_freeCamFov, replay::editor::kMarkerFovMin, replay::editor::kMarkerFovMax);

        const replay::editor::Vec3 point{base.x, base.y, base.z + g_orbit.height};
        const float cp = std::cos(g_orbit.pitch * kDeg), sp = std::sin(g_orbit.pitch * kDeg);
        const float sy = std::sin(g_orbit.yaw * kDeg), cy = std::cos(g_orbit.yaw * kDeg);
        const replay::editor::Vec3 position{point.x - sy * cp * g_orbit.distance, point.y + cy * cp * g_orbit.distance,
            point.z + sp * g_orbit.distance};
        const replay::editor::Vec3 rotation = replay::camera::LookAtWithScreenOffset(point, position, g_orbit.offset, g_freeCamFov,
            g_orbit.roll, MarkerScene().aspect);
        g_freeCamPosition = {position.x, position.y, position.z};
        g_freeCamRotation = {rotation.x, rotation.y, rotation.z};
        g_freeCamVelocity = {};
        PlaceCam(g_freeCam, g_freeCamCut, "orbit", position.x, position.y, position.z, rotation.x, rotation.y, rotation.z,
            g_freeCamFov, deltaSeconds, false);
        return true;
    }

    // ------------------------------------------------------------------
    // The free camera's flight state, its target and its actions.

    replay::log::LogLimit g_fcCollisionLogs;
    replay::log::LogLimit g_fcProbeLogs;
    replay::log::LogLimit g_fcTraceLogs;
    std::uint32_t g_fcTraceTick = 0;

    // Evidence: one FREECAM_TICK line every 20 ticks of the free camera (the
    // branch that placed it, the keys it saw, where it is).
    void TraceFreeCam(const char* branch, const replay::freecam::Input* input = nullptr)
    {
        if (++g_fcTraceTick % 20 != 0 || !g_fcTraceLogs.Take(600))
            return;
        LogFile() << "FREECAM_TICK,branch=" << branch << ",at=" << g_freeCamPosition.x << '/' << g_freeCamPosition.y << '/'
                  << g_freeCamPosition.z << ",rot=" << g_freeCamRotation.x << '/' << g_freeCamRotation.y << '/' << g_freeCamRotation.z
                  << ",fwd=" << (input ? input->moveForward : 0.0f) << ",right=" << (input ? input->moveRight : 0.0f)
                  << ",up=" << (input ? input->moveUp : 0.0f) << ",pad=" << (g_fcPadActive ? 1 : 0)
                  << ",follow=" << static_cast<int>(g_fcFollow) << ",target=" << g_fcTarget << ",lost=" << (g_fcTargetLost ? 1 : 0)
                  << ",locked=" << (g_fcLocked ? 1 : 0) << ",editor=" << (g_editorCoversScreen ? 1 : 0)
                  << ",track=" << (g_cameraTrackEnabled ? 1 : 0) << ",override=" << (g_cameraTrackManualOverride ? 1 : 0)
                  << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // The flight state follows the globals other code sets (Edit Camera, the
    // marker track, the orbit, the start): a view set from outside carries
    // no momentum.
    void SyncFreeCamState()
    {
        const replay::editor::Vec3 at{g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z};
        const replay::editor::Vec3 rotation{g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z};
        const auto same = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        if (same(at.x, g_fcState.position.x) && same(at.y, g_fcState.position.y) && same(at.z, g_fcState.position.z) &&
            same(rotation.x, g_fcState.rotation.x) && same(rotation.y, g_fcState.rotation.y) &&
            same(rotation.z, g_fcState.rotation.z) && same(g_freeCamFov, g_fcState.fov))
            return;
        g_fcState.SetView(at, rotation, g_freeCamFov);
        g_fcState.velocity = {g_freeCamVelocity.x, g_freeCamVelocity.y, g_freeCamVelocity.z};
    }

    // The flight state set to the free camera's view as it is now (a view set
    // from outside: Edit Camera's marker). SyncFreeCamState only resets when the
    // view differs; this resets always.
    void ResetFreeCamToView()
    {
        g_fcState.SetView({g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z},
            {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z}, g_freeCamFov);
        g_freeCamSmoothedLookX = g_freeCamSmoothedLookY = 0.0f;
    }

    void StoreFreeCamState()
    {
        g_freeCamPosition.x = g_fcState.position.x;
        g_freeCamPosition.y = g_fcState.position.y;
        g_freeCamPosition.z = g_fcState.position.z;
        g_freeCamRotation.x = g_fcState.rotation.x;
        g_freeCamRotation.y = g_fcState.rotation.y;
        g_freeCamRotation.z = g_fcState.rotation.z;
        g_freeCamFov = g_fcState.fov;
        g_freeCamVelocity.x = g_fcState.velocity.x;
        g_freeCamVelocity.y = g_fcState.velocity.y;
        g_freeCamVelocity.z = g_fcState.velocity.z;
        g_freeCamSmoothedLookX = g_fcState.smoothedMouseX;
        g_freeCamSmoothedLookY = g_fcState.smoothedMouseY;
    }

    // The target's recorded frame at the playhead (camera2's recorded root).
    bool FreeCamTargetFrame(std::uint32_t id, replay::freecam::TargetFrame& frame, replay::editor::Vec3* head = nullptr)
    {
        replay::camera::EntityState state{};
        if (!id || !MarkerEntity(id, g_transport.PlayheadMs(), state))
            return false;
        frame.position = state.position;
        frame.rotation = {state.pitch, state.roll, state.heading};
        if (head)
            *head = state.hasHead ? state.head : replay::editor::Vec3{state.position.x, state.position.y, state.position.z + 0.6f};
        return true;
    }

    // Follow: the camera carried by the target's move since the last tick
    // (a seek moves it with the target, keeping the framing).
    void FreeCamFollowTick()
    {
        if (g_fcFollow == replay::freecam::Follow::Off || !g_fcTarget)
        {
            g_fcTargetFrameValid = false;
            return;
        }
        replay::freecam::TargetFrame now{};
        if (!FreeCamTargetFrame(g_fcTarget, now))
        {
            g_fcTargetLost = true;
            g_fcTargetFrameValid = false;
            return;
        }
        g_fcTargetLost = false;
        if (g_fcTargetFrameValid)
        {
            SyncFreeCamState();
            const float roll = g_fcState.rotation.y;
            replay::freecam::ApplyFollow(g_fcState, g_fcFollow, g_fcTargetFrame, now);
            if (g_editingMarker) // Edit Camera stays level: a Rigid follow keeps the marker's roll
                replay::freecam::KeepRoll(g_fcState, roll);
            StoreFreeCamState();
        }
        g_fcTargetFrame = now;
        g_fcTargetFrameValid = true;
    }

    replay::freecam::Input FreeCamKeyInput(float rawLookX, float rawLookY)
    {
        const auto held = [](volatile LONG& flag) { return InterlockedCompareExchange(&flag, 0, 0) != 0; };
        replay::freecam::Input input;
        input.mouseX = rawLookX;
        input.mouseY = rawLookY;
        input.turnX = (held(g_turnRight) ? 1.0f : 0.0f) - (held(g_turnLeft) ? 1.0f : 0.0f);
        input.turnY = (held(g_turnUp) ? 1.0f : 0.0f) - (held(g_turnDown) ? 1.0f : 0.0f);
        input.roll = (held(g_rollRight) ? 1.0f : 0.0f) - (held(g_rollLeft) ? 1.0f : 0.0f);
        input.moveForward = (held(g_moveForward) ? 1.0f : 0.0f) - (held(g_moveBackward) ? 1.0f : 0.0f);
        input.moveRight = (held(g_moveRight) ? 1.0f : 0.0f) - (held(g_moveLeft) ? 1.0f : 0.0f);
        input.moveUp = (held(g_moveUp) ? 1.0f : 0.0f) - (held(g_moveDown) ? 1.0f : 0.0f);
        input.zoom = (held(g_fovIn) ? 1.0f : 0.0f) - (held(g_fovOut) ? 1.0f : 0.0f);
        input.fast = held(g_fastMove);
        input.slow = held(g_slowMove);
        return input;
    }

    // The pad (when it, not the keyboard, is in use): left stick moves, right stick
    // turns (a rate, not the mouse's delta), the triggers go up and down, the
    // bumpers roll, the D-pad zooms. [hypothesis: the RDR2 control names below
    // read under SET_PLAYER_CONTROL off, as INPUT_LOOK_* do]
    bool ReadFreeCamPad(replay::freecam::Input& input)
    {
        if (PAD::IS_USING_KEYBOARD_AND_MOUSE(0))
            return false;
        static const Hash moveLr = MISC::GET_HASH_KEY("INPUT_MOVE_LR");
        static const Hash moveUd = MISC::GET_HASH_KEY("INPUT_MOVE_UD");
        static const Hash rise = MISC::GET_HASH_KEY("INPUT_ATTACK");
        static const Hash sink = MISC::GET_HASH_KEY("INPUT_AIM");
        static const Hash rollLeft = MISC::GET_HASH_KEY("INPUT_FRONTEND_LB");
        static const Hash rollRight = MISC::GET_HASH_KEY("INPUT_FRONTEND_RB");
        static const Hash zoomIn = MISC::GET_HASH_KEY("INPUT_FRONTEND_UP");
        static const Hash zoomOut = MISC::GET_HASH_KEY("INPUT_FRONTEND_DOWN");
        for (const Hash control : {moveLr, moveUd, rise, sink, rollLeft, rollRight, zoomIn, zoomOut})
            PAD::DISABLE_CONTROL_ACTION(0, control, TRUE);
        const auto axis = [](Hash control) {
            const float value = PAD::GET_DISABLED_CONTROL_NORMAL(0, control);
            return std::fabs(value) > 0.12f ? value : 0.0f; // the stick's dead zone
        };
        // The right stick: the look axes are a position on a pad.
        input.turnX += input.mouseX;
        input.turnY -= input.mouseY;
        input.turnX = std::clamp(input.turnX, -1.0f, 1.0f);
        input.turnY = std::clamp(input.turnY, -1.0f, 1.0f);
        input.mouseX = input.mouseY = 0.0f;
        input.moveRight += axis(moveLr);
        input.moveForward -= axis(moveUd);
        input.moveUp += axis(rise) - axis(sink);
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, rollLeft))
            input.roll -= 1.0f;
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, rollRight))
            input.roll += 1.0f;
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, zoomIn))
            input.zoom += 1.0f;
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, zoomOut))
            input.zoom -= 1.0f;
        input.analog = true;
        return true;
    }

    // Collision and Walk: the engine's synchronous line probe (as in
    // replay_events.cpp and replay_bench.cpp: flags -1, result 2) and the ground
    // height.
    replay::freecam::World FreeCamWorld()
    {
        replay::freecam::World world;
        world.sweep = [](const replay::editor::Vec3& from, const replay::editor::Vec3& to, float radius,
                          replay::editor::Vec3& hit, replay::editor::Vec3& normal) {
            const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length < 0.0001f)
                return false;
            const float reach = (length + radius) / length;
            const ScrHandle probe = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(from.x, from.y, from.z,
                from.x + dx * reach, from.y + dy * reach, from.z + dz * reach, -1, PLAYER::PLAYER_PED_ID(), 7);
            BOOL didHit = FALSE;
            Vector3 end{};
            Vector3 surface{};
            Entity entity = 0;
            const int status = SHAPETEST::GET_SHAPE_TEST_RESULT(probe, &didHit, &end, &surface, &entity);
            if (g_fcProbeLogs.Take(5))
            {
                LogFile() << "FREECAM_PROBE,status=" << status << ",hit=" << (didHit ? 1 : 0) << ",normal=" << surface.x << '/'
                          << surface.y << '/' << surface.z << '\n';
                // Evidence (log only, once a session): which flags and options see what is
                // under the camera. Map collision is hit by -1, 1, 511; a script-made object by
                // none, 16 included.
                static bool diagnosed = false;
                if (!diagnosed)
                {
                diagnosed = true;
                for (const int flags : {-1, 1, 16, 511, 0x7FFFFFFF})
                    for (const int options : {7, 4, 0})
                    {
                        const ScrHandle down = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(from.x, from.y, from.z,
                            from.x, from.y, from.z - 12.0f, flags, PLAYER::PLAYER_PED_ID(), options);
                        BOOL downHit = FALSE;
                        Vector3 downEnd{}, downNormal{};
                        Entity downEntity = 0;
                        const int downStatus = SHAPETEST::GET_SHAPE_TEST_RESULT(down, &downHit, &downEnd, &downNormal, &downEntity);
                        LogFile() << "FREECAM_PROBE_DIAG,flags=" << flags << ",options=" << options << ",status=" << downStatus
                                  << ",hit=" << (downHit ? 1 : 0) << ",z=" << downEnd.z << ",from_z=" << from.z
                                  << ",entity=" << downEntity << ",type=" << (downEntity ? ENTITY::GET_ENTITY_TYPE(downEntity) : -1) << '\n';
                    }
                }
            }
            if (status != 2 || !didHit)
                return false;
            hit = {end.x, end.y, end.z};
            normal = {surface.x, surface.y, surface.z};
            return true;
        };
        world.ground = [](float x, float y, float z, float& groundZ) {
            return MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, z, &groundZ, FALSE) != FALSE;
        };
        return world;
    }

    // The clip's peds, horses and vehicles at the playhead (what the editor
    // lists as targets).
    std::vector<replay::freecam::Candidate> FreeCamCandidates()
    {
        std::vector<replay::freecam::Candidate> out;
        for (const replay::playback::Puppet& puppet : g_puppets.list)
        {
            const replay::entity::EntityKind kind = puppet.packet.kind;
            if (kind != replay::entity::EntityKind::Ped && kind != replay::entity::EntityKind::Horse &&
                kind != replay::entity::EntityKind::Vehicle)
                continue;
            replay::freecam::TargetFrame frame{};
            replay::editor::Vec3 head{};
            if (FreeCamTargetFrame(puppet.packet.id.Packed(), frame, &head))
                out.push_back({puppet.packet.id.Packed(), head});
        }
        return out;
    }

    void SetFreeCamTarget(std::uint32_t id, const char* how)
    {
        g_fcTarget = id;
        g_fcTargetFrameValid = false;
        g_fcTargetLost = false;
        if (!id)
            g_fcFollow = replay::freecam::Follow::Off;
        ShowObjective(id ? "Redemption Editor: free camera target set" : "Redemption Editor: free camera target cleared");
        LogFile() << "FREECAM_TARGET,how=" << how << ",target=" << id << ",main=" << (id && id == MainCharacterId() ? 1 : 0)
                  << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // Keys (OnKeyboardMessage) and the editor (FreeCamAction,
    // SetCameraOverlay) leave bits; taken here on the script thread.
    void ProcessFreeCamActions()
    {
        // A target belongs to its clip: another clip (or a new recording)
        // drops it and the follow.
        static std::uint32_t clipSeen = ~0u;
        if (clipSeen != g_clipLoadSequence)
        {
            clipSeen = g_clipLoadSequence;
            g_fcTarget = 0;
            g_fcFollow = replay::freecam::Follow::Off;
            g_fcTargetFrameValid = false;
            g_fcLocked = false;
        }
        const LONG bits = InterlockedExchange(&g_fcActionBits, 0);
        if (!bits)
            return;
        bool saveOptions = false;
        const auto toggle = [&](bool& value, const char* on, const char* off) {
            value = !value;
            ShowObjective(value ? on : off);
            saveOptions = true;
        };
        if (bits & kOverlayToggle)
        {
            g_camOverlayEnabled = !g_camOverlayEnabled;
            ShowObjective(g_camOverlayEnabled ? "Redemption Editor: camera markers in the world on" : "Redemption Editor: camera markers in the world off");
            SaveOverlayOptions();
        }
        const bool flying = g_freeCamEnabled && g_transport.Playing();
        if (bits & kFcDrone)
        {
            g_fcSettings.motion = g_fcSettings.motion == replay::freecam::Motion::Drone ? replay::freecam::Motion::Classic
                                                                                      : replay::freecam::Motion::Drone;
            g_fcState.Reset();
            ShowObjective(g_fcSettings.motion == replay::freecam::Motion::Drone ? "Redemption Editor: free camera Drone on"
                                                                               : "Redemption Editor: free camera Drone off");
            saveOptions = true;
        }
        if (bits & kFcAcrobatic)
        {
            toggle(g_fcSettings.acrobatic, "Redemption Editor: acrobatic rotation on", "Redemption Editor: acrobatic rotation off");
            if (!g_fcSettings.acrobatic)
                g_freeCamRotation.x = std::clamp(g_freeCamRotation.x, -89.0f, 89.0f);
        }
        if (bits & kFcWalk)
            toggle(g_fcSettings.walk, "Redemption Editor: free camera Walk on", "Redemption Editor: free camera Walk off");
        if (bits & kFcCollision)
            toggle(g_fcSettings.collision, "Redemption Editor: free camera collision on", "Redemption Editor: free camera collision off");
        if (bits & kFcLockAltitude)
            toggle(g_fcSettings.lockAltitude, "Redemption Editor: altitude locked", "Redemption Editor: altitude free");
        if (bits & kFcLock)
        {
            g_fcLocked = !g_fcLocked;
            ShowObjective(g_fcLocked ? "Redemption Editor: free camera held still" : "Redemption Editor: free camera moves");
        }
        if (bits & kFcClearTarget)
            SetFreeCamTarget(0, "editor");
        if (flying && (bits & (kFcPick | kFcPickNearest)))
        {
            const std::vector<replay::freecam::Candidate> candidates = FreeCamCandidates();
            const replay::editor::Vec3 at{g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z};
            const replay::editor::Vec3 rotation{g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z};
            const std::uint32_t picked = (bits & kFcPickNearest) ? replay::freecam::PickNearest(at, candidates)
                                                                 : replay::freecam::PickAimed(at, rotation, candidates);
            if (picked)
                SetFreeCamTarget(picked, (bits & kFcPickNearest) ? "nearest" : "aimed");
            else
                ShowObjective("Redemption Editor: no character or vehicle there");
        }
        if (bits & kFcFollow)
        {
            if (!g_fcTarget && flying)
            {
                const std::vector<replay::freecam::Candidate> candidates = FreeCamCandidates();
                const std::uint32_t picked = replay::freecam::PickAimed({g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z},
                    {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z}, candidates);
                if (picked)
                    SetFreeCamTarget(picked, "follow");
            }
            if (!g_fcTarget)
                ShowObjective(replay::keys::Fill("Redemption Editor: aim at a character or vehicle to follow it ({key_pick} picks one)",
                    replay::keys::Current()).c_str());
            else
            {
                g_fcFollow = static_cast<replay::freecam::Follow>((static_cast<int>(g_fcFollow) + 1) % replay::freecam::kFollowCount);
                g_fcTargetFrameValid = false;
                ShowObjective(g_fcFollow == replay::freecam::Follow::Rigid ? "Redemption Editor: follow the target (rigid)"
                        : g_fcFollow == replay::freecam::Follow::Translate ? "Redemption Editor: follow the target (position only)"
                                                                           : "Redemption Editor: follow off");
            }
        }
        if (flying && (bits & kFcLevel))
        {
            SyncFreeCamState();
            replay::freecam::LevelHorizon(g_fcState);
            StoreFreeCamState();
            g_cameraTrackManualOverride = true;
        }
        if (flying && (bits & kFcBehind))
        {
            replay::freecam::TargetFrame frame{};
            replay::editor::Vec3 head{};
            const std::uint32_t who = g_fcTarget ? g_fcTarget : MainCharacterId();
            if (FreeCamTargetFrame(who, frame, &head))
            {
                SyncFreeCamState();
                replay::freecam::ShotBehind(g_fcState, frame, &head);
                StoreFreeCamState();
                g_cameraTrackManualOverride = true;
                g_freeCamCut = {}; // a cut, not a move
            }
        }
        if (saveOptions)
            SaveFreeCamOptions();
        LogFreeCamConfig("action");
    }

    // ------------------------------------------------------------------
    // The camera markers in the world. The path is built when the markers change
    // (camera_overlay.h BuildPath, up to 2400 evaluations), projected every tick
    // with the view placed this tick and written to the overlay channel; the
    // add-on draws it after the export's capture (never in a video).
    replay::overlaychannel::Channel* g_overlayChannel = nullptr;
    HANDLE g_overlayMapping = nullptr;
    bool g_overlayChannelFailed = false;
    replay::camera::Track g_overlayTrack;
    std::uint32_t g_overlayClipSequence = ~0u;
    bool g_overlayPathBuilt = false;
    replay::camera::overlay::WorldPath g_overlayPath;
    std::vector<replay::camera::overlay::TrailPoint> g_overlayTrailPoints;
    std::uint64_t g_overlayTrailAt = ~0ull;
    std::uint32_t g_overlayTrailTarget = 0;
    std::uint32_t g_overlayCheckedClip = ~0u;
    std::uint32_t g_overlayShownFrames = 0;
    replay::log::LogLimit g_overlayLogs;

    replay::overlaychannel::Channel* OverlayChannel()
    {
        if (g_overlayChannel || g_overlayChannelFailed)
            return g_overlayChannel;
        namespace oc = replay::overlaychannel;
        wchar_t name[64];
        oc::Name(name, GetCurrentProcessId());
        const std::uint64_t size = sizeof(oc::Channel);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
            static_cast<DWORD>(size & 0xFFFFFFFFu), name);
        void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(oc::Channel)) : nullptr;
        if (!view)
        {
            if (mapping)
                CloseHandle(mapping);
            g_overlayChannelFailed = true;
            LogFile() << "CAMERA_OVERLAY,phase=channel,ok=0,error=" << GetLastError() << '\n';
            return nullptr;
        }
        g_overlayMapping = mapping;
        // A hot reload finds the old mapping: made again (the add-on reads
        // `bytes` and the published frame each present).
        g_overlayChannel = new (view) oc::Channel();
        g_overlayChannel->bytes = static_cast<std::uint32_t>(sizeof(oc::Channel));
        LogFile() << "CAMERA_OVERLAY,phase=channel,ok=1,bytes=" << sizeof(oc::Channel) << '\n';
        return g_overlayChannel;
    }

    void UpdateCameraOverlay()
    {
        REPLAY_PROFILE_ZONE("tick.camoverlay");
        namespace oc = replay::overlaychannel;
        namespace ov = replay::camera::overlay;
        oc::Channel* channel = OverlayChannel();
        if (!channel)
            return;
        oc::Frame& frame = oc::BeginWrite(*channel);
        const bool playing = g_transport.Playing();
        const bool editing = g_editingMarker != 0;
        const bool still = g_transport.Paused() || !replay::playback::NormalPlay();
        const bool show = g_camOverlayEnabled && playing && g_lastPlacedView.valid && !replay::exportprobe::HidesHud() &&
            !ClipLoadWaiting() && (still || editing || g_freeCamEnabled || g_camOverlayWhilePlaying);
        if (!show)
        {
            frame.flags = oc::FrameHidden;
            oc::EndWrite(*channel, frame);
            return;
        }
        const replay::camera::Scene scene = MarkerScene();
        // The path drawn is the one that plays: the Spline defaults and the camera
        // weight (which applies on play) included.
        static replay::editor::SplineTuning builtDefaults = replay::editor::DefaultSplineTuning();
        if (!g_overlayPathBuilt || g_overlayClipSequence != g_clipLoadSequence || !replay::camera::SameMarkers(g_overlayTrack, g_cameraTrack) ||
            !replay::camera::spline::Same(builtDefaults, g_splineDefaults))
        {
            const std::uint64_t t0 = replay::prof::Now();
            g_overlayTrack = g_cameraTrack;
            g_overlayClipSequence = g_clipLoadSequence;
            builtDefaults = g_splineDefaults;
            replay::camera::Scene withWeight = scene;
            withWeight.splineWeight = true;
            g_overlayPath = ov::BuildPath(g_cameraTrack, withWeight);
            g_overlayPathBuilt = true;
            const double ms = static_cast<double>(replay::prof::Now() - t0) * 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
            if (g_overlayLogs.Take(100))
                LogFile() << "CAMERA_OVERLAY,phase=path,points=" << g_overlayPath.points.size() << ",markers=" << g_overlayPath.markers.size()
                          << ",evaluations=" << g_overlayPath.evaluations << ",ms=" << ms << '\n';
        }
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        // The free camera's target: its recorded path around the playhead.
        const std::uint32_t trailTarget = g_camOverlayTrail ? g_fcTarget : 0u;
        if (trailTarget != g_overlayTrailTarget || g_overlayTrailAt == ~0ull ||
            (playheadMs > g_overlayTrailAt ? playheadMs - g_overlayTrailAt : g_overlayTrailAt - playheadMs) > 250)
        {
            g_overlayTrailTarget = trailTarget;
            g_overlayTrailAt = playheadMs;
            g_overlayTrailPoints = trailTarget ? ov::BuildTrail(scene.lookup, trailTarget, playheadMs > 4000 ? playheadMs - 4000 : 0,
                                                     std::min<std::uint64_t>(playheadMs + 8000, PlaybackDurationMs()), 100)
                                               : std::vector<ov::TrailPoint>{};
        }
        ov::ProjectSettings settings;
        settings.playheadMs = playheadMs;
        settings.editingId = g_editingMarker;
        const replay::camera::SegmentPlan plan = replay::camera::Plan(g_cameraTrack, playheadMs);
        if (plan.valid && plan.current >= 0)
            settings.currentId = g_cameraTrack.Keyframes()[plan.current].id;
        settings.labelEveryMs = g_camOverlayTimeLabels ? 1000u : 0u;
        settings.dim = !still && !editing && !g_freeCamEnabled;
        if (g_freeCamEnabled && !editing && g_cameraTrackEnabled)
        {
            replay::editor::CameraState marker{};
            if (replay::camera::EvaluateMarkersRaw(g_cameraTrack, playheadMs, scene, marker))
            {
                settings.showCamera = true;
                settings.cameraAt = marker.position;
            }
        }
        if (g_fcTarget)
        {
            replay::freecam::TargetFrame frameNow{};
            replay::editor::Vec3 head{};
            if (FreeCamTargetFrame(g_fcTarget, frameNow, &head))
            {
                settings.targetId = g_fcTarget;
                settings.targetValid = true;
                settings.targetAt = {head.x, head.y, head.z + 0.35f};
            }
        }
        const ov::ProjectReport report = ov::Project(g_overlayPath, g_overlayTrailPoints, g_lastPlacedView.view, settings, frame);
        oc::EndWrite(*channel, frame);
        if (report.dropped && g_overlayLogs.Take(100))
            LogFile() << "CAMERA_OVERLAY,phase=dropped,lines=" << report.lines << ",dots=" << report.dots << ",labels=" << report.labels
                      << ",dropped=" << report.dropped << '\n';
        // Evidence, once per clip: our projection of a marker against the
        // engine's (GET_SCREEN_COORD_FROM_WORLD_COORD, 0..1, y down). The
        // overlay lines up with the picture only when they agree.
        if (g_overlayCheckedClip != g_clipLoadSequence && ++g_overlayShownFrames > 30)
        {
            for (const ov::MarkerPoint& m : g_overlayPath.markers)
            {
                const replay::editor::Vec3 local = ov::ToCamera(g_lastPlacedView.view, m.at);
                if (local.y < 1.0f)
                    continue;
                float sx = 0.0f, sy = 0.0f;
                if (!GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(m.at.x, m.at.y, m.at.z, &sx, &sy))
                    continue;
                replay::editor::Vec2 a{}, b{};
                if (!ov::ProjectSegment(g_lastPlacedView.view, m.at, m.at, a, b))
                    continue;
                int width = 0, height = 0;
                GRAPHICS::GET_SCREEN_RESOLUTION(&width, &height);
                const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f;
                const float oursX = 0.5f + a.x / aspect * 0.5f, oursY = 0.5f - a.y * 0.5f;
                g_overlayCheckedClip = g_clipLoadSequence;
                LogFile() << "CAMERA_OVERLAY,phase=projection_check,marker=" << m.id << ",ours=" << oursX << '/' << oursY
                          << ",engine=" << sx << '/' << sy << ",err_x=" << (oursX - sx) << ",err_y=" << (oursY - sy)
                          << ",aspect=" << aspect << ",fov=" << g_lastPlacedView.view.fov << ",meters=" << local.y << '\n';
                break;
            }
        }
    }

    void UpdateFreeCamView(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.freecam");
        if (!g_freeCamEnabled || !g_freeCam || !CAM::DOES_CAM_EXIST(g_freeCam))
        {
            g_freeCamCut = {}; // the next freecam view starts with a cut
            return;
        }

        CAM::SET_WIDESCREEN_BORDERS(FALSE, 0); // no cinematic letterbox on the freecam
        // Esc cancels Edit Camera (the editor's overlay), it does not open the game's
        // pause menu over it.
        if (g_editingMarker)
        {
            PAD::DISABLE_CONTROL_ACTION(2, MISC::GET_HASH_KEY("INPUT_FRONTEND_PAUSE"), TRUE);
            PAD::DISABLE_CONTROL_ACTION(2, MISC::GET_HASH_KEY("INPUT_FRONTEND_PAUSE_ALTERNATE"), TRUE);
        }
        float deltaSeconds = static_cast<float>(now - g_freeCamLastUpdateAt) / 1000.0f;
        if (deltaSeconds > 0.05f)
            deltaSeconds = 0.05f;
        g_freeCamLastUpdateAt = now;

        // Read the look axes even after disabling them, so the scripted camera
        // owns mouse-look without also moving RDR2's gameplay camera.
        PAD::DISABLE_CONTROL_ACTION(0, kInputLookLeftRight, TRUE);
        PAD::DISABLE_CONTROL_ACTION(0, kInputLookUpDown, TRUE);
        const float rawLookX =
            PAD::GET_DISABLED_CONTROL_NORMAL(0, kInputLookLeftRight);
        const float rawLookY =
            PAD::GET_DISABLED_CONTROL_NORMAL(0, kInputLookUpDown);

        const bool cameraInput =
            std::fabs(rawLookX) > 0.0001f ||
            std::fabs(rawLookY) > 0.0001f ||
            InterlockedCompareExchange(&g_turnLeft, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_turnRight, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_turnUp, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_turnDown, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_rollLeft, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_rollRight, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveForward, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveBackward, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveLeft, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveRight, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveUp, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveDown, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_fovIn, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_fovOut, 0, 0) != 0;
        // With the editor's UI up the keyboard is the editor's (Ctrl+Z must not roll
        // the camera, typing a clip name must not move it); H hides the UI to fly,
        // Edit Camera hands the keys over.
        const bool keysForEditor = g_editorCoversScreen && !g_editingMarker;
        if (keysForEditor)
        {
            g_freeCamVelocity = {};
            FreeCamFollowTick(); // a followed target still carries the camera
            TraceFreeCam("keys_for_editor");
            PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, false);
            return;
        }
        if (g_editResolvePending && g_editingMarker)
        {
            // Edit Camera: the marker's live view, now that the playhead
            // (and the puppets) stand at its time; until then the camera
            // holds still.
            const replay::camera::Keyframe* key = g_cameraTrack.Find(g_editingMarker);
            if (key && replay::playback::Preparing())
                return;
            g_editResolvePending = false;
            if (key && g_transport.PlayheadMs() == std::min<std::uint64_t>(key->timeMs, PlaybackDurationMs()))
            {
                const replay::camera::LivePose live = EditStartPose(*key);
                const replay::editor::Vec3 rotation = replay::camera::Track::ToEulerDegrees(live.pose.orientation);
                g_freeCamPosition = {live.pose.position.x, live.pose.position.y, live.pose.position.z};
                g_freeCamRotation = {rotation.x, rotation.y, rotation.z};
                g_freeCamFov = live.pose.fov;
                g_freeCamVelocity = {};
                ResetFreeCamToView(); // no roll, banking or turn rate left from before
                g_orbit.marker = 0; // the orbit starts again from this view
                PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                    g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, true);
                return;
            }
        }
        else
            g_editResolvePending = false;
        if (cameraInput)
            g_cameraTrackManualOverride = true;
        if (UpdateOrbitEdit(deltaSeconds, rawLookX, rawLookY))
        {
            TraceFreeCam("orbit");
            return;
        }

        if (g_cameraTrackEnabled && !g_cameraTrackManualOverride)
        {
            replay::editor::CameraState evaluated{};
            replay::camera::SegmentPlan freePlan;
            if (EvaluateCameraTrack(g_transport.PlayheadMs(), evaluated, 0.0f, &freePlan))
            {
                TraceFreeCam("track");
                // The same marker boundary rule as the replay camera (this camera renders
                // while the freecam is on).
                static replay::camcut::MarkerFrame lastFreeMarker;
                const replay::camcut::MarkerFrame freeMarker = MarkerFrameOf(true, freePlan);
                const bool markerCut = replay::camcut::MarkerBoundaryCut(lastFreeMarker, freeMarker);
                lastFreeMarker = freeMarker;
                g_freeCamVelocity = {};
                g_freeCamSmoothedLookX = 0.0f;
                g_freeCamSmoothedLookY = 0.0f;
                g_freeCamPosition = {evaluated.position.x, evaluated.position.y, evaluated.position.z};
                g_freeCamRotation = {evaluated.rotation.x, evaluated.rotation.y, evaluated.rotation.z};
                g_freeCamFov = evaluated.fov;
                PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                    g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds,
                    markerCut || !replay::playback::NormalPlay());
                return;
            }
        }

        // The flight itself (free_cam.h). Classic is the base flight
        // (tests/camera_cine_tests.cpp replays it by hand); Drone, acrobatic, Walk,
        // collision and the locked altitude are the options. The pad moves it too
        // (sticks, triggers, bumpers). The wheel scales the speed (x0.25 to x4), Ctrl
        // moves slow (a quarter).
        {
            static const Hash scrollUp = MISC::GET_HASH_KEY("INPUT_CURSOR_SCROLL_UP");
            static const Hash scrollDown = MISC::GET_HASH_KEY("INPUT_CURSOR_SCROLL_DOWN");
            PAD::DISABLE_CONTROL_ACTION(0, scrollUp, TRUE);
            PAD::DISABLE_CONTROL_ACTION(0, scrollDown, TRUE);
            if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, scrollUp))
                g_freeCamSpeedScale = std::min(4.0f, g_freeCamSpeedScale * 1.25f);
            if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, scrollDown))
                g_freeCamSpeedScale = std::max(0.25f, g_freeCamSpeedScale / 1.25f);
        }
        SyncFreeCamState();
        FreeCamFollowTick();
        if (g_fcLocked)
        {
            TraceFreeCam("locked");
            g_fcState.velocity = {};
            StoreFreeCamState();
            PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, false);
            return;
        }
        replay::freecam::Input input = FreeCamKeyInput(rawLookX, rawLookY);
        const bool pad = ReadFreeCamPad(input);
        g_fcPadActive = pad || (g_fcPadActive && !cameraInput);
        g_fcSettings.speedScale = g_freeCamSpeedScale;
        const replay::freecam::World world = FreeCamWorld();
        // Edit Camera flies level (free_cam.h LevelFlight): the acrobatic rotation
        // and the Drone banking roll the horizon, and a marker's roll is its own.
        const replay::freecam::Settings flight = g_editingMarker ? replay::freecam::LevelFlight(g_fcSettings) : g_fcSettings;
        const replay::freecam::StepReport step = replay::freecam::Step(g_fcState, input, flight, deltaSeconds, &world);
        if (step.collided && g_fcCollisionLogs.Take(20))
            LogFile() << "FREECAM_COLLISION,at=" << g_fcState.position.x << '/' << g_fcState.position.y << '/' << g_fcState.position.z
                      << ",speed=" << step.speed << '\n';
        StoreFreeCamState();
        TraceFreeCam("flight", &input);

        PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
            g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, false);
    }

    // The free camera's near clip and DOF, so what the editor shows while a marker
    // is framed (Edit Camera) or the free camera flies is what the clip will play
    // (the replay camera, ApplyMarkerDof, does not render while the free camera is
    // on: UpdateReplayCamera returns at g_freeCamEnabled).
    //  - Edit Camera: the edited marker's near clip and DOF;
    //  - the free camera playing the markers: the segment's (as the replay camera);
    //  - the free camera flying: Free Camera > Near Clip, no DOF.
    void ApplyFreeCamLens()
    {
        if (!g_freeCamEnabled || !g_freeCam || !CAM::DOES_CAM_EXIST(g_freeCam))
            return;
        g_cameraLive = {};
        const replay::editor::CameraState state{{g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z},
            {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z}, g_freeCamFov};
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        const replay::camera::Keyframe* editing = g_editingMarker ? g_cameraTrack.Find(g_editingMarker) : nullptr;
        replay::camera::FocusPlan focus;
        float nearClip = g_fcNearClip;
        std::uint32_t markerId = 0;
        if (editing)
        {
            g_cameraLive.flags |= replay::editor::CameraLiveEditCamera;
            nearClip = replay::camera::MarkerNearClip(*editing);
            focus = replay::camera::PlanFocus(editing, state, MarkerScene(), editing->timeMs);
            markerId = editing->id;
        }
        else if (g_cameraTrackEnabled && !g_cameraTrackManualOverride && !g_cameraTrack.Empty())
        {
            const replay::camera::SegmentPlan plan = replay::camera::Plan(g_cameraTrack, playheadMs);
            nearClip = replay::camera::NearClipAt(g_cameraTrack, plan);
            focus = replay::camera::BlendFocus(g_cameraTrack, plan, state, MarkerScene(), playheadMs, nullptr, g_camFocusConfig).plan;
            const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
            if (plan.valid && plan.dofIndex >= 0 && plan.dofIndex < static_cast<int>(keys.size()))
                markerId = keys[plan.dofIndex].id;
        }
        ApplyNearClip(g_freeCam, g_freeLens, nearClip, editing ? "edit" : "free");
        LensState& lens = g_freeLens;
        if (focus.source == replay::camera::FocusSource::Default)
        {
            lens.focusSent = -1.0f;
            if (lens.dof)
            {
                CAM::_0x1FC6C727D30FFDDE(g_freeCam); // the engine's DOF clear
                lens.dof = false;
                g_lensChangedAt = GetTickCount64(); // back to Default, shown through the freeze
            }
            if (lens.engineFocus)
            {
                if (lens.pausedBefore >= 0)
                    CAM::_PAUSE_CAMERA_FOCUS(g_freeCam, lens.pausedBefore ? TRUE : FALSE);
                lens.engineFocus = false;
            }
            return;
        }
        const float lensMm = SendDofBlock(g_freeCam, focus, state.fov);
        lens.dof = true;
        float sent = 0.0f;
        if (focus.source == replay::camera::FocusSource::Engine)
        {
            if (!lens.engineFocus)
            {
                std::uint16_t word = 0;
                lens.pausedBefore = ReadCamFlags(g_freeCam, word) ? ((word & kCamFocusPausedBit) ? 1 : 0) : -1;
                CAM::_PAUSE_CAMERA_FOCUS(g_freeCam, FALSE);
                lens.engineFocus = true;
            }
            lens.focusSent = -1.0f;
        }
        else
        {
            if (lens.engineFocus)
            {
                if (lens.pausedBefore >= 0)
                    CAM::_PAUSE_CAMERA_FOCUS(g_freeCam, lens.pausedBefore ? TRUE : FALSE);
                lens.engineFocus = false;
            }
            if (focus.source != replay::camera::FocusSource::Sharp)
            {
                sent = focus.distance;
                SendFocusDistance(g_freeCam, lens, sent, editing ? "edit" : "free");
            }
            else
                lens.focusSent = -1.0f;
        }
        CAM::ALLOW_MOTION_BLUR_DECAY(g_freeCam, FALSE);
        NoteDofReadback(g_freeCam, editing ? "edit" : "free", focus, lensMm, sent, markerId);
    }

    void UpdateFreeCam(ULONGLONG now)
    {
        UpdateFreeCamView(now);
        ApplyFreeCamLens();
    }


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

    // The paused replay stands still: the world (fire, particles, water, cloth,
    // wind, the ambient sound) freezes with the photo mode's freeze
    // (_REQUEST_PHOTO_MODE_FREEZE), which the time scale (at 1 while paused) never
    // does. It also holds the entities, so a moving playhead (scrub, frame step, a
    // marker jump) thaws it until the playhead has stood still for kFreezeSettleMs
    // and the puppets took the new pose. A file marks the freeze on disk: a script
    // reload in it finds the file and thaws the game (natives cannot run at unload).
    constexpr ULONGLONG kFreezeSettleMs = 250;
    const char* kWorldFreezeFile = "RedemptionEditor.world-freeze";
    // Written by an unload that could not see every detour or worker leave this
    // image; reported by the next load.
    const char* kUnloadReportFile = "RedemptionEditor.unload-report";
    struct WorldFreeze
    {
        bool active = false;
        std::uint64_t lastPlayheadMs = ~0ull;
        replay::clipload::Vec3 lastCamera{};
        replay::clipload::Vec3 lastCameraRotation{};
        ULONGLONG thawUntil = 0;
        std::uint32_t freezes = 0;
    };
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
        if (FILE* file = _wfopen(StateFile(kWorldFreezeFile).c_str(), L"w"))
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
    // SEH, each prologue and every field site checked first (a mismatch disables
    // the model for the session: no clear, no reload).

    bool WorldCodeBytes(std::uintptr_t base, std::size_t imageSize, std::uint32_t rva, std::uint8_t* out, std::size_t size)
    {
        if (!base || rva + size > imageSize)
            return false;
        return replay::mem::Copy(out, base + rva, size);
    }

    bool WorldModelChecks(std::string& error)
    {
        using namespace replay::worldmodel;
        if (g_world.checked)
        {
            error = g_world.checkError;
            return g_world.checksOk;
        }
        std::uintptr_t base = 0;
        std::size_t imageSize = 0;
        ImageRange(base, imageSize);
        const bool slotPath = g_recConfig.worldReloadPath == ReloadPath::Slot;
        const Signature* queueSignatures[] = {&kPushSignature, &kProcessSignature, &kQueueUpdateSignature, &kNewestSlotSignature};
        const Signature* slotSignatures[] = {&kLoadFromSlotSignature, &kNewestSlotSignature};
        const Site* queueSites[] = {&kHeaderSite, &kSlotDateSite, &kQueueCountSite, &kOpVtableSite};
        const Site* slotSites[] = {&kHeaderSite, &kSlotDateSite, &kMenuBusySite};
        std::string failed;
        const std::size_t signatureCount = slotPath ? std::size(slotSignatures) : std::size(queueSignatures);
        for (std::size_t index = 0; index < signatureCount && failed.empty(); ++index)
        {
            const Signature& signature = *(slotPath ? slotSignatures[index] : queueSignatures[index]);
            std::uint8_t code[sizeof(signature.bytes)]{};
            if (!WorldCodeBytes(base, imageSize, signature.rva, code, sizeof(code)) || !SignatureMatches(signature, code, sizeof(code)))
                failed = signature.name;
        }
        const std::size_t siteCount = slotPath ? std::size(slotSites) : std::size(queueSites);
        for (std::size_t index = 0; index < siteCount && failed.empty(); ++index)
        {
            const Site& site = *(slotPath ? slotSites[index] : queueSites[index]);
            std::uint8_t code[sizeof(site.bytes)]{};
            if (SiteTarget(site) != site.target || !WorldCodeBytes(base, imageSize, site.rva, code, sizeof(code)) ||
                !SiteMatches(site, code, sizeof(code)))
                failed = site.name;
        }
        OpView op;
        bool opRead = false;
        if (failed.empty() && !slotPath)
        {
            std::uint8_t bytes[kLoadOpSize]{};
            opRead = WorldCodeBytes(base, imageSize, kLoadOpRva, bytes, sizeof(bytes));
            op = ParseOp(bytes);
            if (!opRead)
                failed = "load_op_read";
            else
            {
                const OpCheck check = CheckOp(op, base);
                if (check == OpCheck::WrongVtable || check == OpCheck::WrongType)
                    failed = std::string("load_op_") + OpCheckName(check);
            }
        }
        g_world.checked = true;
        g_world.checksOk = failed.empty();
        g_world.checkError = failed.empty() ? "none" : failed;
        error = g_world.checkError;
        LogFile() << "WORLD_MODEL,phase=checks,path=" << ReloadPathName(g_recConfig.worldReloadPath)
                  << ",ok=" << (g_world.checksOk ? 1 : 0) << ",failed=" << g_world.checkError
                  << ",op_status=" << (opRead ? op.status : -1) << ",op_state=" << (opRead ? op.state : -1) << '\n';
        return g_world.checksOk;
    }

    // The engine's slot table as its newest-slot sort reads it.
    bool ReadWorldSlots(replay::worldmodel::SlotSummary& summary)
    {
        using namespace replay::worldmodel;
        std::uintptr_t base = 0;
        std::size_t imageSize = 0;
        ImageRange(base, imageSize);
        // The two field sites prove the table addresses in this build.
        for (const Site* site : {&kHeaderSite, &kSlotDateSite})
        {
            std::uint8_t code[sizeof(site->bytes)]{};
            if (!WorldCodeBytes(base, imageSize, site->rva, code, sizeof(code)) || !SiteMatches(*site, code, sizeof(code)))
                return false;
        }
        Slot slots[kSlotCount];
        for (std::uint32_t index = 0; index < kSlotCount; ++index)
        {
            std::uint8_t header[sizeof(kHeaderMagic)]{};
            std::int32_t date[6]{};
            if (!WorldCodeBytes(base, imageSize, kHeaderRva + index * kHeaderStride, header, sizeof(header)) ||
                !WorldCodeBytes(base, imageSize, kSlotTableRva + index * kSlotStride + kSlotDateOffset,
                    reinterpret_cast<std::uint8_t*>(date), sizeof(date)))
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

    // ---- Closed theater ----
    struct TheatreSession
    {
        bool owning = false;          // the replay owns the world (clear to reload)
        bool scriptsPaused = false;
        bool perscharHeld = false;    // we held the persistent characters (0x92690B0822493CE0)
        std::uint64_t pauseCalls = 0;
        // The other ASIs' threads given back after each pause (world_theatre.h), and
        // the last check's error.
        std::uint64_t scriptHookUnpaused = 0, scriptHookWriteFailed = 0;
        std::uint32_t scriptHookThreads = 0;
        std::string scriptHookError = "none";
        std::uint64_t spawnCursor = 0, sceneCursor = 0, refusalCursor = 0;
        std::uint64_t spawns = 0, spawnsDenied = 0, scenes = 0, refusals = 0;
        std::uint32_t spawnLines = 0, sceneLines = 0, refusalLines = 0;
        std::uint64_t coreCursor = 0, worldSpeech = 0, worldSpeechPlayer = 0;
        std::uint32_t worldSpeechLines = 0;
        bool coreCursorSet = false;
        std::map<std::string, std::uint32_t> spawnBySource;
        std::map<std::uint32_t, std::uint32_t> sceneByHash;
        std::map<std::uint32_t, std::uint32_t> refusalByCode;
    };
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
#if defined(REDEMPTION_DEV) // the bench's fault commands (replay_bench.h): developer build only
    bool g_benchThrowNextTick = false; // bench "safe_exit exception"
    bool g_benchHoldNormalExit = false; // bench "safe_exit hold_exit": the normal exit is skipped
#endif
    std::string g_tickExceptionWhat;
    std::uint32_t g_tickExceptions = 0;

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
        if (!g_recConfig.theatre.log)
            return;
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

    void TheatreWorldSpeech(); // below

    // While the replay owns the world: the script threads paused every tick
    // (a thread launched since sleeps from its first tick on), and the
    // evidence rings drained into the log.
    void UpdateTheatre()
    {
        using namespace replay::theatre;
        if (!g_theatre.owning)
            return;
        if (g_recConfig.theatre.pauseScripts)
        {
            ANIMSCENE::_PAUSE_SCRIPT_THREADS(TRUE);
            const bool first = g_theatre.pauseCalls++ == 0;
            if (first)
                LogFile() << "THEATRE,phase=pause_scripts,first=1\n";
            g_theatre.scriptsPaused = true;
            // The native pauses every thread but ours, and ScriptHookRDR2 runs each ASI in
            // a thread of its own (another mod, e.g. a camera mod, would stop); those
            // threads run again in this same tick (world_theatre.h;
            // TheatrePauseScriptHook=1 leaves them paused).
            static std::vector<replay::theatre::ThreadView> census;
            census.clear();
            const replay::theatre::ThreadSpare spare = replay::theatre::SpareScriptHookThreads(
                g_recConfig.theatre.pauseScriptHook, first ? &census : nullptr);
            g_theatre.scriptHookUnpaused += spare.unpaused;
            g_theatre.scriptHookWriteFailed += spare.writeFailed;
            g_theatre.scriptHookThreads = spare.scriptHook;
            g_theatre.scriptHookError = spare.error;
            if (first)
            {
                std::ofstream& log = LogFile();
                log << "THEATRE_THREADS_SUMMARY,ok=" << (spare.ok ? 1 : 0) << ",error=" << spare.error
                    << ",pause_scripthook=" << (g_recConfig.theatre.pauseScriptHook ? 1 : 0) << ",threads=" << spare.threads
                    << ",paused_by_native=" << spare.pausedByNative << ",scripthook_threads=" << spare.scriptHook
                    << ",unpaused=" << spare.unpaused << ",write_failed=" << spare.writeFailed << ",list=";
                for (const replay::theatre::ThreadView& thread : census)
                    log << thread.id << ":0x" << std::hex << thread.scriptHash << std::dec << ":p" << static_cast<int>(thread.paused)
                        << (thread.scriptHash == replay::theatre::kScriptHookThreadHash ? ":sh" : "") << '|';
                log << '\n';
            }
        }
        if (!g_recConfig.theatre.log)
            return;
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
            << ",scripthook_threads=" << g_theatre.scriptHookThreads << ",scripthook_unpaused=" << g_theatre.scriptHookUnpaused
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
        state.step = g_recConfig.worldModelStep;
        if (state.step < kStepClear)
            return; // WorldModelStep=1: the pool only (the config line says so)
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
        std::snprintf(flags, sizeof(flags), "0x%X", g_recConfig.worldClearFlags);
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
        if (g_recConfig.worldScriptsCleanup)
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
        if (g_recConfig.worldScriptsCleanup)
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
        MISC::CLEAR_AREA(0.0f, 0.0f, 0.0f, kClearRadius, static_cast<int>(g_recConfig.worldClearFlags));
        const bool trains = !playerInVehicle && (g_recConfig.worldClearFlags & kClearVehicles) != 0;
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
                  << ",scripts_cleanup=" << (g_recConfig.worldScriptsCleanup ? 1 : 0) << ",main_before=" << mainBefore
                  << ",main_after=" << mainAfter << ",wait_ms=" << waitedMs << ",trains=" << (trains ? 1 : 0)
                  << ",player_in_vehicle=" << (playerInVehicle ? 1 : 0) << ",player_kept=" << (playerAlive ? 1 : 0)
                  << ",slot=" << slots.newest << ",slot_date=" << WorldSlotDate(slots) << ",slots_valid=" << slots.valid
                  << ",before" << before << ",after" << WorldPoolCounts() << '\n';
        LogFile().flush();
        replay::hud::ShowNotice(g_recConfig.worldModelStep >= kStepReload
                ? TR("hud.world_cleared", "Redemption Editor: the world is cleared for the replay; leaving the replay loads your last save.")
                : "Redemption Editor: the world is cleared for the replay (WorldModelStep=2); load your last save from the menu after it.");
    }

    void ProjectBeforePlaybackStop();

    void StopPlayback()
    {
        // A stop drops a held or batching start.
        if (g_clipStart.Holding() || g_clipStart.Batching())
            LogFile() << "CLIP_START_STAGGER,phase=cancelled,gate=" << replay::clipstart::PhaseText(g_clipStart.CurrentPhase()) << '\n';
        g_clipStart.Reset();
        replay::playback::BeginStartBatchTick(0);
        // A project's clip keeps its markers in the project.
        ProjectBeforePlaybackStop();
        // Leaving a clip keeps its marker changes (saved, never lost).
        if (g_clipEdited && g_transport.Playing())
            SaveEditedClip("leave");
        g_bufferResumeAt = GetTickCount64() + 1500;
        StopOwnScreenEffects("playback_stop"); // also runs at every playback start
        g_trimReady = false;
        g_editingMarker = 0; // leaving the clip cancels Edit Camera
        if (g_transport.Playing())
        {
            std::ofstream& log = LogFile();
            replay::prof::WriteSessionReport(log, "play");
            log.flush();
        }
        StopFreeCam();
        StopReplayCamera();
        ReleaseWorldFocus("playback_stop");
        replay::gamelistener::Release("playback_stop", LogFile());
        ThawWorld("playback_stop");
        EndClipWorld();
        replay::playback::Stop();
        MusicExit("playback_stop");
        replay::doors::EndPlayback(LogFile());
        g_transport.Stop();
        replay::pose::capture::ReleaseFinalizer(); // the Pose Editor may take it now
        if (g_worldTimeScaleApplied != 1.0f)
        {
            MISC::SET_TIME_SCALE(1.0f);
            g_worldTimeScaleApplied = 1.0f;
        }
        g_playbackSpeedIndex = kNormalSpeedIndex; // each playback starts at 100%
        g_cameraTrackManualOverride = false;

        std::ofstream& log = LogFile();
        log << "PLAYBACK_STOP"
            << ",actor_backend=pose_track_v2"
            << '\n';
        log.flush();
    }

    // The game's cinematic slow motion (kill cam) stretches of the clip, found at
    // playback start from the camera cuts and the time scale.
    std::vector<replay::camera::TimeSpan> g_cinematicSpans;
    std::uint32_t g_cinematicSpanLogs = 0;
    const replay::camera::TimeSpan* g_cinematicSpanShown = nullptr;

    bool StartPlayback(ULONGLONG now)
    {
        replay::prof::ResetSession();
        // A loaded clip plays as loaded (the buffer stayed off since the load); the
        // buffer resumes after this playback stops.
        g_loadedClipHoldsBuffer = false;
        // The buffer plays what it holds: stopped without a file, cut to its window in
        // memory.
        if (g_recording && g_recordKind == RecordKind::Buffer)
            StopBufferForPlayback();
        if (g_transport.DurationMs() == 0)
        {
            ShowObjective(
                "Redemption Editor: record a clip before opening the timeline");
            return false;
        }

        if (g_recording)
            StopRecording();

        g_clipStartTogglesBefore = replay::world::CurrentPressure().iplToggles;
        StopPlayback();
        if (!TakePoseFinalizer())
            return false;
        WorldModelEnter(); // before the transport and before any puppet
        g_transport.Start(PlaybackClockMs());
        BeginClipWorld();
        replay::doors::BeginPlayback(g_clip, LogFile(), g_world.cleared); // the closed theater keeps door ratios
        // The kill cam stretches of the clip (camera_track.h).
        g_cinematicSpans = replay::camera::CinematicSlowSpans(g_clip.gameCamera, g_clip.timeScale);
        g_cinematicSpanLogs = 0;
        g_cinematicSpanShown = nullptr;
        {
            std::ofstream& spanLog = LogFile();
            spanLog << "CINEMATIC_SLOW_SPANS,count=" << g_cinematicSpans.size() << ",spans=";
            for (std::size_t index = 0; index < g_cinematicSpans.size() && index < 16; ++index)
                spanLog << (index ? ";" : "") << g_cinematicSpans[index].startMs << '-' << g_cinematicSpans[index].endMs;
            spanLog << '\n';
        }

        std::ofstream& log = LogFile();
        log << "PLAYBACK_CAPTURE_MODE"
            << ",actor_backend=pose_track_v2"
            << ",task_native_actor_calls=0"
            << ",weapon_actor_calls=0"
            << '\n';
        replay::drawstore::LogSlot("playback_start", log);
        log.flush();
        // The world readiness log of this start, and with ClipStartStagger the puppets
        // wait for the world (UpdateClipLoadWait feeds the gate) instead of streaming
        // their models while the IPLs change.
        g_clipWorldLines = 0;
        g_clipWorldLastLogMs = 0;
        g_clipWorldLastBits = ~0u;
        g_clipStart.Begin(GetTickCount64(), g_clipStartConfig);
        if (g_clipStart.Holding())
        {
            LogFile() << "CLIP_START_STAGGER,phase=hold,batch=" << g_clipStartConfig.batch << ",world_cap_ms=" << g_clipStartConfig.worldCapMs
                      << ",settle_ms=" << g_clipStartConfig.settleMinMs << ",streaming_max=" << g_clipStartConfig.streamingMax
                      << ",entities=" << g_clip.created.size() << '\n';
            ShowObjective("Redemption Editor: loading the world...");
            return true;
        }
        replay::playback::Start();
        LogStreamPressure("start", "immediate", GetTickCount64());
        ShowObjective("Redemption Editor: preparing entities...");
        return true;
    }

    // Scrub keys held this frame: -1 backward, +1 forward, 0 none or both.
    int ScrubDirectionHeld()
    {
        if (const int bench = replay::bench::ScrubOverride(nullptr))
            return bench;
        const bool back = InterlockedCompareExchange(&g_scrubBackHeld, 0, 0) != 0;
        const bool forward = InterlockedCompareExchange(&g_scrubForwardHeld, 0, 0) != 0;
        if (back == forward)
            return 0;
        return back ? -1 : 1;
    }

    void LogScrub(const char* phase, const replay::transport::ScrubState& scrub, ULONGLONG now)
    {
        std::ofstream& log = LogFile();
        log << "PLAYBACK_SCRUB"
            << ",phase=" << phase
            << ",mode=" << (scrub.fine ? "fine" : "speed")
            << ",direction=" << scrub.direction
            << ",from_ms=" << scrub.fromMs
            << ",to_ms=" << scrub.toMs
            << ",frames=" << scrub.frames
            << ",wall_ms=" << (now >= scrub.startedAt ? now - scrub.startedAt : 0ULL) // start: startedAt can be the next tick
            << ",boundary=" << (scrub.atBoundary ? 1 : 0)
            << '\n';
        log.flush();
    }

    // The playhead moves in the transport; this reports what it decided. While
    // scrubbing every subsystem samples the new time on this frame and nothing
    // one-shot plays (PoseV2NormalPlay is false); a scrub ends paused where it
    // stopped.
    // ---- Clip trim, marker speed, effects, marker snap ----
    // The first and last markers are the clip's start and end: a clip opened
    // without markers gets both (Game Camera), the playback stays between them,
    // and they cannot be deleted.
    std::uint64_t g_clipHoldMs = 0;
    bool g_trimReady = false;

    const replay::camera::Keyframe* ActiveMarkerAt(std::uint64_t timeMs)
    {
        const replay::camera::Keyframe* active = nullptr;
        for (const replay::camera::Keyframe& key : g_cameraTrack.Keyframes())
        {
            if (key.timeMs > timeMs)
                break;
            if (key.markerType != replay::editor::MarkerType::Anchor)
                active = &key;
        }
        return active;
    }

    bool TrimActive()
    {
        return g_cameraTrackEnabled && g_cameraTrack.Keyframes().size() >= 2 && !g_cameraTrack.LegacyBlendAnyMode();
    }

    // A clip bound (start or end marker) at timeMs: Game Camera, with the recorded
    // game camera at that time as its pose (a marker that inherits it, or a type
    // change to Free, starts from that view instead of the world origin). The
    // clip's own bounds below and a project clip's bounds (FitMarkers templates)
    // are built this way.
    replay::camera::Keyframe GameBoundMarker(std::uint64_t timeMs)
    {
        replay::camera::Keyframe key;
        key.cameraMode = replay::editor::CameraMode::Game;
        key.timeMs = timeMs;
        replay::editor::CameraState game{};
        if (replay::camera::EvaluateGame(g_clip.gameCamera, timeMs, game, &g_clip.cuts))
            key.pose = {game.position, replay::camera::Track::FromEulerDegrees(game.rotation), game.fov};
        return key;
    }

    bool ProjectReachedClipEnd(ULONGLONG now);

    void ApplyClipTrim(ULONGLONG now)
    {
        if (!g_trimReady && !replay::playback::Preparing())
        {
            g_trimReady = true;
            g_clipHoldMs = g_transport.StartMs();
            // Also a track with one marker (added while the clip was still loading, before
            // the bounds came: otherwise the bounds never come and deleting it leaves no
            // camera track at all).
            if (g_cameraTrack.Keyframes().size() < 2 && !g_cameraTrack.LegacyBlendAnyMode() &&
                g_transport.DurationMs() > g_clipHoldMs + 100)
            {
                // The bounds carry the game camera at their time as their pose: a marker that
                // inherits it, or a type change to Free, starts from that view instead of the
                // world origin.
                const replay::camera::Keyframe start = GameBoundMarker(g_clipHoldMs);
                const replay::camera::Keyframe end = GameBoundMarker(g_transport.DurationMs());
                // A marker already there keeps its place (AddMarker refuses
                // one too close to it).
                g_cameraTrack.AddMarker(start);
                g_cameraTrack.AddMarker(end);
                replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                g_cameraTrackEnabled = true;
                LogFile() << "CAMERA_MARKER,action=clip_bounds,start_ms=" << start.timeMs << ",end_ms=" << end.timeMs << '\n';
            }
        }
        if (!g_trimReady || !TrimActive())
            return;
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        const std::uint64_t start = std::max<std::uint64_t>(g_clipHoldMs, keys.front().timeMs);
        const std::uint64_t end = std::max<std::uint64_t>(start, keys.back().timeMs);
        g_transport.SetStartMs(start);
        if (g_transport.PlayheadMs() + 1 < start)
            g_transport.Seek(start);
        // Nor past the end marker (a paused seek, Ctrl+Right or a scrub going on past
        // it: the playback stays between the clip's start and end markers).
        if (end > start && g_transport.PlayheadMs() > end && (g_transport.Paused() || g_transport.Scrubbing()))
            g_transport.Seek(end);
        if (!g_transport.Paused() && !g_transport.Scrubbing() && g_transport.PlayheadMs() >= end && end > start)
        {
            // A project playing as a whole goes on to its next clip.
            if (ProjectReachedClipEnd(now))
                return;
            SetPlaybackPaused(true, now);
            g_transport.Seek(end);
            g_seekReason = "trim_end";
        }
    }

    // Dead Eye in the replay (off by default): the span at the playhead plays the
    // game's screen effect, and the slow motion is kept (the playhead is not sped
    // up to game speed there).
    // What renders at the playhead is the recorded game camera: no freecam, no
    // Edit Camera, and no marker in effect or a Game-mode one.
    bool GameCameraShownAt(std::uint64_t timeMs)
    {
        if (g_freeCamEnabled || g_editingMarker)
            return false;
        if (!g_cameraTrackEnabled || g_cameraTrack.Empty())
            return true;
        const replay::camera::SegmentPlan plan = replay::camera::Plan(g_cameraTrack, timeMs);
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        if (!plan.valid || plan.current < 0 || plan.current >= static_cast<int>(keys.size()))
            return true;
        return keys[plan.current].mode == replay::editor::CameraMode::Game && !plan.blending;
    }

    const replay::AbilitySpan* AbilityAt(std::uint64_t timeMs)
    {
        for (const replay::AbilitySpan& span : g_clip.abilities)
            if (timeMs >= span.startMs && timeMs <= span.endMs)
                return &span;
        return nullptr;
    }

    std::string g_abilityPostFx;

    // Dead Eye sound: the game plays it from C++ on the frontend entity; the replay
    // plays the same sounds by natives (rung 1): enter, the BED loop with its
    // deadeye_time, the muffling mix scene by hash (0x2233F49A), exit. Only in
    // normal play; a pause, scrub or jump cuts it without the exit.
    struct AbilityAudio
    {
        const replay::AbilitySpan* span = nullptr;
        int bed = -1;
        bool scene = false;
        const char* soundSet = "";
    };
    AbilityAudio g_abilityAudio;
    constexpr Hash kDeadEyeScene = 0x2233F49Au;

    const char* AbilitySoundSet(const replay::AbilitySpan& span)
    {
        if (span.kind == replay::AbilityKind::EagleEye)
            return "EAGLE_EYE_SOUNDS";
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        return g_clip.place.playerModel == john ? "DEADEYE_NEUTRAL_02_SOUNDS" : "DEADEYE_NEUTRAL_01_SOUNDS";
    }

    void StopAbilityAudio(bool withExit)
    {
        AbilityAudio& audio = g_abilityAudio;
        if (!audio.span)
            return;
        if (audio.bed >= 0)
        {
            AUDIO::_STOP_SOUND_WITH_ID(audio.bed);
            AUDIO::RELEASE_SOUND_ID(audio.bed);
        }
        if (audio.scene)
        {
            if (audio.span->kind == replay::AbilityKind::EagleEye)
                AUDIO::STOP_AUDIO_SCENE("EAGLE_EYE_SCENE");
            else
                AUDIO::_0x6AB944DF68B512D3(static_cast<Any>(kDeadEyeScene));
        }
        if (withExit)
            AUDIO::PLAY_SOUND_FRONTEND("exit", audio.soundSet, TRUE, 0);
        LogFile() << "ABILITY_AUDIO,action=stop,exit=" << (withExit ? 1 : 0) << '\n';
        audio = {};
    }

    void UpdateAbilityAudio(const replay::AbilitySpan* span)
    {
        const bool normal = span && replay::playback::NormalPlay();
        AbilityAudio& audio = g_abilityAudio;
        if (audio.span && audio.span != span)
            StopAbilityAudio(span == nullptr && replay::playback::NormalPlay()); // left the span by playing on
        if (!normal)
        {
            if (audio.span)
                StopAbilityAudio(false);
            return;
        }
        if (!audio.span)
        {
            audio.span = span;
            audio.soundSet = AbilitySoundSet(*span);
            const BOOL prepared = AUDIO::PREPARE_SOUNDSET(audio.soundSet, FALSE);
            AUDIO::PLAY_SOUND_FRONTEND("enter", audio.soundSet, TRUE, 0);
            if (span->kind == replay::AbilityKind::EagleEye)
                audio.scene = AUDIO::START_AUDIO_SCENE("EAGLE_EYE_SCENE") != FALSE;
            else
            {
                AUDIO::_0xDC2F83A0612CA34D(static_cast<Any>(kDeadEyeScene));
                audio.scene = true;
                audio.bed = AUDIO::GET_SOUND_ID();
                AUDIO::_PLAY_SOUND_FRONTEND_WITH_SOUND_ID(audio.bed, "BED", audio.soundSet, TRUE);
            }
            LogFile() << "ABILITY_AUDIO,action=start,set=" << audio.soundSet << ",prepared=" << (prepared ? 1 : 0)
                      << ",bed=" << audio.bed << '\n';
        }
        if (audio.bed >= 0)
            AUDIO::_SET_VARIABLE_ON_SOUND_WITH_ID(audio.bed, "deadeye_time",
                static_cast<float>(g_transport.PlayheadMs() - span->startMs) / 1000.0f);
    }

    void ApplyAbilityEffect(bool playing)
    {
        if (!playing)
            StopAbilityAudio(false);
        const replay::AbilitySpan* span = playing && g_recConfig.replayDeadEye ? AbilityAt(g_transport.PlayheadMs()) : nullptr;
        UpdateAbilityAudio(span);
        const char* fx = !span ? "" : span->kind == replay::AbilityKind::DeadEye ? "DEADEYE" : "EagleEye";
        if (g_abilityPostFx == fx)
            return;
        if (!g_abilityPostFx.empty())
            GRAPHICS::ANIMPOSTFX_STOP(g_abilityPostFx.c_str());
        if (*fx)
            GRAPHICS::ANIMPOSTFX_PLAY(fx);
        LogFile() << "ABILITY_PLAY,effect=" << (*fx ? fx : "none") << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        g_abilityPostFx = fx;
    }

    // The marker's speed (5 to 200 %) from it to the next one.
    double MarkerSpeedAt(std::uint64_t timeMs)
    {
        if (!g_cameraTrackEnabled)
            return 1.0;
        const replay::camera::Keyframe* active = ActiveMarkerAt(timeMs);
        return active ? replay::camera::SpeedFactor(active->speedIndex) : 1.0;
    }

    // Effects (the game's own photo mode): the filter is one of its animated post
    // effects with a strength.
    //
    // Exposure and contrast (build 1491.50 and the camera_photomode script):
    //  - _CHANGE_PHOTO_MODE_EXPOSURE / _CONTRAST ADD their argument to the photo
    //    mode value (the handlers are `value += arg`; the photo mode script steps
    //    them by +-0.1 and +-0.0125).
    //  - the post fx update applies them only while the byte that
    //    _0xF5793BB386E1FF9C sets is on (the photo mode sets it to 1 while open,
    //    0 on exit) and a photo mode state predicate holds; outside the photo mode
    //    that predicate is the flag _0x9F6D859C80708B26 (p0 value, p1 0) sets.
    //    Exposure also needs the exposure lock byte at 0
    //    (_SET_PHOTO_MODE_EXPOSURE_LOCKED(FALSE)); it then REPLACES the auto
    //    exposure, clamped to -16..16; contrast is clamped to 0.6..2.
    //  - _UPDATE_PHOTO_MODE_EXPOSURE re-seeds from the exposure already applied,
    //    not from the auto exposure, so calling it every frame and adding the
    //    offset on top runs away to the 16 clamp. The photo mode script seeds the
    //    exposure ONCE (_UPDATE_PHOTO_MODE_EXPOSURE and a read, on the lock toggle
    //    and the first HUD), then only steps it: +-0.1 a press; the HUD shows
    //    ceil(e * 10) + 161 and stops raising at 240, so its top is 7.9; below,
    //    only the engine clamp (-16) holds.
    //  - contrast: +-0.0125 a frame held (0.025 with the keyboard); the HUD shows
    //    ceil(c * 10) - 6 clamped to 0..14, so 0.6 to 2.0.
    // Our -100..100 maps to that range around the value found when grading
    // started: 0 is that value, +100 the photo mode top, -100 its bottom. Every
    // frame the change passed is the delta from the value read to the target (the
    // natives add), never the offset again. An exposure 0 marker drives the
    // exposure back to the seed and gives the exposure back to the auto exposure
    // (lock byte on); the seed is taken again only when an exposure starts from 0
    // (the auto exposure is running then).
    std::string g_effectPostFx;
    constexpr float kGradeExposureMin = -16.0f;
    constexpr float kGradeExposureMax = 7.9f;
    constexpr float kGradeContrastMin = 0.6f;
    constexpr float kGradeContrastMax = 2.0f;
    struct MarkerGrade
    {
        bool engaged = false;
        bool exposureOwned = false; // the seed below is valid and in use
        float baseContrast = 1.0f;
        float baseExposure = 0.0f;
        std::int8_t contrast = 0;
        std::int8_t exposure = 0;
        std::uint32_t logs = 0;
        std::uint32_t settleFrames = 0; // frames left before the settled log
    };
    MarkerGrade g_markerGrade;

    // Marker percent (-100..100) to a value around a base inside [lo, hi].
    float GradeTarget(float base, std::int8_t percent, float lo, float hi)
    {
        const float p = std::clamp(static_cast<float>(percent), -100.0f, 100.0f) / 100.0f;
        const float b = std::clamp(base, lo, hi);
        return p >= 0.0f ? b + p * (hi - b) : b + p * (b - lo);
    }

    void ReleaseMarkerGrade(const char* why)
    {
        MarkerGrade& grade = g_markerGrade;
        if (grade.engaged)
        {
            const float contrastNow = GRAPHICS::_GET_PHOTO_MODE_CONTRAST();
            GRAPHICS::_CHANGE_PHOTO_MODE_CONTRAST(grade.baseContrast - contrastNow);
            if (grade.exposureOwned)
                GRAPHICS::_CHANGE_PHOTO_MODE_EXPOSURE(grade.baseExposure - GRAPHICS::_GET_PHOTO_MODE_EXPOSURE());
            LogFile() << "MARKER_EFFECTS,grade=off,why=" << why << ",contrast_base=" << grade.baseContrast
                      << ",contrast_after=" << GRAPHICS::_GET_PHOTO_MODE_CONTRAST()
                      << ",exposure_base=" << grade.baseExposure
                      << ",exposure_after=" << GRAPHICS::_GET_PHOTO_MODE_EXPOSURE() << '\n';
        }
        // Also when not engaged: a grade of a module instance before a
        // Ctrl+R is not in this instance's state. The lock byte back to 0,
        // the value the photo mode script starts with (bLocal_11 false).
        GRAPHICS::_SET_PHOTO_MODE_EXPOSURE_LOCKED(FALSE);
        GRAPHICS::_0xF5793BB386E1FF9C(FALSE);
        GRAPHICS::_0x9F6D859C80708B26(FALSE, FALSE);
        grade.engaged = false;
        grade.exposureOwned = false;
        grade.contrast = grade.exposure = 0;
        grade.settleFrames = 0;
    }

    void ApplyMarkerGrade(std::int8_t contrast, std::int8_t exposure)
    {
        MarkerGrade& grade = g_markerGrade;
        if (contrast == 0 && exposure == 0)
        {
            if (grade.engaged)
                ReleaseMarkerGrade("marker");
            return;
        }
        if (!grade.engaged)
        {
            const float found = GRAPHICS::_GET_PHOTO_MODE_CONTRAST();
            grade.baseContrast = found >= kGradeContrastMin && found <= kGradeContrastMax ? found : 1.0f;
            grade.engaged = true;
            grade.exposureOwned = false;
            grade.contrast = grade.exposure = 0;
        }
        // Seed the exposure once, before the flags replace the auto one
        // (the photo mode's own order: update, read, then step).
        if (exposure != 0 && !grade.exposureOwned)
        {
            GRAPHICS::_UPDATE_PHOTO_MODE_EXPOSURE();
            grade.baseExposure = std::clamp(GRAPHICS::_GET_PHOTO_MODE_EXPOSURE(), kGradeExposureMin, kGradeExposureMax);
            grade.exposureOwned = true;
        }
        GRAPHICS::_0xF5793BB386E1FF9C(TRUE);
        GRAPHICS::_0x9F6D859C80708B26(TRUE, FALSE);
        // Exposure: lock byte 0 replaces the auto exposure with ours; on an
        // exposure 0 marker the auto exposure runs again (lock on) and our
        // value is also driven back to the seed, so it is 0 either way.
        GRAPHICS::_SET_PHOTO_MODE_EXPOSURE_LOCKED(exposure != 0 ? FALSE : TRUE);
        const float exposureTarget = grade.exposureOwned
            ? GradeTarget(grade.baseExposure, exposure, kGradeExposureMin, kGradeExposureMax) : 0.0f;
        if (grade.exposureOwned)
        {
            const float delta = exposureTarget - GRAPHICS::_GET_PHOTO_MODE_EXPOSURE();
            if (std::fabs(delta) > 0.0005f)
                GRAPHICS::_CHANGE_PHOTO_MODE_EXPOSURE(delta);
            if (exposure == 0 && std::fabs(delta) <= 0.0005f)
                grade.exposureOwned = false; // back at the seed: the next exposure seeds again
        }
        const float contrastTarget = GradeTarget(grade.baseContrast, contrast, kGradeContrastMin, kGradeContrastMax);
        const float contrastNow = GRAPHICS::_GET_PHOTO_MODE_CONTRAST();
        if (std::fabs(contrastTarget - contrastNow) > 0.0005f)
            GRAPHICS::_CHANGE_PHOTO_MODE_CONTRAST(contrastTarget - contrastNow);
        const bool changed = contrast != grade.contrast || exposure != grade.exposure;
        if (changed)
        {
            grade.contrast = contrast;
            grade.exposure = exposure;
            grade.settleFrames = 30;
        }
        // One line on the change and one 30 frames later (the values read
        // must equal the targets and stay there, no drift).
        const bool settled = !changed && grade.settleFrames > 0 && --grade.settleFrames == 0;
        if ((changed || settled) && grade.logs < 48)
        {
            ++grade.logs;
            LogFile() << "MARKER_EFFECTS,grade=on,at=" << (changed ? "change" : "settled")
                      << ",contrast=" << static_cast<int>(contrast)
                      << ",exposure=" << static_cast<int>(exposure) << ",contrast_base=" << grade.baseContrast
                      << ",contrast_target=" << contrastTarget
                      << ",contrast_read=" << GRAPHICS::_GET_PHOTO_MODE_CONTRAST()
                      << ",exposure_base=" << grade.baseExposure
                      << ",exposure_owned=" << (grade.exposureOwned ? 1 : 0)
                      << ",exposure_target=" << exposureTarget
                      << ",exposure_read=" << GRAPHICS::_GET_PHOTO_MODE_EXPOSURE()
                      << ",exposure_lock=" << (exposure != 0 ? 0 : 1) << '\n';
        }
    }

    // The clip's fade in and fade out: a black full screen rectangle drawn every
    // frame with the fade amount at the playhead, so a scrub or a still shows
    // exactly the fade of that instant.
    float g_fadeDrawn = 0.0f;

    void DrawMarkerFade(bool active)
    {
        const float amount = active && g_cameraTrackEnabled
            ? replay::camera::FadeAmount(g_cameraTrack, g_transport.PlayheadMs()) : 0.0f;
        const int alpha = static_cast<int>(amount * 255.0f + 0.5f);
        if (alpha > 0)
            GRAPHICS::DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, 0, 0, 0, alpha, FALSE, TRUE);
        // Evidence at the ends of a fade (not per frame).
        const bool edge = (amount >= 0.999f) != (g_fadeDrawn >= 0.999f) || (amount > 0.0f) != (g_fadeDrawn > 0.0f);
        if (edge)
            LogFile() << "MARKER_EFFECTS,fade=" << amount << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        g_fadeDrawn = amount;
    }

    void ApplyMarkerEffects(bool active)
    {
        const replay::camera::Keyframe* marker = active && g_cameraTrackEnabled ? ActiveMarkerAt(g_transport.PlayheadMs()) : nullptr;
        const std::uint32_t filter = marker ? marker->filter : 0;
        const char* fx = filter < replay::editor::kMarkerFilterCount ? replay::editor::kMarkerFilterPostFx[filter] : "";
        if (g_effectPostFx != fx)
        {
            if (!g_effectPostFx.empty())
                GRAPHICS::ANIMPOSTFX_STOP(g_effectPostFx.c_str());
            if (*fx)
                GRAPHICS::ANIMPOSTFX_PLAY(fx);
            g_effectPostFx = fx;
            LogFile() << "MARKER_EFFECTS,filter=" << (*fx ? fx : "none") << '\n';
        }
        if (*fx)
            GRAPHICS::_ANIMPOSTFX_SET_STRENGTH(fx, marker->filterIntensity / 100.0f);
        // Exposure and contrast are off: turning the photo mode grade on outside photo
        // mode blows the image to white. Saved values stay in the clip, unused.
        constexpr bool kMarkerGradeEnabled = false;
        if (marker && kMarkerGradeEnabled)
            ApplyMarkerGrade(marker->contrast, marker->brightness);
        else if (g_markerGrade.engaged)
            ReleaseMarkerGrade(active ? "no_marker" : "stop");
        DrawMarkerFade(active);
    }

    // Every screen effect the replay starts is stopped when a playback stops or
    // starts and when the mod unloads: the marker filter and the Dead Eye / Eagle
    // Eye post effect it tracks, the photo mode exposure and contrast, and any
    // photo mode filter still running (only the replay plays those outside the
    // game's photo mode; one started by a module instance before a Ctrl+R is not
    // in this instance's state). The game's own effects (damage, low health) are
    // left alone.
    void StopOwnScreenEffects(const char* why)
    {
        ApplyMarkerEffects(false);
        ReleaseMarkerGrade(why); // the photo mode grading flags too
        ApplyAbilityEffect(false);
        std::uint32_t swept = 0;
        for (std::uint32_t index = 1; index < replay::editor::kMarkerFilterCount; ++index)
        {
            const char* name = replay::editor::kMarkerFilterPostFx[index];
            if (GRAPHICS::ANIMPOSTFX_IS_RUNNING(name))
            {
                GRAPHICS::ANIMPOSTFX_STOP(name);
                ++swept;
            }
        }
        if (swept)
            LogFile() << "MARKER_EFFECTS,action=swept,why=" << why << ",stopped=" << swept << '\n';
    }

    // Fine scrub stops on a marker it crosses (a snap): the first marker in the
    // scrub's direction (going back, the latest one below the old playhead), and
    // only on the fine scrub gesture.
    void SnapScrubToMarker(std::uint64_t before)
    {
        if (!g_transport.Scrubbing() || !g_transport.Scrub().fine)
            return;
        if (const replay::camera::Keyframe* key =
                replay::camera::FirstMarkerCrossed(g_cameraTrack, before, g_transport.PlayheadMs()))
            g_transport.Seek(key->timeMs);
    }

    // ---- seek settle (seek_settle.h) ----

    // The "jump" load scene the settle started goes with it.
    void EndSettleJumpScene()
    {
        if (g_settleJumpScene && g_clipWorld.loadPhase && std::strcmp(g_clipWorld.loadPhase, "jump") == 0)
        {
            StopClipLoadScene();
            g_clipWorld.loadPhase = nullptr;
            g_clipWorld.jumpMeters = 0.0f;
        }
        g_settleJumpScene = false;
    }

    void LogSettleEnd(const char* phase, ULONGLONG now, bool timedOut)
    {
        if (!g_jumpLoadLogs.Take(kJumpLoadLogCap))
            return;
        const replay::settle::Readiness& last = g_settle.LastReadiness();
        LogFile() << "JUMP_LOAD,phase=" << phase << ",reason=" << g_settle.Reason() << ",far="
                  << replay::settle::FarReasons(g_settle.FarBits()) << ",wait_ms="
                  << (std::strcmp(phase, "end") == 0 ? g_settle.EndedElapsedMs() : g_settle.ElapsedMs(now))
                  << ",timed_out=" << (timedOut ? 1 : 0) << ",ticks=" << g_settle.Ticks() << ",retargets=" << g_settle.Retargets()
                  << ",missing=" << replay::settle::Missing(last) << ",building=" << last.building
                  << ",models_missing=" << last.modelsMissing << ",streaming_requests=" << STREAMING::GET_NUMBER_OF_STREAMING_REQUESTS()
                  << ",target_ms=" << g_settle.TargetMs() << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // A scrub or a drag cancels the wait (and its preloads).
    void CancelSettle(ULONGLONG now, const char* why)
    {
        if (!g_settle.Active())
            return;
        g_settle.Cancel(now);
        LogSettleEnd(why, now, false);
        EndSettleJumpScene();
    }

    void BeginSettle(ULONGLONG now, const char* reason, bool fine, std::uint64_t fromMs, std::uint64_t toMs)
    {
        replay::settle::JumpFacts facts;
        facts.fine = fine;
        facts.windowCentreMs = g_preloadCentreValid ? g_preloadCentreMs : fromMs;
        facts.targetMs = toMs;
        facts.puppetsWithoutModel = replay::playback::AliveWithoutModel(static_cast<std::uint32_t>(toMs));
        const std::uint32_t bits = replay::settle::FarBitsOf(facts, g_settleConfig);
        const bool wasFar = g_settle.Far();
        g_settle.Begin(reason, now, toMs, bits);
        if (!wasFar && g_settle.Far())
            ++g_seekLoadSequence;
        // The camera part of the test runs in this tick's UpdateReplayCamera,
        // against the camera rendered before the jump.
        g_settleCameraCheck = !fine;
        g_settleCameraFrom = g_renderedCamera.position;
        g_settleCameraFromValid = g_renderedCamera.valid;
        g_nextPreloadAt = 0; // the model window moves to the target now
        if (g_jumpLoadLogs.Take(kJumpLoadLogCap))
        {
            LogFile() << "JUMP_LOAD,phase=begin,reason=" << reason << ",fine=" << (fine ? 1 : 0) << ",far="
                      << replay::settle::FarReasons(g_settle.FarBits()) << ",from_ms=" << fromMs << ",to_ms=" << toMs
                      << ",window_centre_ms=" << facts.windowCentreMs << ",without_model=" << facts.puppetsWithoutModel
                      << ",retargets=" << g_settle.Retargets() << '\n';
        }
    }

    // Before the transport moves: a scrub cancels the settle, the settle holds the
    // time, and normal play holds a frame for a due puppet whose model is loaded
    // (bounded).
    void SettleBeforeTransport(ULONGLONG now, replay::transport::Input& input)
    {
        const bool scrubInput = input.scrubDirection != 0;
        if (scrubInput)
            CancelSettle(now, "cancelled_scrub");
        bool creationHold = false;
        const std::uint32_t due = replay::playback::GetSettleState().dueWithModel;
        if (due == 0)
            g_creationHoldSpent = false;
        if (!g_creationHoldSpent && !g_settle.Active() && !scrubInput && !g_transport.Paused() && !g_transport.Scrubbing() &&
            !replay::playback::Preparing() && !ClipLoadWaiting())
        {
            creationHold = replay::settle::HoldForCreation(replay::playback::NormalPlay(), due, g_creationHeldFrames, g_settleConfig);
        }
        if (creationHold)
        {
            ++g_creationHeldFrames;
            ++g_creationHolds;
        }
        else if (g_creationHeldFrames)
        {
            // Still due after the bound: the engine keeps refusing; play on
            // and hold again only after nothing was due.
            g_creationHoldSpent = due > 0;
            if (g_creationHoldLogs.Take(64))
            {
                LogFile() << "CREATION_HOLD,frames=" << g_creationHeldFrames << ",total=" << g_creationHolds
                          << ",still_due=" << due << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
            g_creationHeldFrames = 0;
        }
        input.hold = replay::playback::Preparing() || ClipLoadWaiting() || (g_settle.Active() && !scrubInput) || creationHold ||
            g_clipStart.Holding();
    }

    // After the transport moved (and the seeks of this tick's commands): a
    // scrub release, a drag end or a jump starts the settle; the cheap scrub
    // flag for this tick's playback.
    void SettleAfterTransport(ULONGLONG now, std::uint32_t events)
    {
        const std::uint64_t playhead = g_transport.PlayheadMs();
        const std::uint64_t before = g_settleLastPlayhead;
        g_settleLastPlayhead = playhead;
        const char* reason = g_seekReason;
        bool fine = g_seekFine;
        g_seekReason = nullptr;
        g_seekFine = false;
        const bool opening = replay::playback::Preparing() ||
            (ClipLoadWaiting() && std::strcmp(g_clipWorld.loadPhase, "jump") != 0); // the clip's own load covers it
        bool begin = false;
        if (opening)
            CancelSettle(now, "cancelled_open"); // a clip (re)opening waits on its own
        if (before == ~0ull || opening)
            begin = false;
        else if ((events & replay::transport::kScrubEnded) && !g_transport.Scrubbing()) // not a direction change
        {
            reason = "scrub_release";
            fine = g_transport.EndedScrub().fine;
            begin = true;
        }
        else if (g_seekDrag.TakeIdleEnd(now, g_settleConfig))
        {
            reason = "drag_idle";
            begin = true;
        }
        else if (reason && std::strcmp(reason, "drag_end") == 0)
            begin = true; // the release may land on the last dragged time
        else if (!g_seekDrag.Dragging() &&
            replay::settle::IsJumpMove(before, playhead, g_transport.Paused(), g_transport.Scrubbing()))
        {
            if (!reason)
                reason = "jump";
            begin = true;
        }
        if (g_seekDrag.Dragging())
            CancelSettle(now, "cancelled_drag");
        if (begin)
            BeginSettle(now, reason, fine, before, playhead);
        // Cheap scrub: while the playhead is scrubbed or dragged, and while
        // the settle still builds puppets (the state rebuild runs once they
        // exist, so it reaches every puppet of the target).
        const bool building = g_settle.Active() && replay::playback::GetSettleState().building > 0;
        replay::playback::SetCheapScrub(g_transport.Scrubbing() || g_seekDrag.Dragging() || building);
    }

    replay::settle::CostCounters CostCountersNow()
    {
        replay::settle::CostCounters counters;
        counters.decodes = replay::pose::DecodedFrameCount();
        counters.checkpointRestores = replay::pose::CheckpointRestoreCount();
        const replay::events::StateCounters state = replay::events::Counters();
        counters.decalReconciles = state.decalReconciles;
        counters.bloodReconciles = state.bloodReconciles;
        counters.goreReconciles = state.goreReconciles;
        counters.fireReconciles = state.fireReconciles;
        counters.deferredTicks = state.deferredTicks;
        const replay::playback::Counters playback = replay::playback::GetCounters();
        counters.creates = playback.creates;
        counters.vehicleRebuilds = playback.vehicleRebuilds;
        return counters;
    }

    // Once per tick after the camera and the clip world: the model window,
    // the settle's readiness and its end, the create budget.
    void UpdateSeekSettle(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.seek_settle");
        if (!g_transport.Playing())
        {
            if (g_settle.Active())
                CancelSettle(now, "cancelled_stop");
            g_settleLastPlayhead = ~0ull;
            g_preloadCentreValid = false;
            g_nextPreloadAt = 0;
            g_seekDrag.Reset();
            g_creationHeldFrames = 0;
            g_creationHoldSpent = false;
            return;
        }
        const std::uint32_t playhead = static_cast<std::uint32_t>(g_transport.PlayheadMs());
        if (now >= g_nextPreloadAt && !replay::playback::Preparing())
        {
            g_nextPreloadAt = now + 250;
            replay::playback::PreloadModels(playhead, now, static_cast<std::uint32_t>(g_settleConfig.windowMs));
            g_preloadCentreMs = playhead;
            g_preloadCentreValid = true;
        }
        if (g_settle.Active())
        {
            const replay::playback::SettleState state = replay::playback::GetSettleState();
            replay::settle::Readiness readiness;
            readiness.building = state.building;
            readiness.modelsMissing = state.modelsMissing;
            readiness.stateDeferred = replay::events::StateDeferred();
            readiness.statePending = replay::events::StatePending();
            readiness.sceneLoaded = !(g_clipWorld.loadPhase && std::strcmp(g_clipWorld.loadPhase, "jump") == 0);
            const replay::settle::Verdict verdict = g_settle.Update(now, readiness, g_settleConfig);
            if (verdict.promoted)
            {
                ++g_seekLoadSequence;
                LogSettleEnd("promoted", now, false);
            }
            if (verdict.done)
            {
                LogSettleEnd("end", now, verdict.timedOut);
                EndSettleJumpScene();
            }
        }
        replay::playback::SetCreateBudget(replay::settle::CreateBudget(g_settle.Active(), g_settle.Far()));
        replay::events::SetStateBudget(g_settle.Active(), g_settle.Far());
        replay::playback::SetWatchHeld(g_settle.Active() || ClipLoadWaiting());
    }

    // One SCRUB_COST line per scrub (keys or a drag), at the tick after it.
    void UpdateScrubCost(double tickUs)
    {
        const bool cheap = g_transport.Playing() && (g_transport.Scrubbing() || g_seekDrag.Dragging());
        if (cheap)
        {
            if (!g_scrubCost.Active())
            {
                const bool drag = g_seekDrag.Dragging();
                const char* kind = drag ? "drag" : g_transport.Scrub().fine ? "fine" : "keys";
                const int direction = drag ? (g_transport.PlayheadMs() >= g_tickStartPlayhead ? 1 : -1) : g_transport.Scrub().direction;
                g_scrubCost.Begin(kind, direction, g_tickStartPlayhead, g_tickStartCounters);
            }
            g_scrubCost.Tick(tickUs, replay::playback::GetSettleState().puppets);
            return;
        }
        if (!g_scrubCost.Active())
            return;
        const std::string line = g_scrubCost.Finish(g_transport.PlayheadMs(), tickUs, CostCountersNow(), replay::playback::CheapScrub());
        if (g_scrubCostLogs.Take(200))
        {
            LogFile() << line;
        }
    }

    // The world time scale and the playhead rate UpdatePlayback below applies at
    // `playheadMs` (playing, not scrubbing: an export never scrubs). Keep the two
    // in step.
    float PlaybackWorldScaleAt(std::uint64_t playheadMs, double& rate)
    {
        const double speed = kPlaybackSpeeds[g_playbackSpeedIndex] * MarkerSpeedAt(playheadMs);
        const float recordedScale = g_clip.timeScale.At(static_cast<std::uint32_t>(playheadMs));
        const bool deadEyeSlow = g_recConfig.replayDeadEye && AbilityAt(playheadMs) != nullptr;
        const replay::camera::TimeSpan* cinematic = replay::camera::SpanAt(g_cinematicSpans, playheadMs);
        const bool slowAbility = deadEyeSlow || (cinematic && GameCameraShownAt(playheadMs));
        rate = slowAbility ? speed : speed / (recordedScale > 0.0f ? recordedScale : 1.0f);
        float world = speed < 1.0 ? static_cast<float>(speed) : 1.0f;
        if (slowAbility)
            world = std::min(world, recordedScale);
        return world;
    }

    // The export's output frames (exportprobe::Request): the span at the rate
    // UpdatePlayback gives each place.
    std::uint64_t ExportOutputFrames(std::uint64_t startMs, std::uint64_t endMs, replay::exporting::FrameRate rate)
    {
        const auto rateAt = [](std::uint64_t ms) {
            double playheadRate = 1.0;
            PlaybackWorldScaleAt(ms, playheadRate);
            return playheadRate;
        };
        return replay::exporting::SpanOutputFrames(startMs, endMs, rate, rateAt, 50000000ull);
    }

    // The lowest world scale over the next two video frames of an export:
    // the playhead walked forward in 1 ms steps of video time at the rate of
    // each place (a marker's slow motion that starts inside the next frame
    // already lowers that frame's motion blur samples).
    float ExportWorldScaleAhead(std::uint64_t playheadMs, float applied)
    {
        if (!replay::exportprobe::Active())
            return applied;
        const replay::exporting::FrameRate rate = replay::exportprobe::ExportRate();
        const double windowMs = 2000.0 * rate.den / (rate.num ? rate.num : 60);
        double playhead = static_cast<double>(playheadMs);
        const double end = static_cast<double>(PlaybackDurationMs());
        float lowest = applied;
        for (double t = 0.0; t <= windowMs && playhead <= end; t += 1.0)
        {
            double playRate = 1.0;
            lowest = std::min(lowest, PlaybackWorldScaleAt(static_cast<std::uint64_t>(playhead), playRate));
            playhead += playRate;
        }
        return lowest;
    }

    void UpdatePlayback(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.playback");
        if (!g_transport.Playing())
            return;

        replay::transport::Input input;
        input.hold = replay::playback::Preparing() || ClipLoadWaiting(); // the world loads
        input.scrubDirection = g_editingMarker ? 0 : ScrubDirectionHeld(); // Edit Camera stays on its marker
        bool benchFine = false;
        input.fineScrub = InterlockedCompareExchange(&g_fastMove, 0, 0) != 0 ||
            (replay::bench::ScrubOverride(&benchFine) != 0 && benchFine);
        SettleBeforeTransport(now, input);
        // Render export: while an export runs, the export clock drives the transport,
        // one tick per game frame, whatever the wall clock does. Inert unless
        // RedemptionEditor.ini [Export] Enabled=1 and the editor asked for an export.
        replay::exportprobe::TickInput exportInput;
        exportInput.playheadMs = g_transport.PlayheadMs();
        exportInput.durationMs = g_transport.DurationMs();
        exportInput.settling = g_settle.Active() || ClipLoadWaiting();
        exportInput.frameTime = MISC::GET_FRAME_TIME();
        exportInput.worldScale = g_worldTimeScaleApplied;
        const replay::exportprobe::TickPlan exportPlan = replay::exportprobe::BeginTick(exportInput, LogFile());
        if (exportPlan.seekToStart)
        {
            SetPlaybackPaused(true, now);
            g_seekReason = "export";
            g_seekFine = false;
            g_transport.Seek(std::min<std::uint64_t>(exportPlan.startMs, PlaybackDurationMs()));
        }
        if (exportPlan.resume)
            SetPlaybackPaused(false, now); // the first export ticks hold time, so the transport takes the export clock
        if (exportPlan.pause)
            SetPlaybackPaused(true, now);
        if (exportPlan.holdTime)
            input.hold = true;
        if (exportPlan.active)
            input.scrubDirection = 0;
        // The game HUD is hidden below (HidesHud joins the playing test),
        // the debug drawing is gated by HidesHud, and the text notices are
        // suppressed: nothing of the mod or the game is burnt into the video.
        replay::hud::SetSuppressed(exportPlan.hideHud);
        if (exportPlan.drawTag)
        {
            // Latency tag (export_channel.h): drawn in preroll and flush
            // pictures only, which are never encoded.
            namespace ec = replay::exportchannel;
            for (std::uint32_t cell = 0; cell < ec::kTagCells; ++cell)
            {
                const int v = ec::TagCellWhite(exportPlan.tagSerial, cell) ? 255 : 0;
                GRAPHICS::DRAW_RECT(ec::TagCellCentreX(cell), ec::TagCellCentreY(), ec::kTagCellWidth, ec::kTagCellHeight, v, v, v, 255, FALSE, TRUE);
            }
        }
        // Time compensation: where the game ran slowed while recording
        // (Dead Eye, weapon wheel), the playhead runs 1/scale faster so the
        // stretch plays at game speed.
        {
            const LONG step = InterlockedExchange(&g_speedStepRequested, 0);
            if (step)
            {
                const int wanted = std::max(0, std::min(static_cast<int>(std::size(kPlaybackSpeeds)) - 1, g_playbackSpeedIndex + static_cast<int>(step)));
                if (wanted != g_playbackSpeedIndex)
                {
                    g_playbackSpeedIndex = wanted;
                    char text[64]{};
                    std::snprintf(text, sizeof(text), "Redemption Editor: speed %d%%", static_cast<int>(kPlaybackSpeeds[wanted] * 100.0 + 0.5));
                    ShowObjective(text);
                    LogFile() << "PLAYBACK_SPEED,percent=" << static_cast<int>(kPlaybackSpeeds[wanted] * 100.0 + 0.5)
                              << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
                }
            }
        }
        ApplyClipTrim(now);
        const double speed = kPlaybackSpeeds[g_playbackSpeedIndex] * MarkerSpeedAt(g_transport.PlayheadMs());
        const float recordedScale = g_clip.timeScale.At(static_cast<std::uint32_t>(g_transport.PlayheadMs()));
        const bool deadEyeSlow = g_recConfig.replayDeadEye && AbilityAt(g_transport.PlayheadMs()) != nullptr;
        // The kill cam keeps its slow motion while the recorded game camera shows it
        // (the time compensation would turn 1.9 s of kill cam into 0.23 s: rate
        // 1/0.061). Under a free or preset marker the stretch plays at game speed.
        const replay::camera::TimeSpan* cinematic = replay::camera::SpanAt(g_cinematicSpans, g_transport.PlayheadMs());
        const bool cinematicSlow = cinematic && GameCameraShownAt(g_transport.PlayheadMs());
        if (cinematic != g_cinematicSpanShown)
        {
            g_cinematicSpanShown = cinematic;
            if (cinematic && g_cinematicSpanLogs < 32)
            {
                ++g_cinematicSpanLogs;
                LogFile() << "CINEMATIC_SLOW,phase=enter,start_ms=" << cinematic->startMs << ",end_ms=" << cinematic->endMs
                          << ",game_camera=" << (cinematicSlow ? 1 : 0) << ",recorded_scale=" << recordedScale
                          << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
        }
        const bool slowAbility = deadEyeSlow || cinematicSlow;
        // With Dead Eye on in the replay its slow motion stays; otherwise the
        // stretch plays at game speed (the time compensation).
        g_transport.SetRate(slowAbility ? speed : speed / recordedScale);
        {
            float world = (speed < 1.0 && !g_transport.Paused() && !g_transport.Scrubbing()) ? static_cast<float>(speed) : 1.0f;
            if (slowAbility && !g_transport.Paused() && !g_transport.Scrubbing())
                world = std::min(world, recordedScale); // sounds and particles slow down with it
            if (world != g_worldTimeScaleApplied)
            {
                MISC::SET_TIME_SCALE(world);
                g_worldTimeScaleApplied = world;
            }
        }
        const std::uint64_t playheadBefore = g_transport.PlayheadMs();
        const std::uint32_t events = g_transport.Update(replay::exportprobe::TransportNowMs(PlaybackClockMs()), input);
        replay::exportprobe::EndTick(g_transport.PlayheadMs(), ExportWorldScaleAhead(g_transport.PlayheadMs(), g_worldTimeScaleApplied),
            LogFile()); // render export: the tick's stamp, the next step (the next frame's blur samples)
        SnapScrubToMarker(playheadBefore);
        SettleAfterTransport(now, events);
        ApplyMarkerEffects(true);
        ApplyAbilityEffect(true);
        if (events == 0)
            return;

        using namespace replay::transport;
        if (events & kScrubEnded)
            LogScrub("end", g_transport.EndedScrub(), now);
        if (events & kPaused)
            LogTransportPaused(true);
        if (events & kScrubStarted)
        {
            const ScrubState& scrub = g_transport.Scrub();
            LogScrub("start", scrub, now);
            ShowObjective(scrub.fine
                ? (scrub.direction < 0 ? "Redemption Editor: fine scrub backward" : "Redemption Editor: fine scrub forward")
                : (scrub.direction < 0 ? "Redemption Editor: scrub 2x backward" : "Redemption Editor: scrub 2x forward"));
        }
        if (events & kReachedStart)
            ShowObjective("Redemption Editor: start of the timeline");
        if (events & kReachedEnd)
            ShowObjective("Redemption Editor: end of the timeline");
    }
    replay::editor::CameraState CurrentEditorCameraState()
    {
        replay::editor::CameraState camera{};
        if (g_freeCamEnabled)
        {
            camera.position = {g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z};
            camera.rotation = {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z};
            camera.fov = g_freeCamFov;
            return camera;
        }

        const Vector3 position = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const Vector3 rotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
        camera.position = {position.x, position.y, position.z};
        camera.rotation = {rotation.x, rotation.y, rotation.z};
        camera.fov = CAM::GET_FINAL_RENDERED_CAM_FOV();
        return camera;
    }

    bool IsValidEasing(replay::editor::Easing easing)
    {
        return easing >= replay::editor::Easing::Linear &&
            easing <= replay::editor::Easing::Hold;
    }

    bool IsValidPath(replay::editor::Path path)
    {
        return path >= replay::editor::Path::Linear &&
            path <= replay::editor::Path::Smooth;
    }

    void AddMarkerAtPlayhead();

    // Insert and Delete outside the editor's menus (the editor closed or hidden,
    // Edit Camera), through the marker API, as the editor's commands (the keyframe
    // API, AddOrReplace and Remove, would turn the track into a converted v2 clip
    // and could delete the clip's start or end marker). Nothing while a marker is
    // in Edit Camera.
    void ProcessCameraKeys()
    {
        if (InterlockedExchange(&g_cameraKeyAddRequested, 0) && !g_editingMarker)
        {
            if (g_transport.Playing())
            {
                const replay::camera::Track before = g_cameraTrack; // a step only when added
                AddMarkerAtPlayhead();
                if (!replay::camera::SameMarkers(before, g_cameraTrack))
                {
                    PushMarkerHistory(static_cast<std::uint32_t>(replay::editor::CommandType::CaptureCameraKeyframe), 0, &before);
                    g_clipEdited = true;
                }
                char text[96]{};
                std::snprintf(text, sizeof(text), "Camera: marker at %.2fs (%u markers)",
                    g_transport.PlayheadMs() / 1000.0, static_cast<unsigned>(g_cameraTrack.Keyframes().size()));
                ShowObjective(text);
            }
            else
                ShowObjective(replay::keys::Fill("Camera: play the replay ({key_stop}) to add markers", replay::keys::Current()).c_str());
        }
        if (InterlockedExchange(&g_cameraKeyDeleteRequested, 0) && !g_editingMarker)
        {
            const auto& keys = g_cameraTrack.Keyframes();
            const bool bounds = TrimActive();
            const std::uint64_t playhead = g_transport.PlayheadMs();
            const auto distance = [&](std::uint64_t t) { return t > playhead ? t - playhead : playhead - t; };
            const replay::camera::Keyframe* nearest = nullptr;
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                if (bounds && (index == 0 || index + 1 == keys.size()))
                    continue; // the clip's start and end stay
                if (!nearest || distance(keys[index].timeMs) < distance(nearest->timeMs))
                    nearest = &keys[index];
            }
            if (nearest)
            {
                const replay::camera::Track before = g_cameraTrack; // a step only when removed
                const std::uint64_t removedAt = nearest->timeMs;
                const std::uint32_t removedId = nearest->id;
                if (g_cameraTrack.RemoveMarker(removedId))
                {
                    PushMarkerHistory(static_cast<std::uint32_t>(replay::editor::CommandType::DeleteCameraKeyframe), removedId, &before);
                    g_clipEdited = true;
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraTrackManualOverride = false;
                    g_cameraSpring.Reset();
                }
                char text[96]{};
                std::snprintf(text, sizeof(text), "Camera: marker at %.2fs deleted (%u markers)", removedAt / 1000.0,
                    static_cast<unsigned>(g_cameraTrack.Keyframes().size()));
                ShowObjective(text);
                LogFile() << "CAMERA_MARKER,action=delete,id=" << removedId << ",from=key,left=" << g_cameraTrack.Keyframes().size()
                          << '\n';
            }
        }
        if (InterlockedExchange(&g_cameraTrackToggleRequested, 0))
        {
            g_cameraTrackEnabled = !g_cameraTrackEnabled;
            g_cameraTrackManualOverride = !g_cameraTrackEnabled;
            ShowObjective(g_cameraTrackEnabled ? "Camera: track on" : "Camera: track off (free camera)");
            LogFile() << "CAMERA_KEY,action=toggle,enabled=" << g_cameraTrackEnabled << '\n';
        }
    }

    // ---- camera markers: add, edit camera ----
    replay::camera::Keyframe g_copiedMarker;
    bool g_hasCopiedMarker = false;
    std::uint32_t g_editingMarker = 0; // "Edit Camera": the free camera frames this marker

    // A marker's attach pose and Look At framing from a world camera: what
    // "Use this view" and leaving Edit Camera store.
    void CaptureMarkerRelations(std::uint32_t id, const replay::editor::CameraState& camera)
    {
        replay::camera::Keyframe* key = g_cameraTrack.FindMutable(id);
        if (!key)
            return;
        const std::uint64_t at = key->timeMs;
        replay::camera::EntityState target{};
        const replay::camera::Scene scene = MarkerScene(); // through the bones
        if (replay::camera::LookupAttach(*key, at, scene, target))
            replay::camera::CaptureAttachPose(*key, camera, target);
        if (replay::camera::LookupLookAt(*key, at, scene, target))
        {
            const replay::editor::Vec3 point = target.hasHead ? target.head
                : replay::editor::Vec3{target.position.x, target.position.y, target.position.z + replay::camera::kLookAtAimHeight};
            replay::editor::Vec2 screen{};
            if (replay::camera::ProjectToScreen(camera, point, MarkerScene().aspect, screen))
            {
                key->lookAtOffset = {std::clamp(screen.x, -1.0f, 1.0f), std::clamp(screen.y, -1.0f, 1.0f)};
                key->lookAtRoll = camera.rotation.y;
            }
        }
    }

    // Add: at the playhead, inheriting from the marker before it; the view is the
    // free camera's when it is on, otherwise the frame being rendered.
    void AddMarkerAtPlayhead()
    {
        const std::uint64_t at = g_transport.PlayheadMs();
        // The start and end markers bound every other marker.
        if (!replay::camera::WithinClipBounds(g_cameraTrack, at))
        {
            replay::hud::ShowNotice(TR("hud.marker_bounds", "Redemption Editor: markers go between the clip's start and end markers."));
            return;
        }
        if (!replay::camera::CanPlace(g_cameraTrack, at))
        {
            replay::hud::ShowNotice(TR("hud.marker_close", "Redemption Editor: too close to another marker."));
            return;
        }
        replay::camera::Keyframe fresh;
        fresh.timeMs = at;
        const replay::camera::Keyframe* previous = nullptr;
        for (const replay::camera::Keyframe& key : g_cameraTrack.Keyframes())
            if (key.timeMs <= at)
                previous = &key;
        if (previous)
            replay::camera::InheritFrom(fresh, *previous);
        // The shake in force there (a marker added inside a blend holds the blended
        // numbers: nothing changes).
        fresh.shake = replay::camera::mshake::HeldAt(g_cameraTrack, at);
        replay::editor::CameraState view{};
        if (g_freeCamEnabled)
        {
            view = CurrentEditorCameraState();
            fresh.cameraMode = replay::editor::CameraMode::Free;
        }
        else
        {
            const Vector3 p = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const Vector3 r = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
            view = {{p.x, p.y, p.z}, {r.x, r.y, r.z}, CAM::GET_FINAL_RENDERED_CAM_FOV()};
        }
        // The pose is always the frame shown at the playhead. InheritFrom copies no
        // pose from a Game Camera marker (the clip's start), so the new marker would
        // keep the default pose, the world origin, and turning it into a Free camera
        // (or Edit Camera, whose orbit clamps the distance to 150 m) would send the
        // camera far away. A preset's stored pose is a placeholder too; a Free marker
        // before it already took the rendered frame.
        fresh.pose = {view.position, replay::camera::Track::FromEulerDegrees(view.rotation),
            std::clamp(view.fov, replay::editor::kMarkerFovMin, replay::editor::kMarkerFovMax)};
        const std::uint32_t id = g_cameraTrack.AddMarker(fresh);
        if (!id)
            return;
        // Always (the freecam off too): the view stored so the marker shows it back
        // with its attach and Look At.
        if (replay::camera::Keyframe* added = g_cameraTrack.FindMutable(id))
            replay::camera::SetMarkerView(*added, view, MarkerScene());
        replay::camera::ApplyLastMarkerRule(g_cameraTrack);
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;
        LogFile() << "CAMERA_MARKER,action=add,id=" << id << ",at_ms=" << at << ",mode="
                  << static_cast<std::uint32_t>(fresh.cameraMode) << ",inherited=" << (previous ? previous->id : 0) << '\n';
    }

    void LeaveMarkerCameraEdit(bool keep)
    {
        if (!g_editingMarker)
            return;
        if (keep && g_freeCamEnabled)
        {
            const replay::editor::CameraState view = CurrentEditorCameraState();
            // Every way out that keeps the view (Enter, F11) is an edit: an undo step and
            // unsaved changes.
            if (g_cameraTrack.Find(g_editingMarker))
            {
                PushMarkerHistory(static_cast<std::uint32_t>(replay::editor::CommandType::EditCameraKeyframe), g_editingMarker);
                g_markerHistory.lastType = 0; // never merged with the next change
                g_clipEdited = true;
            }
            if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(g_editingMarker))
            {
                // The pose only: Track::Update would also re-derive this marker's and the
                // previous one's blend from the legacy easing and turn the whole track into a
                // converted v2 clip (legacyBlendAnyMode: blends between any camera types, the
                // clip's start and end no longer protected nor trimming, saved in the clip).
                replay::camera::SetMarkerView(*key, view, MarkerScene());
                if (g_orbit.marker == key->id)
                {
                    // The orbit's framing and roll as set, not re-derived.
                    if (replay::camera::Keyframe* stored = g_cameraTrack.FindMutable(key->id))
                    {
                        stored->lookAtOffset = g_orbit.offset;
                        stored->lookAtRoll = g_orbit.roll;
                    }
                }
                replay::camera::ApplyLastMarkerRule(g_cameraTrack); // the end follows the new framing now
                LogFile() << "CAMERA_MARKER,action=edit_done,id=" << key->id << '\n';
            }
        }
        g_editingMarker = 0;
        g_orbit.marker = 0;
        // The free camera as it was before Edit Camera (a free camera the player had
        // on stays on).
        if (!g_freeCamBeforeEdit)
            StopFreeCam();
        g_cameraTrackManualOverride = false;
        g_cameraSpring.Reset();
    }

    // "Edit Camera": paused on the marker, the free camera starts from the
    // marker's view; leaving stores the view (and the attach and framing).
    void EnterMarkerCameraEdit(std::uint32_t id, ULONGLONG now)
    {
        const replay::camera::Keyframe* key = g_cameraTrack.Find(id);
        if (!key || !g_transport.Playing())
            return;
        LeaveMarkerCameraEdit(true);
        SetPlaybackPaused(true, now);
        g_transport.Seek(std::min<std::uint64_t>(key->timeMs, PlaybackDurationMs()));
        // The puppets still stand at the old playhead: this view is a first
        // guess; UpdateFreeCam resolves it again once they stand at the
        // marker's time (an attach or Look At target moved in between).
        g_editResolvePending = true;
        g_freeCamBeforeEdit = g_freeCamEnabled;
        replay::editor::CameraState view{};
        const replay::camera::LivePose live = EditStartPose(*key);
        view.position = live.pose.position;
        view.rotation = replay::camera::Track::ToEulerDegrees(live.pose.orientation);
        view.fov = live.pose.fov;
        if (!g_freeCamEnabled && !StartFreeCam(now))
            return;
        g_freeCamPosition = {view.position.x, view.position.y, view.position.z};
        g_freeCamRotation = {view.rotation.x, view.rotation.y, view.rotation.z};
        g_freeCamFov = view.fov;
        // The marker's view, roll included, and nothing else: an acrobatic or Drone
        // flight before this (its orientation, banking, turn rates, momentum) does
        // not carry over.
        g_freeCamVelocity = {};
        ResetFreeCamToView();
        g_cameraTrackManualOverride = true; // the free camera does not snap back to the track
        g_editingMarker = id;
        LogFile() << "CAMERA_MARKER,action=edit,id=" << id << ",level=1,roll=" << view.rotation.y << '\n';
    }

#if defined(REDEMPTION_DEV) // the bench's camera ops (replay_bench.h): developer build only
    // Bench camera markers: marker_add <ms> key=value... with
    // mode=free|game|front_low|rear|right|left|overhead pos=x,y,z rot=p,r,h
    // fov= blend=none|linear|smooth ease=0|1 look=/attach=/focus_t=
    // main|pedN|none mount=rigid|default|flexible shake=none|handheld|drunk|
    // ground|air|explosion|walking|running|horseback|steadicam|wagon shake_i= shake_s=<index> dof=default|custom|none
    // focus=auto|manual|target focus_d= dof_i= roll= dist=
    std::uint32_t BenchTarget(const std::string& name)
    {
        if (name == "main")
            return MainCharacterId();
        // Bench (tool only): horseN and weaponN, the Nth puppet of that kind.
        for (const auto& [prefix, kind] : {std::pair<const char*, replay::entity::EntityKind>{"horse", replay::entity::EntityKind::Horse},
                 std::pair<const char*, replay::entity::EntityKind>{"weapon", replay::entity::EntityKind::Weapon}})
        {
            const std::size_t length = std::strlen(prefix);
            if (name.compare(0, length, prefix) != 0)
                continue;
            const int wanted = std::atoi(name.c_str() + length);
            int seen = 0;
            for (const replay::playback::Puppet& puppet : g_puppets.list)
                if (puppet.packet.kind == kind && ++seen == wanted)
                    return puppet.packet.id.Packed();
            return 0;
        }
        if (name.rfind("ped", 0) == 0)
        {
            const int wanted = std::atoi(name.c_str() + 3);
            const std::uint32_t main = MainCharacterId();
            int seen = 0;
            for (const replay::playback::Puppet& puppet : g_puppets.list)
            {
                if (puppet.packet.kind != replay::entity::EntityKind::Ped || puppet.packet.id.Packed() == main)
                    continue;
                if (++seen == wanted)
                    return puppet.packet.id.Packed();
            }
        }
        return 0;
    }

    // Bench (tool only): attach_bone/look_bone=family:Label (a named bone of
    // camera_bones.h, e.g. human:Head, weapon:Muzzle), near_clip, fstop and
    // lens_mm in metres, f-number and millimetres.
    std::string BenchBoneLabel(std::string label)
    {
        std::replace(label.begin(), label.end(), '_', ' '); // scenario tokens have no spaces: Right_Hand
        return label;
    }

    bool BenchMarkerLensKey(replay::camera::Keyframe& marker, const std::string& key, const std::string& value, std::string& detail)
    {
        namespace bones = replay::camera::bones;
        if (key == "attach_bone" || key == "look_bone")
        {
            const std::size_t colon = value.find(':');
            std::uint32_t ref = 0;
            bool found = false;
            for (std::uint32_t n = 0; n < bones::kNamedCount && colon != std::string::npos; ++n)
                if (value.compare(0, colon, bones::FamilyName(bones::kNamed[n].family)) == 0 && BenchBoneLabel(value.substr(colon + 1)) == bones::kNamed[n].label)
                {
                    ref = bones::Named(n);
                    found = true;
                    break;
                }
            if (!found)
            {
                detail = "unknown_bone=" + value;
                return false;
            }
            (key == "attach_bone" ? marker.attachBone : marker.lookAtBone) = ref;
            return true;
        }
        const float number = static_cast<float>(std::atof(value.c_str()));
        if (key == "near_clip")
            marker.nearClip = number;
        else if (key == "fstop")
            marker.dofFStop = number;
        else if (key == "lens_mm")
            marker.dofLensMm = number;
        return true;
    }

    bool BenchCameraOp(const std::string& op, const std::vector<std::string>& args, std::string& detail)
    {
        if (op == "marker_lens")
        {
            // Bench (tool only): change the lens of the marker at that time in place (what
            // the editor's menu does to the same marker).
            const std::uint64_t at = static_cast<std::uint64_t>(std::atoll(args[0].c_str()));
            replay::camera::Keyframe* key = nullptr;
            for (const replay::camera::Keyframe& k : g_cameraTrack.Keyframes())
                if (k.timeMs == at)
                    key = g_cameraTrack.FindMutable(k.id);
            if (!key)
            {
                detail = "no_marker_at=" + args[0];
                return false;
            }
            for (std::size_t n = 1; n < args.size(); ++n)
            {
                const std::size_t eq = args[n].find('=');
                if (eq == std::string::npos)
                {
                    detail = "bad_arg=" + args[n];
                    return false;
                }
                const std::string name = args[n].substr(0, eq);
                const std::string value = args[n].substr(eq + 1);
                if (name == "focus_d")
                    key->focusDistance = static_cast<float>(std::atof(value.c_str()));
                else if (name == "dof")
                    key->dof = value == "custom" ? replay::editor::DofMode::Custom : value == "none" ? replay::editor::DofMode::None : replay::editor::DofMode::Default;
                else if (!BenchMarkerLensKey(*key, name, value, detail))
                    return false;
            }
            LogFile() << "BENCH_MARKER_LENS,id=" << key->id << ",ms=" << at << ",near_clip=" << key->nearClip << ",fstop=" << key->dofFStop
                      << ",lens_mm=" << key->dofLensMm << ",focus_d=" << key->focusDistance << '\n';
            detail = "id=" + std::to_string(key->id);
            return true;
        }
        using namespace replay::editor;
        // Bench: freecam on|off (the free camera from the rendered view), focus
        // clear|log (clear: the next UpdateWorldFocus sets it again when WorldFocus=1),
        // player_far <m> (the hidden live player that far east of the rendered camera;
        // use with TrailPlayerFollow=0).
        if (op == "freecam")
        {
            const bool on = !args.empty() && args[0] == "on";
            if (on)
            {
                if (!g_freeCamEnabled && !StartFreeCam(GetTickCount64()))
                {
                    detail = "freecam_failed";
                    return false;
                }
            }
            else
            {
                StopFreeCam();
            }
            LogFile() << "BENCH_FREECAM,on=" << (on ? 1 : 0) << ",enabled=" << (g_freeCamEnabled ? 1 : 0) << '\n';
            return true;
        }
        if (op == "focus")
        {
            const std::string what = args.empty() ? "log" : args[0];
            if (what == "clear")
                STREAMING::CLEAR_FOCUS();
            const Ped player = PLAYER::PLAYER_PED_ID();
            const bool playerFocus = player && ENTITY::DOES_ENTITY_EXIST(player) && STREAMING::IS_ENTITY_FOCUS(player) != FALSE;
            char text[160];
            std::snprintf(text, sizeof(text), "what=%s,world_focus=%d,source=%s,player_is_focus=%d", what.c_str(),
                g_worldFocus ? 1 : 0, replay::worldfocus::SourceName(g_worldFocusTracker.last), playerFocus ? 1 : 0);
            detail = text;
            LogFile() << "BENCH_FOCUS," << text << '\n';
            return true;
        }
        if (op == "player_far")
        {
            float meters = 400.0f;
            if (!args.empty())
                meters = static_cast<float>(std::atof(args[0].c_str()));
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            {
                detail = "no_player";
                return false;
            }
            const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const float x = cam.x + meters, y = cam.y;
            float z = cam.z;
            float ground = 0.0f;
            if (MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, cam.z + 200.0f, &ground, FALSE))
                z = ground + 1.0f;
            const Ped mount = PED::IS_PED_ON_MOUNT(player) ? PED::GET_MOUNT(player) : 0;
            replay::pose::playback::MoveHiddenEntity(mount ? mount : player, x, y, z);
            char text[160];
            std::snprintf(text, sizeof(text), "meters=%.0f,at=%.1f;%.1f;%.1f,mounted=%d", meters, x, y, z, mount ? 1 : 0);
            detail = text;
            LogFile() << "BENCH_PLAYER_FAR," << text << '\n';
            return true;
        }
        if (op == "marker_clear")
        {
            g_shakeFoldHold = 2;
            g_cameraTrack.Clear();
            g_cameraSpring.Reset();
            return true;
        }
        if (op == "cam_log")
        {
            const Vector3 p = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const Vector3 r = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
            CameraState evaluated{};
            replay::camera::SegmentPlan plan;
            const bool keyed = EvaluateCameraTrack(g_transport.PlayheadMs(), evaluated, 0.0f, &plan);
            char text[320];
            std::snprintf(text, sizeof(text),
                "playhead_ms=%llu,rendered=%.2f;%.2f;%.2f,rot=%.1f;%.1f;%.1f,fov=%.1f,keyed=%d,eval=%.2f;%.2f;%.2f,eval_rot=%.1f;%.1f;%.1f,eval_fov=%.1f,cur=%d,next=%d,blending=%d,phase=%.2f",
                static_cast<unsigned long long>(g_transport.PlayheadMs()), p.x, p.y, p.z, r.x, r.y, r.z,
                CAM::GET_FINAL_RENDERED_CAM_FOV(), keyed ? 1 : 0, evaluated.position.x, evaluated.position.y, evaluated.position.z,
                evaluated.rotation.x, evaluated.rotation.y, evaluated.rotation.z, evaluated.fov, plan.current, plan.next,
                plan.blending ? 1 : 0, plan.phase);
            detail = text;
            LogFile() << "BENCH_CAM," << text << '\n';
            return true;
        }
        // spline_default key=value...: the Spline defaults for this session (not
        // written to the ini; the bench restores the player's).
        if (op == "spline_default")
        {
            replay::editor::SplineTuning d = g_splineDefaults;
            for (const std::string& arg : args)
            {
                const std::size_t eq = arg.find('=');
                bool known = false;
                std::string error;
                if (eq == std::string::npos || !replay::camera::spline::ParseField(arg.substr(0, eq), arg.substr(eq + 1), d, known, error))
                {
                    detail = eq == std::string::npos ? "bad_arg=" + arg : known ? error : "unknown_key=" + arg.substr(0, eq);
                    return false;
                }
            }
            d.overrides = 0;
            g_splineDefaults = replay::camera::spline::Clamped(d);
            LogSplineDefaults("bench");
            return true;
        }
        // The shake events track. The events (and the legacy marker shake keys) are
        // the migration's input: they wait in the track (they play by the legacy
        // rules, the same picture) and TickShakeMigration folds them into the markers
        // once a tick passes without a marker or event step (the whole set at once: an
        // event ramps from the one before it).
        if (op == "fx_clear")
        {
            g_shakeFoldHold = 2;
            g_cameraTrack.LegacyEffects().Clear();
            return true;
        }
        if (op == "fx_add")
        {
            // "fx_add <t> kind=amp value=1.5 mode=snap"; the legacy form (shake= shake_i=
            // ...) is converted (camera_effects.h ConvertLegacy).
            namespace fx = replay::camera::fx;
            const std::uint64_t timeMs = static_cast<std::uint64_t>(std::atoll(args[0].c_str()));
            const std::vector<std::string> tokens(args.begin() + 1, args.end());
            std::string error;
            std::vector<fx::ShakeEvent> events;
            if (fx::IsLegacyLine(tokens))
            {
                fx::LegacyEvent old;
                old.timeMs = timeMs;
                if (!fx::ParseLegacyFields(tokens, old, error))
                {
                    detail = error;
                    return false;
                }
                events = fx::ConvertLegacy({old});
            }
            else
            {
                fx::ShakeEvent event;
                event.timeMs = timeMs;
                if (!fx::ParseEventFields(tokens, event, error))
                {
                    detail = error;
                    return false;
                }
                events.push_back(event);
            }
            std::uint32_t id = 0;
            for (const fx::ShakeEvent& event : events)
            {
                g_shakeFoldHold = 2;
                id = g_cameraTrack.LegacyEffects().Add(event);
                if (!id)
                    break;
                LogFile() << "CAMERA_FX,action=bench_add,id=" << id << ',' << fx::FormatEventFields(event) << '\n';
            }
            detail = "fx_id=" + std::to_string(id);
            return id != 0;
        }
        // marker_add
        g_shakeFoldHold = 2;
        replay::camera::Keyframe marker;
        marker.timeMs = static_cast<std::uint64_t>(std::atoll(args[0].c_str()));
        const replay::camera::Keyframe* previous = nullptr;
        for (const replay::camera::Keyframe& key : g_cameraTrack.Keyframes())
            if (key.timeMs <= marker.timeMs)
                previous = &key;
        if (previous)
            replay::camera::InheritFrom(marker, *previous);
        CameraState view{};
        view.fov = kMarkerFovDefault;
        if (previous)
        {
            view.position = previous->pose.position;
            view.rotation = replay::camera::Track::ToEulerDegrees(previous->pose.orientation);
            view.fov = previous->pose.fov;
        }
        bool nearSet = false, rotSet = false;
        Vec3 nearOffset{};
        for (std::size_t n = 1; n < args.size(); ++n)
        {
            const std::size_t eq = args[n].find('=');
            if (eq == std::string::npos)
            {
                detail = "bad_arg=" + args[n];
                return false;
            }
            const std::string key = args[n].substr(0, eq);
            const std::string value = args[n].substr(eq + 1);
            float a = 0, b = 0, c = 0;
            if (key == "mode")
            {
                static const std::pair<const char*, CameraMode> modes[] = {{"free", CameraMode::Free}, {"game", CameraMode::Game},
                    {"front_low", CameraMode::FrontLow}, {"rear", CameraMode::Rear}, {"right", CameraMode::RightSide},
                    {"left", CameraMode::LeftSide}, {"overhead", CameraMode::Overhead}};
                for (const auto& [name, mode] : modes)
                    if (value == name)
                        marker.cameraMode = mode;
            }
            else if (key == "pos" && std::sscanf(value.c_str(), "%f,%f,%f", &a, &b, &c) == 3)
                view.position = {a, b, c};
            else if (key == "near" && std::sscanf(value.c_str(), "%f,%f,%f", &a, &b, &c) == 3)
            {
                // World metres from the attach target (else the Look At one, else the main
                // character) at the playhead, looking at its head unless rot= is given.
                nearSet = true;
                nearOffset = {a, b, c};
            }
            else if (key == "rot" && std::sscanf(value.c_str(), "%f,%f,%f", &a, &b, &c) == 3)
            {
                view.rotation = {a, b, c};
                rotSet = true;
            }
            else if (key == "fov")
                view.fov = static_cast<float>(std::atof(value.c_str()));
            else if (key == "blend")
                marker.blend = value == "linear" ? Blend::Linear : value == "smooth" ? Blend::Smooth
                    : value == "spline" ? Blend::Spline : Blend::None;
            else if (key == "ease")
                marker.ease = value != "0";
            else if (key == "look")
                replay::camera::SetLookAt(marker, BenchTarget(value));
            else if (key == "attach")
                replay::camera::SetAttach(marker, BenchTarget(value));
            else if (key == "mount")
                replay::camera::SetAttachType(marker, value == "rigid" ? AttachType::Rigid : value == "flexible" ? AttachType::Flexible : AttachType::Default);
            else if (key == "shake")
            {
                // The legacy keys (shake=, shake_i, shake_s, the Custom numbers) fill the
                // legacy marker shake, folded like an old clip's (the scenarios play the
                // same); the new keys (shake_transition, shake_gain, shake_rate,
                // shake_realtime, shake_restart) the marker's own.
                static const char* names[] = {"none", "handheld", "drunk", "ground", "air", "explosion",
                                              "walking", "running", "horseback", "steadicam", "wagon", "custom"};
                for (int t = 0; t < 12; ++t)
                    if (value == names[t])
                        marker.legacyShake.type = static_cast<ShakeType>(t);
            }
            else if (key == "shake_transition" || key == "shake_gain" || key == "shake_rate" || key == "shake_realtime" ||
                key == "shake_restart")
            {
                bool known = false;
                std::string fieldError;
                if (!replay::camera::mshake::ParseField(key, value, marker.shake, known, fieldError))
                {
                    detail = fieldError;
                    return false;
                }
            }
            else if (key.rfind("shake_", 0) == 0 && key != "shake_i" && key != "shake_s")
            {
                // A Custom shake's numbers.
                bool known = false;
                std::string fieldError;
                replay::camera::shake::ParseCustomField(key, value, marker.legacyShake.custom, known, fieldError);
            }
            else if (key == "shake_i")
                marker.legacyShake.intensity = static_cast<std::uint8_t>(std::clamp(std::atoi(value.c_str()), 1, 10));
            else if (key == "shake_s")
                marker.legacyShake.speedIndex = static_cast<std::uint8_t>(std::clamp(std::atoi(value.c_str()), 0, 8));
            else if (key == "dof")
                marker.dof = value == "custom" ? DofMode::Custom : value == "none" ? DofMode::None : DofMode::Default;
            else if (key == "focus")
                marker.focus = value == "manual" ? FocusMode::Manual : value == "target" ? FocusMode::Target : FocusMode::Auto;
            else if (key == "focus_d")
                marker.focusDistance = static_cast<float>(std::atof(value.c_str()));
            else if (key == "focus_t")
                marker.focusTarget = BenchTarget(value);
            else if (key == "dof_i")
                marker.dofIntensity = static_cast<float>(std::atof(value.c_str()));
            else if (key == "attach_bone" || key == "look_bone" || key == "near_clip" || key == "fstop" || key == "lens_mm")
            {
                // Bench (tool only): the lens and bone fields.
                if (!BenchMarkerLensKey(marker, key, value, detail))
                    return false;
            }
            else if (key == "roll")
                marker.lookAtRoll = static_cast<float>(std::atof(value.c_str()));
            else if (key == "dist")
                marker.presetDistanceScale = static_cast<float>(std::atof(value.c_str()));
            else if (key == "speed")
                marker.speedIndex = static_cast<std::uint8_t>(std::clamp(std::atoi(value.c_str()), 0, 8));
            else if (key == "fade")
                marker.transition = value != "0" ? 1 : 0;
            else if (key == "fade_ms")
                marker.transitionMs = static_cast<std::uint16_t>(std::clamp(std::atoi(value.c_str()), 100, 10000));
            else if (key == "filter")
                marker.filter = static_cast<std::uint8_t>(std::clamp(std::atoi(value.c_str()), 0, static_cast<int>(kMarkerFilterCount) - 1));
            else if (key == "filter_i")
                marker.filterIntensity = static_cast<std::uint8_t>(std::clamp(std::atoi(value.c_str()), 0, 100));
            else if (key == "exposure")
                marker.brightness = static_cast<std::int8_t>(std::clamp(std::atoi(value.c_str()), -100, 100));
            else if (key == "contrast")
                marker.contrast = static_cast<std::int8_t>(std::clamp(std::atoi(value.c_str()), -100, 100));
            else
            {
                // The Spline keys (camera_spline.h ParseField).
                bool known = false;
                std::string fieldError;
                if (!replay::camera::spline::ParseField(key, value, marker.spline, known, fieldError))
                {
                    detail = known ? fieldError : "unknown_key=" + key;
                    return false;
                }
            }
        }
        if (nearSet)
        {
            const std::uint32_t anchor = marker.attachTarget ? marker.attachTarget
                : marker.lookAtTarget ? marker.lookAtTarget : MainCharacterId();
            replay::camera::EntityState target{};
            if (!anchor || !MarkerEntity(anchor, marker.timeMs, target))
            {
                detail = "near_target_missing";
                return false;
            }
            view.position = {target.position.x + nearOffset.x, target.position.y + nearOffset.y, target.position.z + nearOffset.z};
            if (!rotSet)
            {
                const Vec3 aim = target.hasHead ? target.head
                    : Vec3{target.position.x, target.position.y, target.position.z + replay::camera::kLookAtAimHeight};
                view.rotation = replay::camera::LookAtDegrees(view.position, aim);
            }
        }
        marker.pose = {view.position, replay::camera::Track::FromEulerDegrees(view.rotation), view.fov};
        if (!replay::camera::CanPlace(g_cameraTrack, marker.timeMs) || !replay::camera::WithinClipBounds(g_cameraTrack, marker.timeMs))
        {
            detail = "cannot_place";
            return false;
        }
        const std::uint32_t id = g_cameraTrack.AddMarker(marker);
        if (!id)
        {
            detail = "add_failed";
            return false;
        }
        if (marker.attachTarget && marker.attachBone)
            CaptureMarkerRelations(id, view); // through the bone, as "Use this view"
        else if (marker.attachTarget)
        {
            // The relative pose from the given world view at the marker's time.
            replay::camera::EntityState target{};
            if (MarkerEntity(marker.attachTarget, marker.timeMs, target))
                if (replay::camera::Keyframe* stored = g_cameraTrack.FindMutable(id))
                    replay::camera::CaptureAttachPose(*stored, view, target);
        }
        replay::camera::ApplyLastMarkerRule(g_cameraTrack);
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;
        g_cameraSpring.Reset();
        detail = "id=" + std::to_string(id) + ",look=" + std::to_string(marker.lookAtTarget) + ",attach=" + std::to_string(marker.attachTarget);
        return true;
    }
#endif // REDEMPTION_DEV

    // The editor's menu sounds (editor_protocol.h UiSound), played as the game's
    // shop menus play them. A sound asked while the script thread stood still (the
    // pause menu) is dropped, not played late.
    void PlayEditorUiSound(const replay::editor::Command& command)
    {
        constexpr ULONGLONG kLateMs = 300;
        const char* name = replay::editor::UiSoundName(static_cast<replay::editor::UiSound>(command.enabled));
        const ULONGLONG now = GetTickCount64();
        if (!name || command.timeMs > now || now - command.timeMs > kLateMs)
            return;
        AUDIO::PLAY_SOUND_FRONTEND(name, replay::editor::kUiSoundSet, TRUE, 0);
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LogFile() << "EDITOR_UI_SOUND,first=" << name << ",set=" << replay::editor::kUiSoundSet << '\n';
        }
    }

    // A speed change pushes the following markers out and could push the clip's
    // end marker past the clip: the end is held at the clip's duration.
    void HoldEndMarkerInClip()
    {
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        if (keys.size() < 2 || g_cameraTrack.LegacyBlendAnyMode() || !g_transport.Playing())
            return;
        const std::uint64_t duration = PlaybackDurationMs();
        if (keys.back().timeMs <= duration || keys[keys.size() - 2].timeMs >= duration)
            return;
        if (replay::camera::Keyframe* end = g_cameraTrack.FindMutable(keys.back().id))
        {
            LogFile() << "CAMERA_MARKER,action=end_held,from_ms=" << end->timeMs << ",to_ms=" << duration << '\n';
            end->timeMs = duration;
        }
    }

    // The limit a marker command's spacing cascade must fit in (the clip, as
    // HoldEndMarkerInClip holds it).
    std::uint64_t MarkerTimeLimit()
    {
        return g_transport.Playing() && !g_cameraTrack.LegacyBlendAnyMode() ? PlaybackDurationMs()
                                                                          : replay::camera::kNoTimeLimit;
    }

    // A clip of the folder into playback (the gallery's Enter).
    void LoadClipNow(const std::wstring& path, ULONGLONG now, ClipRead* preloaded = nullptr)
    {
        std::string error;
        if (ReadClipFile(path, error, preloaded))
            StartPlayback(now);
        else
        {
            ShowObjective(("Redemption Editor: could not load " + Narrow(path) + ": " + error).c_str());
            g_loadFailedUntil = GetTickCount64() + 5000; // the editor's alert
        }
    }

    // A load waiting for the save of the clip left.
    void ServicePendingLoad(ULONGLONG now)
    {
        if (g_pendingLoadPath.empty())
            return;
        FinishPendingSave(false);
        if (SavePending())
            return;
        // The file is read on a thread; this tick only starts it, a later one
        // publishes it. A file with a save not written yet opens from memory at once
        // (ReadClipFile).
        if (g_clipRead && g_clipRead->path != g_pendingLoadPath)
            CancelClipRead(); // another clip was asked for meanwhile
        if (!g_clipRead)
        {
            if (!g_clipSaves.Holds(g_pendingLoadPath))
            {
                StartClipRead(g_pendingLoadPath);
                return;
            }
        }
        else if (!g_clipRead->done.load())
            return;
        const std::wstring path = std::move(g_pendingLoadPath);
        g_pendingLoadPath.clear();
        std::unique_ptr<ClipRead> read = std::move(g_clipRead);
        if (read)
            read->worker.join();
        LoadClipNow(path, now, read.get());
    }

    // A seek asked by the editor (the timeline, a marker, a project clip): paused
    // there, with the drag semantics of editor_protocol.h (kSeekPlain,
    // kSeekDragging, kSeekDragEnd).
    void SeekPlayheadTo(std::uint64_t target, std::uint32_t seekKind, ULONGLONG now)
    {
        if (!g_transport.Paused())
            SetPlaybackPaused(true, now);
        // A timeline drag moves cheaply and loads once at its end.
        const replay::settle::SeekKind kind = seekKind == replay::editor::kSeekDragging ? replay::settle::SeekKind::Dragging
            : seekKind == replay::editor::kSeekDragEnd                                   ? replay::settle::SeekKind::DragEnd
                                                                                         : replay::settle::SeekKind::Plain;
        g_seekDrag.OnSeek(now, kind);
        g_seekReason = kind == replay::settle::SeekKind::Dragging ? nullptr
            : kind == replay::settle::SeekKind::DragEnd             ? "drag_end"
                                                                     : "seek";
        g_seekFine = false;
        g_transport.Seek(std::min<std::uint64_t>(target, PlaybackDurationMs()));
    }

    // ---- Projects ----
    // A montage: an ordered list of clips, each a clip file of ReplayClips with its
    // own trim and edit markers (src/replay_project.h). The script thread owns the
    // open project. A project clip plays as any clip (its file loaded through the
    // gallery's path, read on a thread), with the project's markers in place of
    // the file's; marker edits go to the project, never to the clip file. Playing
    // the project as a whole (Preview) goes on to the next clip at each clip's end
    // marker: the same file (two pieces of a cut) only moves the playhead and
    // swaps the markers; another file was read ahead while the clip before it
    // played, and is published there (the puppets are built again).
    const wchar_t* kProjectFolder = L"ReplayProjects";
    constexpr std::uint64_t kProjectPreloadLeadMs = 8000; // read the next file this long before the cut
    enum class ProjectMode
    {
        None,    // no project clip in playback
        Edit,    // one clip open in the clip editor: stops at its end marker
        Preview  // the whole project: on to the next clip at each end marker
    };
    replay::project::Project g_project;
    bool g_projectOpen = false;
    std::wstring g_projectPath;
    bool g_projectDirty = false;
    ProjectMode g_projectMode = ProjectMode::None;
    int g_projectActive = -1;             // the project clip in playback
    std::uint32_t g_projectAppliedLoad = 0; // g_clipLoadSequence when its markers went in
    std::vector<bool> g_projectMissing;   // per clip: its file is not in ReplayClips
    // A project clip whose file is being opened (the gallery's load path).
    struct ProjectPendingOpen
    {
        bool active = false;
        int index = -1;
        std::uint64_t clipMs = 0;
        bool play = false;
        std::wstring path;
    };
    ProjectPendingOpen g_projectPending;
    // Once the opened clip's puppets stand: the playhead there, played or paused.
    struct ProjectSeekAfterReady
    {
        bool active = false;
        std::uint64_t clipMs = 0;
        bool play = false;
    };
    ProjectSeekAfterReady g_projectSeekAfter;
    std::uint64_t g_projectDragMs = UINT64_MAX; // a drag over a clip of another file (no load until it ends)
    int g_projectAdvanceTo = -1; // the next clip to open (asked at a clip's end, done after the editor's commands)
    std::uint64_t g_projectSequence = 1;
    std::uint64_t g_projectListSequence = 0;
    std::vector<replay::editor::ProjectInfo> g_projectList;
    std::string g_projectMessage;
    std::uint32_t g_projectMessageSequence = 0;
    ULONGLONG g_projectSaveFailedUntil = 0;

    std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

    // A project file where it is (the base folder first, then the game
    // folder), else the base path a new one takes.
    std::wstring ProjectFilePath(const std::string& file) { return replay::storage::Find(std::wstring(kProjectFolder) + L"\\" + Widen(file)); }

    std::wstring ProjectClipPath(const replay::project::Clip& clip) { return FindClipFile(Widen(clip.file)); }

    void ProjectSay(const std::string& text)
    {
        g_projectMessage = text;
        ++g_projectMessageSequence;
        LogFile() << "PROJECT_MESSAGE,text=" << text << '\n';
    }

    void RefreshProjectMissing()
    {
        g_projectMissing.assign(g_project.clips.size(), false);
        for (std::size_t n = 0; n < g_project.clips.size(); ++n)
            g_projectMissing[n] = GetFileAttributesW(ProjectClipPath(g_project.clips[n]).c_str()) == INVALID_FILE_ATTRIBUTES;
    }

    // `files`: the clip list changed (which files exist is checked again);
    // a marker edit only touches one clip's markers.
    void ProjectChanged(bool files = true)
    {
        g_projectDirty = true;
        ++g_projectSequence;
        if (files)
            RefreshProjectMissing();
    }

    // The projects of every read root (storage_paths.h), a name in two of them once.
    void RebuildProjectList()
    {
        replay::storage::Folder(kProjectFolder);
        std::vector<std::pair<std::uint64_t, replay::editor::ProjectInfo>> found;
        std::vector<std::wstring> folders;
        for (const replay::storage::Entry& entry : replay::storage::List(kProjectFolder, L"*.rpproj", &folders))
        {
            replay::editor::ProjectInfo info{};
            const std::string file = Narrow(entry.name);
            std::snprintf(info.file, sizeof(info.file), "%s", file.c_str());
            info.modified = entry.stamp;
            replay::project::Summary summary;
            if (replay::project::ReadSummary(folders[entry.root] + L"\\" + entry.name, summary))
            {
                std::snprintf(info.name, sizeof(info.name), "%s", summary.name.c_str());
                info.totalMs = summary.totalMs;
                info.clipCount = summary.clipCount;
                info.readable = summary.readable ? 1u : 0u;
            }
            if (file.size() < sizeof(info.file))
                found.emplace_back(info.modified, info);
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        g_projectList.clear();
        for (std::size_t n = 0; n < found.size() && n < replay::editor::kMaxProjectsInSnapshot; ++n)
            g_projectList.push_back(found[n].second);
        ++g_projectListSequence;
    }

    bool SaveProject(const char* why)
    {
        if (!g_projectOpen)
            return false;
        replay::storage::Folder(kProjectFolder);
        // A project opened from a game folder that takes no writes is saved to
        // the base folder (it is listed from there from now on).
        if (!replay::storage::Current().gameWritable)
            g_projectPath = replay::storage::WritablePath(g_projectPath);
        std::string error;
        const ULONGLONG startedAt = GetTickCount64();
        const bool ok = replay::project::SaveFile(g_projectPath, g_project, error);
        LogFile() << "PROJECT_SAVE,why=" << why << ",path=" << Narrow(g_projectPath) << ",clips=" << g_project.clips.size()
                  << ",total_ms=" << replay::project::TotalMs(g_project) << ",ok=" << (ok ? 1 : 0)
                  << ",ms=" << (GetTickCount64() - startedAt) << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        if (ok)
        {
            g_projectDirty = false;
            ++g_projectSequence;
            RebuildProjectList();
        }
        else
        {
            g_projectSaveFailedUntil = GetTickCount64() + 5000;
            ProjectSay(TRF("project.msg.save_failed", "The project could not be saved: {error}.", {{"error", error}}));
        }
        return ok;
    }

    bool ProjectOwnsPlayback()
    {
        return g_projectOpen && g_projectActive >= 0 && g_projectActive < static_cast<int>(g_project.clips.size()) &&
            g_transport.Playing() && g_projectAppliedLoad == g_clipLoadSequence;
    }

    // Declared before ApplyClipEnvironment.
    std::uint64_t EnvironmentOriginMs()
    {
        if (!ProjectOwnsPlayback())
            return 0;
        return g_project.clips[static_cast<std::size_t>(g_projectActive)].inMs;
    }

    // The clip's environment settings changed in the editor: a project clip keeps
    // them in the project, a gallery clip in its file (saved with the markers:
    // Ctrl+S, the autosave on leaving).
    void EnvironmentEdited(const char* field)
    {
        const replay::EnvironmentOverride& edit = g_clip.environment.edit;
        if (ProjectOwnsPlayback())
        {
            replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(g_projectActive)];
            clip.environment = edit;
            clip.hasEnvironment = true;
            ProjectChanged(false);
        }
        else
            g_clipEdited = true;
        LogFile() << "ENV_EDIT,field=" << field << ",clock_min=" << edit.clockMinutes << ",weather=" << WeatherName(edit.weather)
                  << ",blend_s=" << edit.blendS << ",clouds_live=" << static_cast<int>(edit.cloudsLive)
                  << ",project=" << (ProjectOwnsPlayback() ? 1 : 0) << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // The markers of the clip in playback back into the project (an edit of
    // them, a move of its start or end marker: the trim follows).
    void SyncProjectMarkers()
    {
        if (!ProjectOwnsPlayback())
            return;
        replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(g_projectActive)];
        if (clip.hasMarkers && replay::camera::SameMarkers(clip.markers, g_cameraTrack))
            return;
        if (replay::project::TakeMarkers(clip, g_cameraTrack))
        {
            ProjectChanged(false);
            LogFile() << "PROJECT_MARKERS,clip=" << g_projectActive << ",markers=" << g_cameraTrack.Keyframes().size()
                      << ",in_ms=" << clip.inMs << ",out_ms=" << clip.outMs << '\n';
        }
        g_clipEdited = false; // never the clip file's
    }

    void CancelProjectPreload()
    {
        if (!g_projectPreload)
            return;
        LogFile() << "PROJECT_PRELOAD,action=drop,path=" << Narrow(g_projectPreload->path)
                  << ",done=" << (g_projectPreload->done.load() ? 1 : 0) << '\n';
        g_projectPreloadDropped.push_back(std::move(g_projectPreload));
    }

    // The dropped read aheads whose thread has ended (each tick); `wait`
    // (unload) waits for all of them.
    void ReapProjectPreloads(bool wait)
    {
        for (auto it = g_projectPreloadDropped.begin(); it != g_projectPreloadDropped.end();)
        {
            if (wait || (*it)->done.load())
            {
                (*it)->worker.join();
                it = g_projectPreloadDropped.erase(it);
            }
            else
                ++it;
        }
    }

    // The project lets go of the playback (another clip opened from the
    // gallery, the clip stopped, a recording): the clip that plays, if any,
    // is an ordinary clip from now on.
    void ProjectLeavePlayback(const char* why)
    {
        if (g_projectActive < 0 && !g_projectPending.active && g_projectMode == ProjectMode::None)
            return;
        LogFile() << "PROJECT_PLAYBACK,action=leave,why=" << why << ",clip=" << g_projectActive << '\n';
        g_projectActive = -1;
        g_projectMode = ProjectMode::None;
        g_projectPending = {};
        g_projectSeekAfter = {};
        g_projectDragMs = UINT64_MAX;
        g_projectAdvanceTo = -1;
        CancelProjectPreload();
        ++g_projectSequence;
    }

    void ProjectBeforePlaybackStop()
    {
        SyncProjectMarkers();
    }

    // The project clip's markers into the playback (its file is the clip in
    // memory). A clip never opened takes the file's own markers fitted to
    // its trim, with Game Camera bounds where it has none.
    void ApplyProjectMarkers(int index)
    {
        replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(index)];
        if (!clip.hasMarkers)
        {
            const replay::camera::Track fitted = replay::project::FitMarkers(g_cameraTrack, clip.inMs, clip.outMs,
                GameBoundMarker(clip.inMs), GameBoundMarker(clip.outMs));
            const std::size_t fileMarkers = g_cameraTrack.Keyframes().size();
            if (replay::project::TakeMarkers(clip, fitted))
                ProjectChanged(false);
            LogFile() << "PROJECT_MARKERS,clip=" << index << ",action=from_file,file_markers=" << fileMarkers
                      << ",markers=" << clip.markers.Keyframes().size() << '\n';
        }
        // The environment settings as the markers: a clip never opened takes the clip
        // file's own (ENVO), then the project's apply.
        if (!clip.hasEnvironment)
        {
            clip.environment = g_clip.environment.edit;
            clip.hasEnvironment = true;
            if (clip.environment.Any())
                ProjectChanged(false);
        }
        g_clip.environment.edit = clip.environment;
        if (g_editingMarker)
            LeaveMarkerCameraEdit(false);
        g_cameraTrack = clip.markers;
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;
        g_cameraSpring.Reset();
        g_markerHistory = {};
        g_clipEdited = false;
        g_projectActive = index;
        g_projectAppliedLoad = g_clipLoadSequence;
        ++g_projectSequence;
    }

    // Opens a project clip at its own time `clipMs`, paused or playing.
    void OpenProjectClip(int index, std::uint64_t clipMs, bool play, ProjectMode mode, ULONGLONG now)
    {
        if (!g_projectOpen || index < 0 || index >= static_cast<int>(g_project.clips.size()))
            return;
        const replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(index)];
        const std::wstring path = ProjectClipPath(clip);
        if (index < static_cast<int>(g_projectMissing.size()) && g_projectMissing[static_cast<std::size_t>(index)])
        {
            ProjectSay(TRF("project.msg.clip_gone", "The clip {file} is not in ReplayClips any more.", {{"file", clip.file}}));
            return;
        }
        if (g_recording && g_recordKind != RecordKind::Buffer)
        {
            ProjectSay(replay::keys::Fill(TR("project.msg.stop_recording", "Stop the recording first ({key_record})."), replay::keys::Current()));
            return;
        }
        clipMs = std::clamp(clipMs, clip.inMs, clip.outMs);
        SyncProjectMarkers(); // the clip left keeps its edits
        g_projectMode = mode;
        g_projectDragMs = UINT64_MAX;
        // The same file in playback (two pieces of a cut, the same clip
        // again): only the markers and the playhead change.
        if (ProjectOwnsPlayback() && !g_projectPending.active && g_currentClipPath == path)
        {
            const bool sameClip = index == g_projectActive;
            if (!sameClip)
                ApplyProjectMarkers(index);
            const std::uint64_t at = g_transport.PlayheadMs();
            const std::uint64_t gap = at > clipMs ? at - clipMs : clipMs - at;
            // A cut between two pieces in play: no seek at all (the
            // playhead is already there; a seek would hold the frame).
            if (gap > 50 || !play)
                SeekPlayheadTo(clipMs, replay::editor::kSeekPlain, now);
            SetPlaybackPaused(!play, now);
            LogFile() << "PROJECT_PLAYBACK,action=open,clip=" << index << ",same_file=1,clip_ms=" << clipMs << ",play=" << play
                      << ",mode=" << static_cast<int>(mode) << '\n';
            return;
        }
        // Another file: the gallery's load path (read on a thread, then
        // published); the read ahead is taken when it is this file.
        if (g_projectPreload && g_projectPreload->path == path)
        {
            CancelClipRead();
            g_clipRead = std::move(g_projectPreload);
            replay::clipcompress::worker::SetOpenClip(path);
        }
        else
            CancelProjectPreload();
        if (g_transport.Playing())
            StopPlayback();
        g_projectActive = -1;
        g_projectPending = {true, index, clipMs, play, path};
        g_projectSeekAfter = {};
        g_pendingLoadPath = path;
        ++g_projectSequence;
        LogFile() << "PROJECT_PLAYBACK,action=open,clip=" << index << ",same_file=0,clip_ms=" << clipMs << ",play=" << play
                  << ",mode=" << static_cast<int>(mode) << ",preloaded=" << (g_clipRead && g_clipRead->path == path ? 1 : 0) << '\n';
    }

    // ApplyClipTrim at a clip's end marker in normal play: true when the
    // project goes on to its next clip (the playback must not pause).
    bool ProjectReachedClipEnd(ULONGLONG now)
    {
        if (g_projectMode != ProjectMode::Preview || !ProjectOwnsPlayback())
            return false;
        const int next = g_projectActive + 1;
        if (next >= static_cast<int>(g_project.clips.size()))
        {
            LogFile() << "PROJECT_PLAYBACK,action=end,clips=" << g_project.clips.size() << '\n';
            return false; // the project's end: paused there
        }
        // Two pieces of one file that meet (a cut): the markers change and
        // the playback goes on without a seek, right here.
        const replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(next)];
        const std::uint64_t at = g_transport.PlayheadMs();
        // (the playhead passes the end by up to a frame times the speed)
        if (g_currentClipPath == ProjectClipPath(clip) && (at > clip.inMs ? at - clip.inMs : clip.inMs - at) <= 150)
        {
            ApplyProjectMarkers(next);
            LogFile() << "PROJECT_PLAYBACK,action=next,clip=" << next << ",same_file=1,seamless=1\n";
            return true;
        }
        // Anything else waits for the next tick, after the editor's commands
        // (a seek or a clip load inside the transport's own update would
        // break the frame it is computing): paused at the end meanwhile.
        SetPlaybackPaused(true, now);
        g_projectAdvanceTo = next;
        LogFile() << "PROJECT_PLAYBACK,action=next,clip=" << next << ",same_file=0\n";
        return true;
    }

    void CloseProject(const char* why)
    {
        if (!g_projectOpen)
            return;
        SyncProjectMarkers();
        const bool owned = ProjectOwnsPlayback() || g_projectPending.active;
        ProjectLeavePlayback(why);
        if (owned && g_transport.Playing())
            StopPlayback();
        if (owned)
            g_pendingLoadPath.clear();
        if (g_projectDirty)
            SaveProject(why); // never lost (a clip's markers are saved when left too)
        LogFile() << "PROJECT_CLOSE,why=" << why << ",path=" << Narrow(g_projectPath) << '\n';
        g_project = {};
        g_projectOpen = false;
        g_projectPath.clear();
        g_projectDirty = false;
        g_projectMissing.clear();
        ++g_projectSequence;
    }

    // A plain project file name of the folder ("Chase.rpproj").
    bool PlainProjectFile(const std::string& file)
    {
        const std::string ext = replay::project::kExtension;
        return file.size() > ext.size() && file.size() < replay::editor::kClipNameCapacity &&
            file.compare(file.size() - ext.size(), ext.size(), ext) == 0 && file.find_first_of("\\/:*?\"<>|") == std::string::npos &&
            file.find("..") == std::string::npos;
    }

    std::string CommandName(const replay::editor::Command& command)
    {
        return std::string(command.name, strnlen(command.name, sizeof(command.name)));
    }

    // The project's clip index from a command, refusing one that is not there.
    bool ProjectIndexOf(const replay::editor::Command& command, std::size_t& index)
    {
        index = command.keyframeId;
        if (!g_projectOpen || index >= g_project.clips.size())
        {
            ProjectSay(TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        return true;
    }

    // The clip in playback is the one asked for, and where its playhead is.
    bool ProjectActiveAt(std::size_t index, std::uint64_t& clipMs)
    {
        if (!ProjectOwnsPlayback() || g_projectActive != static_cast<int>(index))
        {
            ProjectSay(replay::keys::Fill(TR("project.msg.open_first", "Open that clip first ({key_select}), then put the playhead where it goes."),
                replay::keys::Current()));
            return false;
        }
        clipMs = g_transport.PlayheadMs();
        return true;
    }

    void HandleProjectCommand(replay::editor::Command& command, ULONGLONG now)
    {
        using replay::editor::CommandType;
        std::string why;
        switch (command.type)
        {
        case CommandType::ProjectNew:
        {
            std::string name = replay::project::CleanName(CommandName(command));
            replay::storage::Folder(kProjectFolder);
            if (name.empty())
            {
                for (int n = 1; n < 10000; ++n)
                {
                    name = "Project " + std::to_string(n);
                    if (GetFileAttributesW(ProjectFilePath(name + replay::project::kExtension).c_str()) == INVALID_FILE_ATTRIBUTES)
                        break;
                }
            }
            if (!replay::project::ValidName(name))
            {
                ProjectSay(TR("project.msg.name_chars", "Letters, numbers, spaces, - and _ only."));
                break;
            }
            const std::wstring path = ProjectFilePath(name + replay::project::kExtension);
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                ProjectSay(TR("project.msg.name_exists", "A project with that name exists."));
                break;
            }
            CloseProject("new");
            g_project = {};
            g_project.name = name;
            g_projectPath = path;
            g_projectOpen = true;
            g_projectDirty = false;
            g_projectMissing.clear();
            ++g_projectSequence;
            SaveProject("new");
            LogFile() << "PROJECT_NEW,name=" << name << '\n';
            break;
        }
        case CommandType::ProjectOpen:
        {
            const std::string file = CommandName(command);
            if (!PlainProjectFile(file))
                break;
            const std::wstring path = ProjectFilePath(file);
            if (g_projectOpen && path == g_projectPath)
                break; // already open
            replay::project::Project loaded;
            std::string error;
            if (!replay::project::LoadFile(path, loaded, error))
            {
                ProjectSay(TRF("project.msg.open_failed", "The project could not be opened: {error}.", {{"error", error}}));
                LogFile() << "PROJECT_OPEN,path=" << file << ",ok=0,error=" << error << '\n';
                break;
            }
            CloseProject("open_other");
            g_project = std::move(loaded);
            g_projectPath = path;
            g_projectOpen = true;
            g_projectDirty = false;
            RefreshProjectMissing();
            ++g_projectSequence;
            LogFile() << "PROJECT_OPEN,path=" << file << ",ok=1,clips=" << g_project.clips.size()
                      << ",total_ms=" << replay::project::TotalMs(g_project) << '\n';
            break;
        }
        case CommandType::ProjectSave:
            SyncProjectMarkers();
            SaveProject("editor");
            break;
        case CommandType::ProjectClose:
            CloseProject("editor");
            break;
        case CommandType::ProjectRename:
        {
            // "file|new name". The name is cleaned (CleanName: the characters a
            // file cannot hold become spaces); the project is written under its new
            // file first and the old file removed only once that worked, so a
            // failed rename leaves the project as it was. Another project's file is
            // never replaced.
            const std::string both = CommandName(command);
            const std::size_t bar = both.find('|');
            if (bar == std::string::npos)
                break;
            const std::string file = both.substr(0, bar);
            const std::string name = replay::project::CleanName(both.substr(bar + 1));
            if (!PlainProjectFile(file))
                break;
            if (!replay::project::ValidName(name))
            {
                ProjectSay(TR("project.msg.name_chars", "Letters, numbers, spaces, - and _ only."));
                break;
            }
            const std::wstring from = ProjectFilePath(file);
            if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                RebuildProjectList();
                break;
            }
            std::vector<std::string> files; // every read root's
            for (const replay::storage::Entry& entry : replay::storage::List(kProjectFolder, L"*"))
                files.push_back(Narrow(entry.name));
            const replay::project::RenameCheck check = replay::project::CheckRename(file, name, files);
            if (check == replay::project::RenameCheck::Exists)
            {
                ProjectSay(TR("project.msg.name_exists", "A project with that name exists."));
                LogFile() << "PROJECT_RENAME,from=" << file << ",to=" << name << ",ok=0,error=exists\n";
                break;
            }
            const bool open = g_projectOpen && from == g_projectPath;
            replay::project::Project project;
            std::string error;
            bool ok = true;
            if (open)
            {
                SyncProjectMarkers();
                project = g_project;
            }
            else
                ok = replay::project::LoadFile(from, project, error);
            const bool sameFile = check == replay::project::RenameCheck::Same || check == replay::project::RenameCheck::CaseOnly;
            const std::wstring to = sameFile ? from : ProjectFilePath(replay::project::NameFile(name));
            if (ok && check == replay::project::RenameCheck::Same && project.name == name)
            {
                RebuildProjectList();
                break; // nothing to change
            }
            if (ok)
            {
                project.name = name;
                ok = replay::project::SaveFile(to, project, error);
            }
            std::wstring now = to;
            if (ok && check == replay::project::RenameCheck::CaseOnly)
            {
                // The same file: only its capitals change (the content is saved).
                const std::wstring cased = ProjectFilePath(replay::project::NameFile(name));
                if (MoveFileExW(from.c_str(), cased.c_str(), 0))
                    now = cased;
            }
            else if (ok && !sameFile && !DeleteFileW(from.c_str()))
                LogFile() << "PROJECT_RENAME,old_file_kept=" << file << ",error=" << GetLastError() << '\n'; // two copies, none lost
            if (ok && open)
            {
                g_project.name = name;
                g_projectPath = now;
                g_projectDirty = false;
                ++g_projectSequence;
            }
            if (!ok)
                ProjectSay(TRF("project.msg.rename_failed", "The project could not be renamed: {error}.", {{"error", error}}));
            LogFile() << "PROJECT_RENAME,from=" << file << ",to=" << name << ",open=" << (open ? 1 : 0) << ",ok=" << (ok ? 1 : 0)
                      << ",error=" << (ok ? "none" : error.c_str()) << '\n';
            RebuildProjectList();
            break;
        }
        case CommandType::ProjectDelete:
        {
            const std::string file = CommandName(command);
            if (!PlainProjectFile(file))
                break;
            const std::wstring path = ProjectFilePath(file);
            if (g_projectOpen && path == g_projectPath)
            {
                g_projectDirty = false; // deleted, not saved back
                CloseProject("deleted");
            }
            const bool deleted = DeleteFileW(path.c_str()) != FALSE;
            LogFile() << "PROJECT_DELETE,path=" << file << ",ok=" << (deleted ? 1 : 0) << '\n';
            RebuildProjectList();
            break;
        }
        case CommandType::RefreshProjects:
            RebuildProjectList();
            break;
        case CommandType::ProjectAddClip:
        {
            if (!g_projectOpen)
                break;
            const std::string file = CommandName(command);
            std::uint64_t durationMs = 0;
            bool loadable = false;
            for (const replay::editor::ClipInfo& info : g_editorClipsByDate)
                if (file == info.name)
                {
                    durationMs = info.durationMs;
                    loadable = info.loadable != 0;
                }
            replay::project::Clip clip;
            if (!loadable || !replay::project::MakeClip(file, durationMs, clip))
            {
                ProjectSay(TR("project.msg.add_refused", "That clip cannot be added (another version, or too short)."));
                break;
            }
            const std::size_t at = command.keyframeId == ~0u ? g_project.clips.size()
                                                             : std::min<std::size_t>(command.keyframeId, g_project.clips.size());
            if (!replay::project::AddClip(g_project, at, clip, &why))
            {
                ProjectSay(why);
                break;
            }
            if (g_projectActive >= static_cast<int>(at))
                ++g_projectActive;
            if (g_projectPending.active && g_projectPending.index >= static_cast<int>(at))
                ++g_projectPending.index;
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=add,clip=" << file << ",at=" << at << ",clips=" << g_project.clips.size() << '\n';
            break;
        }
        case CommandType::ProjectMoveClip:
        {
            std::size_t from = 0;
            if (!ProjectIndexOf(command, from))
                break;
            const std::size_t to = std::min<std::size_t>(command.target, g_project.clips.size() - 1);
            SyncProjectMarkers();
            if (!replay::project::MoveClip(g_project, from, to, &why))
            {
                if (!why.empty())
                    ProjectSay(why);
                break;
            }
            const auto follow = [&](int& i) {
                if (i < 0)
                    return;
                const std::size_t at = static_cast<std::size_t>(i);
                if (at == from)
                    i = static_cast<int>(to);
                else if (from < at && at <= to)
                    --i;
                else if (to <= at && at < from)
                    ++i;
            };
            follow(g_projectActive);
            follow(g_projectPending.index);
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=move,from=" << from << ",to=" << to << '\n';
            break;
        }
        case CommandType::ProjectDuplicateClip:
        {
            std::size_t index = 0;
            if (!ProjectIndexOf(command, index))
                break;
            SyncProjectMarkers();
            if (!replay::project::DuplicateClip(g_project, index, &why))
            {
                ProjectSay(why);
                break;
            }
            if (g_projectActive > static_cast<int>(index))
                ++g_projectActive;
            if (g_projectPending.active && g_projectPending.index > static_cast<int>(index))
                ++g_projectPending.index;
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=duplicate,clip=" << index << '\n';
            break;
        }
        case CommandType::ProjectDeleteClip:
        {
            std::size_t index = 0;
            if (!ProjectIndexOf(command, index))
                break;
            const bool active = g_projectActive == static_cast<int>(index) ||
                (g_projectPending.active && g_projectPending.index == static_cast<int>(index));
            if (active)
            {
                ProjectLeavePlayback("clip_deleted");
                if (g_transport.Playing())
                {
                    g_clipEdited = false; // its markers go with it
                    StopPlayback();
                }
                g_pendingLoadPath.clear();
            }
            replay::project::DeleteClip(g_project, index, &why);
            if (g_projectActive > static_cast<int>(index))
                --g_projectActive;
            if (g_projectPending.active && g_projectPending.index > static_cast<int>(index))
                --g_projectPending.index;
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=delete,clip=" << index << ",clips=" << g_project.clips.size() << '\n';
            break;
        }
        case CommandType::ProjectSplitClip:
        {
            std::size_t index = 0;
            std::uint64_t at = 0;
            if (!ProjectIndexOf(command, index) || !ProjectActiveAt(index, at))
                break;
            SyncProjectMarkers();
            if (!replay::project::SplitClip(g_project, index, at, &why))
            {
                ProjectSay(why);
                break;
            }
            ProjectChanged();
            ApplyProjectMarkers(static_cast<int>(index)); // the first piece's markers now
            LogFile() << "PROJECT_EDIT,action=split,clip=" << index << ",at_ms=" << at << ",clips=" << g_project.clips.size() << '\n';
            break;
        }
        case CommandType::ProjectTrimClip:
        {
            std::size_t index = 0;
            std::uint64_t at = 0;
            if (!ProjectIndexOf(command, index) || !ProjectActiveAt(index, at))
                break;
            SyncProjectMarkers();
            const replay::project::Clip& clip = g_project.clips[index];
            const std::uint64_t in = command.direction < 0 ? at : clip.inMs;
            const std::uint64_t out = command.direction < 0 ? clip.outMs : at;
            if (!replay::project::SetTrim(g_project, index, in, out, &why))
            {
                if (!why.empty())
                    ProjectSay(why);
                break;
            }
            ProjectChanged();
            ApplyProjectMarkers(static_cast<int>(index));
            LogFile() << "PROJECT_EDIT,action=trim,clip=" << index << ",in_ms=" << g_project.clips[index].inMs
                      << ",out_ms=" << g_project.clips[index].outMs << '\n';
            break;
        }
        case CommandType::ProjectEditClip:
        {
            std::size_t index = 0;
            if (!ProjectIndexOf(command, index))
                break;
            const replay::project::Clip& clip = g_project.clips[index];
            const std::uint64_t at = ProjectOwnsPlayback() && g_projectActive == static_cast<int>(index) ? g_transport.PlayheadMs()
                                                                                                         : clip.inMs;
            OpenProjectClip(static_cast<int>(index), at, false, ProjectMode::Edit, now);
            break;
        }
        case CommandType::ProjectSeek:
        case CommandType::ProjectPlay:
        {
            if (!g_projectOpen || g_project.clips.empty())
                break;
            const bool play = command.type == CommandType::ProjectPlay;
            if (play && command.enabled == 0)
            {
                if (g_transport.Playing())
                    SetPlaybackPaused(true, now);
                break;
            }
            std::size_t index = 0;
            std::uint64_t clipMs = 0;
            if (play && command.timeMs == ~0ull)
            {
                // From where it is: the clip in playback goes on, or the
                // project starts over from its end, or from the start.
                if (ProjectOwnsPlayback())
                {
                    g_projectMode = ProjectMode::Preview;
                    const bool atProjectEnd = g_projectActive + 1 == static_cast<int>(g_project.clips.size()) &&
                        g_transport.PlayheadMs() + 1 >= g_project.clips.back().outMs;
                    if (!atProjectEnd)
                    {
                        SetPlaybackPaused(false, now);
                        break;
                    }
                }
                OpenProjectClip(0, g_project.clips.front().inMs, true, ProjectMode::Preview, now);
                break;
            }
            replay::project::Locate(g_project, command.timeMs, index, clipMs);
            const std::uint32_t kind = play ? replay::editor::kSeekPlain : command.enabled;
            const bool sameFile = ProjectOwnsPlayback() && !g_projectPending.active &&
                g_currentClipPath == ProjectClipPath(g_project.clips[index]);
            if (!play && kind == replay::editor::kSeekDragging)
            {
                // A drag: cheap moves over the file in memory; over another
                // file nothing loads until the drag ends.
                if (sameFile)
                {
                    g_projectDragMs = UINT64_MAX;
                    if (static_cast<int>(index) != g_projectActive)
                        ApplyProjectMarkers(static_cast<int>(index));
                    g_projectMode = ProjectMode::Preview;
                    SeekPlayheadTo(clipMs, kind, now);
                }
                else
                    g_projectDragMs = command.timeMs;
                break;
            }
            if (sameFile && !play)
            {
                g_projectDragMs = UINT64_MAX;
                if (static_cast<int>(index) != g_projectActive)
                    ApplyProjectMarkers(static_cast<int>(index));
                g_projectMode = ProjectMode::Preview;
                SeekPlayheadTo(clipMs, kind, now);
                break;
            }
            OpenProjectClip(static_cast<int>(index), clipMs, play, ProjectMode::Preview, now);
            break;
        }
        default:
            break;
        }
    }

    // Each tick after the editor's commands: the project clip that opened,
    // the seek once its puppets stand, the read ahead of the next file and
    // the markers edited back into the project.
    void UpdateProject(ULONGLONG now)
    {
        ReapProjectPreloads(false);
        if (!g_projectOpen)
            return;
        // The next clip asked for at a clip's end (ProjectReachedClipEnd).
        if (g_projectAdvanceTo >= 0)
        {
            const int next = g_projectAdvanceTo;
            g_projectAdvanceTo = -1;
            if (ProjectOwnsPlayback() && g_projectMode == ProjectMode::Preview && next < static_cast<int>(g_project.clips.size()))
                OpenProjectClip(next, g_project.clips[static_cast<std::size_t>(next)].inMs, true, ProjectMode::Preview, now);
        }
        // A project clip being opened: published (or failed) once the
        // gallery's load path has run.
        if (g_projectPending.active && g_pendingLoadPath.empty() && !g_clipRead)
        {
            const ProjectPendingOpen pending = g_projectPending;
            g_projectPending = {};
            if (g_transport.Playing() && g_currentClipPath == pending.path && pending.index >= 0 &&
                pending.index < static_cast<int>(g_project.clips.size()))
            {
                ApplyProjectMarkers(pending.index);
                g_projectSeekAfter = {true, pending.clipMs, pending.play};
            }
            else
            {
                ProjectSay(TR("project.msg.clip_open_failed", "The clip could not be opened."));
                ProjectLeavePlayback("open_failed");
            }
        }
        // The project lost the playback (stopped, another clip, a recording).
        if (g_projectActive >= 0 && !g_projectPending.active && !ProjectOwnsPlayback())
            ProjectLeavePlayback(g_transport.Playing() ? "other_clip" : "stopped");
        if (g_projectSeekAfter.active && ProjectOwnsPlayback() && g_trimReady && !replay::playback::Preparing())
        {
            const ProjectSeekAfterReady seek = g_projectSeekAfter;
            g_projectSeekAfter = {};
            const std::uint64_t at = g_transport.PlayheadMs();
            if ((at > seek.clipMs ? at - seek.clipMs : seek.clipMs - at) > 50 || !seek.play)
                SeekPlayheadTo(seek.clipMs, replay::editor::kSeekPlain, now);
            SetPlaybackPaused(!seek.play, now);
        }
        SyncProjectMarkers();
        // The next clip's file read ahead while this one plays.
        if (g_projectMode == ProjectMode::Preview && ProjectOwnsPlayback() && !g_transport.Paused() && !g_projectPreload &&
            !g_clipRead && g_projectActive + 1 < static_cast<int>(g_project.clips.size()))
        {
            const replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(g_projectActive)];
            const replay::project::Clip& next = g_project.clips[static_cast<std::size_t>(g_projectActive) + 1];
            const std::wstring nextPath = ProjectClipPath(next);
            const std::uint64_t at = g_transport.PlayheadMs();
            if (nextPath != g_currentClipPath && at + kProjectPreloadLeadMs >= clip.outMs &&
                !g_projectMissing[static_cast<std::size_t>(g_projectActive) + 1])
                g_projectPreload = BeginClipRead(nextPath, false);
        }
        // A read ahead not wanted any more (the order changed, a pause, a seek back)
        // stays until it is taken or another one is needed; one that names a clip not
        // next any more goes.
        if (g_projectPreload && g_projectPreload->done.load())
        {
            const bool wanted = ProjectOwnsPlayback() && g_projectActive + 1 < static_cast<int>(g_project.clips.size()) &&
                ProjectClipPath(g_project.clips[static_cast<std::size_t>(g_projectActive) + 1]) == g_projectPreload->path;
            if (!wanted)
                CancelProjectPreload();
        }
    }

    // ---- Whole project export ----
    // The export dialog's "Whole project" range (StartExport with enabled 1):
    // the project's clips in their order (replay_project.h ExportPlan), each
    // opened as the clip editor opens it (its markers and environment settings
    // from the project) and, once its world stands, its range exported as one
    // piece (exportprobe::RequestSegment: the editor's settings, the clip's in
    // and out, from its opening hold). The add-on keeps each piece's video and
    // sound and joins them after the last one (render_export.cpp). A cancel, a
    // failure or a clip that does not open ends it there (the add-on deletes
    // the pieces).
    constexpr std::uint32_t kExportErrorProjectClipMissing = 18; // render_export::ExportError
    constexpr std::uint32_t kExportErrorProjectClipLoad = 19;
    constexpr ULONGLONG kProjectExportOpenTimeoutMs = 120000;
    constexpr ULONGLONG kProjectExportOpenGraceMs = 2000;
    struct ProjectExportRun
    {
        enum class Step
        {
            Open,   // open the piece's clip
            Wait,   // its file read, its world and puppets standing
            Export  // its piece requested; SegmentDone moves on
        };
        bool active = false;
        std::uint32_t requestSeq = 0;
        std::vector<replay::project::ExportSegment> plan;
        std::vector<replay::projectexport::SegmentFrames> frames; // the plan's (markers' speeds)
        std::size_t index = 0;
        Step step = Step::Open;
        ULONGLONG stepAt = 0;
        std::uint64_t framesBefore = 0; // the pieces done, as encoded
        std::size_t thumbnail = 0;
    };
    ProjectExportRun g_projectExport;

    void EndProjectExport(const char* why)
    {
        if (!g_projectExport.active)
            return;
        LogFile() << "PROJECT_EXPORT,phase=end,why=" << why << ",pieces_done=" << g_projectExport.index
                  << ",of=" << g_projectExport.plan.size() << ",frames=" << g_projectExport.framesBefore << '\n';
        g_projectExport = {};
    }

    void FailProjectExport(std::uint32_t error, const char* why)
    {
        replay::exportprobe::FailProject(error, why, LogFile());
        EndProjectExport(why);
    }

    // StartExport with enabled 1 (the editor wrote the settings in the
    // channel first, as for any export).
    void StartProjectExport(std::uint32_t requestSeq, ULONGLONG now)
    {
        if (g_projectExport.active)
        {
            LogFile() << "PROJECT_EXPORT,phase=request,status=busy\n";
            return;
        }
        if (!replay::exportprobe::Enabled())
            return;
        const std::uint32_t state = replay::exportprobe::StateValue();
        const auto idle = [](std::uint32_t s) {
            using St = replay::exportchannel::State;
            return s == static_cast<std::uint32_t>(St::Off) || s == static_cast<std::uint32_t>(St::Done) ||
                s == static_cast<std::uint32_t>(St::Failed) || s == static_cast<std::uint32_t>(St::Cancelled);
        };
        if (!idle(state))
        {
            LogFile() << "PROJECT_EXPORT,phase=request,status=busy,state=" << state << '\n';
            return;
        }
        if (!g_projectOpen || g_project.clips.empty())
        {
            ProjectSay(TR("project.msg.export_empty", "Add clips to the project before exporting it."));
            replay::exportprobe::FailProject(kExportErrorProjectClipLoad, "no_project", LogFile());
            return;
        }
        SyncProjectMarkers();
        RefreshProjectMissing();
        for (std::size_t n = 0; n < g_project.clips.size(); ++n)
        {
            if (!g_projectMissing[n])
                continue;
            // Said before anything is exported (nothing to delete afterwards).
            ProjectSay(TRF("project.msg.export_missing", "The clip {file} is not in ReplayClips: the project cannot be exported.",
                {{"file", g_project.clips[n].file}}));
            replay::exportprobe::FailProject(kExportErrorProjectClipMissing, "clip_missing", LogFile());
            LogFile() << "PROJECT_EXPORT,phase=refused,clip=" << n << ",file=" << g_project.clips[n].file << '\n';
            return;
        }
        if (g_recording && g_recordKind != RecordKind::Buffer)
        {
            ProjectSay(replay::keys::Fill(TR("project.msg.stop_recording", "Stop the recording first ({key_record})."), replay::keys::Current()));
            replay::exportprobe::FailProject(kExportErrorProjectClipLoad, "recording", LogFile());
            return;
        }
        ProjectExportRun run;
        run.active = true;
        run.requestSeq = requestSeq;
        run.plan = replay::project::ExportPlan(g_project);
        std::vector<replay::projectexport::SegmentInput> inputs;
        for (const replay::project::ExportSegment& segment : run.plan)
        {
            replay::projectexport::SegmentInput input;
            input.startMs = segment.inMs;
            input.endMs = segment.outMs;
            // A clip never opened in the project takes its file's markers at
            // its first open: counted at 100 % until then.
            if (segment.ownMarkers)
                for (const replay::camera::Keyframe& key : segment.markers.Keyframes())
                    if (key.markerType != replay::editor::MarkerType::Anchor)
                        input.speeds.push_back({key.timeMs, static_cast<double>(replay::camera::SpeedFactor(key.speedIndex))});
            inputs.push_back(std::move(input));
        }
        run.frames = replay::projectexport::PlanFrames(inputs, replay::exportprobe::ExportRate());
        run.thumbnail = replay::projectexport::ThumbnailSegment(run.frames);
        run.step = ProjectExportRun::Step::Open;
        run.stepAt = now;
        replay::exportprobe::BeginProject(static_cast<std::uint32_t>(run.plan.size()), replay::projectexport::TotalFrames(run.frames), LogFile());
        LogFile() << "PROJECT_EXPORT,phase=start,clips=" << run.plan.size() << ",frames_planned=" << replay::projectexport::TotalFrames(run.frames)
                  << ",thumbnail_piece=" << run.thumbnail << ",project=" << g_project.name << '\n';
        g_projectExport = std::move(run);
    }

    // Each tick after the editor's commands and UpdateProject.
    void UpdateProjectExport(ULONGLONG now)
    {
        ProjectExportRun& run = g_projectExport;
        if (!run.active)
            return;
        using St = replay::exportchannel::State;
        const St state = static_cast<St>(replay::exportprobe::StateValue());
        if (run.step == ProjectExportRun::Step::Export)
        {
            if (state != St::SegmentDone)
            {
                if (state == St::Done || state == St::Failed || state == St::Cancelled || state == St::Off)
                {
                    if (state == St::Done)
                    {
                        run.framesBefore += replay::exportprobe::FramesEncoded();
                        ++run.index;
                    }
                    EndProjectExport(state == St::Done ? "done" : state == St::Cancelled ? "cancelled" : "failed");
                }
                return; // the piece runs
            }
            const std::uint64_t frames = replay::exportprobe::FramesEncoded();
            LogFile() << "PROJECT_EXPORT,phase=piece_done,piece=" << run.index << ",frames=" << frames
                      << ",planned=" << (run.index < run.frames.size() ? run.frames[run.index].frames : 0) << '\n';
            run.framesBefore += frames;
            ++run.index;
            replay::exportprobe::ProjectProgress(static_cast<std::uint32_t>(run.index), run.framesBefore);
            run.step = ProjectExportRun::Step::Open;
            run.stepAt = now;
            if (run.index >= run.plan.size())
            {
                FailProjectExport(kExportErrorProjectClipLoad, "past_last_piece"); // the last piece ends in Done
                return;
            }
        }
        else if (state != St::SegmentDone)
        {
            // Cancelled (or failed) between two pieces.
            EndProjectExport(state == St::Cancelled ? "cancelled" : "stopped");
            return;
        }
        if (!g_projectOpen || g_project.clips.size() != run.plan.size() || run.index >= g_project.clips.size())
        {
            FailProjectExport(kExportErrorProjectClipLoad, "project_changed");
            return;
        }
        const replay::project::Clip& clip = g_project.clips[run.index];
        if (run.step == ProjectExportRun::Step::Open)
        {
            if (run.index < g_projectMissing.size() && g_projectMissing[run.index])
            {
                ProjectSay(TRF("project.msg.export_missing", "The clip {file} is not in ReplayClips: the project cannot be exported.",
                    {{"file", clip.file}}));
                FailProjectExport(kExportErrorProjectClipMissing, "clip_missing");
                return;
            }
            LogFile() << "PROJECT_EXPORT,phase=open,piece=" << run.index << ",file=" << clip.file << ",in_ms=" << clip.inMs
                      << ",out_ms=" << clip.outMs << '\n';
            OpenProjectClip(static_cast<int>(run.index), clip.inMs, false, ProjectMode::Edit, now);
            run.step = ProjectExportRun::Step::Wait;
            run.stepAt = now;
            return;
        }
        // Wait: the clip in playback is this piece's, read, its world loaded,
        // its puppets built, the seek to its in point settled (the probe then
        // seeks to the range's start and waits for the settle again).
        const bool open = ProjectOwnsPlayback() && g_projectActive == static_cast<int>(run.index);
        const bool loading = g_projectPending.active || g_projectSeekAfter.active || ClipReadPending() || !g_pendingLoadPath.empty() ||
            ClipLoadWaiting() || replay::playback::Preparing() || !g_trimReady || g_settle.Active() || g_clipStart.Holding();
        if (!open || loading)
        {
            const ULONGLONG waited = now - run.stepAt;
            if ((!open && !g_projectPending.active && waited > kProjectExportOpenGraceMs) || waited > kProjectExportOpenTimeoutMs)
            {
                ProjectSay(TRF("project.msg.export_open_failed", "The clip {file} could not be opened: the project export stopped.",
                    {{"file", clip.file}}));
                FailProjectExport(kExportErrorProjectClipLoad, waited > kProjectExportOpenTimeoutMs ? "clip_open_timeout" : "clip_open_failed");
            }
            return;
        }
        replay::exportprobe::Segment segment;
        segment.index = static_cast<std::uint32_t>(run.index);
        segment.count = static_cast<std::uint32_t>(run.plan.size());
        segment.startMs = clip.inMs;
        segment.endMs = clip.outMs;
        segment.earliestMs = g_transport.StartMs();
        segment.framesBefore = run.framesBefore;
        for (std::size_t k = run.index + 1; k < run.frames.size(); ++k)
            segment.framesAfter += run.frames[k].frames;
        segment.thumbnail = run.index == run.thumbnail;
        if (!replay::exportprobe::RequestSegment(run.requestSeq, segment, LogFile(), ExportOutputFrames))
        {
            FailProjectExport(kExportErrorProjectClipLoad, "piece_refused");
            return;
        }
        run.step = ProjectExportRun::Step::Export;
        run.stepAt = now;
    }

    void FillProjectSnapshot(replay::editor::Snapshot& snapshot)
    {
        snapshot.projectActiveClip = ~0u;
        snapshot.projectListCount = static_cast<std::uint32_t>(g_projectList.size());
        for (std::size_t n = 0; n < g_projectList.size(); ++n)
            snapshot.projects[n] = g_projectList[n];
        snapshot.projectListSequence = g_projectListSequence;
        snapshot.projectSequence = g_projectSequence;
        snapshot.projectMessageSequence = g_projectMessageSequence;
        // Cut on a character's start (a translated message is UTF-8).
        const std::string message = g_projectMessage.substr(0, replay::i18n::Boundary(g_projectMessage, sizeof(snapshot.projectMessage) - 1));
        std::snprintf(snapshot.projectMessage, sizeof(snapshot.projectMessage), "%s", message.c_str());
        if (GetTickCount64() < g_projectSaveFailedUntil)
            snapshot.projectFlags |= replay::editor::ProjectSaveFailed;
        if (!g_projectOpen)
            return;
        snapshot.projectFlags |= replay::editor::ProjectOpenFlag;
        if (g_projectDirty)
            snapshot.projectFlags |= replay::editor::ProjectDirty;
        const bool owns = ProjectOwnsPlayback();
        if (owns && g_projectMode == ProjectMode::Preview)
            snapshot.projectFlags |= replay::editor::ProjectPreviewing;
        if (owns && g_projectMode == ProjectMode::Edit)
            snapshot.projectFlags |= replay::editor::ProjectEditingClip;
        if (g_projectPending.active || g_projectSeekAfter.active)
            snapshot.projectFlags |= replay::editor::ProjectLoadingClip;
        if (g_projectPreload && g_projectPreload->done.load() && g_projectPreload->ok)
            snapshot.projectFlags |= replay::editor::ProjectClipPreloaded;
        std::snprintf(snapshot.projectName, sizeof(snapshot.projectName), "%s", g_project.name.c_str());
        const std::wstring file = g_projectPath.substr(g_projectPath.find_last_of(L'\\') + 1);
        std::snprintf(snapshot.projectFile, sizeof(snapshot.projectFile), "%s", Narrow(file).c_str());
        snapshot.projectTotalMs = replay::project::TotalMs(g_project);
        snapshot.projectClipCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(g_project.clips.size(), replay::editor::kMaxProjectClipsInSnapshot));
        for (std::uint32_t n = 0; n < snapshot.projectClipCount; ++n)
        {
            const replay::project::Clip& clip = g_project.clips[n];
            replay::editor::ProjectClipInfo& info = snapshot.projectClips[n];
            std::snprintf(info.file, sizeof(info.file), "%s", clip.file.c_str());
            info.inMs = static_cast<std::uint32_t>(clip.inMs);
            info.outMs = static_cast<std::uint32_t>(clip.outMs);
            info.rawMs = static_cast<std::uint32_t>(clip.rawDurationMs);
            info.markerCount = static_cast<std::uint32_t>(clip.markers.Keyframes().size());
            if (n < g_projectMissing.size() && g_projectMissing[n])
                info.flags |= replay::editor::ProjectClipMissing;
            if (clip.hasMarkers)
                info.flags |= replay::editor::ProjectClipOwnMarkers;
        }
        const int shown = owns ? g_projectActive : g_projectPending.active ? g_projectPending.index : -1;
        if (shown >= 0 && shown < static_cast<int>(snapshot.projectClipCount))
        {
            snapshot.projectActiveClip = static_cast<std::uint32_t>(shown);
            snapshot.projectClips[shown].flags |= replay::editor::ProjectClipActive;
        }
        if (g_projectDragMs != UINT64_MAX)
            snapshot.projectPlayheadMs = g_projectDragMs;
        else if (owns)
            snapshot.projectPlayheadMs = replay::project::ToProjectMs(g_project, static_cast<std::size_t>(g_projectActive), g_transport.PlayheadMs());
        else if (g_projectPending.active)
            snapshot.projectPlayheadMs = replay::project::ToProjectMs(g_project, static_cast<std::size_t>(g_projectPending.index), g_projectPending.clipMs);
        // A project clip's markers changed and not saved yet: the clip
        // editor's "unsaved changes" (Ctrl+S saves the project).
        if (owns && g_projectDirty)
            snapshot.flags |= replay::editor::SnapshotClipEdited;
    }

    void ProcessEditorCommands(ULONGLONG now)
    {
        ServicePendingLoad(now);
        REPLAY_PROFILE_ZONE("tick.editor_commands");
        replay::editor::Command command{};
        std::uint32_t processed = 0;
        while (processed < 128 && replay::editor::TryPopCommand(command))
        {
            ++processed;
            // Menu sounds, one per key press, stay out of the log.
            if (command.type == replay::editor::CommandType::PlayUiSound)
            {
                PlayEditorUiSound(command);
                continue;
            }
            // The loading cue's keep-alive (every 2 s) too: logged when it starts or stops.
            if (command.type == replay::editor::CommandType::SetLoadingCue)
            {
                const bool was = EditorDrawsLoadingCue();
                g_editorLoadingCueAt = command.enabled ? std::max<ULONGLONG>(GetTickCount64(), 1) : 0;
                if (was != EditorDrawsLoadingCue())
                    LogFile() << "EDITOR_LOADING_CUE,editor_draws=" << (EditorDrawsLoadingCue() ? 1 : 0) << '\n';
                continue;
            }
            // The fields too.
            LogFile() << "EDITOR_COMMAND,type=" << static_cast<std::uint32_t>(command.type) << ",marker=" << command.keyframeId
                      << ",time_ms=" << command.timeMs << ",enabled=" << command.enabled << ",direction=" << command.direction
                      << ",mask=0x" << std::hex << command.options.mask << std::dec << '\n';
            // The commands that change the clip's markers. The markers as they were,
            // compared after the command: an undo step and "unsaved changes" only when it
            // changed them (a refused add, a move to the same time, a value at its limit
            // change nothing).
            bool markerCommand = false;
            std::uint32_t historyKind = 0;
            replay::camera::Track markersBefore;
            switch (command.type)
            {
            case replay::editor::CommandType::CaptureCameraKeyframe:
            case replay::editor::CommandType::DeleteCameraKeyframe:
            case replay::editor::CommandType::MoveCameraKeyframe:
            case replay::editor::CommandType::UpdateCameraKeyframe:
            case replay::editor::CommandType::SetCameraKeyframeOptions:
            case replay::editor::CommandType::ResetCameraKeyframe:
            case replay::editor::CommandType::PasteCameraKeyframe:
            case replay::editor::CommandType::SetCameraKeyframeMode:
            case replay::editor::CommandType::DuplicateCameraKeyframe:
            case replay::editor::CommandType::DistributeCameraKeyframes:
            case replay::editor::CommandType::StretchCameraKeyframes:
            case replay::editor::CommandType::LockCameraKeyframes:
                // (Edit Camera's keep is counted in LeaveMarkerCameraEdit.)
                if (g_transport.Playing())
                {
                    markerCommand = true;
                    // A step of the undo history; an option's runs merge per option (the mask).
                    historyKind = command.type == replay::editor::CommandType::SetCameraKeyframeOptions
                        ? 0x10000u | command.options.mask : static_cast<std::uint32_t>(command.type);
                    markersBefore = g_cameraTrack;
                }
                break;
            default:
                break;
            }
            switch (command.type)
            {
            case replay::editor::CommandType::SaveOpenClip:
                // A project's clip: Ctrl+S saves the project.
                if (ProjectOwnsPlayback())
                {
                    SyncProjectMarkers();
                    SaveProject("editor_clip");
                    break;
                }
                // Ctrl+S on a clip with its own file and no changes does not write it again (a
                // compressed clip would go back to full size); a held save of it (or of
                // another clip) is retried now, as a save asked for does. A buffer clip with
                // no file yet is saved.
                if (g_transport.Playing() && !g_clipEdited && !g_memoryClipPath.empty())
                {
                    g_clipSaves.RetryNow();
                    LogFile() << "CLIP_EDIT_SAVE,why=editor,skipped=no_changes,path=" << Narrow(g_memoryClipPath)
                              << ",held=" << g_clipSaves.HeldCount() << '\n';
                    break;
                }
                SaveEditedClip("editor");
                break;

            // The markers' undo and redo.
            case replay::editor::CommandType::UndoMarkers:
            case replay::editor::CommandType::RedoMarkers:
            {
                const bool undo = command.type == replay::editor::CommandType::UndoMarkers;
                std::vector<replay::camera::Track>& from = undo ? g_markerHistory.undo : g_markerHistory.redo;
                std::vector<replay::camera::Track>& to = undo ? g_markerHistory.redo : g_markerHistory.undo;
                if (from.empty() || !g_transport.Playing())
                    break;
                if (g_editingMarker)
                    LeaveMarkerCameraEdit(false);
                to.push_back(g_cameraTrack);
                g_cameraTrack = std::move(from.back());
                from.pop_back();
                g_markerHistory.lastType = 0;
                g_cameraTrackManualOverride = false;
                g_cameraSpring.Reset();
                g_clipEdited = true;
                LogFile() << "CAMERA_MARKER,action=" << (undo ? "undo" : "redo") << ",markers=" << g_cameraTrack.Keyframes().size()
                          << ",undo_left=" << g_markerHistory.undo.size() << ",redo_left=" << g_markerHistory.redo.size() << '\n';
                break;
            }

            case replay::editor::CommandType::ToggleRecording:
                PressRecord();
                break;

            case replay::editor::CommandType::PlayPause:
                if (!g_transport.Playing())
                    StartPlayback(now);
                else
                {
                    // Play from the end of the trimmed clip starts it over.
                    if (g_transport.Paused() && TrimActive() && g_transport.PlayheadMs() + 1 >= g_cameraTrack.Keyframes().back().timeMs)
                        g_transport.Seek(g_transport.StartMs());
                    SetPlaybackPaused(!g_transport.Paused(), now);
                }
                break;

            case replay::editor::CommandType::Stop:
                if (g_recording)
                    StopRecording();
                if (g_transport.Playing())
                    StopPlayback();
                ProjectLeavePlayback("stop");
                break;

            case replay::editor::CommandType::SeekAbsolute:
            case replay::editor::CommandType::StepFrame:
            {
                if (g_recording || PlaybackDurationMs() == 0)
                    break;
                if (!g_transport.Playing() && !StartPlayback(now))
                    break;
                SetPlaybackPaused(true, now);
                std::uint64_t target = command.timeMs;
                if (command.type == replay::editor::CommandType::StepFrame)
                {
                    const std::uint64_t at = g_transport.PlayheadMs();
                    target = command.direction < 0 ? (at > 33 ? at - 33 : 0) : at + 33;
                    // A frame step is a fine scrub (never far by the camera, only by missing
                    // models).
                    g_seekReason = "step";
                    g_seekFine = true;
                    g_seekDrag.OnSeek(now, replay::settle::SeekKind::Plain);
                }
                else
                {
                    // A timeline drag moves cheaply and loads once at its end (editor_protocol.h
                    // kSeekDragging, kSeekDragEnd); SeekPlayheadTo (the project's seeks too).
                    SeekPlayheadTo(target, command.enabled, now);
                    break;
                }
                g_transport.Seek(std::min<std::uint64_t>(target, PlaybackDurationMs()));
                break;
            }

            case replay::editor::CommandType::SetPlaybackSpeed:
            {
                const int wanted = static_cast<int>(std::min<std::uint32_t>(command.enabled,
                    static_cast<std::uint32_t>(std::size(kPlaybackSpeeds)) - 1));
                g_playbackSpeedIndex = wanted;
                LogFile() << "PLAYBACK_SPEED,percent=" << static_cast<int>(kPlaybackSpeeds[wanted] * 100.0 + 0.5)
                          << ",source=editor\n";
                break;
            }

            case replay::editor::CommandType::RefreshClips:
            {
                g_clipWindowFirst = static_cast<std::uint32_t>(command.timeMs);
                const std::uint32_t sort = command.enabled == 1 ? 1u : 0u;
                // Direction 1 is the gallery's page move: the list read before, re-sorted if
                // needed (no re-read of every clip's header on the script thread).
                if (command.direction == 1 && (!g_editorClipsByDate.empty() || ClipListLoading()))
                {
                    // A sort change re-sorts the list read (no disk).
                    if (sort != g_clipSort)
                    {
                        g_clipSort = sort;
                        ApplyClipSort();
                    }
                    else
                        ++g_editorClipListSequence;
                    break;
                }
                g_clipSort = sort;
                RebuildEditorClipList();
                break;
            }

            case replay::editor::CommandType::SetEditorOpen:
                g_editorCoversScreen = command.enabled != 0;
                break;

            // A clip renamed from the gallery (R); its thumbnail too, and the open clip's
            // paths when it is the one renamed.
            case replay::editor::CommandType::RenameClip:
            {
                char both[replay::editor::kClipNameCapacity]{};
                std::memcpy(both, command.name, sizeof(both) - 1);
                const std::string text(both);
                const std::size_t bar = text.find('|');
                if (bar == std::string::npos)
                    break;
                const std::string from = text.substr(0, bar);
                std::string to = text.substr(bar + 1);
                if (to.size() < 7 || to.substr(to.size() - 7) != ".rpclip")
                    to += ".rpclip";
                const auto plain = [](const std::string& name) {
                    return name.size() > 7 && name.find_first_of("\\/:*?\"<>|") == std::string::npos && name.find("..") == std::string::npos;
                };
                if (!plain(from) || !plain(to) || from == to)
                {
                    g_renameFailedUntil = from == to ? 0 : GetTickCount64() + 5000;
                    break;
                }
                FinishPendingSave(true); // not while its file is written
                // Renamed where it is (the base folder, or the game folder); never to a
                // name another read root already shows.
                const std::wstring fromPath = FindClipFile(std::wstring(from.begin(), from.end()));
                const std::wstring folder = fromPath.substr(0, fromPath.find_last_of(L'\\') + 1);
                const std::wstring toPath = folder + std::wstring(to.begin(), to.end());
                const bool taken = replay::storage::Exists(FindClipFile(std::wstring(to.begin(), to.end())));
                // Nor over a name a save not written yet holds (MoveFileEx refuses only a file
                // on disk).
                const bool moved = !taken && !g_clipSaves.Holds(toPath) && MoveFileExW(fromPath.c_str(), toPath.c_str(), 0) != FALSE;
                if (taken)
                    SetLastError(ERROR_ALREADY_EXISTS);
                if (moved)
                {
                    g_clipSaves.Rename(fromPath, toPath); // its held save follows it
                    // The thumbnail where the editor wrote it (the base folder, or next to the clip).
                    const auto stem = [](const std::string& clip) { return std::wstring(clip.begin(), clip.end() - 7); };
                    const std::wstring thumbFrom = FindClipFile(stem(from) + L".bmp");
                    MoveFileExW(thumbFrom.c_str(), (thumbFrom.substr(0, thumbFrom.find_last_of(L'\\') + 1) + stem(to) + L".bmp").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
                    if (g_currentClipPath == fromPath)
                        g_currentClipPath = toPath;
                    if (g_memoryClipPath == fromPath)
                    {
                        g_memoryClipPath = toPath;
                        replay::clipcompress::worker::SetOpenClip(toPath);
                    }
                    // The open project names the clip by its file.
                    if (g_projectOpen)
                    {
                        std::size_t renamed = 0;
                        for (replay::project::Clip& clip : g_project.clips)
                            if (clip.file == from)
                            {
                                clip.file = to;
                                ++renamed;
                            }
                        if (renamed)
                            ProjectChanged();
                    }
                    RebuildEditorClipList();
                }
                const DWORD renameError = moved ? 0 : GetLastError();
                // The editor is told when the name was taken or the file in use (it has
                // already gone back to the list).
                g_renameFailedUntil = moved ? 0 : GetTickCount64() + 5000;
                LogFile() << "CLIP_RENAME,from=" << from << ",to=" << to << ",ok=" << (moved ? 1 : 0) << ",error="
                          << renameError << '\n';
                break;
            }

            case replay::editor::CommandType::LoadClip:
            case replay::editor::CommandType::DeleteClip:
            {
                char name[replay::editor::kClipNameCapacity]{};
                std::memcpy(name, command.name, sizeof(name) - 1);
                const std::string text(name);
                // A plain file name of the folder, nothing else.
                if (text.empty() || text.find_first_of("\\/:") != std::string::npos || text.find("..") != std::string::npos)
                    break;
                const std::wstring path = FindClipFile(std::wstring(text.begin(), text.end()));
                if (command.type == replay::editor::CommandType::DeleteClip)
                {
                    if (g_transport.Playing() && path == g_currentClipPath)
                    {
                        g_clipEdited = false; // deleted, not saved back (the autosave brought it back)
                        StopPlayback();
                    }
                    // The player deletes it: its saves not written yet go too (one being written,
                    // once done).
                    g_clipSaves.Drop(path);
                    FinishPendingSave(true);
                    const bool deleted = DeleteFileW(path.c_str()) != FALSE;
                    // Its thumbnail (the editor writes <clip>.bmp in the base folder; an
                    // older one sits next to the clip).
                    const std::size_t dot = text.rfind(".rpclip");
                    if (deleted && dot != std::string::npos)
                        for (const std::wstring& clips : replay::storage::FolderRoots(kClipFolder))
                            DeleteFileW((clips + L"\\" + std::wstring(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(dot)) + L".bmp").c_str());
                    LogFile() << "CLIP_DELETE,path=" << text << ",ok=" << deleted << '\n';
                    RebuildEditorClipList();
                    if (g_projectOpen)
                    {
                        RefreshProjectMissing(); // its project clips say "missing"
                        ++g_projectSequence;
                    }
                    break;
                }
                // A gallery clip opened over a project's clip: the project keeps its markers
                // and lets go of the playback.
                if (g_projectActive >= 0 || g_projectPending.active)
                {
                    SyncProjectMarkers();
                    ProjectLeavePlayback("gallery");
                }
                // Leaving a clip with changes saves it on a thread: the clip stops now (its
                // save starts) and the load runs once the save is done (ServicePendingLoad),
                // the editor showing "Opening clip" meanwhile; the file is read on a thread.
                // A recording in progress is saved first, as F4 would: a manual one stops and
                // is saved as a clip; the F4 mark inside the buffer saves the window from the
                // mark to now. Both go through the save queue, and the load waits for the
                // write in flight (ServicePendingLoad), so the chosen clip opens after it.
                if (g_recording && g_recordKind == RecordKind::Manual)
                {
                    LogFile() << "CLIP_LOAD_SAVES_RECORDING,kind=manual,path=" << text << '\n';
                    StopRecording(true);
                }
                else if (g_recording && g_recordKind == RecordKind::Buffer && g_manualInBuffer)
                {
                    const std::uint32_t nowMs = replay::pose::capture::NowMs();
                    LogFile() << "CLIP_LOAD_SAVES_RECORDING,kind=buffer_mark,from_ms=" << g_manualStartMs << ",to_ms=" << nowMs
                              << ",path=" << text << '\n';
                    g_manualInBuffer = false;
                    // The chain's final save is the next part, as at the second F4.
                    g_manualPart = replay::buffercov::FinalPartNumber(g_manualPart);
                    SaveRecordingWindow(g_manualStartMs, nowMs, "manual");
                    g_manualPart = 0;
                }
                if (!g_recording || g_recordKind == RecordKind::Buffer)
                {
                    if (g_transport.Playing())
                        StopPlayback();
                    g_pendingLoadPath = path;
                    LogFile() << "CLIP_LOAD_DEFERRED,path=" << text << ",saving=" << (SavePending() ? 1 : 0) << '\n';
                    break;
                }
                LoadClipNow(path, now);
                break;
            }

            case replay::editor::CommandType::ReservedSetPlaybackDirection:
            {
                std::ofstream& log = LogFile();
                log << "EDITOR_COMMAND_REJECTED,type="
                    << static_cast<std::uint32_t>(command.type)
                    << ",reason=forward_only_transport\n";
                log.flush();
                break;
            }

            case replay::editor::CommandType::ToggleFreeCam:
                if (g_editingMarker)
                    LeaveMarkerCameraEdit(true);
                else if (g_freeCamEnabled)
                    StopFreeCam();
                else
                    StartFreeCam(now);
                break;

            case replay::editor::CommandType::CaptureCameraKeyframe:
                if (g_transport.Playing())
                    AddMarkerAtPlayhead();
                break;

            case replay::editor::CommandType::DeleteCameraKeyframe:
                // The clip's start and end markers stay (they cut the clip).
                if (replay::camera::IsClipBound(g_cameraTrack, command.keyframeId))
                {
                    replay::hud::ShowNotice(TR("hud.marker_bound_delete", "Redemption Editor: the start and end markers cut the clip; move them instead."));
                    break;
                }
                // A marker in Edit Camera is left (cancelled) before it goes; the camera goes
                // back to the track at once.
                if (g_editingMarker == command.keyframeId)
                    LeaveMarkerCameraEdit(false);
                if (g_cameraTrack.RemoveMarker(command.keyframeId))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraTrackManualOverride = false;
                    g_cameraSpring.Reset();
                    LogFile() << "CAMERA_MARKER,action=delete,id=" << command.keyframeId
                              << ",left=" << g_cameraTrack.Keyframes().size() << '\n';
                }
                break;

            case replay::editor::CommandType::MoveCameraKeyframe:
            {
                // Held between its neighbours (ClampMoveTime): CanPlace alone would let a drag
                // jump over markers and reorder them, the clip's start and end included.
                if (g_editingMarker == command.keyframeId)
                    break;
                const std::uint64_t wanted = std::min(command.timeMs, PlaybackDurationMs());
                const std::uint64_t to = replay::camera::ClampMoveTime(g_cameraTrack, command.keyframeId, wanted);
                if (replay::camera::CanPlace(g_cameraTrack, to, command.keyframeId) &&
                    g_cameraTrack.MoveMarker(command.keyframeId, to))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraTrackManualOverride = false;
                    g_cameraSpring.Reset();
                    // The playhead goes with the marker, to the time it took (not the time asked,
                    // which is off the marker when the move was clamped).
                    if (g_transport.Paused())
                        g_transport.Seek(to);
                }
                break;
            }

            case replay::editor::CommandType::SetCameraKeyframeOptions:
            {
                // The last marker's camera type is not the player's; it follows the marker
                // before it (ApplyLastMarkerRule, Plan). The editor greys the row; a type sent
                // anyway is dropped here.
                if ((command.options.mask & replay::editor::CameraOptionMode) != 0 &&
                    replay::camera::IsLastMarker(g_cameraTrack, command.keyframeId))
                {
                    command.options.mask &= ~static_cast<std::uint32_t>(replay::editor::CameraOptionMode);
                    LogFile() << "CAMERA_MARKER,action=refused,reason=last_marker_type,id=" << command.keyframeId << '\n';
                    if (command.options.mask == 0)
                        break;
                }
                // A marker that becomes a Free camera starts from the view it showed (the
                // recorded game camera, the preset around the character). Its stored pose is
                // only a placeholder (the world origin for a Game Camera marker).
                replay::camera::LivePose shown{};
                bool toFree = false;
                // A new attach target or mount type keeps the view the marker showed and stores
                // the pose relative to the target from it (the target matrix is recorded
                // again). Left as it was, the relative pose would be {0,0,0} for a marker
                // never attached (the camera inside the character) or one taken in another
                // mount's frame (a jump to another side).
                bool reattach = false;
                if (const replay::camera::Keyframe* key = g_cameraTrack.Find(command.keyframeId))
                {
                    toFree = (command.options.mask & replay::editor::CameraOptionMode) != 0 &&
                        command.options.cameraMode == replay::editor::CameraMode::Free &&
                        key->cameraMode != replay::editor::CameraMode::Free;
                    reattach = !toFree && key->cameraMode == replay::editor::CameraMode::Free &&
                        (command.options.mask & (replay::editor::CameraOptionAttachTarget | replay::editor::CameraOptionAttachType)) != 0;
                    // A new attach bone keeps the view too (the pose stored relative to the bone),
                    // unless the command sets the attach pose numbers itself.
                    const std::uint32_t poseFields = replay::editor::CameraExtraRelPosition | replay::editor::CameraExtraRelRotation;
                    if (!toFree && key->cameraMode == replay::editor::CameraMode::Free &&
                        (command.options.mask & replay::editor::CameraOptionExtra) != 0 &&
                        (command.extra.fields & replay::editor::CameraExtraAttachBone) != 0 && (command.extra.fields & poseFields) == 0)
                        reattach = true;
                    if (toFree || reattach)
                        shown = replay::camera::Resolve(*key, key->timeMs, MarkerScene());
                }
                if (g_cameraTrack.SetOptions(command.keyframeId, command.options, MarkerTimeLimit()))
                {
                    // The bones, attach pose numbers, near clip and DOF numbers (validated; a value
                    // out of range changes nothing of them and is logged).
                    if ((command.options.mask & replay::editor::CameraOptionExtra) != 0)
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                        {
                            const bool taken = replay::camera::SetMarkerExtra(*key, command.extra);
                            LogFile() << "CAMERA_MARKER,action=extra,id=" << key->id << ",fields=0x" << std::hex << command.extra.fields
                                      << ",attach_bone=0x" << key->attachBone << ",look_bone=0x" << key->lookAtBone << std::dec
                                      << ",near_clip=" << key->nearClip << ",fstop=" << key->dofFStop << ",lens_mm=" << key->dofLensMm
                                      << ",rel=" << key->relPosition.x << '/' << key->relPosition.y << '/' << key->relPosition.z
                                      << ",rel_rot=" << key->relRotation.x << '/' << key->relRotation.y << '/' << key->relRotation.z
                                      << ",taken=" << (taken ? 1 : 0) << '\n';
                        }
                    if (reattach)
                    {
                        replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId);
                        replay::camera::EntityState target{};
                        if (key && key->attachTarget && replay::camera::LookupAttach(*key, key->timeMs, MarkerScene(), target))
                        {
                            const replay::editor::CameraState view{shown.pose.position,
                                replay::camera::Track::ToEulerDegrees(shown.pose.orientation), shown.pose.fov};
                            replay::camera::CaptureAttachPose(*key, view, target);
                            LogFile() << "CAMERA_MARKER,action=attach_pose,id=" << key->id << ",target=" << key->attachTarget
                                      << ",type=" << static_cast<int>(key->attachType) << ",rel=" << key->relPosition.x << '/'
                                      << key->relPosition.y << '/' << key->relPosition.z
                                      << ",playhead_off_ms=" << static_cast<long long>(g_transport.PlayheadMs()) - static_cast<long long>(key->timeMs) << '\n';
                        }
                    }
                    if (toFree)
                    {
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                        {
                            // SetMarkerView stores the view so the marker shows it back (attach and Look
                            // At too).
                            const replay::editor::CameraState view{shown.pose.position,
                                replay::camera::Track::ToEulerDegrees(shown.pose.orientation), shown.pose.fov};
                            replay::camera::SetMarkerView(*key, view, MarkerScene());
                        }
                    }
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraSpring.Reset();
                    // The marker's whole shake.
                    if ((command.options.mask & replay::editor::CameraOptionShake) != 0)
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                        {
                            const bool set = replay::camera::SetShake(*key, command.shake);
                            LogFile() << "CAMERA_SHAKE,action=set,id=" << key->id << ",done=" << (set ? 1 : 0) << ','
                                      << replay::camera::mshake::FormatFields(key->shake) << '\n';
                        }
                    // The marker's Spline tuning.
                    if ((command.options.mask & replay::editor::CameraOptionSpline) != 0)
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                            if (replay::camera::SetSplineTuning(*key, command.spline))
                                LogFile() << "CAMERA_SPLINE,marker,id=" << key->id << ",overrides="
                                          << static_cast<int>(key->spline.overrides) << ','
                                          << replay::camera::spline::Describe(key->spline) << '\n';
                    LogFile() << "CAMERA_MARKER,action=options,id=" << command.keyframeId << ",mask=0x" << std::hex
                              << command.options.mask << std::dec << (toFree ? ",pose=shown" : "") << '\n';
                }
                break;
            }

            case replay::editor::CommandType::ResetCameraKeyframe:
                if (g_cameraTrack.ResetMarker(command.keyframeId, MarkerTimeLimit()))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraSpring.Reset();
                    LogFile() << "CAMERA_MARKER,action=reset,id=" << command.keyframeId << '\n';
                }
                break;

            case replay::editor::CommandType::CopyCameraKeyframe:
                if (const replay::camera::Keyframe* key = g_cameraTrack.Find(command.keyframeId))
                {
                    g_copiedMarker = *key;
                    g_hasCopiedMarker = true;
                    LogFile() << "CAMERA_MARKER,action=copy,id=" << key->id << '\n';
                }
                break;

            case replay::editor::CommandType::PasteCameraKeyframe:
                if (g_hasCopiedMarker && g_cameraTrack.PasteMarker(command.keyframeId, g_copiedMarker,
                                             static_cast<replay::editor::CopyContext>(command.enabled), MarkerTimeLimit()))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraSpring.Reset();
                    LogFile() << "CAMERA_MARKER,action=paste,id=" << command.keyframeId << ",context=" << command.enabled << '\n';
                }
                break;

            case replay::editor::CommandType::EditCameraKeyframe:
                if (command.enabled)
                    EnterMarkerCameraEdit(command.keyframeId, now);
                else
                    LeaveMarkerCameraEdit(command.target == 0); // target 1: cancel
                break;

            case replay::editor::CommandType::UpdateCameraKeyframe:
                if (IsValidEasing(command.easing) && IsValidPath(command.path))
                {
                    replay::editor::CameraState camera = command.camera;
                    camera.fov = std::max(10.0f, std::min(120.0f, camera.fov));
                    if (g_cameraTrack.Update(
                        command.keyframeId,
                        camera,
                        command.easing,
                        command.path))
                    {
                        CaptureMarkerRelations(command.keyframeId, camera);
                        g_cameraTrackManualOverride = false;
                    }
                }
                break;

            // Marker tools.
            case replay::editor::CommandType::DuplicateCameraKeyframe:
            {
                if (g_editingMarker)
                    break;
                const std::uint32_t added = replay::camera::DuplicateMarker(g_cameraTrack, command.keyframeId, MarkerTimeLimit());
                if (added)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                    if (const replay::camera::Keyframe* key = g_cameraTrack.Find(added))
                    {
                        if (g_transport.Paused())
                            g_transport.Seek(key->timeMs);
                    }
                }
                else
                    replay::hud::ShowNotice(TR("hud.marker_no_room", "Redemption Editor: no room for a copy there."));
                LogFile() << "CAMERA_MARKER,action=duplicate,id=" << command.keyframeId << ",added=" << added << '\n';
                break;
            }

            case replay::editor::CommandType::DistributeCameraKeyframes:
            {
                const bool byDistance = command.enabled != 0;
                const bool done = !g_editingMarker &&
                    replay::camera::DistributeMove(g_cameraTrack, command.keyframeId, byDistance, MarkerScene());
                if (done)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                }
                else
                    replay::hud::ShowNotice(TR("hud.move_three", "Redemption Editor: a move needs three markers that blend into each other."));
                LogFile() << "CAMERA_MARKER,action=distribute,id=" << command.keyframeId << ",by_distance=" << (byDistance ? 1 : 0)
                          << ",done=" << (done ? 1 : 0) << '\n';
                break;
            }

            case replay::editor::CommandType::StretchCameraKeyframes:
            {
                const bool done = !g_editingMarker &&
                    replay::camera::StretchMove(g_cameraTrack, command.keyframeId, command.enabled, MarkerTimeLimit(), MarkerScene());
                if (done)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                }
                else
                    replay::hud::ShowNotice(TR("hud.move_fit", "Redemption Editor: the move does not fit (the clip's end, or its end marker)."));
                LogFile() << "CAMERA_MARKER,action=stretch,id=" << command.keyframeId << ",percent=" << command.enabled
                          << ",done=" << (done ? 1 : 0) << '\n';
                break;
            }

            case replay::editor::CommandType::LockCameraKeyframes:
            {
                if (g_editingMarker)
                    break;
                const replay::editor::AttachType type =
                    static_cast<std::uint8_t>(command.options.attachType) <= static_cast<std::uint8_t>(replay::editor::AttachType::Flexible)
                    ? command.options.attachType : replay::editor::AttachType::Default;
                const int changed = replay::camera::LockMarkers(g_cameraTrack, command.keyframeId, command.enabled != 0,
                    command.target, type, MarkerScene());
                if (changed)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                }
                LogFile() << "CAMERA_MARKER,action=" << (command.target ? "lock" : "bake") << ",id=" << command.keyframeId
                          << ",target=" << command.target << ",type=" << static_cast<int>(type) << ",every=" << command.enabled
                          << ",changed=" << changed << '\n';
                break;
            }

            case replay::editor::CommandType::SetFreeCamOption:
            {
                const auto field = static_cast<replay::editor::FreeCamField>(command.keyframeId);
                if (SetFreeCamValue(field, command.camera.fov))
                {
                    if (field == replay::editor::FreeCamField::Motion || field == replay::editor::FreeCamField::Acrobatic)
                        g_fcState.Reset();
                    if (field != replay::editor::FreeCamField::Follow)
                        SaveFreeCamOptions();
                    LogFreeCamConfig("editor");
                }
                break;
            }

            case replay::editor::CommandType::FreeCamAction:
            {
                static const LONG kBits[] = {kFcPick, kFcPickNearest, kFcClearTarget, kFcFollow, kFcLevel, kFcBehind, kFcLock};
                if (command.keyframeId == static_cast<std::uint32_t>(replay::editor::FreeCamActionId::SetTarget))
                    SetFreeCamTarget(command.target, "editor_list");
                else if (command.keyframeId < sizeof(kBits) / sizeof(kBits[0]))
                    InterlockedOr(&g_fcActionBits, kBits[command.keyframeId]);
                break;
            }

            case replay::editor::CommandType::SetCameraOverlay:
            {
                const bool on = command.enabled != 0;
                switch (static_cast<replay::editor::OverlayField>(command.keyframeId))
                {
                case replay::editor::OverlayField::Enabled: g_camOverlayEnabled = on; break;
                case replay::editor::OverlayField::WhilePlaying: g_camOverlayWhilePlaying = on; break;
                case replay::editor::OverlayField::TimeLabels: g_camOverlayTimeLabels = on; break;
                case replay::editor::OverlayField::TargetTrail: g_camOverlayTrail = on; break;
                default: break;
                }
                SaveOverlayOptions();
                LogFreeCamConfig("overlay");
                break;
            }

            case replay::editor::CommandType::SetSplineDefaults:
            {
                // The Spline defaults (overrides ignored), saved to the ini.
                const replay::editor::SplineTuning& t = command.spline;
                if (std::isfinite(t.alpha) && std::isfinite(t.easeIn) && std::isfinite(t.easeOut) && std::isfinite(t.weight) &&
                    t.pacing < replay::editor::kSplinePacingCount)
                {
                    replay::editor::SplineTuning d = replay::camera::spline::Clamped(t);
                    d.overrides = 0;
                    g_splineDefaults = d;
                    SaveSplineDefaults();
                    LogSplineDefaults("editor");
                }
                break;
            }

            case replay::editor::CommandType::EditorUiNote:
            {
                // The editor's own note.
                char note[sizeof(command.name) + 1]{};
                std::memcpy(note, command.name, sizeof(command.name));
                for (char& ch : note)
                    if (ch == '\n' || ch == '\r')
                        ch = ' ';
                LogFile() << "CAMERA_FX_UI," << note << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
                break;
            }


            case replay::editor::CommandType::SaveBuffer:
                SaveBuffer();
                break;

            case replay::editor::CommandType::SetReplayDeadEye:
            {
                g_recConfig.replayDeadEye = command.enabled != 0;
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("ReplayDeadEye", g_recConfig.replayDeadEye ? "1" : "0",
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,replay_dead_eye=" << (g_recConfig.replayDeadEye ? 1 : 0) << ",from=editor\n";
                break;
            }

            case replay::editor::CommandType::SetReplayBuffer:
                SetReplayBuffer(command.enabled != 0, static_cast<std::uint32_t>(command.timeMs), command.keyframeId);
                break;

            // The recording gate's settings, saved to the ini.
            case replay::editor::CommandType::SetRecordCutscenes:
            case replay::editor::CommandType::SetLockCutsceneCamera:
            {
                const bool record = command.type == replay::editor::CommandType::SetRecordCutscenes;
                bool& value = record ? g_recConfig.recordCutscenes : g_recConfig.lockCutsceneCamera;
                value = command.enabled != 0;
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite(record ? "RecordCutscenes" : "LockCutsceneCamera", value ? "1" : "0",
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG," << (record ? "record_cutscenes=" : "lock_cutscene_camera=") << (value ? 1 : 0)
                          << ",from=editor\n";
                break;
            }

            // Render export: the settings are
            // in the export channel; refused unless [Export] Enabled=1.
            // enabled 1: the whole project, one clip after the other.
            case replay::editor::CommandType::StartExport:
                if (command.enabled == 1)
                    StartProjectExport(command.keyframeId, now);
                else
                    replay::exportprobe::Request(command.keyframeId, LogFile(), ExportOutputFrames, g_transport.Playing() ? g_transport.StartMs() : 0);
                break;

            case replay::editor::CommandType::CancelExport:
                replay::exportprobe::Cancel("editor", LogFile());
                break;

            // An environment setting of the clip in playback, one step (Recorded is each
            // one's first value).
            case replay::editor::CommandType::SetEnvironment:
            {
                if (!g_transport.Playing() || !g_clip.environment.recorded)
                    break;
                replay::EnvironmentOverride& edit = g_clip.environment.edit;
                const replay::EnvironmentOverride before = edit;
                const int step = command.direction < 0 ? -1 : 1;
                const char* field = "none";
                switch (static_cast<replay::editor::EnvironmentField>(command.keyframeId))
                {
                case replay::editor::EnvironmentField::Clock:
                    field = "clock";
                    edit.clockMinutes = replay::envoverride::StepClock(edit.clockMinutes,
                        RecordedEnvironmentAt(EnvironmentOriginMs()).clockS, step);
                    break;
                case replay::editor::EnvironmentField::Weather:
                    field = "weather";
                    edit.weather = replay::envoverride::StepWeather(edit.weather, step);
                    break;
                case replay::editor::EnvironmentField::Blend:
                    field = "blend";
                    edit.blendS = replay::envoverride::StepBlend(edit.blendS, step);
                    break;
                case replay::editor::EnvironmentField::Clouds:
                    field = "clouds";
                    edit.cloudsLive = edit.cloudsLive ? 0 : 1;
                    break;
                case replay::editor::EnvironmentField::ResetAll:
                    field = "reset";
                    edit = replay::EnvironmentOverride{};
                    break;
                default:
                    break;
                }
                if (edit != before)
                    EnvironmentEdited(field);
                break;
            }

            // Replay Settings' "Freeze world when paused".
            case replay::editor::CommandType::SetFreezeOnPause:
            {
                g_recConfig.freezeWorldOnPause = command.enabled != 0;
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("FreezeWorldOnPause", g_recConfig.freezeWorldOnPause ? "1" : "0",
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,freeze_world_on_pause=" << (g_recConfig.freezeWorldOnPause ? 1 : 0) << ",from=editor\n";
                break;
            }

            // Replay Settings' "Recording indicator".
            case replay::editor::CommandType::SetRecordingIndicator:
            {
                g_recConfig.recordingIndicator = replay::recindicator::FromValue(command.enabled);
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("RecordingIndicator", replay::recindicator::IniValue(g_recConfig.recordingIndicator),
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,recording_indicator=" << replay::recindicator::IniValue(g_recConfig.recordingIndicator)
                          << ",from=editor\n";
                break;
            }

            case replay::editor::CommandType::SetClipCompression:
                SetClipCompression(command.enabled != 0, command.keyframeId);
                break;

            case replay::editor::CommandType::CompressClip:
            {
                const std::string name(command.name, strnlen(command.name, sizeof(command.name)));
                replay::clipcompress::worker::RequestFile(std::wstring(name.begin(), name.end()));
                LogFile() << "CLIP_COMPRESS_REQUEST,file=" << name << ",from=editor\n";
                break;
            }

            case replay::editor::CommandType::CompressClipsNow:
                replay::clipcompress::worker::RequestPass();
                LogFile() << "CLIP_COMPRESS_REQUEST,days=" << g_recConfig.compressAfterDays << ",from=editor\n";
                break;

            case replay::editor::CommandType::SetCameraKeyframeMode:
            {
                const replay::camera::Keyframe* key = g_cameraTrack.Find(command.keyframeId);
                if (!key || static_cast<std::uint32_t>(command.mode) > 3)
                    break;
                const bool targeted = command.mode == replay::editor::CameraMode::Target ||
                    command.mode == replay::editor::CameraMode::Follow;
                const std::uint32_t target = targeted ? command.target : 0;
                g_cameraTrack.SetMode(key->id, command.mode, target, {});
                g_pendingFollowKeyframe = 0;
                if (command.mode == replay::editor::CameraMode::Follow && g_transport.Playing())
                {
                    SetPlaybackPaused(true, now);
                    g_transport.Seek(std::min<std::uint64_t>(key->timeMs, PlaybackDurationMs()));
                    g_pendingFollowKeyframe = key->id;
                }
                LogFile() << "CAMERA_KEY,action=mode,id=" << key->id << ",mode=" << static_cast<std::uint32_t>(command.mode)
                          << ",target=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(target)) << '\n';
                break;
            }

            case replay::editor::CommandType::SetCameraTrackEnabled:
                g_cameraTrackEnabled = command.enabled != 0;
                g_cameraTrackManualOverride = !g_cameraTrackEnabled;
                break;

            // Projects.
            case replay::editor::CommandType::ProjectNew:
            case replay::editor::CommandType::ProjectOpen:
            case replay::editor::CommandType::ProjectSave:
            case replay::editor::CommandType::ProjectClose:
            case replay::editor::CommandType::ProjectRename:
            case replay::editor::CommandType::ProjectDelete:
            case replay::editor::CommandType::RefreshProjects:
            case replay::editor::CommandType::ProjectAddClip:
            case replay::editor::CommandType::ProjectMoveClip:
            case replay::editor::CommandType::ProjectDuplicateClip:
            case replay::editor::CommandType::ProjectDeleteClip:
            case replay::editor::CommandType::ProjectSplitClip:
            case replay::editor::CommandType::ProjectTrimClip:
            case replay::editor::CommandType::ProjectEditClip:
            case replay::editor::CommandType::ProjectSeek:
            case replay::editor::CommandType::ProjectPlay:
                HandleProjectCommand(command, now);
                break;

            case replay::editor::CommandType::None:
            default:
                break;
            }
            if (markerCommand && !replay::camera::SameMarkers(markersBefore, g_cameraTrack))
            {
                g_clipEdited = true;
                PushMarkerHistory(historyKind, command.keyframeId, &markersBefore);
            }
            else if (markerCommand && command.type != replay::editor::CommandType::MoveCameraKeyframe)
                LogFile() << "CAMERA_MARKER,action=unchanged,type=" << static_cast<std::uint32_t>(command.type) << ",id="
                          << command.keyframeId << '\n';
        }
        if (processed > 0)
            HoldEndMarkerInClip();
    }

    void PublishEditorSnapshot()
    {
        REPLAY_PROFILE_ZONE("tick.editor_snapshot");
        // Static (the snapshot passes 130 KB with the project; the script thread's
        // stack would hold it, and a copy, every tick).
        static replay::editor::Snapshot snapshot;
        std::memset(&snapshot, 0, sizeof(snapshot));
        snapshot.flags = replay::editor::SnapshotCoreReady;
        if (g_recording) snapshot.flags |= replay::editor::SnapshotRecording;
        if (g_transport.Playing()) snapshot.flags |= replay::editor::SnapshotPlaying;
        if (g_transport.Paused()) snapshot.flags |= replay::editor::SnapshotPaused;
        if (g_transport.Scrubbing()) snapshot.flags |= replay::editor::SnapshotScrubbing;
        if (g_freeCamEnabled) snapshot.flags |= replay::editor::SnapshotFreeCamEnabled;
        if (g_debugMode) snapshot.flags |= replay::editor::SnapshotDebugMode;
        snapshot.bufferHeldMs = 0;
        snapshot.recordingMs = 0;
        if (g_recording && g_recordKind == RecordKind::Manual)
            snapshot.recordingMs = replay::pose::capture::NowMs();
        else if (g_recording && g_manualInBuffer)
            snapshot.recordingMs = replay::pose::capture::NowMs() - g_manualStartMs;
        snapshot.savedClipMs = g_savedClipMs;
        snapshot.sessionResetSequence = g_sessionResets + g_safeExitEditorCloses; // the add-on closes when it grows (session reset or safe exit)
        snapshot.editingMarker = g_editingMarker;
        if (g_editingMarker)
            snapshot.flags |= replay::editor::SnapshotEditingCamera;
        if (g_clipEdited)
            snapshot.flags |= replay::editor::SnapshotClipEdited;
        if (g_hasCopiedMarker)
            snapshot.flags |= replay::editor::SnapshotMarkerCopied;
        if (!g_markerHistory.undo.empty())
            snapshot.flags |= replay::editor::SnapshotCanUndo;
        if (!g_markerHistory.redo.empty())
            snapshot.flags |= replay::editor::SnapshotCanRedo;
        if (GetTickCount64() < g_loadFailedUntil)
            snapshot.flags |= replay::editor::SnapshotLoadFailed;
        if (GetTickCount64() < g_renameFailedUntil)
            snapshot.flags |= replay::editor::SnapshotRenameFailed;
        if (ClipListLoading())
            snapshot.flags |= replay::editor::SnapshotClipListLoading;
        if (!g_pendingLoadPath.empty())
            snapshot.flags |= replay::editor::SnapshotOpeningClip;
        snapshot.clipLoadSequence = g_clipLoadSequence;
        snapshot.gameLanguage = g_gameLanguage;
        // The seek settle; frozen: the editor holds the picture it took before the
        // jump, with a spinner.
        snapshot.seekLoadState = !g_settle.Active() ? replay::editor::SeekLoadNone
            : g_settle.Far()                        ? replay::editor::SeekLoadFrozen
                                                    : replay::editor::SeekLoadSettling;
        snapshot.seekLoadSequence = g_seekLoadSequence;
        snapshot.seekLoadFarBits = g_settle.Active() ? g_settle.FarBits() : 0;
        snapshot.seekLoadElapsedMs = g_settle.Active() ? static_cast<std::uint32_t>(g_settle.ElapsedMs(GetTickCount64())) : 0;
        snapshot.seekLoadTargetMs = g_settle.Active() ? g_settle.TargetMs() : 0;
        {
            const std::string working = Narrow(replay::clipcompress::worker::FileInWork());
            std::snprintf(snapshot.compressingClip, sizeof(snapshot.compressingClip), "%s", working.c_str());
        }
        if (g_pendingSave)
            snapshot.flags |= replay::editor::SnapshotSavingClip;
        if (g_clipSaves.HeldCount() > 0)
            snapshot.flags |= replay::editor::SnapshotSaveFailed;
        if (replay::exportprobe::ShowsExporting())
            snapshot.flags |= replay::editor::SnapshotExporting; // render export sketch
        snapshot.bufferEnabled = g_recConfig.buffer ? 1u : 0u;
        if (g_recConfig.replayDeadEye)
            snapshot.flags |= replay::editor::SnapshotReplayDeadEye;
        // The recording gate.
        if (g_recConfig.recordCutscenes)
            snapshot.flags |= replay::editor::SnapshotRecordCutscenes;
        if (g_recConfig.lockCutsceneCamera)
            snapshot.flags |= replay::editor::SnapshotLockCutsceneCamera;
        if (g_recConfig.freezeWorldOnPause)
            snapshot.flags |= replay::editor::SnapshotFreezeOnPause;
        snapshot.recordingIndicator = static_cast<std::uint32_t>(g_recConfig.recordingIndicator);
        snapshot.recordPauseReasons = g_recording && replay::pose::capture::IsClockPaused() ? g_gateReasons : 0u;
        snapshot.spanTotal = static_cast<std::uint32_t>(g_clip.spans.size());
        snapshot.spanCount = std::min(snapshot.spanTotal, replay::editor::kMaxSpansInSnapshot);
        for (std::uint32_t index = 0; index < snapshot.spanCount; ++index)
        {
            const replay::gate::Span& span = g_clip.spans[index];
            snapshot.spans[index] = {span.startMs, span.endMs, static_cast<std::uint32_t>(span.kind), 0u};
        }
        snapshot.cutCount = static_cast<std::uint32_t>(std::min<std::size_t>(g_clip.cuts.size(), replay::editor::kMaxCutsInSnapshot));
        for (std::uint32_t index = 0; index < snapshot.cutCount; ++index)
            snapshot.cuts[index] = g_clip.cuts[index];
        // The world notice of the playback start.
        {
            const replay::world::Notice& notice = replay::world::CurrentNotice();
            snapshot.worldNoticeSequence = notice.sequence;
            snapshot.worldNoticeKind = notice.kind;
            snapshot.worldChapterRecorded = notice.recordedChapter;
            snapshot.worldChapterNow = notice.currentChapter;
            snapshot.worldDifferences = notice.differences;
            snapshot.worldApplied = notice.applied;
        }
        snapshot.bufferSeconds = g_recConfig.bufferSeconds;
        snapshot.bufferMB = g_recConfig.bufferMB;
        {
            const replay::clipcompress::worker::Status compress = replay::clipcompress::worker::CurrentStatus();
            snapshot.compressEnabled = g_recConfig.compressClips ? 1u : 0u;
            snapshot.compressDays = g_recConfig.compressAfterDays;
            snapshot.compressState = static_cast<std::uint32_t>(compress.state);
            snapshot.compressDone = compress.done;
            snapshot.compressTotal = compress.total;
            snapshot.compressedClips = g_compressedClips;
            snapshot.clipsDiskBytes = g_clipsDiskBytes;
            snapshot.clipsBytes = g_clipsBytes;
        }
        if (HUD::IS_PAUSE_MENU_ACTIVE())
            snapshot.flags |= replay::editor::SnapshotGamePaused;
        if (g_recording && g_recordKind == RecordKind::Buffer)
        {
            snapshot.flags |= replay::editor::SnapshotBufferRunning;
            if (g_manualInBuffer)
                snapshot.flags |= replay::editor::SnapshotBufferManual;
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            snapshot.bufferHeldMs = nowMs > g_bufferHeadMs ? nowMs - g_bufferHeadMs : 0;
        }
        if (g_cameraTrackEnabled) snapshot.flags |= replay::editor::SnapshotCameraTrackEnabled;
        if (g_cameraTrackManualOverride)
            snapshot.flags |= replay::editor::SnapshotCameraTrackManualOverride;

        g_editorQueueOverflowed =
            g_editorQueueOverflowed || replay::editor::ConsumeQueueOverflowed();
        if (g_editorQueueOverflowed)
            snapshot.flags |= replay::editor::SnapshotCommandQueueOverflowed;

        // Nominal 30 Hz frames of the timeline, for the editor's counter.
        snapshot.playbackFrameIndex = static_cast<std::uint32_t>(g_transport.PlayheadMs() / 33u);
        snapshot.playbackFrameCount = static_cast<std::uint32_t>(PlaybackDurationMs() / 33u + 1u);
        snapshot.playheadMs = g_transport.PlayheadMs();
        snapshot.durationMs = PlaybackDurationMs();
        if (g_clip.fromDisk) snapshot.flags |= replay::editor::SnapshotClipFromDisk;
        if (replay::playback::Preparing() || ClipLoadWaiting() || g_settle.Far()) snapshot.flags |= replay::editor::SnapshotPreparing; // a far jump
        if (ClipLoadWaiting() || g_exitLoad.active) snapshot.flags |= replay::editor::SnapshotLoadingWorld; // the editor's loading cue
        snapshot.speedIndex = static_cast<std::uint32_t>(g_playbackSpeedIndex);
        snapshot.speedPercent = static_cast<std::uint32_t>(kPlaybackSpeeds[g_playbackSpeedIndex] * 100.0 + 0.5);
        snapshot.clipTotal = static_cast<std::uint32_t>(g_editorClips.size());
        {
            const std::uint32_t first = std::min(g_clipWindowFirst,
                snapshot.clipTotal > replay::editor::kMaxClipsInSnapshot ? snapshot.clipTotal - replay::editor::kMaxClipsInSnapshot : 0u);
            snapshot.clipCount = std::min<std::uint32_t>(snapshot.clipTotal - first, replay::editor::kMaxClipsInSnapshot);
            for (std::uint32_t index = 0; index < snapshot.clipCount; ++index)
                snapshot.clips[index] = g_editorClips[first + index];
        }
        snapshot.clipListSequence = g_editorClipListSequence;
        snapshot.savedClipSequence = g_savedClipSequence;
        std::snprintf(snapshot.savedClip, sizeof(snapshot.savedClip), "%s", g_savedClipName.c_str());
        if (!g_currentClipPath.empty())
        {
            const std::string current = Narrow(g_currentClipPath.substr(g_currentClipPath.find_last_of(L'\\') + 1));
            std::snprintf(snapshot.currentClip, sizeof(snapshot.currentClip), "%s", current.c_str());
        }
        snapshot.camera = CurrentEditorCameraState();
        FillProjectSnapshot(snapshot);
        // The environment settings of the clip in playback.
        snapshot.envClockMinutes = -1;
        if (g_transport.Playing())
        {
            const replay::Environment& environment = g_clip.environment;
            if (environment.recorded)
                snapshot.envFlags |= replay::editor::EnvironmentRecorded | replay::editor::EnvironmentEditable;
            if (!environment.sky.empty())
                snapshot.envFlags |= replay::editor::EnvironmentSkyTrack;
            snapshot.envClockMinutes = environment.edit.clockMinutes;
            snapshot.envWeather = environment.edit.weather;
            snapshot.envBlendS = environment.edit.blendS;
            snapshot.envCloudsLive = environment.edit.cloudsLive;
            snapshot.envRecordedStartS = RecordedEnvironmentAt(EnvironmentOriginMs()).clockS;
        }

        // The free camera's options and the overlay.
        snapshot.freeCamFlags = (g_fcSettings.acrobatic ? replay::editor::FreeCamAcrobatic : 0u) |
            (g_fcSettings.walk ? replay::editor::FreeCamWalk : 0u) | (g_fcSettings.lockAltitude ? replay::editor::FreeCamLockAltitude : 0u) |
            (g_fcSettings.collision ? replay::editor::FreeCamCollision : 0u) | (g_fcLocked ? replay::editor::FreeCamLocked : 0u) |
            (g_fcPadActive ? replay::editor::FreeCamPad : 0u) | (g_fcTargetLost ? replay::editor::FreeCamTargetLost : 0u);
        snapshot.freeCamMotion = g_fcSettings.motion == replay::freecam::Motion::Drone ? 1u : 0u;
        snapshot.freeCamFollow = static_cast<std::uint32_t>(g_fcFollow);
        snapshot.freeCamTarget = g_fcTarget;
        for (std::size_t index = 0; index < static_cast<std::size_t>(replay::editor::FreeCamField::Count); ++index)
            snapshot.freeCamValues[index] = FreeCamValue(static_cast<replay::editor::FreeCamField>(index));
        // The shake in force at the playhead (what a new marker starts from).
        snapshot.shakeAtPlayhead = replay::camera::mshake::HeldAt(g_cameraTrack, g_transport.PlayheadMs());
        snapshot.overlayFlags = (g_camOverlayEnabled ? replay::editor::OverlayEnabled : 0u) |
            (g_camOverlayWhilePlaying ? replay::editor::OverlayWhilePlaying : 0u) |
            (g_camOverlayTimeLabels ? replay::editor::OverlayTimeLabels : 0u) | (g_camOverlayTrail ? replay::editor::OverlayTargetTrail : 0u);

        const std::vector<replay::camera::Keyframe>& keyframes = g_cameraTrack.Keyframes();
        snapshot.cameraKeyframeTotal = static_cast<std::uint32_t>(keyframes.size());
        snapshot.cameraKeyframeCount = std::min(
            snapshot.cameraKeyframeTotal,
            replay::editor::kMaxCameraKeyframesInSnapshot);
        for (std::uint32_t index = 0; index < snapshot.cameraKeyframeCount; ++index)
        {
            replay::camera::FillSnapshotKeyframe(keyframes[index], snapshot.cameraKeyframes[index]);
            snapshot.cameraKeyframeShake[index] = keyframes[index].shake;
            snapshot.cameraKeyframeSpline[index] = keyframes[index].spline;
            replay::camera::FillSnapshotExtra(keyframes[index], snapshot.cameraKeyframeExtra[index]);
        }
        snapshot.cameraLive = g_cameraLive; // what the rendered camera got this tick
        snapshot.splineDefaults = g_splineDefaults;
        snapshot.splineWeightActive = SplineWeightActive() ? 1u : 0u;

        // Keyframe targets: the clip's peds, horses and vehicles, named for the player
        // (the main character by name and first, the others numbered per kind), not by
        // the replay id.
        snapshot.targetCount = 0;
        {
            static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
            const std::uint32_t main = MainCharacterId();
            int peds = 0, horses = 0, vehicles = 0, animals = 0, weapons = 0;
            // Numbered in replay id order, so a label stays with its character while
            // others come and go.
            std::vector<const replay::playback::Puppet*> ordered;
            for (const replay::playback::Puppet& puppet : g_puppets.list)
                ordered.push_back(&puppet);
            std::sort(ordered.begin(), ordered.end(), [](const replay::playback::Puppet* a, const replay::playback::Puppet* b) {
                return a->packet.id.Packed() < b->packet.id.Packed();
            });
            // A third pass lists the weapons (attach to a muzzle or a grip), after the
            // characters, horses, animals and vehicles; a ped that is not human is an
            // animal.
            for (int pass = 0; pass < 3; ++pass)
                for (const replay::playback::Puppet* entry : ordered)
                {
                    const replay::playback::Puppet& puppet = *entry;
                    if (snapshot.targetCount >= replay::editor::kMaxTargetsInSnapshot)
                        break;
                    const replay::entity::EntityKind kind = puppet.packet.kind;
                    const bool weapon = kind == replay::entity::EntityKind::Weapon;
                    if (kind != replay::entity::EntityKind::Ped && kind != replay::entity::EntityKind::Horse &&
                        kind != replay::entity::EntityKind::Vehicle && !weapon)
                        continue;
                    if (weapon != (pass == 2))
                        continue;
                    const bool isMain = puppet.packet.id.Packed() == main;
                    if (pass < 2 && (pass == 0) != isMain)
                        continue;
                    const std::uint32_t slot = snapshot.targetCount;
                    const CamTargetBones& bones = TargetBonesFor(puppet);
                    snapshot.targetFamily[slot] = bones.family;
                    snapshot.targetBoneMask[slot] = static_cast<std::uint32_t>(bones.mask);
                    snapshot.targetBoneMaskHigh[slot] = static_cast<std::uint32_t>(bones.mask >> 32);
                    snapshot.targetBoneCount[slot] = bones.count;
                    replay::editor::TargetInfo& info = snapshot.targets[snapshot.targetCount++];
                    info.id = puppet.packet.id.Packed();
                    info.kind = static_cast<std::uint32_t>(kind);
                    if (isMain)
                        std::snprintf(info.label, sizeof(info.label), "%s", puppet.packet.modelHash == arthur ? "Arthur" : "John");
                    else if (weapon)
                        std::snprintf(info.label, sizeof(info.label), "Weapon %d", ++weapons);
                    else if (kind == replay::entity::EntityKind::Ped &&
                        bones.family == static_cast<std::uint32_t>(replay::camera::bones::Family::Animal))
                        std::snprintf(info.label, sizeof(info.label), "Animal %d", ++animals);
                    else if (kind == replay::entity::EntityKind::Ped)
                        std::snprintf(info.label, sizeof(info.label), "Character %d", ++peds);
                    else if (kind == replay::entity::EntityKind::Horse)
                        std::snprintf(info.label, sizeof(info.label), "Horse %d", ++horses);
                    else
                        std::snprintf(info.label, sizeof(info.label), "Vehicle %d", ++vehicles);
                }
        }

        if (g_recording)
        {
            std::snprintf(
                snapshot.status,
                sizeof(snapshot.status),
                "REC | %u entities | %.1f s",
                static_cast<unsigned int>(PoseV2RecordedCount()),
                static_cast<double>(GetTickCount64() - g_recordingStartedAt) / 1000.0);
        }
        else if (g_transport.Playing())
        {
            std::snprintf(
                snapshot.status,
                sizeof(snapshot.status),
                "%s | %u puppets | %u keyframes",
                TransportLabel(),
                static_cast<unsigned int>(PoseV2PuppetCount()),
                snapshot.cameraKeyframeTotal);
        }
        else
        {
            std::snprintf(
                snapshot.status,
                sizeof(snapshot.status),
                "%.1f s recorded | PoseTrack v2",
                static_cast<double>(PlaybackDurationMs()) / 1000.0);
        }

        replay::editor::Publish(snapshot);
        const std::uint64_t bridgeEpoch = replay::editor::OnlineEpoch();
        if (bridgeEpoch != 0 && bridgeEpoch != g_loggedEditorBridgeEpoch)
        {
            g_loggedEditorBridgeEpoch = bridgeEpoch;
            std::ofstream& log = LogFile();
            log << "EDITOR_BRIDGE,status=online"
                << ",transport=shared_memory"
                << ",transport_version="
                << replay::editor::kSharedTransportVersion
                << ",api_version=" << replay::editor::kApiVersion
                << ",epoch=" << bridgeEpoch
                << '\n';
            log.flush();
        }
    }
}

// One line per unload step, flushed (a hang in DllMain shows where it stopped).
static void DetachStep(const char* step)
{
    LogFile() << "DETACH_STEP," << step << ",recording=" << (g_recording ? 1 : 0) << '\n';
    LogFile().flush();
}

void OnModuleDetach() noexcept
{
    if (InterlockedCompareExchange(&g_refusedStart, 0, 0) != 0)
    {
        // A refused copy started nothing; it only ends its wait.
        InterlockedExchange(&g_moduleDetaching, 1);
        return;
    }
    replay::identity::ReleaseCopyMutex();
    RemoveCrashEvidence();
    InterlockedExchange(&g_moduleDetaching, 1);
    // Every hook off in one freeze, then every detour drained, before any observer
    // removes a trampoline or frees its data. DllMain cannot refuse the unload: a
    // drain that times out is reported (log and the marker the next load reads).
    DetachStep("hooks::DisableAllForUnload");
    const MH_STATUS hooksOff = replay::hooks::DisableAllForUnload();
    const bool drained = replay::lifetime::DrainDetours(3000);
    if (!drained)
        replay::lifetime::NoteUnloadTimeout("detours");
    LogFile() << "DETACH_STEP,hooks_drained,ok=" << (drained ? 1 : 0)
              << ",disable=" << MH_StatusToString(hooksOff)
              << ",active=" << replay::lifetime::DetoursActive() << '\n';
    LogFile().flush();
    // The clip compressor runs this module's code; it stops between chunks and
    // leaves no temporary file.
    DetachStep("clipcompress::worker::StopForUnload");
    replay::clipcompress::worker::StopForUnload();
    // A clip write still running this module's code: wait for its flag, not with
    // join() (DllMain holds the loader lock a thread's exit needs). The clip list
    // reader stops between files on the flag. Both are std::thread, whose CRT start
    // takes a reference on this module and ends in FreeLibraryAndExitThread: while
    // one runs, Ctrl+R's FreeLibrary does not reach DllMain (the unload waits for
    // the thread; ScriptHook logs it when it happens). So here they are already
    // done and these waits return at once; kept as a guard. The CreateThread
    // workers (compressor, pose) are waited for out of this image in their own
    // StopForUnload/Shutdown.
    if (g_clipListRead && g_clipListRead->worker.joinable())
    {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!g_clipListRead->done.load() && GetTickCount64() < deadline)
            Sleep(1);
        g_clipListRead->worker.detach();
    }
    // The report's copies (a std::thread too, stops between chunks on the flag).
    // The clip compression and the zip stop on the QA kit's cancel flag.
    replay::qa::kit::Shutdown();
    if (g_qaReport && g_qaReport->worker.joinable())
    {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!g_qaReport->done.load() && GetTickCount64() < deadline)
            Sleep(1);
        g_qaReport->worker.detach();
    }
    if (g_pendingSave && g_pendingSave->worker.joinable())
    {
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (!g_pendingSave->done.load() && GetTickCount64() < deadline)
            Sleep(1);
        g_pendingSave->worker.detach();
        Sleep(20);
    }
    DetachStep("weapon::ShutdownStrapVisibility");
    replay::weapon::ShutdownStrapVisibility();
    DetachStep("weapon::ShutdownWeaponPassPose");
    replay::weapon::ShutdownWeaponPassPose();
    DetachStep("reins::Shutdown");
    replay::reins::Shutdown();
    DetachStep("animlod::Shutdown");
    replay::animlod::Shutdown();
    DetachStep("muscles::Shutdown");
    replay::muscles::Shutdown();
    DetachStep("entityquery::Shutdown");
    replay::entityquery::Shutdown();
    DetachStep("pedaudiostate::ShutdownReadProbe");
    replay::pedaudiostate::ShutdownReadProbe();
    DetachStep("pedaudiostate::ShutdownSpeedOverride");
    replay::pedaudiostate::ShutdownSpeedOverride();
    DetachStep("pedaudiostate::ShutdownMotionSpeed");
    replay::pedaudiostate::ShutdownMotionSpeed();
    // The read probes' final report, before the observers it rides go (every
    // detour is drained by now).
    DetachStep("soundfx::Shutdown");
    replay::soundfx::Shutdown();
    DetachStep("soundgeneric::Shutdown"); // after the hooks that push into its rings
    replay::soundgeneric::Shutdown();
    DetachStep("rendertarget::Shutdown"); // before the capture probe (it reports C29 and C30 there)
    replay::rendertarget::Shutdown();
    DetachStep("captureprobe::Shutdown");
    replay::captureprobe::Shutdown();
    DetachStep("audio::Shutdown");
    replay::audio::Shutdown();
    DetachStep("census::Shutdown");
    replay::census::Shutdown();
    DetachStep("fx::Shutdown");
    replay::fx::Shutdown();
    DetachStep("decal::Shutdown");
    replay::decal::Shutdown();
    DetachStep("gore::Shutdown");
    replay::gore::Shutdown();
    DetachStep("blood::Shutdown");
    replay::blood::Shutdown();
    DetachStep("vegetation::Shutdown");
    replay::vegetation::Shutdown();
    DetachStep("windreplay::Shutdown");
    replay::windreplay::Shutdown();
    DetachStep("catstate::Shutdown");
    replay::catstate::Shutdown();
    DetachStep("speechduck::Shutdown");
    replay::speechduck::Shutdown();
    // An unload while playing never runs StopPlayback; the microphone hash +0x780
    // and the bit 0x100 the replay owns go back here (guarded memory, no natives).
    DetachStep("gamelistener::Release");
    replay::gamelistener::Release("unload", LogFile());
    DetachStep("frag::Shutdown");
    replay::frag::Shutdown();
    DetachStep("theatre::Shutdown");
    replay::theatre::Shutdown();
    DetachStep("exportprobe::Shutdown");
    replay::exportprobe::Shutdown(); // render export sketch: timer released, channel cancelled
    DetachStep("moon give back");
    GiveBackMoonAtUnload();
    DetachStep("water::Shutdown");
    replay::water::Shutdown();
    DetachStep("fire::Shutdown");
    replay::fire::Shutdown();
    DetachStep("pose::capture::Shutdown");
    replay::pose::capture::Shutdown();
    DetachStep("mount::Shutdown"); // the ped pre-render detour
    replay::mount::Shutdown();
    DetachStep("editor::BeginShutdown");
    replay::editor::BeginShutdown();
    // What did not leave this image in time. The unload goes on regardless
    // (DllMain cannot stop it); the next load reports the marker (UNLOAD_PREVIOUS)
    // so a crash after it is explained.
    const std::int32_t timeouts = replay::lifetime::UnloadTimeouts();
    // What the observer rings' protocol saw in this load (event_ring.h).
    LogFile() << "RING_PROTOCOL,writer_dropped=" << replay::ring::g_counters.writerDropped.load()
              << ",torn=" << replay::ring::g_counters.torn.load() << ",lost=" << replay::ring::g_counters.lost.load()
              << ",waited=" << replay::ring::g_counters.waited.load() << '\n';
    LogFile() << "DETACH_STEP,done,recording=" << (g_recording ? 1 : 0)
              << ",unload_timeouts=" << timeouts
              << ",last_timeout=" << replay::lifetime::LastUnloadTimeout() << '\n';
    LogFile().flush();
    // MH_Initialize paired here, or each hot reload would leave MinHook's private
    // heap and trampoline pages behind. Freed only on a clean unload: every detour
    // drained and removed by its owner above, no wait timed out (a trampoline
    // still running would be freed under it).
    if (timeouts == 0 && drained)
    {
        const MH_STATUS uninit = MH_Uninitialize();
        LogFile() << "DETACH_STEP,mh_uninitialize,status=" << MH_StatusToString(uninit) << '\n';
        LogFile().flush();
    }
    if (timeouts > 0)
    {
        if (FILE* file = _wfopen(StateFile(kUnloadReportFile).c_str(), L"w"))
        {
            std::fprintf(file, "unload_timeouts=%d,last=%s\n", static_cast<int>(timeouts),
                replay::lifetime::LastUnloadTimeout());
            std::fclose(file);
        }
    }
}

void OnModuleDetachComplete() noexcept
{
    if (InterlockedCompareExchange(&g_refusedStart, 0, 0) != 0)
        return;
    replay::editor::FinishShutdown();
}

void OnKeyboardMessage(
    DWORD key,
    WORD,
    BYTE,
    BOOL,
    BOOL,
    BOOL wasDownBefore,
    BOOL isUpNow)
{
    if (InterlockedCompareExchange(
            &g_moduleDetaching,
            0,
            0) != 0 ||
        InterlockedCompareExchange(&g_refusedStart, 0, 0) != 0)
    {
        return;
    }

    const LONG down = isUpNow ? 0 : 1;
    switch (key)
    {
    case 'W': InterlockedExchange(&g_moveForward, down); break;
    case 'S': InterlockedExchange(&g_moveBackward, down); break;
    case 'A': InterlockedExchange(&g_moveLeft, down); break;
    case 'D': InterlockedExchange(&g_moveRight, down); break;
    case 'E': InterlockedExchange(&g_moveUp, down); break;
    case 'Q': InterlockedExchange(&g_moveDown, down); break;
    case 'Z': InterlockedExchange(&g_rollLeft, down); break;
    case 'C': InterlockedExchange(&g_rollRight, down); break;
    case VK_LEFT: InterlockedExchange(&g_turnLeft, down); break;
    case VK_RIGHT: InterlockedExchange(&g_turnRight, down); break;
    case VK_UP: InterlockedExchange(&g_turnUp, down); break;
    case VK_DOWN: InterlockedExchange(&g_turnDown, down); break;
    case VK_PRIOR: InterlockedExchange(&g_fovIn, down); break;
    case VK_NEXT: InterlockedExchange(&g_fovOut, down); break;
    case VK_SHIFT: InterlockedExchange(&g_fastMove, down); break;
    case VK_CONTROL: InterlockedExchange(&g_slowMove, down); break;
    }

    // The actions come from the [Keys] table (editor_keys.h), the same one the
    // add-on loads; the free camera's keys above stay fixed.
    using replay::keys::Action;
    const replay::keys::Table& keys = replay::keys::Current();
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    // The modifiers as the keyboard has them now, not g_fastMove: a Shift
    // release the game never saw (Shift+Tab opens the Steam overlay, Alt+Tab)
    // can leave g_fastMove at 1, and F2 alone (the editor) then read as
    // Shift+F2 and made a problem mark (seen in QA). A stuck g_fastMove
    // is let go here too, so the free camera stops flying fast.
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!shift && key != VK_SHIFT && InterlockedCompareExchange(&g_fastMove, 0, 0) != 0)
        InterlockedExchange(&g_fastMove, 0);

    // The scrub keys while held, with the editor closed (the open editor
    // steps and seeks itself): FrameBack / FrameForward, and Rewind /
    // FastForward (none by default). A release lets go whatever the modifiers.
    if (g_editorCoversScreen)
    {
        InterlockedExchange(&g_scrubBackHeld, 0);
        InterlockedExchange(&g_scrubForwardHeld, 0);
    }
    else
    {
        const bool frameBack = replay::keys::UsesKey(keys, Action::FrameBack, key);
        const bool rewind = replay::keys::UsesKey(keys, Action::Rewind, key);
        if (frameBack || rewind)
        {
            const bool held = down &&
                ((rewind && replay::keys::Matches(keys, Action::Rewind, key, ctrl, shift)) ||
                    (frameBack && replay::keys::Matches(keys, Action::FrameBack, key, ctrl, shift)));
            InterlockedExchange(&g_scrubBackHeld, held ? 1 : 0);
        }
        const bool frameForward = replay::keys::UsesKey(keys, Action::FrameForward, key);
        const bool fastForward = replay::keys::UsesKey(keys, Action::FastForward, key);
        if (frameForward || fastForward)
        {
            const bool held = down &&
                ((fastForward && replay::keys::Matches(keys, Action::FastForward, key, ctrl, shift)) ||
                    (frameForward && replay::keys::Matches(keys, Action::FrameForward, key, ctrl, shift)));
            InterlockedExchange(&g_scrubForwardHeld, held ? 1 : 0);
        }
    }

    if (wasDownBefore || isUpNow)
        return;

    // ScriptHook's reload key (see g_reloadKeyAt).
    if (key == 'R' && ctrl)
        InterlockedExchange64(&g_reloadKeyAt, static_cast<LONG64>(GetTickCount64()));

    const Action action = replay::keys::Find(keys, key, ctrl, shift);
    // The open editor sends its own commands for these keys; handling them
    // here too would toggle twice in one tick (F11 on, then off again).
    if (g_editorCoversScreen &&
        (action == Action::PlayPause || action == Action::FreeCam || action == Action::SpeedDown ||
            action == Action::SpeedUp || (replay::identity::kDevBuild && key == VK_F8) || key == VK_INSERT || key == VK_DELETE ||
            key == VK_END))
        return;

    switch (action)
    {
    case Action::Record: InterlockedExchange(&g_toggleRecordingRequested, 1); return;
    case Action::SaveBuffer: InterlockedExchange(&g_saveBufferRequested, 1); return;
    case Action::Stop: InterlockedExchange(&g_togglePlaybackRequested, 1); return;
    case Action::PlayPause: InterlockedExchange(&g_togglePlayheadPauseRequested, 1); return;
    case Action::FreeCam: InterlockedExchange(&g_toggleFreeCamRequested, 1); return;
    case Action::SpeedDown: InterlockedExchange(&g_speedStepRequested, -1); return;
    case Action::SpeedUp: InterlockedExchange(&g_speedStepRequested, 1); return;
    // A key saves a report (ReplayReports\): Shift+F1. Not Shift+Tab (Steam
    // overlay), Home (Social Club, ReShade), F6 (the game's Photo Mode, any
    // modifier) or Ctrl (the player crouches).
    case Action::Report:
        if (!g_editorCoversScreen)
            InterlockedExchange(&g_qaReportRequested, 1); // with the editor up the add-on sends it
        return;
    // Shift+F2 marks a problem during a playback (F2 alone is the editor's; the
    // add-on ignores Shift+F2).
    case Action::MarkProblem:
        if (!g_editorCoversScreen)
            replay::qa::kit::RequestMark(); // with the editor up the add-on sends it (qa_shared.h)
        return;
    default:
        break; // FrameBack, FrameForward, Rewind, FastForward (held, above); HideUi, Editor (the add-on's)
    }

    // The free camera's modes and target, and the markers in the world. Never
    // while the editor has the keyboard (a clip name typed there); the free
    // camera's only while it flies.
    if (!g_editorCoversScreen)
    {
        LONG bit = 0;
        switch (action)
        {
        case Action::FreeCamDrone: bit = kFcDrone; break;
        case Action::FreeCamAcrobatic: bit = kFcAcrobatic; break;
        case Action::FreeCamWalk: bit = kFcWalk; break;
        case Action::FreeCamCollision: bit = kFcCollision; break;
        case Action::FreeCamLockAltitude: bit = kFcLockAltitude; break;
        case Action::FreeCamFollow: bit = kFcFollow; break;
        case Action::FreeCamPick: bit = kFcPick; break;
        case Action::FreeCamPickNearest: bit = kFcPickNearest; break;
        case Action::FreeCamLevel: bit = kFcLevel; break;
        case Action::FreeCamBehind: bit = kFcBehind; break;
        case Action::FreeCamLock: bit = kFcLock; break;
        case Action::CameraOverlay: bit = kOverlayToggle; break;
        default: break;
        }
        if (bit == kOverlayToggle || (bit && g_freeCamEnabled))
        {
            InterlockedOr(&g_fcActionBits, bit);
            return;
        }
    }

    // Fixed keys (editor_keys.h kReserved: core_fixed). F9 and F8 drive the
    // debug HUD: the developer build only.
    if (replay::identity::kDevBuild && key == VK_F9)
        InterlockedExchange(&g_toggleOverlayRequested, 1);
    else if (replay::identity::kDevBuild && key == VK_F8)
        InterlockedExchange(&g_loadNextClipRequested, 1);
    else if (key == VK_INSERT)
        InterlockedExchange(&g_cameraKeyAddRequested, 1);
    else if (key == VK_DELETE)
        InterlockedExchange(&g_cameraKeyDeleteRequested, 1);
    else if (key == VK_END) // Home opens the Rockstar overlay
        InterlockedExchange(&g_cameraTrackToggleRequested, 1);
}

namespace
{
    // Profiler report window: PROFILE_* lines every 5 s of wall time, in
    // every mode (idle shows the mod's baseline cost).
    constexpr ULONGLONG kProfilerWindowMs = 5000;
    ULONGLONG g_profilerNextReportAt = 0;

    const char* ProfilerMode()
    {
        return g_recording ? "record" : (g_transport.Playing() ? "play" : "idle");
    }

    void UpdateProfilerReport(ULONGLONG now)
    {
        if (!g_profilerNextReportAt)
        {
            g_profilerNextReportAt = now + kProfilerWindowMs;
            return;
        }
        if (now < g_profilerNextReportAt)
            return;
        g_profilerNextReportAt = now + kProfilerWindowMs;
        if (!replay::log::Verbose())
        {
            // Debug=0 keeps no window lines (the session totals at each stop are kept);
            // the window is still taken so it restarts.
            (void)replay::prof::TakeWindow();
            return;
        }
        std::ofstream& log = LogFile();
        replay::prof::WriteWindowReport(log, ProfilerMode(), static_cast<std::uint32_t>(kProfilerWindowMs));
        log.flush();
    }
}

#if defined(REDEMPTION_DEV) // the bench executor exists in the developer build only (replay_bench.h)
namespace
{
    // The bench drives recording and playback through the keys' entry points.
    struct BenchHooks final : replay::bench::Hooks
    {
        void StartRecording() override { ::StartRecording(); }
        void StopRecording() override { ::StopRecording(); }
        bool Recording() const override { return g_recording; }
        bool StartPlayback(std::uint64_t now) override { return ::StartPlayback(now); }
        void StopPlayback() override { ::StopPlayback(); }
        void SetPaused(bool paused, std::uint64_t now) override { SetPlaybackPaused(paused, now); }
        bool CameraOp(const std::string& op, const std::vector<std::string>& args, std::string& detail) override
        {
            return BenchCameraOp(op, args, detail);
        }
        // The environment setting the editor's row would make, for the bench (Live or
        // Recorded).
        bool EnvironmentOp(const std::string& field, const std::string& value, std::string& detail) override
        {
            if (!g_transport.Playing() || !g_clip.environment.recorded)
            {
                detail = "no_environment_playing";
                return false;
            }
            replay::EnvironmentOverride& edit = g_clip.environment.edit;
            const bool live = value == "live";
            if (field == "clock")
                edit.clockMinutes = live ? replay::envoverride::kClockLive : -1;
            else
                edit.weather = live ? replay::envoverride::kWeatherLive : 0u;
            LogFile() << "BENCH_ENV_EDIT,field=" << field << ",value=" << value << ",clock_minutes=" << edit.clockMinutes
                      << ",weather=0x" << std::hex << edit.weather << std::dec << '\n';
            return true;
        }
        bool BufferRecording() const override
        {
            return g_recording && g_recordKind == RecordKind::Buffer;
        }
        void Buffer(const std::string& action) override
        {
            if (action == "save")
                SaveBuffer();
            else
                PressRecord();
        }
        void SetCleanView(bool on) override
        {
            g_cleanView = on;
            replay::hud::SetObjectivesEnabled(g_debugMode && !on);
        }
        bool WriteClip(const std::string& name, std::string& error) override
        {
            return WriteClipFile(std::wstring(name.begin(), name.end()), error);
        }
        bool ReadClip(const std::string& name, std::string& error) override
        {
            return ReadClipFile(ClipPath(std::wstring(name.begin(), name.end())), error);
        }
        bool SafeExitTest(const std::string& what) override
        {
            if (what == "exception")
                g_benchThrowNextTick = true;
            else if (what == "hold_exit")
                g_benchHoldNormalExit = true;
            else if (what == "release_exit")
                g_benchHoldNormalExit = false;
            else
                return false;
            return true;
        }
    };
    BenchHooks g_benchHooks;
}
#endif // REDEMPTION_DEV

namespace
{
    void RunRefusedCopy(const char* other)
    {
        LogFile() << "\n=== Redemption Editor NOT started: " << other
                  << " is also loaded. Delete the old files (ReplayProbe.asi, ReplayEditor.addon64, ReplayEditorCore.dll) "
                     "from the game folder and restart the game. ===\n";
        LogFile() << "MOD_DUPLICATE,refused=1,other=" << other << ",version=" << replay::build::kVersion << '\n';
        LogFile().flush();
        UpdateLanguage(GetTickCount64()); // the notice in the player's language
        std::string notice = TRF("hud.duplicate", "Redemption Editor is off: {other} (an older copy of the mod) is also installed. Delete ReplayProbe.asi, "
            "ReplayEditor.addon64 and ReplayEditorCore.dll from the game folder and restart the game.", {{"other", other}});
        // Repeated for the first minutes after the loading screens, then quiet.
        ULONGLONG lastShown = 0;
        int shown = 0;
        while (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) == 0)
        {
            const ULONGLONG now = GetTickCount64();
            if (shown < 10 && now - lastShown > 20000 && !DLC::GET_IS_LOADING_SCREEN_ACTIVE() &&
                !SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
            {
                replay::hud::ShowNotice(notice.c_str());
                lastShown = now;
                ++shown;
            }
            WAIT(0);
        }
    }
}

namespace
{
    // The replay turns the open world music off while it plays (the
    // OpenWorldMusicDisabled flag); a restart must turn it back on: the restart
    // turns the flag off here (MusicExit), and the startup path clears it again
    // for any other slot.
    bool OpenWorldMusicDisabledForSession() { return g_music.On(); }
    void RestoreOpenWorldMusicAfterRestart(std::ostream& log)
    {
        const bool wasOn = g_music.On();
        if (wasOn)
            MusicExit("session_restart");
        log << "SESSION_RESET_MUSIC,action=restore,restored=" << (wasOn ? 1 : 0) << '\n';
    }

    // ---- the save reload when the replay is left.
    using SavegamePushFn = bool(__fastcall*)(void* operation);
    using LoadFromSlotFn = void(__fastcall*)(int slot);

    // MSVC: no object with a destructor in a function with __try.
    bool CallSavegamePush(std::uintptr_t function, void* operation, bool& pushed, DWORD& code)
    {
        __try
        {
            pushed = reinterpret_cast<SavegamePushFn>(function)(operation);
            return true;
        }
        __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallLoadFromSlot(std::uintptr_t function, int slot, DWORD& code)
    {
        __try
        {
            reinterpret_cast<LoadFromSlotFn>(function)(slot);
            return true;
        }
        __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void WorldReloadFailed(const char* why)
    {
        // A refusal before the push would leave the theater owning the world (scripts
        // paused, persistent characters held, spawn denied) until a manual load; it is
        // handed back here, and the watchdog tries the reload once more.
        g_world.reloadFailed = true;
        g_world.reloadFailedAt = GetTickCount64();
        g_world.forceReload = false;
        g_world.reloadStarted = true;
        g_world.watching = false;
        EndTheatre((std::string("reload_failed_") + why).c_str());
        g_exitReloadPending = replay::session::ExitReloadRequest{}; // not our restart after all
        if (!g_debugMode)
            CAM::DO_SCREEN_FADE_IN(500);
        LogFile() << "WORLD_MODEL,phase=reload,ok=0,why=" << why << '\n';
        LogFile().flush();
        replay::hud::ShowNotice(TR("hud.reload_failed", "Redemption Editor: could not load your last save; load it from the pause menu "
            "(the world was cleared for the replay)."));
    }

    // The push (or its fallback), once out of the replay; the teardown is done by
    // then: StopPlayback deleted the puppets, ended fx, sounds, ropes, the clip's
    // map state and the music flag, and the exit wait is over. Here the clip saves
    // are written, the project saved and closed, and the restart is told it is
    // ours (g_exitReloadPending) before the push.
    void WorldModelReload(ULONGLONG now)
    {
        using namespace replay::worldmodel;
        std::uintptr_t base = 0;
        std::size_t imageSize = 0;
        ImageRange(base, imageSize);
        std::string checkError;
        PushState push;
        push.path = g_recConfig.worldReloadPath;
        push.signaturesOk = WorldModelChecks(checkError);
        push.savePending = SAVE::SAVEGAME_IS_SAVE_PENDING() != FALSE;
        push.loadingScreen = SCRIPTS::IS_LOADING_SCREEN_VISIBLE() != FALSE;
        SlotSummary slots;
        push.newestSlot = ReadWorldSlots(slots) ? slots.newest : -1;
        std::uint8_t opBytes[kLoadOpSize]{};
        OpView op;
        if (push.path == ReloadPath::Queue)
        {
            push.opRead = WorldCodeBytes(base, imageSize, kLoadOpRva, opBytes, sizeof(opBytes));
            op = ParseOp(opBytes);
            push.op = CheckOp(op, base);
            push.queueRead = WorldCodeBytes(base, imageSize, kQueueCountRva, reinterpret_cast<std::uint8_t*>(&push.queueCount),
                sizeof(push.queueCount));
        }
        else
        {
            std::int32_t menuStatus = 0;
            push.menuOpBusy = !WorldCodeBytes(base, imageSize, kMenuLoadOpStatusRva, reinterpret_cast<std::uint8_t*>(&menuStatus),
                                  sizeof(menuStatus)) ||
                menuStatus == kStatusQueued;
        }
        const PushVerdict verdict = DecidePush(push);
        if (verdict == PushVerdict::Wait)
        {
            if (!g_world.pushWaitSince)
            {
                g_world.pushWaitSince = now;
                LogFile() << "WORLD_MODEL,phase=reload,wait=1,save_pending=" << (push.savePending ? 1 : 0)
                          << ",loading_screen=" << (push.loadingScreen ? 1 : 0) << '\n';
            }
            if (now - g_world.pushWaitSince > kPushWaitMs)
                WorldReloadFailed("wait_timeout");
            return;
        }
        std::ofstream& log = LogFile();
        log << "WORLD_MODEL,phase=reload_check,path=" << ReloadPathName(push.path) << ",verdict=" << PushVerdictName(verdict)
            << ",checks=" << checkError << ",slot=" << push.newestSlot << ",slot_date=" << WorldSlotDate(slots)
            << ",slot_at_clear=" << g_world.clearedSlot;
        if (push.path == ReloadPath::Queue)
            log << ",op=" << OpCheckName(push.op) << ",op_status=" << op.status << ",op_state=" << op.state
                << ",queue=" << (push.queueRead ? push.queueCount : -1);
        else
            log << ",menu_busy=" << (push.menuOpBusy ? 1 : 0);
        log << '\n';
        if (verdict != PushVerdict::Push)
        {
            WorldReloadFailed(PushVerdictName(verdict));
            return;
        }
        // The DrawableStore slot as it is right before the session shutdown this push
        // starts (evidence for crash 0x363068).
        replay::drawstore::LogSlot("reload_push", log);
        // Closed theater: the scripts run again and the spawn funnel is open
        // before the load (the save's world must spawn).
        EndTheatre("reload");
        // The player's work first (data only): every clip save written, the project
        // saved when dirty and closed.
        FlushClipSaves();
        const std::wstring projectPath = g_projectOpen ? g_projectPath : std::wstring();
        const std::wstring clipPath = g_currentClipPath;
        if (g_projectOpen)
            CloseProject("world_reload");
        // The music flag off before the push (StopPlayback already did it; this logs
        // it with reload=1 when it was still on).
        MusicExit("world_reload", true);
        if (!g_debugMode)
            CAM::DO_SCREEN_FADE_OUT(0);
        // The restart that follows is ours (SESSION_RESET,why=own_reload).
        g_exitReloadPending.pending = true;
        g_exitReloadPending.requestedAtMs = GetTickCount64();
        g_exitReloadPending.reopenEditor = false; // this version: the game comes back, the editor stays closed
        g_exitReloadPending.clipPath = clipPath;
        g_exitReloadPending.projectPath = projectPath;
        g_world.reloadStarted = true;
        g_world.forceReload = false;
        g_world.reloadFailed = false;
#if defined(REDEMPTION_DEV)
        g_benchHoldNormalExit = false;
#endif
        WriteTheatreMarker(false, true, push.newestSlot);
        DWORD code = 0;
        if (push.path == ReloadPath::Queue)
        {
            // Rung 3: the fields +0x10..+0x1C as the ctor leaves them.
            std::uint8_t reset[kLoadOpSize]{};
            std::memcpy(reset, opBytes, sizeof(reset));
            ResetOpFields(reset);
            if (!replay::mem::Put(base + kLoadOpRva + kOpResetBegin, reset + kOpResetBegin, kOpResetEnd - kOpResetBegin))
            {
                WorldReloadFailed("op_reset_write");
                return;
            }
            bool pushed = false;
            const bool called = CallSavegamePush(base + kPushSignature.rva, reinterpret_cast<void*>(base + kLoadOpRva), pushed, code);
            std::int32_t status = -1;
            replay::mem::Read(base + kLoadOpRva + kOpStatus, status);
            LogFile() << "WORLD_MODEL,phase=reload,path=queue,called=" << (called ? 1 : 0) << ",pushed=" << (pushed ? 1 : 0)
                      << ",status=" << status << ",exception=0x" << std::hex << code << std::dec << ",slot=" << push.newestSlot
                      << ",clip=" << Narrow(clipPath) << ",project=" << Narrow(projectPath) << '\n';
            if (!called || !pushed || status != kStatusQueued)
            {
                WorldReloadFailed(!called ? "push_exception" : "push_refused");
                return;
            }
            g_world.lastStatus = status;
        }
        else
        {
            const bool called = CallLoadFromSlot(base + kLoadFromSlotSignature.rva, push.newestSlot, code);
            std::int32_t status = -1;
            replay::mem::Read(base + kMenuLoadOpStatusRva, status);
            LogFile() << "WORLD_MODEL,phase=reload,path=slot,called=" << (called ? 1 : 0) << ",menu_status=" << status
                      << ",exception=0x" << std::hex << code << std::dec << ",slot=" << push.newestSlot
                      << ",clip=" << Narrow(clipPath) << ",project=" << Narrow(projectPath) << '\n';
            if (!called)
            {
                WorldReloadFailed("load_from_slot_exception");
                return;
            }
            g_world.lastStatus = status;
        }
        LogFile().flush();
        g_world.watching = true;
        g_world.pushedAt = now;
        g_reloadPushedAtMs = now;
    }

    // Every tick: out of the replay for kExitSettleMs, then the reload; after the
    // push, the operation's status until the scripts restart.
    // ---- the safe exit (theatre_exit.h) -----

    // The world handed back, the world still cleared (the reload follows):
    // EndTheatre (scripts run again, persistent characters resumed, the
    // spawn funnel open, the evidence summary), the music flag off, a
    // paused replay's freeze lifted.
    void TheatreRelease(const char* why)
    {
        // An entry stopped before the ownership (an exception half way):
        // EndTheatre only ends an owning theater, so its hold goes here.
        using replay::theatre::safeexit::PartialEntry;
        const PartialEntry partial =
            replay::theatre::safeexit::UndoPartialEntry(g_world.entering, g_theatre.owning, g_theatre.perscharHeld);
        if (partial == PartialEntry::ResumePerschar)
            PERSCHAR::_0x2E957AA81F2C61C9();
        if (partial != PartialEntry::None)
        {
            g_theatre = TheatreSession{};
            LogFile() << "THEATRE_SAFE_EXIT,phase=partial_entry,why=" << why
                      << ",perschar_resumed=" << (partial == PartialEntry::ResumePerschar ? 1 : 0)
                      << ",held_after=" << replay::theatre::PerscharHeld() << '\n';
        }
        g_world.entering = false;
        const bool owned = g_theatre.owning;
        EndTheatre(why);
        MusicExit(why);
        ThawWorld(why);
        LogFile() << "THEATRE_SAFE_EXIT,phase=release,why=" << why << ",owned=" << (owned ? 1 : 0)
                  << ",cleared=" << (g_world.cleared ? 1 : 0) << '\n';
    }

    // The reload key unloaded nothing and the replay goes on: own the world
    // again as WorldModelEnter left it (the hold of the persistent
    // characters again, the pause from the next UpdateTheatre).
    void TheatreRetake()
    {
        if (g_theatre.owning)
            return;
        g_theatre = TheatreSession{};
        g_theatre.owning = true;
        if (g_recConfig.theatre.perschar)
        {
            PERSCHAR::_0x92690B0822493CE0();
            g_theatre.perscharHeld = true;
        }
        replay::theatre::SetOwning(true, g_recConfig.theatre.denySpawn);
        WriteTheatreMarker(true, false, g_world.clearedSlot);
        LogFile() << "THEATRE_SAFE_EXIT,phase=retake,perschar=" << (g_recConfig.theatre.perschar ? 1 : 0) << '\n';
    }

    // Out of the replay now: the playback stopped (puppets, fx, sounds,
    // ropes, the clip's map state), clip and project loads cancelled, the
    // exit's load wait ended, the editor's screens closed.
    void SafeExitStopReplay(const char* why)
    {
        const bool playing = g_transport.Playing();
        if (playing)
            StopPlayback();
        CancelClipRead();
        CancelProjectPreload();
        g_pendingLoadPath.clear();
        g_projectPending = ProjectPendingOpen{};
        if (g_exitLoad.active)
        {
            StopExitLoadScene();
            g_exitLoad = ExitLoadWait{};
        }
        const bool editor = g_editorCoversScreen;
        if (editor)
        {
            ++g_safeExitEditorCloses; // the add-on closes when sessionResetSequence grows
            g_editorCoversScreen = false;
        }
        if (!g_debugMode && CAM::IS_SCREEN_FADED_OUT())
            CAM::DO_SCREEN_FADE_IN(500);
        LogFile() << "THEATRE_SAFE_EXIT,phase=stop_replay,why=" << why << ",was_playing=" << (playing ? 1 : 0)
                  << ",editor_closed=" << (editor ? 1 : 0) << '\n';
    }

    void ApplySafeExit(const replay::theatre::safeexit::Verdict& verdict, ULONGLONG now)
    {
        using namespace replay::theatre::safeexit;
        if (verdict.action == Action::None)
            return;
        const char* why = CauseName(verdict.cause);
        LogFile() << "THEATRE_SAFE_EXIT,action=" << ActionName(verdict.action) << ",cause=" << why
                  << ",owning=" << (g_theatre.owning ? 1 : 0) << ",playing=" << (g_transport.Playing() ? 1 : 0)
                  << ",editor=" << (g_editorCoversScreen ? 1 : 0) << ",reload_started=" << (g_world.reloadStarted ? 1 : 0)
                  << ",watching=" << (g_world.watching ? 1 : 0) << ",failed=" << (g_world.reloadFailed ? 1 : 0)
                  << ",retries=" << g_safeExit.Retries() << ",since_clear_ms=" << (now - g_world.clearedAt) << '\n';
        switch (verdict.action)
        {
        case Action::None:
            break;
        case Action::Release:
            TheatreRelease(why);
            break;
        case Action::ReleaseAndReload:
            SafeExitStopReplay(why);
            TheatreRelease(why);
            g_world.forceReload = true;
            g_world.pushWaitSince = 0;
            break;
        case Action::Retake:
            TheatreRetake();
            break;
        case Action::RetryReload:
            TheatreRelease(why);
            g_world.reloadStarted = false;
            g_world.reloadFailed = false;
            g_world.watching = false;
            g_world.pushWaitSince = 0;
            g_world.forceReload = true;
            break;
        case Action::GiveUp:
            TheatreRelease(why);
            g_world.forceReload = false;
            g_world.watching = false;
            g_world.reloadStarted = true;
            if (!g_debugMode)
                CAM::DO_SCREEN_FADE_IN(500);
            if (!g_world.gaveUpNoticed)
            {
                g_world.gaveUpNoticed = true;
                replay::hud::ShowNotice(TR("hud.reload_gave_up", "Redemption Editor: the world could not be reloaded after the replay; "
                    "load your last save from the pause menu."));
            }
            break;
        }
        LogFile().flush();
    }

    // Every tick with the world cleared (UpdateWorldModel).
    void UpdateSafeExit(ULONGLONG now)
    {
        replay::theatre::safeexit::Tick tick;
        tick.nowMs = now;
        tick.cleared = g_world.cleared;
        tick.owning = g_theatre.owning;
        tick.reloadStarted = g_world.reloadStarted;
        tick.watching = g_world.watching;
        tick.pushedAtMs = g_world.pushedAt;
        tick.reloadFailed = g_world.reloadFailed;
        tick.reloadFailedAtMs = g_world.reloadFailedAt;
        tick.loadingScreen = SCRIPTS::IS_LOADING_SCREEN_VISIBLE() != FALSE || DLC::GET_IS_LOADING_SCREEN_ACTIVE() != FALSE;
        tick.playing = g_transport.Playing();
        tick.editorOpen = g_editorCoversScreen;
        tick.recording = g_recording;
        tick.clipOpening = ClipReadPending() || !g_pendingLoadPath.empty() || ClipLoadWaiting() || g_projectPending.active;
        tick.exitFading = g_exitLoad.active;
        const Player player = PLAYER::PLAYER_ID();
        tick.playerDead = !tick.reloadStarted && PLAYER::IS_PLAYER_DEAD(player) != FALSE;
        tick.exception = g_tickException;
        g_tickException = false;
        const ULONGLONG reloadKeyAt = static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_reloadKeyAt, 0, 0));
        tick.reloadKey = g_scriptHookDev && reloadKeyAt != 0 && now >= reloadKeyAt && now - reloadKeyAt < replay::theatre::safeexit::kReloadKeyMs;
        ApplySafeExit(g_safeExit.Update(tick), now);
    }

    // A copy unloaded with the world cleared (hot reload, Ctrl+R, an unload
    // in the middle of a replay) left the marker: this copy, in the same
    // process, hands the world back (the old copy's pause and hold may
    // still be on) and reloads the save. Startup, after the hooks.
    void RecoverTheatreAtStartup()
    {
        using namespace replay::theatre::safeexit;
        char line[128]{};
        bool present = false;
        FILE* file = nullptr;
        if (_wfopen_s(&file, StateFile(kTheatreMarkerFile).c_str(), L"r") == 0 && file)
        {
            present = true;
            if (!std::fgets(line, sizeof(line), file))
                line[0] = '\0';
            std::fclose(file);
        }
        Marker marker;
        const bool parsed = present && ParseMarker(line, marker);
        const Startup startup = DecideStartup(present, parsed, marker, GetCurrentProcessId());
        if (startup == Startup::None)
            return;
        LogFile() << "THEATRE_SAFE_EXIT,phase=startup,marker=" << StartupName(startup) << ",pid=" << marker.pid
                  << ",owning=" << (marker.owning ? 1 : 0) << ",pushed=" << (marker.pushed ? 1 : 0) << ",slot=" << marker.slot
                  << ",restarted=" << (g_sessionRestarted ? 1 : 0) << '\n';
        if (startup != Startup::Recover)
        {
            RemoveTheatreMarker();
            return;
        }
        // The old copy's state (rung 1, the same natives as EndTheatre): the
        // threads run again (every thread; only the replay paused them) and
        // the persistent characters' hold is let go (the flag back to 0, as
        // the GFM's own step does). The spawn funnel is this copy's detour,
        // not owning; the music flag was cleared by the startup plan.
        ANIMSCENE::_PAUSE_SCRIPT_THREADS(FALSE);
        PERSCHAR::_0x2E957AA81F2C61C9();
        const ULONGLONG now = GetTickCount64();
        g_world = WorldModelSession{};
        g_world.cleared = true;
        g_world.recovered = true;
        g_world.clearedAt = now;
        g_world.clearedSlot = marker.slot;
        g_world.clearAfterLogged = true;
        if (marker.pushed)
        {
            // The old copy pushed the load: watch it (no loading screen in
            // 15 s and the watchdog pushes again).
            g_world.reloadStarted = true;
            g_world.watching = true;
            g_world.pushedAt = now;
        }
        else
            g_world.forceReload = true;
        WriteTheatreMarker(false, marker.pushed, marker.slot);
        LogFile() << "THEATRE_SAFE_EXIT,phase=recover,cause=" << CauseName(Cause::PreviousCopy)
                  << ",perschar_after=" << replay::theatre::PerscharHeld() << ",reload=" << (marker.pushed ? "watch" : "push") << '\n';
        LogFile().flush();
        replay::hud::ShowNotice(TR("hud.reloaded_in_replay", "Redemption Editor: the mod was reloaded during a replay; your last save loads now."));
    }


    void UpdateWorldModel(ULONGLONG now)
    {
        using namespace replay::worldmodel;
        // A cleared world is handled whatever the ini says now (a copy recovered from
        // the marker may run with WorldModel=off).
        if (!g_world.cleared)
        {
            g_tickException = false; // logged; nothing of the world to hand back
            return;
        }
        UpdateSafeExit(now);
        if (!g_world.cleared)
            return;
        // The watchdog's verdict: push now, in or out of the replay.
        if (g_world.forceReload && !g_world.reloadStarted && !g_world.watching)
        {
            WorldModelReload(now);
            return;
        }
        UpdateTheatre(); // closed theater: scripts paused, evidence drained
        if (!g_world.clearAfterLogged && now - g_world.clearedAt >= 1000)
        {
            g_world.clearAfterLogged = true;
            LogFile() << "WORLD_MODEL,phase=clear_after,ms=" << (now - g_world.clearedAt) << WorldPoolCounts()
                      << ",puppets=" << g_puppets.list.size() << '\n';
            TheatreCensus("after_1s");
        }
        if (g_world.watching)
        {
            std::uintptr_t base = 0;
            std::size_t imageSize = 0;
            ImageRange(base, imageSize);
            std::int32_t status = -1;
            replay::mem::Read(base + (g_recConfig.worldReloadPath == ReloadPath::Queue ? kLoadOpRva + kOpStatus : kMenuLoadOpStatusRva), status);
            if (status != g_world.lastStatus)
            {
                LogFile() << "WORLD_MODEL,phase=reload_watch,status=" << status << ",ms=" << (now - g_world.pushedAt)
                          << ",loading_screen=" << (SCRIPTS::IS_LOADING_SCREEN_VISIBLE() ? 1 : 0) << '\n';
                g_world.lastStatus = status;
            }
            if (now - g_world.pushedAt > kReloadWatchMs)
            {
                g_world.watching = false;
                LogFile() << "WORLD_MODEL,phase=reload_watch,stuck=1,status=" << status << '\n';
                WorldReloadFailed("no_restart");
            }
            return;
        }
        ExitState state;
        state.mode = Mode::Gta; // cleared, so the model ran (or the marker recovered it)
        state.step = g_world.recovered ? kStepReload : g_recConfig.worldModelStep;
        state.cleared = g_world.cleared;
        state.reloadStarted = g_world.reloadStarted;
        state.playing = g_transport.Playing();
        state.editorOpen = g_editorCoversScreen;
        state.clipOpening = ClipReadPending() || !g_pendingLoadPath.empty() || ClipLoadWaiting() || g_projectPending.active;
        state.recording = g_recording;
        state.exitFading = g_exitLoad.active;
        const bool inReplay = state.playing || state.editorOpen || state.clipOpening || state.recording || state.exitFading;
        if (inReplay)
            g_world.idleSince = 0;
        else if (!g_world.idleSince)
            g_world.idleSince = now;
        state.idleMs = g_world.idleSince ? now - g_world.idleSince : 0;
        ExitVerdict verdict = DecideExit(state);
#if defined(REDEMPTION_DEV)
        if (g_benchHoldNormalExit && (verdict == ExitVerdict::Reload || verdict == ExitVerdict::Manual))
            verdict = ExitVerdict::Settle; // bench: the watchdog must find the stuck world
#endif
        if (verdict == ExitVerdict::Reload && state.idleMs < g_theatreReloadDelayMs)
            return; // TheatreReloadDelayMs
        if (verdict == ExitVerdict::Reload)
            WorldModelReload(now);
        else if (verdict == ExitVerdict::Manual)
        {
            // WorldModelStep=2: the bench test of the clear alone.
            g_world.reloadStarted = true;
            LogFile() << "WORLD_MODEL,phase=reload,skipped=step2,cleared_ms=" << (now - g_world.clearedAt) << '\n';
            replay::hud::ShowNotice("Redemption Editor: WorldModelStep=2, load your last save from the pause menu now.");
        }
    }

    // The session shutdown for a ScriptHook restart (a save loaded from the menu,
    // a death, a mission reload, the mod's own reload). The module's part
    // (detours, crash handler, worker, bridge) stays; the session's part
    // (recording, buffer, playback, puppets, editor, project) is zeroed, with no
    // native on a handle of the old world. Runs before the setup (WarmUpHooks) on
    // the new fiber; the old one is abandoned in its WAIT at the end of a tick, so
    // no lock of ours is held.
    void ShutdownSessionAfterRestart()
    {
        const ULONGLONG now = GetTickCount64();
        replay::session::State state;
        state.recording = g_recording;
        state.bufferRecording = g_recording && g_recordKind == RecordKind::Buffer;
        state.manualInBuffer = g_recording && g_manualInBuffer;
        state.recordingMs = g_recording ? replay::pose::capture::NowMs() : 0;
        state.playing = g_transport.Playing();
        state.paused = g_transport.Paused();
        state.puppets = static_cast<std::uint32_t>(g_puppets.list.size());
        state.clipInMemory = g_transport.DurationMs() > 0 || !g_clip.created.empty();
        state.clipFromDisk = g_clip.fromDisk;
        state.clipEdited = g_clipEdited;
        state.editorOpen = g_editorCoversScreen;
        state.projectOpen = g_projectOpen;
        state.projectDirty = g_projectDirty;
        state.projectOwnsPlayback = ProjectOwnsPlayback();
        state.freeCam = g_freeCamEnabled || g_freeCam != 0;
        state.replayCam = g_replayCam != 0;
        state.editingMarker = g_editingMarker != 0;
        state.settling = g_settle.Active();
        state.clipLoadWaiting = ClipLoadWaiting();
        state.exitLoadWaiting = g_exitLoad.active;
        state.worldFrozen = g_worldFreeze.active;
        state.clipWorldActive = g_clipWorld.active;
        state.timeScaleChanged = g_worldTimeScaleApplied != 1.0f;
        state.clipReadPending = ClipReadPending() || !g_pendingLoadPath.empty();
        state.openWorldMusicDisabled = OpenWorldMusicDisabledForSession();
        state.exitReloadPending = replay::session::IsOwnReload(g_exitReloadPending, now);
        const replay::session::Plan plan = replay::session::Decide(state);
        LogFile() << replay::session::FormatLine(state, plan) << '\n';
        LogFile().flush();

        // 1. Recording (buffer included): stopped without a file. The stop disarms the
        // pose capture (its detour would take a new ped at a freed skeleton's address
        // into an old record), stops the observers and the worker; its natives are
        // global (clock, weather, the current player). Not saved: the clip's clock ran
        // through the whole loading screen (the entities' last state stretched over
        // it) and the tail may hold frames of reused skeleton addresses.
        if (plan.stopRecording)
        {
            StopRecording(false);
            g_manualInBuffer = false;
        }
        // The always-on decal monitor holds the old world's decals (freed info
        // records, engine ids the new world reuses); a later recording would take them
        // as its initial decals (decal crash 0x2C42D1A). Emptied on every restart,
        // whatever the plan.
        replay::events::ForgetDecalMonitorAfterRestart();
        // 2. The player's work that is data only: a project clip's markers into the
        // project, a gallery clip's marker edits into its file (the copy is made here,
        // the file is written on the save thread).
        if (plan.syncProjectMarkers)
            SyncProjectMarkers();
        if (plan.saveEditedClip)
            SaveEditedClip("session_reset");
        // 3. Reads on threads: their result would open a clip into the new
        // session.
        if (plan.cancelReads)
        {
            CancelClipRead();
            CancelProjectPreload();
            g_pendingLoadPath.clear();
        }
        // 4. Playback: every table of handles emptied, nothing deleted or
        // restored by handle; the map state by the reload path (its backup
        // file); the clip world's file by RestoreLeftClipWorld(true) below.
        replay::playback::ForgetReport forgot;
        std::uint32_t doors = 0;
        bool worldForgot = false;
        if (plan.forgetPlayback)
        {
            forgot = replay::playback::ForgetAfterRestart();
            doors = replay::doors::ForgetPlaybackAfterRestart();
            worldForgot = replay::world::ForgetPlaybackAfterRestart(LogFile());
            g_moonGiveBack = replay::moontrack::GiveBack{}; // the previous session's moon
            g_moonCheck = MoonCheck{};
            g_transport.Stop();
            replay::pose::capture::ReleaseFinalizer(); // the Pose Editor may take it now
            g_clipWorld = ClipWorld{};
            g_exitLoad = ExitLoadWait{};
            if (g_settle.Active())
                g_settle.Cancel(now);
            g_settleJumpScene = false;
            g_abilityAudio = AbilityAudio{}; // its sound id and scene were the old script's
            g_cinematicSpans.clear();
            g_cinematicSpanShown = nullptr;
            g_trimReady = false;
            g_playbackSpeedIndex = kNormalSpeedIndex;
            g_cameraTrackManualOverride = false;
            g_playerLanding = PlayerLanding{};
            // The audio environment is an engine global the load does not reset; the
            // microphone hash +0x780 (and the bit 0x100) the old playback wrote would hold
            // the live game's microphone in the new session. Guarded memory only, and only
            // a value that is still ours.
            replay::gamelistener::Release("script_restart", LogFile());
        }
        if (plan.stopScreenEffects)
            StopOwnScreenEffects("session_reset"); // post effects by name: global
        // 5. Cameras: script cameras die with the old script (H); handles
        // forgotten, never destroyed by number (another script may own it).
        if (plan.forgetCameras)
        {
            g_freeCam = 0;
            g_freeCamPlayer = 0;
            g_freeCamEnabled = false;
            g_freeCamPlayerWasFrozen = false;
            g_freeCamVelocity = {};
            g_replayCam = 0;
            g_replayCamRendering = false;
            g_replayCamDof = false;
            g_replayCamEngineFocus = false;
            g_renderedCamera.valid = false;
            g_editingMarker = 0;
            g_freeCamBeforeEdit = false;
        }
        // 6. Global state the replay changed.
        if (plan.resetTimeScale)
        {
            MISC::SET_TIME_SCALE(1.0f);
            g_worldTimeScaleApplied = 1.0f;
        }
        if (plan.forgetWorldFreeze)
            g_worldFreeze = WorldFreeze{}; // its file is thawed by the setup (WORLD_FREEZE,why=left_after_reload)
        if (plan.restoreMusic)
            RestoreOpenWorldMusicAfterRestart(LogFile());
        // 7. Project: saved when dirty (data), then closed.
        if (plan.closeProject)
            CloseProject("session_reset");
        // 8. The clip in memory: a recording of the old world carries its handles
        // (adopted objects, source handles, decal ids); dropped with its pose records.
        if (plan.dropClip)
        {
            {
                replay::Recording empty; // default-initialised, as g_clip itself
                g_clip = std::move(empty);
            }
            std::string importError;
            replay::pose::capture::ImportRecords({}, importError);
            g_cameraTrack.Clear();
            g_markerHistory = {};
            g_memoryClipPath.clear();
            g_currentClipPath.clear();
            g_clipEdited = false;
            g_loadedClipHoldsBuffer = false;
            ++g_clipLoadSequence;
            g_transport.SetDurationMs(0);
        }
        // 9. Editor: the core's side closed; the add-on sees the sequence grow.
        g_editorCoversScreen = false;
        ++g_sessionResets;
        // 10. Buffer: starts again by UpdateBufferAutoStart once the player
        // plays (after the loading screen); the counters of this load.
        g_bufferResumeAt = now + 3000;
        g_recordingsThisLoad = 0;
        if (plan.tellUser)
            g_sessionResetNotice = TR("hud.session_reset", "Redemption Editor: the game loaded while recording; that recording was discarded.");
        // 11. The mod's own reload.
        if (plan.why == replay::session::Why::OwnReload)
        {
            LogFile() << "SESSION_RESET_OWN_RELOAD,clip=" << Narrow(g_exitReloadPending.clipPath)
                      << ",project=" << Narrow(g_exitReloadPending.projectPath)
                      << ",reopen_editor=" << (g_exitReloadPending.reopenEditor ? 1 : 0) << ",action=hook\n";
            // The editor reopens on the clip here when reopenEditor.
        }
        else if (g_exitReloadPending.pending)
            LogFile() << "SESSION_RESET_OWN_RELOAD,stale=1,age_ms=" << (now - g_exitReloadPending.requestedAtMs) << '\n';
        g_exitReloadPending = replay::session::ExitReloadRequest{};
        // The load ends the cleared world (the save's world is back).
        if (g_world.cleared || g_world.reloadStarted)
            LogFile() << "WORLD_MODEL,phase=session_reset,cleared=" << (g_world.cleared ? 1 : 0)
                      << ",reload_started=" << (g_world.reloadStarted ? 1 : 0)
                      << ",own=" << (plan.why == replay::session::Why::OwnReload ? 1 : 0)
                      << ",since_push_ms=" << (g_world.pushedAt ? now - g_world.pushedAt : 0) << '\n';
        g_world = WorldModelSession{};
        EndTheatre("session_reset");
        RemoveTheatreMarker(); // the save's world is back
        g_safeExit = replay::theatre::safeexit::Watchdog{};
        if (g_reloadPushedAtMs)
        {
            LogFile() << "THEATRE_RELOAD,phase=scripts_restart,ms=" << (now - g_reloadPushedAtMs) << '\n';
            g_reloadDoneLogged = false;
        }
        LogFile() << "SESSION_RESET_DONE,resets=" << g_sessionResets << ",puppets_forgotten=" << forgot.puppets
                  << ",hidden_world_forgotten=" << forgot.hiddenWorld << ",objects_forgotten=" << forgot.objects
                  << ",events_forgotten=" << forgot.events << ",trains_forgotten=" << forgot.trains << ",doors_forgotten=" << doors
                  << ",world_backup_left=" << (worldForgot ? 1 : 0) << ",clip_dropped=" << (plan.dropClip ? 1 : 0)
                  << ",project_closed=" << (plan.closeProject ? 1 : 0) << '\n';
        replay::drawstore::LogSlot("session_reset", LogFile());
        LogFile().flush();
    }

    // The restart's check: the hooks warmed up again and the session is idle.
    void LogSessionCheck()
    {
        const bool hooksOk = g_lastWarmUpStatus == MH_OK;
        const bool idle = !g_recording && !g_transport.Playing() && g_puppets.list.empty();
        LogFile() << "SESSION_CHECK,hooks_status=" << MH_StatusToString(g_lastWarmUpStatus) << ",hooks_applied=" << g_lastWarmUpApplied
                  << ",idle=" << (idle ? 1 : 0) << ",anomaly=" << (hooksOk && idle ? 0 : 1) << '\n';
    }
}

void ScriptMain()
{
    {
        // One copy of the mod per process, checked before anything starts (every ASI
        // is loaded by now; the old one sorts after this).
        const char* other = "";
        bool restarted = false;
        if (replay::identity::AnotherCopyLoaded(other, &restarted))
        {
            InterlockedExchange(&g_refusedStart, 1);
            RunRefusedCopy(other);
            return;
        }
        // The game restarted its scripts (ScriptHook ran ScriptMain again in this
        // module): the setup below runs again.
        if (restarted)
        {
            LogFile() << "SCRIPT_RESTART,why=scripts_relaunched,version=" << replay::build::kVersion << '\n';
            // The session's state is the old world's.
            ShutdownSessionAfterRestart();
        }
        g_sessionRestarted = restarted;
        // An install upgraded from the ReplayProbe names: its settings and the state
        // files a reload or a crash left, under the new names.
        char folder[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, folder);
        const replay::identity::IniMigration migration = replay::identity::MigrateLegacyIni(folder);
        const int stateFiles = replay::identity::MigrateLegacyStateFiles();
        if (migration != replay::identity::IniMigration::None || stateFiles > 0)
            LogFile() << "INI_MIGRATION,from=" << replay::identity::kLegacyIniFile << ",to=" << replay::identity::kIniFile
                      << ",result=" << (migration == replay::identity::IniMigration::Copied ? "copied"
                                         : migration == replay::identity::IniMigration::Failed ? "failed" : "none")
                      << ",state_files=" << stateFiles << '\n';
        // A first run (no settings file at all) gets the shipped one, every value the
        // default, so the player finds the keys he can change.
        if (replay::identity::WriteDefaultIni(folder, replay::inipublic::kShippedIni))
            LogFile() << "CONFIG_INI_DEFAULT,written=1,path=" << folder << '\\' << replay::identity::kIniFile << '\n';
    }
    InstallCrashEvidence();
    replay::hooks::BeginBatch(); // closed by WarmUpHooks: one freeze for every detour
    replay::playback::Attach(g_context);
    replay::objects::Attach(g_context);
    replay::trains::Attach(g_context);
    replay::recorder::Attach(g_context);
    replay::events::Attach(g_context);
    replay::muscles::Attach(g_context);
#if defined(REDEMPTION_DEV)
    replay::bench::Attach(g_context, g_benchHooks); // the public build has no bench (replay_bench.h)
#endif
    WriteLogHeader();
    WarmUpHooks();
    if (g_sessionRestarted)
        LogSessionCheck();
    RestoreLeftClipWorld(g_sessionRestarted);
    // A reload in the middle of a replay; only this slot's bit.
    ApplyMusicPlan(replay::music::Guard::Startup());
    LogFile() << "MUSIC_STOP,phase=startup,flag=0,mode=" << MusicModeText(g_recConfig.musicStop) << '\n';
    // A freeze left by a reload in a paused replay.
    if (FILE* file = _wfopen(StateFile(kWorldFreezeFile).c_str(), L"r"))
    {
        std::fclose(file);
        _wremove(StateFile(kWorldFreezeFile).c_str());
        ANIMSCENE::_REQUEST_PHOTO_MODE_DEFREEZE();
        LogFile() << "WORLD_FREEZE,action=thaw,why=left_after_reload\n";
    }
    // A copy unloaded with the world cleared.
    g_scriptHookDev = GetFileAttributesA("ScriptHookRDR2.dev") != INVALID_FILE_ATTRIBUTES;
    RecoverTheatreAtStartup();
    // The previous unload's report (see OnModuleDetach).
    if (FILE* file = _wfopen(StateFile(kUnloadReportFile).c_str(), L"r"))
    {
        char line[160]{};
        if (!std::fgets(line, sizeof(line), file))
            line[0] = '\0';
        std::fclose(file);
        _wremove(StateFile(kUnloadReportFile).c_str());
        for (char& c : line)
            if (c == '\n' || c == '\r')
                c = '\0';
        LogFile() << "UNLOAD_PREVIOUS,unsafe=1," << line << '\n';
    }
    // The background clip compressor; its first pass 30 s after the load (when
    // CompressClips=1).
    // The base folder's clips only (a game folder that takes no writes keeps its own as they are).
    replay::clipcompress::worker::Start(replay::storage::Folder(kClipFolder), CompressSettings(), 30000);
    RebuildProjectList(); // the home screen's project count
    // The current name and version (VERSION), in English.
    {
        using replay::keys::Action;
        ShowObjective((std::string(replay::build::kDisplayName) + ": " + KeyText(Action::Record) + " record, " +
            KeyText(Action::SaveBuffer) + " save buffer, " + KeyText(Action::Stop) + " play, " + KeyText(Action::PlayPause) +
            " pause, " + KeyText(Action::FrameBack) + " and " + KeyText(Action::FrameForward) + " scrub").c_str());
    }

    while (true)
    {
        if (InterlockedCompareExchange(
                &g_moduleDetaching,
                0,
                0) != 0)
        {
            FlushClipSaves(); // the writer thread runs this module's code; the saves not written get a last try
            CancelClipRead(); // and a clip being read
            CancelProjectPreload(); // and a project's read ahead
            ReapProjectPreloads(true);
            if (g_projectOpen && g_projectDirty)
            {
                SyncProjectMarkers();
                SaveProject("unload"); // never lost
            }
            ServiceClipListRead(true); // the clip list reader too
            StopAbilityAudio(false); // a Dead Eye loop must not outlive the mod
            StopOwnScreenEffects("unload"); // nor a filter or the Dead Eye post effect
            StopClipLoadScene(); // nor the clip's load scene
            ThawWorld("unload"); // nor a paused replay's world freeze
            replay::world::EndPlayback(LogFile()); // nor the clip's map state (Ctrl+R)
            MusicExit("unload"); // nor the open world music flag
            // Nor the closed theater (the marker stays: the next copy reloads the save).
            if (g_theatre.owning)
                EndTheatre("unload");
            return;
        }

        // An exception out of a tick does not end the script with the world owned: it
        // is logged and the safe exit runs on the next tick (theatre_exit.h).
        try
        {
#if defined(REDEMPTION_DEV)
        if (g_benchThrowNextTick)
        {
            g_benchThrowNextTick = false;
            throw std::runtime_error("bench_safe_exit_exception");
        }
#endif
        const ULONGLONG now = GetTickCount64();
        const std::uint64_t tickStart = replay::prof::Now();
        replay::entities::BeginFrame(); // the frame's entity cache
        if (g_transport.Playing())
        {
            // What a scrub tick did (SCRUB_COST).
            g_tickStartCounters = CostCountersNow();
            g_tickStartPlayhead = g_transport.PlayheadMs();
        }
        ExcludeRecordingStall();
        UpdateLanguage(now);
        replay::world::ServiceRestoreLeft(LogFile()); // a backup left by a reload or a crash
        if (!g_sessionResetNotice.empty() && !DLC::GET_IS_LOADING_SCREEN_ACTIVE() && !SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
        {
            replay::hud::ShowNotice(g_sessionResetNotice.c_str());
            g_sessionResetNotice.clear();
        }
        if (!g_keysNotice.empty() && !DLC::GET_IS_LOADING_SCREEN_ACTIVE() && !SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
        {
            replay::hud::ShowNotice(g_keysNotice.c_str()); // a [Keys] value refused
            g_keysNotice.clear();
        }
        if (InterlockedExchange(&g_toggleRecordingRequested, 0))
            PressRecord();
        if (InterlockedExchange(&g_saveBufferRequested, 0))
            SaveBuffer();
        UpdateBufferAutoStart(now);
        UpdateWorldModel(now);
        // Closed theater: the reload's length, from the push to the player
        // back in control with no loading screen.
        if (!g_reloadDoneLogged && g_reloadPushedAtMs && !SCRIPTS::IS_LOADING_SCREEN_VISIBLE() &&
            PLAYER::IS_PLAYER_CONTROL_ON(PLAYER::PLAYER_ID()) && !CAM::IS_SCREEN_FADED_OUT())
        {
            g_reloadDoneLogged = true;
            LogFile() << "THEATRE_RELOAD,phase=done,ms=" << (now - g_reloadPushedAtMs) << '\n';
            g_reloadPushedAtMs = 0;
        }

        if (InterlockedExchange(&g_togglePlaybackRequested, 0))
        {
            if (g_transport.Playing())
                StopPlayback();
            else
                StartPlayback(now);
        }

        // Edit Camera stays paused on its marker (K would play on and the view kept
        // would belong to another time).
        if (InterlockedExchange(&g_togglePlayheadPauseRequested, 0) && !g_editingMarker)
            SetPlaybackPaused(!g_transport.Paused(), now);

        if (InterlockedExchange(&g_toggleFreeCamRequested, 0))
        {
            if (g_editingMarker)
                LeaveMarkerCameraEdit(true);
            else if (g_freeCamEnabled)
                StopFreeCam();
            else
                StartFreeCam(now);
        }

        ServicePoseFinalizer();
        FinishPendingSave(false);
        ServiceClipListRead(false);
        replay::water::Drain(LogFile(), g_recording ? "record" : g_transport.Playing() ? "play" : "idle");
        replay::water::probe::Tick(LogFile(), g_recording ? "record" : g_transport.Playing() ? "play" : "idle",
            g_recording || g_transport.Playing(), now);
        ServiceClipCompressor();
        {
            // Mirrored for the text written on a later tick (deferred_log.h).
            const bool quiet = g_recording && g_recordKind == RecordKind::Buffer && !g_manualInBuffer;
            g_logFilter.SetQuiet(quiet);
            replay::log::SetQuiet(quiet);
        }
        {
            static ULONGLONG nextModuleRefresh = 0;
            if (now >= nextModuleRefresh)
            {
                nextModuleRefresh = now + 30000;
                RefreshKnownModules(); // names for the crash handler
                // The mod environment as loaded (ASI and DLL mods of the game folder, overlays
                // from outside it, vfs/lml/ModManager), at the first refresh and whenever it
                // changes.
                static std::string loggedMods;
                std::string mods = replay::qa::bundle::EnvModsLine();
                if (mods != loggedMods)
                {
                    LogFile() << mods << '\n';
                    loggedMods = std::move(mods);
                }
                // The DrawableStore slot of crash 0x363068 and the load callback, once per load.
                static bool drawStoreLogged = false;
                if (!drawStoreLogged)
                {
                    drawStoreLogged = true;
                    replay::drawstore::LogSlot("load", LogFile());
                }
                // The named threads' ids, when they changed.
                static std::string loggedThreads;
                std::string threads = replay::threads::MapText();
                if (threads != loggedThreads)
                {
                    LogFile() << "THREADS,module=RedemptionEditor.asi," << threads << '\n';
                    loggedThreads = std::move(threads);
                }
            }
        }
        if (g_recording)
        {
            if (g_recTiming.lastTick)
            {
                const double gap = TicksMs(tickStart - g_recTiming.lastTick);
                if (g_recTiming.ticks < 180)
                {
                    g_recTiming.firstMaxMs = (std::max)(g_recTiming.firstMaxMs, gap);
                    g_recTiming.firstOver33 += gap > 33.4 ? 1 : 0;
                    g_recTiming.firstOver100 += gap > 100.0 ? 1 : 0;
                }
                g_recTiming.maxMs = (std::max)(g_recTiming.maxMs, gap);
                g_recTiming.over100 += gap > 100.0 ? 1 : 0;
            }
            g_recTiming.lastTick = tickStart;
            ++g_recTiming.ticks;
        }

        ProcessCameraKeys();
        if (InterlockedExchange(&g_toggleOverlayRequested, 0))
            g_overlayEnabled = !g_overlayEnabled;
        if (InterlockedExchange(&g_qaReportRequested, 0) | (replay::qa::kit::TakeReportRequest() ? 1 : 0)) // or the editor's
        {
            // A problem mark still in its window (or waiting for its note and pictures)
            // goes into this report: it starts when they are in, at most 25 s later.
            if (!g_qaReportWaitSince)
                g_qaReportWaitSince = now;
        }
        if (g_qaReportWaitSince && (replay::qa::kit::ReadyForReport(now) || now - g_qaReportWaitSince > 25000))
        {
            g_qaReportWaitSince = 0;
            g_qaReportWaitSaid = false;
            StartQaReport();
        }
        else if (g_qaReportWaitSince && !g_qaReportWaitSaid)
        {
            g_qaReportWaitSaid = true;
            replay::hud::ShowNotice(TR("hud.report_after_mark", "Redemption Editor: finishing the problem mark, the report follows."));
        }
        ServiceQaReport(false);
        // The F8 list is the debug HUD's (its text is drawn by the engine's debug
        // text); the product lists clips in the editor's gallery.
        if (InterlockedExchange(&g_loadNextClipRequested, 0) && g_debugMode &&
            (!g_recording || g_recordKind == RecordKind::Buffer))
            PressClipPicker(now);
        UpdateClipPicker(now);

        ProcessEditorCommands(now);
        UpdateProject(now);
        UpdateProjectExport(now); // a whole project export: the next piece's clip
        replay::bench::Update(now);

        const ProbeFrame frame = CaptureFrame();
        replay::events::Monitor();
        // The read probes' frame (a no-op with SoundFxProbe=0).
        replay::soundfx::Tick(g_transport.Playing() ? replay::soundfx::Mode::Play
            : g_recording ? replay::soundfx::Mode::Record : replay::soundfx::Mode::Idle);
        {
            // The capture probes' frame (a no-op with CaptureProbe=0).
            replay::captureprobe::TickInput probe;
            probe.mode = g_transport.Playing() ? replay::soundfx::Mode::Play
                : g_recording ? replay::soundfx::Mode::Record : replay::soundfx::Mode::Idle;
            replay::captureprobe::Tick(probe);
        }
        UpdateRecording(frame);
        UpdatePlayback(now);
        UpdateClipStart(GetTickCount64()); // the stagger's batch budget
        replay::playback::Update();
        replay::muscles::TickLive(g_transport.Playing());
        if (g_transport.Playing())
            replay::doors::UpdatePlayback(g_clip, static_cast<std::uint32_t>(g_transport.PlayheadMs()), LogFile());
        ResolvePendingFollow();
        {
            // The QA kit: marks, the health buffer, hitches.
            replay::qa::kit::TickState qa;
            qa.playing = g_transport.Playing();
            qa.paused = g_transport.Paused();
            qa.scrubbing = g_transport.Scrubbing();
            qa.settling = g_settle.Active() || ClipLoadWaiting() || replay::playback::Preparing();
            qa.exporting = replay::exportprobe::Active() || replay::exportprobe::ShowsExporting();
            qa.recording = g_recording;
            qa.editorOpen = g_editorCoversScreen;
            qa.freeCam = g_freeCamEnabled;
            qa.editingMarker = g_editingMarker != 0;
            qa.playheadMs = g_transport.PlayheadMs();
            qa.durationMs = g_transport.DurationMs();
            qa.rate = g_transport.Rate();
            if (qa.playing && !g_currentClipPath.empty())
                qa.clip = ClipFileName(g_currentClipPath);
            qa.puppets = &g_puppets;
            qa.lifecycle = &g_clip.lifecycle;
            replay::qa::kit::Tick(now, qa);
        }
        if (g_debugMode && !g_cleanView && !g_editorCoversScreen && !replay::exportprobe::HidesHud())
            replay::playback::DrawMarkers();
        ProcessFreeCamActions(); // keys and editor options
        UpdateFreeCam(now);
        UpdateReplayCamera();
        UpdateWorldFocus(now); // after every camera placed this tick
        UpdateCameraOverlay(); // the markers in the world (camera_overlay.h)
        UpdateCameraCutRate(now); // CAMERA_CUT_RATE
        UpdateClipWorld(now);
        UpdateSeekSettle(now);
        UpdateExitLoadWait(now);
        UpdateWorldFreeze(now);
        MusicWatch(now);
        PublishEditorSnapshot();

        // The game's HUD (radar, money, cores, place banner) is not part of the
        // replay's picture: hidden while a clip plays.
        if (g_cleanView || g_transport.Playing() || g_editorCoversScreen || replay::exportprobe::HidesHud())
        {
            HUD::HIDE_HUD_AND_RADAR_THIS_FRAME();
            HUD::_HUD_HIDE_THIS_FRAME();
            // The game's button prompts too ("Holster" over the editor's key hints during
            // a clip).
            HUD::_UI_PROMPT_DISABLE_PROMPTS_THIS_FRAME();
        }
        if (g_debugMode && !g_cleanView && g_overlayEnabled && !g_editorCoversScreen && !replay::exportprobe::HidesHud())
            DrawOverlay(frame);

        if (g_debugMode && !g_cleanView && !g_editorCoversScreen && !replay::exportprobe::HidesHud())
            DrawStatus(now);

        {
            // The evidence a recording stop handed to worker threads (per-record lines,
            // the ring's speech lines, the clip pointer audit), written with the log filter
            // as it was in the tick that asked for it, 64 KB of whole lines per tick.
            REPLAY_PROFILE_ZONE("tick.deferred_log");
            replay::deferredlog::Log().Poll(
                [](std::string_view text, bool quiet) {
                    g_logFilter.SetQuiet(quiet);
                    LogFile().write(text.data(), static_cast<std::streamsize>(text.size()));
                    g_logFilter.SetQuiet(replay::log::Quiet());
                },
                64 * 1024);
        }

        const std::uint64_t tickTicks = replay::prof::Now() - tickStart;
        UpdateScrubCost(static_cast<double>(tickTicks) * 1e6 / static_cast<double>(replay::prof::TicksPerSecond()));
        replay::prof::EndFrame(tickTicks);
        // The game frame by mode, the replay buffer apart from a recording.
        replay::prof::MarkGameFrame(replay::prof::Now(),
            g_recording && g_recordKind == RecordKind::Buffer ? "buffer" : ProfilerMode());
        TickShakeMigration();
        UpdateProfilerReport(now);
        // One flush per tick so a crash loses at most a frame of log.
        LogFile().flush();
        }
        catch (const std::exception& exception)
        {
            NoteTickException(exception.what());
        }
        catch (...)
        {
            NoteTickException("unknown");
        }
        WAIT(0);
    }
}
