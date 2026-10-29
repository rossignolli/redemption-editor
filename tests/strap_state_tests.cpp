// The strap state played on a puppet weapon (strap_state_core.h): the rebuild
// request only where the engine made it, the hold and cloth bits as recorded.
// The flags are the ones of clip 232-Valentine (a QA report).
#include "strap_state_core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
    int g_strapStateFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_strapStateFailures;
    }

    namespace st = replay::strap;

    // The rifle 51:1574 and the shotgun 48:270 of clip 232-Valentine, and a
    // few weapons of its other peds.
    constexpr std::uint32_t kRifleOnBack = 0x238084;     // 0 to 8.2 s, 23.0 s on
    constexpr std::uint32_t kRifleAiming = 0x233084;     // 8.209 to 23.045 s
    constexpr std::uint32_t kShotgunOnBack = 0x18084;
    constexpr std::uint32_t kShotgunInHand = 0x13084;    // 43.689 s
    constexpr std::uint32_t kShotgunLater = 0x218084;    // 67.5 s
    constexpr std::uint32_t kNpcCreated = 0x10004;       // a ped's weapon at its first sample
    constexpr std::uint32_t kNpcInHand = 0x12004;
    constexpr std::uint32_t kNpcDrawing = 0x133004;      // 0x1000 while drawing from the back
    constexpr std::uint32_t kNpcDrawnRequest = 0x13a004; // in a hand, the manager still rebuilding

    void TestBits()
    {
        Check(st::kFlagTaskHold == 0x1800, "the manager tests 0x1800");
        Check(st::kReplayedBits == 0x21800, "replayed: the hold bits and the cloth state");
        Check((st::kReplayedBits & st::kFlagHeld) == 0, "the in-hand bit is never written on a puppet");
        Check((st::kReplayedBits & st::kFlagRebuild) == 0, "the rebuild is chosen, not copied");
        Check((kRifleAiming ^ kRifleOnBack) == (st::kFlagTaskHoldB | st::kFlagHeld | st::kFlagRebuild),
            "aiming sets 0x1000 and 0x2000 and clears 0x8000");
    }

    void TestRebuild()
    {
        // As before: every state recorded with the request.
        Check(st::RebuildWanted(kRifleOnBack), "rifle on the back: shoulder sling rebuilt");
        Check(st::RebuildWanted(kShotgunOnBack), "shotgun holstered: rebuilt");
        Check(st::RebuildWanted(kShotgunLater), "shotgun back on the back: rebuilt");
        Check(st::RebuildWanted(kNpcDrawnRequest), "in a hand with the request: rebuilt");
        // As before: no request and no sign of a hand.
        Check(st::RebuildWanted(kNpcCreated), "a ped's weapon at creation: rebuilt as before");
        Check(st::RebuildWanted(0), "an empty word: rebuilt as before");
        // The fix: in a hand without the request.
        Check(!st::RebuildWanted(kRifleAiming), "rifle aiming: no rebuild (the cloth sling)");
        Check(!st::RebuildWanted(kShotgunInHand), "shotgun in hand: no rebuild");
        Check(!st::RebuildWanted(kNpcInHand), "a ped's long arm in a hand: no rebuild");
        Check(!st::RebuildWanted(kNpcDrawing), "the weapon task holding the strap: no rebuild");
        Check(!st::RebuildWanted(st::kFlagTaskHoldA), "0x800 alone holds it too");
    }

    void TestPlaybackFlags()
    {
        // The puppet's own word at its first frames (POSE_V2_WEAPON_ATTACH_CHECK
        // of the report: 0x30004 with the cloth state, bit 15 consumed).
        constexpr std::uint32_t kPuppet = 0x30004;
        Check(st::PlaybackFlags(kPuppet, kRifleOnBack) == 0x38004, "on the back: cloth state kept, rebuild asked");
        Check(st::PlaybackFlags(0x10004, kRifleOnBack) == 0x38004, "the old write, bit for bit");
        Check(st::PlaybackFlags(kPuppet, kRifleAiming) == 0x31004, "aiming: 0x1000 copied, no rebuild");
        Check(st::PlaybackFlags(0x38004, kRifleAiming) == 0x31004, "a pending request is withdrawn");
        Check(st::PlaybackFlags(0x31004, kRifleOnBack) == 0x38004, "back on the back: hold bit cleared, rebuild");
        Check(st::PlaybackFlags(0x30004, kShotgunInHand) == 0x11004, "shotgun in hand: cloth state as recorded (0)");
        // The puppet's own bits are never touched.
        constexpr std::uint32_t kOwn = 0x80000000u | 0x400000u | 0x200000u | 0x2000u | 0x80u | 0x4u;
        Check((st::PlaybackFlags(kOwn, kRifleAiming) & kOwn) == kOwn, "own bits kept (no rebuild)");
        Check((st::PlaybackFlags(kOwn, kRifleOnBack) & kOwn) == kOwn, "own bits kept (rebuild)");
        Check((st::PlaybackFlags(0, kRifleAiming) & st::kFlagHeld) == 0, "the in-hand bit is not written");
        Check((st::PlaybackFlags(0, 0x200080u) & 0x200080u) == 0, "0x80 and 0x200000 are not replayed");
    }

    void TestHolder()
    {
        Check(std::strcmp(st::HolderName(kRifleOnBack), "ped") == 0, "on the back: the ped");
        Check(std::strcmp(st::HolderName(kRifleAiming), "task") == 0, "aiming: the task");
        Check(std::strcmp(st::HolderName(kNpcInHand), "hand") == 0, "in a hand: hand");
        Check(std::strcmp(st::HolderName(kNpcCreated), "none") == 0, "nothing: none");
    }

    void TestIndex()
    {
        std::vector<std::pair<std::uint32_t, int>> states;
        Check(st::StateIndexAt(states, 0) == -1, "no states: none");
        // The rifle's track: 0, 7490, 7527, 7562, 7601, 8209, 23045.
        for (std::uint32_t ms : {0u, 7490u, 7527u, 7562u, 7601u, 8209u, 23045u})
            states.push_back({ms, 0});
        Check(st::StateIndexAt(states, 0) == 0, "at 0: the first");
        Check(st::StateIndexAt(states, 8208) == 4, "just before the aim: on the back");
        Check(st::StateIndexAt(states, 8209) == 5, "at 8209: aiming");
        Check(st::StateIndexAt(states, 15000) == 5, "mid aim");
        Check(st::StateIndexAt(states, 23045) == 6, "at 23045: back on the back");
        Check(st::StateIndexAt(states, 99999) == 6, "past the end: the last");
        std::vector<std::pair<std::uint32_t, int>> late{{28023u, 0}, {30000u, 1}};
        Check(st::StateIndexAt(late, 100) == 0, "before the first change: the first state");
    }
}

int RunStrapStateTests()
{
    TestBits();
    TestRebuild();
    TestPlaybackFlags();
    TestHolder();
    TestIndex();
    if (g_strapStateFailures == 0)
        std::printf("strap state tests: ok\n");
    return g_strapStateFailures;
}
