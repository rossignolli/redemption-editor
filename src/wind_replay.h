#pragma once

// The wind of the audio in the replay, engine side (the cause and the evidence
// are in wind_replay_core.h).
//
// - The fix (WindReplay=1, default): a detour of the box streamer's "loaded
//   about pos" 0x2B98AF8, prologue checked, installed with the other hooks
//   at load. It forwards every call unchanged; only the weather audio
//   pre-update's call (return address 0x31B8715) while the replay owns the
//   world and a scene position is fresh gets the replay's scene position
//   (the focus puppet the trail map follows, else the rendered camera)
//   instead of the hidden live player's. Same streamer, same mask; the
//   engine answers and computes the wind itself. Never calls natives.
// - The evidence (WindReplayLog=1, default): the recording keeps the audio
//   wind strength, the wind rows' weather and the simple path flag (clip
//   section WNDA, 4 samples a second); the playback logs the recorded value
//   against the engine's (WIND_REPLAY every 5 s of clip time,
//   WIND_REPLAY_TOTAL at the end) with the gate's answers.
//
// Engine access: rung 2 reads of the weather entity (sites checked once,
// wind_replay_core.h kSites; the entity's vtable compared) and rung 5 for
// the detour (closed theatre: it changes the answer only while the replay
// owns the world; EndClipWorld clears the scene, a stale scene (3 s without
// a publish) is ignored, Shutdown disables the hook, waits for calls in
// flight and only then removes it).

#include "replay_recording.h"
#include "wind_replay_core.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace replay::windreplay
{
    struct Config
    {
        bool fix = true; // WindReplay
        bool log = true; // WindReplayLog
    };
    void Configure(const Config& config);
    const Config& GetConfig();

    // Load time, inside the hook batch (WarmUpHooks). False with the reason
    // ("off" when WindReplay=0).
    bool Install(std::string& error);
    bool Installed();
    void Shutdown();

    // Script thread, every tick while the replay owns the world: where the
    // weather gate looks (x, y, z of the scene). ClearScene when it does not.
    void PublishScene(float x, float y, float z, SceneSource source, std::ostream& log);
    void ClearScene();

    // Script thread. Recording: one sample per kSamplePeriodMs of clip time.
    void Record(Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log);
    // Playback: begin, every tick (normal: the clock runs at the recording's
    // rate), end.
    void BeginPlayback(const Recording& clip, std::ostream& log);
    void Play(const Recording& clip, std::uint32_t timeMs, bool normal, std::ostream& log);
    void EndPlayback(const Recording& clip, std::ostream& log);
}
