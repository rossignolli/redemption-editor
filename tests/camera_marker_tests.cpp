// The camera marker editor's pure logic, pinned against how the Rockstar
// Editor's camera markers behave. Each group says which behaviour it pins.
// The editor flows (add, type change, Edit Camera, delete) are simulated the
// way the core runs them.
#include "camera_track.h"
#include "camera_marker_shake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    int g_checks = 0;

    void Check(bool condition, const char* message)
    {
        ++g_checks;
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float actual, float expected, float tolerance = 0.02f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    bool NearAngle(float actual, float expected, float tolerance = 0.1f)
    {
        const float delta = std::fmod(actual - expected + 540.0f, 360.0f) - 180.0f;
        return std::fabs(delta) <= tolerance;
    }

    bool NearVec(const Vec3& a, const Vec3& b, float tolerance = 0.01f)
    {
        return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
    }

    bool SameCam(const CameraState& a, const CameraState& b, float position = 0.01f, float angle = 0.1f)
    {
        return NearVec(a.position, b.position, position) && NearAngle(a.rotation.x, b.rotation.x, angle) &&
            NearAngle(a.rotation.y, b.rotation.y, angle) && NearAngle(a.rotation.z, b.rotation.z, angle) &&
            Near(a.fov, b.fov, 0.05f);
    }

    bool Identical(const CameraState& a, const CameraState& b)
    {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    }

    CameraState CamOf(const camera::Pose& pose)
    {
        return {pose.position, Track::ToEulerDegrees(pose.orientation), pose.fov};
    }

    // ---- The world of the tests: a recorded game camera and two entities.

    constexpr std::uint32_t kPed = 7;
    constexpr std::uint32_t kMain = 1;

    CameraState GameAt(std::uint64_t t)
    {
        const float s = static_cast<float>(t) / 1000.0f;
        return {{s * 2.0f, -5.0f, 2.0f}, {-5.0f, 0.0f, s * 10.0f}, 50.0f};
    }

    const std::vector<camera::GameSample>& GameSamples()
    {
        static const std::vector<camera::GameSample> samples = [] {
            std::vector<camera::GameSample> v;
            for (std::uint32_t t = 0; t <= 12000; t += 100)
            {
                const CameraState c = GameAt(t);
                v.push_back({t, c.position, c.rotation, c.fov});
            }
            return v;
        }();
        return samples;
    }

    camera::EntityState PedAt(std::uint64_t t)
    {
        const float s = static_cast<float>(t) / 1000.0f;
        camera::EntityState e;
        e.position = {5.0f, 10.0f + s, 0.0f};
        e.heading = s * 20.0f;
        e.hasHead = true;
        e.head = {e.position.x, e.position.y, 1.6f};
        return e;
    }

    camera::EntityState MainAt(std::uint64_t t)
    {
        camera::EntityState e;
        e.position = {static_cast<float>(t) * 0.0005f, 0.0f, 0.0f};
        e.heading = 30.0f;
        return e;
    }

    camera::Scene MakeScene()
    {
        camera::Scene scene;
        scene.game = &GameSamples();
        scene.aspect = 16.0f / 9.0f;
        scene.mainCharacter = kMain;
        scene.lookup = [](std::uint32_t id, std::uint64_t t, camera::EntityState& out) {
            if (id == kPed)
                out = PedAt(t);
            else if (id == kMain)
                out = MainAt(t);
            else
                return false;
            return true;
        };
        return scene;
    }

    CameraState Eval(const Track& track, std::uint64_t t, const camera::Scene& scene)
    {
        CameraState state{};
        camera::EvaluateMarkers(track, t, scene, state);
        return state;
    }

    Keyframe FreeAt(std::uint64_t t, const CameraState& view, Blend blend = Blend::None, bool ease = true)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.pose = {view.position, Track::FromEulerDegrees(view.rotation), view.fov > 0.0f ? view.fov : 45.0f};
        k.blend = blend;
        k.ease = ease;
        return k;
    }

    // ---- The editor flows, as the core runs them.

    // A clip opened in the editor: its start and end markers, Game Camera,
    // with the recorded camera at their time as pose (ApplyClipTrim).
    Track MakeClip(std::uint64_t end = 10000)
    {
        Track track;
        for (const std::uint64_t t : {std::uint64_t{0}, end})
        {
            Keyframe k;
            k.timeMs = t;
            k.cameraMode = CameraMode::Game;
            const CameraState g = GameAt(t);
            k.pose = {g.position, Track::FromEulerDegrees(g.rotation), g.fov};
            track.AddMarker(k);
        }
        camera::ApplyLastMarkerRule(track);
        return track;
    }

    // Add at the playhead (AddMarkerAtPlayhead): inherit from the marker
    // before, store the frame shown at that time.
    std::uint32_t AddAt(Track& track, std::uint64_t t, const camera::Scene& scene)
    {
        if (!camera::CanPlace(track, t))
            return 0;
        Keyframe fresh;
        fresh.timeMs = t;
        const Keyframe* previous = nullptr;
        for (const Keyframe& k : track.Keyframes())
            if (k.timeMs <= t)
                previous = &k;
        if (previous)
            camera::InheritFrom(fresh, *previous);
        camera::SetMarkerView(fresh, Eval(track, t, scene), scene);
        const std::uint32_t id = track.AddMarker(fresh);
        camera::ApplyLastMarkerRule(track);
        return id;
    }

    // The Camera row (SetCameraKeyframeOptions): a marker that becomes Free
    // keeps the view it showed.
    void SetType(Track& track, std::uint32_t id, CameraMode mode, const camera::Scene& scene)
    {
        const Keyframe* key = track.Find(id);
        const bool toFree = mode == CameraMode::Free && key->cameraMode != CameraMode::Free;
        const camera::LivePose shown = camera::Resolve(*key, key->timeMs, scene);
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionMode;
        o.cameraMode = mode;
        track.SetOptions(id, o);
        if (toFree)
            camera::SetMarkerView(*track.FindMutable(id), CamOf(shown.pose), scene);
        camera::ApplyLastMarkerRule(track);
    }

    void SetBlend(Track& track, std::uint32_t id, Blend blend, bool ease)
    {
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionBlend | editor::CameraOptionEase;
        o.blend = blend;
        o.ease = ease ? 1 : 0;
        track.SetOptions(id, o);
    }

    // Edit Camera, Enter: the edited view is stored.
    void EditView(Track& track, std::uint32_t id, const CameraState& view, const camera::Scene& scene)
    {
        camera::SetMarkerView(*track.FindMutable(id), view, scene);
        camera::ApplyLastMarkerRule(track);
    }

    std::vector<std::uint32_t> Order(const Track& track)
    {
        std::vector<std::uint32_t> ids;
        for (const Keyframe& k : track.Keyframes())
            ids.push_back(k.id);
        return ids;
    }

    // =====================================================================
    // 1. Start and end markers: a clip is born with two markers, its start and
    // end; they cannot be deleted; moving them trims the clip; a Game Camera
    // clip plays the recorded camera everywhere.
    void TestClipBounds()
    {
        const camera::Scene scene = MakeScene();
        Track track = MakeClip();
        const std::uint32_t start = track.Keyframes().front().id;
        const std::uint32_t end = track.Keyframes().back().id;
        bool game = true;
        for (std::uint64_t t = 0; t <= 11000; t += 250)
            game = game && SameCam(Eval(track, t, scene), GameAt(std::min<std::uint64_t>(t, 12000)));
        Check(game, "bounds: a new clip plays the recorded game camera at every time (after the end too)");
        Check(camera::IsClipBound(track, start) && camera::IsClipBound(track, end), "bounds: start and end are the clip's bounds");
        Check(!track.RemoveMarker(start) && !track.RemoveMarker(end) && track.Keyframes().size() == 2,
            "bounds: start and end cannot be deleted");
        const std::uint32_t middle = AddAt(track, 5000, scene);
        Check(!camera::IsClipBound(track, middle) && track.RemoveMarker(middle) && track.Keyframes().size() == 2,
            "bounds: a middle marker is deleted");
        Check(camera::ClampMoveTime(track, start, 4000) == 4000 && track.MoveMarker(start, 4000) &&
                track.Keyframes().front().timeMs == 4000,
            "bounds: moving the start later trims the clip");
        Check(camera::ClampMoveTime(track, start, 20000) == 10000 - 33 && !track.MoveMarker(start, 10500),
            "bounds: the start never passes the end");
        Check(camera::ClampMoveTime(track, end, 0) == 4000 + 33 && track.MoveMarker(end, 8000) &&
                track.Keyframes().back().timeMs == 8000,
            "bounds: the end moves earlier down to the start plus the spacing");
        Track legacy = MakeClip();
        legacy.SetLegacyBlendAnyMode(true);
        Check(!camera::IsClipBound(legacy, legacy.Keyframes().front().id), "bounds: a converted v2 clip has no protected bounds");
    }

    // =====================================================================
    // 2. Add: at the playhead when it fits, inheriting from the PREVIOUS
    // marker: editable camera and not Anchor copies pose, targets and framing;
    // DOF, effects, speed, shake, marker and camera type always; blend and ease
    // only from a Free camera; edited mask cleared; a Free previous gives the
    // frame shown.
    void TestAddInherits()
    {
        const camera::Scene scene = MakeScene();

        // A Game clip: the new marker is a Game Camera, the picture does not change.
        Track game = MakeClip();
        const std::uint32_t added = AddAt(game, 4000, scene);
        const Keyframe* g = game.Find(added);
        Check(added != 0 && g->cameraMode == CameraMode::Game && g->blend == Blend::Spline && g->ease &&
                SameCam(CamOf(g->pose), GameAt(4000)),
            "add: after a Game marker the new one is Game, the default blend (Spline), pose = the frame shown");
        bool same = true;
        for (std::uint64_t t = 0; t <= 10000; t += 250)
            same = same && SameCam(Eval(game, t, scene), GameAt(t));
        Check(same, "add: adding a marker does not change the picture (Game clip)");
        Check(AddAt(game, 4000, scene) == 0 && AddAt(game, 4032, scene) == 0 && AddAt(game, 4033, scene) != 0,
            "add: refused on a marker and inside its spacing (33 ms at 100 %)");

        // In the middle of a Linear blend: the path stays the same.
        Track blend = MakeClip();
        const CameraState va{{0, 0, 0}, {0, 0, 0}, 40};
        const CameraState vb{{20, 0, 0}, {0, 0, 90}, 60};
        blend.AddMarker(FreeAt(1000, va, Blend::Linear, false));
        blend.AddMarker(FreeAt(3000, vb, Blend::None, false));
        camera::ApplyLastMarkerRule(blend);
        std::vector<CameraState> before;
        for (std::uint64_t t = 1000; t <= 3000; t += 100)
            before.push_back(Eval(blend, t, scene));
        const std::uint32_t mid = AddAt(blend, 2000, scene);
        const Keyframe* m = blend.Find(mid);
        Check(m->cameraMode == CameraMode::Free && m->blend == Blend::Linear && !m->ease && m->editedMask == 0 &&
                Near(m->pose.position.x, 10.0f) && Near(m->pose.fov, 50.0f),
            "add: after a Free marker it inherits Free, the blend and ease, and the frame shown (not the previous pose)");
        bool path = true;
        std::size_t n = 0;
        for (std::uint64_t t = 1000; t <= 3000; t += 100, ++n)
            path = path && SameCam(Eval(blend, t, scene), before[n], 0.01f, 0.2f);
        Check(path, "add: a marker added inside a linear blend keeps the camera path");

        // Always copied; the fade is not (first and last marker only).
        Keyframe previous = FreeAt(0, va, Blend::Smooth, false);
        previous.dof = camera::DofMode::Custom;
        previous.focus = camera::FocusMode::Target;
        previous.dofIntensity = 70;
        previous.focusDistance = 3;
        previous.focusTarget = kPed;
        previous.speedIndex = 2;
        previous.legacyShake.type = camera::ShakeType::Drunk;
        previous.legacyShake.intensity = 8;
        previous.legacyShake.speedIndex = 6;
        previous.filter = 4;
        previous.contrast = 20;
        previous.transition = 1;
        previous.lookAtTarget = kPed;
        previous.lookAtOffset = {0.2f, 0.1f};
        previous.lookAtRoll = 5;
        previous.attachTarget = kPed;
        previous.attachType = camera::AttachType::Flexible;
        previous.relPosition = {1, 2, 3};
        previous.editedMask = 0xFFFF;
        Keyframe fresh;
        const bool fromFrame = camera::InheritFrom(fresh, previous);
        Check(fromFrame && fresh.dof == camera::DofMode::Custom && fresh.focus == camera::FocusMode::Target &&
                Near(fresh.dofIntensity, 70) && Near(fresh.focusDistance, 3) && fresh.focusTarget == kPed &&
                fresh.speedIndex == 2 && fresh.legacyShake.type == camera::ShakeType::Drunk && fresh.legacyShake.intensity == 8 &&
                fresh.legacyShake.speedIndex == 6 && fresh.filter == 4 && fresh.contrast == 20 && fresh.transition == 0 &&
                fresh.lookAtTarget == kPed && Near(fresh.lookAtOffset.x, 0.2f) && Near(fresh.lookAtRoll, 5) &&
                fresh.attachTarget == kPed && fresh.attachType == camera::AttachType::Flexible &&
                Near(fresh.relPosition.z, 3) && fresh.blend == Blend::Smooth && !fresh.ease && fresh.editedMask == 0,
            "add: DOF, speed, shake, effects, targets, framing, blend copied; fade and edited mask not");

        Keyframe preset = FreeAt(0, va, Blend::Linear, false);
        preset.cameraMode = CameraMode::RightSide;
        preset.presetDistanceScale = 0.9f;
        Keyframe afterPreset;
        Check(!camera::InheritFrom(afterPreset, preset) && afterPreset.cameraMode == CameraMode::RightSide &&
                Near(afterPreset.presetDistanceScale, 0.9f) && afterPreset.blend == Blend::Spline && afterPreset.ease,
            "add: after a preset: the preset and its distance, the default blend (Spline), ease on");
        Keyframe gameMarker = FreeAt(0, va, Blend::Linear, false);
        gameMarker.cameraMode = CameraMode::Game;
        gameMarker.lookAtTarget = kPed;
        Keyframe afterGame;
        camera::InheritFrom(afterGame, gameMarker);
        Check(afterGame.lookAtTarget == 0 && afterGame.blend == Blend::Spline,
            "add: a Game marker passes no targets nor its blend (the default Spline)");
    }

    // =====================================================================
    // 3. Delete: never the start or end; deleting the penultimate re-applies
    // the last marker rule; deleting the only middle marker gives the Game
    // Camera back; the end never keeps the view of a deleted marker.
    void TestDelete()
    {
        const camera::Scene scene = MakeScene();
        Track track = MakeClip();
        const std::uint32_t end = track.Keyframes().back().id;
        const std::uint32_t m1 = AddAt(track, 4000, scene);
        SetType(track, m1, CameraMode::Free, scene);
        Check(SameCam(Eval(track, 4000, scene), GameAt(4000)) && SameCam(Eval(track, 7000, scene), GameAt(4000)),
            "type to Free: the marker keeps the view it showed and holds it (no jump, E3)");
        SetBlend(track, m1, Blend::Linear, true);
        Check(track.Find(end)->cameraMode == CameraMode::Free && camera::HasBorrowedView(*track.Find(end)) &&
                SameCam(Eval(track, 8000, scene), GameAt(4000)),
            "blend into the end: the end takes the view before it and the camera holds (E4)");
        Check(track.RemoveMarker(m1) && track.Find(end)->cameraMode == CameraMode::Game &&
                !camera::HasBorrowedView(*track.Find(end)),
            "delete the only middle marker: the end is a Game Camera again");
        bool game = true;
        for (std::uint64_t t = 0; t <= 10000; t += 250)
            game = game && SameCam(Eval(track, t, scene), GameAt(t));
        Check(game, "delete the only middle marker: the game camera plays everywhere");

        // The next blend into the end holds the NEW marker's view, not the
        // deleted one's.
        const std::uint32_t m2 = AddAt(track, 5000, scene);
        SetType(track, m2, CameraMode::Free, scene);
        const CameraState framed{{100, 100, 10}, {-10, 0, 45}, 35};
        EditView(track, m2, framed, scene);
        SetBlend(track, m2, Blend::Linear, false);
        Check(SameCam(Eval(track, 8000, scene), framed), "a new blend into the end holds the new marker's view");

        // Deleting the penultimate of a chain: the end follows the new one.
        Track chain = MakeClip();
        const CameraState va{{1, 2, 3}, {0, 0, 10}, 40};
        const CameraState vb{{9, 9, 9}, {0, 0, 80}, 70};
        const std::uint32_t a = chain.AddMarker(FreeAt(2000, va, Blend::Linear, false));
        const std::uint32_t b = chain.AddMarker(FreeAt(5000, vb, Blend::Linear, false));
        camera::ApplyLastMarkerRule(chain);
        Check(SameCam(Eval(chain, 9000, scene), vb), "chain: the end holds the last blending marker's view");
        chain.RemoveMarker(b);
        Check(SameCam(Eval(chain, 9000, scene), va) && SameCam(Eval(chain, 3000, scene), va),
            "chain: after deleting the penultimate the end holds the new penultimate's view");
        Check(chain.Find(a) && chain.Keyframes().size() == 3, "chain: only the deleted marker went");
    }

    // =====================================================================
    // 4. The last marker: with a Free camera with blend before it, it is Free
    // with the same targets; otherwise it is "inherited" and ignored by the
    // evaluator. Here: a view it borrowed follows the marker before it until
    // it is edited.
    void TestLastMarkerView()
    {
        const camera::Scene scene = MakeScene();
        Track track = MakeClip();
        const std::uint32_t end = track.Keyframes().back().id;
        const std::uint32_t a = track.AddMarker(FreeAt(2000, {{1, 1, 1}, {0, 0, 0}, 45}, Blend::Linear, false));
        camera::ApplyLastMarkerRule(track);
        const CameraState reframed{{5, 6, 7}, {-20, 0, 120}, 30};
        EditView(track, a, reframed, scene);
        Check(SameCam(Eval(track, 9000, scene), reframed) && camera::HasBorrowedView(*track.Find(end)),
            "last: an untouched end follows the reframed marker before it (the blend holds)");

        const CameraState endView{{50, 0, 5}, {0, 0, -90}, 70};
        EditView(track, end, endView, scene);
        const float halfway = Eval(track, 6000, scene).position.x;
        Check(!camera::HasBorrowedView(*track.Find(end)) && SameCam(Eval(track, 10000, scene), endView) && halfway > 5.5f &&
                halfway < 49.5f,
            "last: the end framed by the player is its own and the blend goes to it");
        EditView(track, a, {{2, 2, 2}, {0, 0, 0}, 45}, scene);
        const std::uint32_t m = AddAt(track, 6000, scene);
        Check(SameCam(CamOf(track.Find(end)->pose), endView) && track.Find(m)->blend == Blend::Linear,
            "last: the player's end view survives edits before it and a marker added before it");
        SetBlend(track, m, Blend::None, true);
        Check(track.Find(end)->cameraMode == CameraMode::Free && SameCam(Eval(track, 9000, scene), CamOf(track.Find(m)->pose)),
            "last: without a blend into it, the end is ignored (the marker before it holds)");

        // Its FOV set on the menu makes the view an edited one.
        Track fov = MakeClip();
        const std::uint32_t fovEnd = fov.Keyframes().back().id;
        const std::uint32_t fa = fov.AddMarker(FreeAt(2000, {{1, 1, 1}, {0, 0, 0}, 45}, Blend::Linear, false));
        camera::ApplyLastMarkerRule(fov);
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionFov;
        o.fov = 80;
        fov.SetOptions(fovEnd, o);
        EditView(fov, fa, {{3, 3, 3}, {0, 0, 0}, 45}, scene);
        Check(Near(fov.Find(fovEnd)->pose.fov, 80) && Near(fov.Find(fovEnd)->pose.position.x, 1),
            "last: an end with an edited FOV keeps its view");

        // The end cannot leave Free while the marker before it blends (its
        // Camera row is disabled).
        SetType(fov, fovEnd, CameraMode::Game, scene);
        Check(fov.Find(fovEnd)->cameraMode == CameraMode::Free, "last: the end stays Free while a blend goes into it");

        // Anchors after the end do not count as the last marker.
        Track anchored = MakeClip();
        Keyframe anchor;
        anchor.timeMs = 10500;
        anchor.markerType = camera::MarkerType::Anchor;
        anchored.AddMarker(anchor);
        const std::uint32_t realEnd = anchored.Keyframes()[1].id;
        anchored.AddMarker(FreeAt(3000, {{4, 4, 4}, {0, 0, 0}, 45}, Blend::Linear, false));
        camera::ApplyLastMarkerRule(anchored);
        Check(anchored.Find(realEnd)->cameraMode == CameraMode::Free, "last: an Anchor after it is skipped");
    }

    // =====================================================================
    // 5. Move: held between the neighbours with the spacing, never reorders; a
    // Free camera keeps its world pose, an attached one follows its target at
    // the new time; changing the speed repositions the markers that became too
    // close.
    void TestMove()
    {
        const camera::Scene scene = MakeScene();
        Track track = MakeClip();
        for (const std::uint64_t t : {2000, 4000, 6000})
            AddAt(track, t, scene);
        const std::vector<std::uint32_t> order = Order(track);
        bool ordered = true;
        bool spaced = true;
        for (const std::uint32_t id : order)
        {
            for (std::uint64_t wanted = 0; wanted <= 12000; wanted += 137)
            {
                const std::uint64_t to = camera::ClampMoveTime(track, id, wanted);
                track.MoveMarker(id, to);
                ordered = ordered && Order(track) == order;
                const auto& keys = track.Keyframes();
                for (std::size_t n = 1; n < keys.size(); ++n)
                    spaced = spaced && keys[n].timeMs - keys[n - 1].timeMs >= camera::MarkerSpacingMs(keys[n - 1].speedIndex);
            }
        }
        Check(ordered, "move: a drag over the whole clip never reorders the markers");
        Check(spaced, "move: the spacing always holds");
        const std::uint32_t second = track.Keyframes()[1].id;
        const std::uint64_t nextTime = track.Keyframes()[2].timeMs;
        Check(!track.MoveMarker(second, nextTime + 500), "move: MoveMarker refuses a time past the next marker");

        // A Free camera keeps its world pose; an attached one follows.
        Track attach = MakeClip();
        Keyframe k = FreeAt(2000, {{1, 2, 3}, {0, 0, 0}, 45});
        k.attachTarget = kPed;
        k.attachType = camera::AttachType::Default;
        k.relPosition = {0, -3, 1};
        const std::uint32_t attached = attach.AddMarker(k);
        const std::uint32_t loose = attach.AddMarker(FreeAt(5000, {{9, 9, 9}, {0, 0, 0}, 45}));
        camera::ApplyLastMarkerRule(attach);
        attach.MoveMarker(attached, 3000);
        attach.MoveMarker(loose, 6000);
        const camera::EntityState ped = PedAt(3000);
        const Vec3 expected = camera::FromHeadingFrame({0, -3, 1}, ped.heading);
        Check(NearVec(Eval(attach, 3000, scene).position,
                  {ped.position.x + expected.x, ped.position.y + expected.y, ped.position.z + expected.z}),
            "move: an attached marker follows its target at the new time");
        Check(NearVec(Eval(attach, 6000, scene).position, {9, 9, 9}), "move: a Free marker keeps its world pose");

        // Speed change pushes the next markers (33 ms x speed).
        Track speed = MakeClip();
        const std::uint32_t a = speed.AddMarker(FreeAt(2000, {}));
        const std::uint32_t b = speed.AddMarker(FreeAt(2040, {}));
        const std::uint32_t c = speed.AddMarker(FreeAt(2080, {}));
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionSpeed;
        o.speedIndex = 8; // 200 %
        speed.SetOptions(a, o);
        Check(speed.Find(b)->timeMs == 2066 && speed.Find(c)->timeMs == 2099 && speed.Keyframes().back().timeMs == 10000,
            "speed: 200 % pushes the markers after it to keep the spacing");
        Check(camera::ClampMoveTime(speed, b, 2080) == 2066 && speed.MoveMarker(b, 2066),
            "speed: the pushed marker can still be moved");
    }

    // =====================================================================
    // 6. Copy, paste, reset: paste all copies the whole marker (not its id or
    // time; the fade stays with the clip's bounds), paste effects only the
    // effects; reset goes back to the defaults, the last marker keeping its
    // camera type.
    void TestCopyPasteReset()
    {
        const camera::Scene scene = MakeScene();
        Keyframe from = FreeAt(1234, {{7, 8, 9}, {0, 0, 33}, 60}, Blend::Smooth, false);
        from.lookAtTarget = kPed;
        from.legacyShake.type = camera::ShakeType::Handheld;
        from.dof = camera::DofMode::None;
        from.speedIndex = 1;
        from.filter = 2;
        from.brightness = 30;
        from.transition = 0;
        Keyframe to;
        to.id = 5;
        to.timeMs = 50;
        to.transition = 1;
        to.transitionMs = 2500;
        Check(camera::Paste(to, from, camera::CopyContext::All) && to.id == 5 && to.timeMs == 50 &&
                to.cameraMode == CameraMode::Free && to.blend == Blend::Smooth && !to.ease && to.lookAtTarget == kPed &&
                to.legacyShake.type == camera::ShakeType::Handheld && to.dof == camera::DofMode::None && to.speedIndex == 1 &&
                to.filter == 2 && to.brightness == 30 && Near(to.pose.position.z, 9) && to.transition == 1 &&
                to.transitionMs == 2500,
            "paste all: the whole marker, not id, time or the fade of the clip's start");
        Keyframe effects;
        Check(camera::Paste(effects, from, camera::CopyContext::EffectsOnly) && effects.filter == 2 && effects.brightness == 30 &&
                effects.cameraMode == CameraMode::Game && effects.legacyShake.type == camera::ShakeType::None,
            "paste effects: only the effects");
        Check(!camera::Paste(effects, from, camera::CopyContext::AudioOnly), "paste audio: nothing (no per marker audio)");

        // Pasting a Game marker on the end while a blend goes into it: the
        // end stays Free (it cannot change type).
        Track track = MakeClip();
        const std::uint32_t end = track.Keyframes().back().id;
        const CameraState view{{3, 3, 3}, {0, 0, 0}, 45};
        track.AddMarker(FreeAt(2000, view, Blend::Linear, false));
        camera::ApplyLastMarkerRule(track);
        Keyframe game;
        game.cameraMode = CameraMode::Game;
        track.PasteMarker(end, game, camera::CopyContext::All);
        Check(track.Find(end)->cameraMode == CameraMode::Free && SameCam(Eval(track, 9000, scene), view),
            "paste on the end: still Free on the view before it");

        // Paste of a faster marker pushes the next one.
        Track spaced = MakeClip();
        const std::uint32_t a = spaced.AddMarker(FreeAt(2000, {}));
        const std::uint32_t b = spaced.AddMarker(FreeAt(2040, {}));
        Keyframe fast;
        fast.speedIndex = 8;
        spaced.PasteMarker(a, fast, camera::CopyContext::All);
        Check(spaced.Find(b)->timeMs == 2066, "paste: a faster speed pushes the next marker");

        // Reset.
        Track reset = MakeClip();
        Keyframe busy = FreeAt(3000, {{1, 1, 1}, {0, 0, 0}, 70}, Blend::Linear, false);
        busy.legacyShake.type = camera::ShakeType::Drunk;
        busy.dof = camera::DofMode::Custom;
        busy.speedIndex = 0;
        busy.lookAtTarget = kPed;
        const std::uint32_t r = reset.AddMarker(busy);
        reset.ResetMarker(r);
        const Keyframe* rk = reset.Find(r);
        Check(rk->timeMs == 3000 && rk->cameraMode == CameraMode::Game && rk->blend == Blend::Spline && rk->ease &&
                rk->legacyShake.type == camera::ShakeType::None && rk->dof == camera::DofMode::Default &&
                rk->speedIndex == editor::kMarkerSpeedDefaultIndex && rk->lookAtTarget == 0 && Near(rk->pose.fov, 45),
            "reset: the defaults, keeping the time");
        Track resetLast = MakeClip();
        Keyframe anchor;
        anchor.timeMs = 10500;
        anchor.markerType = camera::MarkerType::Anchor;
        resetLast.AddMarker(anchor);
        const std::uint32_t endId = resetLast.Keyframes()[1].id;
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionMode;
        o.cameraMode = CameraMode::Overhead;
        resetLast.SetOptions(endId, o);
        resetLast.ResetMarker(endId);
        Check(resetLast.Find(endId)->cameraMode == CameraMode::Overhead,
            "reset: the last marker (an Anchor after it does not count) keeps its camera type");
    }

    // =====================================================================
    // 7. Camera types: Game plays the recorded camera; Free holds its world
    // pose; Look At frames the target at its screen offset with its roll;
    // Attach Rigid, Default, Flexible; Attach and Look At on the same target
    // follow and look; presets orbit the main character with a distance scale
    // 0.5x to 1.5x and a roll (none on Overhead).
    void TestCameraModes()
    {
        const camera::Scene scene = MakeScene();
        Track free = MakeClip();
        const CameraState fixed{{1, 2, 3}, {-10, 0, 45}, 30};
        free.AddMarker(FreeAt(2000, fixed));
        bool held = true;
        for (std::uint64_t t = 2000; t <= 10000; t += 500)
            held = held && SameCam(Eval(free, t, scene), fixed);
        Check(held, "Free: the world pose held until the next marker (and after the ignored end)");

        Keyframe look = FreeAt(1000, {{0, 0, 2}, {0, 0, 0}, 40});
        look.lookAtTarget = kPed;
        look.lookAtOffset = {0.3f, -0.2f};
        look.lookAtRoll = 10;
        bool framed = true;
        for (std::uint64_t t = 1000; t <= 6000; t += 250)
        {
            const camera::LivePose live = camera::Resolve(look, t, scene);
            editor::Vec2 screen{};
            framed = framed && live.hasLookAt && camera::ProjectToScreen(CamOf(live.pose), PedAt(t).head, scene.aspect, screen) &&
                Near(screen.x, 0.3f, 0.01f) && Near(screen.y, -0.2f, 0.01f) &&
                NearAngle(Track::ToEulerDegrees(live.pose.orientation).y, 10.0f, 0.2f);
        }
        Check(framed, "Look At: the moving target stays at its screen offset, with the roll");
        Keyframe lost = look;
        lost.lookAtTarget = 99;
        const camera::LivePose missing = camera::Resolve(lost, 1000, scene);
        Check(missing.targetMissing && !missing.hasLookAt && SameCam(CamOf(missing.pose), CamOf(look.pose)),
            "Look At: a missing target keeps the stored view");

        Keyframe follow = FreeAt(0, {});
        follow.attachTarget = kPed;
        follow.lookAtTarget = kPed;
        follow.attachType = camera::AttachType::Default;
        follow.relPosition = {0, -4, 1};
        bool follows = true;
        for (std::uint64_t t = 0; t <= 6000; t += 500)
        {
            const camera::LivePose live = camera::Resolve(follow, t, scene);
            const Vec3 d{live.pose.position.x - PedAt(t).position.x, live.pose.position.y - PedAt(t).position.y,
                live.pose.position.z - PedAt(t).position.z};
            editor::Vec2 screen{};
            follows = follows && Near(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::sqrt(17.0f), 0.01f) &&
                camera::ProjectToScreen(CamOf(live.pose), PedAt(t).head, scene.aspect, screen) && Near(screen.x, 0, 0.01f) &&
                Near(screen.y, 0, 0.01f);
        }
        Check(follows, "Attach + Look At on the same target: follows at the same distance, target centred");

        const CameraMode presets[] = {CameraMode::FrontLow, CameraMode::Rear, CameraMode::RightSide, CameraMode::LeftSide,
            CameraMode::Overhead};
        bool orbit = true;
        for (const CameraMode mode : presets)
        {
            Vec3 offset{}, aim{};
            camera::PresetLayout(mode, offset, aim);
            const float length = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
            for (const float scale : {0.0f, 1.0f})
            {
                Keyframe p = FreeAt(0, {});
                p.cameraMode = mode;
                p.presetDistanceScale = scale;
                p.lookAtRoll = 15;
                const std::uint64_t t = 4000;
                const camera::LivePose live = camera::Resolve(p, t, scene);
                const camera::EntityState main = MainAt(t);
                const Vec3 d{live.pose.position.x - main.position.x, live.pose.position.y - main.position.y,
                    live.pose.position.z - main.position.z};
                const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                const float roll = Track::ToEulerDegrees(live.pose.orientation).y;
                orbit = orbit && !live.targetMissing && Near(distance, length * (scale == 0.0f ? 0.5f : 1.5f), 0.01f) &&
                    NearAngle(roll, mode == CameraMode::Overhead ? 0.0f : 15.0f, 0.5f);
            }
        }
        Check(orbit, "presets: around the main character, distance 0.5x to 1.5x, roll (none on Overhead)");
        camera::Scene noMain = scene;
        noMain.mainCharacter = 0;
        Keyframe rear = FreeAt(0, fixed);
        rear.cameraMode = CameraMode::Rear;
        const camera::LivePose noPreset = camera::Resolve(rear, 0, noMain);
        Check(noPreset.targetMissing && SameCam(CamOf(noPreset.pose), CamOf(rear.pose)),
            "presets: without the main character, the stored view");

        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionMode;
        o.cameraMode = CameraMode::Follow;
        Keyframe k;
        Check(!camera::ApplyOptions(k, o) && k.cameraMode == CameraMode::Game, "types: the legacy Target and Follow are refused");
        o.cameraMode = static_cast<CameraMode>(9);
        Check(!camera::ApplyOptions(k, o), "types: out of range refused");
    }

    // =====================================================================
    // 8. Blends: the blend lives on the source marker; only Free to Free
    // blends, anything else cuts; the ease comes from the chain (accelerate at
    // its start, brake at its end); Smooth is Linear plus springs while playing
    // forward; a blend never crosses a recording cut.
    void TestBlends()
    {
        const camera::Scene scene = MakeScene();
        const CameraState va{{0, 0, 0}, {0, 0, 0}, 40};
        const CameraState vb{{20, 0, 0}, {0, 0, 90}, 60};
        Track linear = MakeClip();
        linear.AddMarker(FreeAt(2000, va, Blend::Linear, false));
        linear.AddMarker(FreeAt(4000, vb, Blend::None, false));
        camera::ApplyLastMarkerRule(linear);
        Check(Near(Eval(linear, 2500, scene).position.x, 5) && Near(Eval(linear, 3000, scene).position.x, 10) &&
                Near(Eval(linear, 3500, scene).position.x, 15) && Near(Eval(linear, 3000, scene).fov, 50) &&
                NearAngle(Eval(linear, 3000, scene).rotation.z, 45, 0.2f),
            "Linear without ease: straight, even speed; FOV linear; rotation slerp");
        Check(SameCam(Eval(linear, 1999, scene), GameAt(1999)) && SameCam(Eval(linear, 2000, scene), va),
            "Game start to Free: a cut on the marker");
        Check(camera::Plan(linear, 3000).curve == camera::EaseCurve::Linear, "no ease anywhere: linear curve");

        Track eased = MakeClip();
        eased.AddMarker(FreeAt(2000, va, Blend::Linear, true));
        eased.AddMarker(FreeAt(4000, vb, Blend::None, true));
        const camera::SegmentPlan middle = camera::Plan(eased, 3000);
        Check(middle.curve == camera::EaseCurve::InOut && Near(middle.phase, 0.5f) && camera::Plan(eased, 2200).phase < 0.1f &&
                camera::Plan(eased, 3800).phase > 0.9f,
            "one segment with ease: slow in and out");

        // The destination's own blend does not change this segment (no ease).
        Track other = linear;
        other.FindMutable(other.Keyframes()[2].id)->blend = Blend::Smooth;
        Check(SameCam(Eval(other, 3000, scene), Eval(linear, 3000, scene)), "the segment belongs to its source marker");

        // Cuts: Free to preset, Game to Free even with a blend asked for.
        Track toPreset = MakeClip();
        toPreset.AddMarker(FreeAt(2000, va, Blend::Linear, false));
        Keyframe preset = FreeAt(4000, {});
        preset.cameraMode = CameraMode::Rear;
        toPreset.AddMarker(preset);
        Check(!camera::Plan(toPreset, 3000).blending && SameCam(Eval(toPreset, 3999, scene), va),
            "Free to a preset: a cut");
        Track fromGame = MakeClip();
        fromGame.FindMutable(fromGame.Keyframes()[0].id)->blend = Blend::Linear;
        fromGame.AddMarker(FreeAt(4000, vb));
        Check(!camera::Plan(fromGame, 3000).blending && SameCam(Eval(fromGame, 3000, scene), GameAt(3000)),
            "Game to Free: a cut");

        // A chain of blends with ease: In, then linear, then Out.
        Track chain = MakeClip();
        chain.AddMarker(FreeAt(1000, {{0, 0, 0}, {}, 45}, Blend::Linear, true));
        chain.AddMarker(FreeAt(2000, {{10, 0, 0}, {}, 45}, Blend::Linear, true));
        chain.AddMarker(FreeAt(3000, {{20, 0, 0}, {}, 45}, Blend::Linear, true));
        chain.AddMarker(FreeAt(4000, {{30, 0, 0}, {}, 45}, Blend::None, true));
        Check(camera::Plan(chain, 1500).curve == camera::EaseCurve::In && camera::Plan(chain, 2500).curve == camera::EaseCurve::Linear &&
                camera::Plan(chain, 3500).curve == camera::EaseCurve::Out,
            "chain: accelerates only at its start, brakes only at its end");
        bool continuous = true;
        for (const std::uint64_t t : {2000, 3000})
        {
            const float before = Eval(chain, t - 10, scene).position.x;
            const float at = Eval(chain, t, scene).position.x;
            const float after = Eval(chain, t + 10, scene).position.x;
            continuous = continuous && Near(at - before, after - at, 0.02f) && Near(at - before, 0.1f, 0.02f);
        }
        Check(continuous, "chain: position and speed continuous across the markers");

        // Smooth: Linear without the springs (scrub, pause); with them
        // (playing) the corner is rounded, and after the chain the springs
        // let go.
        Track smooth = chain;
        for (std::size_t n = 1; n <= 3; ++n)
            smooth.FindMutable(smooth.Keyframes()[n].id)->blend = Blend::Smooth;
        Keyframe turn = FreeAt(3000, {{20, 10, 0}, {}, 45}, Blend::Smooth, true);
        smooth.FindMutable(smooth.Keyframes()[3].id)->pose = turn.pose;
        Check(camera::Plan(smooth, 2500).smooth && SameCam(Eval(smooth, 1500, scene), Eval(chain, 1500, scene), 0.001f),
            "Smooth without springs: the linear blend");
        camera::SpringState spring;
        float corner = 0.0f;
        bool letGo = true;
        CameraState played{};
        for (std::uint64_t t = 1000; t <= 4500; t += 16)
        {
            CameraState raw{};
            camera::EvaluateMarkers(smooth, t, scene, played, &spring, 0.016f);
            camera::EvaluateMarkers(smooth, t, scene, raw);
            if (t >= 2900 && t <= 3100)
                corner = std::max(corner, std::fabs(played.position.x - raw.position.x) + std::fabs(played.position.y - raw.position.y));
            if (t > 4000)
                letGo = letGo && SameCam(played, raw, 0.0001f);
        }
        Check(letGo, "Smooth: after the chain the camera is the raw one");
        Check(corner > 0.05f, "Smooth: playing forward rounds the corner");

        // A blend never crosses a recording cut.
        camera::Scene cut = scene;
        const std::vector<std::uint32_t> cuts{3000};
        cut.cuts = &cuts;
        Check(SameCam(Eval(linear, 3500, cut), va), "a blend over a recording cut holds its source");
    }

    // =====================================================================
    // 9. Speed per marker: 5 to 200 %, spacing 33 ms x the speed (at least 2
    // ms), index clamped.
    void TestSpeed()
    {
        Check(Near(camera::SpeedFactor(0), 0.05f) && Near(camera::SpeedFactor(4), 1.0f) && Near(camera::SpeedFactor(8), 2.0f) &&
                Near(camera::SpeedFactor(200), 2.0f),
            "speed: 5 %, 100 %, 200 %, index held");
        Check(camera::MarkerSpacingMs(0) == 2 && camera::MarkerSpacingMs(4) == 33 && camera::MarkerSpacingMs(8) == 66,
            "speed: spacing 2, 33 and 66 ms");
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionSpeed;
        o.speedIndex = 50;
        Keyframe k;
        Check(camera::ApplyOptions(k, o) && k.speedIndex == 8, "speed: options held to 200 %");
    }

    // =====================================================================
    // 10. Shake: every type; intensity gain; one clock for a chain with the
    // same shake (no restart at a marker); intensity blended with the blend;
    // any camera type; a shake that ends fades out before the next marker (an
    // Anchor is not that marker).
    void TestShakeRules()
    {
        const camera::Scene scene = MakeScene();
        Track chain = MakeClip();
        Keyframe a = FreeAt(1000, {{0, 0, 0}, {}, 45});
        a.legacyShake.type = camera::ShakeType::Handheld;
        Keyframe b = FreeAt(3000, {{0, 0, 0}, {}, 45});
        b.legacyShake.type = camera::ShakeType::Handheld;
        chain.AddMarker(a);
        chain.AddMarker(b);
        const double before = camera::Plan(chain, 2999).shake.timeMs;
        const double after = camera::Plan(chain, 3001).shake.timeMs;
        const CameraState x = Eval(chain, 2999, scene);
        const CameraState y = Eval(chain, 3001, scene);
        Check(std::fabs(after - before - 2.0) < 0.01 && NearAngle(x.rotation.x, y.rotation.x, 0.1f) &&
                NearAngle(x.rotation.z, y.rotation.z, 0.1f),
            "shake: same type over a marker, one clock, no jump");

        Track blended = MakeClip();
        Keyframe weak = FreeAt(1000, {}, Blend::Linear, false);
        weak.legacyShake.type = camera::ShakeType::Handheld;
        weak.legacyShake.intensity = 2;
        Keyframe strong = FreeAt(3000, {}, Blend::None, false);
        strong.legacyShake.type = camera::ShakeType::Handheld;
        strong.legacyShake.intensity = 8;
        blended.AddMarker(weak);
        blended.AddMarker(strong);
        Check(Near(camera::Plan(blended, 2000).shake.amplitude,
                  0.5f * (camera::ShakeAmplitude(2) + camera::ShakeAmplitude(8)), 0.001f),
            "shake: the intensity blends with the camera");

        Track game = MakeClip();
        game.FindMutable(game.Keyframes()[0].id)->legacyShake.type = camera::ShakeType::Drunk;
        Check(!SameCam(Eval(game, 5000, scene), GameAt(5000), 0.001f, 0.001f), "shake: on a Game Camera too");

        // An Anchor after the last shaking marker: no fade before it, no pop after it.
        Track anchored = MakeClip(5000);
        Keyframe shaking = FreeAt(1000, {});
        shaking.legacyShake.type = camera::ShakeType::Handheld;
        anchored.AddMarker(shaking);
        Keyframe anchor;
        anchor.timeMs = 2000;
        anchor.markerType = camera::MarkerType::Anchor;
        anchored.AddMarker(anchor);
        const float full = camera::Plan(anchored, 1500).shake.amplitude;
        Check(Near(camera::Plan(anchored, 1990).shake.amplitude, full, 0.0001f) &&
                Near(camera::Plan(anchored, 2010).shake.amplitude, full, 0.0001f),
            "shake: an Anchor does not end the shake (no fade out and pop back)");
        Check(camera::Plan(anchored, 4990).shake.amplitude < 0.01f * full, "shake: it fades out before the clip's end");

        // The marker's shake comes whole (SetShake): every profile, the
        // numbers held to their ranges.
        bool types = true;
        for (std::uint8_t t = 0; t < 11; ++t)
        {
            Keyframe k;
            editor::MarkerShake s = editor::DefaultMarkerShake();
            s.on = 1;
            s.profile = t;
            s.gain = 9.0f;
            types = types && camera::SetShake(k, s) && k.shake.profile == t && k.shake.gain == 4.0f &&
                (k.editedMask & editor::CameraOptionShake) != 0;
        }
        Keyframe bad;
        editor::MarkerShake s = editor::DefaultMarkerShake();
        s.profile = 11;
        Check(types && !camera::SetShake(bad, s), "shake: every profile accepted, gain held, profile 11 refused");
    }

    // =====================================================================
    // 11. Fades: fade in on the first marker, out on the last; a fade on a
    // middle marker does nothing; a new marker does not inherit it.
    void TestFades()
    {
        Track track = MakeClip();
        track.FindMutable(track.Keyframes()[0].id)->transition = 1;
        track.FindMutable(track.Keyframes()[1].id)->transition = 1;
        Keyframe middle = FreeAt(5000, {});
        middle.transition = 1;
        track.AddMarker(middle);
        Check(Near(camera::FadeAmount(track, 0), 1) && Near(camera::FadeAmount(track, 500), 0.5f) &&
                Near(camera::FadeAmount(track, 5000), 0) && Near(camera::FadeAmount(track, 9500), 0.5f) &&
                Near(camera::FadeAmount(track, 10000), 1),
            "fades: in on the start, out on the end, none in the middle");
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionTransitionMs;
        o.transitionMs = 50;
        Keyframe k;
        camera::ApplyOptions(k, o);
        Check(k.transitionMs == 100, "fades: at least 100 ms");
    }

    // =====================================================================
    // 12. Depth of field: Default, Custom, None; intensity 0..100; focus Auto,
    // Manual (0.5..1000 m), Target; the last segment uses the marker before the
    // last.
    void TestDof()
    {
        Track track = MakeClip();
        Keyframe a = FreeAt(2000, {}, Blend::Linear, false);
        a.dof = camera::DofMode::Custom;
        const std::uint32_t id = track.AddMarker(a);
        camera::ApplyLastMarkerRule(track);
        const auto& keys = track.Keyframes();
        const int aIndex = 1;
        Check(keys[aIndex].id == id && camera::Plan(track, 3000).dofIndex == aIndex && camera::Plan(track, 10000).current == 2 &&
                camera::Plan(track, 10000).dofIndex == aIndex && camera::Plan(track, 1000).dofIndex == 0,
            "DOF: each segment its source; on the end, the marker before it");
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionDofIntensity | editor::CameraOptionFocusDistance | editor::CameraOptionDof |
            editor::CameraOptionFocus;
        o.dofIntensity = 150;
        o.focusDistance = 0.1f;
        o.dof = camera::DofMode::None;
        o.focus = camera::FocusMode::Manual;
        Keyframe k;
        Check(camera::ApplyOptions(k, o) && Near(k.dofIntensity, 100) && Near(k.focusDistance, 0.5f) &&
                k.dof == camera::DofMode::None && k.focus == camera::FocusMode::Manual,
            "DOF: values held in range");
        o.focusDistance = 5000;
        camera::ApplyOptions(k, o);
        o.mask = editor::CameraOptionFocus;
        o.focus = static_cast<camera::FocusMode>(3);
        Check(Near(k.focusDistance, 1000) && !camera::ApplyOptions(k, o), "DOF: focus up to 1000 m, modes in range");
    }

    // =====================================================================
    // 13. Limits: FOV 10 to 100; roll wraps; Look At offset -1..1; preset
    // distance 0..1; a non finite value is refused and changes nothing.
    void TestLimits()
    {
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionFov | editor::CameraOptionLookAtRoll | editor::CameraOptionLookAtOffset |
            editor::CameraOptionPresetDistance;
        o.fov = 2;
        o.lookAtRoll = 190;
        o.lookAtOffset = {3, -3};
        o.presetDistanceScale = 4;
        Keyframe k;
        Check(camera::ApplyOptions(k, o) && Near(k.pose.fov, 10) && Near(k.lookAtRoll, -170) && Near(k.lookAtOffset.x, 1) &&
                Near(k.lookAtOffset.y, -1) && Near(k.presetDistanceScale, 1),
            "limits: FOV, roll, offset and distance held");
        o.fov = 500;
        camera::ApplyOptions(k, o);
        Check(Near(k.pose.fov, 100), "limits: FOV at most 100");
        o.fov = std::nanf("");
        Check(!camera::ApplyOptions(k, o) && Near(k.pose.fov, 100), "limits: NaN refused");
        Keyframe view;
        camera::SetMarkerView(view, {{0, 0, 0}, {0, 0, 0}, 150}, MakeScene());
        Check(Near(view.pose.fov, 100), "limits: a stored view's FOV held too");
    }

    // =====================================================================
    // 14. Evaluation at any time: before the first marker the first one,
    // between them the segment, after the last one it holds.
    void TestAnyTime()
    {
        const camera::Scene scene = MakeScene();
        Track track;
        const CameraState va{{1, 0, 0}, {}, 45};
        const CameraState vb{{2, 0, 0}, {}, 45};
        track.AddMarker(FreeAt(1000, va, Blend::Linear, false));
        track.AddMarker(FreeAt(2000, vb, Blend::None, false));
        CameraState out{};
        Check(camera::EvaluateMarkers(track, 0, scene, out) && SameCam(out, va), "any time: before the first marker, the first");
        Check(Near(Eval(track, 1500, scene).position.x, 1.5f), "any time: between, the blend");
        Check(SameCam(Eval(track, 5000, scene), vb) && camera::Plan(track, 5000).next == -1, "any time: after the last, it holds");
        Track empty;
        Check(!camera::EvaluateMarkers(empty, 0, scene, out), "any time: no marker, no camera");
        Track anchors;
        Keyframe anchor;
        anchor.markerType = camera::MarkerType::Anchor;
        anchors.AddMarker(anchor);
        Check(!camera::EvaluateMarkers(anchors, 0, scene, out), "any time: Anchors only, no camera");
    }

    // =====================================================================
    // 15. Scrub consistency (Smooth, shake time warp): the camera at t never
    // depends on the times evaluated before; the springs only act playing
    // forward and let go on a pause or a jump.
    void TestScrub()
    {
        const camera::Scene scene = MakeScene();
        Track track = MakeClip(12000);
        Keyframe a = FreeAt(1000, {{0, 0, 2}, {}, 40}, Blend::Smooth, true);
        a.lookAtTarget = kPed;
        a.legacyShake.type = camera::ShakeType::Walking;
        Keyframe b = FreeAt(3000, {{5, 5, 2}, {}, 60}, Blend::Linear, true);
        b.lookAtTarget = kPed;
        b.lookAtOffset = {0.4f, 0};
        b.legacyShake.type = camera::ShakeType::Walking;
        Keyframe c = FreeAt(5000, {{10, 0, 4}, {0, 0, 90}, 45}, Blend::None, true);
        c.attachTarget = kPed;
        c.relPosition = {0, -3, 1};
        c.legacyShake.type = camera::ShakeType::Explosion;
        Keyframe d = FreeAt(7000, {});
        d.cameraMode = CameraMode::LeftSide;
        track.AddMarker(a);
        track.AddMarker(b);
        track.AddMarker(c);
        track.AddMarker(d);
        std::vector<std::uint64_t> times;
        for (std::uint64_t t = 0; t <= 12000; t += 97)
            times.push_back(t);
        std::vector<CameraState> direct;
        for (const std::uint64_t t : times)
            direct.push_back(Eval(track, t, scene));
        bool same = true;
        for (std::size_t n = 0; n < times.size(); ++n)
        {
            const std::size_t i = (n * 37) % times.size(); // jumps back and forth
            same = same && Identical(Eval(track, times[i], scene), direct[i]);
        }
        for (std::size_t n = times.size(); n-- > 0;)
            same = same && Identical(Eval(track, times[n], scene), direct[n]);
        Check(same, "scrub: forward, backward and jumping give the same camera");
        camera::SpringState spring;
        CameraState played{};
        for (std::uint64_t t = 1000; t < 3000; t += 16)
            camera::EvaluateMarkers(track, t, scene, played, &spring, 0.016f);
        camera::EvaluateMarkers(track, 1500, scene, played, &spring, 0.0f);
        Check(Identical(played, Eval(track, 1500, scene)), "scrub: a jump (dt 0) resets the springs: the raw camera");
        camera::EvaluateMarkers(track, 1516, scene, played, &spring, 0.016f);
        Check(SameCam(played, Eval(track, 1516, scene), 0.0001f, 0.001f), "scrub: playing again starts from the raw camera");
    }

    // =====================================================================
    // 16. Old clips (a converted v2 track, legacyBlendAnyMode): blends into
    // any type, the last keyframe always counts, no protected bounds, the
    // last marker rule does not rewrite it. And the old API on a marker
    // track stores only what it is asked.
    void TestLegacy()
    {
        const camera::Scene scene = MakeScene();
        Track legacy;
        legacy.AddMarker(FreeAt(0, {{0, 0, 0}, {}, 50}, Blend::Linear, false));
        Keyframe end;
        end.timeMs = 1000;
        end.cameraMode = CameraMode::Game;
        end.ease = false;
        legacy.AddMarker(end);
        legacy.SetLegacyBlendAnyMode(true);
        camera::ApplyLastMarkerRule(legacy);
        Check(legacy.Keyframes()[1].cameraMode == CameraMode::Game, "legacy: the last marker rule leaves it alone");
        Check(Near(Eval(legacy, 500, scene).position.x, GameAt(500).position.x * 0.5f, 0.01f),
            "legacy: a blend into the Game Camera");
        Check(camera::Plan(legacy, 2000).current == 1, "legacy: the last keyframe always counts");
        Check(legacy.RemoveMarker(legacy.Keyframes()[0].id) && legacy.Keyframes().size() == 1, "legacy: the first can be deleted");

        Track track = MakeClip();
        const std::uint32_t a = track.AddMarker(FreeAt(2000, {{1, 1, 1}, {}, 45}, Blend::Smooth, false));
        track.AddMarker(FreeAt(4000, {{2, 2, 2}, {}, 45}));
        const CameraState view{{7, 7, 7}, {0, 0, 30}, 55};
        Check(track.Update(a, view, editor::Easing::SmoothStep, editor::Path::Smooth) && !track.LegacyBlendAnyMode() &&
                track.Find(a)->blend == Blend::Smooth && !track.Find(a)->ease && !track.Find(a)->legacyCurve &&
                SameCam(CamOf(track.Find(a)->pose), view),
            "old Update on a marker track: the pose only, the blend and the track stay");
        Check(track.SetMode(a, CameraMode::Target, kPed, {}) && !track.LegacyBlendAnyMode() && track.Find(a)->lookAtTarget == kPed &&
                track.Find(a)->cameraMode == CameraMode::Free && track.Find(a)->blend == Blend::Smooth,
            "old SetMode on a marker track: Target becomes Look At, the blend stays");
        const std::uint32_t added = track.AddOrReplace(6000, view);
        Check(added != 0 && !track.LegacyBlendAnyMode() && track.Find(added)->cameraMode == CameraMode::Free,
            "old AddOrReplace on a marker track: a Free marker, the track stays");
        Check(!track.Remove(track.Keyframes().front().id) && !track.Move(a, 5000) && track.Move(a, 2500),
            "old Remove and Move on a marker track follow the marker rules");
    }

    // =====================================================================
    // 17. Edit Camera: it starts from the marker's live view at its time and
    // leaving stores the view so the marker shows exactly it: the pose, the
    // attach pose relative to the target, the Look At framing and roll.
    void TestEditCamera()
    {
        const camera::Scene scene = MakeScene();
        // The target (the ped's head at 2.5 s) is on screen for this view.
        const CameraState view{{3, 4, 2.5f}, {-8, 6, -10}, 38};
        const std::uint64_t t = 2500;
        struct Case
        {
            const char* name;
            std::uint32_t attach;
            camera::AttachType type;
            std::uint32_t look;
        };
        const Case cases[] = {{"free", 0, camera::AttachType::Default, 0},
            {"look at", 0, camera::AttachType::Default, kPed},
            {"attach rigid", kPed, camera::AttachType::Rigid, 0},
            {"attach default", kPed, camera::AttachType::Default, 0},
            {"attach flexible", kPed, camera::AttachType::Flexible, 0},
            {"attach and look at", kPed, camera::AttachType::Default, kPed}};
        bool all = true;
        for (const Case& c : cases)
        {
            Keyframe k;
            k.timeMs = t;
            k.cameraMode = CameraMode::Free;
            k.attachTarget = c.attach;
            k.attachType = c.type;
            k.lookAtTarget = c.look;
            camera::SetMarkerView(k, view, scene);
            const bool ok = SameCam(CamOf(camera::Resolve(k, t, scene).pose), view, 0.005f, 0.1f);
            if (!ok)
                std::fprintf(stderr, "  edit round trip failed: %s\n", c.name);
            all = all && ok;
        }
        Check(all, "Edit Camera: the stored view is shown back (free, look at, attach, both)");

        Track track = MakeClip();
        const std::uint32_t game = AddAt(track, t, scene);
        Check(SameCam(CamOf(camera::Resolve(*track.Find(game), t, scene).pose), GameAt(t)),
            "Edit Camera start: a Game marker starts from the recorded camera at its time");
        Keyframe look = FreeAt(t, {{0, 0, 2}, {}, 45});
        look.lookAtTarget = kPed;
        look.lookAtOffset = {-0.5f, 0.25f};
        editor::Vec2 screen{};
        Check(camera::ProjectToScreen(CamOf(camera::Resolve(look, t, scene).pose), PedAt(t).head, scene.aspect, screen) &&
                Near(screen.x, -0.5f, 0.01f) && Near(screen.y, 0.25f, 0.01f),
            "Edit Camera start: a Look At marker starts framed as stored");
    }

    // SameMarkers, the core's "did this command change the markers" (an undo
    // step and "unsaved changes" only then): a refused or empty command
    // leaves no step behind.
    void TestSameMarkers()
    {
        const camera::Scene scene = MakeScene();
        Track track = MakeClip();
        const std::uint32_t mid = AddAt(track, 4000, scene);
        Check(mid != 0, "same markers: setup adds a marker");
        const Track copy = track;
        Check(camera::SameMarkers(copy, track), "same markers: a copy is the same");

        Track moved = track;
        moved.MoveMarker(mid, 4000);
        Check(camera::SameMarkers(track, moved), "same markers: a move to the time it had changes nothing");
        moved.MoveMarker(mid, 4500);
        Check(!camera::SameMarkers(track, moved), "same markers: a real move is a change");

        Track fast = track;
        editor::CameraKeyframeOptions o{};
        o.mask = editor::CameraOptionSpeed;
        o.speedIndex = static_cast<std::uint8_t>(editor::kMarkerSpeedCount - 1);
        fast.SetOptions(mid, o);
        Check(!camera::SameMarkers(track, fast), "same markers: a speed change is a change");
        const Track atLimit = fast;
        fast.SetOptions(mid, o); // Right again at 200 %: the same value
        Check(camera::SameMarkers(atLimit, fast), "same markers: a speed already at its limit changes nothing");

        Track refused = track;
        Check(AddAt(refused, 4010, scene) == 0, "same markers: an add too close is refused");
        Check(camera::SameMarkers(track, refused), "same markers: a refused add changes nothing");

        Track bound = track;
        bound.RemoveMarker(bound.Keyframes().front().id);
        Check(camera::SameMarkers(track, bound), "same markers: deleting the clip's start is refused, nothing changes");
        bound.RemoveMarker(mid);
        Check(!camera::SameMarkers(track, bound), "same markers: deleting a middle marker is a change");

        Track fov = track;
        fov.FindMutable(mid)->pose.fov += 1.0f;
        Check(!camera::SameMarkers(track, fov), "same markers: the stored view (Edit Camera) is a change");
        Track legacy = track;
        legacy.SetLegacyBlendAnyMode(true);
        Check(!camera::SameMarkers(track, legacy), "same markers: the legacy flag is part of the markers");
    }

    // =====================================================================
    // 19. A camera attached to a moving character, Smooth blends, a 5 %
    // marker: the camera follows the character exactly, at 5 % as at 100 %.
    // Springs run on the world pose would trail a moving target by metres.
    constexpr std::uint32_t kRunner = 9;

    camera::Scene RunnerScene(float metresPerSecond)
    {
        camera::Scene scene = MakeScene();
        scene.lookup = [metresPerSecond](std::uint32_t id, std::uint64_t t, camera::EntityState& out) {
            if (id != kRunner)
                return false;
            const float s = static_cast<float>(t) / 1000.0f;
            out = {};
            out.position = {metresPerSecond * s, 0.0f, 0.0f};
            out.heading = -90.0f; // faces +X
            out.hasHead = true;
            out.head = {out.position.x, out.position.y, 1.6f};
            return true;
        };
        return scene;
    }

    Keyframe AttachedAt(std::uint64_t t, const Vec3& rel, Blend blend, bool lookAt)
    {
        Keyframe k = FreeAt(t, {}, blend, true);
        k.attachTarget = kRunner;
        k.attachType = camera::AttachType::Default;
        k.relPosition = rel;
        k.relRotation = {-10.0f, 0.0f, 0.0f};
        k.speedIndex = 0; // 5 %
        if (lookAt)
            k.lookAtTarget = kRunner;
        return k;
    }

    // Plays [fromMs, toMs] at 60 real frames per second and `rate` (the
    // playhead rate), with the springs, the way UpdateReplayCamera does.
    // Calls `frame` with the playhead and the camera.
    template <typename Frame>
    void PlayFrames(const Track& track, const camera::Scene& scene, double rate, std::uint64_t fromMs, std::uint64_t toMs,
        Frame frame)
    {
        camera::SpringState spring;
        const float dt = camera::ClipStepSeconds(1.0f / 60.0f, rate);
        for (double t = static_cast<double>(fromMs); t <= static_cast<double>(toMs); t += dt * 1000.0)
        {
            CameraState played{};
            camera::EvaluateMarkers(track, static_cast<std::uint64_t>(t), scene, played, &spring, dt);
            frame(static_cast<std::uint64_t>(t), played);
        }
    }

    void TestFollowSlowMotion()
    {
        Check(Near(camera::ClipStepSeconds(1.0f / 60.0f, 0.05), 0.05f / 60.0f, 1e-6f) &&
                camera::ClipStepSeconds(0.0f, 1.0) == 0.0f && camera::ClipStepSeconds(0.016f, 0.0) == 0.0f,
            "follow: the springs' step is the clip time (real x rate)");
        for (const bool lookAt : {false, true})
        {
            Track track;
            track.AddMarker(AttachedAt(1000, {0.0f, -4.0f, 1.5f}, Blend::Smooth, lookAt));
            track.AddMarker(AttachedAt(1600, {2.5f, -3.0f, 1.2f}, Blend::Smooth, lookAt));
            track.AddMarker(AttachedAt(2200, {-2.0f, -3.5f, 1.8f}, Blend::None, lookAt));
            for (const double rate : {0.05, 1.0})
            {
                // The same markers on a still target and on one running at
                // 6 m/s: the camera relative to the target must not differ.
                std::vector<Vec3> still;
                const camera::Scene stillScene = RunnerScene(0.0f);
                PlayFrames(track, stillScene, rate, 1000, 2200, [&](std::uint64_t, const CameraState& c) {
                    still.push_back(c.position);
                });
                const camera::Scene runScene = RunnerScene(6.0f);
                std::size_t n = 0;
                float worst = 0.0f, worstFrame = 0.0f;
                bool sprung = false;
                PlayFrames(track, runScene, rate, 1000, 2200, [&](std::uint64_t t, const CameraState& c) {
                    camera::EntityState runner;
                    runScene.lookup(kRunner, t, runner);
                    if (n < still.size())
                    {
                        const Vec3 rel{c.position.x - runner.position.x, c.position.y, c.position.z};
                        worst = std::max(worst, std::fabs(rel.x - still[n].x) + std::fabs(rel.y - still[n].y) + std::fabs(rel.z - still[n].z));
                    }
                    CameraState raw{};
                    camera::EvaluateMarkers(track, t, runScene, raw);
                    sprung = sprung || !SameCam(c, raw, 0.001f, 0.01f);
                    if (lookAt)
                    {
                        editor::Vec2 screen{};
                        camera::ProjectToScreen(c, runner.head, runScene.aspect, screen);
                        worstFrame = std::max(worstFrame, std::fabs(screen.x) + std::fabs(screen.y));
                    }
                    ++n;
                });
                char text[160];
                std::snprintf(text, sizeof(text), "follow: attached at %.0f %% speed%s, the camera follows a 6 m/s target exactly (%.4f m)",
                    rate * 100.0, lookAt ? " with Look At" : "", worst);
                Check(n == still.size() && worst < 0.01f, text);
                Check(sprung, "follow: the Smooth springs still round the blend");
                if (lookAt)
                {
                    std::snprintf(text, sizeof(text), "follow: the Look At target stays framed during the Smooth blend (%.4f)", worstFrame);
                    Check(worstFrame < 0.01f, text);
                }
            }
        }

        // Look At only (the camera's own world path): the target keeps its
        // framing while the springs round the path.
        Track look;
        Keyframe a = FreeAt(1000, {{0.0f, -6.0f, 2.0f}, {}, 45.0f}, Blend::Smooth, true);
        Keyframe b = FreeAt(1600, {{3.0f, -5.0f, 2.0f}, {}, 45.0f}, Blend::Smooth, true);
        Keyframe c = FreeAt(2200, {{6.0f, -6.0f, 2.5f}, {}, 45.0f}, Blend::None, true);
        for (Keyframe* k : {&a, &b, &c})
        {
            k->lookAtTarget = kRunner;
            k->speedIndex = 0;
        }
        look.AddMarker(a);
        look.AddMarker(b);
        look.AddMarker(c);
        const camera::Scene runScene = RunnerScene(6.0f);
        float worstFrame = 0.0f;
        PlayFrames(look, runScene, 0.05, 1000, 2200, [&](std::uint64_t t, const CameraState& cam) {
            camera::EntityState runner;
            runScene.lookup(kRunner, t, runner);
            editor::Vec2 screen{};
            camera::ProjectToScreen(cam, runner.head, runScene.aspect, screen);
            worstFrame = std::max(worstFrame, std::fabs(screen.x) + std::fabs(screen.y));
        });
        Check(worstFrame < 0.01f, "follow: Look At with Smooth at 5 %: the target stays at its framing");

        // The plan reports the springs' anchor and lag (CAMERA_FOLLOW).
        Track track;
        track.AddMarker(AttachedAt(1000, {0.0f, -4.0f, 1.5f}, Blend::Smooth, false));
        track.AddMarker(AttachedAt(1600, {2.5f, -3.0f, 1.2f}, Blend::Smooth, false));
        track.AddMarker(AttachedAt(2200, {-2.0f, -3.5f, 1.8f}, Blend::None, false));
        camera::SpringState spring;
        camera::SegmentPlan plan;
        CameraState out{};
        for (std::uint64_t t = 1000; t <= 1300; t += 1)
            camera::EvaluateMarkers(track, t, runScene, out, &spring, 0.001f, &plan);
        Check(plan.smooth && plan.smoothAnchor == kRunner && spring.anchor == kRunner, "follow: the springs run on the attach target");
    }

    // =====================================================================
    // 20. Focus on a character: the focus plane is on the chosen character's
    // head, at its depth along the view axis; Target without a character is the
    // player; a missing target is the engine's autofocus, never a fixed 10 m.
    void TestFocusPlan()
    {
        const camera::Scene scene = MakeScene();
        Check(Near(camera::DofFStop(0.0f), 22.0f, 0.001f) && Near(camera::DofFStop(100.0f), 0.5f, 0.001f) &&
                Near(camera::DofFStop(50.0f), 3.317f, 0.01f),
            "focus: intensity 0..100 is f/22..f/0.5");
        // The ped at t=1000: (5, 11, 0), head at z 1.6. Camera 3 m behind
        // the head, looking along +Y.
        const CameraState straight{{5.0f, 8.0f, 1.6f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        Keyframe k = FreeAt(1000, straight);
        k.dof = camera::DofMode::Custom;
        k.focus = camera::FocusMode::Target;
        k.focusTarget = kPed;
        camera::FocusPlan f = camera::PlanFocus(&k, straight, scene, 1000);
        Check(f.source == camera::FocusSource::Target && f.entity == kPed && Near(f.distance, 3.0f, 0.001f) &&
                Near(f.straightMeters, 3.0f, 0.001f) && Near(f.fstop, 3.317f, 0.01f),
            "focus: Target on the character's head, 3 m");
        CameraState turned = straight;
        turned.rotation.z = 30.0f; // the head 30 degrees off the axis
        f = camera::PlanFocus(&k, turned, scene, 1000);
        Check(Near(f.distance, 3.0f * std::cos(30.0f * 3.14159265f / 180.0f), 0.001f) && Near(f.straightMeters, 3.0f, 0.001f),
            "focus: the depth along the view axis, not the straight line");
        k.focusTarget = 0;
        f = camera::PlanFocus(&k, straight, scene, 1000);
        const camera::EntityState main = MainAt(1000);
        const float mainDx = main.position.x - 5.0f, mainDy = main.position.y - 8.0f, mainDz = main.position.z + 0.3f - 1.6f;
        Check(f.source == camera::FocusSource::Target && f.entity == kMain &&
                Near(f.straightMeters, std::sqrt(mainDx * mainDx + mainDy * mainDy + mainDz * mainDz), 0.001f),
            "focus: Target without a character is the player (the reference's index 0)");
        k.focusTarget = 99;
        f = camera::PlanFocus(&k, straight, scene, 1000);
        Check(f.source == camera::FocusSource::Engine && f.targetMissing, "focus: a target not there: the engine's autofocus, not 10 m");
        k.focus = camera::FocusMode::Manual;
        k.focusDistance = 7.5f;
        f = camera::PlanFocus(&k, straight, scene, 1000);
        Check(f.source == camera::FocusSource::Manual && Near(f.distance, 7.5f, 0.001f), "focus: Manual keeps its distance");
        k.focus = camera::FocusMode::Auto;
        k.lookAtTarget = kPed;
        f = camera::PlanFocus(&k, straight, scene, 1000);
        Check(f.source == camera::FocusSource::LookAt && f.entity == kPed && Near(f.distance, 3.0f, 0.001f), "focus: Auto on the Look At target");
        k.lookAtTarget = 0;
        k.attachTarget = kPed;
        f = camera::PlanFocus(&k, straight, scene, 1000);
        Check(f.source == camera::FocusSource::Attach && f.entity == kPed, "focus: Auto without Look At on the attach target");
        k.attachTarget = 0;
        const CameraState atMain{{0.5f, -5.0f, 0.3f}, {0.0f, 0.0f, 0.0f}, 45.0f}; // the main character 5 m ahead
        f = camera::PlanFocus(&k, atMain, scene, 1000);
        Check(f.source == camera::FocusSource::MainCharacter && Near(f.distance, 5.0f, 0.001f), "focus: Auto without targets on the player on screen");
        const CameraState away{{0.5f, -5.0f, 0.3f}, {0.0f, 0.0f, 180.0f}, 45.0f};
        f = camera::PlanFocus(&k, away, scene, 1000);
        Check(f.source == camera::FocusSource::Engine, "focus: Auto with nobody on screen: the engine's autofocus");
        const CameraState close{{5.0f, 10.95f, 1.6f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        k.focus = camera::FocusMode::Target;
        k.focusTarget = kPed;
        f = camera::PlanFocus(&k, close, scene, 1000);
        Check(Near(f.distance, camera::kFocusMinMeters, 0.001f), "focus: never nearer than the photo mode's 0.2 m");
        k.dof = camera::DofMode::None;
        Check(camera::PlanFocus(&k, straight, scene, 1000).source == camera::FocusSource::Sharp, "focus: DOF None is sharp");
        k.dof = camera::DofMode::Default;
        Check(camera::PlanFocus(&k, straight, scene, 1000).source == camera::FocusSource::Default &&
                camera::PlanFocus(nullptr, straight, scene, 1000).source == camera::FocusSource::Default,
            "focus: DOF Default leaves the camera's own");
    }

    // The last marker, whose camera type cannot be changed in the editor, is
    // the last one that is not an Anchor, with another one before it; none on
    // a legacy track.
    void TestIsLastMarker()
    {
        Track clip = MakeClip();
        const std::uint32_t start = clip.Keyframes()[0].id;
        const std::uint32_t end = clip.Keyframes()[1].id;
        Check(camera::IsLastMarker(clip, end) && !camera::IsLastMarker(clip, start), "last marker: the clip's end");
        Check(!camera::IsLastMarker(clip, 0) && !camera::IsLastMarker(clip, 999), "last marker: no id, unknown id");
        Keyframe anchor;
        anchor.timeMs = 10500;
        anchor.markerType = camera::MarkerType::Anchor;
        const std::uint32_t anchorId = clip.AddMarker(anchor);
        Check(camera::IsLastMarker(clip, end) && !camera::IsLastMarker(clip, anchorId),
            "last marker: an Anchor after it does not count");
        Track single;
        Keyframe only;
        only.timeMs = 0;
        const std::uint32_t onlyId = single.AddMarker(only);
        Check(!camera::IsLastMarker(single, onlyId), "last marker: none with a single marker");
        Track legacy = MakeClip();
        legacy.SetLegacyBlendAnyMode(true);
        Check(!camera::IsLastMarker(legacy, legacy.Keyframes()[1].id), "last marker: none on a legacy track");
    }
}

int RunCameraMarkerTests()
{
    TestClipBounds();
    TestAddInherits();
    TestDelete();
    TestLastMarkerView();
    TestMove();
    TestCopyPasteReset();
    TestCameraModes();
    TestBlends();
    TestSpeed();
    TestShakeRules();
    TestFades();
    TestDof();
    TestLimits();
    TestAnyTime();
    TestScrub();
    TestLegacy();
    TestEditCamera();
    TestSameMarkers();
    TestFollowSlowMotion();
    TestFocusPlan();
    TestIsLastMarker();
    std::printf("  camera markers: %d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0)
        std::puts("Camera marker tests passed.");
    return g_failures;
}
