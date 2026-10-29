// The pure parts of the micro jitter fix. The end-of-frame placement plan of every puppet
// (mount_placement.h PlanPlace, the totals line), the stale pose rule, the
// plants-only physics filter (puppet_collision.h) and the root
// interpolation between samples the playback already does
// (pose::InterpolateWorld). Numbers from a recorded horse the engine
// pushed 0.12 m every 1017 ms.

#include "mount_placement.h"
#include "pose_math.h"
#include "puppet_collision.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    using replay::pose::Quat;
    using replay::pose::Vec3;
    using replay::pose::WorldTransform;

    int g_jitterFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_jitterFailures;
    }

    Quat Yaw(float degrees)
    {
        const float half = degrees * 3.14159265f / 360.0f;
        return {0.0f, 0.0f, std::sin(half), std::cos(half)};
    }

    WorldTransform At(float x, float y, float z, float yawDegrees)
    {
        WorldTransform w;
        w.position = {x, y, z};
        w.rotation = Yaw(yawDegrees);
        return w;
    }

    // The engine's layout (0x68C654): cells truncated toward zero, the
    // remainder in float, z whole, the cells in the position row's w lane.
    void EngineEncode(const WorldTransform& w, float m[16])
    {
        Vec3 right;
        Vec3 forward;
        Vec3 up;
        replay::pose::BasisFromQuat(replay::pose::Normalize(w.rotation), right, forward, up);
        const auto cellOf = [](float value) { return static_cast<std::int16_t>(static_cast<int>(value * 0.03125f)); };
        const std::int16_t cx = cellOf(w.position.x);
        const std::int16_t cy = cellOf(w.position.y);
        m[0] = right.x;   m[1] = right.y;   m[2] = right.z;   m[3] = 0.75f;
        m[4] = forward.x; m[5] = forward.y; m[6] = forward.z; m[7] = 0.0f;
        m[8] = up.x;      m[9] = up.y;      m[10] = up.z;     m[11] = 0.0f;
        m[12] = w.position.x - static_cast<float>(cx) * 32.0f;
        m[13] = w.position.y - static_cast<float>(cy) * 32.0f;
        m[14] = w.position.z;
        const std::uint32_t cells = static_cast<std::uint16_t>(cx) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(cy)) << 16);
        std::memcpy(&m[15], &cells, sizeof(cells));
    }

    double Distance(const replay::mount::EngineFrame& frame, const Vec3& p)
    {
        double at[3]{};
        replay::mount::AbsolutePosition(frame, at);
        return std::sqrt((at[0] - p.x) * (at[0] - p.x) + (at[1] - p.y) * (at[1] - p.y) + (at[2] - p.z) * (at[2] - p.z));
    }

    // A horse placed, then moved 12 cm (and turned a little) by the engine
    // after the placement. The pre-render writes
    // it back, in its own cells, every w lane kept.
    void TestPlaceBackPushedHorse()
    {
        using namespace replay::mount;
        const WorldTransform placed = At(-310.25f, 790.5f, 117.75f, 40.0f);
        const WorldTransform pushed = At(-310.25f + 0.09f, 790.5f - 0.08f, 117.75f + 0.01f, 41.0f);
        float m[16]{};
        EngineEncode(pushed, m);
        EngineFrame now;
        Check(DecodeEngineMatrix(m, now), "the pushed horse decodes");
        const PlacePlan plan = PlanPlace(now, placed, true);
        Check(std::fabs(plan.movedM - 0.1208) < 0.002, "moved_m is the push (about 12 cm)");
        Check(plan.turnedRad > 0.015f && plan.turnedRad < 0.02f, "turned by the push's degree");
        Check(plan.write && !plan.refused, "a push is written back");
        float out[16]{};
        std::memcpy(out, m, sizeof(out));
        EncodeKeepingCells(plan.right, plan.forward, plan.up, plan.position, now.cellX, now.cellY, out);
        EngineFrame back;
        Check(DecodeEngineMatrix(out, back), "the written matrix decodes");
        Check(Distance(back, placed.position) < 1e-3, "written back to the placement");
        Check(replay::pose::AngleBetween(FrameRotation(back), placed.rotation) < 1e-3f, "and to its rotation");
        Check(out[3] == 0.75f && std::memcmp(&out[15], &m[15], 4) == 0, "the w lanes and the cells are kept");
    }

    void TestPlaceAcrossCell()
    {
        using namespace replay::mount;
        // Placed just east of a cell edge, pushed just west of it: the
        // engine matrix holds the west cell, the target is written relative
        // to that cell (x remainder over 32).
        const WorldTransform placed = At(64.02f, -0.5f, 10.0f, 0.0f);
        const WorldTransform pushed = At(63.97f, -0.5f, 10.0f, 0.0f);
        float m[16]{};
        EngineEncode(pushed, m);
        EngineFrame now;
        Check(DecodeEngineMatrix(m, now) && now.cellX == 1, "the pushed matrix is in cell 1");
        const PlacePlan plan = PlanPlace(now, placed, true);
        Check(plan.write && std::fabs(plan.position.x - 32.02f) < 1e-3f, "the target in cell 1's frame (beyond 32)");
        float out[16]{};
        std::memcpy(out, m, sizeof(out));
        EncodeKeepingCells(plan.right, plan.forward, plan.up, plan.position, now.cellX, now.cellY, out);
        EngineFrame back;
        Check(DecodeEngineMatrix(out, back) && Distance(back, placed.position) < 1e-3, "lands on the placement");
        // Negative side: cells truncate toward zero.
        const WorldTransform placedWest = At(-31.99f, -64.5f, 3.0f, 0.0f);
        EngineEncode(At(-32.01f, -64.5f, 3.0f, 0.0f), m);
        Check(DecodeEngineMatrix(m, now) && now.cellX == -1 && now.cellY == -2, "negative cells truncated toward zero");
        const PlacePlan west = PlanPlace(now, placedWest, true);
        Check(west.write && std::fabs(west.position.x - 0.01f) < 1e-3f, "the target in cell -1's frame");
    }

    void TestPlaceThresholds()
    {
        using namespace replay::mount;
        const WorldTransform placed = At(100.0f, 200.0f, 50.0f, 10.0f);
        float m[16]{};
        EngineFrame now;
        EngineEncode(placed, m);
        DecodeEngineMatrix(m, now);
        PlacePlan plan = PlanPlace(now, placed, true);
        Check(!plan.write && plan.movedM < 1e-3, "not moved: nothing written");
        EngineEncode(At(100.001f, 200.0f, 50.0f, 10.0f), m);
        DecodeEngineMatrix(m, now);
        plan = PlanPlace(now, placed, true);
        Check(!plan.write, "1 mm: under the write threshold");
        EngineEncode(At(100.0f, 200.0f, 50.0f, 10.5f), m);
        DecodeEngineMatrix(m, now);
        plan = PlanPlace(now, placed, true);
        Check(plan.write, "half a degree turned: written");
        EngineEncode(At(100.04f, 200.0f, 50.0f, 10.0f), m);
        DecodeEngineMatrix(m, now);
        plan = PlanPlace(now, placed, false);
        Check(!plan.write && !plan.refused && plan.movedM > 0.039, "disabled: measured only");
        EngineEncode(At(140.0f, 200.0f, 50.0f, 10.0f), m);
        DecodeEngineMatrix(m, now);
        plan = PlanPlace(now, placed, true);
        Check(plan.refused && !plan.write, "40 m off is a wrong entry: refused");
    }

    void TestStalePose()
    {
        using replay::mount::PoseStale;
        Check(PoseStale(10, 10), "no finalizer write since the publish: stale");
        Check(!PoseStale(10, 11), "the finalizer wrote after the publish: not stale");
        static_assert(replay::mount::PoseStale(3, 3), "constexpr");
    }

    void TestPuppetTotals()
    {
        using namespace replay::mount;
        PuppetTotals totals;
        totals.NoteTick(3);
        std::string line;
        PuppetRecord missed;
        missed.packed = (2u << 16) | 2572u;
        totals.Add(missed, {}, line);
        Check(line.empty(), "a puppet without a pre-render is only counted as missed");
        PuppetRecord pushed;
        pushed.packed = (2u << 16) | 2572u;
        pushed.calls = 1;
        pushed.playheadMs = 2398;
        pushed.plan.movedM = 0.12;
        pushed.plan.write = true;
        pushed.wrote = true;
        totals.Add(pushed, {}, line);
        Check(line.find("POSE_V2_PRERENDER,phase=placed_back,role=puppet,id=2:2572,playhead_ms=2398,moved_m=0.120") == 0,
            "a 12 cm push gets an evidence line");
        PuppetRecord quiet;
        quiet.packed = (3u << 16) | 2573u;
        quiet.role = PuppetRole::Rider;
        quiet.calls = 1;
        quiet.plan.movedM = 0.004;
        quiet.staleWrote = true;
        totals.Add(quiet, {}, line);
        Check(line.empty(), "4 mm is no evidence line");
        PuppetRecord wagon;
        wagon.packed = (4u << 16) | 9u;
        wagon.role = PuppetRole::Vehicle;
        wagon.calls = 2;
        wagon.plan.movedM = 0.02;
        wagon.plan.write = true;
        wagon.wrote = true;
        totals.Add(wagon, {}, line);
        const std::string summary = totals.Summary(true, true);
        Check(summary.find("POSE_V2_PRERENDER_SUMMARY,frame_end=1,physics_filter=1,ticks=1,entries=3,calls=3,missed=1,"
                           "multi_calls=1,moved_1cm=2,moved_5cm=1,moved_max_m=0.120,placed_back=2,") == 0,
            "summary counts");
        Check(summary.find("stale_pose_writes=1,") != std::string::npos, "summary: stale pose writes");
        Check(summary.find("vehicle_calls=1,vehicle_moved_1cm=1,vehicle_writes=1,") != std::string::npos,
            "summary: the vehicle columns");
    }

    void TestCollisionFilter()
    {
        using namespace replay::collision;
        const std::uint64_t inst = 0x2A0001230ull;
        // A ped capsule's include as the engine might hold it: map 2, peds
        // 9, horses 11, objects 13, foliage 19, vehicles 6, 7, 25, query 24.
        const std::uint64_t include = 0x4 | 0x200 | 0x800 | 0x2000 | 0x80000 | 0xC0 | 0x2000000 | 0x1000000;
        State state;
        Plan plan = PlanRestrict(state, inst, kTypePedCapsule, include, kDefaultKeep);
        Check(plan.step == Step::Write, "first active tick: write");
        Check(plan.value == (0x80000ull | 0x1000000ull), "only foliage and the script query class are kept");
        Check((plan.value & ~include) == 0, "never a bit the instance did not have");
        Check((plan.value & (0x4 | 0x8 | 0x200 | 0x800 | 0x2000 | 0xC0 | 0x2000000)) == 0,
            "no map, ped, horse, object or vehicle class: no pair with any of them");
        Check(plan.original == include, "the original kept");
        state = {inst, plan.original, plan.value};
        plan = PlanRestrict(state, inst, kTypePedCapsule, state.written, kDefaultKeep);
        Check(plan.step == Step::None, "still restricted: nothing to do");
        plan = PlanRestrict(state, inst, kTypePedCapsule, include, kDefaultKeep);
        Check(plan.step == Step::Rewrite && plan.value == state.written, "the engine put it back: restricted again");
        plan = PlanRestrict(state, inst + 0x120, kTypeHorseCapsule, include | 0x8, kDefaultKeep);
        Check(plan.step == Step::NewInstance && plan.original == (include | 0x8), "a reinserted instance: the new one");
        plan = PlanRestrict(state, inst, 0x400, include, kDefaultKeep);
        Check(plan.step == Step::Refuse, "a ragdoll instance (type bit 10) is never written");
        plan = PlanRestrict(state, 0, kTypePedCapsule, include, kDefaultKeep);
        Check(plan.step == Step::Refuse, "no instance: refused");
        // Restore.
        plan = PlanRestore(state, inst, state.written);
        Check(plan.step == Step::Restore && plan.value == include, "frozen again: the original back");
        plan = PlanRestore(state, inst, include);
        Check(plan.step == Step::Lost, "someone else's value: left alone");
        plan = PlanRestore(state, inst + 0x120, state.written);
        Check(plan.step == Step::Lost, "another instance: left alone");
        plan = PlanRestore(State{}, inst, include);
        Check(plan.step == Step::None, "never written: nothing");
        Check(CapsuleType(0x200) && CapsuleType(0x800) && !CapsuleType(0xA00), "capsule types (0xFF59EB)");
        Check(std::string(StepText(Step::NewInstance)) == "new_instance", "step names");
    }

    // (3) The playback already blends the root between the two samples
    // around the playhead (slerp and lerp): checked here so a
    // change that drops it shows.
    void TestRootInterpolation()
    {
        const WorldTransform a = At(10.0f, 0.0f, 1.0f, 0.0f);
        const WorldTransform b = At(10.5f, 0.0f, 1.0f, 20.0f);
        float previous = a.position.x;
        float previousYaw = 0.0f;
        bool monotonic = true;
        for (int step = 1; step <= 10; ++step)
        {
            const WorldTransform w = replay::pose::InterpolateWorld(a, b, static_cast<float>(step) / 10.0f);
            monotonic = monotonic && w.position.x > previous;
            const float yaw = replay::pose::AngleBetween(w.rotation, a.rotation);
            monotonic = monotonic && yaw >= previousYaw;
            previous = w.position.x;
            previousYaw = yaw;
        }
        Check(monotonic, "between two samples the root moves and turns steadily");
        const WorldTransform half = replay::pose::InterpolateWorld(a, b, 0.5f);
        Check(std::fabs(half.position.x - 10.25f) < 1e-5f, "half way: half the distance");
        Check(std::fabs(replay::pose::AngleBetween(half.rotation, a.rotation) - 10.0f * 3.14159265f / 180.0f) < 1e-3f,
            "half way: half the turn");
    }
}

int RunJitterTests()
{
    TestPlaceBackPushedHorse();
    TestPlaceAcrossCell();
    TestPlaceThresholds();
    TestStalePose();
    TestPuppetTotals();
    TestCollisionFilter();
    TestRootInterpolation();
    std::printf("jitter tests: %s\n", g_jitterFailures == 0 ? "ok" : "FAILED");
    return g_jitterFailures;
}
