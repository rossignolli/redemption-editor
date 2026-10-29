#pragma once

// What a script restart has to undo (pure; script.cpp ShutdownSessionAfterRestart
// executes the plan).
//
// ScriptHookRDR2 runs ScriptMain again in the same module when the game
// restarts its scripts (a save loaded from the menu, a death, a mission
// reload, the mod's own exit reload). The old fiber is abandoned in its
// WAIT; statics survive. The replay is split the usual way: a session part
// (recording, buffer, playback, editor, markers) shut down and started again,
// and a module part (detours, crash handler, worker, bridge) that stays. This
// file decides the session part from the state the restart found.
//
// Rule of the plan: nothing that names an entity, a camera, a sound id, an
// fx instance, a decal or a fire of the old world is used again (handles and
// engine pointers are freed by the load and the engine hands them out
// again). Tables are emptied; only the global state the replay changed
// (time scale, its own screen filters) and the engine's weak sound slots
// (the engine nulls them when the sound ends) are touched.

#include <cstdint>
#include <sstream>
#include <string>

namespace replay::session
{
    enum class Why : std::uint8_t
    {
        ScriptsRelaunched = 0, // the game restarted its scripts (load, death, mission)
        OwnReload = 1          // the mod reloaded the save itself (g_exitReloadPending)
    };

    inline const char* WhyText(Why why)
    {
        return why == Why::OwnReload ? "own_reload" : "scripts_relaunched";
    }

    // What the recording in memory becomes.
    enum class RecordingFate : std::uint8_t
    {
        None = 0,          // nothing was being recorded
        DiscardBuffer = 1, // the replay buffer: dropped, as a session shutdown frees its buffer
        DiscardManual = 2  // a manual recording (F4): dropped too, the player is told
    };

    inline const char* FateText(RecordingFate fate)
    {
        switch (fate)
        {
        case RecordingFate::DiscardBuffer: return "discard_buffer";
        case RecordingFate::DiscardManual: return "discard_manual";
        default: return "none";
        }
    }

    // The exit reload request: the exit from the replay reloads the save (the
    // world the replay changed goes back by the game's own load). The
    // request is set before the load is pushed, with the clip and the project
    // already saved; the restart that follows finds it.
    struct ExitReloadRequest
    {
        bool pending = false;
        std::uint64_t requestedAtMs = 0; // GetTickCount64 when the load was pushed
        bool reopenEditor = false;       // reopen the editor on the clip after the load
        std::wstring clipPath;           // the clip to reopen ("" none)
        std::wstring projectPath;        // the project to reopen ("" none)
    };

    // A request older than this is not the restart's cause (a load the game
    // refused, a menu left): the restart is the game's, and it is logged.
    constexpr std::uint64_t kExitReloadWindowMs = 180000;

    inline bool IsOwnReload(const ExitReloadRequest& request, std::uint64_t nowMs)
    {
        return request.pending && nowMs >= request.requestedAtMs && nowMs - request.requestedAtMs <= kExitReloadWindowMs;
    }

    // The session state at the restart (read from script.cpp's statics
    // before anything changes).
    struct State
    {
        bool recording = false;
        bool bufferRecording = false; // the recording is the replay buffer
        bool manualInBuffer = false;  // F4 marked a manual part inside the buffer
        std::uint32_t recordingMs = 0;
        bool playing = false;         // the transport has a clip open (playing or paused)
        bool paused = false;
        std::uint32_t puppets = 0;
        bool clipInMemory = false;    // a clip (recorded or loaded) is held in g_clip
        bool clipFromDisk = false;
        bool clipEdited = false;      // markers changed and not saved
        bool editorOpen = false;      // the add-on said it covers the screen
        bool projectOpen = false;
        bool projectDirty = false;
        bool projectOwnsPlayback = false;
        bool freeCam = false;
        bool replayCam = false;
        bool editingMarker = false;
        bool settling = false;
        bool clipLoadWaiting = false;
        bool exitLoadWaiting = false;
        bool worldFrozen = false;
        bool clipWorldActive = false;
        bool timeScaleChanged = false;
        bool clipReadPending = false; // a clip or project read on a thread, or a load waiting for it
        bool openWorldMusicDisabled = false; // OpenWorldMusicDisabled
        bool exitReloadPending = false;      // IsOwnReload of g_exitReloadPending
    };

    struct Plan
    {
        Why why = Why::ScriptsRelaunched;
        RecordingFate recording = RecordingFate::None;
        bool stopRecording = false;     // StopRecording(false): disarms the capture, stops the observers
        bool forgetPlayback = false;    // puppets, overrides, fx, sounds, decals, fires: tables emptied
        bool syncProjectMarkers = false; // the project clip's markers into the project (data)
        bool saveEditedClip = false;    // the clip's marker changes into its file (data, the save thread)
        bool closeProject = false;      // saved first when dirty
        bool dropClip = false;          // g_clip and the pose records emptied (handles of the old world inside)
        bool cancelReads = false;       // clip and project reads, a clip load waiting
        bool forgetCameras = false;     // free cam, replay cam, Edit Camera: handles forgotten, not destroyed
        bool forgetWorldFreeze = false; // the in-memory freeze; the file is thawed by the setup's existing path
        bool resetTimeScale = false;    // SET_TIME_SCALE(1): global, no handle
        bool stopScreenEffects = false; // the replay's own post effects by name: global, no handle
        bool restoreMusic = false;      // the open world music back on
        bool notifyEditor = true;       // the snapshot's sessionResetSequence grows: the add-on closes
        bool tellUser = false;          // a notice: a manual recording or unsaved work was lost
        bool reopenEditor = false;      // own reload asking for it
        bool resumeBuffer = true;       // the buffer starts again by UpdateBufferAutoStart (after the load)
    };

