#pragma once

// The order a puppet weapon gets its recorded components in, pure and unit
// tested (QA report 232-Valentine).
//
// The recorded component list is in hash order, which can give a wrap
// (COMPONENT_RIFLE_BOLTACTION_WRAP) before the scope. Built in that order the
// puppet draws no scope and the wrap in its scopeless shape (two stiff
// leather loops). Tested in game: regiving the wrap once the scope is on draws
// both as the live game does.
// Research: docs/research/peds-weapon-straps.md
//
// So the wraps go last: every other component first, then each wrap. A wrap
// the weapon already has (a default, or one given earlier) is removed and
// given again when any other component was just given, so it is always
// built over the final set.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace replay::weaponorder
{
    // A wrap component by name: "_WRAP" at the end or followed by a digit
    // (COMPONENT_RIFLE_BOLTACTION_WRAP, COMPONENT_REPEATER_CARBINE_WRAP3),
    // never a tint (COMPONENT_LONGARM_WRAP_TINT_A_5, a material only).
    inline bool IsWrapName(const char* name)
    {
        if (!name)
            return false;
        for (const char* at = std::strstr(name, "_WRAP"); at; at = std::strstr(at + 1, "_WRAP"))
        {
            const char next = at[5];
            if (next == '\0' || (next >= '0' && next <= '9'))
                return true;
        }
        return false;
    }

    struct GiveStep
    {
        std::uint32_t hash = 0;
        bool removeFirst = false; // a wrap already on: remove, then give
    };

    // The steps for one weapon: has(hash) is whether it has the component
    // now, isWrap(hash) whether the component is a wrap.
    template <typename Has, typename IsWrap>
    std::vector<GiveStep> PlanGive(const std::vector<std::uint32_t>& components, Has has, IsWrap isWrap)
    {
        std::vector<GiveStep> steps;
        bool gaveOther = false;
        for (const std::uint32_t hash : components)
        {
            if (isWrap(hash) || has(hash))
                continue;
            steps.push_back({hash, false});
            gaveOther = true;
        }
        for (const std::uint32_t hash : components)
        {
            if (!isWrap(hash))
                continue;
            if (!has(hash))
                steps.push_back({hash, false});
            else if (gaveOther)
                steps.push_back({hash, true});
        }
        return steps;
    }
}
