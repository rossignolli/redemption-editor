#pragma once

// Unattended test bench, developer build only (REDEMPTION_DEV): the public
// build compiles no executor (replay_bench.cpp is not in it), polls no inbox
// and takes no fault command; its entry points below do nothing
// (tools/check_public_build.py checks it). In the developer build it is inert
// until a scenario file shows up in the inbox (kInboxDir, *.scn); the host
// tool drops one once the log says BENCH_READY. The runner executes one step per
// tick on the script thread, with natives only, and drives recording and
// playback through the same entry points as the keys. Every scenario starts
// and ends with a world hygiene pass: the entities it spawned are deleted,
// recording and playback are stopped.

#include "replay_context.h"

#include <cstdint>
#include <string>

namespace replay::bench
{
    // The recording and playback entry points of the ASI core.
    struct Hooks
    {
        virtual ~Hooks() = default;
        virtual void StartRecording() = 0;
        virtual void StopRecording() = 0;
        virtual bool Recording() const = 0;
        virtual bool StartPlayback(std::uint64_t now) = 0;
        virtual void StopPlayback() = 0;
        virtual void SetPaused(bool paused, std::uint64_t now) = 0;
        // The last recording to / a clip from ReplayClips\<name>.rpclip.
        virtual bool WriteClip(const std::string& name, std::string& error) = 0;
        virtual bool ReadClip(const std::string& name, std::string& error) = 0;
        // Frames as the audience sees them (camera preview): no text HUD,
        // markers, messages or game HUD while on.
        virtual void SetCleanView(bool on) = 0;
        // Replay buffer: "save" is Shift+F4, "press" is F4.
        virtual void Buffer(const std::string& action) = 0;
        // True while the running recording is the always-on ring (not a manual
        // one): the end of a scenario leaves it running, as the product does
        // after Shift+F4.
        virtual bool BufferRecording() const { return false; }
        // Camera markers: marker_add, marker_clear, cam_log. False with
        // `detail` on a bad argument.
        virtual bool CameraOp(const std::string& op, const std::vector<std::string>& args, std::string& detail) = 0;
        // The safe exit's bench (theatre_exit.h) faults: "exception" (the next
        // tick throws), "hold_exit" (the normal exit of the closed theater does
        // not run: the watchdog's idle rule must), "release_exit". False on
        // another word.
        virtual bool SafeExitTest(const std::string& what)
        {
            (void)what;
            return false;
        }
        // The playing clip's environment setting, `field` clock or weather,
        // `value` live or recorded. False with `detail` when no clip with the
        // environment track is playing.
        virtual bool EnvironmentOp(const std::string& field, const std::string& value, std::string& detail)
        {
            (void)field;
            (void)value;
            detail = "unsupported";
            return false;
        }
    };

#if defined(REDEMPTION_DEV)
    void Attach(Context& context, Hooks& hooks);
    // Once per tick, before recording and playback update.
    void Update(std::uint64_t now);
    // -1, 0 or +1 while a scrub step holds the scrub; fine scrub in *fine.
    int ScrubOverride(bool* fine);
    // The bench's script camera (`cam`, `topcam`), 0 when none: the recording
    // gate does not take it for a game script camera.
    int OwnCamera();
#else
    // The public build: no bench. Nothing attaches, nothing is polled, no
    // scrub is held and the bench has no camera.
    inline void Update(std::uint64_t) {}
    inline int ScrubOverride(bool*) { return 0; }
    inline int OwnCamera() { return 0; }
#endif
}
