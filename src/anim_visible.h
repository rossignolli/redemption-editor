#pragma once

// The engine's own "animate as if visible" byte of a ped (the ped graphics
// component's +0x4A8, kAnimVisibleTest).
//
// Off screen the anim director gets LOD 3 and every expression that stops
// below 3 (face rig, hat fit, brim springs, manes) is off, so its rows stay
// unwritten and the finalizer takes the bind pose. With this byte set the
// visibility test says visible and the ped is fully animated off screen. The
// engine sets it only from the pedshot slot setter, together with
// always-prerender and a LOD multiplier of 100; neither of those two alone
// fixes visibility.
//
// Component lookup (bytes of kComponentRead and kVisibilityTest): entity type
// byte +0x30 == 4, slot from [ped+0x9C], row stride 0x148, component
// [row+0x98] & ~1, checked by ([component+8] & ~0xF) == ped.
// Evidence: offline dump.
// Research: docs/research/peds-puppets.md
//
// Pure: the reads go through a callable, unit tested in tests/scenery_speech_tests.cpp.

#include "engine_sigs.h"

#include <cstddef>
#include <cstdint>

namespace replay::animvisible
{
    // The visibility test (this = the graphics component, its ped at +8 &
    // ~0xF), reached from the builder's call, and its read of the byte: the
    // offset the recording writes.
    inline const sig::Signature kVisibilityTest{"visibility_test", enginesig::kComponentRead, sig::Rel32{0x3E, 0x42},
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 55 41 56 41 57 48 83 EC 40 48 8B 71 08 4C 8B F1 "
        "48 8B 15 ?? ?? ?? ?? 48 83 E6 F0 45 32 C9 45 32 D2 41 BD 01 00 00 00 8B 86 9C 00 00 00 25 FF FF 01 00 2B 05 "
        "?? ?? ?? ?? 4C 69 C0 48 01 00 00 49 8B 84 10 B8 00 00 00",
        0x8F0740};
    inline const sig::Signature kAnimVisibleTest{"anim_visible_test", kVisibilityTest, sig::Offset{0x251},
        "41 80 BE A8 04 00 00 00", 0x8F0991};

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
    std::uintptr_t GraphicsComponent(const enginesig::Pool& pool, std::uintptr_t ped, Read read)
    {
        if (!pool.Found() || !ped)
            return 0;
        std::uint8_t type = 0;
        if (!read(ped + kEntityTypeOffset, &type, sizeof(type)) || type != kEntityTypePed)
            return 0;
        std::uint32_t handle = 0;
        std::uint32_t baseIndex = 0;
        std::uintptr_t table = 0;
        if (!read(ped + kComponentHandleOffset, &handle, sizeof(handle)) ||
            !read(pool.baseIndex, &baseIndex, sizeof(baseIndex)) ||
            !read(pool.table, &table, sizeof(table)) || !table)
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
