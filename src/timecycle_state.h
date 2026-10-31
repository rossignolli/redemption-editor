#pragma once

// The engine side of the timecycle track (timecycle_track.h). Guarded reads and
// writes of the script timecycle modifier and of the weather types' active
// variations, at addresses taken from the game's own code after a signature
// check (build 1491.50: the SET_TIMECYCLE_MODIFIER, CLEAR_TIMECYCLE_MODIFIER
// and _SET_WEATHER_VARIATION handlers and what they call).
// A mismatch disables the track (logged once); nothing is guessed. Reads and
// writes happen on the script thread, as the natives above do (no detour).
// Research: docs/research/weather-wind-sky-timecycle.md

#include "timecycle_track.h"

#include <cstdint>
#include <string>

namespace replay::tcstate
{
    // Signature checks, once per module load. False (and why) on a mismatch.
    bool Resolve(std::string& error);

    // The modifier fields as they are now (raw indexes).
    bool ReadFields(tctrack::ModifierFields& out);
    // The state now with its keys (timeMs left to the caller). `overflow`:
    // more types with a variation than a sample holds (the rest left out).
    bool Read(TimecycleSample& out, bool* overflow = nullptr);
    // The session's index of a modifier key (-1: unknown or no tables).
    std::int32_t IndexOf(std::uint32_t key);
    // The six fields written (guarded).
    bool WriteFields(const tctrack::ModifierFields& fields);
    // The number of modifiers in the session (the array's count).
    std::size_t ModifierCount();

    struct VariationResult
    {
        std::uint32_t types = 0;   // weather types seen
        std::uint32_t cleared = 0; // engine calls: immediate clear
        std::uint32_t set = 0;     // engine calls: set by index
        std::uint32_t missing = 0; // wanted key not in the type's table
        std::uint32_t faults = 0;  // a guarded call or read failed
    };

    // Every weather type to the variation `wanted` gives it (the default when
    // it has none) through the engine's own variation setter; types already
    // there are left alone.
    VariationResult ApplyVariations(const TimecycleSample& wanted);
}
