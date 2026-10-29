#pragma once

// The horse the player rides has a player variant in the ped audio, like the
// player's own gunshot. Build 1491.50: the ped audio asks one leaf predicate,
// 0x8B69B0, whether the entity linked to a ped's component row slot +0xA8 (the
// component's +0xB0) is the local player (that entity's slot +0xB8 component,
// byte +0x40: bit 1 set and bit 0 clear, the same test as the weapon predicate
// 0x2F02F8C). On a horse the linked entity is its rider. Thirteen callers in
// the ped audio (0x317A000 to 0x31A3000) take the player variant on a yes: the
// movement sounds of 0x3193F20 get init param byte +0x138 = 8 (0x31941F9), the
// footstep path (0x317BB72, 0x317C0EB), the movement attenuation (0x3181680),
// the player horse branches (0x31927C4 with the audio type 0x89AF18A2,
// 0x31928FF -> 0x31A1420, 0x31A00D1) and others. A puppet horse carries a
// puppet rider, never the player, so every answer would be no and the gallop
// would play the NPC horse variant. The detour answers yes for the slot +0xA8
// component of the horse puppet that carries the player's puppet, at these
// thirteen sites only (three gameplay callers, 0x899B0D, 0xD9DC7C and
// 0x2191000, keep the engine's answer).
//
// Pure (no game): the site table and the choice of the mounts; unit tested.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::mountaudio
{
    // Return addresses (call + 5) of the thirteen ped audio callers.
    constexpr std::uint32_t kSites[] = {
        0x317BB77, 0x317C0F0, 0x3181685, 0x3184CC6, 0x318C789, 0x31927C9, 0x3192904,
        0x31941FE, 0x319D431, 0x319EFFE, 0x31A00D6, 0x31A0AF8, 0x31A25AF};
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);
    // Per-site counters: the thirteen, then "other" (the gameplay callers).
    constexpr std::size_t kCounterSlots = kSiteCount + 1;

    // The counter slot of a return RVA: its index in kSites, else kSiteCount.
    constexpr std::size_t SiteSlot(std::uint32_t rva)
    {
        for (std::size_t index = 0; index < kSiteCount; ++index)
            if (kSites[index] == rva)
                return index;
        return kSiteCount;
    }

    constexpr bool IsAudioSite(std::uint32_t rva) { return SiteSlot(rva) < kSiteCount; }

    // The mounts whose audio rides with the player: for every rider id that
    // was the local player, the mount it sits on now (mountOf returns 0 for
    // none). Ids are packed ReplayIds; no duplicates, order of the riders.
    template <typename MountOf>
    std::vector<std::uint32_t> PlayerMounts(const std::vector<std::uint32_t>& playerRiders, MountOf&& mountOf)
    {
        std::vector<std::uint32_t> mounts;
        for (const std::uint32_t rider : playerRiders)
        {
            const std::uint32_t mount = mountOf(rider);
            if (!mount)
                continue;
            bool known = false;
            for (const std::uint32_t seen : mounts)
                known = known || seen == mount;
            if (!known)
                mounts.push_back(mount);
        }
        return mounts;
    }
}
