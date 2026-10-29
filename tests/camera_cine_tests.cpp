// The Spline blend, the shake with movement, the marker tools, the free
// camera's motion (Classic, Drone, acrobatic, Walk, collision, follow, pick)
// and the world overlay (path, projection) and CAMR v5.
#include "camera_overlay.h"
#include "camera_track.h"
#include "camera_marker_shake.h"
#include "clip_file.h"
#include "free_cam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    using namespace replay;
    using camera::Blend;
    using camera::CameraMode;
    using camera::Keyframe;
    using camera::Track;
    using editor::CameraState;
    using editor::Vec3;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL (camera-cine): %s\n", message);
        ++g_failures;
    }

    bool Near(float actual, float expected, float tolerance = 0.02f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    float Distance(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    Keyframe FreeAt(std::uint64_t t, const Vec3& at, const Vec3& rotation, Blend blend, float fov = 45.0f)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.pose = {at, Track::FromEulerDegrees(rotation), fov};
        k.blend = blend;
        k.ease = false; // linear progress: the path's own shape is measured
        return k;
    }

    Track Chain(Blend blend, const std::vector<std::uint64_t>& times, const std::vector<Vec3>& points)
    {
        Track track;
        for (std::size_t n = 0; n < times.size(); ++n)
            track.AddMarker(FreeAt(times[n], points[n], {0.0f, 0.0f, 0.0f}, blend));
        return track;
    }

    Vec3 At(const Track& track, std::uint64_t t, const camera::Scene& scene = {})
    {
        CameraState view{};
        camera::EvaluateMarkersRaw(track, t, scene, view);
        return view.position;
    }

    float SpeedAround(const Track& track, std::uint64_t t, std::int64_t side)
    {
        // Speed just before (side -1) or after (+1) t, metres per second.
        const std::uint64_t a = side < 0 ? t - 6 : t + 1;
        const std::uint64_t b = side < 0 ? t - 1 : t + 6;
        return Distance(At(track, a), At(track, b)) / (static_cast<float>(b - a) / 1000.0f);
    }

    // ---- 1. Spline: through every marker, speed carried through them.
    void TestSpline()
    {
        // Uneven times and a corner at the middle marker.
        const std::vector<std::uint64_t> times{0, 1000, 3000, 4000};
        const std::vector<Vec3> points{{0, 0, 0}, {10, 0, 0}, {10, 20, 0}, {0, 30, 0}};
        const Track spline = Chain(Blend::Spline, times, points);
        const Track linear = Chain(Blend::Linear, times, points);
        for (std::size_t n = 0; n < times.size(); ++n)
            Check(Distance(At(spline, times[n]), points[n]) < 0.01f, "spline: passes through each marker");
        // At 1000 the linear path turns a corner: the direction flips from
        // +x to +y, so the velocity jumps; the spline's speed carries on.
        const float splineBefore = SpeedAround(spline, 1000, -1), splineAfter = SpeedAround(spline, 1000, +1);
        Check(std::fabs(splineBefore - splineAfter) / std::max(splineBefore, splineAfter) < 0.08f,
            "spline: the speed is continuous through a marker");
        const Vec3 l0 = At(linear, 980), l1 = At(linear, 1000), l2 = At(linear, 1020);
        const Vec3 s0 = At(spline, 980), s1 = At(spline, 1000), s2 = At(spline, 1020);
        const auto turn = [](const Vec3& a, const Vec3& b, const Vec3& c) {
            const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - b.x, c.y - b.y, c.z - b.z};
            const float lu = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z), lv = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return std::acos(std::clamp((u.x * v.x + u.y * v.y + u.z * v.z) / (lu * lv), -1.0f, 1.0f)) * 57.2958f;
        };
        Check(turn(l0, l1, l2) > 60.0f, "linear: a corner at the marker (the old behaviour)");
        Check(turn(s0, s1, s2) < 8.0f, "spline: no corner at the marker");
        // Mixed: a Linear segment then a Spline one keeps working (no crash,
        // through the markers).
        Track mixed = Chain(Blend::Linear, times, points);
        Keyframe* second = mixed.FindMutable(mixed.Keyframes()[1].id);
        second->blend = Blend::Spline;
        Check(Distance(At(mixed, 3000), points[2]) < 0.01f, "mixed linear and spline: reaches the marker");
        // A recording cut between two markers: the curve does not use the
        // marker across it as a neighbour (and the segment over the cut holds).
        const std::vector<std::uint32_t> cuts{3500};
        camera::Scene cutScene;
        cutScene.cuts = &cuts;
        Check(Distance(At(spline, 3600, cutScene), points[2]) < 0.01f, "spline: holds over a recording cut");
    }

    // ---- 2. Spline rotation: the short way through 180 degrees, no gimbal.
    void TestSplineRotation()
    {
        Track track;
        track.AddMarker(FreeAt(0, {0, 0, 0}, {0.0f, 0.0f, 170.0f}, Blend::Spline));
        track.AddMarker(FreeAt(1000, {1, 0, 0}, {0.0f, 0.0f, -170.0f}, Blend::Spline));
        track.AddMarker(FreeAt(2000, {2, 0, 0}, {0.0f, 0.0f, -150.0f}, Blend::Spline));
        CameraState view{};
        camera::EvaluateMarkersRaw(track, 500, {}, view);
        const float heading = view.rotation.z;
        Check(std::fabs(std::fabs(heading) - 180.0f) < 4.0f, "spline rotation: through 180, not through 0");
        camera::EvaluateMarkersRaw(track, 1000, {}, view);
        Check(Near(view.rotation.z, -170.0f, 0.5f), "spline rotation: the marker's own heading at its time");
        // Looking almost straight down: still the marker's view at its time.
        Track down;
        down.AddMarker(FreeAt(0, {0, 0, 10}, {-85.0f, 0.0f, 30.0f}, Blend::Spline));
        down.AddMarker(FreeAt(1000, {5, 0, 10}, {-80.0f, 0.0f, 60.0f}, Blend::Spline));
        down.AddMarker(FreeAt(2000, {9, 0, 10}, {-75.0f, 0.0f, 90.0f}, Blend::Spline));
        camera::EvaluateMarkersRaw(down, 1000, {}, view);
        Check(Near(view.rotation.x, -80.0f, 0.5f) && Near(view.rotation.z, 60.0f, 0.5f), "spline rotation: steep view kept");
    }

    // ---- 3. Options: Spline and the shake motion through ApplyOptions.
    void TestOptions()
    {
        Keyframe k = FreeAt(0, {0, 0, 0}, {0, 0, 0}, Blend::None);
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionBlend;
        o.blend = Blend::Spline;
        Check(camera::ApplyOptions(k, o) && k.blend == Blend::Spline, "options: Spline accepted");
        o.blend = static_cast<Blend>(4);
        Check(!camera::ApplyOptions(k, o) && k.blend == Blend::Spline, "options: blend 4 refused");
        // "with movement" is the marker shake's speed coupling
        // (shake_motion=still).
        editor::MarkerShake s = editor::DefaultMarkerShake();
        bool known = false;
        std::string error;
        Check(camera::mshake::ParseField("shake", "handheld", s, known, error) &&
                  camera::mshake::ParseField("shake_motion", "still", s, known, error) && camera::SetShake(k, s) &&
                  k.shake.speedAmp == 1.0f && k.shake.still == 1 && k.shake.speedRef == camera::kShakeMotionFullSpeed,
            "options: shake with movement");
        editor::CameraKeyframe snap{};
        camera::FillSnapshotKeyframe(k, snap);
        Check(snap.shake == editor::ShakeType::Handheld && snap.blend == Blend::Spline, "snapshot: the shake's summary and Spline");
    }

    // ---- 4. Shake with movement.
    void TestShakeMotion()
    {
        Track still;
        still.AddMarker(FreeAt(0, {0, 0, 0}, {0, 0, 0}, Blend::Linear));
        still.AddMarker(FreeAt(2000, {0, 0, 0}, {0, 0, 0}, Blend::Linear));
        Check(Near(camera::ShakeMotionGain(still, 1000, {}, editor::ShakeMotion::Off), 1.0f), "shake motion: off is 1");
        Check(Near(camera::ShakeMotionGain(still, 1000, {}, editor::ShakeMotion::Speed), 1.0f), "shake motion: still camera, gain 1");
        Check(Near(camera::ShakeMotionGain(still, 1000, {}, editor::ShakeMotion::SpeedStill), 0.0f), "shake motion: still camera, gone");
        Track moving;
        moving.AddMarker(FreeAt(0, {0, 0, 0}, {0, 0, 0}, Blend::Linear));
        moving.AddMarker(FreeAt(2000, {30, 0, 0}, {0, 0, 0}, Blend::Linear)); // 15 m/s
        Check(camera::ShakeMotionGain(moving, 1000, {}, editor::ShakeMotion::Speed) > 1.9f, "shake motion: full speed doubles");
        Check(camera::ShakeMotionGain(moving, 1000, {}, editor::ShakeMotion::SpeedStill) > 1.9f, "shake motion: moving keeps it");
        // Deterministic: the same at the same time.
        Check(camera::ShakeMotionGain(moving, 777, {}, editor::ShakeMotion::SpeedStill) ==
                  camera::ShakeMotionGain(moving, 777, {}, editor::ShakeMotion::SpeedStill),
            "shake motion: scrub equals play");
    }

    // ---- 5. Marker tools.
    void TestTools()
    {
        // A move of four markers (all Spline) and a lone one after it.
        Track track = Chain(Blend::Spline, {0, 1000, 1500, 6000}, {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}});
        track.FindMutable(track.Keyframes()[3].id)->blend = Blend::None;
        track.AddMarker(FreeAt(8000, {40, 0, 0}, {0, 0, 0}, Blend::None));
        const std::vector<Keyframe>& keys = track.Keyframes();
        const camera::MoveRange range = camera::MoveOf(track, keys[1].id);
        Check(range.first == 0 && range.last == 3, "move: the run of blends");
        Check(camera::MoveOf(track, keys[4].id).first == 4 && camera::MoveOf(track, keys[4].id).last == 4, "move: a lone marker");

        // Distribute by distance: 10 m each, so 2000 ms each.
        Track even = track;
        Check(camera::DistributeMove(even, even.Keyframes()[1].id, true, {}), "distribute: done");
        Check(even.Keyframes()[1].timeMs == 2000 && even.Keyframes()[2].timeMs == 4000 && even.Keyframes()[3].timeMs == 6000,
            "distribute: constant speed by distance");
        Track byTime = track;
        Check(camera::DistributeMove(byTime, byTime.Keyframes()[0].id, false, {}) && byTime.Keyframes()[1].timeMs == 2000,
            "distribute: even times");
        Track tooShort = Chain(Blend::Spline, {0, 1000}, {{0, 0, 0}, {1, 0, 0}});
        Check(!camera::DistributeMove(tooShort, tooShort.Keyframes()[0].id, true, {}), "distribute: needs three markers");

        // Stretch 50 %: the offsets from the move's start halve; the marker
        // after the move keeps its time.
        Track stretched = track;
        Check(camera::StretchMove(stretched, stretched.Keyframes()[2].id, 50, 20000, {}), "stretch: done");
        Check(stretched.Keyframes()[1].timeMs == 500 && stretched.Keyframes()[3].timeMs == 3000 &&
                  stretched.Keyframes()[4].timeMs == 8000,
            "stretch: halves the move, keeps the next marker");
        // 200 %: the move would end at 12000, past the next marker (8000):
        // that one is pushed only as far as the spacing asks.
        Track longer = track;
        Check(camera::StretchMove(longer, longer.Keyframes()[0].id, 200, 20000, {}), "stretch 200: done");
        Check(longer.Keyframes()[3].timeMs == 12000 && longer.Keyframes()[4].timeMs > 12000 &&
                  longer.Keyframes()[4].timeMs <= 12000 + camera::MarkerSpacingMs(longer.Keyframes()[3].speedIndex),
            "stretch 200: pushes the next marker by the spacing only");
        Track limited = track;
        Check(!camera::StretchMove(limited, limited.Keyframes()[0].id, 200, 10000, {}) &&
                  limited.Keyframes()[3].timeMs == 6000,
            "stretch: refused past the clip, nothing changed");
        // A move that ends on the clip's end marker cannot stretch.
        Track bound = Chain(Blend::Spline, {0, 1000, 2000}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}});
        Check(!camera::StretchMove(bound, bound.Keyframes()[0].id, 150, 20000, {}), "stretch: not over the clip's end marker");

        // Duplicate: halfway to the next marker; after the last one 2 s on.
        Track dup = track;
        const std::uint32_t copy = camera::DuplicateMarker(dup, dup.Keyframes()[0].id);
        const Keyframe* added = dup.Find(copy);
        Check(copy != 0 && added && added->timeMs == 500 && Distance(added->pose.position, Vec3{0, 0, 0}) < 0.001f,
            "duplicate: halfway, same view");
        Track one;
        one.AddMarker(FreeAt(1000, {0, 0, 0}, {0, 0, 0}, Blend::None));
        const std::uint32_t after = camera::DuplicateMarker(one, one.Keyframes()[0].id, 10000);
        Check(after != 0 && one.Find(after)->timeMs == 3000, "duplicate: the last one, 2 s on");
        Check(camera::DuplicateMarker(one, one.Keyframes()[0].id, 1500) == 0 || one.Keyframes().size() == 3,
            "duplicate: refused past the clip");

        // Lock: the markers ride a moving target, each view kept; bake back.
        camera::Scene scene;
        scene.lookup = [](std::uint32_t id, std::uint64_t t, camera::EntityState& s) {
            if (id != 77)
                return false;
            s.position = {static_cast<float>(t) / 100.0f, 5.0f, 0.0f}; // 10 m/s along x
            s.heading = static_cast<float>(t) / 100.0f;                // turning
            return true;
        };
        Track locked = track;
        std::vector<CameraState> before;
        for (const Keyframe& k : locked.Keyframes())
        {
            const camera::LivePose live = camera::Resolve(k, k.timeMs, scene);
            before.push_back({live.pose.position, Track::ToEulerDegrees(live.pose.orientation), live.pose.fov});
        }
        const int changed = camera::LockMarkers(locked, locked.Keyframes()[0].id, true, 77, camera::AttachType::Rigid, scene);
        Check(changed == 5, "lock: every Free marker");
        bool kept = true;
        for (std::size_t n = 0; n < locked.Keyframes().size(); ++n)
        {
            const Keyframe& k = locked.Keyframes()[n];
            const camera::LivePose live = camera::Resolve(k, k.timeMs, scene);
            kept = kept && k.attachTarget == 77 && Distance(live.pose.position, before[n].position) < 0.01f;
        }
        Check(kept, "lock: each marker keeps its view at its time");
        const int baked = camera::LockMarkers(locked, locked.Keyframes()[1].id, false, 0, camera::AttachType::Default, scene);
        Check(baked == 4 && locked.Keyframes()[0].attachTarget == 0 && locked.Keyframes()[4].attachTarget == 77,
            "bake: the move back to world poses");
        Check(Distance(camera::Resolve(locked.Keyframes()[1], locked.Keyframes()[1].timeMs, scene).pose.position,
                  before[1].position) < 0.01f,
            "bake: the view kept");
    }

    // ---- 6. Free camera.
    void TestFreeCamRotation()
    {
        using namespace freecam;
        // The orientation's forward is the engine's direction.
        for (float p : {-60.0f, 0.0f, 35.0f})
            for (float h : {-120.0f, 0.0f, 45.0f, 170.0f})
            {
                const Vec3 f = Forward(FromRotation({p, 0.0f, h}));
                const float pr = p * 0.0174533f, hr = h * 0.0174533f;
                Check(Near(f.x, -std::sin(hr) * std::cos(pr), 0.001f) && Near(f.y, std::cos(hr) * std::cos(pr), 0.001f) &&
                          Near(f.z, std::sin(pr), 0.001f),
                    "freecam: forward as RotationToDirection");
                const Vec3 back = ToRotation(FromRotation({p, 12.0f, h}));
                Check(Near(back.x, p, 0.01f) && Near(back.y, 12.0f, 0.01f) && Near(back.z, h, 0.01f), "freecam: rotation round trip");
            }
        // Straight up: the matrix survives the decomposition.
        const Quat up = FromRotation({90.0f, 20.0f, 30.0f});
        const Vec3 r = ToRotation(up, 20.0f);
        const Vec3 f1 = Forward(up), f2 = Forward(FromRotation(r));
        const Vec3 u1 = Up(up), u2 = Up(FromRotation(r));
        Check(Distance(f1, f2) < 0.01f && Distance(u1, u2) < 0.01f, "freecam: gimbal, the same view");
    }

    void TestFreeCamClassic()
    {
        using namespace freecam;
        // A Classic flight, replayed by hand: the same numbers.
        Settings settings;
        State state;
        state.SetView({0, 0, 0}, {10.0f, 0.0f, 30.0f}, 45.0f);
        Vec3 position{0, 0, 0}, rotation{10.0f, 0.0f, 30.0f}, velocity{0, 0, 0};
        float smoothX = 0.0f, smoothY = 0.0f, fov = 45.0f;
        for (int frame = 0; frame < 120; ++frame)
        {
            const float dt = 1.0f / 60.0f;
            Input input;
            input.mouseX = frame < 30 ? 0.02f : 0.0f;
            input.mouseY = frame < 20 ? -0.01f : 0.0f;
            input.moveForward = frame < 90 ? 1.0f : 0.0f;
            input.moveRight = frame > 40 && frame < 60 ? 1.0f : 0.0f;
            input.turnX = frame > 70 && frame < 80 ? -1.0f : 0.0f; // the left arrow
            input.roll = frame > 80 && frame < 85 ? 1.0f : 0.0f;
            input.zoom = frame > 100 ? 1.0f : 0.0f;
            input.fast = frame > 50;
            Step(state, input, settings, dt, nullptr);
            // By hand: the Classic free camera update.
            const float lookBlend = 1.0f - std::exp(-18.0f * dt);
            smoothX += (input.mouseX - smoothX) * lookBlend;
            smoothY += (input.mouseY - smoothY) * lookBlend;
            rotation.z -= smoothX * 10.0f;
            rotation.x -= smoothY * 10.0f;
            if (input.turnX < 0) rotation.z += 75.0f * dt;
            if (input.roll > 0) rotation.y += 75.0f * dt * 0.5f;
            rotation.x = std::clamp(rotation.x, -89.0f, 89.0f);
            const float pr = rotation.x * 0.0174533f, hr = rotation.z * 0.0174533f;
            const Vec3 forward{-std::sin(hr) * std::fabs(std::cos(pr)), std::cos(hr) * std::fabs(std::cos(pr)), std::sin(pr)};
            const Vec3 right{std::cos(hr), std::sin(hr), 0.0f};
            Vec3 d{forward.x * input.moveForward + right.x * input.moveRight, forward.y * input.moveForward + right.y * input.moveRight,
                   forward.z * input.moveForward};
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len > 0.0001f)
                d = {d.x / len, d.y / len, d.z / len};
            const float speed = input.fast ? 15.0f : 5.0f;
            const float moveBlend = 1.0f - std::exp(-7.0f * dt);
            velocity = {velocity.x + (d.x * speed - velocity.x) * moveBlend, velocity.y + (d.y * speed - velocity.y) * moveBlend,
                        velocity.z + (d.z * speed - velocity.z) * moveBlend};
            position = {position.x + velocity.x * dt, position.y + velocity.y * dt, position.z + velocity.z * dt};
            if (input.zoom > 0) fov -= 30.0f * dt;
            fov = std::clamp(fov, editor::kMarkerFovMin, editor::kMarkerFovMax);
        }
        Check(Distance(state.position, position) < 0.001f, "freecam classic: the same position as the original free camera");
        Check(Distance(state.rotation, rotation) < 0.001f, "freecam classic: the same rotation");
        Check(Near(state.fov, fov, 0.001f), "freecam classic: the same FOV");
    }

    void TestFreeCamDrone()
    {
        using namespace freecam;
        Settings settings;
        settings.motion = Motion::Drone;
        State state;
        state.SetView({0, 0, 0}, {0, 0, 0}, 45.0f);
        Input thrust;
        thrust.moveForward = 1.0f;
        for (int n = 0; n < 600; ++n)
            Step(state, thrust, settings, 1.0f / 60.0f, nullptr);
        const float terminal = settings.droneAcceleration * 2.0f / settings.droneDrag;
        Check(Near(state.velocity.y, terminal, 0.5f), "drone: terminal speed acceleration / drag");
        Input none;
        const float before = state.velocity.y;
        Step(state, none, settings, 0.5f, nullptr);
        Check(state.velocity.y > 0.1f && state.velocity.y < before, "drone: coasts and slows after the thrust");
        // A left turn (the mouse to the left) banks to the left: negative roll.
        State turn;
        turn.SetView({0, 0, 0}, {0, 0, 0}, 45.0f);
        Input left;
        left.mouseX = -0.02f;
        for (int n = 0; n < 60; ++n)
            Step(turn, left, settings, 1.0f / 60.0f, nullptr);
        Check(turn.rotation.z > 8.0f && turn.rotation.y < -1.0f, "drone: banks into a left turn");
        for (int n = 0; n < 240; ++n)
            Step(turn, none, settings, 1.0f / 60.0f, nullptr);
        Check(std::fabs(turn.rotation.y) < 0.2f, "drone: levels after the turn");
        // The zoom eases in.
        State zoom;
        zoom.SetView({0, 0, 0}, {0, 0, 0}, 45.0f);
        Input in;
        in.zoom = 1.0f;
        Step(zoom, in, settings, 1.0f / 60.0f, nullptr);
        Check(zoom.fov > 45.0f - 0.5f * 30.0f / 60.0f, "drone: the zoom starts gently");
    }

    void TestFreeCamWorld()
    {
        using namespace freecam;
        // Collision against the ground plane z = 0.
        Settings settings;
        settings.collision = true;
        World world;
        world.sweep = [](const Vec3& from, const Vec3& to, float radius, Vec3& hit, Vec3& normal) {
            if (to.z - radius > 0.0f)
                return false;
            const float u = from.z == to.z ? 0.0f : (from.z - radius) / (from.z - to.z);
            hit = {from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u, 0.0f};
            normal = {0.0f, 0.0f, 1.0f};
            return true;
        };
        State state;
        state.SetView({0, 0, 2}, {-45.0f, 0, 0}, 45.0f);
        Input down;
        down.moveForward = 1.0f;
        for (int n = 0; n < 300; ++n)
            Step(state, down, settings, 1.0f / 60.0f, &world);
        Check(state.position.z >= settings.collisionRadius - 0.01f && state.position.y > 2.0f, "collision: slides along the ground");
        // Walk: eye height over the ground.
        Settings walk;
        walk.walk = true;
        World ground;
        ground.ground = [](float, float, float, float& z) {
            z = 10.0f;
            return true;
        };
        State walker;
        walker.SetView({0, 0, 40}, {0, 0, 0}, 45.0f);
        Input forward;
        forward.moveForward = 1.0f;
        Step(walker, forward, walk, 1.0f / 60.0f, &ground);
        Check(Near(walker.position.z, 11.7f, 0.001f), "walk: eye height over the ground");
        // Locked altitude: looking down, forward stays level.
        Settings level;
        level.lockAltitude = true;
        State flat;
        flat.SetView({0, 0, 5}, {-60.0f, 0, 0}, 45.0f);
        for (int n = 0; n < 60; ++n)
            Step(flat, forward, level, 1.0f / 60.0f, nullptr);
        Check(Near(flat.position.z, 5.0f, 0.0001f) && flat.position.y > 1.0f, "lock altitude: level flight");
        // Acrobatic: a full loop on the pitch keeps flying (no stop at 89).
        Settings acro;
        acro.acrobatic = true;
        State loop;
        loop.SetView({0, 0, 0}, {0, 0, 0}, 45.0f);
        Input pitchUp;
        pitchUp.turnY = 1.0f;
        for (int n = 0; n < 288; ++n) // 75 deg/s for 4.8 s: 360 degrees
            Step(loop, pitchUp, acro, 1.0f / 60.0f, nullptr);
        const Vec3 f = Forward(loop.orientation);
        Check(Near(f.y, 1.0f, 0.01f), "acrobatic: a full loop comes back level");
        Settings plain;
        State clamp;
        clamp.SetView({0, 0, 0}, {0, 0, 0}, 45.0f);
        for (int n = 0; n < 288; ++n)
            Step(clamp, pitchUp, plain, 1.0f / 60.0f, nullptr);
        Check(Near(clamp.rotation.x, 89.0f, 0.01f), "classic: the pitch stops at 89");
    }

    // Edit Camera flies level (LevelFlight): the user's acrobatic Drone with
    // banking would roll the horizon; the marker's roll stays as set.
    void TestFreeCamEditLevel()
    {
        using namespace freecam;
        Settings user;
        user.motion = Motion::Drone;
        user.acrobatic = true;
        user.droneBanking = 15.0f;
        Input turn; // pitch up and turn right together (the mouse and the arrows)
        turn.turnX = 1.0f;
        turn.turnY = 1.0f;
        turn.mouseX = 0.02f;
        // Without LevelFlight the horizon rolls (the bug).
        State free;
        free.SetView({0, 0, 2}, {10.0f, 0.0f, 30.0f}, 45.0f);
        for (int n = 0; n < 60; ++n)
            Step(free, turn, user, 1.0f / 60.0f, nullptr);
        Check(std::fabs(free.rotation.y) > 1.0f, "the acrobatic Drone rolls the horizon");
        // Edit Camera: the same input, the marker's roll (0 and 7) kept.
        const Settings edit = LevelFlight(user);
        Check(!edit.acrobatic && edit.droneBanking == 0.0f && edit.motion == Motion::Drone, "LevelFlight: no acrobatic, no banking, Drone kept");
        Check(LevelFlight(user).acrobatic == false && user.acrobatic, "LevelFlight leaves the user's settings alone");
        for (const float roll : {0.0f, 7.0f})
        {
            State marker;
            marker.SetView({0, 0, 2}, {10.0f, roll, 30.0f}, 45.0f);
            for (int n = 0; n < 120; ++n)
                Step(marker, turn, edit, 1.0f / 60.0f, nullptr);
            Check(Near(marker.rotation.y, roll, 0.001f), "Edit Camera: the marker's roll stays while it turns");
            Check(marker.rotation.z > 30.0f + 1.0f || marker.rotation.z < 30.0f - 1.0f, "Edit Camera: it still turns");
        }
        // A leftover roll from an acrobatic flight is gone once the view is set
        // from the marker (SetView), and a Rigid follow keeps the roll.
        free.SetView({0, 0, 2}, {10.0f, 3.0f, 30.0f}, 45.0f);
        Check(free.bankRoll == 0.0f && !free.orientationValid, "SetView: no banking or orientation left over");
        State rigid;
        rigid.SetView({0, -5, 2}, {-10.0f, 2.0f, 0.0f}, 45.0f);
        ApplyFollow(rigid, Follow::Rigid, TargetFrame{{0, 0, 0}, {0, 0, 0}}, TargetFrame{{0, 0, 0}, {0, 20.0f, 45.0f}});
        KeepRoll(rigid, 2.0f);
        Check(Near(rigid.rotation.y, 2.0f, 0.001f) && Near(ToRotation(rigid.orientation, 2.0f).y, 2.0f, 0.01f),
            "KeepRoll: a Rigid follow does not roll the marker");
    }

    void TestFreeCamFollowPick()
    {
        using namespace freecam;
        State state;
        state.SetView({0, -5, 2}, {-10.0f, 0.0f, 0.0f}, 45.0f);
        TargetFrame a{{0, 0, 0}, {0, 0, 0}};
        TargetFrame b{{10, 0, 0}, {0, 0, 90}}; // moved 10 m and turned left 90 degrees
        State translated = state;
        ApplyFollow(translated, Follow::Translate, a, b);
        Check(Distance(translated.position, Vec3{10, -5, 2}) < 0.001f && Near(translated.rotation.z, 0.0f, 0.001f),
            "follow translate: moves, keeps the rotation");
        State rigid = state;
        ApplyFollow(rigid, Follow::Rigid, a, b);
        // Behind the target (-Y) turned 90 left is +X behind... the offset (0,-5) turns to (5,0).
        Check(Distance(rigid.position, Vec3{15, 0, 2}) < 0.01f && Near(rigid.rotation.z, 90.0f, 0.01f),
            "follow rigid: orbits and turns with the target");
        std::vector<Candidate> candidates{{1, {0, 20, 0}}, {2, {3, 10, 0}}, {3, {0, 0, 50}}};
        Check(PickAimed({0, 0, 0}, {0, 0, 0}, candidates) == 1, "pick: the one at the centre");
        Check(PickAimed({0, 0, 0}, {0, 0, 0}, candidates, 1.0f) == 1, "pick: inside a narrow cone");
        Check(PickAimed({0, 0, 0}, {0, 0, 180.0f}, candidates) == 0, "pick: nothing behind");
        Check(PickNearest({0, 0, 0}, candidates) == 2, "pick: the nearest");
        State behind;
        ShotBehind(behind, {{0, 0, 0}, {0, 0, 0}}, nullptr);
        Check(Distance(behind.position, Vec3{0, -3, 1.2f}) < 0.001f && Near(behind.rotation.z, 0.0f, 0.01f) &&
                  behind.rotation.x < 0.0f,
            "shot behind: behind, looking at the character");
        State tilted;
        tilted.SetView({0, 0, 0}, {20.0f, 30.0f, 40.0f}, 45.0f);
        LevelHorizon(tilted);
        Check(Near(tilted.rotation.y, 0.0f) && Near(tilted.rotation.x, 20.0f, 0.01f), "level horizon: roll 0, pitch kept");
    }

    // ---- 7. Overlay.
    void TestOverlay()
    {
        Track track = Chain(Blend::Spline, {0, 1000, 2000}, {{0, 10, 0}, {5, 10, 0}, {10, 10, 0}});
        track.AddMarker(FreeAt(4000, {20, 10, 0}, {0, 0, 0}, Blend::None)); // a cut after 2000: no line to it
        track.FindMutable(track.Keyframes()[2].id)->blend = Blend::None;
        const camera::overlay::WorldPath path = camera::overlay::BuildPath(track, {});
        Check(path.markers.size() == 4, "overlay: every marker");
        bool inRange = !path.points.empty();
        for (const auto& p : path.points)
            inRange = inRange && p.timeMs <= 2000;
        Check(inRange, "overlay: the path only where it blends");
        Check(Distance(path.points.back().at, Vec3{10, 10, 0}) < 0.01f, "overlay: the path ends on the marker");
        // A point straight ahead is the screen's centre.
        CameraState camera{{0, 0, 0}, {0, 0, 0}, 45.0f};
        editor::Vec2 a{}, b{};
        Check(camera::overlay::ProjectSegment(camera, {0, 10, 0}, {0, 10, 1}, a, b) && Near(a.x, 0.0f, 0.0001f) &&
                  Near(a.y, 0.0f, 0.0001f) && b.y > 0.0f,
            "overlay: projection centre and up");
        // A segment from behind to ahead is clipped at the near plane.
        Check(camera::overlay::ProjectSegment(camera, {0, -5, 0}, {0, 10, 0}, a, b), "overlay: clipped, drawn");
        Check(!camera::overlay::ProjectSegment(camera, {0, -5, 0}, {0, -10, 0}, a, b), "overlay: behind, not drawn");
        // The same convention as camera::ProjectToScreen.
        CameraState turned{{1, 2, 3}, {10.0f, 5.0f, 40.0f}, 50.0f};
        editor::Vec2 mine{}, theirs{};
        camera::overlay::ProjectSegment(turned, {-8, 12, 6}, {-8, 12, 7}, mine, b);
        camera::ProjectToScreen(turned, {-8, 12, 6}, 1.6f, theirs);
        Check(Near(mine.x, theirs.x * 1.6f, 0.0005f) && Near(mine.y, theirs.y, 0.0005f), "overlay: projection as ProjectToScreen");
        // Into a frame.
        static overlaychannel::Frame frame;
        frame.lineCount = frame.dotCount = frame.labelCount = 0;
        camera::overlay::ProjectSettings settings;
        settings.playheadMs = 1500;
        settings.labelEveryMs = 500;
        const auto report = camera::overlay::Project(path, {}, {{10, -10, 2}, {-5.0f, 0, 0}, 60.0f}, settings, frame);
        Check(report.lines > 10 && report.dots == 4 && report.labels >= 4, "overlay: lines, dots and labels");
        std::uint32_t past = 0;
        for (std::uint32_t n = 0; n < frame.lineCount; ++n)
            past += (frame.lines[n].flags & overlaychannel::FlagPast) ? 1 : 0;
        Check(past > 0 && past < report.lines, "overlay: the path before the playhead marked");
        // A trail from the lookup.
        const auto trail = camera::overlay::BuildTrail(
            [](std::uint32_t, std::uint64_t t, camera::EntityState& s) {
                if (t > 2000 && t < 2500)
                    return false;
                s.position = {static_cast<float>(t) / 1000.0f, 0.0f, 0.0f};
                return true;
            },
            9, 0, 4000, 100);
        std::size_t breaks = 0;
        for (const auto& p : trail)
            breaks += p.breakBefore ? 1 : 0;
        Check(breaks == 2, "trail: a gap where the target is missing");
    }

    // ---- 8. CAMR v5.
    void TestClipV5()
    {
        Track track = Chain(Blend::Spline, {0, 1000, 2000}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}});
        editor::MarkerShake s = editor::DefaultMarkerShake();
        s.on = 1;
        s.speedAmp = 1.5f;
        camera::SetShake(*track.FindMutable(track.Keyframes()[1].id), s);
        Check(clipfile::CameraSectionVersion() >= 5, "CAMR: version 5 or later");
        const std::vector<std::uint8_t> bytes = clipfile::EncodeCameraTrack(track);
        Track back;
        std::string error;
        Check(clipfile::DecodeCameraTrack(bytes.data(), bytes.size(), clipfile::CameraSectionVersion(), back, error), "CAMR: reads back");
        Check(back.Keyframes().size() == 3 && back.Keyframes()[0].blend == Blend::Spline &&
                  back.Keyframes()[1].shake.on && back.Keyframes()[1].shake.speedAmp == 1.5f,
            "CAMR v5: Spline and the shake kept");
        Check(!clipfile::DecodeCameraTrack(bytes.data(), bytes.size(), 4, back, error), "CAMR: a current payload is not read as v4");
    }
}

int RunCameraCineTests()
{
    TestSpline();
    TestSplineRotation();
    TestOptions();
    TestShakeMotion();
    TestTools();
    TestFreeCamRotation();
    TestFreeCamClassic();
    TestFreeCamDrone();
    TestFreeCamWorld();
    TestFreeCamFollowPick();
    TestFreeCamEditLevel();
    TestOverlay();
    TestClipV5();
    if (g_failures == 0)
        std::puts("camera-cine (spline, shake motion, tools, free camera, overlay, CAMR v5) tests passed");
    return g_failures;
}
