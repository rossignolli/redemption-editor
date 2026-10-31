#pragma once

// The horse the player rides has a player variant in the ped audio, like the
// player's own gunshot. The ped audio asks one leaf predicate (the mount
// audio predicate, audio_observer.cpp) whether the entity linked to a ped's
// component row slot +0xA8 (the component's +0xB0) is the local player (that
// entity's slot +0xB8 component, byte +0x40: bit 1 set and bit 0 clear, the
// same test as the player weapon predicate). On a horse the linked entity is
// its rider. Thirteen callers in the ped audio take the player variant on a
// yes: the movement sounds get init param byte +0x138 = 8, the footstep path
// (two sites), the movement attenuation, the player horse branches (one with
// the audio type 0x89AF18A2) and others. A puppet horse carries a puppet
// rider, never the player, so every answer would be no and the gallop would
// play the NPC horse variant. The detour answers yes for the slot +0xA8
// component of the horse puppet that carries the player's puppet, at these
// thirteen sites only (three gameplay callers keep the engine's answer).
//
// Pure (no game): the sites, the counter slots and the choice of the mounts;
// unit tested.

#include "engine_sigs.h"
#include "game_sig_core.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::mountaudio
{
    namespace sig = replay::sig;
    using enginesig::kMovementVolume;

    // The thirteen ped audio callers: each call of the predicate in its
    // function (the detour compares its return address, call + 5).
    inline const sig::Signature kCaller01{"mount_audio_caller_01",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 83 EC 40 0F 29 70 D8 41 BE 04 00 00 00",
        0x317BABC};
    inline const sig::Signature kSite01{"mount_audio_site_01", kCaller01, sig::Offset{0xB6},
        "E8 ?? ?? ?? ??", 0x317BB72};
    inline const sig::Signature kCaller02{"mount_audio_caller_02",
        "48 89 5C 24 08 57 48 83 EC 20 33 DB 48 85 C9 0F 84 ?? ?? ?? ?? 8B 81 9C 00 00 00 4C 8B 0D ?? ?? ?? ??",
        0x317C05C};
    inline const sig::Signature kSite02{"mount_audio_site_02", kCaller02, sig::Offset{0x8F},
        "E8 ?? ?? ?? ??", 0x317C0EB};
    inline const sig::Signature kSite03{"mount_audio_site_03", kMovementVolume, sig::Offset{0xBC},
        "E8 ?? ?? ?? ??", 0x3181680};
    inline const sig::Signature kCaller04{"mount_audio_caller_04",
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 89 43 18", 0x3184C3C};
    inline const sig::Signature kSite04{"mount_audio_site_04", kCaller04, sig::Offset{0x85},
        "E8 ?? ?? ?? ??", 0x3184CC1};
    inline const sig::Signature kCaller05{"mount_audio_caller_05",
        "48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 D8 FD FF FF", 0x318C4C4};
    inline const sig::Signature kSite05{"mount_audio_site_05", kCaller05, sig::Offset{0x2C0},
        "E8 ?? ?? ?? ??", 0x318C784};
    inline const sig::Signature kCaller06{"mount_audio_caller_06",
        "4C 8B DC 49 89 5B 08 57 48 81 EC 80 00 00 00 80 B9 99 05 00 00 00", 0x31925AC};
    inline const sig::Signature kSite06{"mount_audio_site_06", kCaller06, sig::Offset{0x218},
        "E8 ?? ?? ?? ??", 0x31927C4};
    inline const sig::Signature kSite07{"mount_audio_site_07", kCaller06, sig::Offset{0x353},
        "E8 ?? ?? ?? ??", 0x31928FF};
    inline const sig::Signature kCaller07{"mount_audio_caller_07",
        "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 55 41 56 41 57 48 8D A8 48 FF FF FF", 0x3193F20};
    inline const sig::Signature kSite08{"mount_audio_site_08", kCaller07, sig::Offset{0x2D9},
        "E8 ?? ?? ?? ??", 0x31941F9};
    inline const sig::Signature kCaller08{"mount_audio_caller_08",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 57 48 83 EC 40 33 DB", 0x319D2BC};
    inline const sig::Signature kSite09{"mount_audio_site_09", kCaller08, sig::Offset{0x170},
        "E8 ?? ?? ?? ??", 0x319D42C};
    inline const sig::Signature kCaller09{"mount_audio_caller_09",
        "48 89 5C 24 08 57 48 83 EC 20 80 B9 00 07 00 00 00", 0x319EEE8};
    inline const sig::Signature kSite10{"mount_audio_site_10", kCaller09, sig::Offset{0x111},
        "E8 ?? ?? ?? ??", 0x319EFF9};
    inline const sig::Signature kCaller10{"mount_audio_caller_10",
        "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 F8 FE FF FF 48 81 EC C8 01 00 00 0F 29 70 A8 45 33 F6",
        0x319F8A4};
    inline const sig::Signature kSite11{"mount_audio_site_11", kCaller10, sig::Offset{0x82D},
        "E8 ?? ?? ?? ??", 0x31A00D1};
    inline const sig::Signature kCaller11{"mount_audio_caller_11",
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC 88 00 00 00 48 8B 41 18", 0x31A0A44};
    inline const sig::Signature kSite12{"mount_audio_site_12", kCaller11, sig::Offset{0xAF},
        "E8 ?? ?? ?? ??", 0x31A0AF3};
    inline const sig::Signature kCaller12{"mount_audio_caller_12",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 0F 29 70 E8 48 8B F2 0F 29 78 D8",
        0x31A24B8};
    inline const sig::Signature kSite13{"mount_audio_site_13", kCaller12, sig::Offset{0xF2},
        "E8 ?? ?? ?? ??", 0x31A25AA};
    inline const sig::Signature* const kSites[] = {&kSite01, &kSite02, &kSite03, &kSite04, &kSite05, &kSite06, &kSite07,
        &kSite08, &kSite09, &kSite10, &kSite11, &kSite12, &kSite13};
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);
    // Per-site counters: the thirteen, then "other" (the gameplay callers).
    constexpr std::size_t kCounterSlots = kSiteCount + 1;

    // The sites' return addresses (call + 5) as RVAs in this game, 0 for a
    // site not found (nothing matches it).
    struct SiteReturns
    {
        std::uint32_t rva[kSiteCount] = {};
    };

    // The 1491.50 return addresses (the tests' game).
    inline SiteReturns KnownSiteReturns()
    {
        SiteReturns r;
        for (std::size_t index = 0; index < kSiteCount; ++index)
            r.rva[index] = kSites[index]->rva + 5;
        return r;
    }

    // The counter slot of a return RVA: its index among the sites, else
    // kSiteCount.
    inline std::size_t SiteSlot(const SiteReturns& sites, std::uint32_t rva)
    {
        if (!rva)
            return kSiteCount;
        for (std::size_t index = 0; index < kSiteCount; ++index)
            if (sites.rva[index] == rva)
                return index;
        return kSiteCount;
    }

    inline bool IsAudioSite(const SiteReturns& sites, std::uint32_t rva) { return SiteSlot(sites, rva) < kSiteCount; }

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
