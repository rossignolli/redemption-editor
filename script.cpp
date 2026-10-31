// script.cpp: the module state, the log file, the clip picker and clip read, the music plan, StopPlayback, the unload (OnModuleDetach), the keyboard, the bench, the world reload and the safe exit, ScriptMain.
// The other parts are in src/script_*.cpp; shared internals in script_internal.h.

#include "script_internal.h"
#include "src/reshade_check.h"

namespace
{

    constexpr float kPi = 3.14159265358979323846f;
    constexpr ULONGLONG kMaxRecordingMs = 60000;
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
}
    // Debug mode (RedemptionEditor.ini [RedemptionEditor] Debug=1): the
    // development setup, the text HUD and the direct keys, with no product-only
    // steps (the editor's playback bar, the clip load presentation). Without it
    // the mod runs as the product: the editor is the only interface.
    bool g_debugMode = false;
namespace
{
    bool g_cleanView = false; // bench camera preview: no HUD of any kind
}
    volatile bool g_editorCoversScreen = false; // the ImGui editor is open over the game (read by the key handler)
    // The editor draws the loading cue while the world loads (CommandType::SetLoadingCue,
    // sent every 2 s): its tick count, 0 never. Within 5 s of it the core's own
    // "Loading replay..." text is left out (the cue has the caption).
    volatile ULONGLONG g_editorLoadingCueAt = 0;
    bool EditorDrawsLoadingCue() { return g_editorLoadingCueAt != 0 && GetTickCount64() - g_editorLoadingCueAt < 5000; }
    // Simple camera: Insert adds a keyframe at the playhead from the free camera,
    // Delete removes the nearest one, End switches the track.
    volatile LONG g_cameraKeyAddRequested = 0;
    volatile LONG g_cameraKeyDeleteRequested = 0;
    volatile LONG g_cameraTrackToggleRequested = 0;
    int g_playbackSpeedIndex = kNormalSpeedIndex;
    volatile LONG g_speedStepRequested = 0; // -1 slower, +1 faster
    float g_worldTimeScaleApplied = 1.0f;
namespace
{
    volatile LONG g_togglePlayheadPauseRequested = 0;
}
    volatile LONG g_moveForward = 0;
    volatile LONG g_slowMove = 0;      // Ctrl held: the free camera moves slow
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
    bool g_fcTargetFrameValid = false;
    volatile LONG g_fcActionBits = 0;
    bool g_camOverlayEnabled = true;       // CameraOverlay=1
    bool g_camOverlayWhilePlaying = false; // CameraOverlayWhilePlaying=0: shown paused, in Edit Camera and the free camera
    bool g_camOverlayTimeLabels = true;
    bool g_camOverlayTrail = true;
    // The Spline defaults (RedemptionEditor.ini Spline*; the editor's Spline
    // Defaults rows write them back). A marker's tuning overrides some fields.
    replay::editor::SplineTuning g_splineDefaults = replay::editor::DefaultSplineTuning();
    volatile LONG g_moduleDetaching = 0;
namespace
{
    // Set when another copy of the mod runs in this process (an old ReplayProbe.asi
    // or a second RedemptionEditor.asi). This copy then starts nothing: no hook, no
    // thread, no recording; it only says so in the log and on screen.
    volatile LONG g_refusedStart = 0;
}
    // The game restarted its scripts (ScriptMain ran again in this module) and
    // ShutdownSessionAfterRestart zeroed the session. The snapshot carries the
    // count (sessionResetSequence): the add-on closes its screens when it grows.
    // The notice for the player waits for the end of the loading screen.
    std::uint32_t g_sessionResets = 0;
namespace
{
    bool g_sessionRestarted = false; // this ScriptMain is a restart (set before the setup)
    std::string g_sessionResetNotice;
}
    // [Keys] values refused at load, said once the game is up.
    std::string g_keysNotice;
namespace
{
    // Why the editor add-on is not loaded (reshade_check.h): checked once per
    // game when it is playable, and again on the editor key.
    bool g_reshadeChecked = false;
    volatile LONG g_editorKeyWithoutAddon = 0;
    ULONGLONG g_reshadeKeyNoticeAt = 0;
}

    // The key the table holds for an action ("F7", "J / U"), or "unbound" (texts
    // of the game notices).
    std::string KeyText(replay::keys::Action action)
    {
        const std::string text = replay::keys::Keys(replay::keys::Current(), action);
        return text.empty() ? std::string("unbound") : text;
    }
