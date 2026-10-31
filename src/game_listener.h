#pragma once

// The live game's volume listener (game_listener_core.h), recorded (clip
// section LSTN, every volume point and the microphone hash), and its
// microphone put back while the replay renders the recorded game camera.
//
// Engine access: guarded reads of the audio environment (offset proof by
// code sites, game_listener_core.h kSites, checked once; a mismatch disables
// every read and write and logs GAME_LISTENER,phase=unproven). Script thread
// only. Lines: GAME_LISTENER (state changes), LISTENER (once a second, record
// and play, with GameListenerLog=1 or MixProbe=1).
//
// Ini: GameListenerLog (0) the per second LISTENER line; ListenerMicReplay
// (1) puts the recorded microphone back through the engine's override hash
// +0x780 while the replay camera follows the recorded one (the engine builds
// the points itself); a clip without a hash gets the hash of the 2-point
// microphone in use read live in this session (the metadata table entry of
// desc+0x300, who=derived), used only on samples the live game had 2 points
// in; a marker camera within 1 m of the recorded camera counts as following.

#include "replay_recording.h"

#include <cstdint>
#include <ostream>

namespace replay::gamelistener
{
    void Configure(bool logEverySecond);
    void ConfigureMic(bool micReplay); // LSTN v2 is always recorded
    float CamMatchM(); // 1 m

    // Recording tick (script thread, recording clock in ms).
    void Record(Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log);

    // Playback tick, after the cameras of this tick (UpdateWorldFocus):
    // recordedGameCamera is true when the replay camera renders the clip's
    // recorded game camera (or a marker camera within ListenerCamMatchM of
    // it; camErrM < 0 unknown, source for the log).
    void Play(const Recording& clip, std::uint32_t playheadMs, bool recordedGameCamera, std::ostream& log,
        float camErrM = -1.0f, const char* source = nullptr);

    // Playback stop (or the replay lost the world): clears the override the
    // replay set.
    void Release(const char* why, std::ostream& log);
}
