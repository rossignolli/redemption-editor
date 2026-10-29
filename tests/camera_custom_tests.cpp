// The Custom shake (Simple Camera's numbers) on markers and events; the
// old model is kept as the migration's input (camera_marker_shake_tests.cpp
// proves the fold).
#include "camera_shake_custom.h"
#include "camera_track.h"
#include "clip_file.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using camera::Blend;
    using camera::CameraMode;
    using camera::Keyframe;
    using camera::Track;
    using editor::CameraState;
    using editor::ShakeCustom;
    using editor::ShakeType;
    using editor::Vec3;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL (camera-custom): %s\n", message);
        ++g_failures;
    }

    Keyframe FreeAt(std::uint64_t t, const Vec3& at, Blend blend)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.pose = {at, Track::FromEulerDegrees({0, 0, 0}), 45.0f};
        k.blend = blend;
        k.ease = false;
        return k;
    }

    // A move along x at `speed` m/s from 0 to 4000 ms, every marker with the
    // Custom shake `c`.
    Track Moving(float speed, const ShakeCustom& c)
    {
        Track track;
        for (std::uint64_t t : {0ull, 2000ull, 4000ull})
        {
            Keyframe k = FreeAt(t, {speed * static_cast<float>(t) / 1000.0f, 0, 0}, t < 4000 ? Blend::Linear : Blend::None);
            k.legacyShake.type = ShakeType::Custom;
            k.legacyShake.custom = c;
            track.AddMarker(k);
        }
        return track;
    }

    CameraState Shaken(const Track& track, std::uint64_t t, const camera::Scene& scene = {})
    {
        CameraState view{};
        camera::EvaluateMarkers(track, t, scene, view);
        return view;
    }

    CameraState Raw(const Track& track, std::uint64_t t)
    {
        CameraState view{};
        camera::EvaluateMarkersRaw(track, t, {}, view);
        return view;
    }

    float Offset(const CameraState& a, const CameraState& b, bool rotation)
    {
        if (rotation)
            return std::fabs(a.rotation.x - b.rotation.x) + std::fabs(a.rotation.y - b.rotation.y) + std::fabs(a.rotation.z - b.rotation.z);
        return std::fabs(a.position.x - b.position.x) + std::fabs(a.position.y - b.position.y) + std::fabs(a.position.z - b.position.z);
    }

    void TestNumbers()
    {
        namespace sh = camera::shake;
        ShakeCustom c = editor::DefaultShakeCustom();
        Check(sh::ValidCustom(c), "custom: the defaults are valid");
        c.amp = 9.0f;
        Check(!sh::ValidCustom(c) && sh::ClampCustom(c).amp == 5.0f, "custom: out of range refused, clamped");
        // A different pattern seed, a different pattern; the same seed, the same.
        ShakeCustom a = editor::DefaultShakeCustom(), b = a;
        b.seed = 7;
        const sh::Sample sa = sh::EvaluateCustom(a, 1.0f, 3.21, 0.0f, 1.0f, 2.0);
        const sh::Sample sb = sh::EvaluateCustom(b, 1.0f, 3.21, 0.0f, 1.0f, 2.0);
        const sh::Sample sa2 = sh::EvaluateCustom(a, 1.0f, 3.21, 0.0f, 1.0f, 2.0);
        Check(std::memcmp(&sa, &sa2, sizeof(sa)) == 0, "custom: deterministic");
        Check(std::memcmp(&sa, &sb, sizeof(sa)) != 0, "custom: the seed changes the pattern");
        // Weights: rotation 0 leaves only position, and the other way round.
        ShakeCustom noRot = a, noPos = a;
        noRot.rotWeight = 0.0f;
        noPos.posWeight = 0.0f;
        const sh::Sample r = sh::EvaluateCustom(noRot, 1.0f, 1.7, 0.0f, 1.0f, 2.0);
        const sh::Sample p = sh::EvaluateCustom(noPos, 1.0f, 1.7, 0.0f, 1.0f, 2.0);
        Check(r.value[sh::Pitch] == 0.0f && r.value[sh::Roll] == 0.0f && r.value[sh::Yaw] == 0.0f && r.value[sh::X] != 0.0f,
            "custom: rotation weight 0, no rotation");
        Check(p.value[sh::X] == 0.0f && p.value[sh::Y] == 0.0f && p.value[sh::Z] == 0.0f && p.value[sh::Pitch] != 0.0f,
            "custom: position weight 0, no position");
        // Size: rotation reaches amp degrees, position amp x 5 cm.
        float rot = 0.0f, pos = 0.0f;
        for (int n = 0; n < 4000; ++n)
        {
            const sh::Sample s = sh::EvaluateCustom(a, 1.0f, n * 0.013, 0.0f, 1.0f, 2.0);
            rot = std::max(rot, std::fabs(s.value[sh::Yaw]));
            pos = std::max(pos, std::fabs(s.value[sh::Z]));
        }
        Check(rot > 0.25f && rot <= 0.4001f && pos > 0.0125f && pos <= 0.02001f, "custom: amp 0.4 is 0.4 degrees and 2 cm at most");
        // Speed -> amplitude: at the reference speed, (1 + speedAmp) times.
        const sh::Sample slow = sh::EvaluateCustom(a, 1.0f, 0.77, 0.0f, 1.0f, 2.0);
        const sh::Sample fast = sh::EvaluateCustom(a, 1.0f, 0.77, 1.0f, 1.0f, 2.0);
        Check(std::fabs(fast.value[sh::Yaw] - slow.value[sh::Yaw] * (1.0f + a.speedAmp)) < 1e-5f, "custom: speed raises the amplitude");
        // A preset's numbers are valid.
        bool presets = true;
        for (std::uint8_t t = 1; t < 11; ++t)
            presets = presets && sh::ValidCustom(sh::CustomFromPreset(static_cast<ShakeType>(t)));
        Check(presets, "custom: Copy From Preset gives valid numbers");
    }

    void TestMarkers()
    {
        // Scrub equals play: the same t gives the same shake, with or without
        // the phase memo, reached forwards or by a jump.
        ShakeCustom c = editor::DefaultShakeCustom();
        c.speedHz = 1.0f; // the coupling integrated over the curve
        c.speedRef = 10.0f;
        const Track track = Moving(10.0f, c);
        camera::ShakePhaseCache cache;
        camera::Scene withCache;
        withCache.shakePhase = &cache;
        CameraState played{};
        for (std::uint64_t t = 0; t <= 3000; t += 16)
            played = Shaken(track, t, withCache);
        const CameraState jumped = Shaken(track, 3000);
        const CameraState again = Shaken(track, 3000, withCache);
        Check(Offset(played, Shaken(track, 2992, {}), false) < 1.0f, "custom: the played shake is near its neighbour"); // sanity
        Check(Offset(Shaken(track, 2992, withCache), Shaken(track, 2992, {}), false) < 1e-5f &&
                  Offset(again, jumped, true) < 1e-5f && Offset(again, jumped, false) < 1e-6f,
            "custom: scrub equals play (memo or not)");
        // The shake is there: the shaken view differs from the curve.
        Check(Offset(jumped, Raw(track, 3000), true) > 1e-4f, "custom: markers shake");
        // The frequency coupling grows with the distance travelled, not with
        // standing; deterministic with and without the memo.
        const double moving = camera::CustomCouplingCycles(track, {}, 0, 0, 3000);
        const double memo = camera::CustomCouplingCycles(track, withCache, 0, 0, 3000);
        Check(moving > 2.5 && std::fabs(moving - memo) < 1e-9, "custom: speed coupling of the frequency, memo equal");
        const Track still = Moving(0.0f, c);
        Check(camera::CustomCouplingCycles(still, {}, 0, 0, 3000) == 0.0, "custom: no coupling standing still");
        // Stop When Still: a camera standing still has no shake.
        ShakeCustom gated = c;
        gated.still = 1;
        const Track parked = Moving(0.0f, gated);
        Check(Offset(Shaken(parked, 2500), Raw(parked, 2500), true) < 1e-6f, "custom: still, no shake");
        const Track rolling = Moving(10.0f, gated);
        Check(Offset(Shaken(rolling, 2500), Raw(rolling, 2500), true) > 1e-4f, "custom: moving, the shake back");
        // A blend into another Custom marker: the numbers go from one to the other.
        Track blend;
        ShakeCustom low = editor::DefaultShakeCustom(), high = low;
        high.amp = 2.0f;
        Keyframe a = FreeAt(0, {0, 0, 0}, Blend::Linear), b = FreeAt(2000, {1, 0, 0}, Blend::None);
        a.legacyShake.type = b.legacyShake.type = ShakeType::Custom;
        a.legacyShake.custom = low;
        b.legacyShake.custom = high;
        blend.AddMarker(a);
        blend.AddMarker(b);
        Check(std::fabs(camera::Plan(blend, 1000).shake.custom.amp - 1.2f) < 1e-4f, "custom: blended halfway");
    }

    // The events set Simple Camera's numbers one by one (Profile 0: its own
    // noise); the camera shakes with them and the phase integrates a Freq ramp.
    void TestEvents()
    {
        namespace fx = camera::fx;
        Track track = Moving(5.0f, editor::DefaultShakeCustom());
        for (Keyframe k : track.Keyframes())
            track.FindMutable(k.id)->legacyShake.type = ShakeType::None;
        const auto add = [&](std::uint64_t t, fx::Kind kind, float value, fx::Mode mode = fx::Mode::Snap) {
            fx::ShakeEvent e;
            e.timeMs = t;
            e.kind = kind;
            e.value = value;
            e.mode = mode;
            return track.LegacyEffects().Add(e);
        };
        add(500, fx::Kind::Amp, 0.4f);
        add(500, fx::Kind::Freq, 4.0f);
        add(500, fx::Kind::OnOff, 1.0f);
        add(2500, fx::Kind::Amp, 1.4f, fx::Mode::Ramp);
        add(2500, fx::Kind::Freq, 8.0f, fx::Mode::Ramp);
        const fx::FxShake mid = fx::Evaluate(track.LegacyEffects(), 1500);
        Check(mid.active && mid.profile == ShakeType::None && std::fabs(mid.custom.amp - 0.9f) < 1e-4f, "events: Amp ramps");
        // The phase integrates the ramp: 1 s at 4 Hz rising to 6 Hz is 5 cycles.
        Check(std::fabs(mid.cycles - 5.0) < 1e-6, "events: the ramp's frequency integrated");
        Check(Offset(Shaken(track, 1500), Raw(track, 1500), true) > 1e-4f, "events: the numbers shake the camera");
        // Fields as the director writes them.
        fx::ShakeEvent parsed;
        std::string error;
        Check(fx::ParseEventFields({"t=100", "kind=freq", "value=12", "mode=ramp"}, parsed, error) && parsed.kind == fx::Kind::Freq &&
                  parsed.value == 12.0f && parsed.mode == fx::Mode::Ramp && parsed.timeMs == 100,
            "events: the fields parse");
        Check(!fx::ParseEventFields({"kind=amp", "value=9"}, parsed, error), "events: a number out of range is refused");
    }

    // The old CAMR versions (v5..v8, the old marker shake) and CFXE are read
    // and folded into the markers: camera_marker_shake_tests.cpp (it writes
    // the old payloads itself).
}

