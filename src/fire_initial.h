#pragma once

// The fires a clip starts with, read from the engine's own fire list instead
// of only from what the start detour saw. Pure logic (tests in
// tests/scenery_speech_tests.cpp); the reads are in fire_observer.cpp
// (ReadActiveFires) and the use in replay_events.cpp.
//
// Evidence (build 1491.50):
// - A fire is made by CFire::Init 0x1282CC0 (with 0x12870E4) from a
//   descriptor; StartFireCore 0x12B5EA4 is one caller (every script, map,
//   explosion, spread start), but the ptfx fire registration 0x12A1698
//   (reached from the particle instance update 0x1405838 -> 0x145DCD0 ->
//   0x12A1DE8, type 11) calls GetFreeFire 0x127D1E4 and CFire::Init
//   directly: the start detour never sees those fires, whenever they start.
//   The registration has other owners too (0x12C7380 type 11, an
//   entry of a 16-entry system updated from 0x12BDAF8; 0x12A22F4 type 10,
//   from vfx 0x14303A4; 0x12A248C type 9, from 0x15C9854), all with the
//   owner in desc+0xB0: "ptfx-owned" below means registered by any owner.
// - CFire::Init sets flags +0xE8 |= 0x180 when desc+0xB0 (the registering
//   effect instance, kept at fire+0x1A8) is not null; the registration sets
//   0x100 again each frame it finds its fire (0x12A170C); CFire::Update
//   0x12BC2D4 shuts a fire with 0x80 and without 0x100 down (0x12BC36F to
//   0x12BC388). Such a fire lives exactly as long as its effect plays: it is
//   the effect's, not a fire the replay can own on its own.
// - The field copies of Init (desc -> fire): +0x00 -> +0x10, +0x10 -> +0x20,
//   +0x20 (vec4, w = incident group, refcounted) -> +0x50, +0x30 -> +0x120,
//   +0x40 -> +0x70, +0x48 -> +0x78, +0x50 -> +0x98, +0x54 -> +0x9C,
//   +0x58 -> +0xA0, +0x60 -> +0x88, +0x68 -> +0x184, +0x6C -> +0x18C,
//   +0x70 -> +0x190, +0x7C -> +0x14C, +0x90 -> +0x40, +0xA0 -> +0xA4,
//   +0xA4 -> +0x140, +0xA8 -> +0x144, +0xB0 -> +0x1A8, +0xB8 -> +0x1A0,
//   +0xC0 -> +0x80, +0xE0 -> +0x1C4, +0xE4 -> +0x154 (and the fire's seed),
//   +0xE8 -> +0x90, +0x110 -> +0xDC; desc+0x114 bit 0x40 -> +0x184 = FLT_MAX
//   (burns until stopped), bits 0x01/0x02/0x04/0x20 -> +0xE8 bits 11/10/12/13,
//   desc+0x115 bit 0x02 -> +0xE8 bit 2. The defaults of the descriptor
//   constructor 0x1251EBC fill the rest.
// - StopFireInRange 0x1272740 (the STOP_FIRE_IN_RANGE native) walks the
//   active list (+0xB18B8 count, +0xB18BC int[128]), skips fires with +0xE8
//   bit 11 (0x800) and shuts the others within the 3D radius of +0x30 down
//   with Shutdown(fire, 0, 0).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace replay::fire
{
    // CFire fields (offsets into the 0x1F0 state).
    constexpr std::size_t kFireLocalPos = 0x10;
    constexpr std::size_t kFireNormal = 0x20;
    constexpr std::size_t kFireWorldPos = 0x30;
    constexpr std::size_t kFireParentPos = 0x40;
    constexpr std::size_t kFireGroupVec = 0x50;
    constexpr std::size_t kFireEntity = 0x70;
    constexpr std::size_t kFireEntity2 = 0x78;
    constexpr std::size_t kFireMaterial = 0x88;
    constexpr std::size_t kFireField90 = 0x90;
    constexpr std::size_t kFireBone = 0x98;
    constexpr std::size_t kFireType = 0x9C;
    constexpr std::size_t kFireFieldA0 = 0xA0;
    constexpr std::size_t kFireVfxInfo = 0xA4;
    constexpr std::size_t kFireFieldDC = 0xDC;
    constexpr std::size_t kFireFlags = 0xE8;
    constexpr std::size_t kFireField120 = 0x120;
    constexpr std::size_t kFireField140 = 0x140;
    constexpr std::size_t kFireField144 = 0x144;
    constexpr std::size_t kFireField14C = 0x14C;
    constexpr std::size_t kFireSeed = 0x154;
    constexpr std::size_t kFireBurnTime = 0x184;
    constexpr std::size_t kFireMaxStrength = 0x18C;
    constexpr std::size_t kFirePeakTime = 0x190;
    constexpr std::size_t kFireRegIndex = 0x1A0;
    constexpr std::size_t kFireRegOwner = 0x1A8;
    constexpr std::size_t kFireField1C4 = 0x1C4;

    constexpr std::uint32_t kFireFlagPtfx = 0x80;         // registered by a particle effect
    constexpr std::uint32_t kFireFlagStopImmune = 0x800;  // StopFireInRange skips it
    constexpr float kFireMatchTolerance = 0.5f;           // metres

    template <class T>
    T FireField(const std::uint8_t* state, std::size_t offset)
    {
        T value{};
        std::memcpy(&value, state + offset, sizeof(value));
        return value;
    }

    // A fire that belongs to the owner that registers it every frame (a
    // particle effect or another registering system; 0x80 or an owner at
    // +0x1A8): it ends when its owner stops registering it.
    inline bool PtfxOwnedFire(const std::uint8_t* state)
    {
        return (FireField<std::uint32_t>(state, kFireFlags) & kFireFlagPtfx) != 0 ||
            FireField<std::uint64_t>(state, kFireRegOwner) != 0;
    }

    inline bool StopInRangeImmune(const std::uint8_t* state)
    {
        return (FireField<std::uint32_t>(state, kFireFlags) & kFireFlagStopImmune) != 0;
    }

    // A fire with no burn-out (desc+0x114 bit 0x40: burn time FLT_MAX).
    inline bool PersistentFire(const std::uint8_t* state)
    {
        return FireField<float>(state, kFireBurnTime) >= 1.0e30f;
    }

    // The descriptor a fire was made from, rebuilt from the fire: the
    // constructor defaults (0x1251EBC) and every field CFire::Init copies,
    // reversed. No incident group (+0x2C = -1: the group table is
    // refcounted by Init and the descriptor destructor), no effect owner
    // (ptfx-owned fires are never rebuilt), no culprit, no parent.
    // `desc` must hold 0x120 bytes.
    inline void DescFromFire(const std::uint8_t* state, std::uint8_t* desc)
    {
        std::memset(desc, 0, 0x120);
        const auto put32 = [&](std::size_t offset, std::uint32_t value) { std::memcpy(desc + offset, &value, 4); };
        const auto putF = [&](std::size_t offset, float value) { std::memcpy(desc + offset, &value, 4); };
        const auto copy = [&](std::size_t to, std::size_t from, std::size_t size) { std::memcpy(desc + to, state + from, size); };
        // Constructor defaults (the fields Init does not take from a fire).
        put32(0x2C, 0xFFFFFFFFu);
        put32(0x50, 0xFFFFFFFFu);
        put32(0x58, 0xFFFFFFFFu);
        putF(0x68, 10.0f);
        putF(0x6C, 1.0f);
        putF(0x70, 1.0f);
        putF(0x78, 1.0f);
        put32(0xA0, 0xFFFFFFFFu);
        put32(0x108, 0xFFFCFFFFu); // u16 -1, then u16 0xFFFC
        put32(0x10C, 0xFFFFFFFFu);
        put32(0x110, 0xFFFFFFFFu);
        putF(0x118, -1.0f);
        // Init's copies, reversed.
        copy(0x00, kFireLocalPos, 16);
        copy(0x10, kFireNormal, 16);
        copy(0x20, kFireGroupVec, 12);
        put32(0x2C, 0xFFFFFFFFu);
        copy(0x30, kFireField120, 16);
        copy(0x40, kFireEntity, 8);
        copy(0x48, kFireEntity2, 8);
        copy(0x50, kFireBone, 4);
        copy(0x54, kFireType, 4);
        copy(0x58, kFireFieldA0, 4);
        copy(0x60, kFireMaterial, 8);
        copy(0x68, kFireBurnTime, 4);
        copy(0x6C, kFireMaxStrength, 4);
        copy(0x70, kFirePeakTime, 4);
        copy(0x7C, kFireField14C, 4);
        copy(0x90, kFireParentPos, 16);
        copy(0xA0, kFireVfxInfo, 4);
        copy(0xA4, kFireField140, 4);
        copy(0xA8, kFireField144, 4);
        copy(0xE0, kFireField1C4, 4);
        copy(0xE4, kFireSeed, 4);
        copy(0xE8, kFireField90, 8);
        copy(0x110, kFireFieldDC, 4);
        const std::uint32_t flags = FireField<std::uint32_t>(state, kFireFlags);
        std::uint8_t b114 = 0;
        if (flags & (1u << 11))
            b114 |= 0x01;
        if (flags & (1u << 10))
            b114 |= 0x02;
        if (flags & (1u << 12))
            b114 |= 0x04;
        if (flags & (1u << 13))
            b114 |= 0x20;
        if (PersistentFire(state))
            b114 |= 0x40;
        desc[0x114] = b114;
        // +0x115 bit 0 (the parent line-of-sight test) stays clear, as the
        // replay clears it on every start.
        desc[0x115] = (flags & (1u << 2)) ? 0x02 : 0x00;
    }

    struct FireSpot
    {
        float pos[3] = {};
        std::uint32_t type = 0;
    };

    // The world position (+0x30) of a fire state (unaligned copy).
    struct FirePos
    {
        float v[3] = {};
        explicit FirePos(const std::uint8_t* state) { std::memcpy(v, state + kFireWorldPos, sizeof(v)); }
    };

    inline float FireDistance(const float* a, const float* b)
    {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // For each wanted spot the index of the live spot that is the same fire
    // (same type, within `tolerance`), or -1. Closest pairs first; a live
    // spot is used once.
    inline std::vector<int> MatchFireSpots(const std::vector<FireSpot>& wanted, const std::vector<FireSpot>& live,
        float tolerance = kFireMatchTolerance)
    {
        std::vector<int> out(wanted.size(), -1);
        std::vector<std::pair<float, std::pair<std::size_t, std::size_t>>> pairs;
        for (std::size_t w = 0; w < wanted.size(); ++w)
            for (std::size_t l = 0; l < live.size(); ++l)
            {
                if (wanted[w].type != live[l].type)
                    continue;
                const float distance = FireDistance(wanted[w].pos, live[l].pos);
                if (distance <= tolerance)
                    pairs.push_back({distance, {w, l}});
            }
        std::stable_sort(pairs.begin(), pairs.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<bool> used(live.size(), false);
        for (const auto& [distance, pair] : pairs)
        {
            (void)distance;
            if (out[pair.first] >= 0 || used[pair.second])
                continue;
            out[pair.first] = static_cast<int>(pair.second);
            used[pair.second] = true;
        }
        return out;
    }

    // The clip-aware clear's verdict for one live fire in the clear radius.
    enum class ClearVerdict : std::uint8_t
    {
        Stop = 0,
        Protected = 1,   // the world's own copy of a recorded initial fire
        PtfxOwned = 2,   // its effect owns it (ends with the effect)
        StopImmune = 3,  // the native skips it too (+0xE8 bit 11)
        Ours = 4,        // a fire of the replay (the caller handles it)
    };
    inline ClearVerdict ClearVerdictFor(const std::uint8_t* state, bool isProtected, bool isOurs)
    {
        if (isOurs)
            return ClearVerdict::Ours;
        if (isProtected)
            return ClearVerdict::Protected;
        if (StopInRangeImmune(state))
            return ClearVerdict::StopImmune;
        if (PtfxOwnedFire(state))
            return ClearVerdict::PtfxOwned;
        return ClearVerdict::Stop;
    }
    inline const char* ClearVerdictName(ClearVerdict verdict)
    {
        switch (verdict)
        {
        case ClearVerdict::Stop: return "other";
        case ClearVerdict::Protected: return "adopted";
        case ClearVerdict::PtfxOwned: return "ptfx_owned";
        case ClearVerdict::StopImmune: return "stop_immune";
        case ClearVerdict::Ours: return "ours";
        }
        return "?";
    }
}
