#include "pose_math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <emmintrin.h>

namespace replay::pose
{
    namespace
    {
        constexpr float kInvSqrt2 = 0.70710678118654752f;

        float Component(const Quat& q, int index)
        {
            switch (index)
            {
            case 0: return q.x;
            case 1: return q.y;
            case 2: return q.z;
            default: return q.w;
            }
        }

        void SetComponent(Quat& q, int index, float value)
        {
            switch (index)
            {
            case 0: q.x = value; break;
            case 1: q.y = value; break;
            case 2: q.z = value; break;
            default: q.w = value; break;
            }
        }

        // Prepares q for smallest-three packing: normalized, largest
        // component positive. Returns the index of the largest component.
        int CanonicalLargest(Quat& q)
        {
            q = Normalize(q);
            int largest = 0;
            float largestAbs = std::fabs(q.x);
            for (int index = 1; index < 4; ++index)
            {
                const float value = std::fabs(Component(q, index));
                if (value > largestAbs)
                {
                    largestAbs = value;
                    largest = index;
                }
            }
            if (Component(q, largest) < 0.0f)
                q = {-q.x, -q.y, -q.z, -q.w};
            return largest;
        }

        bool g_fastRounding = true; // SetFastRounding

        std::uint64_t QuantizeUnit(float value, std::uint64_t maxCode)
        {
            const float normalized =
                (std::clamp(value, -kInvSqrt2, kInvSqrt2) + kInvSqrt2) /
                (2.0f * kInvSqrt2);
            // RoundHalfAwayFromZero is std::lround without the library call
            // (the same integer for every float).
            const float scaled = normalized * static_cast<float>(maxCode);
            return static_cast<std::uint64_t>(g_fastRounding ? RoundHalfAwayFromZero(scaled) : std::lround(scaled));
        }

        float DequantizeUnit(std::uint64_t code, std::uint64_t maxCode)
        {
            return static_cast<float>(code) / static_cast<float>(maxCode) *
                    (2.0f * kInvSqrt2) -
                kInvSqrt2;
        }

        template <typename Packed>
        Packed PackSmallestThree(Quat q, unsigned bits)
        {
            const int largest = CanonicalLargest(q);
            const std::uint64_t maxCode = (1ull << bits) - 1ull;
            std::uint64_t packed = static_cast<std::uint64_t>(largest)
                << (bits * 3u);
            unsigned shift = bits * 2u;
            for (int index = 0; index < 4; ++index)
            {
                if (index == largest)
                    continue;
                packed |= QuantizeUnit(Component(q, index), maxCode) << shift;
                shift -= bits;
            }
            return static_cast<Packed>(packed);
        }

        Quat UnpackSmallestThree(std::uint64_t packed, unsigned bits)
        {
            const std::uint64_t maxCode = (1ull << bits) - 1ull;
            const int largest = static_cast<int>((packed >> (bits * 3u)) & 3u);
            Quat q{};
            float sumSquares = 0.0f;
            unsigned shift = bits * 2u;
            for (int index = 0; index < 4; ++index)
            {
                if (index == largest)
                    continue;
                const float value =
                    DequantizeUnit((packed >> shift) & maxCode, maxCode);
                SetComponent(q, index, value);
                sumSquares += value * value;
                shift -= bits;
            }
            SetComponent(q, largest, std::sqrt(std::max(0.0f, 1.0f - sumSquares)));
            return Normalize(q);
        }
    }

