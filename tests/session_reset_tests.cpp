// The plan of a script restart (src/session_reset.h) from the state it found,
// the world model request window, the SESSION_RESET line, and the generic
// sound player letting go of its plans without the engine (Player::Abandon).
#include "session_reset.h"
#include "sound_generic_player_core.h"

#include <cstdio>
#include <string>

namespace
{
    using namespace replay::session;

    int g_sessionFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: session_reset: %s\n", message);
        ++g_sessionFailures;
    }

    void QuietSession()
    {
        const Plan plan = Decide(State{});
        Check(plan.why == Why::ScriptsRelaunched, "quiet: why is the game's restart");
        Check(plan.recording == RecordingFate::None, "quiet: no recording fate");
        Check(!plan.stopRecording && !plan.forgetPlayback && !plan.closeProject && !plan.dropClip, "quiet: nothing to stop");
        Check(!plan.tellUser, "quiet: nothing to tell");
        Check(!AnythingToUndo(plan), "quiet: nothing to undo");
        Check(plan.notifyEditor && plan.resumeBuffer, "quiet: the editor is told and the buffer resumes anyway");
    }

    void BufferRecording()
    {
        State s;
        s.recording = true;
        s.bufferRecording = true;
        s.recordingMs = 12000;
        s.clipInMemory = true;
        const Plan plan = Decide(s);
        Check(plan.stopRecording, "buffer: stopped");
        Check(plan.recording == RecordingFate::DiscardBuffer, "buffer: discarded as the reference's ShutdownSession");
        Check(plan.dropClip, "buffer: its clip dropped");
        Check(!plan.tellUser, "buffer: a silent loss (the ring is rolling)");
        Check(!plan.forgetPlayback, "buffer: no playback to forget");
    }

    void ManualRecording()
    {
        State s;
        s.recording = true;
        s.recordingMs = 4000;
        const Plan plan = Decide(s);
        Check(plan.recording == RecordingFate::DiscardManual, "manual: discarded");
        Check(plan.tellUser, "manual: the player is told");
        Check(plan.dropClip, "manual: dropped even before the first duration");

        State marked = s;
        marked.bufferRecording = true;
        marked.manualInBuffer = true;
        const Plan markedPlan = Decide(marked);
        Check(markedPlan.recording == RecordingFate::DiscardManual, "F4 inside the buffer: a manual recording");
        Check(markedPlan.tellUser, "F4 inside the buffer: told");
    }

    void PausedPlayback()
    {
        State s;
        s.playing = true;
        s.paused = true;
        s.puppets = 37;
        s.clipInMemory = true;
        s.clipFromDisk = true;
        s.worldFrozen = true;
        s.replayCam = true;
        s.clipWorldActive = true;
        s.editorOpen = true;
        const Plan plan = Decide(s);
        Check(plan.forgetPlayback, "paused: playback forgotten");
        Check(plan.forgetWorldFreeze, "paused: the freeze dropped");
        Check(plan.forgetCameras, "paused: cameras forgotten");
        Check(plan.stopScreenEffects, "paused: own screen effects stopped");
        Check(plan.dropClip, "paused: the clip dropped (even from disk: the playback state is the old world's)");
        Check(!plan.saveEditedClip, "paused: nothing edited, nothing saved");
        Check(!plan.stopRecording, "paused: no recording");
        Check(AnythingToUndo(plan), "paused: something to undo");

        State edited = s;
        edited.clipEdited = true;
        Check(Decide(edited).saveEditedClip, "edited gallery clip: its markers saved (data)");
        edited.projectOpen = true;
        edited.projectOwnsPlayback = true;
        const Plan projectPlan = Decide(edited);
        Check(!projectPlan.saveEditedClip, "project clip: not into the clip file");
        Check(projectPlan.syncProjectMarkers, "project clip: markers into the project");
        Check(projectPlan.closeProject, "project: closed");
    }

    void LeftoversWithoutTransport()
    {
        // A settle, a clip load wait or puppets left without the transport
        // playing still count as playback state.
        State s;
        s.puppets = 2;
        Check(Decide(s).forgetPlayback, "puppets alone: forgotten");
        State settle;
        settle.settling = true;
        Check(Decide(settle).forgetPlayback, "settle alone: forgotten");
        State world;
        world.clipWorldActive = true;
        Check(Decide(world).forgetPlayback, "clip world alone: forgotten");
        State reads;
        reads.clipReadPending = true;
        const Plan readsPlan = Decide(reads);
        Check(readsPlan.cancelReads && !readsPlan.forgetPlayback, "a read alone: cancelled, nothing else");
        State scale;
        scale.timeScaleChanged = true;
        Check(Decide(scale).resetTimeScale, "time scale: back to 1");
        State freecam;
        freecam.freeCam = true;
        Check(Decide(freecam).forgetCameras, "free cam alone: forgotten");
    }

    void MusicHook()
    {
        State s;
        Check(!Decide(s).restoreMusic, "music: nothing while the flag is off");
        s.openWorldMusicDisabled = true;
        const Plan plan = Decide(s);
        Check(plan.restoreMusic, "music: the music flag on: restored");
        Check(AnythingToUndo(plan), "music: counts as something to undo");
    }

    void OwnReload()
    {
        ExitReloadRequest request;
        Check(!IsOwnReload(request, 1000), "no request: not own");
        request.pending = true;
        request.requestedAtMs = 10000;
        Check(IsOwnReload(request, 10000), "request now: own");
        Check(IsOwnReload(request, 10000 + kExitReloadWindowMs), "request at the window's end: own");
        Check(!IsOwnReload(request, 10000 + kExitReloadWindowMs + 1), "request too old: the game's");
        Check(!IsOwnReload(request, 9999), "request in the future (clock): not own");

        State s;
        s.recording = true; // the exit saved before the push; a manual one left running is still not told
        s.playing = true;
        s.exitReloadPending = true;
        const Plan plan = Decide(s);
        Check(plan.why == Why::OwnReload, "own reload: why");
        Check(!plan.tellUser, "own reload: not a loss");
        Check(plan.forgetPlayback && plan.stopRecording, "own reload: the same zeroing");
        Check(!plan.reopenEditor, "own reload: reopening is task 20's");
        Check(std::string(WhyText(plan.why)) == "own_reload", "own reload: log text");
    }

    void Line()
    {
        State s;
        s.recording = true;
        s.bufferRecording = true;
        s.recordingMs = 15000;
        s.puppets = 3;
        s.projectOpen = true;
        s.projectDirty = true;
        const std::string line = FormatLine(s, Decide(s));
        Check(line.rfind("SESSION_RESET,why=scripts_relaunched,recording=1,kind=buffer,recording_ms=15000,fate=discard_buffer", 0) == 0,
            "line: head");
        Check(line.find(",puppets=3,") != std::string::npos, "line: puppets");
        Check(line.find(",project=1,project_dirty=1,") != std::string::npos, "line: project");
        Check(line.find(",close_project=1,drop_clip=1,tell_user=0") != std::string::npos, "line: tail");
        Check(line.find('\n') == std::string::npos, "line: one line");
    }

    void PlayerAbandon()
    {
        using namespace replay::soundgeneric::player;
        Player player;
        player.Abandon(); // never began: harmless
        Check(!player.Active(), "abandon: a fresh player stays inactive");
        Check(player.Live() == 0 && player.Pending() == 0, "abandon: nothing live or pending");
    }
}

int RunSessionResetTests()
{
    QuietSession();
    BufferRecording();
    ManualRecording();
    PausedPlayback();
    LeftoversWithoutTransport();
    MusicHook();
    OwnReload();
    Line();
    PlayerAbandon();
    if (g_sessionFailures == 0)
        std::printf("session_reset tests passed\n");
    return g_sessionFailures;
}
