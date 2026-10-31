#pragma once

// The reins put back on the saddle, and the wagon reins.
//
// A horse's reins are an engine rope whose saddle attachment (entry 2) the
// reins controller puts back every frame when the in-hand vertex is far from
// where it expects it (the put-back, ctrl+0x70 bit 2). A puppet horse is not
// led by anyone the engine knows, so in playback the put-back would fight the
// recorded entries and the reins hang in stretched loops. The replay owns
// every rope pin of a replay entity while it plays: the detour skips the
// put-back for ropes the playback follows and forwards every other call. The
// recorded entries (including 2 = the horse when the recording had the reins
// on the saddle) go through the attachment setter.
// A signature-checked MinHook detour, SEH-guarded reads, no natives,
// installed once, removed on unload with in-flight calls waited for.

//
// The wagon reins. A draft vehicle's reins update puts the reins back on the
// wagon (hold mode 0) every frame while the driver seat is empty. A puppet
// driver rides its recorded pose and is not in the seat, so like the saddle
// put-back the hold mode change is skipped for ropes the playback follows and
// forwarded for every other rope. Ini ReinsHoldGuard (1).
// A signature-checked MinHook detour, SEH-guarded reads, no natives.
// Evidence: offline dump of 1491.50.
// Research: docs/research/horse-tack-mount.md

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
    // The hold mode detour on or off (ini
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
        // The hold mode changes (calls of the hold mode setter).
        std::uint64_t holdCalls = 0;
        std::uint64_t holdSkipped = 0;   // on followed ropes: skipped
        std::uint64_t holdSkippedRest = 0; // of them, mode 0 (put back on the wagon or the horse)
        std::uint64_t holdFaults = 0;
    };
    Stats Snapshot();
}
