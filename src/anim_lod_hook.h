#pragma once

// Keeps the recorded peds animated at full detail whatever the game camera
// sees, the way a replay recorder raises fidelity while it records.
//
// 0x2BD410C(fwAnimDirector* rcx, u8 lod dl, bool visible r8b) is where the
// engine hands a ped's animation LOD to its anim director: 0x8F0740 calls it
// every frame for each pre-rendered ped with lod = the distance LOD of the
// graphics component (+0x4A0, from 0x8F0AD4 and the LOD multiplier) and
// visible = seen this frame (or the anim-visible byte, anim_visible.h). Its
// tail 0x2BD413C stores the creature component's requested LOD (+0xAE) and
// effective LOD (+0xAF = visible ? lod : 3); the expressions keep only the
// ones whose LOD range holds it (0x2BD036C -> 0x295560C -> 0x290F864). The
// anim-visible byte fixes the off-screen case only: a ped seen at 25 m still
// gets the distance LOD (1 or 2: frozen face, measured +280 to +610 bind
// rows). The detour passes lod 0 and visible 1 for the directors of the
// recorded peds: the animation of a ped near and on screen, with nothing
// else changed (the draw LOD, the fade and the draw distance come from the
// graphics component, which is not touched, unlike SET_PED_LOD_MULTIPLIER).
//
// Callers (rel32 scan of both text sections): 0x8F0A70 (per frame), 0x8E0C4B
// (model change, visible 1) and 0x322B32F (spawn, lod 0, visible 1). Every
// call goes to the original.
//
// The director of a ped, as 0x2BDDE1C(ped) finds it (pure reads):
//   slot = (u32 [ped+0x9C] & 0x1FFFF) - u32 [base+0x3CD4380];
//   entry = u64 [u64 [base+0x3CD4388] + slot * 16] & ~1 (0: none);
//   director = u64 [entry+0x28].
// The detour only compares pointers, it never reads through them.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::animlod
{
    constexpr std::uintptr_t kAnimTableBaseIndexRva = 0x3CD4380;
    constexpr std::uintptr_t kAnimTableRva = 0x3CD4388;
    constexpr std::size_t kAnimRowBytes = 16;
    constexpr std::size_t kDirectorOffset = 0x28;
    constexpr std::size_t kEntityTypeOffset = 0x30;
    constexpr std::uint8_t kEntityTypePed = 4;
    constexpr std::size_t kComponentHandleOffset = 0x9C;
    constexpr std::uint32_t kComponentSlotMask = 0x1FFFF;
    constexpr std::uint32_t kMaxSlot = 0x10000;
    // The directors forced at once: the recorded peds and animals (the
    // recorder's capacity is 56 by default, 256 at most).
    constexpr std::uint32_t kMaxForced = 256;

    // The anim director of `ped` (0 when the chain does not hold).
    // read(address, out, size) -> bool: a guarded read. Pure, unit tested in
    // tests/scenery_speech_tests.cpp.
    template <typename Read>
    std::uintptr_t DirectorOf(std::uintptr_t base, std::uintptr_t ped, Read read)
    {
        if (!base || !ped)
            return 0;
        std::uint8_t type = 0;
        if (!read(ped + kEntityTypeOffset, &type, sizeof(type)) || type != kEntityTypePed)
            return 0;
        std::uint32_t handle = 0;
        std::uint32_t baseIndex = 0;
        std::uintptr_t table = 0;
        if (!read(ped + kComponentHandleOffset, &handle, sizeof(handle)) ||
            !read(base + kAnimTableBaseIndexRva, &baseIndex, sizeof(baseIndex)) ||
            !read(base + kAnimTableRva, &table, sizeof(table)) || !table)
        {
            return 0;
        }
        const std::uint32_t slot = handle & kComponentSlotMask;
        if (slot < baseIndex || slot - baseIndex >= kMaxSlot)
            return 0;
        std::uintptr_t entry = 0;
        if (!read(table + static_cast<std::uintptr_t>(slot - baseIndex) * kAnimRowBytes, &entry, sizeof(entry)) || !entry)
            return 0;
        entry &= ~std::uintptr_t{1};
        std::uintptr_t director = 0;
        if (!read(entry + kDirectorOffset, &director, sizeof(director)))
            return 0;
        return director;
    }

    // Engine side (anim_lod_hook.cpp).
    bool Install(std::string& error);
    void Shutdown();
    bool Installed();
    // The directors to force from now on (replaces the list; empty: none).
    void SetForced(const std::vector<std::uintptr_t>& directors);

    // Two users share the detour: the recording (its recorded peds) and the
    // playback (the ped and horse puppets, the same "animate as seen near"
    // the recording had, so the expressions that write more than bones,
    // such as the animated normal maps of a horse's muscles, stay on for
    // the puppet as they were for the original). Each replaces its own list;
    // the detour sees both. Script thread only.
    enum class Owner : std::uint8_t
    {
        Capture = 0,
        Playback = 1,
    };
    void SetForcedFor(Owner owner, const std::vector<std::uintptr_t>& directors);

    struct Stats
    {
        std::uint64_t calls = 0;   // every call of 0x2BD410C
        std::uint64_t forced = 0;  // calls for a forced director
        std::uint64_t changed = 0; // of those, the ones that had lod > 0 or visible 0
        // Of the forced calls, those whose engine (lod, visible) differs from
        // the previous forced call of the same list slot: the engine's own
        // choice flipping between frames (without the detour, the expressions
        // would switch on and off with it).
        std::uint64_t flips = 0;
    };
    Stats Snapshot();
    void ResetStats();
}
