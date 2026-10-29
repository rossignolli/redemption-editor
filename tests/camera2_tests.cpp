// Camera: the DOF interpolated inside a blend, the Auto focus hysteresis and
// held distance, the focus distance spring, the target from its recorded
// root, the Smooth springs re-expressed on an anchor change and the
// streaming ahead of a cut far away.
#include "camera_track.h"
#include "pose_math.h"

#include <cmath>
#include <algorithm>
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
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float actual, float expected, float tolerance = 0.02f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    Keyframe FreeAt(std::uint64_t t, const CameraState& view, Blend blend = Blend::None)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.pose = {view.position, Track::FromEulerDegrees(view.rotation), view.fov > 0.0f ? view.fov : 45.0f};
        k.blend = blend;
        return k;
    }

    Keyframe WithDof(Keyframe k, camera::DofMode dof, float intensity, float manualMeters)
    {
        k.dof = dof;
        k.dofIntensity = intensity;
        k.focus = camera::FocusMode::Manual;
        k.focusDistance = manualMeters;
        return k;
    }

    // ---- 1. DOF interpolated inside a blend.
    void TestDofBlend()
    {
        const CameraState view{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        Track track;
        track.AddMarker(WithDof(FreeAt(0, view, Blend::Linear), camera::DofMode::Custom, 0.0f, 2.0f));
        track.AddMarker(WithDof(FreeAt(1000, view, Blend::Linear), camera::DofMode::Custom, 100.0f, 20.0f));
        track.AddMarker(FreeAt(2000, view)); // the end: the segment from 1000 uses 1000's DOF
        const camera::Scene scene;
        camera::FocusConfig config;
        camera::FocusMemory memory;

        camera::SegmentPlan plan = camera::Plan(track, 500);
        Check(plan.blending && plan.dofIndex == 0 && plan.dofNextIndex == 1, "dof blend: the plan names the DOF it arrives at");
        camera::BlendedFocus mixed = camera::BlendFocus(track, plan, view, scene, 500, &memory, config);
        const float t = plan.phase;
        Check(mixed.blended && Near(mixed.phase, t, 1e-5f), "dof blend: interpolated with the blend's eased phase");
        Check(Near(mixed.plan.distance, 2.0f + (20.0f - 2.0f) * t, 1e-3f), "dof blend: the focus distance linearly");
        const float fstop = std::exp(std::log(22.0f) + (std::log(0.5f) - std::log(22.0f)) * t);
        Check(Near(mixed.plan.fstop, fstop, 1e-3f), "dof blend: the f-stop in log space (the intensity's scale)");

        // Continuity: the start is the first marker, the arrival the second.
        plan = camera::Plan(track, 0);
        mixed = camera::BlendFocus(track, plan, view, scene, 0, &memory, config);
        Check(Near(mixed.plan.distance, 2.0f, 1e-3f) && Near(mixed.plan.fstop, 22.0f, 1e-3f), "dof blend: phase 0 is the source marker");
        plan = camera::Plan(track, 999);
        const camera::BlendedFocus before = camera::BlendFocus(track, plan, view, scene, 999, &memory, config);
        plan = camera::Plan(track, 1000);
        const camera::BlendedFocus after = camera::BlendFocus(track, plan, view, scene, 1000, &memory, config);
        Check(plan.dofNextIndex == 1 && !after.blended, "dof blend: the last segment keeps its own DOF (the end marker uses it)");
        Check(Near(before.plan.distance, after.plan.distance, 0.1f) && Near(before.plan.fstop, after.plan.fstop, 0.05f),
            "dof blend: no step at the marker the blend arrives at");

        // ReplayCamDofBlend=0: the DOF jumps at the marker.
        camera::FocusConfig off;
        off.blend = false;
        plan = camera::Plan(track, 500);
        mixed = camera::BlendFocus(track, plan, view, scene, 500, &memory, off);
        Check(!mixed.blended && Near(mixed.plan.distance, 2.0f, 1e-3f), "dof blend off: the source marker's DOF until the marker");

        // Default into Custom: f/22 at the other side's distance, then the
        // Custom marker's DOF.
        Track fromDefault;
        fromDefault.AddMarker(FreeAt(0, view, Blend::Linear));
        fromDefault.AddMarker(WithDof(FreeAt(1000, view, Blend::Linear), camera::DofMode::Custom, 100.0f, 6.0f));
        fromDefault.AddMarker(FreeAt(2000, view));
        plan = camera::Plan(fromDefault, 500);
        mixed = camera::BlendFocus(fromDefault, plan, view, scene, 500, &memory, config);
        const float fstopD = std::exp(std::log(22.0f) + (std::log(0.5f) - std::log(22.0f)) * plan.phase);
        Check(mixed.blended && mixed.plan.source == camera::FocusSource::Manual && Near(mixed.plan.distance, 6.0f, 1e-3f) &&
                Near(mixed.plan.fstop, fstopD, 1e-3f),
            "dof blend: Default into Custom opens the aperture from f/22 at the Custom distance");
        plan = camera::Plan(fromDefault, 0);
        mixed = camera::BlendFocus(fromDefault, plan, view, scene, 0, &memory, config);
        Check(Near(mixed.plan.fstop, 22.0f, 1e-3f), "dof blend: Default side is f/22 (sharp) at the blend's start");

        // Both Default: nothing to send.
        Track plain;
        plain.AddMarker(FreeAt(0, view, Blend::Linear));
        plain.AddMarker(FreeAt(1000, view, Blend::Linear));
        plain.AddMarker(FreeAt(2000, view));
        plan = camera::Plan(plain, 500);
        mixed = camera::BlendFocus(plain, plan, view, scene, 500, &memory, config);
        Check(!mixed.blended && mixed.plan.source == camera::FocusSource::Default, "dof blend: Default to Default stays Default");

        // A blend cancelled by a recorded cut does not interpolate.
        std::vector<std::uint32_t> cuts{400};
        camera::Scene withCut;
        withCut.cuts = &cuts;
        CameraState out{};
        camera::EvaluateMarkers(track, 500, withCut, out, nullptr, 0.0f, &plan);
        mixed = camera::BlendFocus(track, plan, view, withCut, 500, &memory, config);
        Check(!plan.blending && plan.dofNextIndex == -1 && !mixed.blended, "dof blend: none across a recorded cut");
    }

    // ---- 2. Auto focus hysteresis on the main character.
    constexpr std::uint32_t kMain = 0x00010001;
    float g_mainX = 0.0f;
    bool g_mainThere = true;

    camera::Scene MainScene()
    {
        camera::Scene scene;
        scene.mainCharacter = kMain;
        scene.aspect = 16.0f / 9.0f;
        scene.lookup = [](std::uint32_t id, std::uint64_t, camera::EntityState& state) {
            if (id != kMain || !g_mainThere)
                return false;
            state.position = {g_mainX, 10.0f, 0.0f};
            state.hasHead = true;
            state.head = {g_mainX, 10.0f, 1.0f};
            return true;
        };
        return scene;
    }

    // The world x that puts the head at screen x `sx` for the camera at the
    // origin looking along +Y.
    float WorldXFor(float sx, const CameraState& camera, const camera::Scene& scene)
    {
        float lo = -100.0f, hi = 100.0f;
        for (int n = 0; n < 60; ++n)
        {
            const float mid = 0.5f * (lo + hi);
            editor::Vec2 screen{};
            camera::ProjectToScreen(camera, {mid, 10.0f, 1.0f}, scene.aspect, screen);
            (screen.x < sx ? lo : hi) = mid;
        }
        return 0.5f * (lo + hi);
    }

    void TestFocusHysteresis()
    {
        const CameraState view{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        const camera::Scene scene = MainScene();
        Keyframe k = FreeAt(0, view);
        k.id = 7;
        k.dof = camera::DofMode::Custom;
        k.focus = camera::FocusMode::Auto;
        camera::FocusConfig config;
        camera::FocusMemory memory;
        g_mainThere = true;

        // Enters only well inside the frame (0.9).
        g_mainX = WorldXFor(0.95f, view, scene);
        camera::FocusPlan f = camera::PlanFocus(&k, view, scene, 0, &memory, config);
        Check(f.source == camera::FocusSource::Engine, "hysteresis: at 0.95 of the frame it does not take the character yet");
        g_mainX = WorldXFor(0.5f, view, scene);
        f = camera::PlanFocus(&k, view, scene, 100, &memory, config);
        Check(f.source == camera::FocusSource::MainCharacter && Near(f.distance, 10.0f, 0.01f),
            "hysteresis: inside the frame it takes the character");
        // The head on the border, back and forth: it stays on the character.
        int engine = 0, held = 0, oldFlips = 0;
        camera::FocusSource oldLast = camera::FocusSource::MainCharacter;
        for (int n = 0; n < 20; ++n)
        {
            g_mainX = WorldXFor(n % 2 ? 1.04f : 0.97f, view, scene);
            f = camera::PlanFocus(&k, view, scene, 200 + n * 16, &memory, config);
            engine += f.source == camera::FocusSource::Engine;
            held += f.source == camera::FocusSource::Held;
            const camera::FocusPlan old = camera::PlanFocus(&k, view, scene, 200 + n * 16);
            oldFlips += old.source != oldLast;
            oldLast = old.source;
        }
        Check(engine == 0 && held == 0, "hysteresis: the head on the border keeps the focus on the character");
        Check(oldFlips >= 19, "hysteresis: the 0.186 rule flips every frame there (the flicker)");
        // Out past 1.1: kept for the grace, then the last distance held.
        g_mainX = WorldXFor(1.3f, view, scene);
        f = camera::PlanFocus(&k, view, scene, 1000, &memory, config);
        Check(f.source == camera::FocusSource::MainCharacter, "hysteresis: just out of the frame, still on the character");
        f = camera::PlanFocus(&k, view, scene, 1400, &memory, config);
        Check(f.source == camera::FocusSource::MainCharacter, "hysteresis: still inside the 500 ms grace");
        const float lastDistance = f.distance;
        f = camera::PlanFocus(&k, view, scene, 1500, &memory, config);
        Check(f.source == camera::FocusSource::Held && Near(f.distance, lastDistance, 1e-4f),
            "hysteresis: after the grace the shot keeps its last distance, not the engine's autofocus");
        g_mainThere = false;
        f = camera::PlanFocus(&k, view, scene, 1600, &memory, config);
        Check(f.source == camera::FocusSource::Held, "hysteresis: the character gone: the distance held");
        // A cut starts the shot over: nothing to hold, the autofocus.
        memory.ResetShot();
        f = camera::PlanFocus(&k, view, scene, 1700, &memory, config);
        Check(f.source == camera::FocusSource::Engine, "hysteresis: after a cut with nobody there, the engine's autofocus");
        // Out and back inside the grace: no release.
        g_mainThere = true;
        g_mainX = WorldXFor(0.0f, view, scene);
        camera::PlanFocus(&k, view, scene, 2000, &memory, config);
        g_mainX = WorldXFor(1.5f, view, scene);
        camera::PlanFocus(&k, view, scene, 2100, &memory, config);
        g_mainX = WorldXFor(1.0f, view, scene);
        camera::PlanFocus(&k, view, scene, 2400, &memory, config);
        g_mainX = WorldXFor(1.5f, view, scene);
        f = camera::PlanFocus(&k, view, scene, 2800, &memory, config);
        Check(f.source == camera::FocusSource::MainCharacter, "hysteresis: back within the frame restarts the grace");
        // ReplayCamFocusHysteresis=0: the plain rule even with a memory.
        camera::FocusConfig off;
        off.hysteresis = false;
        off.holdLast = false;
        camera::FocusMemory other;
        g_mainX = WorldXFor(1.04f, view, scene);
        f = camera::PlanFocus(&k, view, scene, 0, &other, off);
        Check(f.source == camera::FocusSource::Engine, "hysteresis off: out of the frame is the engine's autofocus");
        // A Look At target not there holds the shot's distance too.
        camera::FocusMemory look;
        g_mainX = WorldXFor(0.0f, view, scene);
        camera::PlanFocus(&k, view, scene, 0, &look, config);
        Keyframe lk = k;
        lk.lookAtTarget = 0x00090009; // never found
        g_mainX = WorldXFor(3.0f, view, scene);
        look.hold[0] = camera::FocusHold{};
        look.hold[1] = camera::FocusHold{};
        f = camera::PlanFocus(&lk, view, scene, 100, &look, config);
        Check(f.source == camera::FocusSource::Held && f.targetMissing, "hysteresis: a missing target holds the distance");
    }

    // ---- 3. The focus distance spring.
    void TestFocusSpring()
    {
        camera::FocusMemory memory;
        Check(Near(camera::SmoothFocusDistance(memory, 2.0f, 0.0f, 12.0f), 2.0f, 1e-4f), "focus spring: dt 0 primes on the distance");
        const float first = camera::SmoothFocusDistance(memory, 20.0f, 1.0f / 60.0f, 12.0f);
        Check(first > 2.0f && first < 3.0f, "focus spring: a jump of the focus distance is rounded, not a step");
        float value = first;
        for (int n = 0; n < 60; ++n)
            value = camera::SmoothFocusDistance(memory, 20.0f, 1.0f / 60.0f, 12.0f);
        Check(Near(value, 20.0f, 0.2f), "focus spring: there within a second");
        memory.ResetShot();
        Check(Near(camera::SmoothFocusDistance(memory, 5.0f, 1.0f / 60.0f, 12.0f), 5.0f, 1e-4f), "focus spring: a cut starts it over");
        Check(Near(camera::SmoothFocusDistance(memory, 9.0f, 1.0f / 60.0f, 0.0f), 9.0f, 1e-4f), "focus spring: omega 0 is off");
    }

    // ---- 4. The target from its recorded root.
    // The camera's own convention (camera_track.cpp FromEuler): columns are
    // right, forward, up; R = Rz(heading) Rx(pitch) Ry(roll).
    void BasisFromEuler(float pitch, float roll, float heading, pose::Vec3& right, pose::Vec3& forward, pose::Vec3& up)
    {
        const float d = 3.14159265f / 180.0f;
        const float cp = std::cos(pitch * d), sp = std::sin(pitch * d);
        const float cr = std::cos(roll * d), sr = std::sin(roll * d);
        const float ch = std::cos(heading * d), sh = std::sin(heading * d);
        const float m[3][3] = {
            {ch * cr - sh * sp * sr, -sh * cp, ch * sr + sh * sp * cr},
            {sh * cr + ch * sp * sr, ch * cp, sh * sr - ch * sp * cr},
            {-cp * sr, sp, cp * cr}};
        right = {m[0][0], m[1][0], m[2][0]};
        forward = {m[0][1], m[1][1], m[2][1]};
        up = {m[0][2], m[1][2], m[2][2]};
    }

    void TestRecordedTarget()
    {
        // A recorded engine matrix (rows right, forward, up, position, as the
        // pose record keeps it) decoded the way the playback decodes it.
        const float pitch = 12.0f, roll = -7.0f, heading = 131.0f;
        pose::Vec3 right{}, forward{}, up{};
        BasisFromEuler(pitch, roll, heading, right, forward, up);
        float matrix[16] = {right.x, right.y, right.z, 0.0f, forward.x, forward.y, forward.z, 0.0f, up.x, up.y, up.z, 0.0f,
            3.0f, 4.0f, 5.0f, 0.0f};
        pose::WorldTransform world{};
        Check(pose::DecodeWorldMatrix(matrix, world), "target: the recorded matrix decodes");
        const float q[4] = {world.rotation.x, world.rotation.y, world.rotation.z, world.rotation.w};
        const Vec3 position{world.position.x, world.position.y, world.position.z};
        const Vec3 head{3.0f, 4.0f, 6.6f};
        const camera::EntityState state = camera::EntityFromRecorded(position, q, &head);
        Check(Near(state.pitch, pitch, 0.01f) && Near(state.roll, roll, 0.01f) && Near(state.heading, heading, 0.01f),
            "target: heading, pitch and roll of the recorded root in the camera's convention");
        Check(Near(state.position.x, 3.0f, 1e-4f) && Near(state.position.y, 4.0f, 1e-4f) && Near(state.position.z, 5.0f, 1e-4f) &&
                state.hasHead && Near(state.head.z, 6.6f, 1e-4f),
            "target: position and head as given");

        // An attach Rigid camera on that state: the same pose as on the
        // state read from GET_ENTITY_ROTATION.
        camera::EntityState live;
        live.position = position;
        live.pitch = pitch;
        live.roll = roll;
        live.heading = heading;
        Keyframe k = FreeAt(0, {});
        k.attachTarget = kMain;
        k.attachType = camera::AttachType::Rigid;
        k.relPosition = {0.5f, -3.0f, 1.2f};
        k.relRotation = {-5.0f, 0.0f, 10.0f};
        camera::Scene recordedScene;
        recordedScene.lookup = [&](std::uint32_t, std::uint64_t, camera::EntityState& out) { out = state; return true; };
        camera::Scene liveScene;
        liveScene.lookup = [&](std::uint32_t, std::uint64_t, camera::EntityState& out) { out = live; return true; };
        const camera::LivePose a = camera::Resolve(k, 0, recordedScene);
        const camera::LivePose b = camera::Resolve(k, 0, liveScene);
        const float dx = a.pose.position.x - b.pose.position.x, dy = a.pose.position.y - b.pose.position.y,
                    dz = a.pose.position.z - b.pose.position.z;
        Check(std::sqrt(dx * dx + dy * dy + dz * dz) < 1e-3f, "target: attach Rigid on the recorded root matches the old frame");

        // The camera rides the record: a lookup that answers the recorded
        // root keeps an attach Default camera exactly on the recorded path,
        // whatever the live body does.
        float worst = 0.0f;
        camera::Scene record;
        record.lookup = [](std::uint32_t, std::uint64_t t, camera::EntityState& out) {
            out = camera::EntityState{};
            out.position = {static_cast<float>(t) * 0.002f, 0.0f, 0.0f};
            return true;
        };
        Keyframe follow = FreeAt(0, {});
        follow.attachTarget = kMain;
        follow.attachType = camera::AttachType::Default;
        follow.relPosition = {0.0f, -4.0f, 1.5f};
        Track track;
        track.AddMarker(follow);
        for (std::uint64_t t = 0; t < 2000; t += 16)
        {
            CameraState out{};
            camera::EvaluateMarkers(track, t, record, out);
            worst = std::max(worst, std::fabs(out.position.x - static_cast<float>(t) * 0.002f));
        }
        Check(worst < 1e-4f, "target: the camera rides the recorded root exactly");
    }

    // ---- 5. Smooth springs re-expressed on an anchor change.
    constexpr std::uint32_t kRunner = 0x00020002;

    float LargestStep(bool reframe, std::uint32_t& reframes, std::uint32_t& resets, float& boundaryStep)
    {
        camera::Scene scene;
        scene.lookup = [](std::uint32_t id, std::uint64_t t, camera::EntityState& out) {
            if (id != kRunner)
                return false;
            out = camera::EntityState{};
            out.position = {static_cast<float>(t) * 0.006f, 0.0f, 0.0f}; // 6 m/s
            return true;
        };
        auto attached = [](std::uint64_t t, const Vec3& rel) {
            Keyframe k = FreeAt(t, {}, Blend::Smooth);
            k.attachTarget = kRunner;
            k.attachType = camera::AttachType::Default;
            k.relPosition = rel;
            return k;
        };
        Track track;
        track.AddMarker(attached(0, {0.0f, -4.0f, 1.5f}));
        track.AddMarker(attached(1000, {12.0f, -4.0f, 1.5f})); // 12 m across in a second: the springs lag
        Keyframe free = FreeAt(2000, {{20.0f, -6.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 45.0f}, Blend::Smooth);
        track.AddMarker(free);
        track.AddMarker(FreeAt(3000, {{24.0f, -6.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, 45.0f}));
        camera::SpringState spring;
        spring.reframe = reframe;
        CameraState last{};
        bool have = false;
        float largest = 0.0f;
        boundaryStep = 0.0f;
        for (std::uint64_t t = 0; t <= 2500; t += 16)
        {
            CameraState out{};
            camera::EvaluateMarkers(track, t, scene, out, &spring, 0.016f);
            if (have)
            {
                const float dx = out.position.x - last.position.x, dy = out.position.y - last.position.y,
                            dz = out.position.z - last.position.z;
                const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                largest = std::max(largest, step);
                if (t >= 1000 && t < 1016)
                    boundaryStep = step;
            }
            last = out;
            have = true;
        }
        reframes = spring.reframes;
        resets = spring.resets;
        return largest;
    }

    void TestSmoothReframe()
    {
        std::uint32_t reframes = 0, resets = 0;
        float boundaryNew = 0.0f, boundaryOld = 0.0f;
        LargestStep(true, reframes, resets, boundaryNew);
        Check(reframes == 1 && resets == 0, "smooth: the anchor change is re-expressed once");
        std::uint32_t oldReframes = 0, oldResets = 0;
        LargestStep(false, oldReframes, oldResets, boundaryOld);
        Check(oldReframes == 0 && oldResets == 1, "smooth: ReplayCamSmoothReframe=0 resets (the 0.186 rule)");
        // A frame at 60 fps with a target at 6 m/s and the blend: about 0.1 to
        // 0.3 m. A spring reset would jump by the springs' lag.
        Check(boundaryNew < 0.5f, "smooth: no jump at the anchor change");
        Check(boundaryOld > boundaryNew + 0.2f, "smooth: the old reset jumped there (the lag at once)");
    }

    // ---- 6. Streaming ahead of a cut far away.
    void TestPrefetch()
    {
        Track track;
        track.AddMarker(FreeAt(0, {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 45.0f}));
        track.AddMarker(FreeAt(2000, {{100.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 90.0f}, 45.0f}));
        track.AddMarker(FreeAt(4000, {{110.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 45.0f}, Blend::Linear));
        track.AddMarker(FreeAt(6000, {{300.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 45.0f}));
        const camera::Scene scene;
        const CameraState here{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        camera::SegmentPlan plan = camera::Plan(track, 400);
        Check(!camera::PlanPrefetch(track, plan, 400, here, scene).want, "prefetch: not before the lead (1.5 s)");
        plan = camera::Plan(track, 600);
        camera::PrefetchTarget target = camera::PlanPrefetch(track, plan, 600, here, scene);
        Check(target.want && target.marker == 1 && target.cutMs == 2000 && Near(target.meters, 100.0f, 0.01f) &&
                Near(target.view.position.x, 100.0f, 0.01f) && Near(target.view.rotation.z, 90.0f, 0.05f),
            "prefetch: 1.4 s before a cut 100 m away, at the next marker's view");
        const CameraState there{{100.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 90.0f}, 45.0f};
        plan = camera::Plan(track, 3000);
        Check(!camera::PlanPrefetch(track, plan, 3000, there, scene).want, "prefetch: not for a cut 10 m away (under 30 m)");
        plan = camera::Plan(track, 5000);
        Check(!camera::PlanPrefetch(track, plan, 5000, there, scene).want, "prefetch: not into a marker the camera blends to");
        plan = camera::Plan(track, 600);
        Check(!camera::PlanPrefetch(track, plan, 600, here, scene, 1500, 150.0f).want, "prefetch: the distance is a setting");
    }
}

int RunCamera2Tests()
{
    TestDofBlend();
    TestFocusHysteresis();
    TestFocusSpring();
    TestRecordedTarget();
    TestSmoothReframe();
    TestPrefetch();
    if (g_failures == 0)
        std::puts("camera2 (dof, focus, recorded target, smooth, prefetch) tests passed");
    return g_failures;
}