    Quat Normalize(Quat q)
    {
        const float length =
            std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!(length > 1e-12f))
            return {};
        const float inverse = 1.0f / length;
        return {q.x * inverse, q.y * inverse, q.z * inverse, q.w * inverse};
    }

    Quat Conjugate(Quat q)
    {
        return {-q.x, -q.y, -q.z, q.w};
    }

    Quat Multiply(Quat a, Quat b)
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }

    float Dot(Quat a, Quat b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    float AngleBetween(Quat a, Quat b)
    {
        // atan2 of the relative rotation stays precise for tiny angles,
        // where acos(dot) collapses to float noise.
        const Quat relative = Multiply(Conjugate(Normalize(a)), Normalize(b));
        const float vector = std::sqrt(
            relative.x * relative.x + relative.y * relative.y + relative.z * relative.z);
        return 2.0f * std::atan2(vector, std::fabs(relative.w));
    }

    Quat Slerp(Quat a, Quat b, float t)
    {
        float dot = Dot(a, b);
        if (dot < 0.0f)
        {
            b = {-b.x, -b.y, -b.z, -b.w};
            dot = -dot;
        }
        if (dot > 0.9995f)
        {
            return Normalize({
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t,
                a.w + (b.w - a.w) * t,
            });
        }
        const float theta = std::acos(dot);
        const float sinTheta = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / sinTheta;
        const float wb = std::sin(t * theta) / sinTheta;
        return Normalize({
            a.x * wa + b.x * wb,
            a.y * wa + b.y * wb,
            a.z * wa + b.z * wb,
            a.w * wa + b.w * wb,
        });
    }

    Vec3 Rotate(Quat q, Vec3 v)
    {
        const Quat p{v.x, v.y, v.z, 0.0f};
        const Quat r = Multiply(Multiply(q, p), Conjugate(q));
        return {r.x, r.y, r.z};
    }

    Quat QuatFromBasis(Vec3 right, Vec3 forward, Vec3 up)
    {
        // Columns: right, forward, up. m[row][column].
        const float m00 = right.x, m01 = forward.x, m02 = up.x;
        const float m10 = right.y, m11 = forward.y, m12 = up.y;
        const float m20 = right.z, m21 = forward.z, m22 = up.z;
        const float trace = m00 + m11 + m22;
        Quat q{};
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
        }
        else if (m00 > m11 && m00 > m22)
        {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
        }
        else if (m11 > m22)
        {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
        }
        else
        {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
        }
        return Normalize(q);
    }

    void BasisFromQuat(Quat q, Vec3& right, Vec3& forward, Vec3& up)
    {
        right = Rotate(q, {1.0f, 0.0f, 0.0f});
        forward = Rotate(q, {0.0f, 1.0f, 0.0f});
        up = Rotate(q, {0.0f, 0.0f, 1.0f});
    }

    bool DecodeWorldMatrix(const float matrix[16], WorldTransform& transform)
    {
        const Vec3 right{matrix[0], matrix[1], matrix[2]};
        const Vec3 forward{matrix[4], matrix[5], matrix[6]};
        const Vec3 up{matrix[8], matrix[9], matrix[10]};
        const float lengthRight = right.x * right.x + right.y * right.y + right.z * right.z;
        const float lengthForward =
            forward.x * forward.x + forward.y * forward.y + forward.z * forward.z;
        if (!(std::fabs(lengthRight - 1.0f) < 0.01f) || !(std::fabs(lengthForward - 1.0f) < 0.01f))
            return false; // empty or not a rotation (no sample)

        std::uint32_t cellBits = 0;
        std::memcpy(&cellBits, &matrix[15], sizeof(cellBits));
        const auto cellX = static_cast<std::int16_t>(cellBits & 0xFFFFu);
        const auto cellY = static_cast<std::int16_t>(cellBits >> 16);
        transform.position = {
            static_cast<float>(cellX) * kWorldCellSize + matrix[12],
            static_cast<float>(cellY) * kWorldCellSize + matrix[13],
            matrix[14],
        };
        transform.rotation = QuatFromBasis(right, forward, up);
        return true;
    }

    WorldTransform InterpolateWorld(const WorldTransform& a, const WorldTransform& b, float t)
    {
        return {
            {
                a.position.x + (b.position.x - a.position.x) * t,
                a.position.y + (b.position.y - a.position.y) * t,
                a.position.z + (b.position.z - a.position.z) * t,
            },
            Slerp(a.rotation, b.rotation, t),
        };
    }

    BoneQts Compose(
        const BoneQts& parentObject,
        const BoneQts& local,
        CompositionOrder order,
        bool inheritScale)
    {
        BoneQts result = local;
        Vec3 localTranslation{
            local.translation[0],
            local.translation[1],
            local.translation[2],
        };
        if (inheritScale)
        {
            localTranslation.x *= parentObject.scale[0];
            localTranslation.y *= parentObject.scale[1];
            localTranslation.z *= parentObject.scale[2];
            for (int axis = 0; axis < 3; ++axis)
                result.scale[axis] = parentObject.scale[axis] * local.scale[axis];
        }
        Vec3 offset{};
        if (order == CompositionOrder::ParentThenLocal)
        {
            result.rotation =
                Normalize(Multiply(parentObject.rotation, local.rotation));
            offset = Rotate(parentObject.rotation, localTranslation);
        }
        else
        {
            result.rotation =
                Normalize(Multiply(local.rotation, parentObject.rotation));
            offset = Rotate(Conjugate(parentObject.rotation), localTranslation);
        }
        result.translation[0] = parentObject.translation[0] + offset.x;
        result.translation[1] = parentObject.translation[1] + offset.y;
        result.translation[2] = parentObject.translation[2] + offset.z;
        return result;
    }

    BoneQts Interpolate(const BoneQts& a, const BoneQts& b, float t)
    {
        BoneQts result = t < 0.5f ? a : b;
        result.rotation = Slerp(a.rotation, b.rotation, t);
        for (int axis = 0; axis < 3; ++axis)
        {
            result.translation[axis] =
                a.translation[axis] + (b.translation[axis] - a.translation[axis]) * t;
        }
        for (int axis = 0; axis < 4; ++axis)
            result.scale[axis] = a.scale[axis] + (b.scale[axis] - a.scale[axis]) * t;
        return result;
    }

    float TranslationDistance(const BoneQts& a, const BoneQts& b)
    {
        const float dx = a.translation[0] - b.translation[0];
        const float dy = a.translation[1] - b.translation[1];
        const float dz = a.translation[2] - b.translation[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    long RoundHalfAwayFromZero(float value)
    {
        // Below 2^23 every float with a fraction is exact after the
        // truncation: value - truncated is exact (same sign, less than 1
        // apart from an integer of the same binade or zero), so comparing
        // it with 0.5 is what lround does. NaN, infinities and larger values
        // (never produced by the callers) take the library call.
        if (!(std::fabs(value) < 8388608.0f))
            return std::lround(value);
        const int truncated = _mm_cvtt_ss2si(_mm_set_ss(value));
        const float fraction = value - static_cast<float>(truncated);
        if (fraction >= 0.5f)
            return truncated + 1;
        if (fraction <= -0.5f)
            return truncated - 1;
        return truncated;
    }

    void SetFastRounding(bool enabled)
    {
        g_fastRounding = enabled;
    }

    AngleTest MakeAngleTest(float epsilon)
    {
        AngleTest test;
        test.epsilon = epsilon;
        if (!(epsilon > 0.0f) || !(epsilon < 3.0f))
            return test; // cheap path off: every call takes AngleBetween
        const double tanHalf = std::tan(static_cast<double>(epsilon) * 0.5);
        // Worst absolute error of |v| (relative quaternion vector part) in
        // either computation, in the unnormalized scale (|a||b| <= 2), with
        // an 8x margin over the bound of the float products (about 4e-6):
        // near the threshold the cheap test defers to the exact one.
        constexpr double kVectorError = 3.0e-5;
        const double maxS = 2.0 * tanHalf; // |w| <= |a||b| <= 2
        test.tanHalfSquared = static_cast<float>(tanHalf * tanHalf);
        test.band = static_cast<float>(2.0 * maxS * kVectorError + kVectorError * kVectorError);
        test.cheap = true;
        return test;
    }

    bool AngleExceeds(const AngleTest& test, Quat a, Quat b)
    {
        if (test.cheap)
        {
            const float normA = Dot(a, a);
            const float normB = Dot(b, b);
            if (normA > 0.5f && normA < 2.0f && normB > 0.5f && normB < 2.0f)
            {
                // AngleBetween is 2 atan2(|v|, |w|) of conj(a) * b after
                // normalizing both; the normalization scales v and w alike,
                // so the angle passes epsilon when |v| > |w| tan(epsilon/2).
                const Quat relative = Multiply(Conjugate(a), b);
                const float vector =
                    relative.x * relative.x + relative.y * relative.y + relative.z * relative.z;
                const float limit = relative.w * relative.w * test.tanHalfSquared;
                if (vector > limit + test.band)
                    return true;
                if (vector < limit - test.band)
                    return false;
                // Near the threshold (or NaN): the exact test.
            }
        }
        return AngleBetween(a, b) > test.epsilon;
    }

    bool TranslationExceeds(const BoneQts& a, const BoneQts& b, float epsilon)
    {
        // TranslationDistance(a, b) > epsilon without the square root away
        // from the threshold; within 1e-4 of it (and for NaN) the exact test.
        const float dx = a.translation[0] - b.translation[0];
        const float dy = a.translation[1] - b.translation[1];
        const float dz = a.translation[2] - b.translation[2];
        const float squared = dx * dx + dy * dy + dz * dz;
        const float limit = epsilon * epsilon;
        if (squared > limit * 1.0001f)
            return true;
        if (squared < limit * 0.9999f)
            return false;
        return TranslationDistance(a, b) > epsilon;
    }

    std::uint32_t PackQuat32(Quat q)
    {
        return PackSmallestThree<std::uint32_t>(q, 10u);
    }

    Quat UnpackQuat32(std::uint32_t packed)
    {
        return UnpackSmallestThree(packed, 10u);
    }

    std::uint64_t PackQuat64(Quat q)
    {
        return PackSmallestThree<std::uint64_t>(q, 20u);
    }

    Quat UnpackQuat64(std::uint64_t packed)
    {
        return UnpackSmallestThree(packed, 20u);
    }

    bool PackTranslation16(
        const float translation[3],
        float range,
        std::int16_t packed[3])
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = translation[axis];
            if (!(std::fabs(value) <= range))
                return false;
            const float scaled = value / range * 32767.0f;
            packed[axis] = static_cast<std::int16_t>(
                g_fastRounding ? RoundHalfAwayFromZero(scaled) : std::lround(scaled));
        }
        return true;
    }

    void UnpackTranslation16(
        const std::int16_t packed[3],
        float range,
        float translation[3])
    {
        for (int axis = 0; axis < 3; ++axis)
            translation[axis] = static_cast<float>(packed[axis]) / 32767.0f * range;
    }

    namespace
    {
        // Lanes in memory order: x, y, z, w (Quat and the BoneQts rows).
        // ((v.x + v.y) + v.z) + v.w, the scalar left-to-right sum.
        inline float SumInOrder(__m128 v)
        {
            const float x = _mm_cvtss_f32(v);
            const float y = _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)));
            const float z = _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2)));
            const float w = _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)));
            return ((x + y) + z) + w;
        }

        inline __m128 Negate(__m128 v, __m128 signs) { return _mm_xor_ps(v, signs); }

        // Multiply(a, b) term by term:
        //   x = ((aw*bx + ax*bw) + ay*bz) - az*by
        //   y = ((aw*by - ax*bz) + ay*bw) + az*bx
        //   z = ((aw*bz + ax*by) - ay*bx) + az*bw
        //   w = ((aw*bw - ax*bx) - ay*by) - az*bz
        // a subtraction is the addition of the exactly negated product.
        inline __m128 MultiplySimd(__m128 a, __m128 b)
        {
            const __m128 minus = _mm_set1_ps(-0.0f);
            const __m128 aw = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 3, 3, 3));
            const __m128 ax = _mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 ay = _mm_shuffle_ps(a, a, _MM_SHUFFLE(1, 1, 1, 1));
            const __m128 az = _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 2, 2, 2));
            const __m128 t1 = _mm_mul_ps(aw, b);
            const __m128 t2 = _mm_mul_ps(ax, _mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 1, 2, 3))); // bw bz by bx
            const __m128 t3 = _mm_mul_ps(ay, _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 0, 3, 2))); // bz bw bx by
            const __m128 t4 = _mm_mul_ps(az, _mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 3, 0, 1))); // by bx bw bz
            const __m128 s2 = _mm_and_ps(minus, _mm_castsi128_ps(_mm_set_epi32(-1, 0, -1, 0)));  // + - + -
            const __m128 s3 = _mm_and_ps(minus, _mm_castsi128_ps(_mm_set_epi32(-1, -1, 0, 0)));  // + + - -
            const __m128 s4 = _mm_and_ps(minus, _mm_castsi128_ps(_mm_set_epi32(-1, 0, 0, -1)));  // - + + -
            return _mm_add_ps(_mm_add_ps(_mm_add_ps(t1, Negate(t2, s2)), Negate(t3, s3)), Negate(t4, s4));
        }

        // Normalize: sqrt of the in-order sum of squares; identity below 1e-12.
        inline __m128 NormalizeSimd(__m128 q)
        {
            const float length = std::sqrt(SumInOrder(_mm_mul_ps(q, q)));
            if (!(length > 1e-12f))
                return _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
            const float inverse = 1.0f / length;
            return _mm_mul_ps(q, _mm_set1_ps(inverse));
        }

        inline __m128 ConjugateSimd(__m128 q)
        {
            return _mm_xor_ps(q, _mm_castsi128_ps(_mm_set_epi32(0, static_cast<int>(0x80000000u), static_cast<int>(0x80000000u), static_cast<int>(0x80000000u))));
        }
    }

    void InterpolateSimd(const BoneQts& a, const BoneQts& b, float t, BoneQts& out)
    {
        const __m128 qa = _mm_loadu_ps(&a.rotation.x);
        __m128 qb = _mm_loadu_ps(&b.rotation.x);
        float dot = SumInOrder(_mm_mul_ps(qa, qb));
        if (dot < 0.0f)
        {
            qb = _mm_xor_ps(qb, _mm_set1_ps(-0.0f));
            dot = -dot;
        }
        if (!(dot > 0.9995f))
        {
            out = Interpolate(a, b, t);
            return;
        }
        const __m128 vt = _mm_set1_ps(t);
        const __m128 rotation = NormalizeSimd(_mm_add_ps(qa, _mm_mul_ps(_mm_sub_ps(qb, qa), vt)));
        const __m128 ta = _mm_loadu_ps(a.translation);
        const __m128 tb = _mm_loadu_ps(b.translation);
        __m128 translation = _mm_add_ps(ta, _mm_mul_ps(_mm_sub_ps(tb, ta), vt));
        const __m128 sa = _mm_loadu_ps(a.scale);
        const __m128 sb = _mm_loadu_ps(b.scale);
        const __m128 scale = _mm_add_ps(sa, _mm_mul_ps(_mm_sub_ps(sb, sa), vt));
        // The opaque w is taken whole from the nearer end.
        const float w = t < 0.5f ? a.translation[3] : b.translation[3];
        _mm_storeu_ps(&out.rotation.x, rotation);
        _mm_storeu_ps(out.translation, translation);
        out.translation[3] = w;
        _mm_storeu_ps(out.scale, scale);
    }

    void ComposeSimd(
        const BoneQts& parentObject,
        const BoneQts& local,
        CompositionOrder order,
        bool inheritScale,
        BoneQts& out)
    {
        const __m128 parentRotation = _mm_loadu_ps(&parentObject.rotation.x);
        const __m128 localRotation = _mm_loadu_ps(&local.rotation.x);
        const __m128 parentScale = _mm_loadu_ps(parentObject.scale);
        __m128 scale = _mm_loadu_ps(local.scale);
        // The local translation as a pure quaternion (w = 0).
        __m128 translation = _mm_and_ps(_mm_loadu_ps(local.translation),
            _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
        const float opaqueW = local.translation[3];
        if (inheritScale)
        {
            // x, y, z times the parent scale; lane 3 stays (0 for the
            // translation, the local's own scale w).
            const __m128 xyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
            translation = _mm_mul_ps(translation, _mm_or_ps(_mm_and_ps(xyz, parentScale),
                _mm_andnot_ps(xyz, _mm_set1_ps(1.0f))));
            scale = _mm_or_ps(_mm_and_ps(xyz, _mm_mul_ps(parentScale, scale)), _mm_andnot_ps(xyz, scale));
        }
        __m128 rotation;
        __m128 offset;
        if (order == CompositionOrder::ParentThenLocal)
        {
            rotation = NormalizeSimd(MultiplySimd(parentRotation, localRotation));
            offset = MultiplySimd(MultiplySimd(parentRotation, translation), ConjugateSimd(parentRotation));
        }
        else
        {
            const __m128 inverse = ConjugateSimd(parentRotation);
            rotation = NormalizeSimd(MultiplySimd(localRotation, parentRotation));
            offset = MultiplySimd(MultiplySimd(inverse, translation), ConjugateSimd(inverse));
        }
        const __m128 parentTranslation = _mm_loadu_ps(parentObject.translation);
        const __m128 sum = _mm_add_ps(parentTranslation, offset);
        _mm_storeu_ps(&out.rotation.x, rotation);
        _mm_storeu_ps(out.translation, sum);
        out.translation[3] = opaqueW;
        _mm_storeu_ps(out.scale, scale);
    }
}
