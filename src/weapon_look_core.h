#pragma once

// The look of a weapon object as the replay records and plays it, pure and
// unit tested, shared by the mod and the clip tool.
//
// Per recorded weapon object, a state track (the WCND section, version 2)
// keeps:
//   - the components the engine lists on the weapon (CWeapon at
//     [object+0x448]: the model components in the array at +0x390, count
//     +0x3E0, and the decorations, materials, tints and engravings, in the
//     hash table at +0x10, buckets +0x18, offline from the core of
//     HAS_WEAPON_GOT_WEAPON_COMPONENT 0x10EABB4), as a sorted set;
//   - the wear: degradation +0xD0, permanent degradation (rust) +0xD4, soot
//     +0xD8, visual damage +0xDC, dirt +0xE0 and the sixth channel +0xE4
//     (offline: the getters 0x10E9188/0x10E9584/0x10E90A0/0x1164FD0/
//     0x10E94C0, the setters 0x119FFC0/0x11A21F8/0x11A1A58/0x119FED8/
//     0x11A14E8/0x119F310, and the weapon create 0x1E0A6C0 that applies all
//     six from one struct).
// A state is stored when the component set changes or one wear value moves
// more than kWearStep; playback applies the state at the playhead and reads
// the puppet weapon back (components equal? wear equal?).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace replay::weaponlook
{
    // ---- engine layout (build 1491.50) --------------------------------------

    constexpr std::uint32_t kObjectWeapon = 0x448;     // CObject -> CWeapon
    constexpr std::uint32_t kWeaponInfo = 0x60;        // CWeapon -> weapon info (null: no condition)
    constexpr std::uint32_t kComponentTable = 0x10;    // bucket array (decorations)
    constexpr std::uint32_t kComponentBuckets = 0x18;  // u32 bucket count
    constexpr std::uint32_t kComponentArray = 0x390;   // model components, 8-byte pointers
    constexpr std::uint32_t kComponentCount = 0x3E0;   // s32
    constexpr std::uint32_t kComponentInfo = 0x10;     // component -> info
    constexpr std::uint32_t kInfoHash = 0x20;          // info -> component hash
    constexpr std::uint32_t kNodeValue = 0x08;         // table node -> component
    constexpr std::uint32_t kNodeNext = 0x10;          // table node -> next node
    constexpr std::uint32_t kDegradation = 0xD0;
    constexpr std::uint32_t kPermanent = 0xD4;
    constexpr std::uint32_t kSoot = 0xD8;
    constexpr std::uint32_t kDamage = 0xDC;
    constexpr std::uint32_t kDirt = 0xE0;
    constexpr std::uint32_t kWearE4 = 0xE4;
    // The dirt the drawing uses: the dirt setter (0x119FFC0 -> 0x11A0054)
    // writes it into the weapon shader effect only when the drawable has one
    // ([[weapon+0xE8]+0x28]+0x18, type byte +0xA & 0x1F == 0x16), at
    // [[effect+0x18]+0xC8] (0xB0F6B0). A weapon set before its drawable
    // exists keeps the field and not the drawn value.
    constexpr std::uint32_t kWeaponDrawHolder = 0xE8;
    constexpr std::uint32_t kDrawHolderDrawable = 0x28;
    constexpr std::uint32_t kDrawableEffect = 0x18;
    constexpr std::uint32_t kEffectType = 0x0A;
    constexpr std::uint8_t kEffectTypeWeapon = 0x16;
    constexpr std::uint32_t kEffectData = 0x18;
    constexpr std::uint32_t kEffectDirt = 0xC8;

    // Bounds of the engine read (a stale pointer never walks forever).
    constexpr std::int32_t kArrayMax = 16;
    constexpr std::uint32_t kBucketsMax = 1024;
    constexpr std::uint32_t kChainMax = 64;
    constexpr std::size_t kComponentsMax = 64;

    // A wear value is stored and compared at this resolution.
    constexpr float kWearStep = 0.01f;
    constexpr float kWearTolerance = 0.02f;
    // Playback reads the puppet back this long after an apply, then every
    // kRecheckMs of playhead (the engine itself adds soot when it shoots).
    constexpr std::uint32_t kCheckDelayMs = 150;
    constexpr std::uint32_t kRecheckMs = 1000;

    // flags of a state
    constexpr std::uint8_t kFlagComponents = 0x01;      // components: the engine list
    constexpr std::uint8_t kFlagWearRaw = 0x02;         // the six wear fields read from the weapon
    constexpr std::uint8_t kFlagComponentsNames = 0x04; // components: the public name list (fallback)

    struct Wear
    {
        float dirt = 0.0f;
        float soot = 0.0f;
        float damage = 0.0f;
        float degradation = 0.0f;
        float permanent = 0.0f;
        float e4 = 0.0f;
    };

    inline float Clean(float value)
    {
        if (!std::isfinite(value) || value < 0.0f)
            return 0.0f;
        return value > 1.0f ? 1.0f : value;
    }

    inline Wear Clean(const Wear& wear)
    {
        return {Clean(wear.dirt), Clean(wear.soot), Clean(wear.damage), Clean(wear.degradation), Clean(wear.permanent),
            Clean(wear.e4)};
    }

    // Sorted, unique, zero dropped, at most kComponentsMax.
    inline void Normalize(std::vector<std::uint32_t>& components)
    {
        components.erase(std::remove(components.begin(), components.end(), 0u), components.end());
        std::sort(components.begin(), components.end());
        components.erase(std::unique(components.begin(), components.end()), components.end());
        if (components.size() > kComponentsMax)
            components.resize(kComponentsMax);
    }

    inline float MaxWearDiff(const Wear& a, const Wear& b, bool withRaw)
    {
        float worst = std::max({std::fabs(a.dirt - b.dirt), std::fabs(a.soot - b.soot), std::fabs(a.damage - b.damage),
            std::fabs(a.degradation - b.degradation)});
        if (withRaw)
            worst = std::max({worst, std::fabs(a.permanent - b.permanent), std::fabs(a.e4 - b.e4)});
        return worst;
    }

    inline bool WearMoved(const Wear& a, const Wear& b) { return MaxWearDiff(a, b, true) > kWearStep; }

    // recorded minus actual (missing on the puppet) and actual minus
    // recorded (extra on the puppet); both inputs normalized.
    inline void Diff(const std::vector<std::uint32_t>& recorded, const std::vector<std::uint32_t>& actual,
        std::vector<std::uint32_t>& missing, std::vector<std::uint32_t>& extra)
    {
        missing.clear();
        extra.clear();
        std::set_difference(recorded.begin(), recorded.end(), actual.begin(), actual.end(), std::back_inserter(missing));
        std::set_difference(actual.begin(), actual.end(), recorded.begin(), recorded.end(), std::back_inserter(extra));
    }

    // Index of the state in force at the playhead: the last one at or before
    // it, else the first (a weapon seen before its first sample looks like
    // its first sample). -1 when the track is empty.
    template <typename State>
    int StateAt(const std::vector<std::pair<std::uint32_t, State>>& states, std::uint32_t timeMs)
    {
        if (states.empty())
            return -1;
        int index = 0;
        for (std::size_t n = 0; n < states.size() && states[n].first <= timeMs; ++n)
            index = static_cast<int>(n);
        return index;
    }

    // The verdict of one read back.
    struct Check
    {
        bool componentsKnown = false; // the state carries a component list
        bool componentsRead = false;  // the puppet's list was read
        std::size_t missing = 0;
        std::size_t extra = 0;
        bool wearRead = false;
        float wearDiff = 0.0f;
        bool ComponentsMismatch() const { return componentsKnown && componentsRead && (missing || extra); }
        bool WearMismatch() const { return wearRead && wearDiff > kWearTolerance; }
    };

    inline Check Compare(const std::vector<std::uint32_t>* recorded, const std::vector<std::uint32_t>* actual,
        const Wear* recordedWear, const Wear* actualWear, bool withRaw)
    {
        Check check;
        if (recorded && actual)
        {
            check.componentsKnown = true;
            check.componentsRead = true;
            std::vector<std::uint32_t> missing, extra;
            Diff(*recorded, *actual, missing, extra);
            check.missing = missing.size();
            check.extra = extra.size();
        }
        if (recordedWear && actualWear)
        {
            check.wearRead = true;
            check.wearDiff = MaxWearDiff(*recordedWear, *actualWear, withRaw);
        }
        return check;
    }

    inline std::string HashList(const std::vector<std::uint32_t>& hashes, std::size_t limit = 12)
    {
        if (hashes.empty())
            return "none";
        std::string out;
        char text[16];
        for (std::size_t n = 0; n < hashes.size() && n < limit; ++n)
        {
            std::snprintf(text, sizeof(text), "%s%08x", n ? ";" : "", hashes[n]);
            out += text;
        }
        if (hashes.size() > limit)
            out += ";+" + std::to_string(hashes.size() - limit);
        return out;
    }

    inline std::string WearText(const Wear& wear)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%.3f/%.3f/%.3f/%.3f/%.3f/%.3f", wear.dirt, wear.soot, wear.damage,
            wear.degradation, wear.permanent, wear.e4);
        return text;
    }

    // Playback tally for QA_HEALTH and the fx check.
    struct Tally
    {
        std::uint32_t checked = 0;            // weapons read back at least once
        std::uint32_t componentsMismatch = 0; // weapons whose last check disagreed on components
        std::uint32_t wearMismatch = 0;       // weapons whose last check disagreed on wear
        std::uint32_t applies = 0;            // states applied
        std::uint32_t reapplied = 0;          // wear set again after the engine moved it
    };
}
