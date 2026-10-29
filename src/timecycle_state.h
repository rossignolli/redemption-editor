#pragma once

// The engine side of the timecycle track (timecycle_track.h). Guarded reads and
// writes of the script timecycle modifier and of the weather types' active
// variations, at addresses taken from the game's own code after a signature
// check (build 1491.50):
//   0xF8F4E8  SET_TIMECYCLE_MODIFIER handler: jmp 0xFA2758
//   0xFA2758  lea rcx, [0x4A96E80] (T); call 0x2B2ED78 (name hash -> index);
//             or [T+0x2778], -1; mov [T+0x2770], eax
//   0x2B2ED78 the index of a hash: 0x2B2ED1C (alias at T+0x18, count T+0x38,
//             then the map at T+0x50 via 0x450210) and the position of the
//             pointer in the array at T+0x40 (u16 count at T+0x48)
//   0x450210  the sorted map: data at +0x8, u16 count at +0x10, 16-byte
//             entries (u32 key, u64 pointer at +0x8)
//   0xF22B60  the per frame update: +0x2780 * frame time into +0x277C,
//             against +0x2774; +0x2778 into +0x2770; the byte at +0x2784
//   0xF8AC84  CLEAR_TIMECYCLE_MODIFIER handler: jmp 0xF93770 (index and
//             transition -1, strength = the 1.0 at 0x330C7A8, byte +0x2784 = 0)
//   0x1047A2C _SET_WEATHER_VARIATION handler: jmp 0x105945C
//   0x105945C lea rcx, [0x45722E0] (W); 0x739918 (type by hash: u32 count at
//             W, hash at W + i * 0x1E0 + 0x10); 0x739DF0 -> 0x739DB4
//             (variation by key: table at +0x1C8, u16 count at +0x1D0, 0x44
//             bytes an entry); call 0x757BEC(W, type, variation, 0)
//   0x757BEC  writes the entry's key to W + type * 0x1E0 + 0x14 (and takes
//             the type off the pending clear list at W+0x3E38); a negative
//             variation goes to 0x72C2E8 (the clear, immediate with the 4th
//             argument: +0x14 = +0x10)
// A mismatch disables the track (logged once); nothing is guessed. Reads and
// writes happen on the script thread, as the natives above do (no detour).

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
    // it has none) through the engine's own setter (0x757BEC); types already
    // there are left alone.
    VariationResult ApplyVariations(const TimecycleSample& wanted);
}