namespace
{
    // Finding 15 of the release review: the speed coupling's memo keeps the
    // positions it sampled while the inputs of the path are the same. A preset
    // camera (Rear) on a turning character: its distance (presetDistanceScale)
    // sets the radius, so the speed and the coupling. After a change of the
    // distance alone, the memo must give what a fresh evaluation gives.
    Track RearPreset(float distanceScale, const ShakeCustom& c)
    {
        Track track;
        for (std::uint64_t t : {0ull, 4000ull})
        {
            Keyframe k = FreeAt(t, {0, 0, 0}, t < 4000 ? Blend::Linear : Blend::None);
            k.cameraMode = CameraMode::Rear;
            k.presetDistanceScale = distanceScale;
            k.legacyShake.type = ShakeType::Custom;
            k.legacyShake.custom = c;
            track.AddMarker(k);
        }
        return track;
    }

    void TestMemoFollowsThePath()
    {
        ShakeCustom c = editor::DefaultShakeCustom();
        c.speedHz = 1.0f;
        c.speedRef = 10.0f;
        camera::Scene scene;
        scene.mainCharacter = 1;
        scene.lookup = [](std::uint32_t id, std::uint64_t timeMs, camera::EntityState& state) {
            if (id != 1)
                return false;
            state = {};
            state.heading = static_cast<float>(timeMs) * 0.09f; // 90 degrees a second, in place
            return true;
        };
        camera::ShakePhaseCache cache;
        camera::Scene withCache = scene;
        withCache.shakePhase = &cache;
        const Track nearCamera = RearPreset(0.0f, c);
        const Track farCamera = RearPreset(1.0f, c);
        const double nearCycles = camera::CustomCouplingCycles(nearCamera, withCache, 0, 0, 3000);
        const double farFresh = camera::CustomCouplingCycles(farCamera, scene, 0, 0, 3000);
        const double farMemo = camera::CustomCouplingCycles(farCamera, withCache, 0, 0, 3000);
        Check(std::fabs(farFresh - nearCycles) > 1e-3, "memo: the preset distance changes the coupling");
        Check(std::fabs(farMemo - farFresh) < 1e-9, "memo: a change of the preset distance alone rebuilds the memo");
        // The look at roll (a preset uses it as its roll) and an attach bone
        // count too; a filter (the picture only) does not.
        Track rolled = farCamera;
        const_cast<std::vector<Keyframe>&>(rolled.Keyframes())[0].attachBone = 7;
        const std::uint32_t rebuilds = cache.rebuilds;
        camera::CustomCouplingCycles(rolled, withCache, 0, 0, 3000);
        Check(cache.rebuilds == rebuilds + 1, "memo: an attach bone change rebuilds the memo");
        Track filtered = rolled;
        const_cast<std::vector<Keyframe>&>(filtered.Keyframes())[0].filter = 2;
        camera::CustomCouplingCycles(filtered, withCache, 0, 0, 3000);
        Check(cache.rebuilds == rebuilds + 1, "memo: a filter change keeps the memo");
    }
}

int RunCameraCustomTests()
{
    TestMemoFollowsThePath();
    TestNumbers();
    TestMarkers();
    TestEvents();
    if (g_failures == 0)
        std::puts("camera-custom (Custom shake, event numbers: the old model, the migration's input) tests passed");
    return g_failures;
}
