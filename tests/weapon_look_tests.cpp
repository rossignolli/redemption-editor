// The weapon look as the replay records and plays it (weapon_look_core.h),
// with the components of a player's Volcanic and the wear of a player's
// Cattleman taken from real recordings.
#include "fx_check_core.h"
#include "weapon_look_core.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
    int g_weaponLookFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_weaponLookFailures;
    }

    namespace wl = replay::weaponlook;

    void TestNormalize()
    {
        std::vector<std::uint32_t> list = {0x9f63e06fu, 0u, 0x060e9cc0u, 0x9f63e06fu, 0xf6d79db8u};
        wl::Normalize(list);
        Check(list == std::vector<std::uint32_t>({0x060e9cc0u, 0x9f63e06fu, 0xf6d79db8u}), "normalize: sorted, unique, no zero");
        std::vector<std::uint32_t> many;
        for (std::uint32_t n = 1; n <= wl::kComponentsMax + 10; ++n)
            many.push_back(n);
        wl::Normalize(many);
        Check(many.size() == wl::kComponentsMax, "normalize: capped");
    }

    void TestDiff()
    {
        // The player's Volcanic: a fresh puppet has 9 of the 15 components;
        // the barrel, grip, sight and finishes of the gunsmith are missing.
        std::vector<std::uint32_t> recorded = {0x060e9cc0u, 0xf6d79db8u, 0x9f63e06fu, 0xcce6dc6fu, 0x1831f54du};
        std::vector<std::uint32_t> puppet = {0xcce6dc6fu, 0x1831f54du, 0x11111111u};
        wl::Normalize(recorded);
        wl::Normalize(puppet);
        std::vector<std::uint32_t> missing, extra;
        wl::Diff(recorded, puppet, missing, extra);
        Check(missing.size() == 3 && extra.size() == 1 && extra[0] == 0x11111111u, "diff: missing and extra");
        wl::Diff(recorded, recorded, missing, extra);
        Check(missing.empty() && extra.empty(), "diff: equal lists agree");
        const wl::Check check = wl::Compare(&recorded, &puppet, nullptr, nullptr, false);
        Check(check.ComponentsMismatch() && !check.WearMismatch() && check.missing == 3 && check.extra == 1,
            "compare: components mismatch, wear not read");
    }

    void TestWear()
    {
        Check(wl::Clean(std::numeric_limits<float>::quiet_NaN()) == 0.0f && wl::Clean(-1.0f) == 0.0f &&
                wl::Clean(2.0f) == 1.0f && wl::Clean(0.5f) == 0.5f,
            "clean: NaN, negative and above 1");
        // The player's Cattleman (111:280): soot rises 0.0105 a shot, below
        // the step one shot is not a state, two are.
        const wl::Wear a{0.792489f, 0.0f, 0.12294f, 0.401167f, 0.3f, 0.0f};
        wl::Wear b = a;
        b.soot = 0.0105f;
        Check(wl::WearMoved(a, b), "wear: 0.0105 of soot is a new state");
        b.soot = 0.005f;
        Check(!wl::WearMoved(a, b), "wear: half a step is not");
        b = a;
        b.permanent = 0.35f;
        Check(wl::WearMoved(a, b), "wear: rust is part of the state");
        Check(wl::MaxWearDiff(a, b, false) == 0.0f, "wear: rust left out of the native-only comparison");
        const wl::Check ok = wl::Compare(nullptr, nullptr, &a, &a, true);
        Check(!ok.WearMismatch() && ok.wearRead, "compare: same wear agrees");
        wl::Wear engine = a;
        engine.soot += 0.03f; // the replay fired the puppet weapon
        Check(wl::Compare(nullptr, nullptr, &a, &engine, true).WearMismatch(), "compare: wear moved past the tolerance");
        // NPC weapons: dirt, soot and damage equal, degradation read
        // 0 by the native (0x1164FD0 owner gate) and 0.34 raw.
        const wl::Wear npcNative{0.346527f, 0.346527f, 0.346527f, 0.0f, 0.0f, 0.0f};
        const wl::Wear npcRaw{0.346527f, 0.346527f, 0.346527f, 0.346527f, 0.0f, 0.0f};
        Check(wl::WearMoved(npcNative, npcRaw), "wear: the gated degradation is a different look");
        Check(wl::WearText(npcRaw) == "0.347/0.347/0.347/0.347/0.000/0.000", "wear text");
    }

    void TestStateAt()
    {
        std::vector<std::pair<std::uint32_t, int>> states;
        Check(wl::StateAt(states, 10) == -1, "state at: empty track");
        states = {{100, 0}, {500, 1}, {900, 2}};
        Check(wl::StateAt(states, 0) == 0, "state at: before the first, the first");
        Check(wl::StateAt(states, 500) == 1 && wl::StateAt(states, 899) == 1, "state at: the one in force");
        Check(wl::StateAt(states, 5000) == 2, "state at: after the last, the last");
        Check(wl::StateAt(states, 100) == 0, "state at: seek back to the first");
    }

    void TestHashList()
    {
        Check(wl::HashList({}) == "none", "hash list: none");
        Check(wl::HashList({0x1u, 0xabcdef01u}) == "00000001;abcdef01", "hash list: two");
        std::vector<std::uint32_t> many(15, 7u);
        Check(wl::HashList(many, 12).find(";+3") != std::string::npos, "hash list: the rest counted");
    }

    void TestFlags()
    {
        replay::fxcheck::Tally tally;
        tally.Reset();
        tally.NoteWeaponLook(12, 0, 0);
        Check(tally.Flags().empty(), "fx check: looks that agree leave no finding");
        tally.NoteWeaponLook(12, 2, 1);
        bool components = false, wear = false;
        for (const replay::fxcheck::Flag& flag : tally.Flags())
        {
            components = components || (flag.kind == "weapon_components_mismatch" && flag.value == 2.0);
            wear = wear || (flag.kind == "weapon_wear_mismatch" && flag.value == 1.0);
        }
        Check(components && wear, "fx check: weapon_components_mismatch and weapon_wear_mismatch");
        tally.ResetPlay();
        Check(tally.Flags().empty(), "fx check: look findings reset with the play");
    }
}

int RunWeaponLookTests()
{
    TestNormalize();
    TestDiff();
    TestWear();
    TestStateAt();
    TestHashList();
    TestFlags();
    if (g_weaponLookFailures == 0)
        std::printf("weapon look tests: ok\n");
    return g_weaponLookFailures;
}