namespace
{
    // The exit from the replay that reloads the save sets this before it pushes
    // the load, with the clip and the project already saved. The restart that
    // follows reads it (SESSION_RESET,why=own_reload): not a loss, nothing to tell
    // the player.
    replay::session::ExitReloadRequest g_exitReloadPending;
}
    WorldModelSession g_world;
    // The last HOOK_WARMUP (SESSION_CHECK after a restart).
    MH_STATUS g_lastWarmUpStatus = MH_OK;
    int g_lastWarmUpApplied = 0;
    // Tick count of the last Ctrl+R (ScriptHook's reload key with the .dev file).
    // The unload's DllMain cannot run natives, so a paused replay's world freeze
    // is lifted by the ticks that still run between the key and the unload
    // (UpdateWorldFreeze).
    volatile LONG64 g_reloadKeyAt = 0;
    bool g_overlayEnabled = true;
    bool g_recording = false;
    std::uint32_t g_savedClipMs = 0; // length of the last clip written (the editor's notice)
    // How long the recording start took and the frame gaps of its first ticks.
    double TicksMs(std::uint64_t ticks)
    {
        return static_cast<double>(ticks) * 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
    }
    RecordingTiming g_recTiming;
    int g_recordingsThisLoad = 0;
    RecordingConfig g_recConfig;
    std::uint32_t g_gateReasons = 0;      // the last tick's reasons
    RecordKind g_recordKind = RecordKind::Manual;
    std::uint32_t g_bufferHeadMs = 0;       // capture clock: the ring's first kept block
    bool g_manualInBuffer = false;          // F4 marked a manual start inside the buffer
    std::uint32_t g_manualStartMs = 0;
    ULONGLONG g_bufferResumeAt = 0;         // restart after a playback or a pause
    // A clip loaded from disk waits for its playback: the buffer stopped for the
    // load would otherwise restart on the next tick (nothing is playing) and wipe
    // the loaded clip. Cleared when a playback or a recording starts.
    bool g_loadedClipHoldsBuffer = false;
    int g_manualPart = 0;
    // The clip start's world readiness log and the staggered puppet start
    // (clip_start_gate.h).
    replay::clipstart::Config g_clipStartConfig;
    std::uint32_t g_clipWorldLines = 0;
    std::uint64_t g_clipWorldLastLogMs = 0;
    std::uint32_t g_clipWorldLastBits = ~0u;
    std::uint64_t g_clipStartTogglesBefore = 0; // the IPL toggles when this clip's start began (before the stop)
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
    replay::camera::Track g_cameraTrack;
    bool g_cameraTrackEnabled = true;
    bool g_cameraTrackManualOverride = false;
namespace
{

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
}

    void DrawText(const char* text, float x, float y, float scale, int r, int g, int b, int a)
    {
        replay::hud::DrawText(text, x, y, scale, r, g, b, a);
    }

    void ShowObjective(const char* message) { replay::hud::ShowObjective(message); }
namespace
{

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
}
    QuietFilter g_logFilter;

    // The DOF, target and streaming work (CONFIG_CAMERA2): the DOF interpolated
    // in a blend, the focus hysteresis and the focus distance spring.
    replay::camera::FocusConfig g_camFocusConfig;
    // The view lent to another mod's script camera (ini ReplayCameraRelease,
    // camera_cut.h Release), and whether it is lent now.
    // The streaming focus follows the rendering camera every tick while the replay
    // owns the world (world_focus_core.h); CLEAR_FOCUS only when the playback stops.
    replay::worldfocus::Tracker g_worldFocusTracker;
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
    // Edit Camera's view is resolved again once the puppets stand at the marker's
    // time (UpdateFreeCam).
    bool g_editResolvePending = false;
    bool g_freeCamBeforeEdit = false; // restored when Edit Camera ends
namespace
{

