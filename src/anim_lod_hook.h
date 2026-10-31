#pragma once

// Keeps the recorded peds animated at full detail whatever the game camera
// sees, the way a replay recorder raises fidelity while it records.
//
// The LOD setter (kSetLod: fwAnimDirector* rcx, u8 lod dl, bool visible r8b)
// is where the engine hands a ped's animation LOD to its anim director, every
// frame for each pre-rendered ped. The detour passes lod 0 and visible 1 for
// the directors of the recorded peds (the animation of a ped near and on
// screen) and changes nothing else: the draw LOD, fade and draw distance come
// from the graphics component, which is not touched. The anim-visible byte
// (anim_visible.h) alone fixes only the off screen case, not the distance LOD.
// Every call goes to the original.
//
// The detour only compares director pointers (found through the creature
// lookup, engine_sigs.h kCreatureLookup), it never reads through them.
// Evidence: offline dump (callers by rel32 scan).
// Research: docs/research/pose-bind-and-finalizer.md

#include "engine_sigs.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::animlod
{
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
    std::uintptr_t DirectorOf(const enginesig::Pool& creatures, std::uintptr_t ped, Read read)
    {
        if (!creatures.Found() || !ped)
            return 0;
        std::uint8_t type = 0;
        if (!read(ped + kEntityTypeOffset, &type, sizeof(type)) || type != kEntityTypePed)
            return 0;
        std::uint32_t handle = 0;
        std::uint32_t baseIndex = 0;
        std::uintptr_t table = 0;
        if (!read(ped + kComponentHandleOffset, &handle, sizeof(handle)) ||
            !read(creatures.baseIndex, &baseIndex, sizeof(baseIndex)) ||
            !read(creatures.table, &table, sizeof(table)) || !table)
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
        std::uint64_t calls = 0;   // every call of the LOD setter
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
