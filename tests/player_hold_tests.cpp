// The recorded player's puppet is never hidden (src/player_hold.h, QA 224):
// which created entity is the player, and the visibility verdict of a tick
// for him and for any other body. And the capture's track budget against the
// recorder's own budgets (src/pose_capture_rules.h kCaptureTrackByteBudget).
#include "buffer_coverage.h"
#include "player_hold.h"
#include "pose_capture_rules.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            ++g_failures;
            std::printf("FAIL player hold: %s\n", message);
        }
    }

    using replay::playerhold::Candidate;
    using replay::playerhold::Decide;
    using replay::playerhold::Gate;
    using replay::playerhold::PickPlayer;
    using replay::playerhold::Verdict;

    constexpr std::uint32_t kArthur = 0x0D7114C9u; // player_zero, QA 224's PLAC
    constexpr std::uint32_t kHorse = 0x28F9976Au;
    constexpr std::uint32_t kOther = 0x881F1C91u;

    std::uint32_t Packed(std::uint16_t slot, std::uint16_t instance)
    {
        return (static_cast<std::uint32_t>(slot) << 16) | instance;
    }

    void TestPickPlayer()
    {
        // QA 224: the horse 0:1, Arthur 1:2, both created at 1 ms.
        std::vector<Candidate> created = {
            {Packed(0, 1), false, kHorse, 1},
            {Packed(1, 2), true, kArthur, 1},
            {Packed(5, 69), true, kOther, 5581},
        };
        Check(PickPlayer(created, kArthur) == Packed(1, 2), "pick: QA 224's Arthur is 1:2");
        // A cutscene's copy of Arthur created later is not the player.
        created.push_back({Packed(9, 300), true, kArthur, 40000});
        Check(PickPlayer(created, kArthur) == Packed(1, 2), "pick: a later copy of the model is not the player");
        // Created first anywhere in the list wins; the lower id on a tie.
        std::vector<Candidate> tie = {{Packed(3, 9), true, kArthur, 1}, {Packed(1, 2), true, kArthur, 1}};
        Check(PickPlayer(tie, kArthur) == Packed(1, 2), "pick: the lower id on a tie");
        std::vector<Candidate> later = {{Packed(3, 9), true, kArthur, 500}, {Packed(4, 4), true, kArthur, 20}};
        Check(PickPlayer(later, kArthur) == Packed(4, 4), "pick: the earliest created");
        // No place model: no guess. A horse of the model never counts.
        Check(PickPlayer(created, 0) == 0, "pick: no place model, no player");
        std::vector<Candidate> horseOnly = {{Packed(0, 1), false, kArthur, 1}};
        Check(PickPlayer(horseOnly, kArthur) == 0, "pick: only peds");
        Check(PickPlayer({}, kArthur) == 0, "pick: empty clip");
    }

    Gate Body(bool player)
    {
        Gate gate;
        gate.player = player;
        gate.sampled = true;
        return gate;
    }

    void TestOtherBodiesUnchanged()
    {
        // Every combination of the old rule (sampled && inTrack && !unposed &&
        // recordedVisible && !neverAnimated) gives the same verdict for a body
        // that is not the player.
        for (int bits = 0; bits < 64; ++bits)
        {
            Gate gate;
            gate.player = false;
            gate.sampled = (bits & 1) != 0;
            gate.beforeFirst = (bits & 2) != 0;
            gate.afterLast = (bits & 4) != 0;
            gate.unposed = (bits & 8) != 0;
            gate.recordedVisible = (bits & 16) != 0;
            gate.neverAnimated = (bits & 32) != 0;
            const bool inTrack = !gate.beforeFirst && !gate.afterLast;
            const bool old = gate.sampled && inTrack && !gate.unposed && gate.recordedVisible && !gate.neverAnimated;
            const Verdict verdict = Decide(gate);
            Check(verdict.visible == old && verdict.inTrack == inTrack && !verdict.held && !verdict.keptVisible,
                "other bodies: the rule as before");
        }
    }

    void TestPlayerNeverHidden()
    {
        // QA 224 at 0 to 68 s: before his record's first frame. Held, drawn.
        Gate start = Body(true);
        start.beforeFirst = true;
        Verdict verdict = Decide(start);
        Check(verdict.visible && verdict.held && verdict.inTrack && verdict.keptVisible,
            "player: before his first frame he holds it, drawn");
        // The same tick for any other body hides it (the old behaviour).
        start.player = false;
        Check(!Decide(start).visible, "player: another body before its first frame stays hidden");

        // After his last frame with no later record: holds the last.
        Gate end = Body(true);
        end.afterLast = true;
        verdict = Decide(end);
        Check(verdict.visible && verdict.held, "player: after his last frame he holds it");

        // Inside his span: drawn, nothing held.
        verdict = Decide(Body(true));
        Check(verdict.visible && !verdict.held && !verdict.keptVisible, "player: inside his record, drawn as recorded");

        // Recorded hidden (PVIS) or never animated: drawn anyway.
        Gate pvis = Body(true);
        pvis.recordedVisible = false;
        verdict = Decide(pvis);
        Check(verdict.visible && verdict.keptVisible && !verdict.held, "player: drawn against the recorded visibility");
        Gate never = Body(true);
        never.neverAnimated = true;
        Check(Decide(never).visible, "player: drawn against the never animated hide");

        // Nothing to hold: no sample at all (no record), or a skeleton the
        // override cannot write yet (the T-pose): the only hidden cases.
        Gate none = Body(true);
        none.sampled = false;
        none.beforeFirst = true;
        verdict = Decide(none);
        Check(!verdict.visible && !verdict.held, "player: no pose to hold, not drawn");
        Gate unposed = Body(true);
        unposed.unposed = true;
        Check(!Decide(unposed).visible, "player: unposed (T-pose) is not drawn");
    }

    using replay::pose::Quat;
    using replay::pose::Vec3;
    using replay::pose::WorldTransform;

    Quat Yaw(float degrees)
    {
        const float half = degrees * 3.14159265f / 360.0f;
        return {0.0f, 0.0f, std::sin(half), std::cos(half)};
    }

    WorldTransform At(float x, float y, float z, float yawDegrees)
    {
        WorldTransform world;
        world.position = {x, y, z};
        world.rotation = Yaw(yawDegrees);
        return world;
    }

    bool Near(const Vec3& a, float x, float y, float z)
    {
        return std::fabs(a.x - x) < 1e-3f && std::fabs(a.y - y) < 1e-3f && std::fabs(a.z - z) < 1e-3f;
    }

    bool SameRotation(const Quat& a, const Quat& b)
    {
        return std::fabs(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1.0f) < 1e-4f;
    }

    void TestCarry()
    {
        using replay::playerhold::Carry;
        // QA 224's frame: the rider 0.1 m behind and 1.0 m above the horse's
        // origin at his first frame (horse facing +Y, yaw 0).
        const WorldTransform horseFirst = At(963.3f, -626.0f, 76.7f, 0.0f);
        const WorldTransform riderFirst = At(963.3f, -626.1f, 77.7f, 10.0f);
        // The same horse transform: the rider exactly as recorded.
        WorldTransform carried = Carry(riderFirst, horseFirst, horseFirst);
        Check(Near(carried.position, 963.3f, -626.1f, 77.7f) && SameRotation(carried.rotation, riderFirst.rotation),
            "carry: at the first frame's mount transform, the recorded rider");
        // The horse 735 m away at 0 s: the rider rides it with his offset.
        carried = Carry(riderFirst, horseFirst, At(1695.73f, -427.23f, 48.82f, 0.0f));
        Check(Near(carried.position, 1695.73f, -427.33f, 49.82f), "carry: translation follows the mount");
        // The horse turned 90 degrees left (counter-clockwise): the offset 0.1
        // m behind (-Y) turns to +X, the rider's heading turns with it.
        carried = Carry(riderFirst, horseFirst, At(0.0f, 0.0f, 0.0f, 90.0f));
        Check(Near(carried.position, 0.1f, 0.0f, 1.0f), "carry: the offset turns with the mount");
        Check(SameRotation(carried.rotation, Yaw(100.0f)), "carry: the rider's heading turns with the mount");
        // From a turned first frame back to yaw 0.
        const WorldTransform horseTurned = At(10.0f, 10.0f, 0.0f, 90.0f);
        const WorldTransform riderTurned = At(10.1f, 10.0f, 1.0f, 95.0f);
        carried = Carry(riderTurned, horseTurned, At(0.0f, 0.0f, 0.0f, 0.0f));
        Check(Near(carried.position, 0.0f, -0.1f, 1.0f) && SameRotation(carried.rotation, Yaw(5.0f)),
            "carry: the first frame's turn taken out");
    }

    void TestCaptureTrackBudget()
    {
        constexpr std::size_t MB = 1024 * 1024;
        // The recorder's budgets bound the tracks before their own does: a
        // manual part closes at kManualMaxMB, the ring's largest budget is
        // 4096 MB (script.cpp ReplayBufferMaxMB clamp).
        Check(replay::pose::capture::kCaptureTrackByteBudget >= static_cast<std::size_t>(replay::buffercov::kManualMaxMB) * MB,
            "budget: a track never trims itself inside a manual part");
        Check(replay::pose::capture::kCaptureTrackByteBudget >= std::size_t{4096} * MB,
            "budget: nor inside the buffer's largest ring");
        // QA 224's player record: 33357800 bytes kept of 257.6 s, 34 chunks lost.
        Check(replay::pose::capture::kCaptureTrackByteBudget > std::size_t{33357800} * 2,
            "budget: QA 224's whole player record fits");
    }
}

int RunPlayerHoldTests()
{
    TestPickPlayer();
    TestOtherBodiesUnchanged();
    TestPlayerNeverHidden();
    TestCarry();
    TestCaptureTrackBudget();
    if (g_failures == 0)
        std::puts("Player hold tests passed.");
    return g_failures;
}
