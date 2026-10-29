// The pure part of the rider follow at the ped pre-render
// (src/mount_placement.h): the engine matrix with its 32 m cells, the rigid
// offset, the follow plan, the evidence totals, and the jerk detector's
// prerender cause. Numbers from a recorded case: horse 36:37 pushed 0.113 m at
// 2581 ms, its frozen rider 35:36 left behind, engine_m 0.000.

#include "mount_placement.h"
#include "pose_jerk.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using replay::pose::Quat;
    using replay::pose::Vec3;
    using replay::pose::WorldTransform;
    using namespace replay::mount;

    int g_mountFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_mountFailures;
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

    WorldTransform Compose(const WorldTransform& parent, const Vec3& local, float yawDegrees)
    {
        const Vec3 r = replay::pose::Rotate(parent.rotation, local);
        WorldTransform child;
        child.position = {parent.position.x + r.x, parent.position.y + r.y, parent.position.z + r.z};
        child.rotation = replay::pose::Normalize(replay::pose::Multiply(parent.rotation, Yaw(yawDegrees)));
        return child;
    }

    // The engine's encoder (0x68C654, 0x179DF4): cell = truncation of the
    // position / 32 toward zero, remainder = position - cell * 32 in float,
    // z whole, the two int16 cells in the position row's w lane. The rows'
    // w lanes get marker values here to show they are kept.
    void EngineEncode(const WorldTransform& w, float m[16])
    {
        Vec3 right;
        Vec3 forward;
        Vec3 up;
        replay::pose::BasisFromQuat(replay::pose::Normalize(w.rotation), right, forward, up);
        const auto cellOf = [](float value) { return static_cast<std::int16_t>(static_cast<int>(value * 0.03125f)); };
        const std::int16_t cx = cellOf(w.position.x);
        const std::int16_t cy = cellOf(w.position.y);
        m[0] = right.x;   m[1] = right.y;   m[2] = right.z;   m[3] = 1.5f;
        m[4] = forward.x; m[5] = forward.y; m[6] = forward.z; m[7] = 0.0f;
        m[8] = up.x;      m[9] = up.y;      m[10] = up.z;     m[11] = -2.25f;
        m[12] = w.position.x - static_cast<float>(cx) * 32.0f;
        m[13] = w.position.y - static_cast<float>(cy) * 32.0f;
        m[14] = w.position.z;
        const std::uint32_t cells = static_cast<std::uint16_t>(cx) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(cy)) << 16);
        std::memcpy(&m[15], &cells, sizeof(cells));
    }

    bool Decode(const WorldTransform& w, EngineFrame& frame)
    {
        float m[16]{};
        EngineEncode(w, m);
        return DecodeEngineMatrix(m, frame);
    }

    double DistanceTo(const EngineFrame& frame, const Vec3& p)
    {
        double at[3]{};
        AbsolutePosition(frame, at);
        const double dx = at[0] - p.x;
        const double dy = at[1] - p.y;
        const double dz = at[2] - p.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    void TestDecodeEncode()
    {
        const WorldTransform w = At(-1234.567f, 2345.25f, 80.5f, 30.0f);
        float m[16]{};
        EngineEncode(w, m);
        EngineFrame frame;
        Check(DecodeEngineMatrix(m, frame), "decode: an engine matrix");
        Check(frame.cellX == -38 && frame.cellY == 73, "decode: cells truncated toward zero, as the engine's encoder");
        Check(DistanceTo(frame, w.position) < 1e-3, "decode: cell * 32 + remainder is the position");
        WorldTransform reference;
        Check(replay::pose::DecodeWorldMatrix(m, reference), "the recorder's decode reads the same matrix");
        Check(std::fabs(reference.position.x - w.position.x) < 1e-3f && std::fabs(reference.position.y - w.position.y) < 1e-3f,
            "decode agrees with pose::DecodeWorldMatrix on the position");
        Check(replay::pose::AngleBetween(FrameRotation(frame), reference.rotation) < 1e-3f,
            "decode agrees with pose::DecodeWorldMatrix on the rotation");
        // Written back unchanged: the same bits, every w lane kept.
        float back[16]{};
        std::memcpy(back, m, sizeof(back));
        EncodeKeepingCells(frame.right, frame.forward, frame.up, frame.local, frame.cellX, frame.cellY, back);
        Check(std::memcmp(back, m, sizeof(back)) == 0, "encode of the decoded frame gives the same bytes");
        // Another cell's origin: the remainder moves by the cell difference.
        float other[16]{};
        std::memcpy(other, m, sizeof(other));
        const Vec3 inNeighbour = LocalIn(frame, static_cast<std::int16_t>(frame.cellX - 1), frame.cellY);
        Check(std::fabs(inNeighbour.x - (frame.local.x + 32.0f)) < 1e-4f, "LocalIn: one cell west is 32 m more");
        EncodeKeepingCells(frame.right, frame.forward, frame.up, inNeighbour, static_cast<std::int16_t>(frame.cellX - 1),
            frame.cellY, other);
        EngineFrame otherFrame;
        Check(DecodeEngineMatrix(other, otherFrame) && otherFrame.cellX == frame.cellX && otherFrame.cellY == frame.cellY &&
                std::fabs(otherFrame.local.x - frame.local.x) < 1e-5f,
            "encode from a neighbour cell's origin lands in the matrix's own cell");
        Check(other[3] == 1.5f && other[7] == 0.0f && other[11] == -2.25f && std::memcmp(&other[15], &m[15], 4) == 0,
            "the rows' w lanes and the cells are left alone");
    }

    void TestDecodeRejects()
    {
        float zero[16]{};
        EngineFrame frame;
        Check(!DecodeEngineMatrix(zero, frame), "decode: an empty matrix is not a rotation");
        float m[16]{};
        EngineEncode(At(10.0f, 20.0f, 1.0f, 0.0f), m);
        float scaled[16]{};
        std::memcpy(scaled, m, sizeof(scaled));
        scaled[0] *= 1.2f;
        scaled[1] *= 1.2f;
        scaled[2] *= 1.2f;
        Check(!DecodeEngineMatrix(scaled, frame), "decode: a scaled row is refused");
        float broken[16]{};
        std::memcpy(broken, m, sizeof(broken));
        const std::uint32_t nan = 0x7FC00000u;
        std::memcpy(&broken[13], &nan, sizeof(nan));
        Check(!DecodeEngineMatrix(broken, frame), "decode: a NaN position is refused");
    }

    struct Pair
    {
        WorldTransform horse;
        WorldTransform rider;
    };

    // A rider in the saddle (0.02 m right, 0.05 m forward, 0.33 m up, the
    // seat of clip 104316), turned 2 degrees against the horse.
    Pair Saddle(float x, float y, float z, float yaw, const Vec3& seat = {0.02f, 0.05f, 0.33f})
    {
        Pair pair;
        pair.horse = At(x, y, z, yaw);
        pair.rider = Compose(pair.horse, seat, 2.0f);
        return pair;
    }

    FollowInput Input(const Pair& placed, const WorldTransform& horseNow, const WorldTransform& riderNow, bool enabled)
    {
        FollowInput input;
        Check(Decode(horseNow, input.mount), "the mount decodes");
        Check(Decode(riderNow, input.rider), "the rider decodes");
        input.mountPlaced = placed.horse;
        input.riderPlaced = placed.rider;
        input.offset = OffsetFromTransforms(placed.horse, placed.rider);
        input.enabled = enabled;
        return input;
    }

    WorldTransform Pushed(const WorldTransform& w, const Vec3& push, float yawDegrees = 0.0f)
    {
        WorldTransform out = w;
        out.position = {w.position.x + push.x, w.position.y + push.y, w.position.z + push.z};
        out.rotation = replay::pose::Normalize(replay::pose::Multiply(Yaw(yawDegrees), w.rotation));
        return out;
    }

    // The rider's matrix after the detour's write, decoded.
    EngineFrame Written(const WorldTransform& riderNow, const FollowPlan& plan)
    {
        float m[16]{};
        EngineEncode(riderNow, m);
        EngineFrame before;
        DecodeEngineMatrix(m, before);
        EncodeKeepingCells(plan.right, plan.forward, plan.up, plan.position, before.cellX, before.cellY, m);
        EngineFrame after;
        Check(DecodeEngineMatrix(m, after), "the written matrix decodes");
        return after;
    }

    void TestNoPush()
    {
        // Far from the origin (float steps of 0.25 mm there): still no write.
        const Pair placed = Saddle(-2345.6f, 1876.3f, 95.2f, 37.0f);
        const FollowPlan plan = PlanFollow(Input(placed, placed.horse, placed.rider, true));
        Check(!plan.write && !plan.refused, "horse at its placement: the rider is not written");
        Check(plan.correctionM < 1e-3 && plan.correctionRad < 0.0005f, "the follow target is the recorded rider");
        Check(plan.mountMovedM < 1e-3 && plan.joltM < 1e-3 && plan.riderMovedM < 1e-3, "nothing moved, no jolt");
    }

    void TestPushedHorse060323()
    {
        const Pair placed = Saddle(-114.2f, -56.1f, 93.5f, 120.0f);
        const Vec3 push{0.080f, -0.075f, 0.030f}; // 0.1137 m
        const WorldTransform horseNow = Pushed(placed.horse, push);
        // Off: measured only. The frozen rider stays; the frame shows him
        // 11 cm off the saddle (what the detector's mount_offset counted).
        const FollowPlan measured = PlanFollow(Input(placed, horseNow, placed.rider, false));
        Check(!measured.write, "MountPreRender=0: no write");
        Check(std::fabs(measured.mountMovedM - 0.1137) < 1e-3, "the horse is 11 cm off its placement at the pre-render");
        Check(measured.riderMovedM < 1e-3, "the frozen rider is where the recording put him (engine_m 0.000)");
        Check(std::fabs(measured.joltM - 0.1137) < 1e-3 && measured.joltM >= FollowThresholds{}.joltM,
            "the drawn jolt is the horse's push, over the 4 cm of mount_offset");
        Check(std::fabs(measured.joltAfterM - measured.joltM) < 1e-9, "not written: the jolt stays");
        // On: the rider is carried with the horse.
        const FollowPlan plan = PlanFollow(Input(placed, horseNow, placed.rider, true));
        Check(plan.write, "MountPreRender=1: the rider is written");
        Check(std::fabs(plan.correctionM - 0.1137) < 1e-3, "moved by the horse's push");
        Check(plan.joltAfterM < 1e-3, "no jolt left in the drawn frame");
        const EngineFrame after = Written(placed.rider, plan);
        const Vec3 expected{placed.rider.position.x + push.x, placed.rider.position.y + push.y, placed.rider.position.z + push.z};
        Check(DistanceTo(after, expected) < 1e-3, "the written rider sits where the pushed horse's saddle is");
        Check(replay::pose::AngleBetween(FrameRotation(after), placed.rider.rotation) < 0.001f, "a pure push does not turn him");
    }

    void TestTurnedHorse()
    {
        // A second rider behind the saddle (y -0.7): a 1 degree yaw push of
        // the horse about its origin moves him sideways too.
        const Pair placed = Saddle(512.4f, -1730.8f, 60.0f, -75.0f, {0.0f, -0.7f, 0.3f});
        const Vec3 push{0.03f, 0.02f, 0.0f};
        const WorldTransform horseNow = Pushed(placed.horse, push, 1.0f);
        const FollowPlan plan = PlanFollow(Input(placed, horseNow, placed.rider, true));
        Check(plan.write, "turned horse: written");
        const WorldTransform expected = Compose(horseNow, {0.0f, -0.7f, 0.3f}, 2.0f);
        const EngineFrame after = Written(placed.rider, plan);
        Check(DistanceTo(after, expected.position) < 1e-3, "the rider turns about the horse's origin with it");
        Check(replay::pose::AngleBetween(FrameRotation(after), expected.rotation) < 0.001f, "and turns with it");
        Check(std::fabs(plan.mountTurnedRad - 0.01745f) < 0.0005f, "the horse's turn is measured");
    }

    void TestCellEdges()
    {
        // The rider 1 cm short of his cell's east edge, the horse pushed 5 cm
        // east: the written remainder is past 32 m in the same cell, which
        // decodes to the right place.
        const Pair placed = Saddle(32.0f * 9.0f + 31.99f, 100.0f, 5.0f, 90.0f, {0.0f, 0.0f, 0.33f});
        const Vec3 push{0.05f, 0.0f, 0.0f};
        const FollowPlan plan = PlanFollow(Input(placed, Pushed(placed.horse, push), placed.rider, true));
        const EngineFrame after = Written(placed.rider, plan);
        Check(after.cellX == 9 && after.local.x > 32.0f, "the rider's cell is kept, the remainder runs past 32 m");
        const Vec3 expected{placed.rider.position.x + push.x, placed.rider.position.y, placed.rider.position.z};
        Check(DistanceTo(after, expected) < 1e-3, "past the edge it still decodes to the pushed saddle");
        // The horse already over the edge in the next cell: LocalIn brings it
        // into the rider's cell.
        // Yaw 90 turns the seat's +y to -x: the rider 1 cm west, in cell 9.
        const Pair across = Saddle(32.0f * 10.0f + 0.004f, 100.0f, 5.0f, 90.0f, {0.0f, 0.01f, 0.33f});
        FollowInput input = Input(across, Pushed(across.horse, push), across.rider, true);
        Check(input.mount.cellX == 10 && input.rider.cellX == 9, "horse and rider in neighbouring cells");
        const FollowPlan acrossPlan = PlanFollow(input);
        const EngineFrame acrossAfter = Written(across.rider, acrossPlan);
        const Vec3 acrossExpected{across.rider.position.x + push.x, across.rider.position.y, across.rider.position.z};
        Check(DistanceTo(acrossAfter, acrossExpected) < 1e-3, "followed across the cell edge");
    }

    void TestThresholds()
    {
        const Pair placed = Saddle(-300.0f, 400.0f, 20.0f, 10.0f);
        const FollowPlan small = PlanFollow(Input(placed, Pushed(placed.horse, {0.0015f, 0.0f, 0.0f}), placed.rider, true));
        Check(!small.write, "1.5 mm: under the 2 mm write threshold");
        const FollowPlan enough = PlanFollow(Input(placed, Pushed(placed.horse, {0.003f, 0.0f, 0.0f}), placed.rider, true));
        Check(enough.write, "3 mm: written");
        const FollowPlan turned = PlanFollow(Input(placed, Pushed(placed.horse, {}, 0.2f), placed.rider, true));
        Check(turned.write, "a 0.2 degree turn alone: written");
        const FollowPlan far = PlanFollow(Input(placed, Pushed(placed.horse, {2.0f, 0.0f, 0.0f}), placed.rider, true));
        Check(far.refused && !far.write, "2 m off: a wrong pair, refused");
        const FollowPlan farOff = PlanFollow(Input(placed, Pushed(placed.horse, {2.0f, 0.0f, 0.0f}), placed.rider, false));
        Check(!farOff.refused && !farOff.write, "off: nothing refused, nothing written");
    }

    FrameRecord Record(std::uint32_t calls, double mountM, double joltM, bool wrote, double appliedM)
    {
        FrameRecord record;
        record.rider = (35u << 16) | 36u;
        record.mount = (36u << 16) | 37u;
        record.playheadMs = 2581;
        record.calls = calls;
        record.ageUs = 4200;
        record.wrote = wrote;
        record.plan.mountMovedM = mountM;
        record.plan.joltM = joltM;
        record.plan.correctionM = appliedM;
        record.plan.joltAfterM = wrote ? 0.0 : joltM;
        return record;
    }

    void TestTotals()
    {
        Totals totals;
        std::string line;
        totals.NoteTick(1);
        totals.Add(Record(0, 0.0, 0.0, false, 0.0), {}, line);
        Check(line.empty(), "no pre-render call: counted as missed, no line");
        totals.Add(Record(1, 0.0, 0.0, false, 0.0), {}, line);
        Check(line.empty(), "an ordinary frame: no line");
        totals.Add(Record(1, 0.113, 0.113, false, 0.0), {}, line);
        Check(line.rfind("POSE_V2_MOUNT_PRERENDER,phase=moved,rider=35:36,mount=36:37,playhead_ms=2581,mount_m=0.113", 0) == 0,
            "a pushed horse, measured: a moved line with the pair and the push");
        totals.Add(Record(1, 0.113, 0.113, true, 0.113), {}, line);
        Check(line.rfind("POSE_V2_MOUNT_PRERENDER,phase=reposition,", 0) == 0 && line.find("applied_m=0.113") != std::string::npos &&
                line.find("jolt_after_m=0.000") != std::string::npos,
            "written: a reposition line with the size");
        const std::string summary = totals.Summary(true, "installed");
        Check(summary.rfind("POSE_V2_MOUNT_PRERENDER_SUMMARY,enabled=1,hook=installed,ticks=1,pairs=1,rider_calls=3,missed=1,", 0) == 0,
            "summary: the head");
        Check(summary.find("drawn_jolt_4cm=2,") != std::string::npos, "summary: two drawn jolts");
        Check(summary.find("repositions=1,applied_max_m=0.113,applied_mean_m=0.113,") != std::string::npos,
            "summary: one reposition and its size");
        Check(summary.find("left_jolt_4cm=1,") != std::string::npos, "summary: the measured one stays a jolt");
        Check(summary.find("mount_moved_5cm=2,") != std::string::npos, "summary: the horse pushed past 5 cm twice");
        Check(summary.find("age_min_us=4200,age_mean_us=4200,age_max_us=4200,") != std::string::npos, "summary: publish to pre-render");
        for (int n = 0; n < 60; ++n)
            totals.Add(Record(1, 0.2, 0.2, false, 0.0), {}, line);
        Check(line.empty(), "the evidence lines stop at the cap");
        Check(totals.Summary(false, "installed").find(",lines=48\n") != std::string::npos, "48 lines at most");
        totals.Reset();
        Check(totals.Ticks() == 0 && totals.Repositions() == 0, "reset");
    }

    // The parent matrix is written only while the walk from the ped finds the
    // published one; the stale case is counted and shown.
    void TestParentCheck()
    {
        Check(CheckParent(0, false, 0) == ParentCheck::None, "nothing published: no parent to check");
        Check(CheckParent(0, true, 0x1000) == ParentCheck::None, "nothing published, walk found one: still none");
        Check(CheckParent(0x2000, true, 0x2000) == ParentCheck::Same, "the published matrix: written");
        Check(CheckParent(0x2000, true, 0x3000) == ParentCheck::Stale, "a rebuilt skeleton: stale");
        Check(CheckParent(0x2000, false, 0x2000) == ParentCheck::Stale, "a failed walk: stale, whatever it left");
        Check(CheckParent(0x2000, true, 0) == ParentCheck::Stale, "a walk with no parent: stale");

        Totals totals;
        std::string line;
        totals.NoteTick(2);
        totals.Add(Record(1, 0.0, 0.0, false, 0.0), {}, line);
        Check(line.empty(), "an ordinary frame: no line");
        FrameRecord stale = Record(1, 0.0, 0.0, true, 0.001);
        stale.parentStale = true;
        totals.Add(stale, {}, line);
        Check(line.rfind("POSE_V2_MOUNT_PRERENDER,", 0) == 0 && line.find(",parent_stale=1\n") != std::string::npos,
            "a stale parent: an evidence line that says so");
        FrameRecord missed = stale;
        missed.calls = 0;
        totals.Add(missed, {}, line);
        Check(line.empty(), "no pre-render call: nothing stale counted");
        Check(totals.Summary(true, "installed").find(",parent_stale=1,lines=1\n") != std::string::npos,
            "summary: one stale parent");
    }

    // The jerk detector: a rider the pre-render carried with his mount moved
    // by the mount's push. No mount_offset, and the push is reported once,
    // on the horse.
    void TestDetectorCause()
    {
        using namespace replay::jerk;
        Check(CauseText(kCauseMounted | kCausePreRender) == "mounted+prerender", "prerender cause text");
        Detector detector;
        std::vector<Finding> findings;
        std::uint32_t playhead = 3000;
        double wall = 100000.0;
        float y = 0.0f;
        const auto tick = [&](const Vec3& horsePush, const Vec3& riderPush, std::uint32_t riderCauses) {
            playhead += 16;
            wall += 16.0;
            const Vec3 previousHorse{0.0f, y, 1.0f};
            const Vec3 previousRider{0.0f, y + 0.05f, 1.33f};
            y += 0.216f;
            detector.BeginTick(playhead, wall, 1.0, true);
            Observation horse;
            horse.packed = (36u << 16) | 37u;
            horse.placed = true;
            horse.placedAt = previousHorse;
            horse.hasEngine = true;
            horse.engine = {previousHorse.x + horsePush.x, previousHorse.y + horsePush.y, previousHorse.z + horsePush.z};
            horse.recorded = {0.0f, y, 1.0f};
            horse.playheadMs = playhead;
            horse.causes = kCausePhysics;
            detector.Observe(horse, findings);
            Observation rider;
            rider.packed = (35u << 16) | 36u;
            rider.mount = horse.packed;
            rider.placed = true;
            rider.placedAt = previousRider;
            rider.hasEngine = true;
            rider.engine = {previousRider.x + riderPush.x, previousRider.y + riderPush.y, previousRider.z + riderPush.z};
            rider.recorded = {0.0f, y + 0.05f, 1.33f};
            rider.playheadMs = playhead;
            rider.causes = kCauseFrozen | riderCauses;
            detector.Observe(rider, findings);
            detector.EndTick(findings);
        };
        for (int n = 0; n < 4; ++n)
            tick({}, {}, 0);
        findings.clear();
        const Vec3 push{0.0f, 0.113f, 0.0f};
        tick(push, {}, 0);
        std::size_t mountOffsets = 0;
        for (const Finding& f : findings)
            mountOffsets += f.kind == Kind::MountOffset ? 1 : 0;
        Check(mountOffsets == 1, "0.175: the frozen rider left behind is a mount_offset");
        findings.clear();
        tick(push, push, kCausePreRender);
        std::size_t riderMoved = 0;
        std::size_t horseMoved = 0;
        mountOffsets = 0;
        for (const Finding& f : findings)
        {
            mountOffsets += f.kind == Kind::MountOffset ? 1 : 0;
            riderMoved += (f.kind == Kind::EngineMoved && f.packed == ((35u << 16) | 36u)) ? 1 : 0;
            horseMoved += (f.kind == Kind::EngineMoved && f.packed == ((36u << 16) | 37u)) ? 1 : 0;
        }
        Check(mountOffsets == 0, "carried at the pre-render: no mount_offset");
        Check(riderMoved == 0, "carried: the rider's displacement is not an engine_moved of his own");
        Check(horseMoved == 1, "the horse's push is still reported");
        findings.clear();
        tick({}, push, 0);
        riderMoved = 0;
        for (const Finding& f : findings)
            riderMoved += (f.kind == Kind::EngineMoved && f.packed == ((35u << 16) | 36u)) ? 1 : 0;
        Check(riderMoved == 1, "without the cause, a rider moved alone is still an engine_moved");
    }
    // Two riders on one horse. Each is a pair of its own in the pre-render table;
    // the summary says how many shared a mount.
    void TestSharedMountRiders()
    {
        std::uint32_t perMount = 9;
        Check(SharedMountRiders(nullptr, 0, perMount) == 0 && perMount == 0, "no rider: nothing shared");
        const std::uint64_t alone[] = {0x7000};
        Check(SharedMountRiders(alone, 1, perMount) == 0 && perMount == 1, "one rider on one horse");
        const std::uint64_t garupa[] = {0x7000, 0x7100, 0x7000, 0, 0x7200};
        Check(SharedMountRiders(garupa, 5, perMount) == 2 && perMount == 2, "035: two riders on 0x7000, the others alone");
        const std::uint64_t three[] = {0x7000, 0x7000, 0x7000};
        Check(SharedMountRiders(three, 3, perMount) == 3 && perMount == 3, "three on one");
        Totals totals;
        totals.NoteTick(2);
        totals.NoteShared(2, 2);
        totals.NoteTick(1);
        totals.NoteShared(0, 1);
        const std::string summary = totals.Summary(false, "installed");
        Check(summary.find(",ticks=2,pairs=3,") != std::string::npos, "the pairs: both riders counted");
        Check(summary.find(",shared_mount_riders=2,max_riders_per_mount=2,parent_stale=0,lines=0\n") != std::string::npos,
            "summary: the shared riders and the most on one mount");
        totals.Reset();
        Check(totals.Summary(false, "x").find(",shared_mount_riders=0,max_riders_per_mount=0,") != std::string::npos, "reset");
    }
}

int RunMountPlacementTests()
{
    TestDecodeEncode();
    TestDecodeRejects();
    TestNoPush();
    TestPushedHorse060323();
    TestTurnedHorse();
    TestCellEdges();
    TestThresholds();
    TestTotals();
    TestParentCheck();
    TestDetectorCause();
    TestSharedMountRiders();
    std::printf("mount placement tests: %s\n", g_mountFailures == 0 ? "ok" : "FAILED");
    return g_mountFailures;
}
