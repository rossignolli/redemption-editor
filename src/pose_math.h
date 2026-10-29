#pragma once

// PoseTrack v2 math and quantization primitives.
//
// Bone transforms in the RDR2 1491.50 skeleton pose buffer are 48-byte QTS
// records (live evidence): row 0 is a unit quaternion (x, y, z, w), row 1 is
// translation (x, y, z) plus an opaque per-bone w value, row 2 is scale
// (x, y, z, w). The helpers below never assume anything else about the engine;
// they only operate on that record.

#include <cstdint>

namespace replay::pose
{
    struct Quat
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct BoneQts
    {
        Quat rotation;
        float translation[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    static_assert(sizeof(BoneQts) == 48, "BoneQts must match the engine record");

    // Order in which a parent object transform and a child local transform
    // compose into the child object transform. Discovered per skeleton, not
    // assumed (see pose_hierarchy.h).
    enum class CompositionOrder : std::uint8_t
    {
        ParentThenLocal = 0, // q = qp * ql, t = tp + rotate(qp, tl)
        LocalThenParent = 1, // q = ql * qp, t = tp + rotate(conj(qp), tl)
    };

    Quat Normalize(Quat q);
    Quat Conjugate(Quat q);
    Quat Multiply(Quat a, Quat b);
    float Dot(Quat a, Quat b);
    // Smallest angle between two orientations, in radians (sign-agnostic).
    float AngleBetween(Quat a, Quat b);
    Quat Slerp(Quat a, Quat b, float t);
    Vec3 Rotate(Quat q, Vec3 v);

    // Rotation whose columns are the entity basis vectors (right = +X,
    // forward = +Y, up = +Z). Note: GET_ENTITY_MATRIX returns forward
    // first, then right (live evidence), whatever the header says.
    Quat QuatFromBasis(Vec3 right, Vec3 forward, Vec3 up);
    void BasisFromQuat(Quat q, Vec3& right, Vec3& forward, Vec3& up);

    // Entity world transform as the engine stores it in the skeleton parent
    // matrix (RDR2 1491.50, live evidence): four float4 columns
    // right, forward, up, position; the position is relative to a 32 m cell
    // whose signed 16-bit indices (x low, y high) are packed in the position
    // column's w lane. World = cell * 32 + local.
    struct WorldTransform
    {
        Vec3 position;
        Quat rotation;
    };

    constexpr float kWorldCellSize = 32.0f;

    bool DecodeWorldMatrix(const float matrix[16], WorldTransform& transform);
    WorldTransform InterpolateWorld(const WorldTransform& a, const WorldTransform& b, float t);

    // With inheritScale the parent object scale multiplies the child local
    // translation and scale (non-uniform scaled rigs, e.g. horses).
    BoneQts Compose(
        const BoneQts& parentObject,
        const BoneQts& local,
        CompositionOrder order,
        bool inheritScale = false);

    // Slerp rotation, lerp translation and scale. The opaque translation w
    // is taken from `a` when t < 0.5 and from `b` otherwise.
    BoneQts Interpolate(const BoneQts& a, const BoneQts& b, float t);

    float TranslationDistance(const BoneQts& a, const BoneQts& b);

    // std::lround(value) for every float, without the library call
    // (tests/pose_capture_tests.cpp checks millions of pairs near the
    // threshold, both hemispheres, near 0 and pi, off-unit and NaN).
    long RoundHalfAwayFromZero(float value);
    // The quantizers use RoundHalfAwayFromZero (default) or std::lround
    // (slower, same codes).
    void SetFastRounding(bool enabled);
    // AngleBetween(a, b) > epsilon: from the squared vector part of
    // conj(a) * b against tan^2(epsilon/2) times its squared w (no
    // normalization, square root or atan2) when both are near unit and the
    // result is outside an error band around the threshold; the exact
    // AngleBetween otherwise.
    struct AngleTest
    {
        float epsilon = 0.0f;
        float tanHalfSquared = 0.0f;
        float band = 0.0f;
        bool cheap = false;
    };
    AngleTest MakeAngleTest(float epsilon);
    bool AngleExceeds(const AngleTest& test, Quat a, Quat b);
    // TranslationDistance(a, b) > epsilon, squared away from the threshold.
    bool TranslationExceeds(const BoneQts& a, const BoneQts& b, float epsilon);

    // Interpolate and Compose with SSE on one bone at a time, the
    // same float operations in the same order (products, then sums left to
    // right, exact negations for the subtractions), so the result is bit for
    // bit the scalar one (tests/pose_sample_tests.cpp checks it on random and
    // recorded bones). The Slerp arc path (dot at or below 0.9995) calls the
    // scalar code.
    void InterpolateSimd(const BoneQts& a, const BoneQts& b, float t, BoneQts& out);
    void ComposeSimd(
        const BoneQts& parentObject,
        const BoneQts& local,
        CompositionOrder order,
        bool inheritScale,
        BoneQts& out);

    // Smallest-three quaternion packing: 2 bits select the dropped largest
    // component, the other three are stored in [-1/sqrt2, 1/sqrt2].
    // 32-bit: 3 x 10 bits. 64-bit: 3 x 20 bits.
    std::uint32_t PackQuat32(Quat q);
    Quat UnpackQuat32(std::uint32_t packed);
    std::uint64_t PackQuat64(Quat q);
    Quat UnpackQuat64(std::uint64_t packed);

    // Fixed-range signed 16-bit translation. Returns false when any axis is
    // outside [-range, range] so the caller can fall back to float storage.
    bool PackTranslation16(
        const float translation[3],
        float range,
        std::int16_t packed[3]);
    void UnpackTranslation16(
        const std::int16_t packed[3],
        float range,
        float translation[3]);
}
