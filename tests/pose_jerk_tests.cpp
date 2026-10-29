// Rider and mount pairing and the playback jerk detector (src/pose_jerk.h),
// with the numbers of a recorded clip (Arthur on his horse, galloping at 11 to
// 14 m/s through Valentine).

#include "pose_jerk.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace replay::jerk;
    using replay::pose::Quat;
    using replay::pose::Vec3;
    using replay::pose::WorldTransform;

    int g_jerkFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_jerkFailures;
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

    // The point at `local` (mount space) in the world.
    Vec3 FromMount(const WorldTransform& mount, const Vec3& local)
    {
        const Vec3 r = replay::pose::Rotate(mount.rotation, local);
        return {mount.position.x + r.x, mount.position.y + r.y, mount.position.z + r.z};
    }

    std::size_t CountKind(const std::vector<Finding>& findings, Kind kind, std::uint32_t packed = 0)
    {
        std::size_t n = 0;
        for (const Finding& f : findings)
            n += (f.kind == kind && (!packed || f.packed == packed)) ? 1 : 0;
        return n;
    }

    void TestSaddle()
    {
        // clip 104316, ReplayClipTool mount 1:4246 0:4245: rider origin in
        // the horse's space over 3 to 13 s (samples at 3.2, 5.6, 8.0, 12.3 s).
        const Vec3 recorded[] = {{0.011f, 0.006f, 0.351f}, {-0.024f, 0.003f, 0.340f}, {-0.021f, 0.076f, 0.390f},
            {-0.022f, 0.074f, 0.386f}, {0.009f, 0.029f, 0.375f}, {-0.004f, 0.045f, 0.276f}};
        const WorldTransform horse = At(-136.57f, 481.79f, 112.46f, 37.0f);
        for (const Vec3& local : recorded)
        {
            Check(InSaddle(horse, FromMount(horse, local), kSaddleEnter), "Arthur's recorded seat is in the saddle box");
            const Vec3 back = InMountSpace(horse, FromMount(horse, local));
            Check(std::fabs(back.x - local.x) < 1e-3f && std::fabs(back.z - local.z) < 1e-3f, "mount space round trip");
        }
        Check(!InSaddle(horse, FromMount(horse, {0.75f, 0.1f, 0.0f}), kSaddleEnter), "a ped beside the horse does not ride it");
        Check(!InSaddle(horse, FromMount(horse, {0.1f, 1.4f, -0.05f}), kSaddleEnter), "a ped at the horse's head does not ride it");
        Check(!InSaddle(horse, FromMount(horse, {0.0f, -1.5f, 0.0f}), kSaddleEnter), "a ped behind the horse does not ride it");
        Check(InSaddle(horse, FromMount(horse, {0.0f, -0.7f, 0.3f}), kSaddleEnter), "a second rider behind the saddle rides it");
    }

    void TestPairs()
    {
        MountPairs pairs;
        const WorldTransform horse = At(10.0f, 20.0f, 1.0f, 90.0f);
        std::vector<Body> bodies(3);
        bodies[0].packed = 0x1095; bodies[0].horse = true; bodies[0].world = horse;
        bodies[1].packed = 0x11096; bodies[1].world = At(0, 0, 0, 90.0f);
        bodies[1].world.position = FromMount(horse, {0.0f, 0.05f, 0.33f});
        bodies[2].packed = 0x22000; bodies[2].world = At(0, 0, 0, 0.0f);
        bodies[2].world.position = FromMount(horse, {0.9f, 0.0f, 0.0f}); // standing beside
        pairs.Update(bodies);
        Check(pairs.MountOf(0x11096) == 0x1095, "the rider in the saddle is paired");
        Check(pairs.MountOf(0x22000) == 0, "the ped beside is not");
        Check(pairs.MountOf(0x1095) == 0, "a horse rides nothing");
        // Dismounting: at z 0.05 (out of the tight box, inside the loose one)
        // the pair holds; on the ground beside the horse it ends.
        bodies[1].world.position = FromMount(horse, {0.3f, 0.0f, 0.05f});
        pairs.Update(bodies);
        Check(pairs.MountOf(0x11096) == 0x1095, "the pair holds at the edge (no flicker)");
        bodies[1].world.position = FromMount(horse, {0.8f, 0.0f, -0.1f});
        pairs.Update(bodies);
        Check(pairs.MountOf(0x11096) == 0, "the pair ends off the horse");
        Check(pairs.Enters() == 1 && pairs.Leaves() == 1, "one enter, one leave");
        // A horse gone: its rider rides nothing.
        bodies[1].world.position = FromMount(horse, {0.0f, 0.05f, 0.33f});
        pairs.Update(bodies);
        bodies.erase(bodies.begin());
        pairs.Update(bodies);
        Check(pairs.MountOf(0x11096) == 0, "no horse, no pair");
    }

    struct Gallop
    {
        Detector detector;
        std::vector<Finding> findings;
        std::uint32_t playhead = 3000;
        double wall = 100000.0;
        float x = 0.0f;
        std::uint64_t riderWrites = 0;
        std::uint64_t horseWrites = 0;
        float riderRootZ = 0.0f;

        // One tick: the playhead moves playStep, the wall wallStep; the
        // horse gallops at 13.5 m/s along +Y, the rider in the saddle.
        void Tick(float playStep, double wallStep, const Vec3& riderPush = {}, const Vec3& horsePush = {},
            bool normal = true, bool riderWritten = true)
        {
            playhead += static_cast<std::uint32_t>(playStep);
            wall += wallStep;
            const Vec3 previousHorse{0.0f, x, 1.0f};
            const Vec3 previousRider{0.0f, x + 0.05f, 1.33f};
            x += 13.5f * playStep / 1000.0f;
            detector.BeginTick(playhead, wall, 1.0, normal);
            Observation horse;
            horse.packed = 0x1095;
            horse.placed = true;
            horse.placedAt = previousHorse;
            horse.hasEngine = true;
            horse.engine = {previousHorse.x + horsePush.x, previousHorse.y + horsePush.y, previousHorse.z + horsePush.z};
            horse.recorded = {0.0f, x, 1.0f};
            horse.hasOverride = true;
            horse.overrideWrites = ++horseWrites;
            horse.playheadMs = playhead;
            horse.causes = kCausePhysics;
            detector.Observe(horse, findings);
            Observation rider;
            rider.packed = 0x11096;
            rider.mount = 0x1095;
            rider.placed = true;
            rider.placedAt = previousRider;
            rider.hasEngine = true;
            rider.engine = {previousRider.x + riderPush.x, previousRider.y + riderPush.y, previousRider.z + riderPush.z};
            rider.recorded = {0.0f, x + 0.05f, 1.33f};
            rider.hasOverride = true;
            if (riderWritten)
                ++riderWrites;
            rider.overrideWrites = riderWrites;
            rider.hasRoot = true;
            riderRootZ += 0.04f; // the gallop bob, 4 cm per tick here
            rider.rootObject = {0.0f, 0.0f, riderRootZ};
            rider.playheadMs = playhead;
            detector.Observe(rider, findings);
            detector.EndTick(findings);
        }
    };

    void TestSmoothGallop()
    {
        Gallop g;
        for (int n = 0; n < 120; ++n)
            g.Tick(n % 3 == 0 ? 17.0f : 16.0f, n % 3 == 0 ? 17.0 : 16.0);
        Check(g.findings.empty(), "a smooth gallop has no jerk");
        Check(g.detector.Totals().ticks == 119, "every tick after the first is observed");
        Check(g.detector.Totals().observations == 2 * 119, "both puppets observed on consecutive ticks");
    }

    void TestRiderPushed()
    {
        // The rider's capsule hits a kerb: the engine leaves him 12 cm up
        // while the horse is where we placed it.
        Gallop g;
        for (int n = 0; n < 10; ++n)
            g.Tick(16.0f, 16.0);
        g.Tick(16.0f, 16.0, {0.0f, -0.02f, 0.12f});
        Check(CountKind(g.findings, Kind::EngineMoved, 0x11096) == 1, "the push moved the rider off his placement");
        Check(CountKind(g.findings, Kind::EngineMoved, 0x1095) == 0, "the horse was not moved");
        Check(CountKind(g.findings, Kind::MountOffset, 0x11096) == 1, "the rider moved against his mount");
        const Finding& pair = g.findings.back();
        Check(pair.kind == Kind::MountOffset && pair.mount == 0x1095 && std::fabs(pair.magnitudeM - 0.1217f) < 0.002f,
            "pair finding: mount and size");
        Check((pair.causes & kCauseMounted) && (pair.causes & kCausePhysics), "causes: mounted, physics");
        // Both moved together (a whole-pair push) is no pair jerk.
        Gallop h;
        for (int n = 0; n < 10; ++n)
            h.Tick(16.0f, 16.0);
        h.Tick(16.0f, 16.0, {0.0f, 0.0f, 0.1f}, {0.0f, 0.0f, 0.1f});
        Check(CountKind(h.findings, Kind::MountOffset) == 0, "the pair moved as one: no pair jerk");
        Check(CountKind(h.findings, Kind::EngineMoved) == 2, "but each was moved off its placement");
    }

    // With the end-of-frame placement on, a push it puts back before the frame is
    // drawn is counted apart, not a jerk.
    void TestPlacedBack()
    {
        Detector detector;
        std::vector<Finding> findings;
        std::uint32_t playhead = 1000;
        double wall = 5000.0;
        for (int n = 0; n < 4; ++n)
        {
            playhead += 16;
            wall += 16.0;
            detector.BeginTick(playhead, wall, 1.0, true);
            Observation o;
            o.packed = 0x2003;
            o.placed = true;
            o.placedAt = {0.0f, 0.0f, 1.0f};
            o.hasEngine = true;
            o.engine = {0.0f, 0.0f, n == 2 ? 1.12f : 1.0f};
            o.recorded = {0.0f, 0.0f, 1.0f};
            o.playheadMs = playhead;
            o.causes = kCausePhysics;
            o.frameEndRefuseM = n == 3 ? 0.05f : 25.0f;
            if (n == 3)
                o.engine = {0.0f, 0.0f, 1.12f};
            detector.Observe(o, findings);
            detector.EndTick(findings);
        }
        Check(detector.Totals().placedBack == 1, "the 12 cm push the end-of-frame placement undid is counted apart");
        Check(CountKind(findings, Kind::EngineMoved, 0x2003) == 1, "a push past the placement's refuse limit is still a jerk");
    }

    void TestHeldAndCapped()
    {
        // A creation hold: the wall moved 16 ms, the playhead did not.
        Gallop g;
        for (int n = 0; n < 10; ++n)
            g.Tick(16.0f, 16.0);
        g.Tick(0.0f, 16.0);
        Check(CountKind(g.findings, Kind::TimeStep) == 1, "a held tick is a time step jerk");
        const Finding& held = g.findings.back();
        Check((held.causes & kCauseHeld) && std::fabs(held.magnitudeM - 13.5f * 0.016f) < 0.01f,
            "held: cause and size (speed x the lost step)");
        Check(g.detector.Totals().heldTicks == 1, "one held tick");
        // A 500 ms stall: the playhead moves the capped 200 ms.
        g.Tick(200.0f, 500.0);
        Check(g.detector.Totals().cappedTicks == 1 && g.detector.Totals().slowTicks == 1, "capped and slow");
        Check(CountKind(g.findings, Kind::TimeStep) == 2, "the capped stall is a time step jerk");
        // A 60 ms frame followed by the playhead: the motion is right, a stall.
        g.Tick(60.0f, 60.0);
        Check(CountKind(g.findings, Kind::Stall) == 1, "a long frame the playhead followed is a stall");
    }

    void TestPoseStale()
    {
        Gallop g;
        for (int n = 0; n < 10; ++n)
            g.Tick(16.0f, 16.0);
        g.Tick(16.0f, 16.0, {}, {}, true, false); // the rider's finalizer did not run
        Check(CountKind(g.findings, Kind::PoseStale, 0x11096) == 1, "a skeleton not written for a frame is a stale pose");
        Check(CountKind(g.findings, Kind::PoseStale, 0x1095) == 0, "the horse was written");
    }

    void TestPauseForgets()
    {
        Gallop g;
        for (int n = 0; n < 5; ++n)
            g.Tick(16.0f, 16.0);
        g.Tick(0.0f, 500.0, {}, {}, false); // paused
        g.Tick(16.0f, 16.0, {0.0f, 0.0f, 0.3f}); // first play tick: nothing carried
        Check(g.findings.empty(), "a paused tick forgets the placements");
        g.Tick(16.0f, 16.0, {0.0f, 0.0f, 0.3f});
        Check(CountKind(g.findings, Kind::EngineMoved) == 1, "the next tick checks again");
        // Sent back (a seek): no time jerk from the negative step.
        g.playhead -= 1000;
        g.Tick(16.0f, 16.0);
        Check(CountKind(g.findings, Kind::TimeStep) == 0, "a seek back is no time jerk");
        const std::vector<PuppetSummary> worst = g.detector.Worst(8);
        Check(!worst.empty() && worst.front().packed == 0x11096, "worst puppet: the rider");
        Check(CauseText(kCauseHeld | kCauseMounted) == "held+mounted", "cause text");
    }

    // POSE_JERK id=35:36 kind=mount_offset magnitude 0.113 engine_m 0.000 and, the
    // same tick, id=36:37 (its horse) engine_moved 0.113, causes physics.
    void TestHorsePushedRiderFrozen()
    {
        Gallop g;
        for (int n = 0; n < 5; ++n)
            g.Tick(16.0f, 16.0);
        g.findings.clear();
        g.Tick(16.0f, 16.0, {}, {0.0f, 0.113f, 0.0f});
        Check(CountKind(g.findings, Kind::EngineMoved, 0x1095) == 1, "0.174: the horse pushed 11 cm");
        Check(CountKind(g.findings, Kind::MountOffset, 0x11096) == 1, "0.174: the frozen rider left behind");
        bool riderStill = false;
        for (const Finding& f : g.findings)
            if (f.kind == Kind::MountOffset)
                riderStill = f.engineM == 0.0f && std::fabs(f.magnitudeM - 0.113f) < 1e-3f;
        Check(riderStill, "0.174: engine_m 0 for the rider, the size of the horse's push");
        // Attached: the engine carries the rider with the horse.
        g.findings.clear();
        g.Tick(16.0f, 16.0, {0.0f, 0.113f, 0.0f}, {0.0f, 0.113f, 0.0f});
        Check(CountKind(g.findings, Kind::MountOffset) == 0, "0.175: an attached rider follows the pushed horse");
        Check(CountKind(g.findings, Kind::EngineMoved, 0x11096) == 1, "the pair still shows as pushed (real, not hidden)");
    }

    // Kieran 1:2250 behind Arthur 0:2249 on Arthur's horse 2:2251 (numbers as
    // ReplayClipTool reads the clip): Kieran's seat x 0.25 y -0.53 z 0.43 at the
    // enter (9.0 s), Arthur 0.03 -0.03 0.36 (12.5 s); mount 1:2250 2:2251: y -0.45
    // to -0.58 and z 0.22 to 0.54 for the rest of the ride.
    void TestTwoRidersPairs()
    {
        MountPairs pairs;
        const WorldTransform horse = At(-1456.0f, 1614.0f, 238.0f, 27.0f);
        std::vector<Body> bodies(3);
        bodies[0].packed = 0x208cb; bodies[0].horse = true; bodies[0].world = horse; // 2:2251
        bodies[1].packed = 0x108ca; bodies[1].world = horse;                          // 1:2250, Kieran
        bodies[1].world.position = FromMount(horse, {0.25f, -0.53f, 0.43f});
        bodies[2].packed = 0x008c9; bodies[2].world = horse;                          // 0:2249, Arthur
        bodies[2].world.position = FromMount(horse, {0.8f, 0.2f, 0.0f});               // still beside the horse
        pairs.Update(bodies);
        Check(pairs.MountOf(0x108ca) == 0x208cb, "035: the passenger alone is a rider of the horse");
        Check(pairs.SeatOf(0x108ca) == MountPairs::kSaddleSeat, "alone on the horse he is the front-most rider");
        Check(pairs.RidersOf(0x208cb) == 1 && pairs.MaxRiders() == 1 && pairs.SharedUpdates() == 0, "one rider");
        bodies[2].world.position = FromMount(horse, {0.03f, -0.03f, 0.36f}); // Arthur in the saddle
        pairs.Update(bodies);
        Check(pairs.MountOf(0x008c9) == 0x208cb && pairs.MountOf(0x108ca) == 0x208cb, "035: two riders, one horse");
        Check(pairs.SeatOf(0x008c9) == -1 && pairs.SeatOf(0x108ca) == 0, "the saddle -1, the passenger behind 0");
        // Kieran kept his mount and his enter line said -1; the seat change is
        // reported, Arthur's is an enter (not a change).
        Check(pairs.SeatChanges().size() == 1 && pairs.SeatChanges()[0] == 0x108ca && pairs.PreviousSeatOf(0x108ca) == -1,
            "035: the passenger's seat change -1 to 0 is reported once");
        Check(pairs.RidersOf(0x208cb) == 2 && pairs.MaxRiders() == 2 && pairs.SharedUpdates() == 1, "two riders counted");
        // The ride: the passenger bobs between the recorded extremes and
        // keeps the pair (the loose box), behind the saddle.
        const Vec3 ride[] = {{-0.013f, -0.574f, 0.406f}, {0.003f, -0.507f, 0.228f}, {-0.002f, -0.473f, 0.540f},
            {0.019f, -0.443f, 0.320f}, {-0.024f, -0.487f, 0.344f}};
        for (const Vec3& local : ride)
        {
            bodies[1].world.position = FromMount(horse, local);
            pairs.Update(bodies);
            Check(pairs.MountOf(0x108ca) == 0x208cb && pairs.SeatOf(0x108ca) == 0 && pairs.SeatChanges().empty(),
                "035: the passenger holds his seat");
        }
        Check(pairs.Enters() == 2 && pairs.Leaves() == 0, "two enters, no flicker");
        // Arthur gets off: Kieran is the only rider again.
        bodies[2].world.position = FromMount(horse, {0.9f, 0.0f, -0.1f});
        pairs.Update(bodies);
        Check(pairs.MountOf(0x008c9) == 0 && pairs.RidersOf(0x208cb) == 1, "the saddle rider left");
        Check(pairs.PreviousSeatOf(0x008c9) == -1 && pairs.SeatOf(0x108ca) == -1 && pairs.SeatChanges().size() == 1 &&
                pairs.SeatChanges()[0] == 0x108ca,
            "the leave keeps the seat he had; the passenger moves up to the saddle (a seat change)");
        Check(pairs.MaxRiders() == 2, "the most riders is kept for the playback");
        pairs.ResetShared();
        Check(pairs.MaxRiders() == 0 && pairs.SharedUpdates() == 0 && pairs.Enters() == 2, "the shared counts reset alone");
        Check(pairs.RidersOf(0) == 0, "no mount, no riders");
        pairs.Clear();
        Check(pairs.Size() == 0 && pairs.SeatChanges().empty() && pairs.PreviousSeatOf(0x108ca) == -1, "clear");
    }

    // The horse 2:2251 off its placement by 10.6 cm, both riders at engine_m 0, two
    // mount_offset of 0.106 at 25286 ms. One move of the mount is one finding
    // (riders=2); under the end-of-frame placement it is put back before the frame
    // is drawn.
    struct TwoRiders
    {
        Detector detector;
        std::vector<Finding> findings;
        std::uint32_t playhead = 25000;
        double wall = 900000.0;
        float y = 0.0f;

        void Tick(const Vec3& horsePush, const Vec3& saddlePush = {}, float refuseM = 0.0f)
        {
            playhead += 16;
            wall += 16.0;
            const float before = y;
            y += 7.8f * 0.016f;
            detector.BeginTick(playhead, wall, 1.0, true);
            const auto observe = [&](std::uint32_t packed, std::uint32_t mount, float dy, float dz, const Vec3& push,
                                     std::uint32_t causes) {
                Observation o;
                o.packed = packed;
                o.mount = mount;
                o.placed = true;
                o.placedAt = {0.0f, before + dy, 238.0f + dz};
                o.hasEngine = true;
                o.engine = {push.x, before + dy + push.y, 238.0f + dz + push.z};
                o.recorded = {0.0f, y + dy, 238.0f + dz};
                o.playheadMs = playhead;
                o.causes = causes;
                o.frameEndRefuseM = refuseM;
                detector.Observe(o, findings);
            };
            observe(0x208cb, 0, 0.0f, 0.0f, horsePush, kCausePhysics);
            observe(0x008c9, 0x208cb, -0.03f, 0.36f, saddlePush, kCauseFrozen);
            observe(0x108ca, 0x208cb, -0.53f, 0.43f, {}, kCauseFrozen);
            detector.EndTick(findings);
        }
    };

    void TestTwoRidersOneMountMove()
    {
        TwoRiders t;
        for (int n = 0; n < 5; ++n)
            t.Tick({});
        Check(t.findings.empty(), "two riders galloping still: nothing");
        t.Tick({0.0f, 0.106f, 0.0f});
        Check(CountKind(t.findings, Kind::EngineMoved, 0x208cb) == 1, "035: the horse moved 10.6 cm");
        Check(CountKind(t.findings, Kind::MountOffset) == 1, "035: one mount_offset for the horse's move, not one per rider");
        const Finding* pair = nullptr;
        for (const Finding& f : t.findings)
            if (f.kind == Kind::MountOffset)
                pair = &f;
        Check(pair && pair->riders == 2 && pair->mount == 0x208cb && pair->engineM == 0.0f &&
                std::fabs(pair->magnitudeM - 0.106f) < 1e-3f,
            "035: riders=2, the horse, engine_m 0, the size of the move");
        Check(pair && pair->packed == 0x008c9, "equal offsets: the lower packed id names the finding");
        Check(t.detector.Totals().byKind[static_cast<int>(Kind::MountOffset)] == 1 &&
                t.detector.Totals().mountOffsetRiders == 2,
            "summary: one mount_offset, two riders behind it");
        // The saddle rider pushed on his own the same tick the horse moved:
        // his own finding, the passenger's offset the horse's.
        t.findings.clear();
        t.Tick({0.0f, 0.106f, 0.0f}, {0.0f, 0.0f, 0.12f});
        Check(CountKind(t.findings, Kind::MountOffset) == 2, "own move and mount move: two findings");
        std::uint32_t riders = 0;
        for (const Finding& f : t.findings)
            riders += f.kind == Kind::MountOffset ? f.riders : 0;
        Check(riders == 2, "one rider each");
        Check(CountKind(t.findings, Kind::MountOffset, 0x008c9) == 1 && CountKind(t.findings, Kind::MountOffset, 0x108ca) == 1,
            "the saddle rider's own, the passenger's from the horse");
    }

    void TestTwoRidersPlacedBack()
    {
        TwoRiders t;
        for (int n = 0; n < 5; ++n)
            t.Tick({}, {}, 25.0f);
        t.Tick({0.0f, 0.106f, 0.0f}, {}, 25.0f);
        Check(t.findings.empty(), "035 under FrameEnd: the move put back before the frame is drawn is no jerk");
        Check(t.detector.Totals().placedBack == 1, "the horse's engine_moved placed back (task 55)");
        Check(t.detector.Totals().pairPlacedBack == 1, "the pair offset placed back once, for two riders");
        // Past the placement's refuse limit it is still a finding.
        t.Tick({0.0f, 0.106f, 0.0f}, {}, 0.05f);
        Check(CountKind(t.findings, Kind::MountOffset) == 1 && CountKind(t.findings, Kind::EngineMoved) == 1,
            "past the refuse limit: still a jerk");
    }

    // GroupPairOffsets (pure).
    void TestGroupPairOffsets()
    {
        std::vector<PairGroupInput> offsets(5);
        offsets[0] = {0x30, 0x9, 0.08f, 0.0f, false};  // mount 9, from the mount
        offsets[1] = {0x10, 0x9, 0.10f, 0.01f, false}; // mount 9, from the mount (worst)
        offsets[2] = {0x20, 0x9, 0.15f, 0.15f, false}; // mount 9, its own move
        offsets[3] = {0x40, 0x5, 0.07f, 0.0f, true};   // mount 5, put back
        offsets[4] = {0x50, 0x5, 0.07f, 0.0f, true};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> found; // rider, riders
        int back = 0;
        GroupPairOffsets(
            offsets, 0.04f, [&](std::size_t n, std::uint32_t riders) { found.push_back({offsets[n].rider, riders}); },
            [&]() { ++back; });
        Check(found.size() == 2 && back == 1, "two findings, one placed back");
        Check(!found.empty() && found[0].first == 0x20 && found[0].second == 1, "the own move first, alone");
        Check(found.size() == 2 && found[1].first == 0x10 && found[1].second == 2, "mount 9: one finding, two riders, the worst");
        std::vector<PairGroupInput> none;
        GroupPairOffsets(none, 0.04f, [&](std::size_t, std::uint32_t) { back += 10; }, [&]() { back += 10; });
        Check(back == 1, "nothing in, nothing out");
    }
}

int RunPoseJerkTests()
{
    TestSaddle();
    TestPairs();
    TestSmoothGallop();
    TestRiderPushed();
    TestHeldAndCapped();
    TestPlacedBack();
    TestPoseStale();
    TestPauseForgets();
    TestHorsePushedRiderFrozen();
    TestTwoRidersPairs();
    TestTwoRidersOneMountMove();
    TestTwoRidersPlacedBack();
    TestGroupPairOffsets();
    std::printf("pose jerk tests: %s\n", g_jerkFailures == 0 ? "ok" : "FAILED");
    return g_jerkFailures;
}
