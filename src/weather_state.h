#pragma once

// The engine side of the weather transition in the environment track
// (environment_track.h). Rung 2 only: guarded reads and writes of the weather
// manager's transition fields, and guarded reads of the script overrides of
// wind, rain and snow (to give them back). Addresses come from the game's own
// code after a signature check (build 1491.50).
//
// A mismatch disables the transition and the overrides (logged once by the
// caller); nothing is guessed. Only the script thread calls these. That the
// weather update runs on the same game thread between script ticks is a
// hypothesis, so WriteTransition orders its writes so a frame in between
// stays sane.
// Research: docs/research/weather-wind-sky-timecycle.md

#include <cstdint>
#include <string>

namespace replay::weatherstate
{
    // The manager's transition fields as the engine keeps them.
    struct Transition
    {
        std::int32_t index = -1;  // W+0x3F38, -1 none
        float totalMs = 0.0f;     // W+0x3F3C
        float leftMs = 0.0f;      // W+0x3F40
        float progress = 0.0f;    // W+0x3F44
        std::uint8_t flag = 0;    // W+0x4170
        std::uint32_t hash = 0;   // the target type's hash (read only), 0 none
    };

    // The script overrides of the levels (SET_RAIN, _SET_SNOW_LEVEL,
    // SET_WIND_SPEED, SET_WIND_DIRECTION), to give them back.
    struct Overrides
    {
        float rain = -1.0f; // < 0: not forced
        float snow = -1.0f;
        bool windSpeedSet = false;
        float windSpeed = 0.0f;
        bool windDirectionSet = false;
        float windX = 0.0f; // the stored vector (-sin d, -cos d)
        float windY = 0.0f;
    };

    // Signature checks, once per module load. False (and why) on a mismatch.
    bool Resolve(std::string& error);
    bool Ready();
    // The transition now, with the target's hash; false when unreadable or
    // the target index is outside the type table.
    bool ReadTransition(Transition& out);
    // The five fields (hash ignored); the index must be -1 or a type.
    bool WriteTransition(const Transition& in);
    // The type index of a hash, -1 when the manager has none.
    std::int32_t IndexOf(std::uint32_t hash);
    bool ReadOverrides(Overrides& out);
}
