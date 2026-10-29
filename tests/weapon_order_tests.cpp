// The order a puppet weapon gets its components in (weapon_order_core.h):
// the wraps after everything else, so the bolt-action WRAP is built over the
// scope (QA report 232-Valentine).
#include "weapon_order_core.h"
#include "weapon_components.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    int g_orderFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_orderFailures;
    }

    namespace wo = replay::weaponorder;

    constexpr std::uint32_t kWrap = 0x549A7FC8u;  // COMPONENT_RIFLE_BOLTACTION_WRAP
    constexpr std::uint32_t kScope = 0x920B56D7u; // COMPONENT_RIFLE_SCOPE03
    constexpr std::uint32_t kSight = 0x4E2B941Cu; // COMPONENT_RIFLE_BOLTACTION_SIGHT_WIDE
    constexpr std::uint32_t kTint = 0x8BA47E5Cu;  // COMPONENT_LONGARM_WRAP_TINT_A_5
    constexpr std::uint32_t kGrip = 0xFFA150A5u;  // COMPONENT_RIFLE_BOLTACTION_GRIP_ENGRAVED

    bool IsWrapHash(std::uint32_t hash)
    {
        for (const auto& component : replay::weapon::kComponents)
        {
            if (component.hash == hash)
                return wo::IsWrapName(component.name);
        }
        return false;
    }

    void TestNames()
    {
        Check(wo::IsWrapName("COMPONENT_RIFLE_BOLTACTION_WRAP"), "the bolt-action wrap");
        Check(wo::IsWrapName("COMPONENT_REPEATER_CARBINE_WRAP3"), "a numbered wrap");
        Check(!wo::IsWrapName("COMPONENT_LONGARM_WRAP_TINT_A_5"), "a wrap tint is not a wrap");
        Check(!wo::IsWrapName("COMPONENT_RIFLE_SCOPE03"), "a scope");
        Check(!wo::IsWrapName(nullptr) && !wo::IsWrapName(""), "no name");
        Check(IsWrapHash(kWrap) && !IsWrapHash(kTint) && !IsWrapHash(kScope), "by hash, from the list");
    }

    void TestQaRifle()
    {
        // The recorded list of 51:1574 is in hash order: the wrap before the scope.
        const std::vector<std::uint32_t> recorded{0x06BA0BAAu, kSight, kWrap, kTint, kScope, kGrip};
        Check(std::find(recorded.begin(), recorded.end(), kWrap) < std::find(recorded.begin(), recorded.end(), kScope),
            "the recording has the wrap first");
        std::vector<std::uint32_t> on; // a new puppet: nothing yet
        const auto has = [&](std::uint32_t hash) { return std::find(on.begin(), on.end(), hash) != on.end(); };
        const auto steps = wo::PlanGive(recorded, has, IsWrapHash);
        Check(steps.size() == recorded.size(), "every component given once");
        Check(!steps.empty() && steps.back().hash == kWrap && !steps.back().removeFirst, "the wrap last, given");
        std::size_t scopeAt = steps.size();
        std::size_t wrapAt = steps.size();
        for (std::size_t n = 0; n < steps.size(); ++n)
        {
            if (steps[n].hash == kScope)
                scopeAt = n;
            if (steps[n].hash == kWrap)
                wrapAt = n;
        }
        Check(scopeAt < wrapAt, "the scope before the wrap");
        // The other components keep their recorded order.
        Check(steps[0].hash == 0x06BA0BAAu && steps[1].hash == kSight && steps[2].hash == kTint &&
                steps[3].hash == kScope && steps[4].hash == kGrip,
            "the rest in recorded order");
    }

    void TestWrapAlreadyOn()
    {
        const std::vector<std::uint32_t> recorded{kWrap, kScope};
        // The wrap on (a default), the scope missing: give the scope, then
        // build the wrap again over it.
        std::vector<std::uint32_t> on{kWrap};
        auto has = [&](std::uint32_t hash) { return std::find(on.begin(), on.end(), hash) != on.end(); };
        auto steps = wo::PlanGive(recorded, has, IsWrapHash);
        Check(steps.size() == 2 && steps[0].hash == kScope && !steps[0].removeFirst && steps[1].hash == kWrap &&
                steps[1].removeFirst,
            "wrap on, scope given: the wrap rebuilt after it");
        // Everything on already: nothing to do (no rebuild every call).
        on = {kWrap, kScope};
        steps = wo::PlanGive(recorded, has, IsWrapHash);
        Check(steps.empty(), "all on: nothing");
        // No wrap in the recording: the plain give, in order.
        on.clear();
        steps = wo::PlanGive(std::vector<std::uint32_t>{kScope, kSight}, has, IsWrapHash);
        Check(steps.size() == 2 && steps[0].hash == kScope && steps[1].hash == kSight, "no wrap: as before");
    }
}

int RunWeaponOrderTests()
{
    TestNames();
    TestQaRifle();
    TestWrapAlreadyOn();
    if (g_orderFailures == 0)
        std::printf("weapon component order tests: ok\n");
    return g_orderFailures;
}
