#pragma once

// The engine's speech duck fed with the recording's state in the replay,
// engine side (the cause, the evidence and the layout are in
// speech_duck_core.h).
//
// - The replay path (SpeechDuckReplay=1): a detour of the duck driver 0x2F2D1EC
//   (prologue and 17 sites checked, installed with the other hooks at load)
//   forwards every call; while the replay owns the world and a playback
//   published the inputs at the playhead (fresh within 1 s), it writes the
//   duck level (and, above 0, the four line flags; from a MIXS v3 clip the
//   second duck's level too) right before the driver reads them. The engine
//   then starts, sets and stops its own duck scene. Never calls natives.
// - The evidence (SpeechDuckLog=1): SPEECH_DUCK lines once a second in
//   recording and playback with the duck scene the engine has (its seven
//   variables), the gates and, in playback, the inputs written; a line on
//   every change of the published level.
//
// Engine access: rung 2 reads of the duck's globals and scene (sites checked
// once), rung 5 for the detour (closed theatre: it writes only while the
// replay owns the world); Shutdown stops writing, disables the hook, waits
// for the calls in flight and only then removes it.

#include "replay_recording.h"
#include "speech_duck_core.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace replay::speechduck
{
    struct Config
    {
        bool replay = true;                          // SpeechDuckReplay
        std::uint8_t derivedFlags = kDefaultDerivedFlags; // SpeechDuckDerivedFlags
        bool log = true;                             // SpeechDuckLog
    };
    void Configure(const Config& config);
    const Config& GetConfig();

    bool Install(std::string& error);
    bool Installed();
    void Shutdown();

    // Script thread.
    void Record(const Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log);
    void BeginPlayback(const Recording& clip, std::ostream& log);
    void Play(const Recording& clip, std::uint32_t timeMs, bool owning, std::ostream& log);
    void EndPlayback(const Recording& clip, std::ostream& log);
    // A script restart with a playback open: the detour stops writing at
    // once (the driver zeroes the level itself).
    void ForgetPlayback();
}