    // PoseTrack v2 entity registry, recording side. Every recorded entity gets a
    // ReplayId (slot + instance). Its create and delete packets go to the
    // lifecycle track, which opens every block with a full create of the live
    // entities, and its pose record in replay::pose::capture is keyed by the same
    // id. Entities enter and leave by the proximity scan below, and also leave when
    // they stop existing or their skeleton is replaced (a new entity for the replay).
    constexpr std::uint32_t kPoseV2SampleHz = replay::recorder::kSampleHz;
}

    // The class capacities are replay::capacity::PedCapacity() (ini PedCapacity)
    // and replay::capacity::VehicleCapacity() (8, or the vehicle pool less 1
    // with WorldModel=gta).

    // The clip in memory (src/replay_recording.h): the recorder fills it,
    // playback reads it.
    replay::Recording g_clip;
namespace
{
    // The recorder's registry (src/replay_recorder.h): engine entities
    // being recorded and what the recorder remembers about them.
    replay::recorder::Recorder g_recorder;
}
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
    std::uint32_t g_clipSort = 0; // 0 newest first, 1 longest first
    std::uint64_t g_editorClipListSequence = 0;
    std::wstring g_currentClipPath;
    // The file that holds the clip in memory (loaded, or written from it), empty
    // for a buffer window played before it was saved: the editor's save writes
    // there (g_currentClipPath also names the last buffer window saved, another
    // clip). g_clipEdited: markers changed since.
    std::wstring g_memoryClipPath;
    bool g_clipEdited = false;

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
        // The deadband smoothing (pose_deadband_smooth.h): done on the read
        // thread for a preloaded read, here for a direct one. A queued save's
        // snapshot holds the session's tracks (already fine or smoothed).
        if (ok && !fromQueue && replay::pose::smooth::LoadSmoothing())
        {
            const bool onThread = preloaded && preloaded->path == path && preloaded->posesSmoothed;
            replay::pose::smooth::RecordsSummary summary;
            ULONGLONG smoothMs = 0;
            if (onThread)
            {
                summary = preloaded->smoothSummary;
                smoothMs = preloaded->smoothMs;
            }
            else
            {
                const ULONGLONG smoothStart = GetTickCount64();
                replay::pose::smooth::SmoothRecords(poses, summary);
                smoothMs = GetTickCount64() - smoothStart;
            }
            LogFile() << "CLIP_POSE_SMOOTH,records=" << summary.records << ",smoothed=" << summary.smoothed
                      << ",failed=" << summary.failed << ",frames=" << summary.frames << ",smoothed_frames=" << summary.smoothedFrames
                      << ",bytes_in=" << summary.bytesIn << ",bytes_out=" << summary.bytesOut << ",world_spikes=" << summary.worldSpikes
                      << ",spike_records=" << summary.spikeRecords << ",ms=" << smoothMs
                      << ",thread=" << (onThread ? "read" : "script") << '\n';
        }
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
namespace
{
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

    void ApplyMusicPlan(const replay::music::Plan& plan)
    {
        if (plan.setFlag)
            AUDIO::SET_AUDIO_FLAG(replay::music::kFlagName, plan.flagValue ? TRUE : FALSE);
        if (plan.stopScenes)
            AUDIO::STOP_AUDIO_SCENES(); // this script's slot only (the scenes this script started)
    }
}

    // The replay takes the world (end of BeginClipWorld, the screen going black in
    // product mode). With the save reload this goes after the scripts' cleanup:
    // their FAIL/STOP events would play over a stop made before it.
    void MusicEnter()
    {
        const bool playingBefore = AUDIO::AUDIO_IS_MUSIC_PLAYING() != FALSE;
        const int playtime = AUDIO::GET_MUSIC_PLAYTIME();
        const replay::music::Plan plan = g_music.Enter(replay::music::Mode::Natives, GetTickCount64());
        if (plan.already)
        {
            LogFile() << "MUSIC_STOP,phase=enter,mode=1,already=1\n";
            return;
        }
        ApplyMusicPlan(plan);
        const bool eventOk = plan.triggerStop && AUDIO::TRIGGER_MUSIC_EVENT(replay::music::kStopEvent) != FALSE;
        LogFile() << "MUSIC_STOP,phase=enter,mode=1,playing_before=" << (playingBefore ? 1 : 0) << ",playtime_ms=" << playtime
                  << ",flag=1,event=" << replay::music::kStopEvent << ",event_ok=" << (eventOk ? 1 : 0)
                  << ",internal=skip\n";
    }
