#pragma once

// The strap state of a weapon as the replay plays it back, pure and unit
// tested. The recorder keeps, per weapon object with straps, every change of
// the strap state (weapon_observer.h StrapState: the hide bits, the weapon
// flags word [CWeapon+0x62C] and the two blend weights; the STRP section).
// Playback puts that state on the puppet weapon.
//
// Rule: playback requests the strap rebuild from the parent ped (flag 0x8000)
// only where the engine did, that is in a state recorded with 0x8000 or with
// no sign of being in a hand (neither the hold bits 0x1800 nor 0x2000). A
// state recorded in a hand without 0x8000 (the cloth sling while aiming) gets
// no rebuild, otherwise the rebuild re-poses the sling over the recorded pose
// every frame. The hold bits and 0x20000 are replayed as recorded; 0x2000 is
// read only and never written on a puppet (it would run the weapon process).
//
// Evidence: offline dump for the flag bits, tested in game with the strap_test
// bench op. The rigid loops of QA 232-Valentine are not a strap but the
// component order (weapon_order_core.h).
// Research: docs/research/peds-weapon-straps.md

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace replay::strap
{
    constexpr std::uint32_t kFlagTaskHoldA = 0x800;
    constexpr std::uint32_t kFlagTaskHoldB = 0x1000;
    constexpr std::uint32_t kFlagTaskHold = kFlagTaskHoldA | kFlagTaskHoldB; // the manager's test
    constexpr std::uint32_t kFlagHeld = 0x2000;
    constexpr std::uint32_t kFlagRebuild = 0x8000;
    constexpr std::uint32_t kFlagCallback = 0x10000;
    constexpr std::uint32_t kFlagClothState = 0x20000;
    // The bits playback copies from the recorded state onto the puppet.
    constexpr std::uint32_t kReplayedBits = kFlagTaskHold | kFlagClothState;

    // Whether the engine was driving the strap from the ped in this recorded
    // state (see above).
    constexpr bool RebuildWanted(std::uint32_t recorded)
    {
        if ((recorded & kFlagRebuild) != 0)
            return true;
        return (recorded & (kFlagTaskHold | kFlagHeld)) == 0;
    }

    // The flags word written on the puppet weapon: its own bits, the
    // replayed bits from the recording, and the rebuild request when wanted
    // (cleared otherwise, so no request is left pending).
    constexpr std::uint32_t PlaybackFlags(std::uint32_t current, std::uint32_t recorded)
    {
        const std::uint32_t kept = current & ~(kReplayedBits | kFlagRebuild);
        return kept | (recorded & kReplayedBits) | (RebuildWanted(recorded) ? kFlagRebuild : 0u);
    }

    // For the log: who held the strap in the recorded state.
    //   "ped"   the weapon manager drove it (rebuilt every frame);
    //   "task"  the weapon task held it (0x800 or 0x1000);
    //   "hand"  in a hand, no rebuild and no hold bit;
    //   "none"  no rebuild and no sign of a hand (rebuilt, as before).
    constexpr const char* HolderName(std::uint32_t recorded)
    {
        if ((recorded & kFlagRebuild) != 0)
            return "ped";
        if ((recorded & kFlagTaskHold) != 0)
            return "task";
        if ((recorded & kFlagHeld) != 0)
            return "hand";
        return "none";
    }

    // The state at the playhead: the last one at or before timeMs, the first
    // before the first change (the recorder's first state is the look at the
    // weapon's first sample). -1 with no states.
    template <typename State>
    int StateIndexAt(const std::vector<std::pair<std::uint32_t, State>>& states, std::uint32_t timeMs)
    {
        if (states.empty())
            return -1;
        int index = 0;
        for (std::size_t n = 0; n < states.size(); ++n)
        {
            if (states[n].first <= timeMs)
                index = static_cast<int>(n);
        }
        return index;
    }
}