    inline Plan Decide(const State& s)
    {
        Plan plan;
        plan.why = s.exitReloadPending ? Why::OwnReload : Why::ScriptsRelaunched;
        if (s.recording)
        {
            plan.stopRecording = true;
            plan.recording = s.bufferRecording && !s.manualInBuffer ? RecordingFate::DiscardBuffer : RecordingFate::DiscardManual;
        }
        const bool playbackState = s.playing || s.puppets > 0 || s.settling || s.clipLoadWaiting || s.clipWorldActive;
        plan.forgetPlayback = playbackState;
        plan.syncProjectMarkers = s.projectOpen && s.projectOwnsPlayback;
        // A gallery clip's marker edits are its file's (SaveEditedClip copies
        // g_clip on the script thread, writes on the save thread: no native).
        plan.saveEditedClip = s.playing && s.clipEdited && !s.projectOwnsPlayback;
        plan.closeProject = s.projectOpen;
        plan.dropClip = s.clipInMemory || s.recording;
        plan.cancelReads = s.clipReadPending;
        plan.forgetCameras = s.freeCam || s.replayCam || s.editingMarker;
        plan.forgetWorldFreeze = s.worldFrozen;
        plan.resetTimeScale = s.timeScaleChanged;
        plan.stopScreenEffects = playbackState;
        plan.restoreMusic = s.openWorldMusicDisabled;
        // The mod's own reload saved the clip and the project before the
        // push: nothing is lost, nothing to tell.
        plan.tellUser = plan.why == Why::ScriptsRelaunched && plan.recording == RecordingFate::DiscardManual;
        plan.reopenEditor = false; // decided from ExitReloadRequest::reopenEditor
        return plan;
    }

    // Anything at all to undo (a restart on a quiet session logs it too).
    inline bool AnythingToUndo(const Plan& plan)
    {
        return plan.stopRecording || plan.forgetPlayback || plan.closeProject || plan.dropClip || plan.cancelReads ||
            plan.forgetCameras || plan.forgetWorldFreeze || plan.resetTimeScale || plan.restoreMusic;
    }

    // SESSION_RESET: what the restart found and what the plan does.
    inline std::string FormatLine(const State& s, const Plan& plan)
    {
        std::ostringstream line;
        line << "SESSION_RESET,why=" << WhyText(plan.why) << ",recording=" << (s.recording ? 1 : 0)
             << ",kind=" << (!s.recording ? "none" : s.bufferRecording ? (s.manualInBuffer ? "buffer_manual" : "buffer") : "manual")
             << ",recording_ms=" << s.recordingMs << ",fate=" << FateText(plan.recording) << ",playing=" << (s.playing ? 1 : 0)
             << ",paused=" << (s.paused ? 1 : 0) << ",puppets=" << s.puppets << ",clip=" << (s.clipInMemory ? 1 : 0)
             << ",clip_from_disk=" << (s.clipFromDisk ? 1 : 0) << ",clip_edited=" << (s.clipEdited ? 1 : 0)
             << ",editor=" << (s.editorOpen ? 1 : 0) << ",project=" << (s.projectOpen ? 1 : 0)
             << ",project_dirty=" << (s.projectDirty ? 1 : 0) << ",freecam=" << (s.freeCam ? 1 : 0)
             << ",replay_cam=" << (s.replayCam ? 1 : 0) << ",editing_marker=" << (s.editingMarker ? 1 : 0)
             << ",settling=" << (s.settling ? 1 : 0) << ",clip_load_wait=" << (s.clipLoadWaiting ? 1 : 0)
             << ",exit_load_wait=" << (s.exitLoadWaiting ? 1 : 0) << ",world_frozen=" << (s.worldFrozen ? 1 : 0)
             << ",clip_world=" << (s.clipWorldActive ? 1 : 0) << ",time_scale=" << (s.timeScaleChanged ? 1 : 0)
             << ",reads=" << (s.clipReadPending ? 1 : 0) << ",music_off=" << (s.openWorldMusicDisabled ? 1 : 0)
             << ",forget_playback=" << (plan.forgetPlayback ? 1 : 0) << ",save_edited=" << (plan.saveEditedClip ? 1 : 0)
             << ",close_project=" << (plan.closeProject ? 1 : 0) << ",drop_clip=" << (plan.dropClip ? 1 : 0)
             << ",tell_user=" << (plan.tellUser ? 1 : 0);
        return line.str();
    }
}
