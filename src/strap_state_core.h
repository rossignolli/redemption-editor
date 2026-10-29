#pragma once

// The strap state of a weapon as the replay plays it back, pure and unit
// tested (QA report 232-Valentine: the red cloth sling of a bolt-action rifle
// stood up in rigid loops while Arthur aimed).
//
// The recorder keeps, per weapon object with straps, every change of the
// strap state (weapon_observer.h StrapState: the component hide bits, the
// whole weapon flags word [CWeapon+0x62C] and the two blend weights; the STRP
// section, unchanged). Playback puts that state on the puppet weapon.
//
// The flag bits, offline from build 1491.50 (dump of pid 18760):
//   0x00000004  set by the per-weapon pass 0x117D49C once the weapon's
//               matrices are built this frame, cleared at its start. Not a
//               strap bit.
//   0x00000080  one-shot request read and cleared by 0x11B5578. No strap
//               code reads it. Not replayed.
//   0x00000800  "strap held by the weapon task" (A): set when the weapon
//               task's state 3 starts (0x2140E58, the FSM 0x215B570), cleared
//               by its update and exit and by the holster paths (0x2141754,
//               0x2141454, with the in-hand bit below).
//   0x00001000  "strap held by the weapon task" (B): set every update of the
//               same task's state 5 (0x213CEE4 -> 0x211FF80(task, ped, 1),
//               on the weapon of both item slots at task+0x4A0) and cleared
//               on its exit (0x213CD94 -> 0x211FF80(..., 0)).
//               The owner ped's weapon manager (0x117232C) tests 0x1800
//               (0x1172569, 0x117259E, 0x117263A): with either bit set it
//               keeps its strap choice, does not write the hide bits, the
//               0x20000 state or the weights, and does not request the
//               rebuild (0x8000). The recording shows it: the rifle 51:1574
//               aims from 8.209 s to 23.045 s with 0x233084 (0x1000 set,
//               0x8000 clear) and is 0x238084 on the back (0x8000 set).
//   0x00002000  the weapon object is held in a hand: set by the equip paths
//               (0x11B58C5, 0x172CD17, 0x117A59B, 0x11A2DB4 through the setter
//               0x11A2780, which also stamps +0x624), cleared by the holster
//               natives (0x2141B8B, 0x2141C91, 0x2141FBC). Readers: 0x119A238
//               (held and not one of four excluded weapons), which makes the
//               object pass 0x4FEF3C run the whole weapon process 0x11814DC
//               for the held weapon every frame, and the object predicate
//               0x7F00D0. Not a strap bit; not written on a puppet (that
//               would run the weapon process on it). Read only, to choose the
//               rebuild below.
//   0x00008000  "rebuild the straps from the attach parent ped": set every
//               frame by the weapon manager when it drives the strap
//               (0x11727AB, never under 0x1800); consumed by the object's
//               skeleton callback 0x117EA40 (registered by 0x113B380 for a
//               weapon with straps; runs only with 0x8000 and 0x10000), which
//               re-poses the strap bones from the ped's (bone 0x5842 of some
//               rifles, then 0x118F044: the 10-bone cloth chain 0x118FD54 with
//               weight +0x5D8 and the shoulder strap 0x118F1C0 with weight
//               +0x5D4, each only while that strap is shown) and writes their
//               object matrices (0x264967C), after the pose the replay wrote.
//   0x00010000  gate of that callback (set at weapon init 0x110B89D).
//   0x00020000  set by the weapon manager when its per-hash strap state is
//               2 (0x1172785, the table at manager+0x408); the callback picks
//               the weapon's anim parameter 0x3E89 from it. Replayed as
//               before.
//   0x00200000  read only by script natives (0x210CFDB, 0x213881B,
//               0x213F1F9, 0x2157C28); no strap code. Not replayed.
//
// The defect: playback set 0x8000 on every frame, so the callback re-posed
// the visible cloth chain from the ped with the recorded weight (0 while
// aiming) over the recorded pose, every frame, where the engine had left the
// strap to the weapon's own pose. Playback now asks for the rebuild only
// where the engine did: in a state recorded with 0x8000 (the shoulder sling
// on the back, holstered weapons: as before), or in a state recorded with no
// sign of being in a hand (neither the hold bits nor 0x2000: as before, so
// nothing that plays well today changes). A state recorded in a hand without
// 0x8000 (the cloth sling while aiming) gets no rebuild. The hold bits and
// 0x20000 are replayed as recorded.
//
// In game (a QA clip at 13442 ms) the flags are played exactly
// (read back 0x31000) and the loops stay: they are not a strap. The bench op
// strap_test (developer build) hid one component at a time on the puppet:
// hiding COMPONENT_RIFLE_BOLTACTION_WRAP (0x549A7FC8) removes the leather
// loop; hiding REPEATER_CLOTH_STRAP01, the scope, the sight or the grip, or
// showing REPEATER_STRAP01 or the shoulder sling, changes nothing; clearing
// 0x1000, 0x20000 or 0x200000 or forcing either weight changes nothing. So
// the fallback once planned here (hide the cloth strap, show REPEATER_STRAP01)
// is wrong and was not made. The cause is the order the puppet got its
// components in: the wrap before the scope (weapon_order_core.h).

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
