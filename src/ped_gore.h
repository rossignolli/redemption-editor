#pragma once

// Ped gore (dismemberment). The engine keeps, per ped, a gore entry with a limb
// bit mask (entry +0x18, table at 0x497CB80, getter 0xB4498C; the native
// _HAS_PED_TAKEN_GORE_DAMAGE reads bit `limb`) and applies a limb through
// 0xB5203C(limb, context*, hash*, u8): it walks the limb's bones and calls the
// gore setter 0xB50A3C for each. The engine's own mask replay (0xEAC808) calls
// it for every set bit 0..14 with a context holding only the ped and the hash
// 0xE4DF46D5; playback does the same on the puppet. Build 1491.50,
// signature-checked, SEH-guarded.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::gore
{
    // Limbs the native documents: 3 left hand, 4 left arm, 6 right hand,
    // 7 right arm, 9 left foot, 10 left leg, 12 right foot, 13 right leg,
    // 37 head.
    constexpr int kLimbs[] = {3, 4, 6, 7, 9, 10, 12, 13, 37};

    // Applies one limb's gore on a ped (engine address); false with a reason.
    bool ApplyLimb(void* ped, int limb, std::string& error);

    // The engine's gore mask of a ped (bit n = limb n lost, bit 0 the head, which
    // the native never reports): guarded read of the gore table (pointer at
    // module+0x497CB80, u16 count at +0x497CB88, entries of 0x20: +0x00 ped, +0x18
    // mask), as the getter 0xB4498C walks it.
    bool ReadGoreMask(const void* ped, std::uint32_t& mask);

    // The gameplay gore event. Every dismemberment the game does goes through the
    // handler 0xB4EB3C(event*) (called by the damage path 0x2BEB48 and three
    // others). The event is 0x50 bytes: +0x00 ped, +0x08 inflictor entity (may be
    // null), +0x10/+0x20/+0x30 vectors (hit point and directions), +0x40 hash,
    // +0x44 bone tag, +0x48 u16 limb mask, +0x4A u8. With the hit data the handler
    // builds the stump, the exposed bone and the wounds; the mask replay above only
    // removes the limb. A read-only detour records each event after the original
    // ran; playback calls the handler with the recorded event on the puppet.
    constexpr std::size_t kEventBytes = 0x50;
    struct EventCall
    {
        std::uint64_t timeUs = 0;
        std::uint8_t context[kEventBytes] = {};
        std::uint8_t result = 0;
        std::uint32_t caller = 0;
    };
    bool InstallObserver(std::string& error);
    void SetObserving(bool on);
    void Shutdown();
    std::uint64_t NowUs();
    std::uint64_t TotalEvents();
    void CollectEventsSince(std::uint64_t& cursor, std::vector<EventCall>& out);
    // Replays one recorded event: +0x00 becomes `ped`, +0x08 `inflictor`
    // (null when not replayed). handled is the handler's own result.
    bool ReplayEvent(const std::uint8_t* context, void* ped, void* inflictor, bool& handled, std::string& error);
    // Bench: a gameplay-equivalent gore event (live evidence: the gameplay
    // limb events carry only ped, inflictor, the hash 0xE4DF46D5 and the
    // limb mask; bit 0 is the head). Goes through the hooked entry, so the
    // observer records it like a real hit.
    bool FireEvent(void* ped, void* inflictor, std::uint16_t mask, bool& handled, std::string& error);
}
