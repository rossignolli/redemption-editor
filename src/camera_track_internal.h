// camera_track_internal.h: the helpers the camera_track*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "camera_track.h"

#include "camera_bones.h"
#include "camera_shake.h"
#include "camera_shake_custom.h"
#include "camera_marker_shake.h"
#include "camera_spline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegreesToRadians = kPi / 180.0f;
    float Clamp01(float value);
    replay::camera::Quaternion Multiply(
        const replay::camera::Quaternion& a,
        const replay::camera::Quaternion& b);
    replay::camera::Quaternion Normalize(replay::camera::Quaternion value);
    replay::camera::Quaternion Slerp(
        replay::camera::Quaternion a,
        replay::camera::Quaternion b,
        float amount);
    float Lerp(float from, float to, float amount);
    float Hermite(
        float p0, float p1, float p2, float p3,
        double t0, double t1, double t2, double t3,
        double queryTime);

namespace replay::camera
{
    bool EvaluateGame(const std::vector<GameSample>& samples, std::uint64_t timeMs, editor::CameraState& camera,
        const std::vector<std::uint32_t>* cuts);
    editor::Vec3 ToHeadingFrame(const editor::Vec3& world, float headingDegrees);
    editor::Vec3 FromHeadingFrame(const editor::Vec3& local, float headingDegrees);
}

// ----------------------------------------------------------------------
// Marker model: rules, conversion and the evaluator (API 8, CAMR v3).

    using replay::editor::Vec2;
    using replay::editor::Vec3;

    // Row major 3x3 rotation. Columns are the camera's right (+X), forward
    // (+Y) and up (+Z) in world space; R = Rz(heading) Rx(pitch) Ry(roll),
    // the same order as the game's rotation order 2.
    struct Mat3
    {
        float m[3][3];
    };
    Mat3 Identity();
    Mat3 FromEuler(const Vec3& rotation);
    float WrapDegrees(float angle);
    Vec3 ToEuler(const Mat3& r);
    Mat3 Mul(const Mat3& a, const Mat3& b);
    Mat3 Transpose(const Mat3& a);
    Vec3 Mul(const Mat3& a, const Vec3& v);
    Vec3 Add(const Vec3& a, const Vec3& b);
    Vec3 Sub(const Vec3& a, const Vec3& b);
    Vec3 LerpVec(const Vec3& a, const Vec3& b, float t);
    float LerpAngle(float a, float b, float t);
    Mat3 AttachFrame(replay::camera::AttachType type, const replay::camera::EntityState& state);

    // A primed spring's state (in `from`, origin fromOrigin) re-expressed in
    // `to` (origin toOrigin): the same world pose and the same world velocity.
    // The angular rates are kept (Euler rates; the two frames are a target's
    // heading apart at most).
    void ReframeSpring(replay::camera::SpringState& spring, const Mat3& from, const Vec3& fromOrigin, const Mat3& to,
        const Vec3& toOrigin);
    Vec3 AimPoint(const replay::camera::EntityState& state);

namespace replay::camera
{
    std::uint32_t MarkerSpacingMs(std::uint8_t speedIndex);
        void ClearBorrowed(Keyframe& k);
    bool IsClipBound(const Track& track, std::uint32_t id);
    bool WithinClipBounds(const Track& track, std::uint64_t timeMs);
    void ApplyLastMarkerRule(Track& track);
    void SetAttach(Keyframe& marker, std::uint32_t target);
    void SetAttachType(Keyframe& marker, AttachType type);
    SegmentPlan Plan(const Track& track, std::uint64_t timeMs);
    editor::Vec3 LookAtWithScreenOffset(
        const editor::Vec3& point,
        const editor::Vec3& cameraPosition,
        const editor::Vec2& screenOffset,
        float fov,
        float roll,
        float aspect);
    bool ProjectToScreen(const editor::CameraState& camera, const editor::Vec3& point, float aspect, editor::Vec2& screen);
    LivePose Resolve(const Keyframe& marker, std::uint64_t timeMs, const Scene& scene);
    void SetMarkerView(Keyframe& marker, const editor::CameraState& view, const Scene& scene);
    bool LookupTarget(const Scene& scene, std::uint32_t id, std::uint32_t bone, std::uint64_t timeMs, EntityState& state,
        bool* boneMissing);
}
