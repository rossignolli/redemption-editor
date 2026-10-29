// The replay camera attached to a bone (the bone's world state from the recorded
// root and object space bone, the attach frame, the blend between two
// markers on the same bone, the springs' anchor), the bone references and
// their step through a target's bones, the marker's extra fields (bones,
// attach pose numbers, near clip, DOF numbers) validated, the near clip of
// the instant, the DOF lens and depth in focus, and CAMR v8 in a clip and a
// project (an older payload still reads).
#include "camera_bones.h"
#include "camera_track.h"
#include "clip_file.h"
#include "old_camera_payload.h"
#include "replay_project.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using camera::AttachType;
    using camera::Blend;
    using camera::CameraMode;
    using camera::EntityState;
    using camera::Keyframe;
    using camera::Track;
    using editor::CameraState;
    using editor::Vec3;
    namespace bones = camera::bones;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float actual, float expected, float tolerance = 0.01f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    bool NearVec(const Vec3& a, const Vec3& b, float tolerance = 0.01f)
    {
        return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
    }

    // A quaternion (x, y, z, w) turning `degrees` about +Z.
    void Heading(float degrees, float q[4])
    {
        const float half = degrees * 3.14159265f / 360.0f;
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = std::sin(half);
        q[3] = std::cos(half);
    }

    // The puppet 7 walks along +X at 1 m/s, facing 90 degrees; its right
    // hand (named Right Hand, or index 282) sits 1 m in front of the root
    // and 0.5 m up, turned 30 degrees of pitch in object space.
    constexpr std::uint32_t kPuppet = 7;
    const std::uint32_t kHand = bones::Named(4); // Human "Right Hand"

    EntityState RootAt(std::uint64_t t)
    {
        float q[4];
        Heading(90.0f, q);
        const Vec3 at{10.0f + static_cast<float>(t) / 1000.0f, 20.0f, 0.0f};
        return camera::EntityFromRecorded(at, q, nullptr);
    }

    EntityState HandAt(std::uint64_t t)
    {
        float root[4];
        Heading(90.0f, root);
        const float half = 30.0f * 3.14159265f / 360.0f;
        const float bone[4] = {std::sin(half), 0.0f, 0.0f, std::cos(half)}; // pitch 30 (about +X)
        const float offset[3] = {0.0f, 1.0f, 0.5f};
        return camera::EntityFromBone({10.0f + static_cast<float>(t) / 1000.0f, 20.0f, 0.0f}, root, offset, bone);
    }

    camera::Scene SceneWithBones(bool withBones)
    {
        camera::Scene scene;
        scene.lookup = [](std::uint32_t id, std::uint64_t t, EntityState& s) {
            if (id != kPuppet)
                return false;
            s = RootAt(t);
            return true;
        };
        if (withBones)
            scene.boneLookup = [](std::uint32_t id, std::uint32_t bone, std::uint64_t t, EntityState& s) {
                if (id != kPuppet || bone != kHand)
                    return false;
                s = HandAt(t);
                return true;
            };
        return scene;
    }

    Keyframe AttachedAt(std::uint64_t t, const Vec3& rel, Blend blend = Blend::None)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.blend = blend;
        k.attachTarget = kPuppet;
        k.attachBone = kHand;
        k.attachType = AttachType::Rigid;
        k.relPosition = rel;
        return k;
    }

    // ---- 1. A bone's world state and the attach frame on it.
    void TestBoneMath()
    {
        const EntityState hand = HandAt(0);
        // Root at (10, 20, 0) facing 90: its +Y (forward) is world -X.
        Check(NearVec(hand.position, {9.0f, 20.0f, 0.5f}), "bone: root x object space translation");
        Check(Near(hand.heading, 90.0f, 0.05f) && Near(hand.pitch, 30.0f, 0.05f) && Near(hand.roll, 0.0f, 0.05f),
            "bone: world rotation is root x bone (heading 90, pitch 30)");
        Check(hand.hasHead && NearVec(hand.head, hand.position), "bone: the head is the bone itself (Look At aims at it)");
        // A non unit quaternion is normalised; a null one is identity.
        const float big[4] = {0.0f, 0.0f, 0.0f, 3.0f};
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float t[3] = {1.0f, 2.0f, 3.0f};
        const EntityState plain = camera::EntityFromBone({0.0f, 0.0f, 0.0f}, big, t, zero);
        Check(NearVec(plain.position, {1.0f, 2.0f, 3.0f}) && Near(plain.heading, 0.0f) && Near(plain.pitch, 0.0f),
            "bone: scaled and null quaternions are safe");

        // Rigid on the bone: 2 m behind it in its frame.
        const camera::Scene scene = SceneWithBones(true);
        Keyframe k = AttachedAt(0, {0.0f, -2.0f, 0.0f});
        const camera::LivePose live = camera::Resolve(k, 0, scene);
        // Behind = -forward of the bone frame (heading 90, pitch 30): forward is (-cos30, 0, sin30).
        const Vec3 expected{9.0f + 2.0f * std::cos(30.0f * 3.14159265f / 180.0f), 20.0f, 0.5f - 2.0f * 0.5f};
        Check(!live.targetMissing && NearVec(live.pose.position, expected, 0.02f), "attach: Rigid on the bone rides its full rotation");
        // Default on the bone: only its heading.
        k.attachType = AttachType::Default;
        const camera::LivePose flat = camera::Resolve(k, 0, scene);
        Check(NearVec(flat.pose.position, {11.0f, 20.0f, 0.5f}, 0.02f), "attach: Default on the bone takes its heading only");
        // The view stored and given back (Edit Camera's SetMarkerView).
        Keyframe stored = AttachedAt(0, {});
        const CameraState view{{9.5f, 21.0f, 1.2f}, {-5.0f, 0.0f, 120.0f}, 40.0f};
        camera::SetMarkerView(stored, view, scene);
        const camera::LivePose back = camera::Resolve(stored, 0, scene);
        const Vec3 rotation = Track::ToEulerDegrees(back.pose.orientation);
        Check(NearVec(back.pose.position, view.position, 0.01f) && Near(rotation.z, 120.0f, 0.1f) && Near(rotation.x, -5.0f, 0.1f),
            "attach: a view stored relative to the bone comes back");
        // The bone moves with the clip: 1 s later the camera moved 1 m along +X.
        const camera::LivePose later = camera::Resolve(stored, 1000, scene);
        Check(NearVec(later.pose.position, {10.5f, 21.0f, 1.2f}, 0.01f), "attach: follows the bone over time (play, seek, scrub)");
    }

    // ---- 2. A missing bone falls back to the root; a missing entity keeps the stored pose.
    void TestMissing()
    {
        const camera::Scene noBones = SceneWithBones(false);
        Keyframe k = AttachedAt(0, {0.0f, -2.0f, 0.0f});
        EntityState state;
        bool boneMissing = false;
        Check(camera::LookupAttach(k, 0, noBones, state, &boneMissing) && boneMissing && NearVec(state.position, {10.0f, 20.0f, 0.0f}),
            "missing bone: the entity's root, flagged");
        const camera::LivePose live = camera::Resolve(k, 0, noBones);
        Check(!live.targetMissing && NearVec(live.pose.position, {12.0f, 20.0f, 0.0f}, 0.02f), "missing bone: the camera stays on the root");
        k.attachTarget = 99; // the entity left the clip (died, deleted, other clip)
        k.pose.position = {1.0f, 2.0f, 3.0f};
        const camera::LivePose gone = camera::Resolve(k, 0, noBones);
        Check(gone.targetMissing && NearVec(gone.pose.position, {1.0f, 2.0f, 3.0f}), "missing entity: the stored world pose");
        // A reference out of range is never asked.
        Keyframe bad = AttachedAt(0, {});
        bad.attachBone = (7u << 16) | 3u;
        Check(camera::LookupAttach(bad, 0, SceneWithBones(true), state, &boneMissing) && boneMissing,
            "an invalid reference is the root");
    }

    // ---- 3. The blend between two markers on the same bone, and the springs' anchor.
    void TestBlend()
    {
        const camera::Scene scene = SceneWithBones(true);
        Track track;
        track.AddMarker(AttachedAt(0, {0.0f, -2.0f, 0.0f}, Blend::Linear));
        track.AddMarker(AttachedAt(1000, {0.0f, -4.0f, 0.0f}, Blend::None));
        CameraState mid{};
        camera::SegmentPlan plan;
        Check(camera::EvaluateMarkersRaw(track, 500, scene, mid, &plan) && plan.blending, "blend: two markers on the same bone blend");
        const Keyframe& a = track.Keyframes()[0];
        const Keyframe& b = track.Keyframes()[1];
        const camera::LivePose pa = camera::Resolve(a, 500, scene);
        const camera::LivePose pb = camera::Resolve(b, 500, scene);
        const float t = plan.phase;
        const Vec3 expected{pa.pose.position.x + (pb.pose.position.x - pa.pose.position.x) * t,
            pa.pose.position.y + (pb.pose.position.y - pa.pose.position.y) * t,
            pa.pose.position.z + (pb.pose.position.z - pa.pose.position.z) * t};
        Check(NearVec(mid.position, expected, 0.02f), "blend: each end resolved on the moving bone, then lerped");

        camera::SpringState spring;
        Track smooth;
        smooth.AddMarker(AttachedAt(0, {0.0f, -2.0f, 0.0f}, Blend::Smooth));
        smooth.AddMarker(AttachedAt(1000, {0.0f, -2.0f, 0.0f}, Blend::None));
        CameraState out{};
        for (std::uint64_t at = 0; at <= 400; at += 33)
            camera::EvaluateMarkers(smooth, at, scene, out, &spring, 0.033f);
        Check(spring.anchor == kPuppet && spring.anchorBone == kHand, "springs: anchored on the target and its bone");
        const camera::LivePose own = camera::Resolve(smooth.Keyframes()[0], 400, scene);
        Check(NearVec(out.position, own.pose.position, 0.05f), "springs: the sprung camera follows the bone exactly");
    }

    // ---- 4. References and their step through a target's bones.
    void TestReferences()
    {
        Check(bones::Valid(0) && bones::Valid(bones::Named(0)) && bones::Valid(bones::Index(281)), "refs: root, named, index");
        Check(!bones::Valid(bones::Named(bones::kNamedCount)) && !bones::Valid(bones::Index(bones::kMaxBoneIndex)) &&
                  !bones::Valid((3u << 16) | 1u),
            "refs: out of the list, out of range, unknown mode");
        // Head (0) and Right Hand (4) found on the skeleton, 3 bones by index.
        const std::uint32_t mask = (1u << 0) | (1u << 4);
        std::uint32_t r = 0;
        r = bones::Step(r, 1, mask, 3);
        Check(r == bones::Named(0), "step: root, then the first named bone");
        r = bones::Step(r, 1, mask, 3);
        Check(r == bones::Named(4), "step: only the named bones the skeleton has");
        r = bones::Step(r, 1, mask, 3);
        Check(r == bones::Index(0), "step: then the full list by index");
        r = bones::Step(bones::Index(2), 1, mask, 3);
        Check(r == 0, "step: wraps to the root");
        Check(bones::Step(0, -1, mask, 3) == bones::Index(2), "step: backwards from the root to the last index");
        Check(bones::Step(0, 5, mask, 3) == bones::Index(2), "step: five at a time (PageUp)");
        Check(bones::Step(bones::Index(50), 1, 0, 0) == 0 && bones::Step(0, 1, 0, 0) == 0, "step: no skeleton known, root only");
        int humans = 0, weapons = 0;
        for (const bones::NamedBone& b : bones::kNamed)
        {
            humans += b.family == bones::Family::Human;
            weapons += b.family == bones::Family::Weapon;
            Check(b.names[0] != nullptr && b.label[0] != 0, "named: a label and at least one engine name");
        }
        Check(humans == 8 && weapons == 40 && std::strcmp(bones::kNamed[19].names[0], "gun_muzzle") == 0 &&
                  std::strcmp(bones::kNamed[20].names[0], "gun_gripr") == 0 && std::strcmp(bones::kNamed[21].names[0], "gun_barrels") == 0,
            "named: head, neck, chest, pelvis, hands, feet; the engine's weapon bone names (append only list)");
        // A weapon's named bones come in the editor's order
        // (Muzzle, Barrel, Cylinder, Hammer, Trigger, Lever, Pump, ..., Grip),
        // the ones past the first 32 too (u64 mask).
        int cylinder = -1, trigger = -1, root = -1;
        for (std::uint32_t n = 0; n < bones::kNamedCount; ++n)
        {
            const char* name = bones::kNamed[n].names[0];
            if (std::strcmp(name, "gun_drum") == 0)
                cylinder = static_cast<int>(n);
            if (std::strcmp(name, "WAPTrigger1") == 0)
                trigger = static_cast<int>(n);
            if (std::strcmp(name, "gun_root") == 0)
                root = static_cast<int>(n);
        }
        Check(cylinder > 21 && trigger > 21 && root >= 32, "named: Cylinder, Trigger and Gun Root are in the list (Gun Root past bit 32)");
        const std::uint64_t gun = (1ull << 19) | (1ull << 20) | (1ull << 21) | (1ull << cylinder) | (1ull << trigger) | (1ull << root);
        std::uint32_t at = bones::Step(0, 1, gun, 30);
        Check(at == bones::Named(19), "weapon order: Muzzle first");
        at = bones::Step(at, 1, gun, 30);
        Check(at == bones::Named(21), "weapon order: then Barrel");
        at = bones::Step(at, 1, gun, 30);
        Check(at == bones::Named(static_cast<std::uint32_t>(cylinder)), "weapon order: then Cylinder");
        at = bones::Step(at, 1, gun, 30);
        Check(at == bones::Named(static_cast<std::uint32_t>(trigger)), "weapon order: then Trigger");
        at = bones::Step(at, 1, gun, 30);
        Check(at == bones::Named(20), "weapon order: then Grip");
        at = bones::Step(at, 1, gun, 30);
        Check(at == bones::Named(static_cast<std::uint32_t>(root)), "weapon order: Gun Root after the useful ones");
        Check(bones::Step(at, 1, gun, 30) == bones::Index(0), "weapon order: then every bone by index");
    }

    // ---- 5. The marker's extra fields.
    void TestExtra()
    {
        Keyframe k;
        k.cameraMode = CameraMode::Free;
        editor::CameraMarkerExtra e{};
        e.fields = editor::CameraExtraAttachBone;
        e.attachBone = kHand;
        Check(camera::SetMarkerExtra(k, e) && k.attachBone == 0, "extra: a bone without a target is dropped");
        k.attachTarget = kPuppet;
        Check(camera::SetMarkerExtra(k, e) && k.attachBone == kHand && (k.editedMask & editor::CameraOptionExtra),
            "extra: the bone of the attach target, marked edited");
        e.fields = editor::CameraExtraNearClip;
        e.nearClip = 0.005f;
        Check(!camera::SetMarkerExtra(k, e) && k.nearClip == 0.0f, "extra: near clip under 0.01 m refused");
        e.nearClip = 0.03f;
        Check(camera::SetMarkerExtra(k, e) && Near(camera::MarkerNearClip(k), 0.03f, 1e-6f), "extra: near clip 0.03 m");
        e.nearClip = 0.0f;
        Check(camera::SetMarkerExtra(k, e) && Near(camera::MarkerNearClip(k), editor::kNearClipDefault, 1e-6f), "extra: 0 is the default 0.1 m");
        e.fields = editor::CameraExtraDofFStop | editor::CameraExtraDofLens;
        e.dofFStop = 40.0f;
        e.dofLensMm = 85.0f;
        Check(!camera::SetMarkerExtra(k, e) && k.dofLensMm == 0.0f, "extra: all or nothing (f/40 refused, the lens not taken)");
        e.dofFStop = 1.4f;
        Check(camera::SetMarkerExtra(k, e) && Near(k.dofFStop, 1.4f, 1e-6f) && Near(k.dofLensMm, 85.0f, 1e-6f), "extra: f/1.4 and 85 mm");
        e.fields = editor::CameraExtraRelPosition | editor::CameraExtraRelRotation;
        e.relPosition = {0.1f, -0.3f, 0.05f};
        e.relRotation = {120.0f, 370.0f, -190.0f};
        Check(camera::SetMarkerExtra(k, e) && NearVec(k.relPosition, {0.1f, -0.3f, 0.05f}) && Near(k.relRotation.x, 89.5f) &&
                  Near(k.relRotation.y, 10.0f) && Near(k.relRotation.z, 170.0f),
            "extra: the attach pose as numbers (pitch held to 89.5, angles wrapped)");
        e.relPosition = {std::nanf(""), 0.0f, 0.0f};
        Check(!camera::SetMarkerExtra(k, e), "extra: a non finite offset refused");

        // A new target: an index belongs to the old skeleton, a named bone travels.
        k.attachBone = bones::Index(282);
        camera::SetAttach(k, 8);
        Check(k.attachBone == 0, "target change: a skeleton index is dropped");
        k.attachBone = kHand;
        camera::SetAttach(k, 9);
        Check(k.attachBone == kHand, "target change: a named bone travels");
        camera::SetAttach(k, 0);
        Check(k.attachBone == 0, "no target: no bone");
        k.lookAtTarget = kPuppet;
        k.lookAtBone = bones::Index(5);
        camera::SetLookAt(k, 3);
        Check(k.lookAtBone == 0, "Look At target change: the index dropped too");

        // A new marker inherits them; the snapshot carries them.
        Keyframe previous = AttachedAt(0, {0.0f, -1.0f, 0.0f});
        previous.lookAtTarget = kPuppet;
        previous.lookAtBone = bones::Named(0);
        previous.nearClip = 0.02f;
        previous.dofFStop = 2.0f;
        previous.dofLensMm = 50.0f;
        Keyframe fresh;
        camera::InheritFrom(fresh, previous);
        Check(fresh.attachBone == kHand && fresh.lookAtBone == bones::Named(0) && fresh.nearClip == 0.02f && fresh.dofFStop == 2.0f &&
                  fresh.dofLensMm == 50.0f,
            "inherit: bones, near clip and DOF numbers");
        editor::CameraMarkerExtra snap{};
        camera::FillSnapshotExtra(fresh, snap);
        Check(snap.attachBone == kHand && snap.nearClip == 0.02f && NearVec(snap.relPosition, {0.0f, -1.0f, 0.0f}), "snapshot: the extra entry");
        Track a, b;
        a.AddMarker(previous);
        b.AddMarker(previous);
        Check(camera::SameMarkers(a, b), "same markers");
        b.FindMutable(b.Keyframes()[0].id)->nearClip = 0.05f;
        Check(!camera::SameMarkers(a, b), "same markers: the near clip counts (undo step, unsaved changes)");
    }

    // ---- 6. Near clip of the instant, DOF lens and depth in focus.
    void TestNearClipAndDof()
    {
        Track track;
        Keyframe a = AttachedAt(0, {}, Blend::Linear);
        a.attachTarget = 0;
        a.attachBone = 0;
        a.nearClip = 0.02f;
        Keyframe b = a;
        b.timeMs = 1000;
        b.blend = Blend::None;
        b.nearClip = 0.0f;
        track.AddMarker(a);
        track.AddMarker(b);
        camera::SegmentPlan plan = camera::Plan(track, 500);
        const float mid = camera::NearClipAt(track, plan);
        Check(plan.blending && Near(mid, 0.02f + (0.1f - 0.02f) * plan.phase, 1e-4f), "near clip: lerped inside a blend");
        Check(Near(camera::NearClipAt(track, camera::SegmentPlan{}), editor::kNearClipDefault, 1e-6f), "near clip: default without a plan");

        // The lens: FOV 45 is a 29 mm (12 / tan 22.5), times 2.0 at intensity 0, 2.5 at 100.
        Check(Near(camera::DofLensMm(45.0f, 0.0f, 0.0f), 2.0f * 12.0f / std::tan(22.5f * 3.14159265f / 180.0f), 0.05f) &&
                  Near(camera::DofLensMm(45.0f, 100.0f, 0.0f), 2.5f * 12.0f / std::tan(22.5f * 3.14159265f / 180.0f), 0.05f),
            "lens: the focal length multiplier on the FOV's lens");
        Check(Near(camera::DofLensMm(45.0f, 50.0f, 135.0f), 135.0f) && Near(camera::DofLensMm(5.0f, 100.0f, 0.0f), 600.0f, 0.01f) &&
                  Near(camera::DofLensMm(45.0f, 50.0f, 5000.0f), 600.0f, 0.01f),
            "lens: the marker's own, held to 600 mm");
        float nearM = 0.0f, farM = 0.0f;
        camera::InFocusRange(2.8f, 85.0f, 3.0f, nearM, farM);
        Check(nearM > 2.85f && nearM < 3.0f && farM > 3.0f && farM < 3.2f, "depth in focus: 85 mm f/2.8 at 3 m is a few cm deep");
        camera::InFocusRange(22.0f, 24.0f, 10.0f, nearM, farM);
        Check(farM == 0.0f && nearM > 0.0f && nearM < 1.5f, "depth in focus: a wide lens at f/22 reaches infinity");

        // PlanFocus: the marker's own f-stop and lens over the intensity.
        Keyframe dof;
        dof.dof = editor::DofMode::Custom;
        dof.dofIntensity = 50.0f;
        dof.focus = editor::FocusMode::Manual;
        dof.focusDistance = 4.0f;
        const CameraState camera{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        camera::FocusPlan plain = camera::PlanFocus(&dof, camera, camera::Scene{}, 0);
        Check(Near(plain.fstop, camera::DofFStop(50.0f), 1e-4f) && plain.lensMm == 0.0f && Near(plain.distance, 4.0f),
            "focus: from the intensity when the numbers are Auto");
        dof.dofFStop = 1.2f;
        dof.dofLensMm = 100.0f;
        plain = camera::PlanFocus(&dof, camera, camera::Scene{}, 0);
        Check(Near(plain.fstop, 1.2f, 1e-6f) && Near(plain.lensMm, 100.0f, 1e-6f), "focus: the marker's f-stop and lens");
        // A Look At on a bone focuses the bone.
        Keyframe look = dof;
        look.focus = editor::FocusMode::Auto;
        look.lookAtTarget = kPuppet;
        look.lookAtBone = kHand;
        const CameraState fromBack{{9.0f, 15.0f, 0.5f}, {0.0f, 0.0f, 0.0f}, 45.0f};
        const camera::FocusPlan onBone = camera::PlanFocus(&look, fromBack, SceneWithBones(true), 0);
        Check(onBone.source == camera::FocusSource::LookAt && Near(onBone.distance, 5.0f, 0.02f), "focus: Look At on a bone focuses the bone");
    }

    // ---- 7. CAMR v8 in a clip and in a project; older payloads read.
    void TestFormats()
    {
        Check(clipfile::CameraSectionVersion() == 9, "CAMR: version 9 (the per-marker shake)");
        Track track;
        Keyframe a = AttachedAt(0, {0.0f, -2.0f, 0.3f}, Blend::Linear);
        a.lookAtTarget = kPuppet;
        a.lookAtBone = bones::Index(282);
        a.nearClip = 0.02f;
        a.dofFStop = 1.8f;
        a.dofLensMm = 70.0f;
        Keyframe b = AttachedAt(2000, {0.0f, -3.0f, 0.3f});
        track.AddMarker(a);
        track.AddMarker(b);
        const std::vector<std::uint8_t> bytes = clipfile::EncodeCameraTrack(track);
        Track back;
        std::string error;
        bool kept = clipfile::DecodeCameraTrack(bytes.data(), bytes.size(), 9, back, error) && back.Keyframes().size() == 2 &&
            clipfile::EncodeCameraTrack(back) == bytes;
        kept = kept && back.Keyframes()[0].attachBone == kHand && back.Keyframes()[0].lookAtBone == bones::Index(282) &&
            back.Keyframes()[0].nearClip == 0.02f && back.Keyframes()[0].dofFStop == 1.8f && back.Keyframes()[0].dofLensMm == 70.0f;
        Check(kept, "CAMR v8: round trip (bones, near clip, DOF numbers)");
        // v7: the payload an old build wrote (no bones, near clip, DOF numbers) reads with root, default near clip, DOF from the intensity.
        // Written by old_camera_payload.h; in v9 the v8 fields sit before the
        // marker's shake and origin (62 bytes).
        const std::size_t header = 4 + 1 + 8, per = (bytes.size() - header) / 2, v8End = per - 62;
        const std::vector<std::uint8_t> v7 = testpayload::OldCamera(track, 7);
        Track old;
        Check(clipfile::DecodeCameraTrack(v7.data(), v7.size(), 7, old, error) && old.Keyframes().size() == 2 &&
                  old.Keyframes()[0].attachBone == 0 && old.Keyframes()[0].nearClip == 0.0f && old.Keyframes()[0].dofFStop == 0.0f &&
                  old.Keyframes()[0].attachTarget == kPuppet,
            "CAMR v7 still reads (an old project or clip opens)");
        Check(!clipfile::DecodeCameraTrack(v7.data(), v7.size(), 8, old, error), "CAMR: a v7 payload is not read as v8");
        std::vector<std::uint8_t> broken = bytes;
        const std::uint32_t badRef = (9u << 16) | 1u;
        std::memcpy(broken.data() + header + v8End - 20, &badRef, sizeof(badRef));
        Check(!clipfile::DecodeCameraTrack(broken.data(), broken.size(), 9, old, error), "CAMR v8: a bone reference out of range refused");
        std::vector<std::uint8_t> nearBad = bytes;
        const float tiny = 0.001f;
        std::memcpy(nearBad.data() + header + v8End - 12, &tiny, sizeof(tiny));
        Check(!clipfile::DecodeCameraTrack(nearBad.data(), nearBad.size(), 9, old, error), "CAMR v8: a near clip out of range refused");

        // The project keeps each clip's markers in the same bytes.
        project::Project p;
        p.name = "Bone camera";
        project::Clip clip;
        Check(project::MakeClip("001-Valentine.rpclip", 10000, clip), "project: a clip");
        Track fitted = project::FitMarkers(track, 0, 10000);
        Check(project::TakeMarkers(clip, fitted), "project: the markers taken");
        project::AddClip(p, 0, clip);
        const std::vector<std::uint8_t> file = project::Encode(p);
        project::Project loaded;
        Check(project::Decode(file.data(), file.size(), loaded, error) && loaded.clips.size() == 1 && project::Encode(loaded) == file,
            "project: round trip with the bones");
        bool boned = false;
        for (const Keyframe& k : loaded.clips[0].markers.Keyframes())
            boned = boned || (k.attachBone == kHand && k.nearClip == 0.02f);
        Check(boned, "project: the bone and near clip survive the file");
    }
}

int RunCameraAttachTests()
{
    TestBoneMath();
    TestMissing();
    TestBlend();
    TestReferences();
    TestExtra();
    TestNearClipAndDof();
    TestFormats();
    {
        // A marker at the clip start, the record from 14 ms.
        namespace b = replay::camera::bones;
        std::uint32_t at = 0;
        Check(b::CameraRecordTime(0, 14, 9800, 100, at) && at == 14, "camera record time: before the first sample holds the first");
        Check(b::CameraRecordTime(300, 14, 9800, 100, at) && at == 300, "camera record time: inside the record unchanged");
        Check(b::CameraRecordTime(9890, 14, 9800, 100, at) && at == 9890, "camera record time: the end slack stays");
        Check(!b::CameraRecordTime(9901, 14, 9800, 100, at), "camera record time: past the end slack no record");
    }
    if (g_failures == 0)
        std::puts("camera-attach (bone attach, references, extra fields, near clip, dof lens, CAMR v8) tests passed");
    return g_failures;
}
