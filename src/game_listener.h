#pragma once

// The live game's volume listener (game_listener_core.h), recorded (clip
// section LSTN) and put back while the replay renders the recorded game
// camera.
//
// Engine access: guarded reads of the audio environment (offset proof by
// code sites, game_listener_core.h kSites, checked once; a mismatch disables
// every read and write and logs GAME_LISTENER,phase=unproven). Playback writes
// (ReplayGameListener=1 only) set the engine's own volume listener override
// (bit 0x100 of env+0x650, matrix env+0x6B0) while the replay owns the world
// and clear it when the recorded game camera stops rendering or the playback
// stops. Script thread only. Lines: GAME_LISTENER (state changes),
// LISTENER (once a second, record and play, with GameListenerLog=1 or
// MixProbe=1).
//
// Ini: ListenerRecord (1) records the track; ReplayGameListener (0) applies
// it; GameListenerLog (0) the per second LISTENER line.
//
// ListenerRecordV2 (1) records every volume point and the microphone hash
// (LSTN v2); ListenerMicReplay (1) puts the recorded microphone back through
// the engine's override hash +0x780 while the replay camera follows the
// recorded one (the engine builds the points itself; the bit 0x100 path stays
// off meanwhile); ListenerMicFallbackHash (0) the hash for a clip without one
// (0: the hash of the 2-point microphone in use read live in this session, from
// the metadata table entry of desc+0x300, who=derived), used only
// on samples the live game had 2 points in; ListenerCamMatchM (1.0) a marker
// camera within this many meters of the recorded camera counts as following.

#include "replay_recording.h"

#include <cstdint>
#include <ostream>

namespace replay::gamelistener
{
    void Configure(bool record, bool apply, bool logEverySecond);
    void ConfigureMic(bool micReplay, std::uint32_t fallbackHash, float camMatchM); // LSTN v2 is always recorded
    bool RecordEnabled();
    bool ApplyEnabled();
    float CamMatchM(); // ListenerCamMatchM

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
