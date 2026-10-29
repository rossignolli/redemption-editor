#pragma once

// The reins put back on the saddle, and the wagon reins.
//
// A horse's reins are an engine rope (type 5, 39 vertices) with 24
// attachment entries (rope +0x2C0, stride 0x80): 0 and 1 the bit, 2 the
// saddle (bone 62111, vertex 19), the others the hands of whoever holds it.
// The horse's reins controller (ctrl; rope at [[ctrl+0x10]+0x7A0]) keeps a
// state bit, ctrl+0x70 bit 2, "the reins are off the saddle, in a hand".
// Build 1491.50:
//   - 0x6D6B68(ctrl, &offset): takes the reins off the saddle (entry 2 set
//     to null through the attachment setter 0x2D24DE0, entry+0x70 and
//     +0x74 = 1) and sets bit 2. Called by 0x324D05C and 0x8AE041.
//   - 0x702F78(ctrl): when bit 2 is set and bit 14 is not, puts them back
//     (entry 2 = the entity of entry 0, the horse; entry+0x74 = 0) and
//     clears bits 2 and 13. Called by the controller update at 0x7055E4
//     (the in-hand vertex too far from where the controller expects it),
//     by 0x324D07B (the same event handler as the take) and by 0x8EA156.
// A puppet horse is not led by anyone the engine knows, so in playback its
// controller calls 0x702F78 every frame while the recorded entries say the
// saddle entry is empty and the rope's middle is in the puppet's hand
// (without the guard the reins hang in stretched loops from the bit through
// the saddle and the hand). The replay owns every rope pin of a replay
// entity while it plays; the detour skips the put-back for ropes the
// playback follows and forwards every other call. The recorded entries
// (including 2 = the horse when the recording had the reins on the saddle)
// go through the setter.
// A signature-checked MinHook detour, SEH-guarded reads, no natives,
// installed once, removed on unload with in-flight calls waited for.

//
// The wagon reins. A draft
// vehicle's reins are the draft horses' reins ropes (type 5, 43 vertices, 12
// entries: 0..3 the horse, 4 the wagon's rest pin, bone 7434, 5..9 the hands
// of the driver, 10 and 11 the other horse). Each rope's controller has hold
// modes (its tuning at [ctrl+0x28]+0x70, stride 0x20: the slots of the mode
// and which of them take the holder). Build 1491.50:
//   - 0x6B2C00(ctrl, holder, mode): sets the slots of `mode` to `holder` or
//     null through the slot setter 0x6FA1E4 (attachment setter 0x2D24DE0,
//     entry +0x70 active, +0x74 cleared) and keeps the mode at ctrl+0x60;
//   - the draft vehicle's reins update 0x14DE990 (one caller, 0x14BDAB9),
//     every frame for each of its six controllers (draft object at
//     vehicle+0x23D0, controllers at +0x18): with the driver seat empty it
//     calls 0x6B2C00(ctrl, vehicle, 0) (0x14DF017, 0x14DEB42), mode 0 = the
//     reins rest on the wagon: entry 4 = the wagon, the hand entries null;
//     with a driver, 0x6B2C00(ctrl, driver, mode);
//   - the driver comes from the vehicle's seat manager (0x1485198 reads the
//     occupant of the driver seat, [seats+0xB0+8*[seats+0x21]]), the same
//     answer the natives PHYSICS 0x31160EC47E7C9549 (mode 0..3) and
//     0xEAF529446488EB18 (mode 0) get.
// A puppet driver rides its recorded pose and is not in the seat (the seat is
// data, replay_playback.cpp), so on the puppet wagon the update would put the
// reins back on the wagon every frame over the recorded hand entries (slot 4
// the wagon, slots 5..9 null). Like the saddle put-back, the hold mode change
// is skipped for ropes the playback follows (the replay owns every pin of a
// replay rope) and forwarded for every other rope. Ini ReinsHoldGuard (1).
// A signature-checked MinHook detour, SEH-guarded reads, no natives.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::reins
{
    // Pure: whether the put-back on `rope` is skipped (the rope is one the
    // playback follows). Unit tested.
    inline bool SkipRestore(std::uint64_t rope, const std::uint64_t* owned, std::size_t count)
    {
        if (!rope)
            return false;
        for (std::size_t n = 0; n < count; ++n)
            if (owned[n] == rope)
                return true;
        return false;
    }

    bool Install(std::string& error);
    // The hold mode detour (0x6B2C00) on or off (ini
    // ReinsHoldGuard); read before the first Install.
    void SetHoldGuard(bool on);
    bool HoldInstalled();
    void Shutdown();
    bool Installed();
    // Rope instances (the RopeInfo address) the playback follows this tick;
    // empty outside playback.
    void SetOwnedRopes(const std::vector<std::uint64_t>& ropes);

    struct Stats
    {
        std::uint64_t calls = 0;   // detour calls
        std::uint64_t skipped = 0; // put-backs skipped on followed ropes
        std::uint64_t faults = 0;  // unreadable controller (forwarded)
        // The hold mode changes (0x6B2C00).
        std::uint64_t holdCalls = 0;
        std::uint64_t holdSkipped = 0;   // on followed ropes: skipped
        std::uint64_t holdSkippedRest = 0; // of them, mode 0 (put back on the wagon or the horse)
        std::uint64_t holdFaults = 0;
    };
    Stats Snapshot();
}
