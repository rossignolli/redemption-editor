// The still rule (still_pose_core.h): its decision, the drawn comparison (same,
// bind, other, torn reads, sign of the quaternion) and the per-puppet totals and
// verdicts of a pelt that flickers, one the engine replaced, and a clean one.
#include "still_pose_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::stillpose;
    using replay::pose::BoneQts;
    using replay::pose::Quat;

    int g_stillFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_stillFailures;
    }

    Quat AboutZ(float rad)
    {
        Quat q;
        q.z = std::sin(rad * 0.5f);
        q.w = std::cos(rad * 0.5f);
        return q;
    }

    std::vector<BoneQts> Pose(std::uint32_t bones, float rad)
    {
        std::vector<BoneQts> pose(bones);
        for (std::uint32_t bone = 0; bone < bones; ++bone)
        {
            pose[bone].rotation = AboutZ(rad * static_cast<float>(bone));
            pose[bone].translation[0] = 0.1f * static_cast<float>(bone);
        }
        return pose;
    }

    void TestDecision()
    {
        Check(Decide(true, true, true, true) == Decision::SkipStill, "decide: attached still record is left to the engine");
        Check(Decide(true, true, true, false) == Decision::Arm, "decide: an animated record is armed");
        Check(Decide(false, true, true, true) == Decision::ArmRuleOff, "decide: rule off arms as before");
        Check(Decide(true, false, true, true) == Decision::ArmFree, "decide: a free object is armed");
        Check(Decide(true, true, false, true) == Decision::ArmUnread, "decide: unread track arms as before");
        Check(std::string(DecisionName(Decision::SkipStill)) == "still_engine_posed", "decide: the line's name");
    }

    void TestCompare()
    {
        const std::uint32_t bones = 8;
        const std::vector<BoneQts> folded = Pose(bones, 0.2f);
        std::vector<Quat> bind(bones);
        for (std::uint32_t bone = 0; bone < bones; ++bone)
            bind[bone] = AboutZ(0.0f);
        const std::vector<BoneQts> flat = Pose(bones, 0.0f);

        Comparison same = Compare(folded.data(), folded.data(), bind.data(), bones);
        Check(same.shape == Shape::Same && same.changed == 0, "compare: identical pose is same");

        std::vector<BoneQts> negated = folded;
        for (BoneQts& b : negated)
        {
            b.rotation.x = -b.rotation.x;
            b.rotation.y = -b.rotation.y;
            b.rotation.z = -b.rotation.z;
            b.rotation.w = -b.rotation.w;
        }
        Check(Compare(folded.data(), negated.data(), bind.data(), bones).shape == Shape::Same,
            "compare: q and -q are the same rotation");

        std::vector<BoneQts> jitter = folded;
        jitter[3].translation[1] += 0.002f;
        Check(Compare(folded.data(), jitter.data(), bind.data(), bones).shape == Shape::Same,
            "compare: 2 mm is within tolerance");

        Comparison toBind = Compare(folded.data(), flat.data(), bind.data(), bones);
        Check(toBind.shape == Shape::Bind, "compare: the engine's bind pose over ours is bind");
        Check(toBind.changed == bones - 1 && toBind.nearBind == toBind.changed, "compare: bone 0 matches, the rest bind");
        Check(toBind.worstBone == static_cast<std::int32_t>(bones - 1), "compare: worst bone is the most turned");

        Comparison noBind = Compare(folded.data(), flat.data(), nullptr, bones);
        Check(noBind.shape == Shape::Other, "compare: without a bind a change is other");

        const std::vector<BoneQts> sagged = Pose(bones, 0.1f);
        Check(Compare(folded.data(), sagged.data(), bind.data(), bones).shape == Shape::Other,
            "compare: the engine's own pose is other");

        // Locals then objects: locals as expected, objects recomposed.
        std::vector<BoneQts> full = folded;
        full.insert(full.end(), folded.begin(), folded.end());
        std::vector<BoneQts> recomposed = full;
        recomposed[bones + 5].translation[2] += 0.05f;
        FullComparison fo = CompareFull(full.data(), recomposed.data(), bind.data(), bones, true);
        Check(fo.shape == Shape::Objects && fo.objects.changed == 1 && fo.locals.changed == 0,
            "full: object matrices alone differ");
        Check(CompareFull(full.data(), recomposed.data(), bind.data(), bones, false).shape == Shape::Same,
            "full: objects not compared without them");
        std::vector<BoneQts> bothFlat = flat;
        bothFlat.insert(bothFlat.end(), flat.begin(), flat.end());
        Check(CompareFull(full.data(), bothFlat.data(), bind.data(), bones, true).shape == Shape::Bind,
            "full: the locals decide first");
        Totals withObjects;
        withObjects.Add(fo);
        Check(withObjects.objects == 1 && withObjects.Changed() == 1, "full: totals count objects");

        std::vector<BoneQts> torn = folded;
        torn[2].rotation = Quat{0.0f, 0.0f, 0.0f, 0.0f};
        Comparison t = Compare(folded.data(), torn.data(), bind.data(), bones);
        Check(t.shape == Shape::Other && t.changed == 1 && t.worstBone == 2, "compare: an unusable rotation is a change");
    }

    void TestTotals()
    {
        Comparison same;
        Comparison other;
        other.shape = Shape::Other;
        other.changed = 7;
        other.maxRad = 0.4f;

        Totals flicker;
        bool anyFlip = false;
        for (int tick = 0; tick < 300; ++tick)
            anyFlip = flicker.Add(tick % 3 == 0 ? same : other) || anyFlip;
        Check(anyFlip && flicker.flips > 100, "totals: alternating ticks flip");
        Check(std::string(Verdict(flicker)) == "flicker", "totals: alternating is flicker");
        Check(flicker.longestRun == 2, "totals: longest changed run");

        Totals replaced;
        Check(!replaced.Add(other), "totals: the first tick never flips");
        for (int tick = 0; tick < 299; ++tick)
            replaced.Add(other);
        Check(std::string(Verdict(replaced)) == "replaced" && replaced.flips == 0, "totals: steady engine pose is replaced");

        Totals clean;
        for (int tick = 0; tick < 50; ++tick)
            clean.Add(same);
        Check(std::string(Verdict(clean)) == "clean", "totals: clean");
        Check(std::string(Verdict(Totals{})) == "none", "totals: unchecked");
        const std::string text = FormatTotals(flicker);
        Check(text.find("verdict=flicker") != std::string::npos && text.find("checks=300") != std::string::npos,
            "totals: the summary text");

        Check(ShouldLogCheck(true, false, 0), "log: first check");
        Check(!ShouldLogCheck(false, false, 0), "log: steady ticks are not logged");
        Check(!ShouldLogCheck(false, true, kDrawnLineCap), "log: cap");
    }
}

int RunStillPoseTests()
{
    g_stillFailures = 0;
    TestDecision();
    TestCompare();
    TestTotals();
    if (!g_stillFailures)
        std::printf("Still pose tests passed.\n");
    return g_stillFailures;
}
