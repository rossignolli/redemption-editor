#pragma once

// The engine side of the weather transition in the environment track
// (environment_track.h). Rung 2 only: guarded reads and writes of the weather
// manager's transition fields, and guarded reads of the script overrides of
// wind, rain and snow (to give them back), at addresses taken from the game's
// own code after a signature check (build 1491.50):
//   0x1042D0C GET_CURR_WEATHER_STATE handler: lea r10, [0x45722E0] (the
//             manager W), types at W + 0x10 + i*0x1E0 (hash first),
//             previous index at W+0x3F24.
//   0x7340BC  the transition setter (SET_WEATHER_TYPE with a time, via
//             0x10479B4 -> 0x1059348): index < [W] (the type count), time *
//             [0x330F44C] (1000.0: milliseconds), W+0x3F44 progress = 0,
//             W+0x3F38 target index, W+0x4170 flag byte, W+0x3F3C total,
//             W+0x3F40 time left.
//   0x75FFEC  the per-frame update: time left -= frame time * 1000, progress
//             = 1 - min(left / total, 1); at 1 it calls 0x733F34 with
//             lea rcx, [W] (the target becomes the weather) and clears the
//             five fields (target -1).
//   0x10477E8 SET_RAIN handler: movss [W+0x4160], xmm0; 0x1047868
//             _SET_SNOW_LEVEL handler: movss [W+0x4164], xmm0. The level
//             readers (0x760EAC rain, 0x762C30 snow) use the override when
//             it is >= [0x330F380] (0.0); the manager starts them at -0.01
//             (0x73D554): a negative value is "not forced".
//   0x1059500 SET_WIND_SPEED: below [0x330F380] clears the flag byte at
//             [0x457CCD8]; otherwise lea rcx, [0x457CC70] (the wind object
//             V) -> 0x75676C: speed at V+0x60, flag byte V+0x68.
//   0x10594BC SET_WIND_DIRECTION: below 0.0 clears [V+0x80]; otherwise
//             lea rcx, [V] -> 0x73A01C (degrees * [0x3311368] = pi/180,
//             vector (-sin, -cos, 0)) stored at V+0x70, flag byte V+0x80.
// A mismatch disables the transition and the overrides (logged once by the
// caller); nothing is guessed. Only the script thread calls these. That the
// weather update (0x75FFEC, called only from 0xF23D49 in 0xF237D0, itself
// called only from 0xF21AA8) runs on the same game thread, between script
// ticks, is a hypothesis (the chain up to the thread was not followed):
// WriteTransition orders its writes so a frame in between stays sane.

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
