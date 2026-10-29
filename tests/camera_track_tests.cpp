// Marker model, rules and evaluator (API 8, CAMR v3): the pure camera code.
#include "camera_track.h"
#include "camera_marker_shake.h"
#include "camera_shake.h"
#include "clip_file.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using camera::Keyframe;
    using camera::Track;
    using editor::Vec3;

    int g_cameraFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_cameraFailures;
    }

    bool Near(float actual, float expected, float tolerance = 0.02f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    bool NearAngle(float actual, float expected, float tolerance = 0.05f)
    {
        float delta = std::fmod(actual - expected + 540.0f, 360.0f) - 180.0f;
        return std::fabs(delta) <= tolerance;
    }

    Keyframe Marker(std::uint64_t timeMs, camera::CameraMode mode, Vec3 position, Vec3 rotation = {}, float fov = 45.0f)
    {
        Keyframe k;
        k.timeMs = timeMs;
        k.cameraMode = mode;
        k.pose = {position, Track::FromEulerDegrees(rotation), fov};
        return k;
    }

    Keyframe Free(std::uint64_t timeMs, Vec3 position, camera::Blend blend = camera::Blend::None, bool ease = true)
    {
        Keyframe k = Marker(timeMs, camera::CameraMode::Free, position);
        k.blend = blend;
        k.ease = ease;
        return k;
    }

    editor::CameraState Eval(const Track& track, std::uint64_t t, const camera::Scene& scene = {})
    {
        editor::CameraState state{};
        camera::EvaluateMarkers(track, t, scene, state);
        return state;
    }

    // 1. The blend lives on the source marker.
    void TestBlendOnSource()
    {
        Track linear;
        linear.AddMarker(Free(0, {0, 0, 0}, camera::Blend::Linear, false));
        linear.AddMarker(Free(1000, {10, 0, 0}, camera::Blend::None, false));
        Check(Near(Eval(linear, 500).position.x, 5.0f), "Linear blend on A: halfway is the average");

        Track cut;
        cut.AddMarker(Free(0, {0, 0, 0}, camera::Blend::None));
        cut.AddMarker(Free(1000, {10, 0, 0}, camera::Blend::Linear));
        cut.AddMarker(Free(2000, {20, 0, 0}));
        Check(Near(Eval(cut, 500).position.x, 0.0f) && Near(Eval(cut, 999).position.x, 0.0f),
            "No Blend on A: A's pose until B");
        Check(Near(Eval(cut, 1000).position.x, 10.0f), "No Blend on A: B at B's time");
    }

    // 2. Free to Game is a cut even with a blend asked for.
    void TestFreeToGameIsCut()
    {
        const std::vector<camera::GameSample> game = {{0, {50, 0, 0}, {0, 0, 0}, 60.0f}};
        camera::Scene scene;
        scene.game = &game;
        Track track;
        track.AddMarker(Free(0, {0, 0, 0}, camera::Blend::Linear));
        track.AddMarker(Marker(1000, camera::CameraMode::Game, {}));
        track.AddMarker(Free(2000, {5, 0, 0}));
        Check(Near(Eval(track, 500, scene).position.x, 0.0f), "Free to Game: no blend before B");
        Check(Near(Eval(track, 1000, scene).position.x, 50.0f) && Near(Eval(track, 1000, scene).fov, 60.0f),
            "Game marker plays the recorded camera");
        const camera::SegmentPlan plan = camera::Plan(track, 500);
        Check(!plan.blending, "the plan says cut");

        Track legacy;
        legacy.AddMarker(Free(0, {0, 0, 0}, camera::Blend::Linear, false));
        legacy.AddMarker(Marker(1000, camera::CameraMode::Game, {}));
        legacy.SetLegacyBlendAnyMode(true);
        legacy.FindMutable(legacy.Keyframes()[1].id)->ease = false;
        Check(Near(Eval(legacy, 500, scene).position.x, 25.0f), "legacyBlendAnyMode blends Free to Game");
    }

    // 3. The six rows of the chain ease table.
    void TestChainEase()
    {
        using camera::Blend;
        using camera::EaseCurve;
        const Keyframe blendEase = Free(0, {}, Blend::Linear, true);
        const Keyframe blendNoEase = Free(0, {}, Blend::Linear, false);
        const Keyframe easeNoBlend = Free(0, {}, Blend::None, true);
        const Keyframe plain = Free(0, {}, Blend::None, false);
        const Keyframe source = Free(0, {}, Blend::Linear, true);
        const Keyframe sourceNoEase = Free(0, {}, Blend::Linear, false);
        Check(camera::ChainEase(&blendEase, source, &blendNoEase) == EaseCurve::Out &&
                camera::ChainEase(&blendEase, source, &easeNoBlend) == EaseCurve::Out,
            "chain ease row 1: end of chain brakes");
        Check(camera::ChainEase(&blendEase, source, &blendEase) == EaseCurve::Linear, "chain ease row 2: middle linear");
        Check(camera::ChainEase(nullptr, source, &easeNoBlend) == EaseCurve::InOut &&
                camera::ChainEase(&plain, source, &easeNoBlend) == EaseCurve::InOut,
            "chain ease row 3: one segment chain");
        Check(camera::ChainEase(nullptr, source, &blendEase) == EaseCurve::In, "chain ease row 4: start accelerates");
        Check(camera::ChainEase(&blendEase, sourceNoEase, &easeNoBlend) == EaseCurve::Out,
            "chain ease row 5: no ease, destination brakes");
        Check(camera::ChainEase(&blendEase, sourceNoEase, &blendEase) == EaseCurve::Linear,
            "chain ease row 6: linear");
        Check(Near(camera::ApplyEase(EaseCurve::In, 0.0f), 0.0f) && Near(camera::ApplyEase(EaseCurve::In, 1.0f), 1.0f) &&
                camera::ApplyEase(EaseCurve::In, 0.5f) < 0.5f && camera::ApplyEase(EaseCurve::Out, 0.5f) > 0.5f &&
                Near(camera::ApplyEase(EaseCurve::InOut, 0.5f), 0.5f),
            "ease curve shapes");
    }

    // 4. A new marker inherits from the previous one.
    void TestInheritance()
    {
        Keyframe freePrev = Free(0, {1, 2, 3}, camera::Blend::Smooth, false);
        freePrev.lookAtTarget = 7;
        freePrev.attachTarget = 9;
        freePrev.attachType = camera::AttachType::Flexible;
        freePrev.legacyShake.type = camera::ShakeType::Drunk;
        freePrev.editedMask = 0xFF;
        Keyframe fresh;
        fresh.timeMs = 500;
        const bool fromFrame = camera::InheritFrom(fresh, freePrev);
        Check(fromFrame && fresh.lookAtTarget == 7 && fresh.attachTarget == 9 &&
                fresh.attachType == camera::AttachType::Flexible && fresh.blend == camera::Blend::Smooth && !fresh.ease &&
                fresh.legacyShake.type == camera::ShakeType::Drunk && fresh.editedMask == 0 && fresh.timeMs == 500,
            "free previous: targets and blend copied, pose from the rendered frame");

        Keyframe preset = Marker(0, camera::CameraMode::Rear, {4, 5, 6});
        preset.blend = camera::Blend::Linear;
        preset.ease = false;
        Keyframe afterPreset;
        const bool presetFromFrame = camera::InheritFrom(afterPreset, preset);
        Check(!presetFromFrame && afterPreset.blend == camera::Blend::Spline && afterPreset.ease &&
                afterPreset.cameraMode == camera::CameraMode::Rear && Near(afterPreset.pose.position.x, 4.0f),
            "preset previous: blend back to the default (Spline), pose copied");

        Keyframe anchor = Free(0, {9, 9, 9});
        anchor.markerType = camera::MarkerType::Anchor;
        anchor.lookAtTarget = 3;
        Keyframe afterAnchor;
        afterAnchor.pose.position = {1, 1, 1};
        camera::InheritFrom(afterAnchor, anchor);
        Check(Near(afterAnchor.pose.position.x, 1.0f) && afterAnchor.lookAtTarget == 0,
            "anchor previous: no pose or targets");

        Keyframe reset = freePrev;
        reset.id = 4;
        reset.timeMs = 1234;
        camera::ResetToDefault(reset, false);
        Check(reset.id == 4 && reset.timeMs == 1234 && reset.cameraMode == camera::CameraMode::Game &&
                reset.lookAtTarget == 0 && Near(reset.pose.fov, 45.0f),
            "reset: defaults keeping the time");
        Keyframe resetLast = freePrev;
        camera::ResetToDefault(resetLast, true);
        Check(resetLast.cameraMode == camera::CameraMode::Free, "reset: the last marker keeps its camera type");

        Keyframe pasted = Marker(777, camera::CameraMode::Game, {});
        pasted.id = 12;
        Check(camera::Paste(pasted, freePrev, camera::CopyContext::All) && pasted.id == 12 && pasted.timeMs == 777 &&
                pasted.lookAtTarget == 7 && pasted.cameraMode == camera::CameraMode::Free,
            "paste all copies the options, not id or time");
        Keyframe graded = freePrev;
        graded.filter = 3;
        graded.saturation = -40;
        Keyframe effects = Marker(900, camera::CameraMode::Game, {});
        Check(camera::Paste(effects, graded, camera::CopyContext::EffectsOnly) && effects.filter == 3 &&
                effects.saturation == -40 && effects.cameraMode == camera::CameraMode::Game,
            "paste effects copies only the effects");
        Check(!camera::Paste(pasted, freePrev, camera::CopyContext::AudioOnly), "paste audio is a stub");
    }

    // 5. Look At and Attach exclusion rules.
    void TestLookAtAttachRules()
    {
        Keyframe k = Free(0, {});
        camera::SetAttach(k, 5);
        camera::SetLookAt(k, 7);
        Check(k.attachType == camera::AttachType::Flexible, "Look At on another target forces Flexible");
        camera::SetAttachType(k, camera::AttachType::Rigid);
        Check(k.lookAtTarget == 0 && k.attachType == camera::AttachType::Rigid, "Rigid clears a Look At on another target");
        Keyframe same = Free(0, {});
        camera::SetAttach(same, 5);
        camera::SetLookAt(same, 5);
        camera::SetAttachType(same, camera::AttachType::Rigid);
        Check(same.lookAtTarget == 5 && same.attachType == camera::AttachType::Rigid, "same target: follow and look");

        editor::CameraKeyframeOptions options{};
        options.mask = editor::CameraOptionMode;
        options.cameraMode = camera::CameraMode::Target;
        Keyframe target = Free(0, {});
        Check(!camera::ApplyOptions(target, options), "options refuse a legacy mode");
        options.mask = editor::CameraOptionFov | editor::CameraOptionLookAtTarget;
        options.fov = 150.0f;
        options.lookAtTarget = 8;
        Check(camera::ApplyOptions(target, options) && Near(target.pose.fov, 100.0f) &&
                target.lookAtTarget == 8 && (target.editedMask & editor::CameraOptionFov) != 0,
            "options clamp and mark edited");
        options.mask = editor::CameraOptionBlend;
        options.blend = static_cast<camera::Blend>(9);
        Check(!camera::ApplyOptions(target, options), "options refuse an enum out of range");

        editor::CameraKeyframe snapshot{};
        target.id = 3;
        camera::FillSnapshotKeyframe(target, snapshot);
        Check(snapshot.id == 3 && snapshot.lookAtTarget == 8 && snapshot.cameraMode == camera::CameraMode::Free &&
                Near(snapshot.camera.fov, 100.0f),
            "snapshot entry");
    }

    // 6. The last marker rule.
    void TestLastMarker()
    {
        Track track;
        track.AddMarker(Free(0, {0, 0, 0}, camera::Blend::None));
        const std::uint32_t last = track.AddMarker(Marker(1000, camera::CameraMode::Game, {}));
        camera::ApplyLastMarkerRule(track);
        Check(camera::Plan(track, 1500).current == 0 && Near(Eval(track, 1500).position.x, 0.0f),
            "last marker ignored without a blend before it");

        Keyframe* first = track.FindMutable(track.Keyframes()[0].id);
        first->blend = camera::Blend::Linear;
        first->lookAtTarget = 11;
        first->attachTarget = 12;
        camera::ApplyLastMarkerRule(track);
        const Keyframe* end = track.Find(last);
        Check(end->cameraMode == camera::CameraMode::Free && end->lookAtTarget == 11 && end->attachTarget == 12,
            "last marker becomes Free with the targets");
        Check(camera::Plan(track, 1500).current == 1, "last marker counts with a blend before it");
    }

    // Markers added in time order (the bench, and the editor adding at the
    // end) keep the blend set on each one while it was the last.
    void TestBlendSurvivesBeingLast()
    {
        using camera::Blend;
        Track track;
        const auto add = [&](const Keyframe& marker) {
            const std::uint32_t id = track.AddMarker(marker);
            camera::ApplyLastMarkerRule(track);
            return id;
        };
        add(Marker(0, camera::CameraMode::Game, {}));
        const std::uint32_t linear = add(Free(2000, {0, 0, 0}, Blend::Linear, false));
        Check(track.Find(linear)->blend == Blend::Linear, "the last marker keeps its blend");
        const std::uint32_t smooth = add(Free(4000, {10, 0, 0}, Blend::Smooth, true));
        add(Free(5500, {20, 0, 0}, Blend::None, true));
        add(Marker(6000, camera::CameraMode::FrontLow, {}));
        Check(track.Find(linear)->blend == Blend::Linear && track.Find(smooth)->blend == Blend::Smooth,
            "blends set while last survive the next markers");
        const camera::SegmentPlan middle = camera::Plan(track, 3000);
        Check(middle.blending && middle.current == 1 && middle.next == 2 && Near(middle.rawPhase, 0.5f),
            "linear blends in the middle of its segment");
        Check(Near(Eval(track, 3000).position.x, 5.0f, 0.5f), "linear segment moves the camera");
        const camera::SegmentPlan smoothPlan = camera::Plan(track, 5000);
        Check(smoothPlan.blending && smoothPlan.smooth && smoothPlan.next == 3, "smooth blends to a Free marker");
        Check(!camera::Plan(track, 5800).blending, "a No Blend marker holds up to the next one");

        // A Look At set on a marker added last survives (look=ped1 at 4000
        // after a Linear 2000).
        Track looked;
        const auto addTo = [&](const Keyframe& marker) {
            const std::uint32_t id = looked.AddMarker(marker);
            camera::ApplyLastMarkerRule(looked);
            return id;
        };
        addTo(Marker(0, camera::CameraMode::Game, {}));
        addTo(Free(2000, {0, 0, 0}, Blend::Linear, false));
        Keyframe lookMarker = Free(4000, {10, 0, 0}, Blend::Smooth, true);
        lookMarker.lookAtTarget = 0x0008004Bu;
        const std::uint32_t lookId = addTo(lookMarker);
        Check(looked.Find(lookId)->lookAtTarget == 0x0008004Bu, "the last marker keeps its own Look At");
        const std::uint32_t afterId = addTo(Free(5500, {20, 0, 0}, Blend::None, true));
        Check(looked.Find(afterId)->lookAtTarget == 0x0008004Bu, "an untargeted last marker takes the Look At before it");
        addTo(Marker(6000, camera::CameraMode::FrontLow, {}));
        Check(looked.Find(lookId)->lookAtTarget == 0x0008004Bu && looked.Find(afterId)->lookAtTarget == 0x0008004Bu,
            "Look At set while last survives the next markers");
        camera::Scene lookScene;
        lookScene.aspect = 16.0f / 9.0f;
        lookScene.lookup = [](std::uint32_t id, std::uint64_t, camera::EntityState& out) {
            if (id != 0x0008004Bu)
                return false;
            out = {};
            out.position = {15, 10, 0};
            return true;
        };
        const editor::CameraState aimed = Eval(looked, 5000, lookScene);
        const Vec3 aimAt{15, 10, camera::kLookAtAimHeight};
        editor::Vec2 onScreen{};
        Check(camera::ProjectToScreen(aimed, aimAt, lookScene.aspect, onScreen) && Near(onScreen.x, 0.0f, 0.02f) &&
                Near(onScreen.y, 0.0f, 0.02f),
            "the blend between two Look At markers frames the target");

        // The last marker's stored blend never blends out, and the chain
        // sees it as the end: the same ease as with No Blend.
        Track a;
        a.AddMarker(Free(0, {0, 0, 0}, Blend::Linear, true));
        a.AddMarker(Free(1000, {10, 0, 0}, Blend::Linear, true));
        Track b = a;
        a.FindMutable(a.Keyframes()[1].id)->blend = Blend::None;
        Check(camera::Plan(b, 500).curve == camera::Plan(a, 500).curve && Near(Eval(b, 500).position.x, Eval(a, 500).position.x),
            "a stored blend on the last marker does not change the ease into it");
        Check(!camera::Plan(b, 1500).blending && Near(Eval(b, 1500).position.x, 10.0f), "the last marker holds after it");
    }

    // Fade in on the first marker, fade out on the last.
    void TestFade()
    {
        Track track;
        Keyframe first = Free(500, {0, 0, 0});
        first.transition = 1;
        first.transitionMs = 1500;
        track.AddMarker(first);
        track.AddMarker(Free(3000, {0, 0, 0}));
        Keyframe anchor = Free(11800, {0, 0, 0});
        anchor.markerType = camera::MarkerType::Anchor;
        track.AddMarker(anchor);
        Keyframe last = Marker(11500, camera::CameraMode::Game, {});
        last.transition = 1;
        last.transitionMs = 1500;
        track.AddMarker(last);
        Check(Near(camera::FadeAmount(track, 0), 1.0f) && Near(camera::FadeAmount(track, 500), 1.0f),
            "black before and at the first marker");
        Check(Near(camera::FadeAmount(track, 800), 0.8f) && Near(camera::FadeAmount(track, 2000), 0.0f),
            "fade in over its length");
        Check(Near(camera::FadeAmount(track, 5000), 0.0f) && Near(camera::FadeAmount(track, 10000), 0.0f),
            "clear between the fades");
        Check(Near(camera::FadeAmount(track, 11000), 2.0f / 3.0f) && Near(camera::FadeAmount(track, 11500), 1.0f) &&
                Near(camera::FadeAmount(track, 11900), 1.0f),
            "fade out ends black on the last marker (an Anchor after it does not count)");
        track.FindMutable(track.Keyframes()[0].id)->transition = 0;
        Check(Near(camera::FadeAmount(track, 0), 0.0f), "no fade in without the transition");
        Track single;
        single.AddMarker(first);
        Check(Near(camera::FadeAmount(single, 1250), 0.5f) && Near(camera::FadeAmount(single, 9000), 0.0f),
            "one marker only fades in");
    }

    // 7. Anchor (exclusive) markers are skipped.
    void TestAnchorSkipped()
    {
        Track track;
        track.AddMarker(Free(0, {0, 0, 0}));
        Keyframe anchor = Free(500, {99, 0, 0});
        anchor.markerType = camera::MarkerType::Anchor;
        track.AddMarker(anchor);
        track.AddMarker(Free(1000, {10, 0, 0}, camera::Blend::Linear));
        track.AddMarker(Free(2000, {20, 0, 0}));
        const camera::SegmentPlan plan = camera::Plan(track, 700);
        Check(plan.current == 0 && plan.next == 2 && Near(Eval(track, 700).position.x, 0.0f), "anchor skipped by the plan");
    }

    // 8. Spacing and the 512 limit.
    void TestCanPlace()
    {
        Track track;
        track.AddMarker(Free(1000, {}));
        Check(!camera::CanPlace(track, 1032) && camera::CanPlace(track, 1033), "spacing 33 ms at 100 %");
        Check(!camera::CanPlace(track, 1000) && !camera::CanPlace(track, 968) && camera::CanPlace(track, 967),
            "spacing before the next marker");
        track.FindMutable(track.Keyframes()[0].id)->speedIndex = 0; // 5 %
        Check(!camera::CanPlace(track, 1001) && camera::CanPlace(track, 1002), "spacing 2 ms at 5 %");
        Check(track.AddMarker(Free(1001, {})) == 0, "AddMarker refuses a tight time");

        Track full;
        for (std::uint64_t n = 0; n < camera::kMaxMarkers; ++n)
            full.AddMarker(Free(n * 40, {}));
        Check(full.Keyframes().size() == 512 && !camera::CanPlace(full, 512 * 40 + 100), "512 markers at most");
        Check(full.MoveMarker(full.Keyframes().back().id, 512 * 40 + 100), "moving within the limit");
    }

    // 9. Attach Rigid, Default and Flexible on a rotated target.
    void TestAttach()
    {
        camera::EntityState state;
        state.position = {10, 0, 0};
        state.heading = 90.0f;
        state.pitch = 30.0f;
        camera::Scene scene;
        scene.lookup = [&](std::uint32_t id, std::uint64_t, camera::EntityState& out) {
            if (id != 5)
                return false;
            out = state;
            return true;
        };
        Keyframe k = Free(0, {0, 0, 0});
        k.attachTarget = 5;
        k.relPosition = {0, -3, 1};

        k.attachType = camera::AttachType::Rigid;
        camera::LivePose live = camera::Resolve(k, 0, scene);
        Vec3 rotation = Track::ToEulerDegrees(live.pose.orientation);
        Check(Near(live.pose.position.x, 13.098f) && Near(live.pose.position.y, 0.0f) &&
                Near(live.pose.position.z, -0.634f) && NearAngle(rotation.z, 90.0f) && NearAngle(rotation.x, 30.0f),
            "attach Rigid follows heading and pitch");

        k.attachType = camera::AttachType::Default;
        live = camera::Resolve(k, 0, scene);
        rotation = Track::ToEulerDegrees(live.pose.orientation);
        Check(Near(live.pose.position.x, 13.0f) && Near(live.pose.position.z, 1.0f) && NearAngle(rotation.z, 90.0f) &&
                NearAngle(rotation.x, 0.0f),
            "attach Default follows the heading only");

        k.attachType = camera::AttachType::Flexible;
        live = camera::Resolve(k, 0, scene);
        rotation = Track::ToEulerDegrees(live.pose.orientation);
        Check(Near(live.pose.position.x, 10.0f) && Near(live.pose.position.y, -3.0f) && NearAngle(rotation.z, 0.0f),
            "attach Flexible follows the position only");

        // The relative pose captured from a world pose puts it back.
        Keyframe captured = k;
        captured.attachType = camera::AttachType::Rigid;
        const editor::CameraState world{{4, 5, 6}, {10, 5, -30}, 45.0f};
        camera::CaptureAttachPose(captured, world, state);
        live = camera::Resolve(captured, 0, scene);
        rotation = Track::ToEulerDegrees(live.pose.orientation);
        Check(Near(live.pose.position.x, 4.0f) && Near(live.pose.position.y, 5.0f) && Near(live.pose.position.z, 6.0f) &&
                NearAngle(rotation.x, 10.0f, 0.1f) && NearAngle(rotation.y, 5.0f, 0.1f) && NearAngle(rotation.z, -30.0f, 0.1f),
            "attach capture round trip");

        // A missing target keeps the stored world pose.
        Keyframe missing = k;
        missing.attachTarget = 99;
        live = camera::Resolve(missing, 0, scene);
        Check(live.targetMissing && Near(live.pose.position.x, 0.0f), "missing attach target keeps the world pose");

        // A preset orbits the main character.
        camera::Scene preset = scene;
        preset.mainCharacter = 5;
        state.pitch = 0.0f;
        Keyframe overhead = Marker(0, camera::CameraMode::Overhead, {});
        live = camera::Resolve(overhead, 0, preset);
        Check(Near(live.pose.position.z, 8.0f) && Track::ToEulerDegrees(live.pose.orientation).x < -80.0f,
            "overhead preset looks down from above");
    }

    // 10. Look At framing.
    void TestLookAtScreenOffset()
    {
        const Vec3 camera{1, -2, 3};
        const Vec3 point{5, 8, 2};
        const float aspect = 16.0f / 9.0f;
        editor::Vec2 screen{};
        editor::CameraState state{camera, camera::LookAtWithScreenOffset(point, camera, {0, 0}, 50.0f, 0.0f, aspect), 50.0f};
        Check(camera::ProjectToScreen(state, point, aspect, screen) && Near(screen.x, 0.0f, 0.005f) &&
                Near(screen.y, 0.0f, 0.005f),
            "offset 0 centres the target");
        const Vec3 plain = camera::LookAtDegrees(camera, point);
        Check(NearAngle(state.rotation.x, plain.x) && NearAngle(state.rotation.z, plain.z), "offset 0 equals LookAtDegrees");
        state.rotation = camera::LookAtWithScreenOffset(point, camera, {1, 0}, 50.0f, 0.0f, aspect);
        // 1 degree at the edge of a 50 degree tall, 16:9 view is about 0.03 of the half width.
        Check(camera::ProjectToScreen(state, point, aspect, screen) && Near(screen.x, 1.0f, 0.03f) &&
                Near(screen.y, 0.0f, 0.03f),
            "offset (1, 0) puts the target at the right edge");
        state.rotation = camera::LookAtWithScreenOffset(point, camera, {0.5f, 0.3f}, 40.0f, 20.0f, aspect);
        state.fov = 40.0f;
        Check(camera::ProjectToScreen(state, point, aspect, screen) && Near(screen.x, 0.5f, 0.01f) &&
                Near(screen.y, 0.3f, 0.01f) && NearAngle(state.rotation.y, 20.0f),
            "offset and roll together");

        // Blending two markers that look at the same target keeps it framed.
        camera::EntityState target;
        target.position = {0, 10, 0};
        camera::Scene scene;
        scene.aspect = aspect;
        scene.lookup = [&](std::uint32_t, std::uint64_t, camera::EntityState& out) {
            out = target;
            return true;
        };
        Track track;
        Keyframe a = Free(0, {-5, 0, 0}, camera::Blend::Linear, false);
        a.lookAtTarget = 3;
        Keyframe b = Free(1000, {5, 0, 0}, camera::Blend::None, false);
        b.lookAtTarget = 3;
        b.lookAtOffset = {0.5f, 0};
        track.AddMarker(a);
        track.AddMarker(b);
        const editor::CameraState mid = Eval(track, 500, scene);
        const Vec3 aim{0, 10, camera::kLookAtAimHeight};
        Check(camera::ProjectToScreen(mid, aim, aspect, screen) && Near(screen.x, 0.25f, 0.01f),
            "same target blend recomputes the look at");
    }

    // 11. The Smooth spring.
    void TestSpring()
    {
        camera::SpringState spring;
        editor::CameraState start{{0, 0, 0}, {0, 0, 170}, 45};
        editor::CameraState goal{{10, 0, 0}, {0, 0, -170}, 45};
        spring.Step(start, 10.0f, 1.0f / 60.0f); // primes at the start
        float previous = 0.0f;
        bool monotonic = true;
        bool overshoot = false;
        editor::CameraState now{};
        for (int n = 0; n < 600; ++n)
        {
            now = spring.Step(goal, 10.0f, 1.0f / 60.0f);
            monotonic = monotonic && now.position.x >= previous - 0.0001f;
            overshoot = overshoot || now.position.x > 10.0001f;
            previous = now.position.x;
        }
        Check(monotonic && !overshoot && Near(now.position.x, 10.0f, 0.05f), "spring converges without overshoot");
        Check(NearAngle(now.rotation.z, -170.0f, 0.1f), "spring angles go the short way");
        spring.Reset();
        const editor::CameraState reset = spring.Step(start, 10.0f, 1.0f / 60.0f);
        Check(Near(reset.position.x, 0.0f, 0.0001f) && Near(reset.rotation.z, 170.0f, 0.0001f), "reset returns the target");

        // The plan's weights on a chain of Smooth segments.
        Track track;
        track.AddMarker(Free(0, {0, 0, 0}, camera::Blend::Smooth));
        track.AddMarker(Free(1000, {10, 0, 0}, camera::Blend::Smooth));
        track.AddMarker(Free(2000, {10, 10, 0}));
        Check(Near(camera::Plan(track, 100).smoothWeight, 0.2f) && Near(camera::Plan(track, 900).smoothWeight, 1.0f) &&
                Near(camera::Plan(track, 1900).smoothWeight, 0.2f) && Near(camera::Plan(track, 1900).smoothMix, 0.5f),
            "smooth weight rises in the first segment and falls in the last");
    }

    // 12. Procedural shake.
    void TestShake()
    {
        const camera::ShakeDelta once = camera::ShakeOffset(camera::ShakeType::Handheld, 0.75f, 1234.0);
        const camera::ShakeDelta again = camera::ShakeOffset(camera::ShakeType::Handheld, 0.75f, 1234.0);
        Check(std::memcmp(&once, &again, sizeof(once)) == 0 && (once.rotation.x != 0.0f || once.rotation.z != 0.0f),
            "shake is deterministic");
        Check(Near(camera::ShakeAmplitude(1), 0.111f, 0.001f) && Near(camera::ShakeAmplitude(5), 1.0f, 0.001f) &&
                Near(camera::ShakeAmplitude(10), 3.361f, 0.001f),
            "shake gain curve ((i + 1) / 6)^2");

        Track track;
        Keyframe a = Free(0, {});
        a.legacyShake.type = camera::ShakeType::Handheld;
        Keyframe b = Free(1000, {});
        b.legacyShake.type = camera::ShakeType::Handheld;
        track.AddMarker(a);
        const std::uint32_t second = track.AddMarker(b);
        track.AddMarker(Free(2000, {}));
        track.FindMutable(track.Keyframes()[0].id)->blend = camera::Blend::None;
        Check(std::fabs(camera::Plan(track, 1500).shake.timeMs - 1500.0) < 0.01, "same shake keeps its clock");
        const editor::CameraState first = Eval(track, 1500);
        const editor::CameraState repeat = Eval(track, 1500);
        Check(std::memcmp(&first, &repeat, sizeof(first)) == 0, "same time, same shaken camera");

        track.FindMutable(track.Keyframes()[0].id)->legacyShake.speedIndex = 8; // 200 %
        Check(std::fabs(camera::Plan(track, 500).shake.timeMs - 1000.0) < 0.01 &&
                std::fabs(camera::Plan(track, 1500).shake.timeMs - 2500.0) < 0.01,
            "shake speed 200 % doubles the shake clock (the frequency)");

        track.FindMutable(second)->legacyShake.type = camera::ShakeType::Drunk;
        Check(std::fabs(camera::Plan(track, 1500).shake.timeMs - 500.0) < 0.01, "another shake type starts its own clock");

        // The chain ending at a marker without shake fades out
        // over its last 250 ms.
        const float full = camera::Plan(track, 1500).shake.amplitude;
        Check(full > 0.0f && Near(camera::Plan(track, 1750).shake.amplitude, full) &&
                Near(camera::Plan(track, 1875).shake.amplitude, full * 0.5f, 0.001f) &&
                camera::Plan(track, 1999).shake.amplitude < full * 0.001f,
            "shake fades out before a marker without it");
        Check(Near(camera::Plan(track, 875).shake.amplitude, camera::Plan(track, 500).shake.amplitude * 0.5f, 0.001f),
            "and before a marker with another shake (that one fades in)");
    }

    // 13. Realistic shake (camera_shake.h): the profiles' contract.
    namespace shake = camera::shake;
    using camera::ShakeType;

    constexpr ShakeType kContinuousShakes[] = {ShakeType::Handheld, ShakeType::Drunk, ShakeType::GroundVibration,
        ShakeType::AirTurbulence, ShakeType::Walking, ShakeType::Running, ShakeType::Horseback, ShakeType::Steadicam,
        ShakeType::Wagon};

    std::vector<float> ShakeSeries(ShakeType type, int channel, double start, double seconds, double rate, float gain = 1.0f)
    {
        std::vector<float> v(static_cast<std::size_t>(seconds * rate));
        for (std::size_t n = 0; n < v.size(); ++n)
            v[n] = shake::Evaluate(type, gain, start + static_cast<double>(n) / rate).value[channel];
        return v;
    }

    double Rms(const std::vector<float>& v)
    {
        double sum = 0.0;
        for (float x : v)
            sum += static_cast<double>(x) * x;
        return std::sqrt(sum / static_cast<double>(v.size()));
    }

    // Power per 0.1 Hz bin from 0 to maxHz (plain DFT over 10 s segments).
    std::vector<double> Spectrum(const std::vector<float>& v, double rate, double maxHz)
    {
        const std::size_t segment = static_cast<std::size_t>(rate * 10.0);
        const std::size_t bins = static_cast<std::size_t>(maxHz * 10.0);
        std::vector<double> power(bins, 0.0);
        for (std::size_t s = 0; s + segment <= v.size(); s += segment)
            for (std::size_t k = 1; k < bins; ++k)
            {
                double re = 0.0, im = 0.0;
                const double w = 6.283185307179586 * static_cast<double>(k) / static_cast<double>(segment);
                for (std::size_t n = 0; n < segment; ++n)
                {
                    re += v[s + n] * std::cos(w * static_cast<double>(n));
                    im += v[s + n] * std::sin(w * static_cast<double>(n));
                }
                power[k] += re * re + im * im;
            }
        return power;
    }

    double BandShare(const std::vector<double>& power, double fromHz, double toHz)
    {
        double band = 0.0, total = 0.0;
        for (std::size_t k = 1; k < power.size(); ++k)
        {
            const double hz = static_cast<double>(k) / 10.0;
            total += power[k];
            if (hz >= fromHz && hz < toHz)
                band += power[k];
        }
        return total > 0.0 ? band / total : 0.0;
    }

    double DominantHz(const std::vector<double>& power)
    {
        std::size_t best = 1;
        for (std::size_t k = 1; k < power.size(); ++k)
            if (power[k] > power[best])
                best = k;
        return static_cast<double>(best) / 10.0;
    }

    void TestShakeRealism()
    {
        // None is silent, every type is deterministic.
        const shake::Sample none = shake::Evaluate(ShakeType::None, 3.0f, 12.3);
        bool silent = true;
        for (float x : none.value)
            silent = silent && x == 0.0f;
        Check(silent, "shake None is zero");
        bool same = true;
        for (std::uint8_t t = 1; t < editor::kShakeTypeCount; ++t)
        {
            const shake::Sample a = shake::Evaluate(static_cast<ShakeType>(t), 1.0f, 0.731);
            const shake::Sample b = shake::Evaluate(static_cast<ShakeType>(t), 1.0f, 0.731);
            same = same && std::memcmp(&a, &b, sizeof(a)) == 0;
        }
        Check(same, "every shake type is deterministic");

        // Continuity: 1 ms apart, small steps (no pops), and a smooth start.
        bool continuous = true;
        bool softStart = true;
        for (std::uint8_t t = 1; t < editor::kShakeTypeCount; ++t)
        {
            const auto type = static_cast<ShakeType>(t);
            shake::Sample previous = shake::Evaluate(type, 1.0f, 0.0);
            for (int ms = 1; ms < 20000; ++ms)
            {
                const shake::Sample now = shake::Evaluate(type, 1.0f, ms / 1000.0);
                for (int c = 0; c < shake::ChannelCount; ++c)
                    continuous = continuous && std::fabs(now.value[c] - previous.value[c]) < (c < 3 ? 0.1f : 0.002f);
                previous = now;
            }
            // Continuous types fade in; the one shots (Explosion, Gunshot) hit
            // at once (still from zero).
            const shake::Sample first = shake::Evaluate(type, 1.0f, 0.001);
            for (int c = 0; c < 3; ++c)
                softStart = softStart && std::fabs(first.value[c]) < (shake::OneShot(type) ? 0.1f : 0.001f);
        }
        Check(continuous, "shake moves < 0.1 deg / 2 mm per ms");
        Check(softStart, "shake starts from zero at the chain start");

        // Intensity: gain is linear in the output, perceptual in the intensity.
        const std::vector<float> mid = ShakeSeries(ShakeType::Handheld, shake::Yaw, 5.0, 20.0, 100.0, shake::Gain(5));
        const std::vector<float> top = ShakeSeries(ShakeType::Handheld, shake::Yaw, 5.0, 20.0, 100.0, shake::Gain(10));
        Check(std::fabs(Rms(top) / Rms(mid) - shake::Gain(10) / shake::Gain(5)) < 0.001 && shake::Gain(1) < shake::Gain(2) &&
                shake::Gain(9) < shake::Gain(10),
            "shake amplitude scales with the intensity gain");

        // Explosion: a hit, then settled after 3 s.
        float peak = 0.0f, tail = 0.0f;
        for (int ms = 0; ms < 6000; ms += 2)
        {
            const float v = std::fabs(shake::Evaluate(ShakeType::Explosion, 1.0f, ms / 1000.0).value[shake::Pitch]);
            if (ms < 1000)
                peak = std::max(peak, v);
            else if (ms >= 3000)
                tail = std::max(tail, v);
        }
        Check(peak > 1.0f && tail < 0.01f * peak, "explosion kicks over 1 deg and settles below 1 % after 3 s");

        // Handheld: rotation dominant, energy below 3 Hz, a faint 7 to 13 Hz tremor.
        const std::vector<float> pitch = ShakeSeries(ShakeType::Handheld, shake::Pitch, 10.0, 60.0, 100.0);
        const std::vector<double> pitchPower = Spectrum(pitch, 100.0, 20.0);
        const double tremor = BandShare(pitchPower, 7.0, 13.0);
        Check(BandShare(pitchPower, 0.0, 3.0) > 0.9 && tremor > 0.0005 && tremor < 0.05,
            "handheld pitch: most energy below 3 Hz, a small tremor band");
        const double sway = Rms(ShakeSeries(ShakeType::Handheld, shake::X, 10.0, 60.0, 100.0));
        const double rotationAtTen = Rms(pitch) * 3.14159265 / 180.0 * 10.0; // image shift at 10 m
        Check(sway < 0.25 * rotationAtTen, "handheld: rotation dominates translation at 10 m");

        // Walking: the step rhythm in the vertical, the half rate in the roll.
        const std::vector<double> bob = Spectrum(ShakeSeries(ShakeType::Walking, shake::Z, 10.0, 40.0, 100.0), 100.0, 10.0);
        const std::vector<double> rollSway = Spectrum(ShakeSeries(ShakeType::Walking, shake::Roll, 10.0, 40.0, 100.0), 100.0, 10.0);
        Check(std::fabs(DominantHz(bob) - shake::GaitHz(ShakeType::Walking)) < 0.15 &&
                std::fabs(DominantHz(rollSway) - 0.5 * shake::GaitHz(ShakeType::Walking)) < 0.15,
            "walking: step bob at the gait rate, roll sway at half");

        // Relative character: steadicam is calmer than handheld, drunk wider.
        const double handheldRoll = Rms(ShakeSeries(ShakeType::Handheld, shake::Roll, 10.0, 60.0, 50.0));
        Check(Rms(ShakeSeries(ShakeType::Steadicam, shake::Yaw, 10.0, 60.0, 50.0)) <
                    0.5 * Rms(ShakeSeries(ShakeType::Handheld, shake::Yaw, 10.0, 60.0, 50.0)) &&
                Rms(ShakeSeries(ShakeType::Drunk, shake::Roll, 10.0, 60.0, 50.0)) > 5.0 * handheldRoll,
            "steadicam calmer than handheld, drunk roll far wider");
        const std::vector<double> rumble = Spectrum(ShakeSeries(ShakeType::GroundVibration, shake::Z, 10.0, 20.0, 200.0), 200.0, 40.0);
        Check(BandShare(rumble, 10.0, 30.0) > 0.8, "ground vibration: vertical energy between 10 and 30 Hz");

        // Shake speed 200 % doubles the dominant frequency (through the chain clock).
        Track track;
        Keyframe a = Free(0, {});
        a.legacyShake.type = ShakeType::Walking;
        track.AddMarker(a);
        track.AddMarker(Free(60000, {}));
        const auto markerSeries = [&track]() {
            std::vector<float> v;
            for (std::uint64_t t = 10000; t < 50000; t += 10)
            {
                const camera::SegmentPlan plan = camera::Plan(track, t);
                v.push_back(camera::ShakeOffset(plan.shake.type, plan.shake.amplitude, plan.shake.timeMs).position.z);
            }
            return v;
        };
        const double normal = DominantHz(Spectrum(markerSeries(), 100.0, 10.0));
        track.FindMutable(track.Keyframes()[0].id)->legacyShake.speedIndex = 8; // 200 %
        const double fast = DominantHz(Spectrum(markerSeries(), 100.0, 10.0));
        Check(std::fabs(fast - 2.0 * normal) < 0.2 && normal > 1.5, "shake speed 200 % doubles the dominant frequency");

        // The editor accepts the new types, not past them (through the
        // marker's shake, SetShake).
        Keyframe k = Free(0, {});
        editor::MarkerShake s = editor::DefaultMarkerShake();
        s.on = 1;
        s.profile = static_cast<std::uint8_t>(ShakeType::Wagon);
        const bool wagon = camera::SetShake(k, s) && k.shake.profile == static_cast<std::uint8_t>(ShakeType::Wagon);
        s.profile = static_cast<std::uint8_t>(ShakeType::Custom);
        Check(wagon && !camera::SetShake(k, s), "the marker's shake takes the profiles only (the noise is 0)");

        // Cheap: well under 10 us per call.
        const auto begin = std::chrono::steady_clock::now();
        float sink = 0.0f;
        for (int n = 0; n < 200000; ++n)
            sink += shake::Evaluate(kContinuousShakes[n % 9], 1.0f, n * 0.00137).value[n % 6];
        const double micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 200000.0;
        Check(micros < 10.0 && sink == sink, "shake evaluation under 10 us per call");
        std::printf("  shake: %.3f us per call\n", micros);
    }

    // The old API keeps the model in sync, and a plain linear track gives
    // the same camera through both evaluators.
    void TestLegacySync()
    {
        Track track;
        editor::CameraState a{{0, 0, 0}, {0, 0, 0}, 40};
        editor::CameraState b{{10, 0, 0}, {0, 0, 90}, 60};
        track.AddOrReplace(0, a, editor::Easing::Linear, editor::Path::Linear);
        const std::uint32_t id = track.AddOrReplace(1000, b, editor::Easing::Linear, editor::Path::Linear);
        Check(track.LegacyBlendAnyMode() && track.Keyframes()[0].blend == camera::Blend::Linear &&
                !track.Keyframes()[0].ease && track.Keyframes()[1].cameraMode == camera::CameraMode::Free,
            "old API derives the model");
        editor::CameraState oldWay{};
        track.Evaluate(500, oldWay);
        const editor::CameraState newWay = Eval(track, 500);
        Check(Near(oldWay.position.x, newWay.position.x) && Near(oldWay.fov, newWay.fov) &&
                NearAngle(oldWay.rotation.z, newWay.rotation.z),
            "linear track: both evaluators agree");
        track.SetMode(id, editor::CameraMode::Follow, 7, {1, 2, 3});
        const Keyframe& follow = track.Keyframes()[1];
        Check(follow.cameraMode == camera::CameraMode::Free && follow.attachTarget == 7 && follow.lookAtTarget == 7 &&
                follow.attachType == camera::AttachType::Default && Near(follow.relPosition.y, 2.0f),
            "old SetMode Follow becomes Attach + Look At");
    }

    // 13. CAMR v2 to v3 and the v3 round trip.
    std::uint64_t Fnv64(const std::uint8_t* data, std::size_t size)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (std::size_t n = 0; n < size; ++n)
        {
            hash ^= data[n];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    template <typename T>
    void Put(std::vector<std::uint8_t>& out, T value)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    std::vector<Keyframe> LegacyKeys()
    {
        std::vector<Keyframe> keys(4);
        const editor::Easing easing[] = {editor::Easing::Linear, editor::Easing::SmoothStep, editor::Easing::Hold, editor::Easing::Linear};
        const editor::Path path[] = {editor::Path::Linear, editor::Path::Smooth, editor::Path::Linear, editor::Path::Linear};
        const editor::CameraMode mode[] = {editor::CameraMode::Free, editor::CameraMode::Target, editor::CameraMode::Follow, editor::CameraMode::Game};
        for (std::size_t n = 0; n < keys.size(); ++n)
        {
            keys[n].id = static_cast<std::uint32_t>(n + 1);
            keys[n].timeMs = n * 1000;
            keys[n].pose.position = {static_cast<float>(n), 0, 0};
            keys[n].easing = easing[n];
            keys[n].path = path[n];
            keys[n].mode = mode[n];
            keys[n].target = n == 1 || n == 2 ? 7u : 0u;
            keys[n].offset = n == 2 ? Vec3{1, -3, 1} : Vec3{};
        }
        return keys;
    }

    void CheckConverted(const std::vector<Keyframe>& k, const char* what)
    {
        const bool ok = k.size() == 4 &&
            k[0].cameraMode == camera::CameraMode::Free && k[0].blend == camera::Blend::Linear && k[0].ease && k[0].legacyCurve &&
            k[1].cameraMode == camera::CameraMode::Free && k[1].lookAtTarget == 7 && k[1].attachTarget == 0 &&
            k[1].blend == camera::Blend::None &&
            k[2].cameraMode == camera::CameraMode::Free && k[2].attachTarget == 7 && k[2].lookAtTarget == 7 &&
            k[2].attachType == camera::AttachType::Default && Near(k[2].relPosition.y, -3.0f) &&
            k[2].blend == camera::Blend::Linear && !k[2].ease && !k[2].legacyCurve &&
            k[3].cameraMode == camera::CameraMode::Game && k[3].blend == camera::Blend::None && !k[3].ease;
        Check(ok, what);
    }

    void TestCamrConversion()
    {
        std::vector<Keyframe> keys = LegacyKeys();
        camera::ConvertV2ToV3(keys);
        CheckConverted(keys, "v2 to v3 conversion (pure)");

        // A clip whose CAMR is a v2 payload loads converted.
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        Track empty;
        std::vector<std::uint8_t> bytes;
        std::string error;
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kSize = 0x6000000;
        Check(clipfile::Encode(recording, poses, empty, 4000, kBase, kSize, bytes, error), "encode a clip");
        std::size_t camr = bytes.size();
        for (std::size_t n = bytes.size() - 4; n > 0; --n)
        {
            if (std::memcmp(bytes.data() + n, "CAMR", 4) == 0)
            {
                camr = n;
                break;
            }
        }
        Check(camr < bytes.size(), "CAMR section found");
        if (camr >= bytes.size())
            return;
        std::vector<std::uint8_t> payload;
        Put<std::uint32_t>(payload, 5); // next id
        Put<std::uint64_t>(payload, 4);
        for (const Keyframe& k : LegacyKeys())
        {
            Put(payload, k.id);
            Put(payload, k.timeMs);
            Put(payload, k.pose.position.x);
            Put(payload, k.pose.position.y);
            Put(payload, k.pose.position.z);
            Put(payload, k.pose.orientation.w);
            Put(payload, k.pose.orientation.x);
            Put(payload, k.pose.orientation.y);
            Put(payload, k.pose.orientation.z);
            Put(payload, k.pose.fov);
            Put(payload, static_cast<std::uint32_t>(k.easing));
            Put(payload, static_cast<std::uint32_t>(k.path));
            Put(payload, static_cast<std::uint32_t>(k.mode));
            Put(payload, k.target);
            Put(payload, k.offset.x);
            Put(payload, k.offset.y);
            Put(payload, k.offset.z);
        }
        std::vector<std::uint8_t> v2(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(camr));
        v2.insert(v2.end(), {'C', 'A', 'M', 'R'});
        Put<std::uint16_t>(v2, 2);
        Put<std::uint64_t>(v2, payload.size());
        v2.insert(v2.end(), payload.begin(), payload.end());
        Put<std::uint64_t>(v2, Fnv64(payload.data(), payload.size()));

        Recording loadedRecording;
        std::vector<pose::PoseRecordExport> loadedPoses;
        Track loaded;
        std::uint64_t duration = 0;
        const bool decoded = clipfile::Decode(v2.data(), v2.size(), kBase, kSize, loadedRecording, loadedPoses, loaded, duration, error);
        Check(decoded, ("a v2 camera section loads: " + error).c_str());
        CheckConverted(loaded.Keyframes(), "v2 section converted on load");
        Check(loaded.LegacyBlendAnyMode() && loaded.Keyframes().size() == 4 &&
                loaded.Keyframes()[1].easing == editor::Easing::SmoothStep && loaded.Keyframes()[2].mode == editor::CameraMode::Follow,
            "v2 load keeps the legacy view and marks the track");

        std::vector<std::uint8_t> v4 = v2;
        v4[camr + 4] = 10; // 9 is current, 10 is newer
        Check(!clipfile::Decode(v4.data(), v4.size(), kBase, kSize, loadedRecording, loadedPoses, loaded, duration, error) &&
                error.find("version") != std::string::npos,
            "a newer camera version is refused");
        std::vector<std::uint8_t> v1 = v2;
        v1[camr + 4] = 1;
        Check(!clipfile::Decode(v1.data(), v1.size(), kBase, kSize, loadedRecording, loadedPoses, loaded, duration, error),
            "camera version 1 is refused");

        // v3 round trip with every field set.
        Track full;
        Keyframe a = Free(0, {1, 2, 3}, camera::Blend::Smooth, false);
        a.lookAtTarget = 0x00020003u;
        a.lookAtOffset = {0.25f, -0.5f};
        a.lookAtRoll = 12.0f;
        a.attachTarget = 0x00040001u;
        a.attachType = camera::AttachType::Rigid;
        a.relPosition = {0.5f, -2, 1};
        a.relRotation = {-5, 3, 20};
        a.legacyShake.type = camera::ShakeType::AirTurbulence;
        a.legacyShake.intensity = 7;
        a.legacyShake.speedIndex = 2;
        a.dof = camera::DofMode::Custom;
        a.focus = camera::FocusMode::Manual;
        a.dofIntensity = 66;
        a.focusDistance = 3.5f;
        a.focusTarget = 0x00020003u;
        a.speedIndex = 6;
        a.markerType = camera::MarkerType::EditAndAnchor;
        a.legacyCurve = true;
        a.editedMask = 0x12345;
        full.AddMarker(a);
        Keyframe preset = Marker(1000, camera::CameraMode::LeftSide, {}, {}, 30);
        preset.presetDistanceScale = 0.8f;
        full.AddMarker(preset);
        full.SetLegacyBlendAnyMode(true);
        std::vector<std::uint8_t> v3;
        // The old shake is folded first (a save writes the marker's shake,
        // CAMR v9).
        camera::mshake::Migrate(full);
        Check(full.Keyframes()[0].shake.on && full.Keyframes()[0].shake.profile == static_cast<std::uint8_t>(camera::ShakeType::AirTurbulence),
            "the old shake folded into the marker's");
        Check(clipfile::Encode(recording, poses, full, 4000, kBase, kSize, v3, error), "encode v3");
        Track back;
        Check(clipfile::Decode(v3.data(), v3.size(), kBase, kSize, loadedRecording, loadedPoses, back, duration, error),
            "decode v3");
        bool same = back.Keyframes().size() == full.Keyframes().size() && back.LegacyBlendAnyMode();
        for (std::size_t n = 0; same && n < back.Keyframes().size(); ++n)
        {
            const Keyframe& x = full.Keyframes()[n];
            const Keyframe& y = back.Keyframes()[n];
            same = x.id == y.id && x.timeMs == y.timeMs && std::memcmp(&x.pose, &y.pose, sizeof(x.pose)) == 0 &&
                x.cameraMode == y.cameraMode && x.markerType == y.markerType && x.blend == y.blend && x.ease == y.ease &&
                x.lookAtTarget == y.lookAtTarget && x.lookAtOffset.x == y.lookAtOffset.x &&
                x.lookAtOffset.y == y.lookAtOffset.y && x.lookAtRoll == y.lookAtRoll && x.attachTarget == y.attachTarget &&
                x.attachType == y.attachType && std::memcmp(&x.relPosition, &y.relPosition, sizeof(Vec3)) == 0 &&
                std::memcmp(&x.relRotation, &y.relRotation, sizeof(Vec3)) == 0 &&
                x.presetDistanceScale == y.presetDistanceScale && camera::mshake::Same(x.shake, y.shake) && x.origin == y.origin &&
                x.dof == y.dof &&
                x.focus == y.focus && x.dofIntensity == y.dofIntensity && x.focusDistance == y.focusDistance &&
                x.focusTarget == y.focusTarget && x.speedIndex == y.speedIndex && x.legacyCurve == y.legacyCurve &&
                x.editedMask == y.editedMask;
        }
        Check(same, "v3 round trip keeps every field");
        Check(back.Keyframes()[0].mode == editor::CameraMode::Follow && back.Keyframes()[1].easing == editor::Easing::SmoothStep,
            "v3 load derives the legacy view");

        // The converted v2 clip saved as v3 and read back evaluates the same.
        std::vector<std::uint8_t> resaved;
        clipfile::Encode(recording, poses, loaded, 4000, kBase, kSize, resaved, error);
        Track reloaded;
        clipfile::Decode(v2.data(), v2.size(), kBase, kSize, loadedRecording, loadedPoses, loaded, duration, error);
        clipfile::Encode(recording, poses, loaded, 4000, kBase, kSize, resaved, error);
        Check(clipfile::Decode(resaved.data(), resaved.size(), kBase, kSize, loadedRecording, loadedPoses, reloaded, duration, error),
            "converted clip saves as v3");
        bool sameCamera = true;
        for (std::uint64_t t = 0; t <= 3500; t += 250)
        {
            const editor::CameraState x = Eval(loaded, t);
            const editor::CameraState y = Eval(reloaded, t);
            sameCamera = sameCamera && std::memcmp(&x, &y, sizeof(x)) == 0;
        }
        Check(sameCamera, "converted clip evaluates the same after the v3 round trip");
    }
    // Marker edge cases.
    void TestNight0929()
    {
        // The clip's end (Game Camera, no pose of its own) turned Free
        // by a blend before it takes that marker's view, not the origin.
        Track track;
        track.AddMarker(Marker(0, camera::CameraMode::Game, {}));
        const std::uint32_t middle = track.AddMarker(Free(1000, {100, 200, 30}, camera::Blend::Linear));
        const std::uint32_t end = track.AddMarker(Marker(3000, camera::CameraMode::Game, {}));
        camera::ApplyLastMarkerRule(track);
        Check(track.Find(end)->cameraMode == camera::CameraMode::Free && Near(track.Find(end)->pose.position.x, 100.0f) &&
                Near(Eval(track, 2000).position.y, 200.0f),
            "a converted last marker holds the view before it");
        // Once the middle marker is gone the Game start rules the clip again.
        track.RemoveMarker(middle);
        std::vector<camera::GameSample> game;
        game.push_back({0, {5, 6, 7}, {0, 0, 0}, 50.0f});
        game.push_back({4000, {5, 6, 7}, {0, 0, 0}, 50.0f});
        camera::Scene scene;
        scene.game = &game;
        Check(Near(Eval(track, 2000, scene).position.x, 5.0f), "deleting the only marker gives the game camera back");

        // A moved marker stays between its neighbours.
        Track moved;
        moved.AddMarker(Free(0, {}));
        const std::uint32_t mid = moved.AddMarker(Free(1000, {}));
        moved.AddMarker(Free(2000, {}));
        Check(camera::ClampMoveTime(moved, mid, 5000) == 2000 - 33 && camera::ClampMoveTime(moved, mid, 0) == 33 &&
                camera::ClampMoveTime(moved, mid, 1500) == 1500,
            "a moved marker does not jump over its neighbours");
        Check(camera::ClampMoveTime(moved, moved.Keyframes()[0].id, 1500) == 1000 - 33, "the first marker stays first");

        // The scope keeps the last third person camera, turned with it.
        camera::ScopeAnchor anchor;
        const Vec3 player{10, 10, 0};
        camera::TrackThirdPerson(anchor, {{10, 7, 1}, {-5, 0, 0}, 50.0f}, player);
        const editor::CameraState first = camera::ThirdPersonDuringScope(anchor, {{10, 10.1f, 0.7f}, {2, 0, 0}, 20.0f}, player);
        Check(Near(first.position.x, 10.0f) && Near(first.position.y, 7.0f) && Near(first.position.z, 1.0f) &&
                Near(first.rotation.x, -5.0f) && Near(first.fov, 50.0f),
            "scope start: the third person camera goes on");
        const editor::CameraState turned = camera::ThirdPersonDuringScope(anchor, {{10, 10.1f, 0.7f}, {2, 0, 90}, 20.0f}, player);
        Check(Near(turned.position.x, 13.0f) && Near(turned.position.y, 10.0f) && NearAngle(turned.rotation.z, 90.0f),
            "scope turn: the camera orbits behind the look");
        camera::ScopeAnchor none;
        const editor::CameraState fallback = camera::ThirdPersonDuringScope(none, {{0, 0, 1}, {0, 0, 0}, 20.0f}, {0, 0, 0});
        Check(Near(fallback.position.y, -2.4f) && Near(fallback.fov, 50.0f), "scope without a third person camera: over the shoulder");
    }

    // The kill cam of a shootout clip: a cut of 36 m and 170 degrees in
    // 15 ms, the time scale falling to 0.061 on the same frame, a static
    // close-up, a cut back at 47561 with the scale at 0.66. Weapon wheel dips
    // (scale 0.1, no cut) are not kill cams.
    void TestKillCamCutAndSlowMotion()
    {
        using camera::GameSample;
        std::vector<GameSample> game;
        const Vec3 gameplay{-1900.0f, 1290.0f, 200.0f};
        const Vec3 close{-1929.32f, 1273.93f, 197.56f};
        for (std::uint32_t t = 45000; t <= 45679; t += 17)
            game.push_back({t, {gameplay.x + (t - 45000) * 0.001f, gameplay.y, gameplay.z}, {-5.0f, 0.0f, 153.4f}, 34.8f});
        game.push_back({45679, gameplay, {-5.0f, 0.0f, 153.4f}, 34.8f});
        for (std::uint32_t t = 45694; t <= 47544; t += 17)
            game.push_back({t, close, {-39.8f, 0.0f, -15.8f}, 27.0f});
        game.push_back({47561, gameplay, {-5.0f, 0.0f, 153.0f}, 35.0f});
        for (std::uint32_t t = 47578; t <= 49000; t += 17)
            game.push_back({t, gameplay, {-5.0f, 0.0f, 153.0f}, 35.0f});
        // Sorted with the duplicate 45679 removed (the loop may land on it).
        std::vector<GameSample> sorted;
        for (const GameSample& sample : game)
            if (sorted.empty() || sample.timeMs > sorted.back().timeMs)
                sorted.push_back(sample);

        Check(camera::IsGameCameraCut({45679, gameplay, {-5, 0, 153.4f}, 34.8f}, {45694, close, {-39.8f, 0, -15.8f}, 27.0f}),
            "36 m in 15 ms is a cut");
        Check(!camera::IsGameCameraCut({0, {0, 0, 0}, {0, 0, 0}, 50.0f}, {17, {0.5f, 0, 0}, {0, 0, 20.0f}, 50.0f}),
            "0.5 m and 20 degrees in a frame is camera motion");
        Check(!camera::IsGameCameraCut({0, {0, 0, 0}, {0, 0, 0}, 50.0f}, {200, {30.0f, 0, 0}, {0, 0, 0}, 50.0f}),
            "far apart samples (a gap) are not judged");
        Check(camera::IsGameCameraCut({0, {0, 0, 0}, {0, 0, 170.0f}, 50.0f}, {17, {0, 0, 0}, {0, 0, -170.0f}, 50.0f}) == false,
            "170 to -170 is a 20 degree turn the short way");
        Check(camera::IsGameCameraCut({0, {0, 0, 0}, {0, 0, 0}, 50.0f}, {17, {0, 0, 0}, {0, 0, 0}, 20.0f}),
            "a 30 degree FOV jump in a frame is a cut");

        editor::CameraState mid{};
        Check(camera::EvaluateGame(sorted, 45687, mid) && Near(mid.position.x, gameplay.x) && Near(mid.fov, 34.8f),
            "no frame halfway between the shots: the earlier shot holds up to the cut");
        Check(camera::EvaluateGame(sorted, 45694, mid) && Near(mid.position.x, close.x), "the new shot at its sample");
        Check(camera::EvaluateGame(sorted, 45694 + 8, mid) && Near(mid.position.x, close.x), "inside the shot: blended as usual");

        timescale::Track scale;
        scale.Note(0, 1.0f);
        scale.Note(20000, 0.10f); // weapon wheel, no cut
        scale.Note(21000, 1.0f);
        scale.Note(45694, 0.62f);
        scale.Note(45711, 0.32f);
        scale.Note(45728, 0.13f);
        scale.Note(45745, 0.061f);
        scale.Note(47561, 0.66f);
        scale.Note(47700, 1.0f);
        const std::vector<camera::TimeSpan> spans = camera::CinematicSlowSpans(sorted, scale);
        Check(spans.size() == 1 && spans[0].startMs == 45679 && spans[0].endMs == 47561,
            "one kill cam span from the cut in to the cut out");
        Check(camera::SpanAt(spans, 46000) && !camera::SpanAt(spans, 20500) && !camera::SpanAt(spans, 48000),
            "the weapon wheel dip is not a kill cam");

        // A second angle inside the slow motion keeps the span open.
        std::vector<GameSample> twoAngles = sorted;
        for (GameSample& sample : twoAngles)
            if (sample.timeMs >= 46500 && sample.timeMs < 47561)
                sample.position.x += 22.2f;
        const std::vector<camera::TimeSpan> kept = camera::CinematicSlowSpans(twoAngles, scale);
        Check(kept.size() == 1 && kept[0].startMs == 45679 && kept[0].endMs == 47561,
            "a cut inside the slow motion (a second angle) does not end it");
    }
}

int RunCameraMarkerTests(); // camera_marker_tests.cpp

int RunCameraTrackTests()
{
    TestBlendOnSource();
    TestFreeToGameIsCut();
    TestChainEase();
    TestInheritance();
    TestLookAtAttachRules();
    TestLastMarker();
    TestBlendSurvivesBeingLast();
    TestFade();
    TestAnchorSkipped();
    TestCanPlace();
    TestAttach();
    TestLookAtScreenOffset();
    TestSpring();
    TestShake();
    TestShakeRealism();
    TestLegacySync();
    TestCamrConversion();
    TestNight0929();
    TestKillCamCutAndSlowMotion();
    g_cameraFailures += RunCameraMarkerTests();
    if (g_cameraFailures == 0)
        std::puts("Camera track tests passed.");
    return g_cameraFailures;
}
