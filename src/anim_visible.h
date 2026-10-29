#pragma once

// The engine's own "animate as if visible" byte of a ped.
//
// Every frame the pre-render builder 0x56F4B8 calls 0x8F0740 for each ped it
// pre-renders. 0x8F0740 decides whether the ped is visible for the animation:
// one of its visibility stamps (+0x102, +0x103, +0x104) equals the frame tag
// [0x4A468F5], or the ped graphics component's byte +0x4A8 is set (0x8F0991),
// or the mount or attach parent is visible, or a global animscene state is on.
// It then calls 0x2BD410C(anim director, lod = component+0x4A0, visible),
// whose tail 0x2BD413C stores the creature component's effective LOD
// (+0xAF) = visible ? lod : 3. A change of it reaches the expressions
// component (message 0xE, 0x2BD036C -> 0x295560C): an expression stays on
// only while its LOD range [+0x81, +0x82] holds the LOD (0x290F864). Off
// screen the LOD is 3 and every expression that stops below 3 (face rig, hat
// fit, brim springs, manes) is off: its rows stay unwritten (0xFFFFFFFF
// lanes) and the finalizer 0x266C294 takes the bind pose (measured: +530 to
// +810 rows per human, +661 per horse, +280 per dog).
//
// The byte is written only by the component's constructor (0, 0x886669) and
// by the pedshot slot setter 0x5E6A78 (1 on assign, 0 on release, together
// with always-prerender and a LOD multiplier of 100): the engine's recipe for
// a ped fully animated off screen. SET_PED_LOD_MULTIPLIER only feeds the
// requested LOD (component+0x49C -> 0x8F0AD4 -> +0x4A0): it fixes distance,
// not visibility; SET_ENTITY_ALWAYS_PRERENDER only puts the ped in the
// pre-render set (0x8F0740 runs, visible stays false).
//
// Layout (build 1491.50):
//   entity type byte +0x30 == 4 (ped);
//   slot = (u32 [ped+0x9C] & 0x1FFFF) - u32 [base+0x39ECE40];
//   row = u64 [base+0x39ECE48] + slot * 0x148;
//   component = u64 [row+0x98] & ~1 (0: none);
//   check: (u64 [component+8] & ~0xF) == ped.
// The two table globals are runtime .data of this build.
//
// Pure: the reads go through a callable, unit tested in tests/scenery_speech_tests.cpp.

#include <cstddef>
#include <cstdint>

namespace replay::animvisible
{
    constexpr std::uintptr_t kComponentBaseIndexRva = 0x39ECE40;
    constexpr std::uintptr_t kComponentTableRva = 0x39ECE48;
    constexpr std::size_t kEntityTypeOffset = 0x30;
    constexpr std::uint8_t kEntityTypePed = 4;
    constexpr std::size_t kComponentHandleOffset = 0x9C;
    constexpr std::uint32_t kComponentSlotMask = 0x1FFFF;
    constexpr std::size_t kComponentRowBytes = 0x148;
    constexpr std::size_t kGraphicsComponentSlot = 0x98;
    constexpr std::size_t kComponentOwnerOffset = 0x8;
    constexpr std::size_t kAnimVisibleOffset = 0x4A8;
    // Slots far beyond any ped pool: a garbage handle is refused before it
    // becomes an address.
    constexpr std::uint32_t kMaxSlot = 0x10000;

    // The graphics component of `ped` (0 when the chain does not hold).
    // read(address, out, size) -> bool: a guarded read.
    template <typename Read>
    std::uintptr_t GraphicsComponent(std::uintptr_t base, std::uintptr_t ped, Read read)
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
            !read(base + kComponentBaseIndexRva, &baseIndex, sizeof(baseIndex)) ||
            !read(base + kComponentTableRva, &table, sizeof(table)) || !table)
        {
            return 0;
        }
        const std::uint32_t slot = handle & kComponentSlotMask;
        if (slot < baseIndex || slot - baseIndex >= kMaxSlot)
            return 0;
        std::uintptr_t component = 0;
        const std::uintptr_t row = table + static_cast<std::uintptr_t>(slot - baseIndex) * kComponentRowBytes;
        if (!read(row + kGraphicsComponentSlot, &component, sizeof(component)) || !component)
            return 0;
        component &= ~std::uintptr_t{1};
        std::uintptr_t owner = 0;
        if (!read(component + kComponentOwnerOffset, &owner, sizeof(owner)) || (owner & ~std::uintptr_t{0xF}) != ped)
            return 0;
        return component;
    }
}
