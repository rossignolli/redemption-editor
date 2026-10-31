// script_internal.h: the state and helpers script.cpp and the src/script_*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "script.h"
#include "src/camera_track.h"
#include "src/i18n.h"
#include "src/camera_marker_shake.h"
#include "src/camera_bones.h"
#include "src/camera_spline.h"
#include "src/clip_file.h"
#include "src/clip_sites.h"
#include "src/clip_save_queue.h"
#include "src/clip_load_wait.h"
#include "src/buffer_coverage.h"
#include "src/clip_start_gate.h"
#include "src/world_landing.h"
#include "src/object_check.h"
#include "src/clip_compress.h"
#include "src/clip_compress_worker.h"
#include "src/zone_names.h"
#include "src/model_names.h"
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
#include "src/export_channel.h" // render export: latency tag geometry
#include "src/export_hold.h" // render export: the hold's signal bits
#include "src/export_probe.h" // render export
#include "src/fire_observer.h"
#include "src/audio_observer.h"
#include "src/sound_census.h"
#include "src/mix_probe.h"
#include "src/world_focus_core.h"
#include "src/game_listener.h"
#include "src/game_listener_core.h" // FollowsRecordedCamera
#include "src/game_sig.h"
#include "src/engine_calls.h"
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
#include "src/pose_deadband_smooth.h"
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
#include <share.h> // _SH_DENYNO
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>


    ULONGLONG PlaybackDurationMs();
    std::size_t PoseV2RecordedCount();
    std::size_t PoseV2PuppetCount();
    extern bool g_debugMode;
    extern volatile bool g_editorCoversScreen;
    extern volatile ULONGLONG g_editorLoadingCueAt;
    bool EditorDrawsLoadingCue();
    extern volatile LONG g_cameraKeyAddRequested;
    extern volatile LONG g_cameraKeyDeleteRequested;
    extern volatile LONG g_cameraTrackToggleRequested;
    // Playback speed: the marker speeds, [ and ] step through them. Below 1x the
    // world slows with it (SET_TIME_SCALE: particles, fire, the game's own
    // slow-motion audio), restored to 1 on pause, scrub, stop.
    constexpr double kPlaybackSpeeds[] = {0.05, 0.20, 0.35, 0.50, 1.00, 1.25, 1.50, 1.75, 2.00};
    constexpr int kNormalSpeedIndex = 4;
    extern int g_playbackSpeedIndex;
    extern volatile LONG g_speedStepRequested;
    extern float g_worldTimeScaleApplied;
    extern volatile LONG g_moveForward;
    extern volatile LONG g_slowMove;
    extern volatile LONG g_moveBackward;
    extern volatile LONG g_moveLeft;
    extern volatile LONG g_moveRight;
    extern volatile LONG g_moveUp;
    extern volatile LONG g_moveDown;
    extern volatile LONG g_turnLeft;
    extern volatile LONG g_turnRight;
    extern volatile LONG g_turnUp;
    extern volatile LONG g_turnDown;
    extern volatile LONG g_rollLeft;
    extern volatile LONG g_rollRight;
    extern volatile LONG g_fovIn;
    extern volatile LONG g_fovOut;
    extern volatile LONG g_fastMove;
    extern replay::freecam::Settings g_fcSettings;
    extern replay::freecam::State g_fcState;
    extern replay::freecam::Follow g_fcFollow;
    extern std::uint32_t g_fcTarget;
    extern bool g_fcLocked;
    extern bool g_fcPadActive;
    extern bool g_fcTargetLost;
    extern bool g_fcTargetFrameValid;
    extern volatile LONG g_fcActionBits;
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
    extern bool g_camOverlayEnabled;
    extern bool g_camOverlayWhilePlaying;
    extern bool g_camOverlayTimeLabels;
    extern bool g_camOverlayTrail;
    extern replay::editor::SplineTuning g_splineDefaults;
    extern volatile LONG g_moduleDetaching;
    extern std::uint32_t g_sessionResets;
    extern std::string g_keysNotice;
    std::string KeyText(replay::keys::Action action);
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
    extern WorldModelSession g_world;
    extern MH_STATUS g_lastWarmUpStatus;
    extern int g_lastWarmUpApplied;
    extern volatile LONG64 g_reloadKeyAt;
    extern bool g_overlayEnabled;
    extern bool g_recording;
    extern std::uint32_t g_savedClipMs;
    double TicksMs(std::uint64_t ticks);

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
    extern RecordingTiming g_recTiming;
    extern int g_recordingsThisLoad;
    struct RecordingConfig
    {
        bool buffer = true; // always recording by default
        std::uint32_t bufferSeconds = 60;
        std::uint32_t bufferMB = 128;
        std::uint32_t manualMaxSeconds = 0;
        std::uint32_t manualMaxMB = 900;
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
        // PoseDetail (standard | maximum; pose_deadband_smooth.h); PoseCodecFine=0
        // (dev) records with the old coarse codec instead, for an A/B.
        replay::pose::smooth::Detail poseDetail = replay::pose::smooth::Detail::Standard;
        bool poseCodecCoarse = false;
        // WorldModel (off | gta).
        replay::worldmodel::Mode worldModel = replay::worldmodel::Mode::Off;
        // The closed theater: the engine's own functions, one key each.
        replay::theatre::Config theatre;
    };
    extern RecordingConfig g_recConfig;
    extern std::uint32_t g_gateReasons;
    enum class RecordKind { Manual, Buffer };
    extern RecordKind g_recordKind;
    extern std::uint32_t g_bufferHeadMs;
    extern bool g_manualInBuffer;
    extern std::uint32_t g_manualStartMs;
    extern ULONGLONG g_bufferResumeAt;
    extern bool g_loadedClipHoldsBuffer;
    extern int g_manualPart;
    extern replay::clipstart::Config g_clipStartConfig;
    extern std::uint32_t g_clipWorldLines;
    extern std::uint64_t g_clipWorldLastLogMs;
    extern std::uint32_t g_clipWorldLastBits;
    extern std::uint64_t g_clipStartTogglesBefore;
    extern replay::transport::Transport g_transport;
    extern volatile LONG g_scrubBackHeld;
    extern volatile LONG g_scrubForwardHeld;
    extern bool g_freeCamEnabled;
    extern bool g_freeCamPlayerWasFrozen;
    extern ULONGLONG g_recordingStartedAt;
    extern Ped g_freeCamPlayer;
    extern Cam g_freeCam;
    extern Cam g_replayCam;
    extern bool g_replayCamRendering;
    extern Vector3 g_freeCamPosition;
    extern Vector3 g_freeCamRotation;
    extern Vector3 g_freeCamVelocity;
    extern float g_freeCamFov;
    extern float g_freeCamSmoothedLookX;
    extern float g_freeCamSmoothedLookY;
    extern replay::camera::Track g_cameraTrack;
    extern bool g_cameraTrackEnabled;
    extern bool g_cameraTrackManualOverride;

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
    void DrawText(const char* text, float x, float y, float scale, int r, int g, int b, int a);
    void ShowObjective(const char* message);

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
    extern QuietFilter g_logFilter;
    extern replay::camera::FocusConfig g_camFocusConfig;
    // The view is lent to another mod's script camera only while the replay is
    // paused (camera_cut.h Release::Paused).
    constexpr replay::camcut::Release kCamRelease = replay::camcut::Release::Paused;
    extern replay::worldfocus::Tracker g_worldFocusTracker;
    extern Vector3 g_worldFocusAt;
    std::ofstream& LogFile();
    void RefreshKnownModules();
    void InstallCrashEvidence();
    void RemoveCrashEvidence();
    std::string Narrow(const std::wstring& text); // below, with the clip paths
    extern float g_fcNearClip;
    float FreeCamValue(replay::editor::FreeCamField field);
    bool SetFreeCamValue(replay::editor::FreeCamField field, float value);
    void SaveFreeCamOptions();
    void SaveSplineDefaults();
    void LogSplineDefaults(const char* why);
    void SaveOverlayOptions();
    void LogFreeCamConfig(const char* why);
    extern bool g_moonReplay;
    extern std::uint32_t g_gameLanguage;
    void UpdateLanguage(ULONGLONG now);
    void WriteLogHeader();
    const char* TransportLabel();
    void DrawOverlay(const ProbeFrame& f);
    void DrawStatus(ULONGLONG now);
    void StopPlayback();
    bool StartPlayback(ULONGLONG now);
    void StartRecording(RecordKind kind = RecordKind::Manual);
    void StopRecording(bool save = true);
    extern std::uint32_t g_editingMarker;
    extern bool g_editResolvePending;
    extern bool g_freeCamBeforeEdit;
    void LeaveMarkerCameraEdit(bool keep);
    extern bool g_trimReady;
    void StopOwnScreenEffects(const char* why);
    extern replay::Recording g_clip;
    extern replay::playback::Puppets g_puppets;
    extern replay::Context g_context;
    extern const wchar_t* kClipFolder;
    extern std::vector<replay::editor::ClipInfo> g_editorClips;
    extern std::vector<replay::editor::ClipInfo> g_editorClipsByDate;
    extern std::uint32_t g_clipLoadSequence;
    extern std::wstring g_pendingLoadPath;
    extern std::uint32_t g_clipSort;
    extern std::uint64_t g_editorClipListSequence;
    extern std::wstring g_currentClipPath;
    extern std::wstring g_memoryClipPath;
    extern bool g_clipEdited;
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
    extern MarkerHistory g_markerHistory;
    void PushMarkerHistory(std::uint32_t type, std::uint32_t marker, const replay::camera::Track* before);
    void RebuildEditorClipList();
    void RecordEnvironment(std::uint32_t nowMs, bool force);
    bool SavePending();
    void FinishPendingSave(bool wait);
    bool CopyQueuedClip(const std::wstring& path, replay::Recording& recording, std::vector<replay::pose::PoseRecordExport>& poses,
        replay::camera::Track& camera, std::uint64_t& durationMs, std::string& error);
    std::size_t HeldClipSaves(); // saves that failed and wait for a retry
    extern std::uint64_t g_clipsDiskBytes;
    extern std::uint64_t g_clipsBytes;
    extern std::uint32_t g_compressedClips;
    void ImageRange(std::uintptr_t& base, std::size_t& size);
    std::wstring ClipPath(const std::wstring& name);
    std::wstring FindClipFile(const std::wstring& fileName);
    extern std::string g_savedClipName;
    extern std::string g_lastWrittenClipName;
    extern std::uint64_t g_savedClipSequence;
    bool WriteClipFile(const std::wstring& name, std::string& error);

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
        // The coarse codec tracks smoothed on the read thread (SmoothLoadedPoses).
        bool posesSmoothed = false;
        replay::pose::smooth::RecordsSummary smoothSummary;
        ULONGLONG smoothMs = 0;
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
    extern std::unique_ptr<ClipRead> g_clipRead;
    extern std::unique_ptr<ClipRead> g_projectPreload;
    extern std::vector<std::unique_ptr<ClipRead>> g_projectPreloadDropped;
    bool ClipReadPending();
    std::unique_ptr<ClipRead> BeginClipRead(const std::wstring& path, bool openClip);
    void StartClipRead(const std::wstring& path);
    void CancelClipRead();
    bool ReadClipFile(const std::wstring& path, std::string& error, ClipRead* preloaded);
    std::vector<std::wstring> ListClipFiles();
    std::string WeatherName(std::uint32_t hash);

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
    extern std::unique_ptr<ClipListRead> g_clipListRead;
    bool ClipListLoading();
    void ApplyClipSort();
    void ServiceClipListRead(bool wait);


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
    extern std::unique_ptr<QaReportJob> g_qaReport;
    extern ULONGLONG g_qaReportWaitSince;
    extern bool g_qaReportWaitSaid;
    void StartQaReport();
    void ServiceQaReport(bool wait);
    extern bool g_clipPickerOpen;
    extern std::vector<std::wstring> g_clipPickerFiles;
    extern std::size_t g_clipPickerIndex;
    extern ULONGLONG g_clipPickerLastPress;
    bool TakePoseFinalizer();
    void WarmUpHooks();
    void ServicePoseFinalizer();

    bool SaveRecordingWindow(std::uint32_t fromMs, std::uint32_t toMs, const char* why);
    void PressRecord();
    void SetReplayBuffer(bool enabled, std::uint32_t seconds, std::uint32_t megabytes);
    replay::clipcompress::worker::Settings CompressSettings();
    void SetClipCompression(bool enabled, std::uint32_t days);
    void ServiceClipCompressor();
    void SaveBuffer();
    void UpdateBufferAutoStart(ULONGLONG now);

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
    extern replay::savequeue::Queue<ClipSaveData> g_clipSaves;

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
    extern std::unique_ptr<PendingSave> g_pendingSave;
    void FlushClipSaves();

    // The editor's save (Ctrl+S, and on leaving a clip with changed markers): the
    // clip in memory and its markers over its own file, or a new file for a buffer
    // window never saved, on a thread like the other saves (the file is written
    // aside and renamed: a failed write leaves the old clip whole).
    bool ProjectOwnsPlayback();
    void SyncProjectMarkers();
    void SaveEditedClip(const char* why);
    void StopBufferForPlayback();
    bool WeatherStateReady();
    std::string SkyValuesText(const replay::SkySample& s);
    std::string TimecycleText(const replay::TimecycleSample& s);
    std::uint32_t LiveClockS();
    std::string MoonDateText(const replay::MoonSample& s);
    bool ReadMoonNow(replay::MoonSample& sample);
    void ExcludeRecordingStall();
    void UpdateRecording(const ProbeFrame& frame);
    void StopFreeCam();

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
        int loadSceneLost = 0;    // ours stopped by someone else, started again (clipload::PlanScene)
        int loadSceneRefused = 0; // LOAD_SCENE_START returned false
        replay::clipload::StreamingIdle loadIdle;
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
    extern PlayerLanding g_playerLanding;
    extern ClipWorld g_clipWorld;
    extern replay::moontrack::GiveBack g_moonGiveBack;
    struct MoonCheck
    {
        bool pending = false;
        ULONGLONG at = 0;
        double angle = 0.0;
        double orbit = 0.0;
        std::uint32_t clockS = 0;
        const char* why = "";
    };
    extern MoonCheck g_moonCheck;

    // Where the replay camera renders this tick (set by UpdateReplayCamera): the
    // load scene's target and the long jump test.
    struct RenderedCamera
    {
        bool valid = false;
        replay::clipload::Vec3 position{};
        replay::clipload::Vec3 rotation{}; // degrees, rotation order 2
        std::uint64_t playheadMs = 0;
    };
    extern RenderedCamera g_renderedCamera;
    bool ClipLoadWaiting();
    extern replay::settle::Config g_settleConfig;
    extern replay::settle::Settle g_settle;
    extern replay::settle::DragGate g_seekDrag;
    extern replay::settle::ScrubCost g_scrubCost;
    extern std::uint64_t g_settleLastPlayhead;
    extern const char* g_seekReason;
    extern bool g_seekFine;
    extern bool g_settleCameraCheck;
    extern replay::clipload::Vec3 g_settleCameraFrom;
    extern bool g_settleCameraFromValid;
    extern bool g_settleJumpScene;
    extern std::uint64_t g_preloadCentreMs;
    extern bool g_preloadCentreValid;
    extern ULONGLONG g_nextPreloadAt;
    extern std::uint32_t g_seekLoadSequence;
    extern replay::log::LogLimit g_jumpLoadLogs;
    extern replay::log::LogLimit g_scrubCostLogs;
    extern std::uint32_t g_creationHeldFrames;
    extern bool g_creationHoldSpent;
    extern replay::log::LogLimit g_creationHoldLogs;
    extern std::uint64_t g_creationHolds;
    extern replay::settle::CostCounters g_tickStartCounters;
    extern std::uint64_t g_tickStartPlayhead;
    constexpr std::uint32_t kJumpLoadLogCap = 300;
    void StopClipLoadScene();

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
    extern CamPrefetch g_camPrefetch;
    extern std::uint64_t g_camPrefetchDoneCutMs;
    extern std::uint32_t g_camPrefetchStarted;
    extern std::uint32_t g_camPrefetchLoadedAtCut;
    extern std::uint32_t g_camPrefetchNotLoadedAtCut;
    extern std::uint32_t g_camPrefetchCancelled;
    extern replay::log::LogLimit g_camPrefetchLogs;
    void StopCamPrefetch(const char* why);

    // The start's wait for the map objects the closed theater's clear turned into
    // dummies (doors included): the object pool count holding and the moving doors
    // near the first camera found, bounded by MapObjectsWaitMs (door_rebind.h).
    std::uint32_t WorldPoolUsed(replay::pools::Kind kind);
    void BeginClipLoadWait(const char* phase, ULONGLONG now);
    void LogStreamPressure(const char* phase, const char* reason, ULONGLONG now);
    std::wstring StateFile(const char* name);
    void RestoreLeftClipWorld(bool afterRestart);

    // The clip's environment at the playhead: the clock and the weather pair and
    // mix of the track, interpolated. The weather is set again when it moved or
    // once a second (the weather system moves on by itself). Where the clip starts
    // for the player (the trim's in for a project clip): the override clock and
    // the weather blend count from there.
    std::uint64_t EnvironmentOriginMs();
    replay::envoverride::Recorded RecordedEnvironmentAt(std::uint64_t playheadMs);
    void GiveBackMoonAtUnload();
    extern replay::music::Guard g_music;
    void MusicEnter();
    void BeginClipWorld();
    void UpdateClipWorld(ULONGLONG now);

    // Back to the game behind a loading screen too: the screen stays black while
    // the world around the gameplay camera streams (the load scene, the same rules
    // as the clip load: minimum black, timeout), then fades in.
    struct ExitLoadWait
    {
        bool active = false;
        bool sceneActive = false;
        ULONGLONG startedAt = 0;
    };
    extern ExitLoadWait g_exitLoad;
    void StopExitLoadScene();
    void UpdateExitLoadWait(ULONGLONG now);
    void EndClipWorld();
    extern std::uint32_t g_pendingFollowKeyframe;
    void ResolvePendingFollow();
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
    bool MarkerEntity(std::uint32_t id, std::uint64_t timeMs, replay::camera::EntityState& out);
    const CamTargetBones& TargetBonesFor(const replay::playback::Puppet& puppet);
    std::uint32_t MainCharacterId();
    extern int g_shakeFoldHold;
    bool SplineWeightActive();
    replay::camera::Scene MarkerScene();
    extern replay::camera::SpringState g_cameraSpring;
    void TickShakeMigration();
    bool EvaluateCameraTrack(std::uint64_t timeMs, replay::editor::CameraState& state, float dtSeconds,
        replay::camera::SegmentPlan* plan);
    extern replay::camcut::Tracker g_freeCamCut;

    // The last view PlaceCam set (the replay camera's or the free camera's: one of
    // them renders), for the world overlay.
    struct PlacedView
    {
        bool valid = false;
        replay::editor::CameraState view{};
    };
    extern PlacedView g_lastPlacedView;
    void PlaceCam(Cam cam, replay::camcut::Tracker& tracker, const char* which, float px, float py, float pz, float rx,
        float ry, float rz, float fov, float realDt, bool forceCut);
    replay::camcut::MarkerFrame MarkerFrameOf(bool keyed, const replay::camera::SegmentPlan& plan);

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
    extern LensState g_freeLens;
    extern ULONGLONG g_lensChangedAt;
    extern bool g_lensHeldLogged;
    int PhotoFreezeState();
    void NewLensCamera(LensState& lens);
    extern bool g_replayCamDof;
    extern bool g_replayCamEngineFocus;
    constexpr std::uint16_t kCamFocusPausedBit = 0x4000;
    bool ReadCamFlags(Cam cam, std::uint16_t& flags);
    extern replay::editor::CameraLiveInfo g_cameraLive;
    extern replay::log::LogLimit g_camLensLogs;
    void ApplyNearClip(Cam cam, LensState& lens, float value, const char* which);
    float SendDofBlock(Cam cam, const replay::camera::FocusPlan& focus, float fov);
    void SendFocusDistance(Cam cam, LensState& lens, float distance, const char* which);
    void NoteDofReadback(Cam cam, const char* which, const replay::camera::FocusPlan& focus, float lens, float focusSent,
        std::uint32_t markerId);
    double MarkerSpeedAt(std::uint64_t timeMs); // below, with the marker speed
    void UpdateCameraCutRate(ULONGLONG now);
    void StopReplayCamera();
    void UpdateReplayCamera();
    void UpdateWorldFocus(ULONGLONG now);
    void ReleaseWorldFocus(const char* why);
    bool StartFreeCam(ULONGLONG now);
    replay::camera::LivePose EditStartPose(const replay::camera::Keyframe& key);

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
    extern OrbitEdit g_orbit;
    void ResetFreeCamToView();
    void SetFreeCamTarget(std::uint32_t id, const char* how);
    void ProcessFreeCamActions();
    void UpdateCameraOverlay();
    void UpdateFreeCam(ULONGLONG now);
    void LogTransportPaused(bool paused);
    std::uint64_t PlaybackClockMs();
    extern const char* kWorldFreezeFile;
    extern const char* kUnloadReportFile;
    struct WorldFreeze
    {
        bool active = false;
        std::uint64_t lastPlayheadMs = ~0ull;
        replay::clipload::Vec3 lastCamera{};
        replay::clipload::Vec3 lastCameraRotation{};
        ULONGLONG thawUntil = 0;
        std::uint32_t freezes = 0;
    };
    extern WorldFreeze g_worldFreeze;
    void ThawWorld(const char* why);
    void UpdateWorldFreeze(ULONGLONG now);
    void SetPlaybackPaused(bool paused, ULONGLONG);
    bool WorldModelChecks(std::string& error);
    bool ReadWorldSlots(replay::worldmodel::SlotSummary& summary);
    std::string WorldSlotDate(const replay::worldmodel::SlotSummary& summary);
    std::string WorldPoolCounts();

    // The quit to desktop alert or the pause menu is up (public natives:
    // IS_PAUSE_MENU_ACTIVE, _UI_STICKY_FEED_IS_ALERT_SCREEN_ACTIVE, the latter
    // seen on for the quit alert in game). Such a screen waits for the player:
    // nothing of the replay may hide its prompts or take its input.
    bool FrontendScreenUp();

    // ---- Closed theater ----
    struct TheatreSession
    {
        bool owning = false;          // the replay owns the world (clear to reload)
        bool scriptsPaused = false;
        // The quit alert or the pause menu is up (TheatreFrontendScripts=1:
        // the game's scripts run, paused again after).
        bool frontendSpared = false;
        std::uint64_t frontendSpares = 0;
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
    extern TheatreSession g_theatre;
    extern ULONGLONG g_reloadPushedAtMs;
    extern bool g_reloadDoneLogged;
    extern const char* kTheatreMarkerFile;
    extern replay::theatre::safeexit::Watchdog g_safeExit;
    extern std::uint32_t g_safeExitEditorCloses;
    extern bool g_tickException;
    extern bool g_scriptHookDev;
    void WriteTheatreMarker(bool owning, bool pushed, int slot);
    void RemoveTheatreMarker();
    void NoteTickException(const char* what);
    void TheatreCensus(const char* phase);
    void UpdateTheatre();
    void EndTheatre(const char* why);
    void WorldModelEnter();

    void ProjectBeforePlaybackStop();
    extern std::vector<replay::camera::TimeSpan> g_cinematicSpans;
    extern std::uint32_t g_cinematicSpanLogs;
    extern const replay::camera::TimeSpan* g_cinematicSpanShown;
    bool TrimActive();
    replay::camera::Keyframe GameBoundMarker(std::uint64_t timeMs);

    bool ProjectReachedClipEnd(ULONGLONG now);

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
    extern AbilityAudio g_abilityAudio;
    void StopAbilityAudio(bool withExit);
    replay::settle::CostCounters CostCountersNow();
    void UpdateSeekSettle(ULONGLONG now);
    void UpdateScrubCost(double tickUs);
    std::uint64_t ExportOutputFrames(std::uint64_t startMs, std::uint64_t endMs, replay::exporting::FrameRate rate);
    void UpdatePlayback(ULONGLONG now);
    replay::editor::CameraState CurrentEditorCameraState();
    bool IsValidEasing(replay::editor::Easing easing);
    bool IsValidPath(replay::editor::Path path);

    void AddMarkerAtPlayhead();
    void ProcessCameraKeys();
    extern replay::camera::Keyframe g_copiedMarker;
    extern bool g_hasCopiedMarker;
    void CaptureMarkerRelations(std::uint32_t id, const replay::editor::CameraState& camera);
    void EnterMarkerCameraEdit(std::uint32_t id, ULONGLONG now);
    extern bool g_projectOpen;
    extern std::wstring g_projectPath;
    extern bool g_projectDirty;
    // A project clip whose file is being opened (the gallery's load path).
    struct ProjectPendingOpen
    {
        bool active = false;
        int index = -1;
        std::uint64_t clipMs = 0;
        bool play = false;
        std::wstring path;
    };
    extern ProjectPendingOpen g_projectPending;
    void RebuildProjectList();
    bool SaveProject(const char* why);
    void CancelProjectPreload();
    void ReapProjectPreloads(bool wait);
    void CloseProject(const char* why);
    void UpdateProject(ULONGLONG now);
    void UpdateProjectExport(ULONGLONG now);
    void ProcessEditorCommands(ULONGLONG now);
    void PublishEditorSnapshot();
