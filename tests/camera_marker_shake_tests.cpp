// The shake in the markers (camera_marker_shake.h). The model (Cut, Blend, the
// clock, Restart, the fade out), and the migration of the old marker shake and
// the old shake events track: every case below is folded and compared frame by
// frame with the old rules (mshake::Verify), the shake and the camera path, to
// 1e-5. The old CAMR payloads (v4..v8) are written here byte by byte as the old
// builds wrote them.
#include "camera_marker_shake.h"
#include "camera_effects.h"
#include "camera_shake.h"
#include "clip_file.h"
#include "old_camera_payload.h"

#include <algorithm>
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
    using editor::MarkerShake;
    using editor::ShakeType;
    using editor::Vec3;
    namespace fx = camera::fx;
    namespace mshake = camera::mshake;

    int g_failures = 0;
    double g_worstShake = 0.0; // the largest shake difference over every exact case (printed)
    double g_worstPath = 0.0;
    int g_cases = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL (shake-markers): %s\n", message);
        ++g_failures;
    }

    Keyframe FreeAt(std::uint64_t t, const Vec3& at, Blend blend, bool ease = false, float yaw = 0.0f)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.pose = {at, Track::FromEulerDegrees({-5.0f, 0.0f, yaw}), 45.0f};
        k.blend = blend;
        k.ease = ease;
        return k;
    }

    Keyframe Legacy(Keyframe k, ShakeType type, std::uint8_t intensity = 5, std::uint8_t speedIndex = 4,
        editor::ShakeMotion motion = editor::ShakeMotion::Off)
    {
        k.legacyShake.type = type;
        k.legacyShake.intensity = intensity;
        k.legacyShake.speedIndex = speedIndex;
        k.legacyShake.motion = motion;
        return k;
    }

    fx::ShakeEvent Event(std::uint64_t t, fx::Kind kind, float value, fx::Mode mode = fx::Mode::Snap)
    {
        fx::ShakeEvent e;
        e.timeMs = t;
        e.kind = kind;
        e.value = value;
        e.mode = mode;
        return e;
    }

    // The recorded game camera (a slow pan), for the Game Camera markers.
    std::vector<camera::GameSample> GameSamples(std::uint32_t durationMs)
    {
        std::vector<camera::GameSample> samples;
        for (std::uint32_t t = 0; t <= durationMs; t += 33)
        {
            const float s = static_cast<float>(t) / 1000.0f;
            samples.push_back({t, {s * 3.0f, -6.0f, 2.0f}, {-4.0f, 0.0f, s * 8.0f}, 50.0f});
        }
        return samples;
    }

    // Folds the track and checks the proof: the shake and the path the
    // same at every frame. `expectInexact`: the case is one the model does
    // not hold (the report must say so).
    mshake::Proof Fold(Track& track, std::uint64_t durationMs, const char* what, const camera::Scene& scene = {},
        bool expectInexact = false)
    {
        Check(track.HasLegacyShake(), what);
        mshake::Migrate(track);
        const camera::ShakeMigrationReport& report = track.Migration();
        Check(report.ran && report.before && !track.HasLegacyShake(), what);
        mshake::Proof proof;
        if (!report.before)
            return proof;
        proof = mshake::Verify(*report.before, track, scene, durationMs, 0);
        ++g_cases;
        if (expectInexact)
        {
            Check(report.inexact > 0, what);
            return proof;
        }
        g_worstShake = std::max({g_worstShake, proof.shakeRotation, proof.shakePosition});
        g_worstPath = std::max({g_worstPath, proof.pathPosition, proof.pathRotation, proof.pathFov});
        const bool exact = proof.Exact(1e-5) && report.inexact == 0;
        if (!exact)
            std::fprintf(stderr, "  %s: %s\n", what, mshake::FormatLine(report, proof).c_str());
        Check(exact, what);
        return proof;
    }

    // ---- 1. The model.

    void TestModel()
    {
        Track track;
        Keyframe a = FreeAt(0, {0, 0, 0}, Blend::Linear);
        Keyframe b = FreeAt(2000, {10, 0, 0}, Blend::None);
        a.shake.on = 1;
        mshake::ApplyProfile(a.shake, static_cast<std::uint8_t>(ShakeType::Handheld));
        a.shake.gain = 1.0f;
        b.shake = a.shake;
        b.shake.gain = 3.0f;
        a.shake.transition = static_cast<std::uint8_t>(editor::ShakeTransition::Blend);
        a.shake.blendMask = editor::ShakeBlendGain;
        track.AddMarker(a);
        track.AddMarker(b);
        const mshake::Resolved mid = mshake::Resolve(track, 1000);
        Check(mid.active && std::fabs(mid.gain - 2.0f) < 1e-6f, "model: Blend takes the gain halfway");
        Check(std::fabs(mid.cycles - 4.0) < 1e-9, "model: 4 Hz for 1 s is 4 cycles (a profile at its own speed)");
        Check(std::fabs(mshake::Resolve(track, 2500).cycles - 10.0) < 1e-9, "model: the clock goes on over a marker");
        Track cut = track;
        cut.FindMutable(cut.Keyframes()[0].id)->shake.transition = 0;
        Check(std::fabs(mshake::Resolve(cut, 1999).gain - 1.0f) < 1e-6f && std::fabs(mshake::Resolve(cut, 2000).gain - 3.0f) < 1e-6f,
            "model: Cut holds until the next marker");
        Keyframe* second = cut.FindMutable(cut.Keyframes()[1].id);
        second->shake.flags |= editor::ShakeFlagRestart;
        second->shake.startCycles = 0.5;
        Check(std::fabs(mshake::Resolve(cut, 2250).cycles - 1.5) < 1e-9, "model: Restart starts the phase from its startCycles");
        Keyframe* first = cut.FindMutable(cut.Keyframes()[0].id);
        first->shake.flags |= editor::ShakeFlagFadeOut;
        Check(std::fabs(mshake::Resolve(cut, 1875).gain - 0.5f) < 1e-5f && mshake::Resolve(cut, 1700).gain == 1.0f,
            "model: FadeOut is a smooth 250 ms fade");
        first->shake.rate = 2.0f;
        Check(std::fabs(mshake::Resolve(cut, 1000).cycles - 8.0) < 1e-9, "model: the clock rate (shake speed)");
        first->shake.flags |= editor::ShakeFlagRealTime;
        first->speedIndex = 3; // 50 %: real time doubles it again
        Check(std::fabs(mshake::Resolve(cut, 1000).cycles - 16.0) < 1e-6, "model: real time over the marker's speed");
        Check(mshake::Resolve(track, 0).active && !mshake::Resolve(Track{}, 100).active, "model: no marker, no shake");
        // An Anchor placed by hand holds none; one added by the migration does.
        Track anchors = track;
        Keyframe anchor = FreeAt(1000, {5, 0, 0}, Blend::None);
        anchor.markerType = camera::MarkerType::Anchor;
        anchor.shake.on = 0;
        anchors.InsertMigrated(anchor);
        Check(mshake::Resolve(anchors, 1500).active, "model: a plain Anchor does not hold a shake");
        for (const Keyframe& k : anchors.Keyframes())
            if (k.markerType == camera::MarkerType::Anchor)
                anchors.FindMutable(k.id)->origin = camera::kMarkerOriginShakeMigration;
        Check(!mshake::Resolve(anchors, 1500).active, "model: an Anchor added by the migration does");
        // Scrub equals play: the same t, the same delta.
        const camera::ShakeDelta x = mshake::Delta(track, 1234, {}), y = mshake::Delta(track, 1234, {});
        Check(std::memcmp(&x, &y, sizeof(x)) == 0 && std::fabs(x.rotation.x) + std::fabs(x.rotation.z) > 0.0f,
            "model: deterministic, and it shakes");
        // A new marker inside a blend holds the blended state.
        const MarkerShake held = mshake::HeldAt(track, 1000);
        Check(held.on && std::fabs(held.gain - 2.0f) < 1e-6f && (held.flags & editor::ShakeFlagRestart) == 0,
            "model: HeldAt is the state in force, no restart");
        // The UI's intensity.
        Check(mshake::IntensityOf(mshake::GainOf(1)) == 1 && mshake::IntensityOf(mshake::GainOf(5)) == 5 &&
                  mshake::IntensityOf(mshake::GainOf(10)) == 10,
            "model: intensity 1..10 and its gain both ways");
        MarkerShake p = editor::DefaultMarkerShake();
        mshake::ApplyPreset(p, 3);
        Check(p.on && p.profile == 0 && p.amp == 1.5f && p.hz == 10.0f && mshake::MatchingPreset(p) == 3 &&
                  mshake::Describe(p) == "simple Earthquake",
            "model: Simple Camera's presets fill the numbers");
    }

    // ---- 2. The old marker shake, folded.

    void TestLegacyMarkers()
    {
        // A chain of Handheld that blends 3 -> 8 (eased), then Drunk (fade
        // out before it), None, Custom with couplings, a speed change.
        Track track;
        track.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear, true, 0), ShakeType::Handheld, 3));
        track.AddMarker(Legacy(FreeAt(2000, {8, 0, 1}, Blend::Linear, true, 20), ShakeType::Handheld, 8));
        track.AddMarker(Legacy(FreeAt(3500, {12, 3, 1}, Blend::Smooth, false, 40), ShakeType::Drunk, 4, 6));
        track.AddMarker(Legacy(FreeAt(5000, {12, 8, 2}, Blend::None, false, 60), ShakeType::None));
        Keyframe c = Legacy(FreeAt(6000, {14, 8, 2}, Blend::Spline, false, 80), ShakeType::Custom, 7, 4);
        c.legacyShake.custom.amp = 0.8f;
        c.legacyShake.custom.hz = 2.0f;
        c.legacyShake.custom.speedHz = 0.8f;
        c.legacyShake.custom.speedAmp = 1.0f;
        c.legacyShake.custom.seed = 9;
        track.AddMarker(c);
        Keyframe d = Legacy(FreeAt(8000, {30, 8, 2}, Blend::Spline, false, 100), ShakeType::Custom, 2, 5);
        d.legacyShake.custom = c.legacyShake.custom;
        d.legacyShake.custom.hz = 6.0f;
        d.legacyShake.custom.still = 1;
        d.speedIndex = 3; // 50 %: the old clock was real time
        track.AddMarker(d);
        track.AddMarker(Legacy(FreeAt(10000, {31, 9, 2}, Blend::None, false, 120), ShakeType::Walking, 5, 8,
            editor::ShakeMotion::SpeedStill));
        track.AddMarker(Legacy(FreeAt(11500, {40, 9, 2}, Blend::None, false, 140), ShakeType::Explosion, 9));
        track.AddMarker(Legacy(FreeAt(14000, {40, 9, 2}, Blend::None, false, 140), ShakeType::Horseback, 6, 4,
            editor::ShakeMotion::Speed));
        Track any = track;
        any.SetLegacyBlendAnyMode(true);
        Fold(track, 16000, "old marker shake: blend, fade out, None, Custom couplings, speeds, motion, explosion");

        // The last marker that only ends the clip (the one before does not
        // blend into it): its shake never played, the one before goes on.
        Track last;
        last.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::None), ShakeType::Handheld, 5));
        last.AddMarker(Legacy(FreeAt(1000, {1, 0, 0}, Blend::None), ShakeType::Running, 5));
        last.AddMarker(Legacy(FreeAt(3000, {2, 0, 0}, Blend::None), ShakeType::Steadicam, 4));
        Fold(last, 5000, "old marker shake: the clip's last marker");

        // An Anchor between: no end of the chain; Game Camera markers.
        Track mixed;
        mixed.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear), ShakeType::AirTurbulence, 6));
        Keyframe anchor = FreeAt(1500, {5, 0, 0}, Blend::None);
        anchor.markerType = camera::MarkerType::Anchor;
        mixed.AddMarker(anchor);
        mixed.AddMarker(Legacy(FreeAt(3000, {9, 0, 0}, Blend::None), ShakeType::AirTurbulence, 2));
        Keyframe game = Legacy(FreeAt(4000, {9, 0, 0}, Blend::None), ShakeType::GroundVibration, 5);
        game.cameraMode = CameraMode::Game;
        mixed.AddMarker(game);
        Keyframe edit = Legacy(FreeAt(6000, {9, 2, 0}, Blend::Linear), ShakeType::Wagon, 5);
        edit.markerType = camera::MarkerType::EditAndAnchor;
        mixed.AddMarker(edit);
        mixed.AddMarker(Legacy(FreeAt(8000, {12, 2, 0}, Blend::None), ShakeType::Wagon, 9));
        const std::vector<camera::GameSample> samples = GameSamples(10000);
        camera::Scene scene;
        scene.game = &samples;
        Fold(mixed, 10000, "old marker shake: an Anchor between, a Game Camera, Edit and Anchor", scene);

        // A converted v2 clip (blends into any camera type).
        Fold(any, 16000, "old marker shake: a legacyBlendAnyMode track", scene);
    }

    // ---- 3. The old events track (Simple Camera's model), folded.

    void TestLegacyEvents()
    {
        // The bench's camera-cine-fx scenario: markers with Handheld 3, the
        // events over them (ramps, a preset, Randomize, off, an old line).
        Track track;
        track.AddMarker(FreeAt(0, {0, 0, 0}, Blend::None));
        Keyframe game0 = track.Keyframes()[0];
        track.FindMutable(game0.id)->cameraMode = CameraMode::Game;
        track.AddMarker(Legacy(FreeAt(1000, {6, 0, 1.5f}, Blend::Spline, false, 60), ShakeType::Handheld, 3));
        track.AddMarker(Legacy(FreeAt(9000, {0, 14, 1.5f}, Blend::None, false, 170), ShakeType::Handheld, 3));
        Keyframe end = FreeAt(12000, {0, 0, 0}, Blend::None);
        end.cameraMode = CameraMode::Game;
        track.AddMarker(end);
        fx::EffectTrack& fx = track.LegacyEffects();
        fx.Add(Event(2000, fx::Kind::Amp, 1.5f));
        fx.Add(Event(2000, fx::Kind::Freq, 0.3f));
        fx.Add(Event(2000, fx::Kind::OnOff, 1.0f));
        fx.Add(Event(5000, fx::Kind::Amp, 0.4f, fx::Mode::Ramp));
        fx.Add(Event(5000, fx::Kind::Freq, 6.0f, fx::Mode::Ramp));
        fx.Add(Event(6000, fx::Kind::Preset, 4.0f));
        fx.Add(Event(7000, fx::Kind::Randomize, 7.0f));
        fx.Add(Event(8000, fx::Kind::OnOff, 0.0f));
        fx::LegacyEvent old;
        old.timeMs = 9000;
        old.shake = ShakeType::Horseback;
        for (const fx::ShakeEvent& e : fx::ConvertLegacy({old}))
            fx.Add(e);
        const std::vector<camera::GameSample> samples = GameSamples(12000);
        camera::Scene scene;
        scene.game = &samples;
        Fold(track, 12000, "events: the camera-cine-fx scenario", scene);
        Check(track.Migration().events == 8 + 9 && track.Migration().markersAdded >= 4,
            "events: the report counts the events and the added Anchors");

        // The camera-cine-custom scenario: Custom markers with couplings, the
        // events' numbers ramping (rotation off, position on), Randomize.
        Track custom;
        Keyframe a = FreeAt(1000, {0, 0, 1}, Blend::Spline, false, 60);
        a = Legacy(a, ShakeType::Custom, 5);
        a.legacyShake.custom.amp = 0.6f;
        a.legacyShake.custom.hz = 2.0f;
        a.legacyShake.custom.speedHz = 0.5f;
        a.legacyShake.custom.still = 1;
        custom.AddMarker(FreeAt(0, {0, 0, 0}, Blend::None));
        custom.AddMarker(a);
        Keyframe b = a;
        b.timeMs = 9000;
        b.pose.position = {20, 10, 1};
        b.blend = Blend::None;
        custom.AddMarker(b);
        custom.AddMarker(FreeAt(12000, {20, 10, 1}, Blend::None));
        fx::EffectTrack& cf = custom.LegacyEffects();
        cf.Add(Event(4000, fx::Kind::Amp, 0.8f));
        cf.Add(Event(4000, fx::Kind::Freq, 4.0f));
        cf.Add(Event(4000, fx::Kind::PosWeight, 0.0f));
        cf.Add(Event(4000, fx::Kind::OnOff, 1.0f));
        cf.Add(Event(4000, fx::Kind::SpeedFreq, 0.7f));
        cf.Add(Event(7000, fx::Kind::Amp, 0.3f, fx::Mode::Ramp));
        cf.Add(Event(7000, fx::Kind::Freq, 9.0f, fx::Mode::Ramp));
        cf.Add(Event(7000, fx::Kind::RotWeight, 0.0f, fx::Mode::Ramp));
        cf.Add(Event(7000, fx::Kind::PosWeight, 1.5f, fx::Mode::Ramp));
        cf.Add(Event(7000, fx::Kind::Randomize, 7.0f));
        cf.Add(Event(10000, fx::Kind::OnOff, 0.0f));
        Fold(custom, 12000, "events: the camera-cine-custom scenario (couplings, ramps, Randomize)");

        // Events only (no shake on the markers), ramps that cross markers,
        // a profile, couplings, Stop When Still, events at a marker's time.
        Track only;
        only.AddMarker(FreeAt(0, {0, 0, 0}, Blend::Linear, true));
        only.AddMarker(FreeAt(2500, {10, 0, 0}, Blend::Linear, true, 45));
        only.AddMarker(FreeAt(5000, {10, 10, 0}, Blend::Spline, false, 90));
        only.AddMarker(FreeAt(7000, {0, 10, 3}, Blend::None, false, 135));
        fx::EffectTrack& of = only.LegacyEffects();
        of.Add(Event(500, fx::Kind::Profile, 7.0f)); // Running: on
        of.Add(Event(500, fx::Kind::Amp, 1.2f));
        of.Add(Event(500, fx::Kind::SpeedAmp, 1.0f));
        of.Add(Event(2500, fx::Kind::Freq, 8.0f, fx::Mode::Ramp)); // at a marker
        of.Add(Event(3000, fx::Kind::StopWhenStill, 1.0f));
        of.Add(Event(4200, fx::Kind::Amp, 0.2f, fx::Mode::Ramp)); // across the marker at 2500? no: 500 -> 4200
        of.Add(Event(6000, fx::Kind::Profile, 0.0f)); // the noise from here
        of.Add(Event(6000, fx::Kind::SpeedFreq, 1.2f));
        of.Add(Event(6500, fx::Kind::Amp, 0.9f, fx::Mode::Ramp));
        Fold(only, 8000, "events: profiles, ramps across markers, couplings, Stop When Still");
        Check(only.Migration().eventsOnMarkers == 1, "events: the one on a marker went into it");

        // A ramp of a switch and of an index: the step where it crosses.
        Track steps;
        steps.AddMarker(FreeAt(0, {0, 0, 0}, Blend::Linear));
        steps.AddMarker(FreeAt(6000, {6, 0, 0}, Blend::None));
        fx::EffectTrack& sf = steps.LegacyEffects();
        sf.Add(Event(0, fx::Kind::OnOff, 0.0f));
        sf.Add(Event(1000, fx::Kind::OnOff, 1.0f, fx::Mode::Ramp)); // on at 500
        sf.Add(Event(1000, fx::Kind::Profile, 1.0f));
        sf.Add(Event(4000, fx::Kind::Profile, 4.0f, fx::Mode::Ramp)); // 1 -> 4, three steps
        sf.Add(Event(4500, fx::Kind::StopWhenStill, 1.0f, fx::Mode::Ramp));
        Fold(steps, 6500, "events: ramps of a switch and a profile");
        // A Preset that ramps (the old phase's straight line over a stepped
        // frequency): not held exactly, and the report says so.
        Track presetRamp;
        presetRamp.AddMarker(FreeAt(0, {0, 0, 0}, Blend::Linear));
        presetRamp.AddMarker(FreeAt(6000, {6, 0, 0}, Blend::None));
        presetRamp.LegacyEffects().Add(Event(4500, fx::Kind::Preset, 1.0f));
        presetRamp.LegacyEffects().Add(Event(5500, fx::Kind::Preset, 3.0f, fx::Mode::Ramp));
        const mshake::Proof ramp = Fold(presetRamp, 6500, "events: a Preset ramp (counted inexact)", {}, true);
        std::printf("  shake-markers: a Preset ramp (not held): shake diff %.3g deg, %.3g m\n", ramp.shakeRotation, ramp.shakePosition);

        // Nothing changes: an event that sets what is there adds no Anchor.
        Track same;
        same.AddMarker(FreeAt(0, {0, 0, 0}, Blend::None));
        same.AddMarker(FreeAt(4000, {1, 0, 0}, Blend::None));
        same.LegacyEffects().Add(Event(1000, fx::Kind::OnOff, 1.0f));
        same.LegacyEffects().Add(Event(2000, fx::Kind::Amp, 0.0f));
        same.LegacyEffects().Add(Event(2000, fx::Kind::Amp, 0.0f));
        same.LegacyEffects().Add(Event(3000, fx::Kind::RotWeight, 1.0f)); // the default
        Fold(same, 4000, "events: a change that changes nothing");
        Check(same.Migration().pruned >= 1, "events: the Anchor of a change that changes nothing is dropped");

        // Old CFXE v1/v2 events (a whole shake each), converted then folded.
        Track whole;
        whole.AddMarker(FreeAt(0, {0, 0, 0}, Blend::Linear));
        whole.AddMarker(FreeAt(5000, {5, 5, 0}, Blend::None));
        std::vector<fx::LegacyEvent> olds(3);
        olds[0].timeMs = 500;
        olds[0].shake = ShakeType::Handheld;
        olds[0].intensity = 7;
        olds[1].timeMs = 2000;
        olds[1].shake = ShakeType::Custom;
        olds[1].custom.amp = 1.1f;
        olds[1].mode = fx::Mode::Ramp;
        olds[2].timeMs = 3500;
        olds[2].shake = ShakeType::None;
        for (const fx::ShakeEvent& e : fx::ConvertLegacy(olds))
            whole.LegacyEffects().Add(e);
        Fold(whole, 5000, "events: CFXE v1/v2 whole-shake events");
    }

    // ---- 4. Both: the events take over from the markers' shake and give it
    // back. Exact where the markers' shake holds (Cut) around the switch;
    // a switch inside the markers' blend is a case the model does not hold
    // (the report counts it as inexact).
    void TestBoth()
    {
        Track track;
        track.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear, false), ShakeType::Walking, 4));
        track.AddMarker(Legacy(FreeAt(3000, {9, 0, 0}, Blend::None, false, 30), ShakeType::Walking, 6));
        track.AddMarker(Legacy(FreeAt(9000, {9, 9, 0}, Blend::None, false, 60), ShakeType::Steadicam, 5));
        fx::EffectTrack& f = track.LegacyEffects();
        f.Add(Event(4000, fx::Kind::Profile, 5.0f)); // Explosion over the Walking
        f.Add(Event(4000, fx::Kind::Amp, 1.0f));
        f.Add(Event(6000, fx::Kind::OnOff, 0.0f)); // the Walking again (its clock went on meanwhile)
        Fold(track, 10000, "both: the events over the markers' shake and back");

        Track inside;
        inside.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear, true), ShakeType::Handheld, 2));
        inside.AddMarker(Legacy(FreeAt(4000, {9, 0, 0}, Blend::None), ShakeType::Handheld, 9));
        inside.LegacyEffects().Add(Event(1000, fx::Kind::Profile, 3.0f));
        inside.LegacyEffects().Add(Event(2000, fx::Kind::OnOff, 0.0f)); // back inside the markers' blend
        const mshake::Proof proof = Fold(inside, 5000, "both: back inside the markers' blend (counted inexact)", {}, true);
        Check(proof.pathPosition < 1e-6, "both: the camera path stays the same even there");
    }

    // ---- 5. Formats: CAMR v9, the old payloads, the refusal of a newer one.

    using testpayload::Put;
    using testpayload::OldCamera;

    std::uint64_t Fnv(const std::uint8_t* data, std::size_t size)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (std::size_t n = 0; n < size; ++n)
        {
            hash ^= data[n];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    // A clip file with its CAMR section replaced (the old builds' clip).
    std::vector<std::uint8_t> WithCamera(const std::vector<std::uint8_t>& file, std::uint16_t version, const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> out(file.begin(), file.begin() + 20);
        for (std::size_t offset = 20; offset + 14 <= file.size();)
        {
            std::uint32_t tag = 0;
            std::uint64_t length = 0;
            std::memcpy(&tag, file.data() + offset, 4);
            std::memcpy(&length, file.data() + offset + 6, 8);
            const std::size_t next = offset + 14 + static_cast<std::size_t>(length) + 8;
            if (tag == 0x524D4143u)
            {
                Put(out, tag);
                Put(out, version);
                Put<std::uint64_t>(out, payload.size());
                out.insert(out.end(), payload.begin(), payload.end());
                Put(out, Fnv(payload.data(), payload.size()));
            }
            else
                out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(next));
            offset = next;
        }
        return out;
    }

    void TestFormats()
    {
        Check(clipfile::CameraSectionVersion() == 9, "CAMR: version 9 (the per-marker shake)");
        Track old;
        old.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear, true), ShakeType::Handheld, 3));
        Keyframe c = Legacy(FreeAt(1500, {4, 0, 0}, Blend::Linear, true), ShakeType::Custom, 6, 6);
        c.legacyShake.custom.amp = 1.25f;
        c.legacyShake.custom.seed = 42;
        c.legacyShake.custom.still = 1;
        old.AddMarker(c);
        old.AddMarker(Legacy(FreeAt(3000, {8, 1, 0}, Blend::None), ShakeType::Running, 8, 4, editor::ShakeMotion::Speed));
        Track direct = old;
        mshake::Migrate(direct);
        for (std::uint16_t version : {std::uint16_t{6}, std::uint16_t{7}, std::uint16_t{8}})
        {
            const std::vector<std::uint8_t> payload = OldCamera(old, version);
            Track back;
            std::string error;
            const bool read = clipfile::DecodeCameraTrack(payload.data(), payload.size(), version, back, error);
            bool same = read && back.Keyframes().size() == direct.Keyframes().size() && back.HasLegacyShake();
            mshake::Migrate(back);
            for (std::size_t n = 0; same && n < back.Keyframes().size(); ++n)
                same = mshake::Same(back.Keyframes()[n].shake, direct.Keyframes()[n].shake);
            char what[96];
            std::snprintf(what, sizeof(what), "CAMR v%u: the old marker shake read and folded", static_cast<unsigned>(version));
            Check(same, what);
        }
        // v4/v5 (no Custom, no motion): read with the defaults.
        {
            Track noCustom;
            noCustom.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear), ShakeType::Drunk, 4));
            noCustom.AddMarker(Legacy(FreeAt(2000, {3, 0, 0}, Blend::None), ShakeType::Drunk, 9));
            for (std::uint16_t version : {std::uint16_t{4}, std::uint16_t{5}})
            {
                const std::vector<std::uint8_t> payload = OldCamera(noCustom, version);
                Track back;
                std::string error;
                Check(clipfile::DecodeCameraTrack(payload.data(), payload.size(), version, back, error) &&
                          back.Keyframes()[1].legacyShake.type == ShakeType::Drunk && back.Keyframes()[1].legacyShake.intensity == 9,
                    "CAMR v4/v5: the old marker shake read");
            }
        }
        // v9 round trip of the marker's shake and its origin.
        const std::vector<std::uint8_t> v9 = clipfile::EncodeCameraTrack(direct);
        Track back;
        std::string error;
        bool same = clipfile::DecodeCameraTrack(v9.data(), v9.size(), 9, back, error) && back.Keyframes().size() == direct.Keyframes().size();
        for (std::size_t n = 0; same && n < back.Keyframes().size(); ++n)
            same = mshake::Same(back.Keyframes()[n].shake, direct.Keyframes()[n].shake) &&
                back.Keyframes()[n].origin == direct.Keyframes()[n].origin && !back.HasLegacyShake();
        Check(same, "CAMR v9: the marker's shake kept exactly");
        // A build from before reads at most v8: what it does with v9 is
        // refuse it (the version check), never read the bytes as v8. And
        // this build refuses a v10.
        Check(!clipfile::DecodeCameraTrack(v9.data(), v9.size(), 8, back, error), "CAMR v9 bytes are not v8 bytes (refused)");
        Check(!clipfile::DecodeCameraTrack(v9.data(), v9.size(), 10, back, error) && error.find("version 10") != std::string::npos,
            "CAMR v10 refused by version");
        // A bad shake in a v9 payload is refused.
        {
            Track badTrack = direct;
            badTrack.FindMutable(badTrack.Keyframes()[0].id)->shake.profile = 11;
            const std::vector<std::uint8_t> bad = clipfile::EncodeCameraTrack(badTrack);
            Check(!clipfile::DecodeCameraTrack(bad.data(), bad.size(), 9, back, error), "CAMR v9: a profile out of range is refused");
        }

        // A whole old clip (CAMR v8 with the old shake, CFXE v3): opens,
        // folded, and a save writes v9 and no CFXE.
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        Track withEvents = old;
        withEvents.LegacyEffects().Add(Event(500, fx::Kind::Amp, 0.7f));
        withEvents.LegacyEffects().Add(Event(500, fx::Kind::OnOff, 1.0f));
        withEvents.LegacyEffects().Add(Event(1000, fx::Kind::OnOff, 0.0f));
        std::vector<std::uint8_t> file;
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kSize = 0x10000000ull;
        Check(clipfile::Encode(recording, poses, withEvents, 4000, kBase, kSize, file, error), "clip: encode with events waiting");
        const std::vector<std::uint8_t> oldFile = WithCamera(file, 8, OldCamera(old, 8));
        Recording loadedRecording;
        std::vector<pose::PoseRecordExport> loadedPoses;
        Track loaded;
        std::uint64_t duration = 0;
        const bool opened = clipfile::Decode(oldFile.data(), oldFile.size(), kBase, kSize, loadedRecording, loadedPoses, loaded, duration, error);
        Check(opened && !loaded.HasLegacyShake() && loaded.Migration().ran && loaded.Migration().events == 3 &&
                  loaded.Migration().legacyMarkers == 3,
            "clip: an old clip opens with its shake folded");
        if (opened && loaded.Migration().before)
        {
            const mshake::Proof proof = mshake::Verify(*loaded.Migration().before, loaded, {}, 4000, 0);
            Check(proof.Exact() && loaded.Migration().inexact == 0, "clip: the old clip plays the same (proof)");
            g_worstShake = std::max({g_worstShake, proof.shakeRotation, proof.shakePosition});
            ++g_cases;
        }
        std::vector<std::uint8_t> saved;
        Check(clipfile::Encode(loadedRecording, loadedPoses, loaded, duration, kBase, kSize, saved, error), "clip: saves again");
        bool cfxe = false;
        std::uint16_t camr = 0;
        for (std::size_t offset = 20; offset + 14 <= saved.size();)
        {
            std::uint32_t tag = 0;
            std::uint16_t version = 0;
            std::uint64_t length = 0;
            std::memcpy(&tag, saved.data() + offset, 4);
            std::memcpy(&version, saved.data() + offset + 4, 2);
            std::memcpy(&length, saved.data() + offset + 6, 8);
            cfxe = cfxe || tag == 0x45584643u;
            if (tag == 0x524D4143u)
                camr = version;
            offset += 14 + static_cast<std::size_t>(length) + 8;
        }
        Check(!cfxe && camr == 9, "clip: a save writes CAMR v9 and no events section");
    }

    // ---- 6. The director's and the bench's fields.
    void TestFields()
    {
        MarkerShake s = editor::DefaultMarkerShake();
        bool known = false;
        std::string error;
        Check(mshake::ParseField("shake", "earthquake", s, known, error) && known && s.on && s.profile == 0 && s.amp == 1.5f,
            "fields: a Simple Camera preset");
        Check(mshake::ParseField("shake", "horseback", s, known, error) && s.profile == static_cast<std::uint8_t>(ShakeType::Horseback) &&
                  s.amp == 1.0f && s.hz == 4.0f,
            "fields: a profile");
        Check(mshake::ParseField("shake_i", "8", s, known, error) && s.gain == camera::shake::Gain(8), "fields: the intensity");
        Check(mshake::ParseField("shake_transition", "blend", s, known, error) && s.transition == 1 && s.blendMask == editor::ShakeBlendAll,
            "fields: the transition");
        Check(!mshake::ParseField("shake_hz", "99", s, known, error) && known, "fields: out of range refused");
        Check(mshake::ParseField("dof", "custom", s, known, error) && !known, "fields: another key is not the shake's");
        Check(mshake::ParseField("shake", "off", s, known, error) && !s.on && mshake::FormatFields(s) == "shake=off",
            "fields: off");
    }

    // ---- 7. The one shot (QA: "a camera marker for a gunshot and an
    // explosion shake, it just doesn't shake at all"). Explosion is an impulse
    // that settles in about 3 s; it went on from the clock where the shake
    // turned on, so a marker set to it after another shake played the tail
    // of a blast long run out. It fires at a marker that sets it.

    MarkerShake Profile(ShakeType type, std::uint8_t intensity = 5)
    {
        MarkerShake s = editor::DefaultMarkerShake();
        mshake::ApplyProfile(s, static_cast<std::uint8_t>(type));
        s.gain = mshake::GainOf(intensity);
        return s;
    }

    Keyframe WithShake(Keyframe k, const MarkerShake& s)
    {
        k.shake = s;
        return k;
    }

    // The largest rotation the shake adds in [from, to), each 10 ms.
    double PeakRotation(const Track& track, std::uint64_t from, std::uint64_t to)
    {
        double peak = 0.0;
        for (std::uint64_t t = from; t < to; t += 10)
        {
            const camera::ShakeDelta d = mshake::Delta(track, t, {});
            peak = std::max({peak, std::fabs(static_cast<double>(d.rotation.x)), std::fabs(static_cast<double>(d.rotation.y)),
                std::fabs(static_cast<double>(d.rotation.z))});
        }
        return peak;
    }

    void TestOneShot()
    {
        Check(camera::shake::OneShot(ShakeType::Explosion) && !camera::shake::OneShot(ShakeType::Handheld) &&
                  !camera::shake::OneShot(ShakeType::GroundVibration) && !camera::shake::OneShot(ShakeType::Custom),
            "one shot: Explosion only");

        // The trigger decision.
        const MarkerShake blast = Profile(ShakeType::Explosion, 5);
        MarkerShake off = blast;
        off.on = 0;
        MarkerShake louder = Profile(ShakeType::Explosion, 8);
        MarkerShake faster = blast;
        faster.rate = 2.0f;
        MarkerShake ramp = blast;
        ramp.transition = static_cast<std::uint8_t>(editor::ShakeTransition::Blend);
        ramp.blendMask = editor::ShakeBlendGain;
        const MarkerShake handheld = Profile(ShakeType::Handheld);
        Check(mshake::FiresImpulse(nullptr, blast) && mshake::FiresImpulse(&off, blast) && mshake::FiresImpulse(&handheld, blast),
            "one shot: fires where the shake turns on or switches to it");
        Check(!mshake::FiresImpulse(&blast, blast) && !mshake::FiresImpulse(&blast, mshake::Continued(blast)),
            "one shot: the plain continuation does not fire again");
        Check(mshake::FiresImpulse(&blast, louder) && mshake::FiresImpulse(&blast, faster),
            "one shot: another intensity or speed is another blast");
        Check(!mshake::FiresImpulse(&ramp, louder), "one shot: a blend into the marker is a continuation");
        Check(!mshake::FiresImpulse(nullptr, handheld) && !mshake::FiresImpulse(&blast, handheld) && !mshake::FiresImpulse(nullptr, off),
            "one shot: a continuous profile or off never fires");

        // The QA case: Handheld from the start, Explosion at 5 s, the shake
        // off at 10 s. The blast shows right after its marker.
        Track qa;
        qa.AddMarker(WithShake(FreeAt(0, {0, 0, 0}, Blend::None), handheld));
        qa.AddMarker(WithShake(FreeAt(5000, {0, 0, 0}, Blend::None), blast));
        qa.AddMarker(WithShake(FreeAt(10000, {0, 0, 0}, Blend::None), off));
        const double handheldPeak = PeakRotation(qa, 1000, 5000);
        const double hit = PeakRotation(qa, 5000, 5600);
        Check(hit > 1.0 && hit > 2.0 * handheldPeak, "one shot: Explosion after Handheld kicks over 1 deg at its marker");
        Check(std::fabs(mshake::Resolve(qa, 5000).cycles) < 1e-12 && mshake::Resolve(qa, 5000).originMs == 5000,
            "one shot: its clock starts at its marker");
        Check(PeakRotation(qa, 9000, 10000) < 0.01 * hit, "one shot: settled before the next marker");

        // A gunshot and an explosion, one after the other: two blasts.
        Track two;
        two.AddMarker(WithShake(FreeAt(0, {0, 0, 0}, Blend::None), off));
        two.AddMarker(WithShake(FreeAt(2000, {0, 0, 0}, Blend::None), Profile(ShakeType::Explosion, 2)));
        two.AddMarker(WithShake(FreeAt(6000, {0, 0, 0}, Blend::None), louder));
        const double shot = PeakRotation(two, 2000, 2600);
        const double big = PeakRotation(two, 6000, 6600);
        Check(shot > 0.2 && big > 1.5 && big > 3.0 * shot && PeakRotation(two, 5500, 6000) < 0.01 * shot,
            "one shot: the gunshot and the explosion each kick at their marker");

        // A camera marker added inside the blast (HeldAt) goes on with it.
        Track inside = two;
        Keyframe added = WithShake(FreeAt(6300, {1, 0, 0}, Blend::None), mshake::HeldAt(two, 6300));
        inside.AddMarker(added);
        bool same = true;
        for (std::uint64_t t = 6000; t < 7500; t += 10)
        {
            const camera::ShakeDelta a = mshake::Delta(two, t, {}), b = mshake::Delta(inside, t, {});
            same = same && std::memcmp(&a.rotation, &b.rotation, sizeof(a.rotation)) == 0;
        }
        Check(same, "one shot: a marker added inside the blast does not fire it again");
        // Restart Here on it fires it again.
        inside.FindMutable(inside.Keyframes()[3].id)->shake.flags |= editor::ShakeFlagRestart;
        Check(std::fabs(mshake::Resolve(inside, 6300).cycles) < 1e-12 && PeakRotation(inside, 6300, 6900) > 1.5,
            "one shot: Restart Here fires it again");

        // Scrub equals play: each frame in order, then the same frames
        // backwards and at random, give the same shake.
        std::vector<camera::ShakeDelta> play;
        for (std::uint64_t t = 0; t < 8000; t += 33)
            play.push_back(mshake::Delta(two, t, {}));
        bool scrub = true;
        for (std::size_t n = play.size(); n-- > 0;)
        {
            const camera::ShakeDelta d = mshake::Delta(two, n * 33, {});
            scrub = scrub && std::memcmp(&d, &play[n], sizeof(d)) == 0;
        }
        std::uint32_t lcg = 12345;
        for (int seek = 0; seek < 200; ++seek)
        {
            lcg = lcg * 1664525u + 1013904223u;
            const std::size_t n = (lcg >> 8) % play.size();
            const camera::ShakeDelta d = mshake::Delta(two, n * 33, {});
            scrub = scrub && std::memcmp(&d, &play[n], sizeof(d)) == 0;
        }
        Check(scrub, "one shot: play, scrub backwards and seeks give the same shake");

        // The old clips keep their look: the old rules went on with the clock
        // over a second Explosion marker of another intensity; the migration
        // keeps that phase with a Restart there (proved frame by frame).
        Track legacy;
        legacy.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::None), ShakeType::Explosion, 4));
        legacy.AddMarker(Legacy(FreeAt(1500, {3, 0, 0}, Blend::None), ShakeType::Explosion, 9));
        legacy.AddMarker(Legacy(FreeAt(3000, {6, 0, 0}, Blend::None), ShakeType::Handheld, 5));
        legacy.AddMarker(Legacy(FreeAt(4500, {6, 2, 0}, Blend::None), ShakeType::Explosion, 7));
        Fold(legacy, 6000, "one shot: old Explosion markers keep their phase");
        int pinned = 0;
        for (const Keyframe& k : legacy.Keyframes())
            if (k.shake.on && k.shake.profile == static_cast<std::uint8_t>(ShakeType::Explosion) &&
                (k.shake.flags & editor::ShakeFlagRestart) != 0 && k.shake.startCycles > 0.0)
                ++pinned;
        Check(pinned >= 1, "one shot: the migration pins the old phase as a Restart");
    }

    // ---- 8. Gunshot: a short recoil kick (one shot, about 0.3 s), fired at
    // its marker like Explosion; stored in CAMR v9 so an older build opens it.
    void TestGunshot()
    {
        namespace shake = camera::shake;
        Check(shake::OneShot(ShakeType::Gunshot) && static_cast<int>(ShakeType::Gunshot) == 12 &&
                  static_cast<int>(ShakeType::Custom) == 11 && editor::kShakeTypeCount == 13,
            "gunshot: a one shot, id 12 after Custom (the old ids keep their values)");

        // The curve at intensity 5: a clear but small kick, early, gone by 0.35 s.
        double peak = 0.0, peakAt = 0.0, tail = 0.0, explosion = 0.0;
        for (int us = 500; us < 3000000; us += 500)
        {
            const double t = us / 1e6;
            const shake::Sample g = shake::Evaluate(ShakeType::Gunshot, 1.0f, t);
            const shake::Sample e = shake::Evaluate(ShakeType::Explosion, 1.0f, t);
            const double r = std::max({std::fabs(g.value[0]), std::fabs(g.value[1]), std::fabs(g.value[2])});
            if (r > peak)
            {
                peak = r;
                peakAt = t;
            }
            if (t >= 0.35)
                tail = std::max(tail, r);
            explosion = std::max({explosion, static_cast<double>(std::fabs(e.value[0])), static_cast<double>(std::fabs(e.value[1])),
                static_cast<double>(std::fabs(e.value[2]))});
        }
        Check(peak > 0.3 && peak < 0.8 && peak < 0.5 * explosion, "gunshot: intensity 5 kicks 0.3 to 0.8 deg, under half an explosion");
        Check(peakAt < 0.08 && tail < 0.01 * peak, "gunshot: the kick within 80 ms, below 1 % of it after 0.35 s");
        const shake::Sample first = shake::Evaluate(ShakeType::Gunshot, 1.0f, 0.001);
        Check(std::fabs(first.value[0]) < 0.05f && shake::Evaluate(ShakeType::Gunshot, 1.0f, 0.0).value[0] == 0.0f,
            "gunshot: starts from zero");
        const shake::Sample loud = shake::Evaluate(ShakeType::Gunshot, shake::Gain(10), 0.03);
        const shake::Sample mid = shake::Evaluate(ShakeType::Gunshot, shake::Gain(5), 0.03);
        Check(std::fabs(loud.value[0] / mid.value[0] - shake::Gain(10)) < 1e-4f, "gunshot: scales with the intensity gain");

        // The state's rules take profile 12 (11 is the Custom type, never a profile).
        MarkerShake g = Profile(ShakeType::Gunshot);
        Check(g.profile == 12 && mshake::Usable(g) && mshake::Same(g, mshake::Clamped(g)), "gunshot: a usable profile");
        MarkerShake eleven = g;
        eleven.profile = 11;
        Check(!mshake::Usable(eleven) && mshake::Clamped(eleven).profile == 0, "gunshot: profile 11 is still refused");
        Check(mshake::Describe(g) == "gunshot" && mshake::SummaryType(g) == ShakeType::Gunshot, "gunshot: its name");
        MarkerShake parsed = editor::DefaultMarkerShake();
        bool known = false;
        std::string error;
        Check(mshake::ParseField("shake", "gunshot", parsed, known, error) && parsed.profile == 12 && parsed.on &&
                  mshake::FormatFields(parsed).rfind("shake=gunshot ", 0) == 0,
            "gunshot: the director's and the bench's field");

        // At its marker: after Handheld, after Explosion, and again with Restart Here.
        const MarkerShake handheld = Profile(ShakeType::Handheld);
        const MarkerShake blast = Profile(ShakeType::Explosion);
        Check(mshake::FiresImpulse(&handheld, g) && mshake::FiresImpulse(&blast, g) && mshake::FiresImpulse(&g, blast) &&
                  !mshake::FiresImpulse(&g, g) && mshake::FiresImpulse(&g, Profile(ShakeType::Gunshot, 8)),
            "gunshot: fires on a switch or another intensity, not on its continuation");
        Track track;
        track.AddMarker(WithShake(FreeAt(0, {0, 0, 0}, Blend::None), handheld));
        track.AddMarker(WithShake(FreeAt(4000, {0, 0, 0}, Blend::None), g));
        track.AddMarker(WithShake(FreeAt(6000, {0, 0, 0}, Blend::None), g));
        const double kick = PeakRotation(track, 4000, 4200);
        Check(kick > 0.3 && mshake::Resolve(track, 4000).originMs == 4000, "gunshot: kicks at its marker after Handheld");
        Check(PeakRotation(track, 4500, 6000) < 0.01 * kick && PeakRotation(track, 6000, 6400) < 0.01 * kick,
            "gunshot: settled, and the continuation does not fire");
        track.FindMutable(track.Keyframes()[2].id)->shake.flags |= editor::ShakeFlagRestart;
        Check(PeakRotation(track, 6000, 6200) > 0.3, "gunshot: Restart Here fires it again");
        bool scrub = true;
        std::vector<camera::ShakeDelta> play;
        for (std::uint64_t t = 3900; t < 6500; t += 10)
            play.push_back(mshake::Delta(track, t, {}));
        for (std::size_t n = play.size(); n-- > 0;)
        {
            const camera::ShakeDelta d = mshake::Delta(track, 3900 + n * 10, {});
            scrub = scrub && std::memcmp(&d, &play[n], sizeof(d)) == 0;
        }
        Check(scrub, "gunshot: scrub backwards equals play");

        // CAMR v9: written as Explosion with the tag seed (an older build reads
        // an Explosion and opens the clip), read back as Gunshot.
        Track tagged = track;
        for (const Keyframe& k : track.Keyframes())
            if (k.shake.profile == 12)
            {
                Keyframe* m = tagged.FindMutable(k.id);
                m->shake.profile = static_cast<std::uint8_t>(ShakeType::Explosion);
                m->shake.seed = 0x6A57;
            }
        const std::vector<std::uint8_t> bytes = clipfile::EncodeCameraTrack(track);
        Check(bytes == clipfile::EncodeCameraTrack(tagged), "gunshot: on disk an Explosion with the tag seed");
        Track back;
        Check(clipfile::DecodeCameraTrack(bytes.data(), bytes.size(), 9, back, error) && back.Keyframes().size() == 3 &&
                  back.Keyframes()[1].shake.profile == 12 && back.Keyframes()[2].shake.profile == 12 &&
                  back.Keyframes()[0].shake.profile == static_cast<std::uint8_t>(ShakeType::Handheld) &&
                  (back.Keyframes()[2].shake.flags & editor::ShakeFlagRestart) != 0,
            "gunshot: read back as Gunshot");
        // An older build's reader: profiles 0..10 only; the bytes it sees hold 5.
        bool olderReads = true;
        for (const Keyframe& k : tagged.Keyframes())
            olderReads = olderReads && k.shake.profile <= 10;
        Check(olderReads, "gunshot: the stored profile is one an older build reads");
    }
}