namespace
{

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
#if defined(REDEMPTION_DEV) // the bench's fault commands (replay_bench.h): developer build only
    bool g_benchThrowNextTick = false; // bench "safe_exit exception"
    bool g_benchHoldNormalExit = false; // bench "safe_exit hold_exit": the normal exit is skipped
#endif
}

    void StopPlayback()
    {
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
namespace
{

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
            std::snprintf(text, sizeof(text), "what=%s,world_focus=1,source=%s,player_is_focus=%d", what.c_str(),
                replay::worldfocus::SourceName(g_worldFocusTracker.last), playerFocus ? 1 : 0);
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
            replay::camera::InheritFrom(marker, *previous, replay::camera::InsideBlend(g_cameraTrack, marker.timeMs));
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
            else if (key == "pos" && sscanf_s(value.c_str(), "%f,%f,%f", &a, &b, &c) == 3)
                view.position = {a, b, c};
            else if (key == "near" && sscanf_s(value.c_str(), "%f,%f,%f", &a, &b, &c) == 3)
            {
                // World metres from the attach target (else the Look At one, else the main
                // character) at the playhead, looking at its head unless rot= is given.
                nearSet = true;
                nearOffset = {a, b, c};
            }
            else if (key == "rot" && sscanf_s(value.c_str(), "%f,%f,%f", &a, &b, &c) == 3)
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
        if (FILE* file = _wfsopen(StateFile(kUnloadReportFile).c_str(), L"w", _SH_DENYNO))
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
    // The add-on's key; without the add-on or the own overlay nothing opened, so the script says why.
    case Action::Editor:
        if (!GetModuleHandleW(replay::identity::kAddonFileW) && !GetModuleHandleW(replay::identity::kHostFileW))
            InterlockedExchange(&g_editorKeyWithoutAddon, 1);
        break;
    default:
        break; // FrameBack, FrameForward, Rewind, FastForward (held, above); HideUi (the add-on's)
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
#pragma warning(push)
#pragma warning(disable : 4127) // kDevBuild is constant in the public build
    if (replay::identity::kDevBuild && key == VK_F9)
        InterlockedExchange(&g_toggleOverlayRequested, 1);
    else if (replay::identity::kDevBuild && key == VK_F8)
        InterlockedExchange(&g_loadNextClipRequested, 1);
#pragma warning(pop)
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

#if defined(REDEMPTION_DEV) // the bench executor exists in the developer build only (replay_bench.h)
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
#endif // REDEMPTION_DEV

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
    bool CallLoadFromSlot(std::uintptr_t function, int slot, DWORD& code)
    {
        if (replay::engine::Call(replay::engine::kLoadFromSlot, function, slot))
            return true;
        code = replay::engine::LastFault();
        return false;
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
        std::string checkError;
        PushState push;
        push.path = ReloadPath::Slot; // the menu's LoadFromSlot with the newest slot
        push.signaturesOk = WorldModelChecks(checkError);
        push.savePending = SAVE::SAVEGAME_IS_SAVE_PENDING() != FALSE;
        push.loadingScreen = SCRIPTS::IS_LOADING_SCREEN_VISIBLE() != FALSE;
        SlotSummary slots;
        push.newestSlot = ReadWorldSlots(slots) ? slots.newest : -1;
        {
            std::int32_t menuStatus = 0;
            push.menuOpBusy = !replay::mem::Copy(&menuStatus, replay::sig::Find(kMenuOpStatus), sizeof(menuStatus)) ||
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
            << ",slot_at_clear=" << g_world.clearedSlot << ",menu_busy=" << (push.menuOpBusy ? 1 : 0) << '\n';
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
        {
            const bool called = CallLoadFromSlot(replay::sig::Find(kLoadFromSlot), push.newestSlot, code);
            std::int32_t status = -1;
            replay::mem::Read(replay::sig::Find(kMenuOpStatus), status);
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
            std::int32_t status = -1;
            replay::mem::Read(replay::sig::Find(kMenuOpStatus), status);
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
        state.step = kStepReload;
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
        if (g_benchHoldNormalExit && verdict == ExitVerdict::Reload)
            verdict = ExitVerdict::Settle; // bench: the watchdog must find the stuck world
#endif
        if (verdict == ExitVerdict::Reload)
            WorldModelReload(now);
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

    // A file of the game folder, at most `limit` bytes, read while ReShade
    // keeps it open for writing. `since`: a FILETIME; a file last written
    // before it reads as "" (another session's log). `found` and `fresh`
    // say which.
    std::string ReadSharedFile(const char* name, std::size_t limit, ULONGLONG since, bool& found, bool& fresh)
    {
        found = false;
        fresh = false;
        HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return std::string();
        found = true;
        FILETIME written{};
        GetFileTime(file, nullptr, nullptr, &written);
        const ULONGLONG writtenAt = (static_cast<ULONGLONG>(written.dwHighDateTime) << 32) | written.dwLowDateTime;
        fresh = since == 0 || writtenAt >= since;
        std::string text;
        if (fresh)
        {
            LARGE_INTEGER size{};
            GetFileSizeEx(file, &size);
            text.resize(static_cast<std::size_t>((std::min<LONGLONG>)(size.QuadPart, static_cast<LONGLONG>(limit))));
            DWORD read = 0;
            if (!text.empty() && !ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
                read = 0;
            text.resize(read);
        }
        CloseHandle(file);
        return text;
    }

    // What Windows and ReShade say about the editor add-on (reshade_check.h).
    replay::reshadecheck::State CheckReShade(const char* why)
    {
        using namespace replay::reshadecheck;
        Facts facts;
        facts.addonLoaded = GetModuleHandleW(replay::identity::kAddonFileW) != nullptr;
        facts.hostLoaded = GetModuleHandleW(replay::identity::kHostFileW) != nullptr;
        bool exported = false;
        HMODULE modules[1024];
        DWORD needed = 0;
        if (!facts.addonLoaded && !facts.hostLoaded && K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        {
            const DWORD count = (std::min<DWORD>)(needed / sizeof(HMODULE), 1024);
            for (DWORD n = 0; n < count && !exported; ++n)
                exported = GetProcAddress(modules[n], "ReShadeRegisterAddon") != nullptr;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const ULONGLONG started = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        bool logFound = false, logFresh = false, iniFound = false, iniFresh = false;
        if (!facts.addonLoaded && !facts.hostLoaded)
        {
            facts.log = ReadSharedFile(kLogFile, kLogReadMax, started, logFound, logFresh);
            facts.ini = ReadSharedFile(kIniFile, 1u << 20, 0, iniFound, iniFresh);
        }
        facts.reshadeRunning = exported || logFresh;
        facts.addonFilePresent = GetFileAttributesA(kAddonFile) != INVALID_FILE_ATTRIBUTES;
        const State state = Classify(facts);
        LogFile() << "RESHADE_CHECK,why=" << why << ",state=" << Name(state) << ",addon_module=" << (facts.addonLoaded ? 1 : 0)
                  << ",host_module=" << (facts.hostLoaded ? 1 : 0)
                  << ",reshade_export=" << (exported ? 1 : 0) << ",log=" << (!logFound ? "none" : logFresh ? "fresh" : "stale")
                  << ",ini=" << (iniFound ? 1 : 0) << ",addon_file=" << (facts.addonFilePresent ? 1 : 0) << '\n';
        return state;
    }

    // Once per game, when it is playable: the add-on is in, or the player is
    // told why not. Again on the editor key while it is not (at most every
    // kKeyNoticeGapMs).
    void ServiceReShadeCheck(ULONGLONG now)
    {
        const bool keyPressed = InterlockedExchange(&g_editorKeyWithoutAddon, 0) != 0;
        if (g_reshadeChecked && !keyPressed)
            return;
        if (DLC::GET_IS_LOADING_SCREEN_ACTIVE() || SCRIPTS::IS_LOADING_SCREEN_VISIBLE() || CAM::IS_SCREEN_FADED_OUT() ||
            !PLAYER::IS_PLAYER_CONTROL_ON(PLAYER::PLAYER_ID()))
        {
            if (keyPressed)
                InterlockedExchange(&g_editorKeyWithoutAddon, 1); // told once the screen is up
            return;
        }
        const bool first = !g_reshadeChecked;
        g_reshadeChecked = true;
        if (!first && g_reshadeKeyNoticeAt && now - g_reshadeKeyNoticeAt < replay::reshadecheck::kKeyNoticeGapMs)
            return;
        const replay::reshadecheck::State state = CheckReShade(first ? "startup" : "editor_key");
        const std::string text = replay::reshadecheck::NoticeText(state, replay::keys::Current());
        if (text.empty())
            return;
        g_reshadeKeyNoticeAt = now;
        replay::hud::ShowNotice(text.c_str());
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
    {
        // The game code the mod relies on, found by signature before any module
        // reads it or puts a detour in (game_sig.h).
        char folder[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, folder);
        const std::string iniPath = replay::identity::IniPathIn(folder);
        replay::sig::Configure(replay::identity::DevIniInt("SigForceScan", 0, iniPath.c_str()) != 0);
        replay::sig::ResolveAll();
        // SigDumpCode (dev): this exe's code for tools/sigs/sigtool.py, once
        // per load (a new game build: which signatures broke).
        static bool dumped = false;
        if (!dumped && replay::identity::DevIniInt("SigDumpCode", 0, iniPath.c_str()) != 0)
        {
            dumped = true;
            wchar_t local[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            std::string report = "error=no_local_app_data";
            bool ok = false;
            if (length && length < MAX_PATH - 64)
            {
                wchar_t parent[MAX_PATH]{};
                swprintf_s(parent, L"%ls\\RedemptionEditor", local);
                CreateDirectoryW(parent, nullptr);
                swprintf_s(parent, L"%ls\\RedemptionEditor\\code", local);
                CreateDirectoryW(parent, nullptr);
                ok = replay::sig::DumpCode(parent, report);
            }
            LogFile() << "SIG_DUMP,ok=" << (ok ? 1 : 0) << ',' << report << '\n';
        }
        // The game addresses a clip names (clip_sites.h): on another exe, where
        // its signatures were found; the 1491.50 exe keeps the declared ones
        // (the default), so its clips read exactly as before.
        const bool known = replay::sig::KnownExe();
        const std::uintptr_t base = replay::sig::ImageBase();
        const replay::sig::SiteMap live = replay::clipsites::BuildSiteMap([base](const replay::sig::Signature& s) {
            const std::uintptr_t at = replay::sig::Find(s);
            return at > base ? static_cast<std::uint32_t>(at - base) : 0u;
        });
        if (!known)
            replay::clipfile::SetSiteMap(live);
        LogFile() << "CLIP_SITES,map=" << (known ? "known" : "live") << ",found=" << live.Ranges().size()
                  << ",declared=" << replay::clipsites::KnownSiteMap().Ranges().size() << '\n';
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
    LogFile() << "MUSIC_STOP,phase=startup,flag=0,mode=1\n";
    // A freeze left by a reload in a paused replay.
    if (FILE* file = _wfsopen(StateFile(kWorldFreezeFile).c_str(), L"r", _SH_DENYNO))
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
    if (FILE* file = _wfsopen(StateFile(kUnloadReportFile).c_str(), L"r", _SH_DENYNO))
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
        ServiceReShadeCheck(now);
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
        replay::playback::Update();
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
            // a clip). Not under the quit alert or the pause menu: the alert's
            // "Yes Enter / No Esc" are prompts, and with prompts disabled they were
            // neither drawn nor answered (seen in game: the alert stuck
            // under a paused replay until F7, which ends Playing() and this call,
            // brought them back). FrontendPrompts=0 (developer key) for the old way.
            const bool frontend = g_recConfig.theatre.frontendPrompts && FrontendScreenUp();
            if (!frontend)
                HUD::_UI_PROMPT_DISABLE_PROMPTS_THIS_FRAME();
            static bool s_promptsLifted = false;
            if (frontend != s_promptsLifted)
            {
                s_promptsLifted = frontend;
                LogFile() << "FRONTEND_PROMPTS,lifted=" << (frontend ? 1 : 0) << ",menu=" << (HUD::IS_PAUSE_MENU_ACTIVE() ? 1 : 0)
                          << ",alert=" << (UISTICKYFEED::_UI_STICKY_FEED_IS_ALERT_SCREEN_ACTIVE() ? 1 : 0)
                          << ",playing=" << (g_transport.Playing() ? 1 : 0) << '\n';
            }
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
            // The first fault of each engine function the mod called (engine_calls.h).
            replay::engine::PollFaults(LogFile());
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