namespace
{
    // Finding 3 of the release review: the public build logs the migration's
    // counts with proof=skipped (the proof is the developer build's), and the
    // proof's samples are the same walked as they were stored (each 30 Hz
    // frame or step, and every marker with 1 ms either side, each once).
    void TestProofCost()
    {
        Track track;
        track.AddMarker(Legacy(FreeAt(0, {0, 0, 0}, Blend::Linear, true, 0), ShakeType::Handheld, 3));
        track.AddMarker(Legacy(FreeAt(2000, {8, 0, 1}, Blend::None, true, 20), ShakeType::Handheld, 8));
        mshake::Migrate(track);
        const camera::ShakeMigrationReport& report = track.Migration();
        Check(report.ran && report.before != nullptr, "proof cost: migrated");
        if (!report.before)
            return;
        // 0..1000 at 30 Hz is 31 frames; the marker at 0 adds 1 ms (0 and its
        // -1 are frame 0); the one at 2000 is past the duration.
        Check(mshake::Verify(*report.before, track, {}, 1000, 0).samples == 32, "proof cost: 31 frames and 1 ms after a marker");
        // Every 500 ms: 0, 500, 1000 and 1; and to 2001 the markers' 1999, 2000, 2001 too.
        Check(mshake::Verify(*report.before, track, {}, 1000, 500).samples == 4, "proof cost: steps and the marker's neighbour");
        Check(mshake::Verify(*report.before, track, {}, 2001, 500).samples == 8, "proof cost: each time once");
        mshake::Proof skipped;
        skipped.skipped = true;
        const std::string line = mshake::FormatLine(report, skipped);
        Check(line.rfind("SHAKE_MIGRATION,legacy_markers=", 0) == 0 &&
                  line.find(",inexact=" + std::to_string(report.inexact) + ",proof=skipped") != std::string::npos &&
                  line.find("samples=") == std::string::npos && line.find("exact=") == line.find("inexact=") + 2,
            "proof cost: the public line keeps the counts, the proof marked skipped");
        const std::string full = mshake::FormatLine(report, mshake::Verify(*report.before, track, {}, 1000, 0));
        Check(full.find("samples=32") != std::string::npos && full.find("proof=") == std::string::npos,
            "proof cost: the developer line as it was");
    }
}

int RunCameraMarkerShakeTests()
{
    TestProofCost();
    TestModel();
    TestLegacyMarkers();
    TestLegacyEvents();
    TestBoth();
    TestFormats();
    TestFields();
    TestOneShot();
    TestGunshot();
    std::printf("  shake-markers: %d migrations proved, worst shake diff %.3g, worst path diff %.3g\n", g_cases, g_worstShake,
        g_worstPath);
    if (g_failures == 0)
        std::puts("shake-markers (the shake in the markers, the migration and its proof, CAMR v9) tests passed");
    return g_failures;
}
