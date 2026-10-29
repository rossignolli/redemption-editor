#include "camera_track.h"

#include "camera_bones.h"
#include "camera_shake.h"
#include "camera_shake_custom.h"
#include "camera_marker_shake.h"
#include "camera_spline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegreesToRadians = kPi / 180.0f;
    constexpr float kRadiansToDegrees = 180.0f / kPi;
    constexpr std::uint64_t kReplaceToleranceMs = 1;

    float Clamp01(float value)
    {
        return std::max(0.0f, std::min(1.0f, value));
    }

    float ApplyEasing(replay::editor::Easing easing, float value)
    {
        const float t = Clamp01(value);
        switch (easing)
        {
        case replay::editor::Easing::SmoothStep:
            return t * t * (3.0f - 2.0f * t);
        case replay::editor::Easing::EaseIn:
            return t * t;
        case replay::editor::Easing::EaseOut:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case replay::editor::Easing::Hold:
            return t >= 1.0f ? 1.0f : 0.0f;
        case replay::editor::Easing::Linear:
        default:
            return t;
        }
    }

    replay::camera::Quaternion Multiply(
        const replay::camera::Quaternion& a,
        const replay::camera::Quaternion& b)
    {
        return {
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w
        };
    }

    replay::camera::Quaternion Normalize(replay::camera::Quaternion value)
    {
        const float length = std::sqrt(
            value.w * value.w + value.x * value.x +
            value.y * value.y + value.z * value.z);
        if (length <= 0.000001f)
            return {1.0f, 0.0f, 0.0f, 0.0f};
        value.w /= length;
        value.x /= length;
        value.y /= length;
        value.z /= length;
        return value;
    }

    replay::camera::Quaternion Slerp(
        replay::camera::Quaternion a,
        replay::camera::Quaternion b,
        float amount)
    {
        float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (dot < 0.0f)
        {
            b.w = -b.w;
            b.x = -b.x;
            b.y = -b.y;
            b.z = -b.z;
            dot = -dot;
        }

        if (dot > 0.9995f)
        {
            return Normalize({
                a.w + (b.w - a.w) * amount,
                a.x + (b.x - a.x) * amount,
                a.y + (b.y - a.y) * amount,
                a.z + (b.z - a.z) * amount
            });
        }

        dot = std::max(-1.0f, std::min(1.0f, dot));
        const float angle = std::acos(dot);
        const float divisor = std::sin(angle);
        if (std::fabs(divisor) < 0.000001f)
            return a;
        const float aWeight = std::sin((1.0f - amount) * angle) / divisor;
        const float bWeight = std::sin(amount * angle) / divisor;
        return Normalize({
            a.w * aWeight + b.w * bWeight,
            a.x * aWeight + b.x * bWeight,
            a.y * aWeight + b.y * bWeight,
            a.z * aWeight + b.z * bWeight
        });
    }

    float Lerp(float from, float to, float amount)
    {
        return from + (to - from) * amount;
    }

    float Hermite(
        float p0, float p1, float p2, float p3,
        double t0, double t1, double t2, double t3,
        double queryTime)
    {
        const double segmentDuration = t2 - t1;
        if (segmentDuration <= 0.0001)
            return p1;

        const double outerDuration1 = t2 - t0;
        const double outerDuration2 = t3 - t1;
        const double tangent1 = outerDuration1 > 0.0001 ? (p2 - p0) / outerDuration1 : 0.0;
        const double tangent2 = outerDuration2 > 0.0001 ? (p3 - p1) / outerDuration2 : 0.0;
        const double u = std::max(0.0, std::min(1.0, (queryTime - t1) / segmentDuration));
        const double u2 = u * u;
        const double u3 = u2 * u;
        const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
        const double h10 = u3 - 2.0 * u2 + u;
        const double h01 = -2.0 * u3 + 3.0 * u2;
        const double h11 = u3 - u2;
        return static_cast<float>(
            h00 * p1 + h10 * segmentDuration * tangent1 +
            h01 * p2 + h11 * segmentDuration * tangent2);
    }

    // Defined with the marker model below.
    void CameraFromLegacy(replay::camera::Keyframe& k);
}
namespace replay::camera
{
    std::uint32_t Track::AddOrReplace(
        std::uint64_t timeMs,
        const editor::CameraState& camera,
        editor::Easing easing,
        editor::Path path)
    {
        // On a marker track the old API stores the pose only (an old call
        // re-deriving blends from the legacy easing would turn the whole clip
        // into a converted v2 one).
        const bool model = ModelTrack();
        for (Keyframe& keyframe : keyframes_)
        {
            const std::uint64_t delta = keyframe.timeMs > timeMs
                ? keyframe.timeMs - timeMs
                : timeMs - keyframe.timeMs;
            if (delta <= kReplaceToleranceMs)
            {
                keyframe.pose = {camera.position, FromEulerDegrees(camera.rotation), camera.fov};
                keyframe.easing = easing;
                keyframe.path = path;
                const std::uint32_t replaced = keyframe.id;
                if (model)
                    return replaced;
                const std::size_t index = IndexOf(replaced);
                SyncFromLegacy(index, false);
                if (index > 0)
                    SyncFromLegacy(index - 1, false);
                legacyBlendAnyMode_ = true;
                return replaced;
            }
        }

        const std::uint32_t id = nextId_++;
        Keyframe added;
        added.id = id;
        added.timeMs = timeMs;
        added.pose = {camera.position, FromEulerDegrees(camera.rotation), camera.fov};
        added.easing = easing;
        added.path = path;
        if (model)
        {
            CameraFromLegacy(added);
            keyframes_.push_back(added);
            Sort();
            return id;
        }
        keyframes_.push_back(added);
        Sort();
        const std::size_t index = IndexOf(id);
        SyncFromLegacy(index, true);
        if (index > 0)
            SyncFromLegacy(index - 1, false);
        legacyBlendAnyMode_ = true;
        return id;
    }

    bool Track::Remove(std::uint32_t id)
    {
        if (ModelTrack())
            return RemoveMarker(id);
        const auto iterator = std::find_if(
            keyframes_.begin(), keyframes_.end(),
            [id](const Keyframe& keyframe) { return keyframe.id == id; });
        if (iterator == keyframes_.end())
            return false;
        const std::size_t index = static_cast<std::size_t>(iterator - keyframes_.begin());
        keyframes_.erase(iterator);
        if (index > 0)
            SyncFromLegacy(index - 1, false);
        legacyBlendAnyMode_ = true;
        return true;
    }

    bool Track::Move(std::uint32_t id, std::uint64_t timeMs)
    {
        if (ModelTrack())
            return MoveMarker(id, timeMs);
        const auto iterator = std::find_if(
            keyframes_.begin(), keyframes_.end(),
            [id](const Keyframe& keyframe) { return keyframe.id == id; });
        if (iterator == keyframes_.end())
            return false;
        const std::uint32_t before = iterator != keyframes_.begin() ? (iterator - 1)->id : 0;
        iterator->timeMs = timeMs;
        Sort();
        if (before)
            SyncFromLegacy(IndexOf(before), false);
        const std::size_t index = IndexOf(id);
        SyncFromLegacy(index, false);
        if (index > 0)
            SyncFromLegacy(index - 1, false);
        legacyBlendAnyMode_ = true;
        return true;
    }

    bool Track::Update(
        std::uint32_t id,
        const editor::CameraState& camera,
        editor::Easing easing,
        editor::Path path)
    {
        const auto iterator = std::find_if(
            keyframes_.begin(), keyframes_.end(),
            [id](const Keyframe& keyframe) { return keyframe.id == id; });
        if (iterator == keyframes_.end())
            return false;
        iterator->pose = {camera.position, FromEulerDegrees(camera.rotation), camera.fov};
        iterator->easing = easing;
        iterator->path = path;
        if (ModelTrack())
            return true;
        const std::size_t index = static_cast<std::size_t>(iterator - keyframes_.begin());
        SyncFromLegacy(index, false);
        if (index > 0)
            SyncFromLegacy(index - 1, false);
        legacyBlendAnyMode_ = true;
        return true;
    }

    bool Track::SetMode(std::uint32_t id, editor::CameraMode mode, std::uint32_t target, const editor::Vec3& offset)
    {
        for (Keyframe& keyframe : keyframes_)
        {
            if (keyframe.id != id)
                continue;
            keyframe.mode = mode;
            keyframe.target = target;
            keyframe.offset = offset;
            if (ModelTrack())
            {
                // Target becomes Free + Look At, Follow Free + Attach and
                // Look At; the blends stay.
                CameraFromLegacy(keyframe);
                return true;
            }
            SyncFromLegacy(IndexOf(id), true);
            legacyBlendAnyMode_ = true;
            return true;
        }
        return false;
    }

    const Keyframe* Track::Find(std::uint32_t id) const
    {
        for (const Keyframe& keyframe : keyframes_)
        {
            if (keyframe.id == id)
                return &keyframe;
        }
        return nullptr;
    }

    bool Track::Evaluate(std::uint64_t timeMs, editor::CameraState& camera, const Resolver& resolver) const
    {
        if (keyframes_.empty())
            return false;

        const auto poseOf = [&](const Keyframe& keyframe) {
            Pose pose = keyframe.pose;
            if (keyframe.mode != editor::CameraMode::Free && resolver)
                resolver(keyframe, timeMs, pose);
            return pose;
        };

        if (keyframes_.size() == 1 || timeMs <= keyframes_.front().timeMs)
        {
            const Pose first = poseOf(keyframes_.front());
            camera = {first.position, ToEulerDegrees(first.orientation), first.fov};
            return true;
        }
        if (timeMs >= keyframes_.back().timeMs)
        {
            const Pose last = poseOf(keyframes_.back());
            camera = {last.position, ToEulerDegrees(last.orientation), last.fov};
            return true;
        }

        const auto right = std::upper_bound(
            keyframes_.begin(), keyframes_.end(), timeMs,
            [](std::uint64_t value, const Keyframe& keyframe) {
                return value < keyframe.timeMs;
            });
        const std::size_t rightIndex = static_cast<std::size_t>(right - keyframes_.begin());
        const std::size_t leftIndex = rightIndex - 1;
        const Keyframe& left = keyframes_[leftIndex];
        const Keyframe& rightKeyframe = keyframes_[rightIndex];
        const Pose leftPose = poseOf(left);
        const Pose rightPose = poseOf(rightKeyframe);
        const double duration = static_cast<double>(rightKeyframe.timeMs - left.timeMs);
        const float normalized = duration > 0.0
            ? static_cast<float>((timeMs - left.timeMs) / duration)
            : 0.0f;
        const float eased = ApplyEasing(rightKeyframe.easing, normalized);

        editor::Vec3 position{};
        if (rightKeyframe.path == editor::Path::Smooth)
        {
            const Keyframe& previous = keyframes_[leftIndex > 0 ? leftIndex - 1 : leftIndex];
            const Keyframe& next = keyframes_[rightIndex + 1 < keyframes_.size()
                ? rightIndex + 1
                : rightIndex];
            const Pose previousPose = poseOf(previous);
            const Pose nextPose = poseOf(next);
            const double easedTime = left.timeMs + eased * duration;
            const auto axis = [&](float editor::Vec3::*member) {
                return Hermite(
                    previousPose.position.*member, leftPose.position.*member,
                    rightPose.position.*member, nextPose.position.*member,
                    static_cast<double>(previous.timeMs), static_cast<double>(left.timeMs),
                    static_cast<double>(rightKeyframe.timeMs), static_cast<double>(next.timeMs),
                    easedTime);
            };
            position = {axis(&editor::Vec3::x), axis(&editor::Vec3::y), axis(&editor::Vec3::z)};
        }
        else
        {
            position = {
                Lerp(leftPose.position.x, rightPose.position.x, eased),
                Lerp(leftPose.position.y, rightPose.position.y, eased),
                Lerp(leftPose.position.z, rightPose.position.z, eased)
            };
        }

        const Quaternion orientation = Slerp(leftPose.orientation, rightPose.orientation, eased);
        camera = {
            position,
            ToEulerDegrees(orientation),
            Lerp(leftPose.fov, rightPose.fov, eased)
        };
        return true;
    }

    const std::vector<Keyframe>& Track::Keyframes() const noexcept
    {
        return keyframes_;
    }

    bool Track::Empty() const noexcept
    {
        return keyframes_.empty();
    }

    void Track::Clear()
    {
        effects_.Clear();
        migration_ = {};
        keyframes_.clear();
        nextId_ = 1;
        legacyBlendAnyMode_ = false;
    }

    Quaternion Track::FromEulerDegrees(const editor::Vec3& rotation)
    {
        const float halfYaw = -rotation.z * kDegreesToRadians * 0.5f;
        const float halfPitch = rotation.x * kDegreesToRadians * 0.5f;
        const float halfRoll = rotation.y * kDegreesToRadians * 0.5f;
        const Quaternion yaw{std::cos(halfYaw), 0.0f, 0.0f, std::sin(halfYaw)};
        const Quaternion pitch{std::cos(halfPitch), std::sin(halfPitch), 0.0f, 0.0f};
        const Quaternion roll{std::cos(halfRoll), 0.0f, std::sin(halfRoll), 0.0f};
        return Normalize(Multiply(Multiply(yaw, pitch), roll));
    }

    editor::Vec3 Track::ToEulerDegrees(const Quaternion& source)
    {
        const Quaternion orientation = Normalize(source);
        const float m12 = 2.0f * (orientation.x * orientation.y - orientation.z * orientation.w);
        const float m22 = 1.0f - 2.0f * (orientation.x * orientation.x + orientation.z * orientation.z);
        const float m31 = 2.0f * (orientation.x * orientation.z - orientation.y * orientation.w);
        const float m32 = 2.0f * (orientation.y * orientation.z + orientation.x * orientation.w);
        const float m33 = 1.0f - 2.0f * (orientation.x * orientation.x + orientation.y * orientation.y);

        editor::Vec3 result{};
        result.x = std::asin(std::max(-1.0f, std::min(1.0f, m32))) * kRadiansToDegrees;
        if (std::fabs(m32) < 0.999f)
        {
            // FromEulerDegrees converts RAGE's clockwise Z rotation to the
            // quaternion's counter-clockwise convention. Convert it back here
            // or captured keyframes reappear mirrored around the yaw axis.
            result.z = -std::atan2(-m12, m22) * kRadiansToDegrees;
            result.y = std::atan2(-m31, m33) * kRadiansToDegrees;
        }
        return result;
    }

    void Track::Sort()
    {
        std::stable_sort(
            keyframes_.begin(), keyframes_.end(),
            [](const Keyframe& left, const Keyframe& right) {
                if (left.timeMs != right.timeMs)
                    return left.timeMs < right.timeMs;
                return left.id < right.id;
            });
    }

    bool EvaluateGame(const std::vector<GameSample>& samples, std::uint64_t timeMs, editor::CameraState& camera,
        const std::vector<std::uint32_t>* cuts)
    {
        if (samples.empty())
            return false;
        const auto right = std::upper_bound(
            samples.begin(), samples.end(), timeMs,
            [](std::uint64_t value, const GameSample& sample) { return value < sample.timeMs; });
        if (right == samples.begin() || right == samples.end())
        {
            const GameSample& end = right == samples.begin() ? samples.front() : samples.back();
            camera = {end.position, end.rotation, end.fov};
            return true;
        }
        const GameSample& a = *(right - 1);
        const GameSample& b = *right;
        float t = b.timeMs > a.timeMs
            ? static_cast<float>(static_cast<double>(timeMs - a.timeMs) / static_cast<double>(b.timeMs - a.timeMs))
            : 0.0f;
        // A camera cut between the two samples (the kill cam's
        // cut in and out): the earlier shot holds until the later sample.
        if (IsGameCameraCut(a, b))
            t = 0.0f;
        // Across a recording cut, the side the playhead is on.
        if (cuts && !cuts->empty())
        {
            std::uint32_t cut = 0;
            for (const std::uint32_t c : *cuts)
            {
                if (c > b.timeMs)
                    break;
                if (c > a.timeMs)
                {
                    cut = c;
                    break;
                }
            }
            if (cut)
                t = timeMs < cut ? 0.0f : 1.0f;
        }
        const auto lerp = [t](float from, float to) { return from + (to - from) * t; };
        const auto angle = [t](float from, float to) {
            float delta = std::fmod(to - from, 360.0f);
            if (delta > 180.0f) delta -= 360.0f;
            if (delta < -180.0f) delta += 360.0f;
            return from + delta * t;
        };
        camera.position = {lerp(a.position.x, b.position.x), lerp(a.position.y, b.position.y), lerp(a.position.z, b.position.z)};
        camera.rotation = {angle(a.rotation.x, b.rotation.x), angle(a.rotation.y, b.rotation.y), angle(a.rotation.z, b.rotation.z)};
        camera.fov = lerp(a.fov, b.fov);
        return true;
    }

    bool IsGameCameraCut(const GameSample& a, const GameSample& b)
    {
        if (b.timeMs < a.timeMs || b.timeMs - a.timeMs > kGameCutMaxGapMs)
            return false;
        const float dx = b.position.x - a.position.x;
        const float dy = b.position.y - a.position.y;
        const float dz = b.position.z - a.position.z;
        if (dx * dx + dy * dy + dz * dz > kGameCutMeters * kGameCutMeters)
            return true;
        const auto turn = [](float from, float to) {
            float delta = std::fmod(to - from, 360.0f);
            if (delta > 180.0f) delta -= 360.0f;
            if (delta < -180.0f) delta += 360.0f;
            return std::fabs(delta);
        };
        if (turn(a.rotation.x, b.rotation.x) > kGameCutDegrees || turn(a.rotation.y, b.rotation.y) > kGameCutDegrees ||
            turn(a.rotation.z, b.rotation.z) > kGameCutDegrees)
            return true;
        return std::fabs(b.fov - a.fov) > kGameCutFov;
    }

    std::vector<TimeSpan> CinematicSlowSpans(const std::vector<GameSample>& samples, const timescale::Track& scale)
    {
        std::vector<TimeSpan> spans;
        const std::vector<timescale::Sample>& changes = scale.Samples();
        // The lowest recorded scale in [fromMs, toMs].
        const auto lowest = [&](std::uint32_t fromMs, std::uint32_t toMs) {
            float low = scale.At(fromMs);
            for (const timescale::Sample& change : changes)
            {
                if (change.timeMs > toMs)
                    break;
                if (change.timeMs >= fromMs && change.scale < low)
                    low = change.scale;
            }
            return low;
        };
        bool open = false;
        TimeSpan span;
        for (std::size_t index = 1; index < samples.size(); ++index)
        {
            const GameSample& a = samples[index - 1];
            const GameSample& b = samples[index];
            if (open)
            {
                // Closed by a cut back out with the scale recovered, or by
                // the scale alone (a slow motion that ends without a cut).
                const bool recovered = scale.At(b.timeMs) >= kCinematicScale;
                if (recovered && (IsGameCameraCut(a, b) || scale.At(b.timeMs) >= 0.95f))
                {
                    span.endMs = b.timeMs;
                    spans.push_back(span);
                    open = false;
                }
                continue;
            }
            if (!IsGameCameraCut(a, b))
                continue;
            const std::uint32_t from = a.timeMs;
            if (lowest(from, b.timeMs + kCinematicScaleWindowMs) < kCinematicScale)
            {
                span.startMs = from;
                open = true;
            }
        }
        if (open)
        {
            span.endMs = samples.back().timeMs;
            spans.push_back(span);
        }
        return spans;
    }

    const TimeSpan* SpanAt(const std::vector<TimeSpan>& spans, std::uint64_t timeMs)
    {
        for (const TimeSpan& span : spans)
            if (timeMs >= span.startMs && timeMs <= span.endMs)
                return &span;
        return nullptr;
    }

    editor::Vec3 LookAtDegrees(const editor::Vec3& from, const editor::Vec3& to)
    {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float horizontal = std::sqrt(dx * dx + dy * dy);
        // Inverse of the direction a rotation gives: (-sin yaw, cos yaw) * cos pitch, sin pitch.
        return {std::atan2(dz, horizontal) * kRadiansToDegrees, 0.0f, std::atan2(-dx, dy) * kRadiansToDegrees};
    }

    editor::Vec3 ToHeadingFrame(const editor::Vec3& world, float headingDegrees)
    {
        const float h = headingDegrees * kDegreesToRadians;
        const float c = std::cos(h);
        const float s = std::sin(h);
        return {world.x * c + world.y * s, -world.x * s + world.y * c, world.z};
    }

    editor::Vec3 FromHeadingFrame(const editor::Vec3& local, float headingDegrees)
    {
        const float h = headingDegrees * kDegreesToRadians;
        const float c = std::cos(h);
        const float s = std::sin(h);
        return {local.x * c - local.y * s, local.x * s + local.y * c, local.z};
    }
}

// ----------------------------------------------------------------------
// Marker model: rules, conversion and the evaluator (API 8, CAMR v3).

namespace
{
    using replay::editor::Vec2;
    using replay::editor::Vec3;

    // Row major 3x3 rotation. Columns are the camera's right (+X), forward
    // (+Y) and up (+Z) in world space; R = Rz(heading) Rx(pitch) Ry(roll),
    // the same order as the game's rotation order 2.
    struct Mat3
    {
        float m[3][3];
    };

    Mat3 Identity()
    {
        return {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    }

    Mat3 FromEuler(const Vec3& rotation)
    {
        const float p = rotation.x * kDegreesToRadians;
        const float r = rotation.y * kDegreesToRadians;
        const float h = rotation.z * kDegreesToRadians;
        const float cp = std::cos(p), sp = std::sin(p);
        const float cr = std::cos(r), sr = std::sin(r);
        const float ch = std::cos(h), sh = std::sin(h);
        return {{
            {ch * cr - sh * sp * sr, -sh * cp, ch * sr + sh * sp * cr},
            {sh * cr + ch * sp * sr, ch * cp, sh * sr - ch * sp * cr},
            {-cp * sr, sp, cp * cr}
        }};
    }

    float WrapDegrees(float angle)
    {
        float wrapped = std::fmod(angle + 180.0f, 360.0f);
        if (wrapped < 0.0f)
            wrapped += 360.0f;
        return wrapped - 180.0f;
    }

    Vec3 ToEuler(const Mat3& r)
    {
        const float s = std::max(-1.0f, std::min(1.0f, r.m[2][1]));
        Vec3 out{};
        out.x = std::asin(s) * kRadiansToDegrees;
        if (std::fabs(s) < 0.99999f)
        {
            out.z = std::atan2(-r.m[0][1], r.m[1][1]) * kRadiansToDegrees;
            out.y = std::atan2(-r.m[2][0], r.m[2][2]) * kRadiansToDegrees;
        }
        else
        {
            out.z = std::atan2(r.m[1][0], r.m[0][0]) * kRadiansToDegrees;
            out.y = 0.0f;
        }
        return out;
    }

    Mat3 Mul(const Mat3& a, const Mat3& b)
    {
        Mat3 out{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        return out;
    }

    Mat3 Transpose(const Mat3& a)
    {
        Mat3 out{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out.m[i][j] = a.m[j][i];
        return out;
    }

    Vec3 Mul(const Mat3& a, const Vec3& v)
    {
        return {
            a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z
        };
    }

    Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    Vec3 LerpVec(const Vec3& a, const Vec3& b, float t)
    {
        return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
    }
    float LerpAngle(float a, float b, float t) { return WrapDegrees(a + WrapDegrees(b - a) * t); }

    // The frame an attach target lends the camera for an attach type.
    Mat3 AttachFrame(replay::camera::AttachType type, const replay::camera::EntityState& state)
    {
        switch (type)
        {
        case replay::camera::AttachType::Rigid:
            return FromEuler({state.pitch, state.roll, state.heading});
        case replay::camera::AttachType::Default:
            return FromEuler({0.0f, 0.0f, state.heading});
        case replay::camera::AttachType::Flexible:
        default:
            return Identity();
        }
    }

    // A primed spring's state (in `from`, origin fromOrigin) re-expressed in
    // `to` (origin toOrigin): the same world pose and the same world velocity.
    // The angular rates are kept (Euler rates; the two frames are a target's
    // heading apart at most).
    void ReframeSpring(replay::camera::SpringState& spring, const Mat3& from, const Vec3& fromOrigin, const Mat3& to,
        const Vec3& toOrigin);

    Vec3 AimPoint(const replay::camera::EntityState& state)
    {
        if (state.hasHead)
            return state.head;
        return {state.position.x, state.position.y, state.position.z + replay::camera::kLookAtAimHeight};
    }

    void ReframeSpring(replay::camera::SpringState& spring, const Mat3& from, const Vec3& fromOrigin, const Mat3& to,
        const Vec3& toOrigin)
    {
        const Mat3 toInverse = Transpose(to);
        const Mat3 change = Mul(toInverse, from);
        const Vec3 world = Add(fromOrigin, Mul(from, Vec3{spring.value[0], spring.value[1], spring.value[2]}));
        const Vec3 position = Mul(toInverse, Sub(world, toOrigin));
        const Vec3 velocity = Mul(change, Vec3{spring.velocity[0], spring.velocity[1], spring.velocity[2]});
        const Vec3 rotation = ToEuler(Mul(change, FromEuler({spring.value[3], spring.value[4], spring.value[5]})));
        spring.value[0] = position.x;
        spring.value[1] = position.y;
        spring.value[2] = position.z;
        spring.value[3] = rotation.x;
        spring.value[4] = rotation.y;
        spring.value[5] = rotation.z;
        spring.velocity[0] = velocity.x;
        spring.velocity[1] = velocity.y;
        spring.velocity[2] = velocity.z;
    }

    bool IsPreset(replay::camera::CameraMode mode)
    {
        return mode >= replay::camera::CameraMode::FrontLow && mode <= replay::camera::CameraMode::Overhead;
    }

    bool IsModelMode(replay::camera::CameraMode mode)
    {
        return mode == replay::camera::CameraMode::Free || mode == replay::camera::CameraMode::Game || IsPreset(mode);
    }

    bool Editable(const replay::camera::Keyframe& marker)
    {
        return marker.cameraMode == replay::camera::CameraMode::Free || IsPreset(marker.cameraMode);
    }

    // Legacy view to model: the camera fields of one keyframe.
    void CameraFromLegacy(replay::camera::Keyframe& k)
    {
        using replay::camera::CameraMode;
        switch (k.mode)
        {
        case CameraMode::Game:
            k.cameraMode = CameraMode::Game;
            k.lookAtTarget = 0;
            k.attachTarget = 0;
            break;
        case CameraMode::Target:
            k.cameraMode = CameraMode::Free;
            k.lookAtTarget = k.target;
            k.attachTarget = 0;
            break;
        case CameraMode::Follow:
            k.cameraMode = CameraMode::Free;
            k.attachTarget = k.target;
            k.attachType = replay::camera::AttachType::Default;
            k.relPosition = k.offset;
            k.relRotation = {};
            k.lookAtTarget = k.target;
            break;
        case CameraMode::Free:
        default:
            k.cameraMode = CameraMode::Free;
            k.lookAtTarget = 0;
            k.attachTarget = 0;
            break;
        }
    }

    // Legacy view to model: the blend of keyframe i comes from the easing
    // and path of keyframe i + 1 (the one arriving). The last keyframe has
    // no blend; its ease is its own arriving easing (does the end brake).
    void BlendFromLegacy(std::vector<replay::camera::Keyframe>& keys, std::size_t i)
    {
        using replay::editor::Easing;
        replay::camera::Keyframe& k = keys[i];
        if (i + 1 >= keys.size())
        {
            k.blend = replay::camera::Blend::None;
            k.ease = k.easing != Easing::Linear && k.easing != Easing::Hold;
            k.legacyCurve = false;
            return;
        }
        const replay::camera::Keyframe& arriving = keys[i + 1];
        switch (arriving.easing)
        {
        case Easing::Hold:
            k.blend = replay::camera::Blend::None;
            k.ease = true;
            break;
        case Easing::Linear:
            k.blend = replay::camera::Blend::Linear;
            k.ease = false;
            break;
        default:
            k.blend = replay::camera::Blend::Linear;
            k.ease = true;
            break;
        }
        k.legacyCurve = k.blend != replay::camera::Blend::None && arriving.path == replay::editor::Path::Smooth;
    }
}

namespace replay::camera
{
    std::size_t Track::IndexOf(std::uint32_t id) const
    {
        for (std::size_t index = 0; index < keyframes_.size(); ++index)
        {
            if (keyframes_[index].id == id)
                return index;
        }
        return keyframes_.size();
    }

    void Track::SyncFromLegacy(std::size_t index, bool cameraFields)
    {
        if (index >= keyframes_.size())
            return;
        if (cameraFields)
            CameraFromLegacy(keyframes_[index]);
        BlendFromLegacy(keyframes_, index);
    }

    void ConvertV2ToV3(std::vector<Keyframe>& keyframes)
    {
        for (Keyframe& keyframe : keyframes)
            CameraFromLegacy(keyframe);
        for (std::size_t index = 0; index < keyframes.size(); ++index)
            BlendFromLegacy(keyframes, index);
    }

    void Track::ConvertLegacy()
    {
        ConvertV2ToV3(keyframes_);
        legacyBlendAnyMode_ = true;
    }

    void Track::DeriveLegacy()
    {
        for (std::size_t index = 0; index < keyframes_.size(); ++index)
        {
            Keyframe& k = keyframes_[index];
            if (k.cameraMode == CameraMode::Game)
            {
                k.mode = CameraMode::Game;
                k.target = 0;
            }
            else if (k.cameraMode == CameraMode::Free && k.attachTarget != 0)
            {
                // Follow looks at its target; an attach without Look At has
                // no exact legacy form, the position still follows.
                k.mode = CameraMode::Follow;
                k.target = k.attachTarget;
                k.offset = k.relPosition;
            }
            else if (k.cameraMode == CameraMode::Free && k.lookAtTarget != 0)
            {
                k.mode = CameraMode::Target;
                k.target = k.lookAtTarget;
            }
            else
            {
                k.mode = CameraMode::Free; // presets show their stored pose in the old core
                k.target = 0;
            }

            if (index == 0)
            {
                k.easing = editor::Easing::Linear;
                k.path = editor::Path::Linear;
                continue;
            }
            const Keyframe& source = keyframes_[index - 1];
            if (source.blend == Blend::None)
                k.easing = editor::Easing::Hold;
            else if (source.blend == Blend::Linear && !source.ease)
                k.easing = editor::Easing::Linear;
            else
                k.easing = editor::Easing::SmoothStep;
            k.path = source.legacyCurve || source.blend == Blend::Spline ? editor::Path::Smooth : editor::Path::Linear;
        }
    }

    bool Track::LegacyBlendAnyMode() const noexcept
    {
        return legacyBlendAnyMode_;
    }

    void Track::SetLegacyBlendAnyMode(bool value) noexcept
    {
        legacyBlendAnyMode_ = value;
    }

    // ------------------------------------------------------------------
    // Marker API.

    float SpeedFactor(std::uint8_t speedIndex)
    {
        const std::uint32_t index = std::min<std::uint32_t>(speedIndex, editor::kMarkerSpeedCount - 1);
        return static_cast<float>(editor::kMarkerSpeedPercent[index]) / 100.0f;
    }

    std::uint32_t MarkerSpacingMs(std::uint8_t speedIndex)
    {
        const std::uint32_t index = std::min<std::uint32_t>(speedIndex, editor::kMarkerSpeedCount - 1);
        const std::uint32_t spacing = (kMarkerFrameMs * editor::kMarkerSpeedPercent[index] + 99) / 100;
        return std::max(kMarkerMinSpacingMs, spacing);
    }

    bool CanPlace(const Track& track, std::uint64_t timeMs, std::uint32_t ignoreId)
    {
        const Keyframe* previous = nullptr;
        const Keyframe* next = nullptr;
        const Keyframe* self = nullptr;
        std::size_t count = 0;
        for (const Keyframe& keyframe : track.Keyframes())
        {
            if (ignoreId != 0 && keyframe.id == ignoreId)
            {
                self = &keyframe;
                continue;
            }
            ++count;
            if (keyframe.timeMs <= timeMs)
                previous = &keyframe;
            else if (!next)
                next = &keyframe;
        }
        if (!self && count >= kMaxMarkers)
            return false;
        if (previous && timeMs - previous->timeMs < MarkerSpacingMs(previous->speedIndex))
            return false;
        if (next)
        {
            const std::uint8_t speed = self ? self->speedIndex
                : previous ? previous->speedIndex
                : static_cast<std::uint8_t>(editor::kMarkerSpeedDefaultIndex);
            if (next->timeMs - timeMs < MarkerSpacingMs(speed))
                return false;
        }
        return true;
    }

    bool Track::HasLegacyShake() const noexcept
    {
        if (!effects_.Empty())
            return true;
        for (const Keyframe& k : keyframes_)
            if (k.legacyShake.type != ShakeType::None)
                return true;
        return false;
    }

    std::uint32_t Track::InsertMigrated(const Keyframe& marker)
    {
        if (keyframes_.size() >= kMaxMarkers)
            return 0;
        Keyframe added = marker;
        added.id = nextId_++;
        keyframes_.push_back(added);
        Sort();
        return added.id;
    }

    std::uint32_t Track::AddMarker(const Keyframe& marker)
    {
        if (!CanPlace(*this, marker.timeMs, 0))
            return 0;
        Keyframe added = marker;
        added.id = nextId_++;
        keyframes_.push_back(added);
        Sort();
        return added.id;
    }

    bool Track::RemoveMarker(std::uint32_t id)
    {
        const std::size_t index = IndexOf(id);
        if (index >= keyframes_.size() || IsClipBound(*this, id))
            return false;
        keyframes_.erase(keyframes_.begin() + static_cast<std::ptrdiff_t>(index));
        ApplyLastMarkerRule(*this);
        return true;
    }

    bool Track::MoveMarker(std::uint32_t id, std::uint64_t timeMs)
    {
        const std::size_t index = IndexOf(id);
        if (index >= keyframes_.size() || ClampMoveTime(*this, id, timeMs) != timeMs || !CanPlace(*this, timeMs, id))
            return false;
        keyframes_[index].timeMs = timeMs;
        Sort();
        ApplyLastMarkerRule(*this);
        return true;
    }

    Keyframe* Track::FindMutable(std::uint32_t id)
    {
        const std::size_t index = IndexOf(id);
        return index < keyframes_.size() ? &keyframes_[index] : nullptr;
    }

    bool Track::SetOptions(std::uint32_t id, const editor::CameraKeyframeOptions& options, std::uint64_t limitMs)
    {
        const std::vector<Keyframe> before = keyframes_; // all or nothing
        Keyframe* marker = FindMutable(id);
        if (!marker || !ApplyOptions(*marker, options))
            return false;
        if ((options.mask & editor::CameraOptionSpeed) && !KeepSpacingAfter(IndexOf(id), limitMs))
        {
            keyframes_ = before;
            return false;
        }
        ApplyLastMarkerRule(*this);
        return true;
    }

    bool Track::ModelTrack() const noexcept
    {
        return !legacyBlendAnyMode_ && !keyframes_.empty();
    }

    bool Track::KeepSpacingAfter(std::size_t index, std::uint64_t limitMs)
    {
        bool lastMoved = false;
        for (std::size_t n = index; n + 1 < keyframes_.size(); ++n)
        {
            const std::uint64_t earliest = keyframes_[n].timeMs + MarkerSpacingMs(keyframes_[n].speedIndex);
            if (keyframes_[n + 1].timeMs >= earliest)
                break;
            keyframes_[n + 1].timeMs = earliest;
            lastMoved = n + 2 == keyframes_.size();
        }
        // Duration and spacing together. The push is the smallest that keeps
        // every spacing, so a last marker pushed past the limit means no
        // placement after `index` fits in the clip (holding only the end there
        // would break the spacing just made): the caller refuses the change. A
        // last marker the push did not reach is left as it was.
        return !lastMoved || keyframes_.back().timeMs <= limitMs;
    }

    bool Track::ResetMarker(std::uint32_t id, std::uint64_t limitMs)
    {
        const std::size_t index = IndexOf(id);
        if (index >= keyframes_.size())
            return false;
        const std::vector<Keyframe> before = keyframes_; // all or nothing
        // The last marker is the last one that is not an Anchor.
        bool isLast = true;
        for (std::size_t n = index + 1; n < keyframes_.size(); ++n)
            isLast = isLast && keyframes_[n].markerType == MarkerType::Anchor;
        ResetToDefault(keyframes_[index], isLast);
        if (!KeepSpacingAfter(index, limitMs))
        {
            keyframes_ = before;
            return false;
        }
        ApplyLastMarkerRule(*this);
        return true;
    }

    bool Track::PasteMarker(std::uint32_t id, const Keyframe& from, CopyContext context, std::uint64_t limitMs)
    {
        const std::vector<Keyframe> before = keyframes_; // all or nothing
        Keyframe* marker = FindMutable(id);
        if (!marker || !Paste(*marker, from, context))
            return false;
        if (!KeepSpacingAfter(IndexOf(id), limitMs))
        {
            keyframes_ = before;
            return false;
        }
        ApplyLastMarkerRule(*this);
        return true;
    }

    const Keyframe* FirstMarkerCrossed(const Track& track, std::uint64_t fromMs, std::uint64_t toMs)
    {
        // The list is sorted by time; going back the first one crossed is the
        // latest below fromMs, not the earliest.
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (fromMs < toMs)
        {
            for (const Keyframe& key : keys)
                if (key.timeMs > fromMs && key.timeMs < toMs)
                    return &key;
        }
        else if (toMs < fromMs)
        {
            for (auto key = keys.rbegin(); key != keys.rend(); ++key)
                if (key->timeMs < fromMs && key->timeMs > toMs)
                    return &*key;
        }
        return nullptr;
    }

    bool InheritFrom(Keyframe& fresh, const Keyframe& previous)
    {
        const bool poseFromPrevious = Editable(previous) && previous.markerType != MarkerType::Anchor;
        if (poseFromPrevious)
        {
            fresh.pose = previous.pose;
            fresh.relPosition = previous.relPosition;
            fresh.relRotation = previous.relRotation;
            fresh.lookAtTarget = previous.lookAtTarget;
            fresh.lookAtOffset = previous.lookAtOffset;
            fresh.lookAtRoll = previous.lookAtRoll;
            fresh.attachTarget = previous.attachTarget;
            fresh.attachType = previous.attachType;
            fresh.attachBone = previous.attachBone;
            fresh.lookAtBone = previous.lookAtBone;
            fresh.presetDistanceScale = previous.presetDistanceScale;
        }
        fresh.nearClip = previous.nearClip; // with the camera options
        fresh.dofFStop = previous.dofFStop; // the DOF is always inherited
        fresh.dofLensMm = previous.dofLensMm;
        fresh.dof = previous.dof;
        fresh.dofIntensity = previous.dofIntensity;
        fresh.focus = previous.focus;
        fresh.focusDistance = previous.focusDistance;
        fresh.focusTarget = previous.focusTarget;
        fresh.speedIndex = previous.speedIndex;
        fresh.legacyShake = previous.legacyShake;
        fresh.markerType = previous.markerType;
        fresh.cameraMode = previous.cameraMode;
        // The shake goes on as it was (no restart of the pattern; the caller
        // may put in the blended state in force at the new time,
        // mshake::HeldAt).
        fresh.shake = mshake::Continued(previous.shake);
        fresh.origin = 0;
        fresh.spline = previous.spline;
        // Effects are always inherited; a fade belongs to the first or last
        // marker only.
        fresh.filter = previous.filter;
        fresh.filterIntensity = previous.filterIntensity;
        fresh.saturation = previous.saturation;
        fresh.contrast = previous.contrast;
        fresh.vignette = previous.vignette;
        fresh.brightness = previous.brightness;
        fresh.transition = 0;
        if (previous.cameraMode == CameraMode::Free)
        {
            fresh.blend = previous.blend;
            fresh.ease = previous.ease;
        }
        else
        {
            // Spline is the default blend (it blends only once the
            // marker is a Free camera).
            fresh.blend = Blend::Spline;
            fresh.ease = true;
        }
        fresh.legacyCurve = false;
        fresh.editedMask = 0;
        return poseFromPrevious && previous.cameraMode == CameraMode::Free;
    }

    void ResetToDefault(Keyframe& marker, bool isLast)
    {
        Keyframe fresh;
        fresh.id = marker.id;
        fresh.timeMs = marker.timeMs;
        fresh.pose = marker.pose;
        fresh.pose.fov = editor::kMarkerFovDefault;
        fresh.easing = marker.easing;
        fresh.path = marker.path;
        fresh.mode = marker.mode;
        fresh.target = marker.target;
        fresh.offset = marker.offset;
        if (isLast)
            fresh.cameraMode = marker.cameraMode;
        marker = fresh;
    }

    namespace
    {
        constexpr std::uint64_t kBorrowHashMask = 0x7FFFFFFFull << 32;

        // A 31 bit hash of the view fields the last marker rule copies.
        std::uint32_t ViewHash(const Keyframe& k)
        {
            std::uint32_t hash = 2166136261u;
            const auto mix = [&hash](const void* data, std::size_t size) {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                for (std::size_t n = 0; n < size; ++n)
                {
                    hash ^= bytes[n];
                    hash *= 16777619u;
                }
            };
            mix(&k.pose.position, sizeof(k.pose.position));
            mix(&k.pose.orientation, sizeof(k.pose.orientation));
            mix(&k.pose.fov, sizeof(k.pose.fov));
            mix(&k.lookAtTarget, sizeof(k.lookAtTarget));
            mix(&k.lookAtOffset, sizeof(k.lookAtOffset));
            mix(&k.lookAtRoll, sizeof(k.lookAtRoll));
            mix(&k.attachTarget, sizeof(k.attachTarget));
            mix(&k.attachType, sizeof(k.attachType));
            mix(&k.relPosition, sizeof(k.relPosition));
            mix(&k.relRotation, sizeof(k.relRotation));
            mix(&k.attachBone, sizeof(k.attachBone));
            mix(&k.lookAtBone, sizeof(k.lookAtBone));
            return hash & 0x7FFFFFFFu;
        }

        void ClearBorrowed(Keyframe& k)
        {
            k.editedMask &= ~(kEditedBorrowedView | kBorrowHashMask);
        }

        void MarkBorrowed(Keyframe& k)
        {
            ClearBorrowed(k);
            k.editedMask |= kEditedBorrowedView | (static_cast<std::uint64_t>(ViewHash(k)) << 32);
        }
    }

    bool HasBorrowedView(const Keyframe& marker)
    {
        return (marker.editedMask & kEditedBorrowedView) != 0 &&
            ((marker.editedMask & kBorrowHashMask) >> 32) == ViewHash(marker);
    }

    bool IsClipBound(const Track& track, std::uint32_t id)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        return !track.LegacyBlendAnyMode() && keys.size() >= 2 && id != 0 &&
            (keys.front().id == id || keys.back().id == id);
    }

    bool WithinClipBounds(const Track& track, std::uint64_t timeMs)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (track.LegacyBlendAnyMode() || keys.size() < 2)
            return true;
        return timeMs > keys.front().timeMs && timeMs < keys.back().timeMs;
    }

    bool IsLastMarker(const Track& track, std::uint32_t id)
    {
        if (track.LegacyBlendAnyMode() || id == 0)
            return false;
        const std::vector<Keyframe>& keys = track.Keyframes();
        int seen = 0;
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        {
            if (it->markerType == MarkerType::Anchor)
                continue;
            if (seen++ == 0 && it->id != id)
                return false;
            if (seen == 2)
                return true;
        }
        return false;
    }

    bool SameMarkers(const Track& a, const Track& b)
    {
        const auto v2 = [](const editor::Vec2& p, const editor::Vec2& q) { return p.x == q.x && p.y == q.y; };
        const auto v3 = [](const editor::Vec3& p, const editor::Vec3& q) { return p.x == q.x && p.y == q.y && p.z == q.z; };
        const auto same = [&](const Keyframe& p, const Keyframe& q) {
            return p.id == q.id && p.timeMs == q.timeMs && v3(p.pose.position, q.pose.position) &&
                p.pose.orientation.w == q.pose.orientation.w && p.pose.orientation.x == q.pose.orientation.x &&
                p.pose.orientation.y == q.pose.orientation.y && p.pose.orientation.z == q.pose.orientation.z &&
                p.pose.fov == q.pose.fov && p.easing == q.easing && p.path == q.path && p.mode == q.mode &&
                p.target == q.target && v3(p.offset, q.offset) && p.cameraMode == q.cameraMode &&
                p.markerType == q.markerType && p.blend == q.blend && p.ease == q.ease && p.lookAtTarget == q.lookAtTarget &&
                v2(p.lookAtOffset, q.lookAtOffset) && p.lookAtRoll == q.lookAtRoll && p.attachTarget == q.attachTarget &&
                p.attachType == q.attachType && v3(p.relPosition, q.relPosition) && v3(p.relRotation, q.relRotation) &&
                p.presetDistanceScale == q.presetDistanceScale && mshake::Same(p.shake, q.shake) && p.origin == q.origin &&
                p.legacyShake.type == q.legacyShake.type && p.legacyShake.intensity == q.legacyShake.intensity &&
                p.legacyShake.speedIndex == q.legacyShake.speedIndex && p.legacyShake.motion == q.legacyShake.motion &&
                shake::SameCustom(p.legacyShake.custom, q.legacyShake.custom) && spline::Same(p.spline, q.spline) && p.dof == q.dof && p.focus == q.focus &&
                p.dofIntensity == q.dofIntensity && p.focusDistance == q.focusDistance && p.focusTarget == q.focusTarget &&
                p.speedIndex == q.speedIndex && p.legacyCurve == q.legacyCurve && p.transition == q.transition &&
                p.transitionMs == q.transitionMs && p.filter == q.filter && p.filterIntensity == q.filterIntensity &&
                p.saturation == q.saturation && p.contrast == q.contrast && p.vignette == q.vignette &&
                p.brightness == q.brightness && p.editedMask == q.editedMask &&
                p.attachBone == q.attachBone && p.lookAtBone == q.lookAtBone && p.nearClip == q.nearClip &&
                p.dofFStop == q.dofFStop && p.dofLensMm == q.dofLensMm;
        };
        const std::vector<Keyframe>& x = a.Keyframes();
        const std::vector<Keyframe>& y = b.Keyframes();
        if (x.size() != y.size() || a.LegacyBlendAnyMode() != b.LegacyBlendAnyMode() || a.LegacyEffects() != b.LegacyEffects())
            return false;
        for (std::size_t n = 0; n < x.size(); ++n)
            if (!same(x[n], y[n]))
                return false;
        return true;
    }

    void ApplyLastMarkerRule(Track& track)
    {
        // A converted v2 clip keeps the old evaluator's rules (it
        // blends into any camera type, a Game Camera end included).
        if (track.LegacyBlendAnyMode())
            return;
        const std::vector<Keyframe>& keys = track.Keyframes();
        std::uint32_t last = 0;
        std::uint32_t penultimate = 0;
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        {
            if (it->markerType == MarkerType::Anchor)
                continue;
            if (!last)
                last = it->id;
            else
            {
                penultimate = it->id;
                break;
            }
        }
        if (!last || !penultimate)
            return;
        Keyframe* end = track.FindMutable(last);
        const Keyframe* before = track.Find(penultimate);
        // A borrowed view nobody edited follows the marker before it; an
        // edited one is the editor's own from then on.
        const bool borrowed = HasBorrowedView(*end);
        if (!borrowed)
            ClearBorrowed(*end);
        // The last marker's own blend is kept, not cleared. It has no segment
        // to blend while it is last (Plan ignores it), and clearing it would
        // lose the blend of every marker added in time order: each one is the
        // last when added. The same for its own Look At and Attach: the targets
        // of the marker before are taken only when the last has none of its
        // own.
        if (before->blend != Blend::None && before->cameraMode == CameraMode::Free)
        {
            // A last marker that was another type (the clip's end is a Game
            // Camera) has no pose of its own: its placeholder is the world
            // origin, and a blend into it would fly there. It takes the view
            // of the marker before it: the blend holds that view until the
            // last one is framed.
            const bool converted = end->cameraMode != CameraMode::Free;
            end->cameraMode = CameraMode::Free;
            const bool takesTargets = borrowed || (end->lookAtTarget == 0 && end->attachTarget == 0);
            if (takesTargets)
            {
                end->lookAtTarget = before->lookAtTarget;
                end->attachTarget = before->attachTarget;
                end->attachType = before->attachType;
                end->attachBone = before->attachBone;
                end->lookAtBone = before->lookAtBone;
            }
            if (converted || borrowed)
            {
                end->pose = before->pose;
                if (takesTargets)
                {
                    end->relPosition = before->relPosition;
                    end->relRotation = before->relRotation;
                    end->lookAtOffset = before->lookAtOffset;
                    end->lookAtRoll = before->lookAtRoll;
                }
                MarkBorrowed(*end);
            }
        }
        else if (borrowed)
        {
            // No blend into it any more: the borrowed view goes, the end is a
            // Game Camera again. Without this, deleting or un-blending the
            // marker before it would leave the end Free on that marker's old
            // view, and the next blend into the end would fly back there.
            end->cameraMode = CameraMode::Game;
            ClearBorrowed(*end);
        }
    }

    std::uint64_t ClampMoveTime(const Track& track, std::uint32_t id, std::uint64_t timeMs)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        std::size_t index = keys.size();
        for (std::size_t n = 0; n < keys.size(); ++n)
            if (keys[n].id == id)
                index = n;
        if (index >= keys.size())
            return timeMs;
        const Keyframe& self = keys[index];
        std::uint64_t low = 0;
        std::uint64_t high = UINT64_MAX;
        if (index > 0)
            low = keys[index - 1].timeMs + MarkerSpacingMs(keys[index - 1].speedIndex);
        if (index + 1 < keys.size())
        {
            const std::uint64_t spacing = MarkerSpacingMs(self.speedIndex);
            high = keys[index + 1].timeMs > spacing ? keys[index + 1].timeMs - spacing : 0;
        }
        if (low > high)
            return self.timeMs;
        return std::max(low, std::min(high, timeMs));
    }

    void SetLookAt(Keyframe& marker, std::uint32_t target)
    {
        // A skeleton index belongs to the old target's skeleton;
        // a named bone travels (Head on another character).
        if (target == 0 || (target != marker.lookAtTarget && bones::ModeOf(marker.lookAtBone) == bones::kModeIndex))
            marker.lookAtBone = 0;
        marker.lookAtTarget = target;
        if (target != 0 && marker.attachTarget != 0 && target != marker.attachTarget)
            marker.attachType = AttachType::Flexible;
    }

    void SetAttach(Keyframe& marker, std::uint32_t target)
    {
        if (target == 0 || (target != marker.attachTarget && bones::ModeOf(marker.attachBone) == bones::kModeIndex))
            marker.attachBone = 0; // as SetLookAt
        marker.attachTarget = target;
        if (target != 0 && marker.lookAtTarget != 0 && target != marker.lookAtTarget)
            marker.attachType = AttachType::Flexible;
    }

    void SetAttachType(Keyframe& marker, AttachType type)
    {
        marker.attachType = type;
        if (type == AttachType::Rigid && marker.lookAtTarget != 0 && marker.attachTarget != 0 &&
            marker.lookAtTarget != marker.attachTarget)
        {
            marker.lookAtTarget = 0;
        }
    }

    bool Paste(Keyframe& to, const Keyframe& from, CopyContext context)
    {
        if (context == CopyContext::EffectsOnly)
        {
            to.filter = from.filter;
            to.filterIntensity = from.filterIntensity;
            to.saturation = from.saturation;
            to.contrast = from.contrast;
            to.vignette = from.vignette;
            to.brightness = from.brightness;
            return true;
        }
        if (context != CopyContext::All)
            return false; // no per marker audio
        Keyframe copy = from;
        copy.id = to.id;
        copy.timeMs = to.timeMs;
        copy.editedMask = to.editedMask;
        copy.easing = to.easing;
        copy.path = to.path;
        copy.mode = to.mode;
        copy.target = to.target;
        copy.offset = to.offset;
        // A fade belongs to the clip's first or last marker: pasting a middle
        // marker on them keeps no fade, pasting them on a middle marker
        // carries none.
        copy.transition = to.transition;
        copy.transitionMs = to.transitionMs;
        copy.origin = to.origin;
        copy.shake = mshake::Continued(from.shake);
        to = copy;
        return true;
    }

    bool ApplyOptions(Keyframe& marker, const editor::CameraKeyframeOptions& o)
    {
        const std::uint32_t mask = o.mask;
        const auto has = [mask](std::uint32_t bit) { return (mask & bit) != 0; };
        const auto finite = [](float value) { return std::isfinite(value); };
        if (has(editor::CameraOptionMode) && !IsModelMode(o.cameraMode))
            return false;
        if (has(editor::CameraOptionBlend) && static_cast<std::uint8_t>(o.blend) >= editor::kBlendCount)
            return false;
        if (has(editor::CameraOptionAttachType) &&
            static_cast<std::uint8_t>(o.attachType) > static_cast<std::uint8_t>(AttachType::Flexible))
            return false;
        if (has(editor::CameraOptionDof) && static_cast<std::uint8_t>(o.dof) > static_cast<std::uint8_t>(DofMode::None))
            return false;
        if (has(editor::CameraOptionFocus) &&
            static_cast<std::uint8_t>(o.focus) > static_cast<std::uint8_t>(FocusMode::Target))
            return false;
        if (has(editor::CameraOptionMarkerType) &&
            static_cast<std::uint8_t>(o.markerType) > static_cast<std::uint8_t>(MarkerType::Anchor))
            return false;
        if ((has(editor::CameraOptionLookAtOffset) && (!finite(o.lookAtOffset.x) || !finite(o.lookAtOffset.y))) ||
            (has(editor::CameraOptionLookAtRoll) && !finite(o.lookAtRoll)) ||
            (has(editor::CameraOptionFov) && !finite(o.fov)) ||
            (has(editor::CameraOptionPresetDistance) && !finite(o.presetDistanceScale)) ||
            (has(editor::CameraOptionDofIntensity) && !finite(o.dofIntensity)) ||
            (has(editor::CameraOptionFocusDistance) && !finite(o.focusDistance)))
            return false;

        if (has(editor::CameraOptionTransition) && o.transition > 1)
            return false;
        if (has(editor::CameraOptionFilter) && o.filter >= editor::kMarkerFilterCount)
            return false;
        if (has(editor::CameraOptionTransition))
            marker.transition = o.transition;
        if (has(editor::CameraOptionTransitionMs))
            marker.transitionMs = static_cast<std::uint16_t>(std::max<int>(100, std::min<int>(10000, o.transitionMs)));
        if (has(editor::CameraOptionFilter))
            marker.filter = o.filter;
        const auto percent = [](int value) { return static_cast<std::int8_t>(std::max(-100, std::min(100, value))); };
        if (has(editor::CameraOptionFilterIntensity))
            marker.filterIntensity = static_cast<std::uint8_t>(std::max<int>(0, std::min<int>(100, o.filterIntensity)));
        if (has(editor::CameraOptionSaturation))
            marker.saturation = percent(o.saturation);
        if (has(editor::CameraOptionContrast))
            marker.contrast = percent(o.contrast);
        if (has(editor::CameraOptionVignette))
            marker.vignette = percent(o.vignette);
        if (has(editor::CameraOptionBrightness))
            marker.brightness = percent(o.brightness);
        if (has(editor::CameraOptionMode))
            marker.cameraMode = o.cameraMode;
        if (has(editor::CameraOptionMarkerType))
            marker.markerType = o.markerType;
        if (has(editor::CameraOptionBlend))
            marker.blend = o.blend;
        if (has(editor::CameraOptionEase))
            marker.ease = o.ease != 0;
        if (has(editor::CameraOptionAttachTarget))
            SetAttach(marker, o.attachTarget);
        if (has(editor::CameraOptionLookAtTarget))
            SetLookAt(marker, o.lookAtTarget);
        if (has(editor::CameraOptionAttachType))
            SetAttachType(marker, o.attachType);
        if (has(editor::CameraOptionLookAtOffset))
            marker.lookAtOffset = {std::max(-1.0f, std::min(1.0f, o.lookAtOffset.x)),
                                   std::max(-1.0f, std::min(1.0f, o.lookAtOffset.y))};
        if (has(editor::CameraOptionLookAtRoll))
            marker.lookAtRoll = WrapDegrees(o.lookAtRoll);
        // The shake comes in Command::shake (CameraOptionShake, SetShake); the
        // options' old shake fields are not read.
        if (has(editor::CameraOptionSpeed))
            marker.speedIndex = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(o.speedIndex, editor::kMarkerSpeedCount - 1));
        if (has(editor::CameraOptionFov))
            marker.pose.fov = std::max(editor::kMarkerFovMin, std::min(editor::kMarkerFovMax, o.fov));
        if (has(editor::CameraOptionPresetDistance))
            marker.presetDistanceScale = std::max(0.0f, std::min(1.0f, o.presetDistanceScale));
        if (has(editor::CameraOptionDof))
            marker.dof = o.dof;
        if (has(editor::CameraOptionDofIntensity))
            marker.dofIntensity = std::max(0.0f, std::min(100.0f, o.dofIntensity));
        if (has(editor::CameraOptionFocus))
            marker.focus = o.focus;
        if (has(editor::CameraOptionFocusDistance))
            marker.focusDistance = std::max(0.5f, std::min(1000.0f, o.focusDistance));
        if (has(editor::CameraOptionFocusTarget))
            marker.focusTarget = o.focusTarget;
        marker.editedMask |= mask;
        return true;
    }

    void FillSnapshotKeyframe(const Keyframe& marker, editor::CameraKeyframe& out)
    {
        out = {};
        out.id = marker.id;
        out.timeMs = marker.timeMs;
        out.camera.position = marker.pose.position;
        out.camera.rotation = Track::ToEulerDegrees(marker.pose.orientation);
        out.camera.fov = marker.pose.fov;
        out.easing = marker.easing;
        out.path = marker.path;
        out.mode = marker.mode;
        out.target = marker.target;
        out.cameraMode = marker.cameraMode;
        out.lookAtTarget = marker.lookAtTarget;
        out.attachTarget = marker.attachTarget;
        out.focusTarget = marker.focusTarget;
        out.lookAtOffset = marker.lookAtOffset;
        out.lookAtRoll = marker.lookAtRoll;
        out.presetDistanceScale = marker.presetDistanceScale;
        out.dofIntensity = marker.dofIntensity;
        out.focusDistance = marker.focusDistance;
        out.markerType = marker.markerType;
        out.blend = marker.blend;
        out.ease = marker.ease ? 1 : 0;
        out.attachType = marker.attachType;
        // A summary for the old fields (the whole shake
        // is in Snapshot::cameraKeyframeShake).
        out.shake = mshake::SummaryType(marker.shake);
        out.shakeIntensity = mshake::IntensityOf(marker.shake.gain);
        out.shakeSpeedIndex = static_cast<std::uint8_t>(editor::kMarkerSpeedDefaultIndex);
        out.shakeMotion = 0;
        out.markerOrigin = marker.origin;
        out.speedIndex = marker.speedIndex;
        out.dof = marker.dof;
        out.focus = marker.focus;
        out.legacyCurve = marker.legacyCurve ? 1 : 0;
        out.transition = marker.transition;
        out.transitionMs = marker.transitionMs;
        out.filter = marker.filter;
        out.filterIntensity = marker.filterIntensity;
        out.saturation = marker.saturation;
        out.contrast = marker.contrast;
        out.vignette = marker.vignette;
        out.brightness = marker.brightness;
    }

    // ------------------------------------------------------------------
    // Evaluator.

    namespace
    {
        bool BlendsOut(const Keyframe& k, bool anyMode)
        {
            return k.blend != Blend::None && (anyMode || k.cameraMode == CameraMode::Free);
        }

        bool HasEase(const Keyframe& k, bool anyMode)
        {
            return k.ease && (anyMode || k.cameraMode == CameraMode::Free);
        }
    }

    EaseCurve ChainEase(const Keyframe* previous, const Keyframe& source, const Keyframe* next, bool anyMode)
    {
        const bool previousBlendEase = previous && BlendsOut(*previous, anyMode) && HasEase(*previous, anyMode);
        const bool nextEaseNoBlend = next && HasEase(*next, anyMode) && !BlendsOut(*next, anyMode);
        const bool nextBlendNoEase = next && BlendsOut(*next, anyMode) && !HasEase(*next, anyMode);
        if (HasEase(source, anyMode))
        {
            if (previousBlendEase)
                return nextBlendNoEase || nextEaseNoBlend ? EaseCurve::Out : EaseCurve::Linear;
            return nextEaseNoBlend ? EaseCurve::InOut : EaseCurve::In;
        }
        return nextEaseNoBlend ? EaseCurve::Out : EaseCurve::Linear;
    }

    // [hypothesis] In starts at rest and leaves with speed 1 (2p^2 - p^3), Out
    // mirrors it, InOut is a smoothstep. Named so the shapes change without
    // touching the rules.
    float ApplyEase(EaseCurve curve, float phase)
    {
        const float p = Clamp01(phase);
        switch (curve)
        {
        case EaseCurve::In:
            return 2.0f * p * p - p * p * p;
        case EaseCurve::Out:
        {
            const float q = 1.0f - p;
            return 1.0f - (2.0f * q * q - q * q * q);
        }
        case EaseCurve::InOut:
            return p * p * (3.0f - 2.0f * p);
        case EaseCurve::Linear:
        default:
            return p;
        }
    }

    constexpr float kShakeFadeOutMs = 250.0f;

    float ShakeAmplitude(std::uint8_t intensity)
    {
        return shake::Gain(intensity);
    }

    float FadeAmount(const Track& track, std::uint64_t timeMs)
    {
        const Keyframe* first = nullptr;
        const Keyframe* last = nullptr;
        for (const Keyframe& key : track.Keyframes())
        {
            if (key.markerType == MarkerType::Anchor)
                continue;
            if (!first)
                first = &key;
            last = &key;
        }
        float amount = 0.0f;
        if (first && first->transition == 1)
        {
            const double length = (std::max)(1.0, static_cast<double>(first->transitionMs));
            if (timeMs <= first->timeMs)
                amount = 1.0f;
            else
                amount = Clamp01(static_cast<float>(1.0 - static_cast<double>(timeMs - first->timeMs) / length));
        }
        if (last && last != first && last->transition == 1)
        {
            const double length = (std::max)(1.0, static_cast<double>(last->transitionMs));
            float out = 0.0f;
            if (timeMs >= last->timeMs)
                out = 1.0f;
            else if (static_cast<double>(last->timeMs - timeMs) < length)
                out = Clamp01(static_cast<float>(1.0 - static_cast<double>(last->timeMs - timeMs) / length));
            amount = (std::max)(amount, out);
        }
        return amount;
    }

    SegmentPlan Plan(const Track& track, std::uint64_t timeMs)
    {
        SegmentPlan plan;
        const std::vector<Keyframe>& keys = track.Keyframes();
        const bool anyMode = track.LegacyBlendAnyMode();
        std::vector<int> usable;
        usable.reserve(keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            if (keys[index].markerType != MarkerType::Anchor)
                usable.push_back(static_cast<int>(index));
        }
        if (usable.empty())
            return plan;
        // The last marker counts only as the destination of a blend (it is
        // the clip's end), or always on a legacy track.
        const int lastMarker = usable.back();
        if (usable.size() >= 2 && !anyMode && !BlendsOut(keys[usable[usable.size() - 2]], false))
            usable.pop_back();

        std::size_t position = 0;
        for (std::size_t n = 0; n < usable.size(); ++n)
        {
            if (keys[usable[n]].timeMs <= timeMs)
                position = n;
            else
                break;
        }
        const Keyframe& current = keys[usable[position]];
        plan.valid = true;
        plan.current = usable[position];
        plan.previous = position > 0 ? usable[position - 1] : -1;
        plan.next = position + 1 < usable.size() ? usable[position + 1] : -1;
        plan.nextNext = position + 2 < usable.size() ? usable[position + 2] : -1;
        const Keyframe* previous = plan.previous >= 0 ? &keys[plan.previous] : nullptr;
        const Keyframe* next = plan.next >= 0 ? &keys[plan.next] : nullptr;
        // The last marker keeps its stored blend (ApplyLastMarkerRule)
        // but never blends out: the chain ease sees it as No Blend.
        Keyframe lastAsEnd;
        if (next && plan.next == lastMarker && next->blend != Blend::None)
        {
            lastAsEnd = *next;
            lastAsEnd.blend = Blend::None;
            next = &lastAsEnd;
        }

        plan.blending = next && timeMs >= current.timeMs && current.blend != Blend::None &&
            (anyMode || (current.cameraMode == CameraMode::Free && next->cameraMode == CameraMode::Free));
        if (next && next->timeMs > current.timeMs && timeMs >= current.timeMs)
        {
            plan.rawPhase = Clamp01(static_cast<float>(
                static_cast<double>(timeMs - current.timeMs) / static_cast<double>(next->timeMs - current.timeMs)));
        }
        if (plan.blending && current.blend == Blend::Spline && !current.legacyCurve)
        {
            // A Spline segment eases with its own numbers (Ease In and Ease
            // Out, camera_spline.h), applied by EvaluateMarkers on time; the
            // chain ease is for Linear and Smooth.
            plan.curve = EaseCurve::Linear;
            plan.phase = plan.rawPhase;
        }
        else if (plan.blending)
        {
            plan.curve = ChainEase(previous, current, next, anyMode);
            plan.phase = ApplyEase(plan.curve, plan.rawPhase);
        }

        if (plan.blending && current.blend == Blend::Smooth)
        {
            plan.smooth = true;
            const bool first = !(previous && previous->blend == Blend::Smooth && BlendsOut(*previous, anyMode) &&
                (anyMode || current.cameraMode == CameraMode::Free));
            const bool last = !(next && next->blend == Blend::Smooth && plan.nextNext >= 0 &&
                BlendsOut(*next, anyMode) && (anyMode || keys[plan.nextNext].cameraMode == CameraMode::Free));
            float weight = 1.0f;
            if (first)
                weight = std::min(weight, Clamp01(plan.rawPhase / 0.5f));
            if (last)
                weight = std::min(weight, Clamp01((1.0f - plan.rawPhase) / 0.5f));
            plan.smoothWeight = weight;
            plan.smoothMix = last && plan.rawPhase > 0.8f ? Clamp01((1.0f - plan.rawPhase) / 0.2f) : 1.0f;
        }

        // DOF: the last marker's segment uses the one before it.
        plan.dofIndex = plan.current;
        if (position > 0 && plan.current == lastMarker)
        {
            plan.dofIndex = usable[position - 1];
        }
        // The DOF the blend arrives at (the segment that starts at the last
        // marker uses the one before it: current).
        if (plan.blending && plan.next >= 0)
            plan.dofNextIndex = plan.next == lastMarker ? plan.current : plan.next;

        // The OLD marker shake (legacyShake: only on a track not migrated
        // yet; the migration and its proof read it).
        // Shake: the chain of markers with the same shake keeps one clock,
        // each segment counting its duration x shake speed / marker speed.
        plan.shake.type = current.legacyShake.type;
        if (current.legacyShake.type != ShakeType::None)
        {
            plan.shake.amplitude = ShakeAmplitude(current.legacyShake.intensity);
            if (plan.blending && next->legacyShake.type == current.legacyShake.type)
                plan.shake.amplitude = Lerp(plan.shake.amplitude, ShakeAmplitude(next->legacyShake.intensity), plan.phase);
            // A chain that ends (the next marker has no shake or another one,
            // which fades in) fades out over its last 250 ms instead of
            // stopping dead on the marker. The last marker is no blend target
            // without a blend before it, but its shake still starts (or stops)
            // there. An Anchor is not the next marker (it has no shake of its
            // own; the shake would fade out before it and pop back).
            const Keyframe* following = next;
            for (std::size_t n = 0; !following && n < keys.size(); ++n)
            {
                if (keys[n].timeMs > current.timeMs && keys[n].markerType != MarkerType::Anchor)
                    following = &keys[n];
            }
            if (following && following->legacyShake.type != current.legacyShake.type && following->timeMs > timeMs)
            {
                const float left = static_cast<float>(following->timeMs - timeMs) / kShakeFadeOutMs;
                if (left < 1.0f)
                    plan.shake.amplitude *= left * left * (3.0f - 2.0f * left);
            }
            std::size_t start = position;
            while (start > 0 && keys[usable[start - 1]].legacyShake.type == current.legacyShake.type)
                --start;
            double shakeMs = 0.0;
            // A Custom shake's phase, its frequency integrated over the same
            // warped clock (linear inside a blend into another Custom marker).
            const bool customLerp = current.legacyShake.type == ShakeType::Custom && plan.blending && next->legacyShake.type == current.legacyShake.type;
            plan.shake.custom = customLerp ? shake::LerpCustom(current.legacyShake.custom, next->legacyShake.custom, plan.rawPhase)
                                           : current.legacyShake.custom;
            double cycles = 0.0;
            for (std::size_t n = start; n < position; ++n)
            {
                const Keyframe& a = keys[usable[n]];
                const Keyframe& b = keys[usable[n + 1]];
                const double span = static_cast<double>(b.timeMs - a.timeMs) * SpeedFactor(a.legacyShake.speedIndex) / SpeedFactor(a.speedIndex);
                shakeMs += span;
                const bool lerped = a.blend != Blend::None && a.cameraMode == CameraMode::Free && b.cameraMode == CameraMode::Free &&
                    b.legacyShake.type == a.legacyShake.type;
                cycles += span / 1000.0 * (lerped ? 0.5 * (a.legacyShake.custom.hz + b.legacyShake.custom.hz) : a.legacyShake.custom.hz);
            }
            if (timeMs > current.timeMs)
            {
                const double span = static_cast<double>(timeMs - current.timeMs) *
                    SpeedFactor(current.legacyShake.speedIndex) / SpeedFactor(current.speedIndex);
                shakeMs += span;
                const double hz = customLerp
                    ? current.legacyShake.custom.hz + (next->legacyShake.custom.hz - current.legacyShake.custom.hz) * plan.rawPhase * 0.5
                    : current.legacyShake.custom.hz;
                cycles += span / 1000.0 * hz;
            }
            plan.shake.timeMs = shakeMs;
            plan.shake.cycles = cycles;
            plan.shake.runStartMs = keys[usable[start]].timeMs;
        }
        return plan;
    }

    editor::Vec3 LookAtWithScreenOffset(
        const editor::Vec3& point,
        const editor::Vec3& cameraPosition,
        const editor::Vec2& screenOffset,
        float fov,
        float roll,
        float aspect)
    {
        Vec3 w = Sub(point, cameraPosition);
        const float length = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
        if (length < 0.0001f)
            return {0.0f, roll, 0.0f};
        w = Scale(w, 1.0f / length);

        // The direction the point must have in camera space (x right, y
        // forward, z up) to land at the offset on screen.
        const float tanV = std::tan(std::max(1.0f, std::min(170.0f, fov)) * 0.5f * kDegreesToRadians);
        const float tanH = tanV * (aspect > 0.01f ? aspect : 16.0f / 9.0f);
        Vec3 d{screenOffset.x * tanH, 1.0f, screenOffset.y * tanV};
        const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        d = Scale(d, 1.0f / dl);

        // R = Rz(h) Rx(p) Ry(r): apply the roll, then solve the pitch so the
        // height matches, then the heading so the ground direction matches.
        const float r = roll * kDegreesToRadians;
        const float cr = std::cos(r), sr = std::sin(r);
        const Vec3 rolled{cr * d.x + sr * d.z, d.y, -sr * d.x + cr * d.z};
        const float amplitude = std::sqrt(rolled.y * rolled.y + rolled.z * rolled.z);
        const float phi = std::atan2(rolled.z, rolled.y);
        const float ratio = amplitude > 0.0001f ? std::max(-1.0f, std::min(1.0f, w.z / amplitude)) : 0.0f;
        const float pitch = std::asin(ratio) - phi;
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const Vec3 v{rolled.x, rolled.y * cp - rolled.z * sp, rolled.y * sp + rolled.z * cp};
        const float heading = std::atan2(w.y, w.x) - std::atan2(v.y, v.x);
        return {pitch * kRadiansToDegrees, WrapDegrees(roll), WrapDegrees(heading * kRadiansToDegrees)};
    }

    bool ProjectToScreen(const editor::CameraState& camera, const editor::Vec3& point, float aspect, editor::Vec2& screen)
    {
        const Vec3 local = Mul(Transpose(FromEuler(camera.rotation)), Sub(point, camera.position));
        if (local.y <= 0.0001f)
            return false;
        const float tanV = std::tan(camera.fov * 0.5f * kDegreesToRadians);
        const float tanH = tanV * aspect;
        screen = {local.x / local.y / tanH, local.z / local.y / tanV};
        return true;
    }

    bool PresetLayout(CameraMode mode, editor::Vec3& offset, editor::Vec3& lookAt)
    {
        switch (mode)
        {
        case CameraMode::FrontLow:
            offset = {0.0f, 2.2f, 0.4f};
            lookAt = {0.0f, 0.0f, 0.3f}; // the chest
            return true;
        case CameraMode::Rear:
            offset = {0.0f, -3.0f, 1.2f};
            lookAt = {0.0f, 0.0f, 0.5f};
            return true;
        case CameraMode::RightSide:
            offset = {3.0f, 0.0f, 1.0f};
            lookAt = {0.0f, 0.0f, 0.5f};
            return true;
        case CameraMode::LeftSide:
            offset = {-3.0f, 0.0f, 1.0f};
            lookAt = {0.0f, 0.0f, 0.5f};
            return true;
        case CameraMode::Overhead:
            offset = {0.0f, -0.5f, 8.0f};
            lookAt = {0.0f, 0.0f, 0.0f};
            return true;
        default:
            return false;
        }
    }

    LivePose Resolve(const Keyframe& marker, std::uint64_t timeMs, const Scene& scene)
    {
        LivePose live;
        live.pose = marker.pose;
        live.lookAtOffset = marker.lookAtOffset;
        live.lookAtRoll = marker.lookAtRoll;
        Vec3 position = marker.pose.position;
        Vec3 rotation = Track::ToEulerDegrees(marker.pose.orientation);
        const float fov = marker.pose.fov;

        if (marker.cameraMode == CameraMode::Game)
        {
            editor::CameraState game{};
            if (scene.game && EvaluateGame(*scene.game, timeMs, game, scene.cuts))
                live.pose = {game.position, Track::FromEulerDegrees(game.rotation), game.fov};
            return live;
        }

        if (IsPreset(marker.cameraMode))
        {
            EntityState main;
            Vec3 offset{}, look{};
            if (!scene.lookup || !scene.mainCharacter || !PresetLayout(marker.cameraMode, offset, look) ||
                !scene.lookup(scene.mainCharacter, timeMs, main))
            {
                live.targetMissing = true;
                return live;
            }
            const Mat3 frame = AttachFrame(AttachType::Rigid, main);
            const float distance = Lerp(0.5f, 1.5f, Clamp01(marker.presetDistanceScale));
            position = Add(main.position, Mul(frame, Scale(offset, distance)));
            const Vec3 aim = Add(main.position, Mul(frame, look));
            const float roll = marker.cameraMode == CameraMode::Overhead ? 0.0f : marker.lookAtRoll;
            rotation = LookAtWithScreenOffset(aim, position, {0.0f, 0.0f}, fov, roll, scene.aspect);
            live.hasLookAt = true;
            live.lookAtPoint = aim;
            live.lookAtOffset = {0.0f, 0.0f};
            live.lookAtRoll = roll;
            live.pose = {position, Track::FromEulerDegrees(rotation), fov};
            return live;
        }

        // Free (a legacy Target or Follow value never reaches here through
        // the model; it is treated as Free).
        if (marker.attachTarget != 0)
        {
            EntityState target;
            if (LookupAttach(marker, timeMs, scene, target)) // through its bone
            {
                const Mat3 frame = AttachFrame(marker.attachType, target);
                position = Add(target.position, Mul(frame, marker.relPosition));
                rotation = ToEuler(Mul(frame, FromEuler(marker.relRotation)));
            }
            else
                live.targetMissing = true;
        }
        if (marker.lookAtTarget != 0)
        {
            EntityState target;
            if (LookupLookAt(marker, timeMs, scene, target))
            {
                const Vec3 aim = AimPoint(target);
                rotation = LookAtWithScreenOffset(aim, position, marker.lookAtOffset, fov, marker.lookAtRoll, scene.aspect);
                live.hasLookAt = true;
                live.lookAtPoint = aim;
            }
            else
                live.targetMissing = true;
        }
        live.pose = {position, Track::FromEulerDegrees(rotation), fov};
        return live;
    }

    void CaptureAttachPose(Keyframe& marker, const editor::CameraState& world, const EntityState& target)
    {
        const Mat3 frame = AttachFrame(marker.attachType, target);
        const Mat3 inverse = Transpose(frame);
        marker.relPosition = Mul(inverse, Sub(world.position, target.position));
        marker.relRotation = ToEuler(Mul(inverse, FromEuler(world.rotation)));
    }

    void SetMarkerView(Keyframe& marker, const editor::CameraState& view, const Scene& scene)
    {
        editor::CameraState stored = view;
        stored.fov = std::max(editor::kMarkerFovMin, std::min(editor::kMarkerFovMax, view.fov));
        marker.pose = {stored.position, Track::FromEulerDegrees(stored.rotation), stored.fov};
        EntityState target;
        if (LookupAttach(marker, marker.timeMs, scene, target))
            CaptureAttachPose(marker, stored, target);
        if (LookupLookAt(marker, marker.timeMs, scene, target))
        {
            editor::Vec2 screen{};
            if (ProjectToScreen(stored, AimPoint(target), scene.aspect, screen))
            {
                marker.lookAtOffset = {std::max(-1.0f, std::min(1.0f, screen.x)), std::max(-1.0f, std::min(1.0f, screen.y))};
                marker.lookAtRoll = WrapDegrees(stored.rotation.y);
            }
        }
        ClearBorrowed(marker);
    }

    void SpringState::Reset()
    {
        primed = false;
        for (int n = 0; n < 7; ++n)
        {
            value[n] = 0.0f;
            velocity[n] = 0.0f;
        }
    }

    editor::CameraState SpringState::Step(const editor::CameraState& target, float stiffness, float dtSeconds)
    {
        const float goal[7] = {
            target.position.x, target.position.y, target.position.z,
            target.rotation.x, target.rotation.y, target.rotation.z, target.fov
        };
        if (!primed || dtSeconds <= 0.0f || stiffness <= 0.0f)
        {
            for (int n = 0; n < 7; ++n)
            {
                value[n] = goal[n];
                velocity[n] = 0.0f;
            }
            primed = dtSeconds > 0.0f;
            return target;
        }
        // Critically damped (ratio 1): x'' = -k e - 2 sqrt(k) x', solved
        // exactly over the step so any dt is stable.
        const float omega = std::sqrt(stiffness);
        const float decay = std::exp(-omega * dtSeconds);
        for (int n = 0; n < 7; ++n)
        {
            const bool angle = n >= 3 && n <= 5;
            const float error = angle ? -WrapDegrees(goal[n] - value[n]) : value[n] - goal[n];
            const float v0 = velocity[n];
            const float c = v0 + omega * error;
            const float e = (error + c * dtSeconds) * decay;
            velocity[n] = (v0 - omega * c * dtSeconds) * decay;
            value[n] = angle ? WrapDegrees(value[n] + (e - error)) : goal[n] + e;
        }
        return {{value[0], value[1], value[2]}, {value[3], value[4], value[5]}, value[6]};
    }

    ShakeDelta ShakeOffset(ShakeType type, float amplitude, double shakeTimeMs)
    {
        const shake::Sample sample = shake::Evaluate(type, amplitude, shakeTimeMs / 1000.0);
        ShakeDelta delta;
        delta.rotation = {sample.value[shake::Pitch], sample.value[shake::Roll], sample.value[shake::Yaw]};
        delta.position = {sample.value[shake::X], sample.value[shake::Y], sample.value[shake::Z]};
        return delta;
    }

    namespace
    {
        // A recording cut strictly after `from`, at or before `to`.
        bool CutBetween(const Scene& scene, std::uint64_t from, std::uint64_t to)
        {
            if (!scene.cuts)
                return false;
            for (const std::uint32_t cut : *scene.cuts)
                if (cut > from && cut <= to)
                    return true;
            return false;
        }

        // ------------------------------------------------------------------
        // The Spline blend's curve. A run of markers where each one blends
        // Spline into the next (both Free, no recording cut between) is one
        // curve. The evaluator sees a window of it around the segment that
        // plays: slots -3..4, the current marker at 0 and the next one at 1
        // (a segment's curve needs its markers' neighbours, Continuous also
        // the two segments around it). Each segment's path follows its own
        // source marker's tuning (pacing, curve shape, weight), so two
        // segments that share a marker with the same tuning agree on it.
        constexpr int kSlotLow = -3;
        constexpr int kSlotHigh = 4;
        constexpr int kSlotCount = kSlotHigh - kSlotLow + 1;
        // The arc length table of a segment (Continuous and Per Segment):
        // samples of its path, chord lengths summed. The same count for every
        // segment, so a segment's length is the same whichever window asks.
        constexpr int kArcSamples = 48;

        int PreviousUsable(const std::vector<Keyframe>& keys, int index)
        {
            for (int n = index - 1; n >= 0; --n)
                if (keys[n].markerType != MarkerType::Anchor)
                    return n;
            return -1;
        }

        int NextUsable(const std::vector<Keyframe>& keys, int index)
        {
            for (int n = index + 1; n < static_cast<int>(keys.size()); ++n)
                if (keys[n].markerType != MarkerType::Anchor)
                    return n;
            return -1;
        }

        bool SplineLinked(const std::vector<Keyframe>& keys, int from, int to, const Scene& scene)
        {
            if (from < 0 || to < 0)
                return false;
            const Keyframe& a = keys[from];
            const Keyframe& b = keys[to];
            return a.blend == Blend::Spline && !a.legacyCurve && a.cameraMode == CameraMode::Free &&
                b.cameraMode == CameraMode::Free && b.timeMs > a.timeMs && !CutBetween(scene, a.timeMs, b.timeMs);
        }

        Quaternion Conjugate(const Quaternion& q)
        {
            return {q.w, -q.x, -q.y, -q.z};
        }

        float Dot(const Quaternion& a, const Quaternion& b)
        {
            return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        }

        Quaternion AlignTo(Quaternion q, const Quaternion& reference)
        {
            if (Dot(q, reference) < 0.0f)
                q = {-q.w, -q.x, -q.y, -q.z};
            return q;
        }

        // The rotation of a unit quaternion as a vector (axis x half angle)
        // and back: the camera weight averages rotations in this space
        // around the curve's own rotation, not as Euler angles.
        Vec3 LogRotation(Quaternion q)
        {
            if (q.w < 0.0f)
                q = {-q.w, -q.x, -q.y, -q.z};
            const double length = std::sqrt(static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
                static_cast<double>(q.z) * q.z);
            if (length < 1e-9)
                return {q.x, q.y, q.z};
            const double half = std::atan2(length, static_cast<double>(q.w));
            const float scale = static_cast<float>(half / length);
            return {q.x * scale, q.y * scale, q.z * scale};
        }

        Quaternion ExpRotation(const Vec3& v)
        {
            const double half = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                static_cast<double>(v.z) * v.z);
            if (half < 1e-9)
                return Normalize({1.0f, v.x, v.y, v.z});
            const float scale = static_cast<float>(std::sin(half) / half);
            return {static_cast<float>(std::cos(half)), v.x * scale, v.y * scale, v.z * scale};
        }

        struct SplineRun
        {
            const std::vector<Keyframe>* keys = nullptr;
            const Scene* scene = nullptr;
            std::uint64_t timeMs = 0;
            double originMs = 0.0;          // the current marker's time
            int first = 0;                  // the lowest and highest slots on the curve
            int last = 1;
            int index[kSlotCount];
            editor::SplineTuning tuning[kSlotCount]; // each marker's effective tuning
            mutable bool resolved[kSlotCount];
            mutable LivePose live[kSlotCount];

            bool Has(int slot) const { return slot >= first && slot <= last; }
            const Keyframe& Key(int slot) const { return (*keys)[index[slot - kSlotLow]]; }
            const editor::SplineTuning& Tuning(int slot) const { return tuning[slot - kSlotLow]; }
            double Seconds(int slot) const { return (static_cast<double>(Key(slot).timeMs) - originMs) / 1000.0; }
            double Weight(int slot) const { return scene->splineWeight ? Tuning(slot).weight : 0.0; }
            const LivePose& Live(int slot) const
            {
                const int at = slot - kSlotLow;
                if (!resolved[at])
                {
                    live[at] = Resolve(Key(slot), timeMs, *scene);
                    resolved[at] = true;
                }
                return live[at];
            }
        };

        SplineRun MakeRun(const std::vector<Keyframe>& keys, const SegmentPlan& plan, const Scene& scene, std::uint64_t timeMs,
            const LivePose& a, const LivePose& b)
        {
            SplineRun run;
            run.keys = &keys;
            run.scene = &scene;
            run.timeMs = timeMs;
            run.originMs = static_cast<double>(keys[plan.current].timeMs);
            for (int n = 0; n < kSlotCount; ++n)
            {
                run.index[n] = -1;
                run.resolved[n] = false;
            }
            run.index[0 - kSlotLow] = plan.current;
            run.index[1 - kSlotLow] = plan.next;
            run.live[0 - kSlotLow] = a;
            run.live[1 - kSlotLow] = b;
            run.resolved[0 - kSlotLow] = run.resolved[1 - kSlotLow] = true;
            for (int slot = -1; slot >= kSlotLow; --slot)
            {
                const int to = run.index[slot + 1 - kSlotLow];
                const int from = PreviousUsable(keys, to);
                if (!SplineLinked(keys, from, to, scene))
                    break;
                run.index[slot - kSlotLow] = from;
                run.first = slot;
            }
            for (int slot = 2; slot <= kSlotHigh; ++slot)
            {
                const int from = run.index[slot - 1 - kSlotLow];
                const int to = NextUsable(keys, from);
                if (!SplineLinked(keys, from, to, scene))
                    break;
                run.index[slot - kSlotLow] = to;
                run.last = slot;
            }
            for (int slot = run.first; slot <= run.last; ++slot)
                run.tuning[slot - kSlotLow] = spline::Effective(run.Key(slot).spline, scene.splineDefaults);
            return run;
        }

        // A knot past the curve's ends: the spacing mirrored about the end
        // marker (so the B-spline passes through the curve's first and last
        // marker with the mirrored control point).
        template <typename RealKnot>
        double KnotAt(const SplineRun& run, int slot, const RealKnot& real, int depth = 0)
        {
            if (run.Has(slot) || depth > 3)
                return real(std::max(run.first, std::min(run.last, slot)));
            if (slot < run.first)
                return 2.0 * real(run.first) - KnotAt(run, 2 * run.first - slot, real, depth + 1);
            return 2.0 * real(run.last) - KnotAt(run, 2 * run.last - slot, real, depth + 1);
        }

        // A control point past the curve's ends: the mirror of the one inside.
        Vec3 PointAt(const SplineRun& run, int slot)
        {
            if (run.Has(slot))
                return run.Live(slot).pose.position;
            const int end = slot < run.first ? run.first : run.last;
            const int inner = slot < run.first ? run.first + 1 : run.last - 1;
            const Vec3 e = run.Live(end).pose.position;
            const Vec3 i = run.Live(inner).pose.position;
            return {2.0f * e.x - i.x, 2.0f * e.y - i.y, 2.0f * e.z - i.z};
        }

        // One segment's path (slots s to s+1): a cubic Hermite over its knots,
        // pulled toward the cubic B-spline over the same markers by the
        // camera weight. Natural: the knots are the marker times (seconds
        // from the current marker), the tangents the time weighted secants
        // (Simple Camera's curve). Off Natural: Catmull-Rom knots spaced
        // |dP|^alpha. At the curve's first and last marker the natural end
        // condition (no bend there).
        struct PathSegment
        {
            double p1[3]{}, p2[3]{}, m1[3]{}, m2[3]{};
            double k0 = 0.0;          // the knot at its start
            double h = 1.0;           // its length in knot units
            double w1 = 0.0, w2 = 0.0; // the camera weight at its two markers
            double knots[6]{};        // slots s-2..s+3
            double control[4][3]{};   // slots s-1..s+2

            Vec3 At(double u) const
            {
                const double x = h > 0.0 ? (u - k0) / h : 0.0;
                double out[3];
                for (int axis = 0; axis < 3; ++axis)
                    out[axis] = spline::Hermite(p1[axis], p2[axis], m1[axis], m2[axis], h, x);
                if (w1 > 0.0 || w2 > 0.0)
                {
                    const double weight = spline::WeightBetween(w1, w2, x);
                    if (weight > 0.0)
                    {
                        double b[4];
                        spline::BSplineWeights(knots, u, b);
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const double bs = b[0] * control[0][axis] + b[1] * control[1][axis] + b[2] * control[2][axis] +
                                b[3] * control[3][axis];
                            out[axis] += weight * (bs - out[axis]);
                        }
                    }
                }
                return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
            }
        };

        PathSegment MakePathSegment(const SplineRun& run, int s)
        {
            const editor::SplineTuning& tuning = run.Tuning(s);
            const bool natural = static_cast<editor::SplinePacing>(tuning.pacing) == editor::SplinePacing::Natural;
            const double alpha = tuning.alpha;
            // The real knots of the slots on the curve around s.
            double real[kSlotCount]{};
            if (natural)
            {
                for (int slot = run.first; slot <= run.last; ++slot)
                    real[slot - kSlotLow] = run.Seconds(slot);
            }
            else
            {
                real[s - kSlotLow] = 0.0;
                const auto spacing = [&run, alpha](int slot) {
                    const Vec3 d = Sub(run.Live(slot + 1).pose.position, run.Live(slot).pose.position);
                    const double length = std::max(1e-3, std::sqrt(static_cast<double>(d.x) * d.x +
                        static_cast<double>(d.y) * d.y + static_cast<double>(d.z) * d.z));
                    return alpha <= 0.0 ? 1.0 : std::pow(length, alpha);
                };
                const int low = std::max(run.first, s - 2);
                const int high = std::min(run.last, s + 3);
                for (int slot = s + 1; slot <= high; ++slot)
                    real[slot - kSlotLow] = real[slot - 1 - kSlotLow] + spacing(slot - 1);
                for (int slot = s - 1; slot >= low; --slot)
                    real[slot - kSlotLow] = real[slot + 1 - kSlotLow] - spacing(slot);
            }
            const auto realKnot = [&real](int slot) { return real[slot - kSlotLow]; };
            const auto knot = [&](int slot) { return KnotAt(run, slot, realKnot); };

            PathSegment seg;
            seg.k0 = knot(s);
            seg.h = std::max(1e-9, knot(s + 1) - seg.k0);
            const Vec3 a = PointAt(run, s);
            const Vec3 b = PointAt(run, s + 1);
            const double pa[3] = {a.x, a.y, a.z};
            const double pb[3] = {b.x, b.y, b.z};
            // The tangent at an inner marker (one with both neighbours on the curve).
            const auto tangent = [&](int slot, double out[3]) {
                const Vec3 before = PointAt(run, slot - 1);
                const Vec3 here = PointAt(run, slot);
                const Vec3 after = PointAt(run, slot + 1);
                const double kb = knot(slot - 1), kh = knot(slot), ka = knot(slot + 1);
                const double vb[3] = {before.x, before.y, before.z};
                const double vh[3] = {here.x, here.y, here.z};
                const double va[3] = {after.x, after.y, after.z};
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (natural)
                        out[axis] = ka - kb > 1e-12 ? (va[axis] - vb[axis]) / (ka - kb) : 0.0;
                    else
                    {
                        const double left = std::max(1e-12, kh - kb);
                        const double right = std::max(1e-12, ka - kh);
                        out[axis] = (vh[axis] - vb[axis]) / left - (va[axis] - vb[axis]) / (left + right) +
                            (va[axis] - vh[axis]) / right;
                    }
                }
            };
            const bool inner1 = run.Has(s - 1);
            const bool inner2 = run.Has(s + 2);
            if (inner1)
                tangent(s, seg.m1);
            if (inner2)
                tangent(s + 1, seg.m2);
            for (int axis = 0; axis < 3; ++axis)
            {
                seg.p1[axis] = pa[axis];
                seg.p2[axis] = pb[axis];
                const double chord = (pb[axis] - pa[axis]) / seg.h;
                if (!inner1 && !inner2)
                    seg.m1[axis] = seg.m2[axis] = chord;
                else if (!inner1)
                    seg.m1[axis] = spline::NaturalEnd(chord, seg.m2[axis]);
                else if (!inner2)
                    seg.m2[axis] = spline::NaturalEnd(chord, seg.m1[axis]);
            }
            seg.w1 = run.Weight(s);
            seg.w2 = run.Weight(s + 1);
            if (seg.w1 > 0.0 || seg.w2 > 0.0)
            {
                for (int n = 0; n < 6; ++n)
                    seg.knots[n] = knot(s - 2 + n);
                for (int n = 0; n < 4; ++n)
                {
                    const Vec3 p = PointAt(run, s - 1 + n);
                    seg.control[n][0] = p.x;
                    seg.control[n][1] = p.y;
                    seg.control[n][2] = p.z;
                }
            }
            return seg;
        }

        // A segment's path sampled into its arc length.
        struct ArcTable
        {
            double u[kArcSamples + 1]{};
            double s[kArcSamples + 1]{};
            double length = 0.0;

            void Build(const PathSegment& seg)
            {
                Vec3 previous{};
                for (int n = 0; n <= kArcSamples; ++n)
                {
                    u[n] = seg.k0 + seg.h * static_cast<double>(n) / kArcSamples;
                    const Vec3 p = seg.At(u[n]);
                    if (n == 0)
                        s[n] = 0.0;
                    else
                    {
                        const Vec3 d = Sub(p, previous);
                        s[n] = s[n - 1] + std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y +
                            static_cast<double>(d.z) * d.z);
                    }
                    previous = p;
                }
                length = s[kArcSamples];
            }

            // The parameter at a distance along the segment: inside each
            // sample a cubic of distance (slopes from the neighbouring
            // samples), so the speed has no steps at the samples.
            double ParamAt(double distance) const
            {
                if (length < 1e-9)
                    return u[0];
                if (distance <= 0.0)
                    return u[0];
                if (distance >= length)
                    return u[kArcSamples];
                int low = 0, high = kArcSamples;
                while (high - low > 1)
                {
                    const int mid = (low + high) / 2;
                    if (s[mid] <= distance)
                        low = mid;
                    else
                        high = mid;
                }
                const double span = s[high] - s[low];
                if (span < 1e-12)
                    return u[low];
                const auto slope = [this](int n) {
                    const int a = std::max(0, n - 1), b = std::min(kArcSamples, n + 1);
                    const double ds = s[b] - s[a];
                    return ds > 1e-12 ? (u[b] - u[a]) / ds : 0.0;
                };
                const double x = (distance - s[low]) / span;
                const double value = spline::Hermite(u[low], u[high], slope(low), slope(high), span, x);
                return std::max(u[low], std::min(u[high], value));
            }
        };

        struct SplineSample
        {
            Vec3 position{};
            Quaternion orientation{};
            float fov = 45.0f;
            float phase = 0.0f; // the eased fraction of the segment (every channel's)
        };

        // The Spline segment at t: the eased time, the position by the pacing,
        // the rotation and the zoom by the (eased) time. A channel the tuning
        // leaves out goes straight (lerp, slerp) on the eased phase.
        SplineSample EvaluateSpline(const std::vector<Keyframe>& keys, const SegmentPlan& plan, const Scene& scene,
            std::uint64_t timeMs, const LivePose& a, const LivePose& b)
        {
            const SplineRun run = MakeRun(keys, plan, scene, timeMs, a, b);
            const editor::SplineTuning& own = run.Tuning(0);
            const double h0 = std::max(1e-6, run.Seconds(1));
            const double rawPhase = std::max(0.0, std::min(1.0, (static_cast<double>(timeMs) - run.originMs) / 1000.0 / h0));
            const bool eased = own.easeIn > 0.0f || own.easeOut > 0.0f;
            const double phase = eased ? spline::Ease(rawPhase, own.easeIn, own.easeOut) : rawPhase;
            const double te = eased ? phase * h0 : std::max(0.0, std::min(h0, (static_cast<double>(timeMs) - run.originMs) / 1000.0));
            SplineSample out;
            out.phase = static_cast<float>(phase);

            // Position.
            if ((own.channels & editor::SplineChannelPosition) == 0)
                out.position = LerpVec(a.pose.position, b.pose.position, out.phase);
            else
            {
                const PathSegment seg = MakePathSegment(run, 0);
                const editor::SplinePacing pacing = static_cast<editor::SplinePacing>(own.pacing);
                if (pacing == editor::SplinePacing::Natural)
                    out.position = seg.At(te);
                else
                {
                    ArcTable table;
                    table.Build(seg);
                    double distance = phase * table.length;
                    if (pacing == editor::SplinePacing::Continuous)
                    {
                        // One speed curve through the move: a monotone cubic
                        // of time through (marker time, distance so far), so
                        // the speed carries through the markers and each one
                        // is reached at its time.
                        const bool before = run.Has(-1);
                        const bool after = run.Has(2);
                        double lengthBefore = 0.0, lengthAfter = 0.0, hBefore = 0.0, hAfter = 0.0;
                        if (before)
                        {
                            ArcTable t;
                            t.Build(MakePathSegment(run, -1));
                            lengthBefore = t.length;
                            hBefore = std::max(1e-6, -run.Seconds(-1));
                        }
                        if (after)
                        {
                            ArcTable t;
                            t.Build(MakePathSegment(run, 1));
                            lengthAfter = t.length;
                            hAfter = std::max(1e-6, run.Seconds(2) - run.Seconds(1));
                        }
                        const double d0 = table.length / h0;
                        const double dBefore = before ? lengthBefore / hBefore : 0.0;
                        const double dAfter = after ? lengthAfter / hAfter : 0.0;
                        const double slopeStart = before ? spline::MonotoneInner(hBefore, h0, dBefore, d0)
                            : after ? spline::MonotoneEnd(h0, hAfter, d0, dAfter) : d0;
                        const double slopeEnd = after ? spline::MonotoneInner(h0, hAfter, d0, dAfter)
                            : before ? spline::MonotoneEnd(h0, hBefore, d0, dBefore) : d0;
                        distance = spline::Hermite(0.0, table.length, slopeStart, slopeEnd, h0, phase);
                    }
                    out.position = seg.At(table.ParamAt(std::max(0.0, std::min(table.length, distance))));
                }
            }

            // The rotation and the zoom: by time on the markers' times (never
            // by the arc length, which would kick them at the markers).
            double times[kSlotCount]{};
            for (int slot = run.first; slot <= run.last; ++slot)
                times[slot - kSlotLow] = run.Seconds(slot);
            const auto realTime = [&times](int slot) { return times[slot - kSlotLow]; };
            const auto timeKnot = [&](int slot) { return KnotAt(run, slot, realTime); };
            const bool inner0 = run.Has(-1);
            const bool inner1 = run.Has(2);
            const double weight = spline::WeightBetween(run.Weight(0), run.Weight(1), phase);

            if ((own.channels & editor::SplineChannelRotation) == 0)
                out.orientation = Slerp(a.pose.orientation, b.pose.orientation, out.phase);
            else
            {
                // Hermite on the quaternion's components (each aligned to the
                // one before it), natural at the curve's ends.
                Quaternion q[4];
                q[1] = a.pose.orientation;
                q[2] = AlignTo(b.pose.orientation, q[1]);
                q[0] = inner0 ? AlignTo(run.Live(-1).pose.orientation, q[1]) : q[1];
                q[3] = inner1 ? AlignTo(run.Live(2).pose.orientation, q[2]) : q[2];
                const double t0 = inner0 ? timeKnot(-1) : 0.0, t3 = inner1 ? timeKnot(2) : h0;
                double rotation[4];
                for (int c = 0; c < 4; ++c)
                {
                    const auto component = [c](const Quaternion& v) {
                        return static_cast<double>(c == 0 ? v.w : c == 1 ? v.x : c == 2 ? v.y : v.z);
                    };
                    const double p0 = component(q[0]), p1 = component(q[1]), p2 = component(q[2]), p3 = component(q[3]);
                    const double chord = (p2 - p1) / h0;
                    double m1 = 0.0, m2 = 0.0;
                    if (inner0)
                        m1 = (p2 - p0) / std::max(1e-9, h0 - t0);
                    if (inner1)
                        m2 = (p3 - p1) / std::max(1e-9, t3);
                    if (!inner0 && !inner1)
                        m1 = m2 = chord;
                    else if (!inner0)
                        m1 = spline::NaturalEnd(chord, m2);
                    else if (!inner1)
                        m2 = spline::NaturalEnd(chord, m1);
                    rotation[c] = spline::Hermite(p1, p2, m1, m2, h0, te / h0);
                }
                Quaternion curve = Normalize({static_cast<float>(rotation[0]), static_cast<float>(rotation[1]),
                    static_cast<float>(rotation[2]), static_cast<float>(rotation[3])});
                if (weight > 0.0)
                {
                    // The camera weight on the rotation: the B-spline's
                    // average of the markers' rotations, taken as rotation
                    // vectors around the curve's own rotation (exp/log), then
                    // the curve turned toward it.
                    double knots[6];
                    for (int n = 0; n < 6; ++n)
                        knots[n] = timeKnot(-2 + n);
                    double basis[4];
                    spline::BSplineWeights(knots, te, basis);
                    const Quaternion inverse = Conjugate(curve);
                    double sum[3] = {};
                    for (int n = 0; n < 4; ++n)
                    {
                        const int slot = -1 + n;
                        Quaternion control;
                        if (run.Has(slot))
                            control = run.Live(slot).pose.orientation;
                        else
                        {
                            // Mirrored past the curve's end: e * i^-1 * e.
                            const int end = slot < run.first ? run.first : run.last;
                            const int inner = slot < run.first ? run.first + 1 : run.last - 1;
                            const Quaternion e = run.Live(end).pose.orientation;
                            const Quaternion i = AlignTo(run.Live(inner).pose.orientation, e);
                            control = Normalize(Multiply(Multiply(e, Conjugate(i)), e));
                        }
                        const Vec3 v = LogRotation(Normalize(Multiply(inverse, control)));
                        sum[0] += basis[n] * v.x;
                        sum[1] += basis[n] * v.y;
                        sum[2] += basis[n] * v.z;
                    }
                    const Vec3 toward{static_cast<float>(sum[0] * weight), static_cast<float>(sum[1] * weight),
                        static_cast<float>(sum[2] * weight)};
                    curve = Normalize(Multiply(curve, ExpRotation(toward)));
                }
                out.orientation = curve;
            }

            if ((own.channels & editor::SplineChannelFov) == 0)
                out.fov = Lerp(a.pose.fov, b.pose.fov, out.phase);
            else
            {
                // A monotone cubic (Fritsch-Carlson): a zoom never passes its
                // target and comes back.
                const double f1 = a.pose.fov, f2 = b.pose.fov;
                const double d0 = (f2 - f1) / h0;
                double hBefore = 0.0, dBefore = 0.0, hAfter = 0.0, dAfter = 0.0;
                if (inner0)
                {
                    hBefore = std::max(1e-9, -timeKnot(-1));
                    dBefore = (f1 - run.Live(-1).pose.fov) / hBefore;
                }
                if (inner1)
                {
                    hAfter = std::max(1e-9, timeKnot(2) - h0);
                    dAfter = (run.Live(2).pose.fov - f2) / hAfter;
                }
                const double m1 = inner0 ? spline::MonotoneInner(hBefore, h0, dBefore, d0)
                    : inner1 ? spline::MonotoneEnd(h0, hAfter, d0, dAfter) : d0;
                const double m2 = inner1 ? spline::MonotoneInner(h0, hAfter, d0, dAfter)
                    : inner0 ? spline::MonotoneEnd(h0, hBefore, d0, dBefore) : d0;
                double fov = spline::Hermite(f1, f2, m1, m2, h0, te / h0);
                if (weight > 0.0)
                {
                    double knots[6];
                    for (int n = 0; n < 6; ++n)
                        knots[n] = timeKnot(-2 + n);
                    double basis[4];
                    spline::BSplineWeights(knots, te, basis);
                    double bs = 0.0;
                    for (int n = 0; n < 4; ++n)
                    {
                        const int slot = -1 + n;
                        double value = 0.0;
                        if (run.Has(slot))
                            value = run.Live(slot).pose.fov;
                        else
                        {
                            const int end = slot < run.first ? run.first : run.last;
                            const int inner = slot < run.first ? run.first + 1 : run.last - 1;
                            value = 2.0 * run.Live(end).pose.fov - run.Live(inner).pose.fov;
                        }
                        bs += basis[n] * value;
                    }
                    fov += weight * (bs - fov);
                }
                out.fov = std::max(1.0f, static_cast<float>(fov));
            }
            return out;
        }
    }

    static bool EvaluateMarkersImpl(const Track& track, std::uint64_t timeMs, const Scene& scene, editor::CameraState& camera,
        SpringState* spring, float dtSeconds, SegmentPlan* planOut, bool withShake);

    namespace
    {
        // The camera's speed (metres per clip second) from the curve itself
        // around t, without springs or shake: scrub equals play.
        float CurveSpeed(const Track& track, const Scene& scene, std::int64_t at)
        {
            const std::uint64_t from = static_cast<std::uint64_t>(std::max<std::int64_t>(0, at - kShakeMotionProbeMs));
            const std::uint64_t to = static_cast<std::uint64_t>(std::max<std::int64_t>(0, at + kShakeMotionProbeMs));
            editor::CameraState a{}, b{};
            if (to <= from || !EvaluateMarkersImpl(track, from, scene, a, nullptr, 0.0f, nullptr, false) ||
                !EvaluateMarkersImpl(track, to, scene, b, nullptr, 0.0f, nullptr, false))
                return 0.0f;
            const float dx = b.position.x - a.position.x, dy = b.position.y - a.position.y, dz = b.position.z - a.position.z;
            const float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / (static_cast<float>(to - from) / 1000.0f);
            return speed < kShakeMotionCutSpeed ? speed : 0.0f; // a jump is not a move
        }

        // Gone while the camera stands still: a fast attack, a release over
        // about 250 ms (Simple Camera's Stop When Still), from the curve.
        float StillGate(const Track& track, const Scene& scene, std::int64_t now, float speedNow)
        {
            const auto moving = [](float s) {
                return std::max(0.0f, std::min(1.0f, (s - kShakeMotionStillSpeed) / 0.6f));
            };
            return std::max({moving(speedNow), 0.6f * moving(CurveSpeed(track, scene, now - 120)),
                0.25f * moving(CurveSpeed(track, scene, now - 240))});
        }

        // What the Custom shake's phase memo depends on: every marker field
        // the evaluated camera (its path, the shake) can read, the events, the
        // cuts and the scene's inputs (FNV-1a over them). A field left out
        // keeps positions sampled from an older path (finding 15 of the
        // release review: presetDistanceScale, the look at offset and roll, the
        // bones, the legacy view). Only the picture's own settings (DOF,
        // focus, near clip, filters, colour, the edited mask) are not mixed.
        std::uint64_t ShakeSignature(const Track& track, const Scene& scene)
        {
            std::uint64_t h = 1469598103934665603ull;
            const auto mix = [&h](const void* data, std::size_t size) {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                for (std::size_t n = 0; n < size; ++n)
                    h = (h ^ bytes[n]) * 1099511628211ull;
            };
            for (const Keyframe& k : track.Keyframes())
            {
                mix(&k.id, sizeof(k.id));
                mix(&k.timeMs, sizeof(k.timeMs));
                mix(&k.pose, sizeof(k.pose));
                mix(&k.cameraMode, sizeof(k.cameraMode));
                mix(&k.blend, sizeof(k.blend));
                mix(&k.ease, sizeof(k.ease));
                mix(&k.lookAtTarget, sizeof(k.lookAtTarget));
                mix(&k.attachTarget, sizeof(k.attachTarget));
                mix(&k.attachType, sizeof(k.attachType));
                mix(&k.relPosition, sizeof(k.relPosition));
                mix(&k.relRotation, sizeof(k.relRotation));
                mix(&k.shake, sizeof(k.shake)); // the MarkerShake
                mix(&k.origin, sizeof(k.origin));
                mix(&k.legacyShake.type, sizeof(k.legacyShake.type));
                mix(&k.legacyShake.speedIndex, sizeof(k.legacyShake.speedIndex));
                mix(&k.speedIndex, sizeof(k.speedIndex));
                mix(&k.markerType, sizeof(k.markerType));
                mix(&k.legacyShake.custom, sizeof(k.legacyShake.custom));
                mix(&k.spline, sizeof(k.spline)); // the curve's tuning
                // The rest of the path's inputs.
                mix(&k.easing, sizeof(k.easing));
                mix(&k.path, sizeof(k.path));
                mix(&k.mode, sizeof(k.mode));
                mix(&k.target, sizeof(k.target));
                mix(&k.offset, sizeof(k.offset));
                mix(&k.lookAtOffset, sizeof(k.lookAtOffset));
                mix(&k.lookAtRoll, sizeof(k.lookAtRoll));
                mix(&k.presetDistanceScale, sizeof(k.presetDistanceScale));
                mix(&k.legacyShake.intensity, sizeof(k.legacyShake.intensity));
                mix(&k.legacyShake.motion, sizeof(k.legacyShake.motion));
                mix(&k.legacyCurve, sizeof(k.legacyCurve));
                mix(&k.transition, sizeof(k.transition));
                mix(&k.transitionMs, sizeof(k.transitionMs));
                mix(&k.attachBone, sizeof(k.attachBone));
                mix(&k.lookAtBone, sizeof(k.lookAtBone));
            }
            mix(&scene.splineDefaults, sizeof(scene.splineDefaults));
            mix(&scene.splineWeight, sizeof(scene.splineWeight));
            mix(&scene.aspect, sizeof(scene.aspect));
            mix(&scene.mainCharacter, sizeof(scene.mainCharacter));
            const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(scene.game);
            const std::size_t gameSamples = scene.game ? scene.game->size() : 0;
            mix(&game, sizeof(game));
            mix(&gameSamples, sizeof(gameSamples));
            const bool gameOnly = static_cast<bool>(scene.gameOnly);
            mix(&gameOnly, sizeof(gameOnly));
            for (const fx::ShakeEvent& e : track.LegacyEffects().Events())
            {
                mix(&e.id, sizeof(e.id));
                mix(&e.timeMs, sizeof(e.timeMs));
                mix(&e.kind, sizeof(e.kind));
                mix(&e.value, sizeof(e.value));
                mix(&e.mode, sizeof(e.mode));
            }
            const std::size_t cuts = scene.cuts ? scene.cuts->size() : 0;
            mix(&cuts, sizeof(cuts));
            if (scene.cuts)
                for (const std::uint32_t cut : *scene.cuts)
                    mix(&cut, sizeof(cut));
            return h;
        }

        // hz x speedHz and speedRef of the source's Custom shake at t (0 when
        // the source plays no Custom shake there).
        void CouplingAt(const Track& track, int source, std::uint64_t t, float& rate, float& speedRef)
        {
            rate = 0.0f;
            speedRef = 30.0f;
            if (source == 1)
            {
                // The events' Freq drives the noise and the
                // profiles' clock alike, so the coupling counts for both.
                const fx::State state = fx::StateAt(track.LegacyEffects(), static_cast<double>(t));
                if (state.enabled)
                {
                    rate = std::max(0.0f, state.hz) * state.speedHz;
                    speedRef = 30.0f;
                }
                return;
            }
            if (source == 2)
            {
                // The markers' shake, its numbers at t.
                const mshake::Resolved r = mshake::Resolve(track, t);
                if (r.active)
                {
                    rate = std::max(0.0f, r.custom.hz) * r.custom.speedHz;
                    speedRef = r.custom.speedRef;
                }
                return;
            }
            const SegmentPlan plan = Plan(track, t);
            if (plan.valid && plan.shake.type == ShakeType::Custom)
            {
                rate = plan.shake.custom.hz * plan.shake.custom.speedHz;
                speedRef = plan.shake.custom.speedRef;
            }
        }
    }

    double CustomCouplingCycles(const Track& track, const Scene& scene, int source, std::uint64_t runStartMs, std::uint64_t timeMs)
    {
        if (timeMs <= runStartMs)
            return 0.0;
        ShakePhaseCache local;
        ShakePhaseCache& cache = scene.shakePhase ? *scene.shakePhase : local;
        const std::uint64_t signature = ShakeSignature(track, scene);
        if (cache.signature != signature || cache.runStartMs != runStartMs || cache.source != source)
        {
            cache.Clear();
            cache.signature = signature;
            cache.runStartMs = runStartMs;
            cache.source = source;
            ++cache.rebuilds;
        }
        const std::uint64_t cell = (timeMs - runStartMs) / kShakePhaseGridMs;
        const auto point = [&](std::uint64_t n) {
            if (cache.points.size() <= n)
            {
                cache.points.resize(static_cast<std::size_t>(n) + 1);
                cache.known.resize(static_cast<std::size_t>(n) + 1, 0);
            }
            if (!cache.known[n])
            {
                editor::CameraState view{};
                EvaluateMarkersImpl(track, runStartMs + n * kShakePhaseGridMs, scene, view, nullptr, 0.0f, nullptr, false);
                cache.points[n] = view.position;
                cache.known[n] = 1;
            }
            return cache.points[n];
        };
        // One cell's coupling rate (cycles per second): hz x speedHz x the
        // normalised speed over the cell.
        const auto cellRate = [&](std::uint64_t n) {
            float rate = 0.0f, speedRef = 30.0f;
            CouplingAt(track, source, runStartMs + n * kShakePhaseGridMs, rate, speedRef);
            if (rate == 0.0f)
                return 0.0;
            const editor::Vec3 a = point(n), b = point(n + 1);
            const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            const float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / (static_cast<float>(kShakePhaseGridMs) / 1000.0f);
            const float norm = speed < kShakeMotionCutSpeed ? std::min(1.0f, speed / std::max(1.0f, speedRef)) : 0.0f;
            return static_cast<double>(rate) * norm;
        };
        if (cache.sums.empty())
            cache.sums.push_back(0.0);
        while (cache.sums.size() <= cell)
        {
            const std::uint64_t n = cache.sums.size() - 1;
            cache.sums.push_back(cache.sums.back() + cellRate(n) * (kShakePhaseGridMs / 1000.0));
        }
        const double partial = static_cast<double>(timeMs - runStartMs - cell * kShakePhaseGridMs) / 1000.0;
        return cache.sums[static_cast<std::size_t>(cell)] + cellRate(cell) * partial;
    }

    bool SetShakeCustom(Keyframe& marker, const editor::ShakeCustom& custom)
    {
        const editor::ShakeCustom c = custom;
        if (!std::isfinite(c.amp) || !std::isfinite(c.hz) || !std::isfinite(c.speedAmp) || !std::isfinite(c.speedHz) ||
            !std::isfinite(c.rotWeight) || !std::isfinite(c.posWeight) || !std::isfinite(c.speedRef))
            return false;
        marker.legacyShake.custom = shake::ClampCustom(c);
        return true;
    }

    bool SetShake(Keyframe& marker, const editor::MarkerShake& shake)
    {
        if (!mshake::Usable(shake))
            return false;
        marker.shake = mshake::Clamped(shake);
        marker.editedMask |= editor::CameraOptionShake;
        return true;
    }

    float ShakeCurveSpeed(const Track& track, const Scene& scene, std::int64_t at) { return CurveSpeed(track, scene, at); }

    float ShakeStillGate(const Track& track, const Scene& scene, std::int64_t now, float speedNow)
    {
        return StillGate(track, scene, now, speedNow);
    }

    bool SetSplineTuning(Keyframe& marker, const editor::SplineTuning& tuning)
    {
        if (!std::isfinite(tuning.alpha) || !std::isfinite(tuning.easeIn) || !std::isfinite(tuning.easeOut) ||
            !std::isfinite(tuning.weight) || tuning.pacing >= editor::kSplinePacingCount)
            return false;
        marker.spline = spline::Clamped(tuning);
        marker.editedMask |= editor::CameraOptionSpline;
        return true;
    }

    float ShakeMotionGain(const Track& track, std::uint64_t timeMs, const Scene& scene, ShakeMotion motion)
    {
        if (motion == ShakeMotion::Off)
            return 1.0f;
        const auto speedAt = [&](std::int64_t at) { return CurveSpeed(track, scene, at); };
        const std::int64_t now = static_cast<std::int64_t>(timeMs);
        const float speed = speedAt(now);
        float gain = 1.0f + std::max(0.0f, std::min(1.0f, speed / kShakeMotionFullSpeed));
        if (motion == ShakeMotion::SpeedStill)
            gain *= StillGate(track, scene, now, speed);
        return gain;
    }

    bool EvaluateMarkers(
        const Track& track,
        std::uint64_t timeMs,
        const Scene& scene,
        editor::CameraState& camera,
        SpringState* spring,
        float dtSeconds,
        SegmentPlan* planOut)
    {
        return EvaluateMarkersImpl(track, timeMs, scene, camera, spring, dtSeconds, planOut, true);
    }

    bool EvaluateMarkersRaw(const Track& track, std::uint64_t timeMs, const Scene& scene, editor::CameraState& camera,
        SegmentPlan* planOut)
    {
        return EvaluateMarkersImpl(track, timeMs, scene, camera, nullptr, 0.0f, planOut, false);
    }

    static bool EvaluateMarkersImpl(
        const Track& track,
        std::uint64_t timeMs,
        const Scene& scene,
        editor::CameraState& camera,
        SpringState* spring,
        float dtSeconds,
        SegmentPlan* planOut,
        bool withShake)
    {
        SegmentPlan plan = Plan(track, timeMs);
        // A blend never crosses a recording cut: the segment holds
        // its source marker (No Blend) up to the next marker.
        if (plan.valid && plan.blending && plan.next >= 0 && scene.cuts && !scene.cuts->empty())
        {
            const std::vector<Keyframe>& keys = track.Keyframes();
            const std::uint64_t from = keys[plan.current].timeMs;
            const std::uint64_t to = keys[plan.next].timeMs;
            for (const std::uint32_t cut : *scene.cuts)
            {
                if (cut > from && cut <= to)
                {
                    plan.blending = false;
                    plan.smooth = false;
                    plan.phase = plan.rawPhase = 0.0f;
                    plan.dofNextIndex = -1;
                    break;
                }
            }
        }
        if (planOut)
            *planOut = plan;
        if (!plan.valid)
            return false;
        // LockCutsceneCamera: inside a recorded cutscene every
        // marker is the recorded game camera.
        if (scene.gameOnly && scene.game && scene.gameOnly(timeMs))
        {
            if (spring)
                spring->Reset();
            return EvaluateGame(*scene.game, timeMs, camera, scene.cuts);
        }
        const std::vector<Keyframe>& keys = track.Keyframes();
        const Keyframe& current = keys[plan.current];
        const LivePose a = Resolve(current, timeMs, scene);

        editor::CameraState raw{};
        // The shared Look At of a blend, aimed again after the springs.
        Vec3 lookAtPointValue{};
        const Vec3* lookAtPoint = nullptr;
        Vec2 lookAtOffset{};
        float lookAtRoll = 0.0f;
        if (!plan.blending)
        {
            raw = {a.pose.position, Track::ToEulerDegrees(a.pose.orientation), a.pose.fov};
        }
        else
        {
            const Keyframe& next = keys[plan.next];
            const LivePose b = Resolve(next, timeMs, scene);
            const bool spline = !current.legacyCurve && current.blend == Blend::Spline;
            float t = plan.phase;
            if (spline)
            {
                // The curve through the markers (Simple Camera's time-aware
                // Hermite) with REPlus's options (camera_spline.h): natural
                // ends, the zoom as a monotone cubic, the numeric ease on time,
                // the pacing, the curve shape, the camera weight and the
                // channels. Each marker is resolved at t (an attached one moves
                // with its target, as the blend's ends do).
                const SplineSample sample = EvaluateSpline(keys, plan, scene, timeMs, a, b);
                raw.position = sample.position;
                raw.fov = sample.fov;
                raw.rotation = Track::ToEulerDegrees(sample.orientation);
                t = sample.phase;
                plan.phase = sample.phase;
                if (planOut)
                    planOut->phase = sample.phase;
            }
            else if (current.legacyCurve)
            {
                // The old Hermite path through the outer neighbours (clips
                // converted from v2 whose segment was Curved).
                const Keyframe& before = plan.previous >= 0 ? keys[plan.previous] : current;
                const Keyframe& after = plan.nextNext >= 0 ? keys[plan.nextNext] : next;
                const Vec3 p0 = plan.previous >= 0 ? Resolve(before, timeMs, scene).pose.position : a.pose.position;
                const Vec3 p3 = plan.nextNext >= 0 ? Resolve(after, timeMs, scene).pose.position : b.pose.position;
                const double duration = static_cast<double>(next.timeMs - current.timeMs);
                const double easedTime = static_cast<double>(current.timeMs) + t * duration;
                const auto axis = [&](float editor::Vec3::*member) {
                    return Hermite(p0.*member, a.pose.position.*member, b.pose.position.*member, p3.*member,
                        static_cast<double>(before.timeMs), static_cast<double>(current.timeMs),
                        static_cast<double>(next.timeMs), static_cast<double>(after.timeMs), easedTime);
                };
                raw.position = {axis(&editor::Vec3::x), axis(&editor::Vec3::y), axis(&editor::Vec3::z)};
            }
            else
            {
                raw.position = LerpVec(a.pose.position, b.pose.position, t);
            }
            if (!spline)
                raw.fov = Lerp(a.pose.fov, b.pose.fov, t);
            if (a.hasLookAt && b.hasLookAt && current.lookAtTarget != 0 && current.lookAtTarget == next.lookAtTarget)
            {
                const Vec2 offset{Lerp(a.lookAtOffset.x, b.lookAtOffset.x, t), Lerp(a.lookAtOffset.y, b.lookAtOffset.y, t)};
                const float roll = LerpAngle(a.lookAtRoll, b.lookAtRoll, t);
                raw.rotation = LookAtWithScreenOffset(b.lookAtPoint, raw.position, offset, raw.fov, roll, scene.aspect);
                lookAtPointValue = b.lookAtPoint;
                lookAtPoint = &lookAtPointValue;
                lookAtOffset = offset;
                lookAtRoll = roll;
            }
            else if (!spline)
            {
                raw.rotation = Track::ToEulerDegrees(Slerp(a.pose.orientation, b.pose.orientation, t));
            }
        }

        if (spring)
        {
            if (plan.smooth && dtSeconds > 0.0f)
            {
                // Springs run on the world pose would trail a camera attached
                // to a moving target by 2 v / sqrt(k) (3.8 m behind a 6 m/s
                // gallop at k = 10), in clip time, so at 5 % too. With both
                // markers on the same attach target they run in its attach
                // frame: the target is followed exactly and only the blend
                // between the markers is rounded. A Look At shared by both is
                // aimed again from the sprung position, so the target keeps its
                // framing.
                const Keyframe& next = keys[plan.next];
                std::uint32_t anchor = 0;
                std::uint32_t anchorBone = 0; // the same target and the same bone
                AttachType anchorType = AttachType::Flexible;
                EntityState anchorState;
                if (current.attachTarget != 0 && current.attachTarget == next.attachTarget &&
                    current.attachBone == next.attachBone && LookupAttach(current, timeMs, scene, anchorState))
                {
                    anchor = current.attachTarget;
                    anchorBone = current.attachBone;
                    anchorType = current.attachType == next.attachType ? current.attachType : AttachType::Flexible;
                }
                const Mat3 frame = anchor ? AttachFrame(anchorType, anchorState) : Identity();
                if (spring->primed &&
                    (spring->anchor != anchor || spring->anchorBone != anchorBone || spring->anchorType != anchorType))
                {
                    // The springs keep the camera where it was, re-expressed in
                    // the new anchor's frame (the old one read at the same
                    // time).
                    bool reframed = false;
                    if (spring->reframe)
                    {
                        EntityState oldState;
                        const bool oldFound = spring->anchor == 0 ||
                            LookupTarget(scene, spring->anchor, spring->anchorBone, timeMs, oldState);
                        if (oldFound)
                        {
                            const Mat3 oldFrame = spring->anchor ? AttachFrame(spring->anchorType, oldState) : Identity();
                            const Vec3 oldOrigin = spring->anchor ? oldState.position : Vec3{};
                            const Vec3 newOrigin = anchor ? anchorState.position : Vec3{};
                            ReframeSpring(*spring, oldFrame, oldOrigin, frame, newOrigin);
                            ++spring->reframes;
                            reframed = true;
                        }
                    }
                    if (!reframed)
                    {
                        spring->Reset();
                        ++spring->resets;
                    }
                }
                spring->anchor = anchor;
                spring->anchorBone = anchorBone;
                spring->anchorType = anchorType;
                const Mat3 inverse = Transpose(frame);
                editor::CameraState local = raw;
                if (anchor)
                {
                    local.position = Mul(inverse, Sub(raw.position, anchorState.position));
                    local.rotation = ToEuler(Mul(inverse, FromEuler(raw.rotation)));
                }
                const editor::CameraState sprungLocal = spring->Step(local, Lerp(100.0f, 10.0f, plan.smoothWeight), dtSeconds);
                editor::CameraState sprung = sprungLocal;
                if (anchor)
                {
                    sprung.position = Add(anchorState.position, Mul(frame, sprungLocal.position));
                    sprung.rotation = ToEuler(Mul(frame, FromEuler(sprungLocal.rotation)));
                }
                const float mix = plan.smoothMix;
                const Vec3 rawPosition = raw.position;
                raw.position = LerpVec(raw.position, sprung.position, mix);
                raw.rotation = {LerpAngle(raw.rotation.x, sprung.rotation.x, mix),
                                LerpAngle(raw.rotation.y, sprung.rotation.y, mix),
                                LerpAngle(raw.rotation.z, sprung.rotation.z, mix)};
                raw.fov = Lerp(raw.fov, sprung.fov, mix);
                if (lookAtPoint)
                    raw.rotation = LookAtWithScreenOffset(*lookAtPoint, raw.position, lookAtOffset, raw.fov, lookAtRoll, scene.aspect);
                const Vec3 lag = Sub(raw.position, rawPosition);
                plan.smoothAnchor = anchor;
                plan.smoothLagMeters = std::sqrt(lag.x * lag.x + lag.y * lag.y + lag.z * lag.z);
                if (planOut)
                {
                    planOut->smoothAnchor = plan.smoothAnchor;
                    planOut->smoothLagMeters = plan.smoothLagMeters;
                }
            }
            else
            {
                spring->Reset();
            }
        }

        // The shake is the markers' (camera_marker_shake.h). A track not
        // migrated yet (the old marker shake or the old events track still in
        // it: the migration's own proof, the tests of the old model) plays the
        // old rules (mshake::LegacyDelta).
        if (withShake)
        {
            ShakeDelta delta = mshake::Delta(track, timeMs, scene);
            if (track.HasLegacyShake())
            {
                const ShakeDelta old = mshake::LegacyDelta(track, timeMs, scene);
                delta.position = Add(delta.position, old.position);
                delta.rotation = Add(delta.rotation, old.rotation);
            }
            raw.position = Add(raw.position, delta.position);
            raw.rotation = {raw.rotation.x + delta.rotation.x, WrapDegrees(raw.rotation.y + delta.rotation.y),
                            WrapDegrees(raw.rotation.z + delta.rotation.z)};
        }
        camera = raw;
        return true;
    }
}

namespace replay::camera
{
    void TrackThirdPerson(ScopeAnchor& anchor, const editor::CameraState& rendered, const editor::Vec3& player)
    {
        const editor::Vec3 world{rendered.position.x - player.x, rendered.position.y - player.y, rendered.position.z - player.z};
        anchor.valid = true;
        anchor.inScope = false;
        anchor.offset = ToHeadingFrame(world, rendered.rotation.z);
        anchor.pitch = rendered.rotation.x;
        anchor.roll = rendered.rotation.y;
        anchor.heading = rendered.rotation.z;
        anchor.fov = rendered.fov;
    }

    editor::CameraState ThirdPersonDuringScope(ScopeAnchor& anchor, const editor::CameraState& scope, const editor::Vec3& player)
    {
        if (!anchor.inScope)
        {
            anchor.inScope = true;
            anchor.scopeHeading = scope.rotation.z;
            anchor.scopePitch = scope.rotation.x;
            if (!anchor.valid)
            {
                anchor.offset = kScopeDefaultOffset;
                anchor.pitch = std::max(-20.0f, std::min(20.0f, scope.rotation.x));
                anchor.roll = 0.0f;
                anchor.heading = scope.rotation.z;
                anchor.fov = 50.0f;
            }
        }
        // The view turns with the scope: heading by the same amount, pitch
        // too but held short of straight up or down.
        const float heading = WrapDegrees(anchor.heading + WrapDegrees(scope.rotation.z - anchor.scopeHeading));
        const float pitch = std::max(-70.0f, std::min(70.0f, anchor.pitch + WrapDegrees(scope.rotation.x - anchor.scopePitch)));
        const editor::Vec3 offset = FromHeadingFrame(anchor.offset, heading);
        editor::CameraState camera{};
        camera.position = {player.x + offset.x, player.y + offset.y, player.z + offset.z};
        camera.rotation = {pitch, anchor.roll, heading};
        camera.fov = anchor.fov;
        return camera;
    }
}

// Depth of field of a marker (focus source, f-stop, focus depth).
namespace replay::camera
{
    const char* FocusSourceName(FocusSource source)
    {
        switch (source)
        {
        case FocusSource::Default: return "default";
        case FocusSource::Sharp: return "none";
        case FocusSource::Manual: return "manual";
        case FocusSource::LookAt: return "look_at";
        case FocusSource::Attach: return "attach";
        case FocusSource::Target: return "target";
        case FocusSource::MainCharacter: return "main";
        case FocusSource::Engine: return "engine";
        case FocusSource::Held: return "held";
        }
        return "?";
    }

    float DofFStop(float intensity)
    {
        const float amount = std::max(0.0f, std::min(100.0f, intensity)) / 100.0f;
        return kDofSharpFStop * std::pow(0.5f / kDofSharpFStop, amount);
    }

    float FocusDepth(const editor::CameraState& camera, const editor::Vec3& point)
    {
        const Vec3 delta = Sub(point, camera.position);
        const Vec3 local = Mul(Transpose(FromEuler(camera.rotation)), delta);
        float depth = local.y;
        if (!(depth > kFocusMinMeters))
            depth = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (!std::isfinite(depth))
            depth = kFocusMinMeters;
        return std::max(kFocusMinMeters, std::min(kFocusMaxMeters, depth));
    }

    FocusPlan PlanFocus(const Keyframe* marker, const editor::CameraState& camera, const Scene& scene, std::uint64_t timeMs)
    {
        return PlanFocus(marker, camera, scene, timeMs, nullptr, FocusConfig{});
    }

    void FocusMemory::ResetShot()
    {
        hold[0] = FocusHold{};
        hold[1] = FocusHold{};
        hasDistance = false;
        lastDistance = 10.0f;
        springPrimed = false;
        springValue = 0.0f;
        springVelocity = 0.0f;
        lastSource = FocusSource::Default;
    }

    FocusHold& FocusMemory::HoldFor(std::uint32_t markerId)
    {
        if (hold[0].marker == markerId)
            return hold[0];
        if (hold[1].marker == markerId)
            return hold[1];
        // The blend's two markers: a new one pushes out the oldest.
        if (hold[0].marker == 0)
        {
            hold[0] = FocusHold{};
            hold[0].marker = markerId;
            return hold[0];
        }
        if (hold[1].marker != 0)
            hold[0] = hold[1];
        hold[1] = FocusHold{};
        hold[1].marker = markerId;
        return hold[1];
    }

    namespace
    {
        bool HasDistance(FocusSource source)
        {
            return source == FocusSource::Manual || source == FocusSource::LookAt || source == FocusSource::Attach ||
                source == FocusSource::Target || source == FocusSource::MainCharacter || source == FocusSource::Held;
        }

        void Remember(FocusMemory* memory, const FocusPlan& plan)
        {
            if (!memory)
                return;
            if (HasDistance(plan.source))
            {
                memory->hasDistance = true;
                memory->lastDistance = plan.distance;
            }
        }
    }

    FocusPlan PlanFocus(const Keyframe* marker, const editor::CameraState& camera, const Scene& scene, std::uint64_t timeMs,
        FocusMemory* memory, const FocusConfig& config)
    {
        FocusPlan plan;
        if (!marker || marker->dof == DofMode::Default)
            return plan;
        if (marker->dof == DofMode::None)
        {
            plan.source = FocusSource::Sharp;
            return plan;
        }
        // The marker's own f-stop and lens when set (the intensity stays the
        // shortcut that maps to both).
        plan.fstop = marker->dofFStop > 0.0f ? marker->dofFStop : DofFStop(marker->dofIntensity);
        plan.lensMm = marker->dofLensMm;
        plan.intensity = marker->dofIntensity;
        if (marker->focus == FocusMode::Manual)
        {
            plan.source = FocusSource::Manual;
            plan.distance = std::max(kFocusMinMeters, std::min(kFocusMaxMeters, marker->focusDistance));
            plan.straightMeters = plan.distance;
            Remember(memory, plan);
            return plan;
        }
        const bool hysteresis = memory && config.hysteresis;
        const auto take = [&](std::uint32_t id, FocusSource source, const EntityState& state) {
            const Vec3 point = AimPoint(state);
            const Vec3 delta = Sub(point, camera.position);
            plan.source = source;
            plan.entity = id;
            plan.point = point;
            plan.distance = FocusDepth(camera, point);
            plan.straightMeters = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            Remember(memory, plan);
            return true;
        };
        const auto focusOn = [&](std::uint32_t id, FocusSource source, bool onScreenOnly) {
            EntityState state;
            if (!id || !scene.lookup || !scene.lookup(id, timeMs, state))
                return false;
            if (onScreenOnly)
            {
                editor::Vec2 screen{};
                if (!ProjectToScreen(camera, AimPoint(state), scene.aspect, screen) || std::fabs(screen.x) > 1.0f ||
                    std::fabs(screen.y) > 1.0f)
                    return false;
            }
            return take(id, source, state);
        };
        // The main character in Auto, with the hysteresis (FocusConfig).
        const auto mainInFrame = [&]() {
            const std::uint32_t id = scene.mainCharacter;
            if (!hysteresis)
                return focusOn(id, FocusSource::MainCharacter, true);
            FocusHold& hold = memory->HoldFor(marker->id);
            EntityState state;
            const bool found = id && scene.lookup && scene.lookup(id, timeMs, state);
            editor::Vec2 screen{};
            const bool projected = found && ProjectToScreen(camera, AimPoint(state), scene.aspect, screen);
            const bool inside = projected && std::fabs(screen.x) < config.enter && std::fabs(screen.y) < config.enter;
            const bool within = projected && std::fabs(screen.x) <= config.leave && std::fabs(screen.y) <= config.leave;
            if (!hold.locked)
            {
                if (inside)
                {
                    hold.locked = true;
                    hold.outside = false;
                }
            }
            else if (within)
            {
                hold.outside = false;
            }
            else
            {
                if (!hold.outside || timeMs < hold.outsideSinceMs)
                {
                    hold.outside = true;
                    hold.outsideSinceMs = timeMs;
                }
                if (timeMs - hold.outsideSinceMs >= config.graceMs)
                {
                    hold.locked = false;
                    hold.outside = false;
                }
            }
            return hold.locked && found && take(id, FocusSource::MainCharacter, state);
        };
        if (marker->focus == FocusMode::Target)
        {
            // The focus target list starts at the player (index 0); an unset
            // target (0) is the main character.
            const std::uint32_t id = marker->focusTarget ? marker->focusTarget : scene.mainCharacter;
            if (focusOn(id, FocusSource::Target, false))
                return plan;
            plan.targetMissing = id != 0;
        }
        else
        {
            // A Look At on a bone focuses that bone.
            EntityState lookState;
            if (marker->lookAtTarget && marker->lookAtBone && LookupLookAt(*marker, timeMs, scene, lookState) &&
                take(marker->lookAtTarget, FocusSource::LookAt, lookState))
                return plan;
            if (marker->lookAtTarget && focusOn(marker->lookAtTarget, FocusSource::LookAt, false))
                return plan;
            plan.targetMissing = marker->lookAtTarget != 0;
            if (marker->attachTarget && focusOn(marker->attachTarget, FocusSource::Attach, false))
                return plan;
            if (mainInFrame())
                return plan;
        }
        plan.entity = 0;
        if (hysteresis && config.holdLast && memory->hasDistance)
        {
            plan.source = FocusSource::Held;
            plan.distance = memory->lastDistance;
            plan.straightMeters = plan.distance;
            return plan;
        }
        plan.source = FocusSource::Engine;
        return plan;
    }

    BlendedFocus BlendFocus(const Track& track, const SegmentPlan& plan, const editor::CameraState& camera, const Scene& scene,
        std::uint64_t timeMs, FocusMemory* memory, const FocusConfig& config)
    {
        BlendedFocus out;
        const std::vector<Keyframe>& keys = track.Keyframes();
        const int count = static_cast<int>(keys.size());
        const Keyframe* from = plan.valid && plan.dofIndex >= 0 && plan.dofIndex < count ? &keys[plan.dofIndex] : nullptr;
        const FocusPlan a = PlanFocus(from, camera, scene, timeMs, memory, config);
        out.plan = a;
        out.from = out.to = a.source;
        if (!config.blend || !from || !plan.blending || plan.dofNextIndex < 0 || plan.dofNextIndex >= count ||
            plan.dofNextIndex == plan.dofIndex)
            return out;
        const FocusPlan b = PlanFocus(&keys[plan.dofNextIndex], camera, scene, timeMs, memory, config);
        out.to = b.source;
        if (a.source == FocusSource::Default && b.source == FocusSource::Default)
            return out;
        const float t = Clamp01(plan.phase);
        out.blended = true;
        out.phase = t;
        const bool sharpA = a.source == FocusSource::Default || a.source == FocusSource::Sharp;
        const bool sharpB = b.source == FocusSource::Default || b.source == FocusSource::Sharp;
        const float fstopA = sharpA ? kDofSharpFStop : a.fstop;
        const float fstopB = sharpB ? kDofSharpFStop : b.fstop;
        const bool distA = HasDistance(a.source), distB = HasDistance(b.source);
        FocusPlan r = t < 0.5f ? a : b;
        r.fstop = std::exp(Lerp(std::log(std::max(0.01f, fstopA)), std::log(std::max(0.01f, fstopB)), t));
        // The lens numbers through the blend (an automatic lens
        // on one side takes the other's when it has one).
        r.intensity = Lerp(sharpA ? b.intensity : a.intensity, sharpB ? a.intensity : b.intensity, t);
        if (a.lensMm > 0.0f && b.lensMm > 0.0f)
            r.lensMm = Lerp(a.lensMm, b.lensMm, t);
        if (!distA && !distB)
        {
            // Nothing to focus on either side: the autofocus if a side asks
            // for it, else sharp.
            r.source = a.source == FocusSource::Engine || b.source == FocusSource::Engine ? FocusSource::Engine
                                                                                            : FocusSource::Sharp;
            out.plan = r;
            return out;
        }
        const float dA = distA ? a.distance : b.distance;
        const float dB = distB ? b.distance : a.distance;
        r.distance = std::max(kFocusMinMeters, std::min(kFocusMaxMeters, Lerp(dA, dB, t)));
        r.straightMeters = Lerp(distA ? a.straightMeters : b.straightMeters, distB ? b.straightMeters : a.straightMeters, t);
        if (!HasDistance(r.source))
        {
            const FocusPlan& other = HasDistance(a.source) ? a : b;
            r.source = other.source;
            r.entity = other.entity;
            r.point = other.point;
        }
        out.plan = r;
        return out;
    }

    float SmoothFocusDistance(FocusMemory& memory, float distance, float dtSeconds, float omega)
    {
        const float goal = std::log(std::max(kFocusMinMeters, std::min(kFocusMaxMeters, distance)));
        if (!memory.springPrimed || !(dtSeconds > 0.0f) || !(omega > 0.0f))
        {
            memory.springValue = goal;
            memory.springVelocity = 0.0f;
            memory.springPrimed = true;
            return std::exp(goal);
        }
        // The same exact critically damped step as SpringState::Step.
        const float decay = std::exp(-omega * dtSeconds);
        const float error = memory.springValue - goal;
        const float v0 = memory.springVelocity;
        const float c = v0 + omega * error;
        const float e = (error + c * dtSeconds) * decay;
        memory.springVelocity = (v0 - omega * c * dtSeconds) * decay;
        memory.springValue = goal + e;
        return std::exp(memory.springValue);
    }

    // ------------------------------------------------------------------
    // Attach and Look At targets, bones and the near clip.

    bool LookupTarget(const Scene& scene, std::uint32_t id, std::uint32_t bone, std::uint64_t timeMs, EntityState& state,
        bool* boneMissing)
    {
        if (boneMissing)
            *boneMissing = false;
        if (!id)
            return false;
        if (bone != 0 && bones::Valid(bone) && scene.boneLookup && scene.boneLookup(id, bone, timeMs, state))
            return true;
        if (bone != 0 && boneMissing)
            *boneMissing = true;
        return scene.lookup && scene.lookup(id, timeMs, state);
    }

    namespace
    {
        Mat3 FromQuaternion(const float q[4])
        {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            Mat3 r{};
            r.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
            r.m[0][1] = 2.0f * (x * y - w * z);
            r.m[0][2] = 2.0f * (x * z + w * y);
            r.m[1][0] = 2.0f * (x * y + w * z);
            r.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
            r.m[1][2] = 2.0f * (y * z - w * x);
            r.m[2][0] = 2.0f * (x * z - w * y);
            r.m[2][1] = 2.0f * (y * z + w * x);
            r.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
            return r;
        }

        bool UnitQuaternion(const float q[4], float out[4])
        {
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(n > 1e-6f) || !std::isfinite(n))
                return false;
            for (int i = 0; i < 4; ++i)
                out[i] = q[i] / n;
            return true;
        }
    }

    EntityState EntityFromBone(const editor::Vec3& rootPosition, const float rootQuaternion[4], const float boneTranslation[3],
        const float boneQuaternion[4])
    {
        float rq[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float bq[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        UnitQuaternion(rootQuaternion, rq);
        UnitQuaternion(boneQuaternion, bq);
        const Mat3 root = FromQuaternion(rq);
        const Mat3 world = Mul(root, FromQuaternion(bq));
        const Vec3 at = Add(rootPosition, Mul(root, Vec3{boneTranslation[0], boneTranslation[1], boneTranslation[2]}));
        const Vec3 euler = ToEuler(world);
        EntityState state;
        state.position = at;
        state.pitch = euler.x;
        state.roll = euler.y;
        state.heading = euler.z;
        state.hasHead = true;
        state.head = at;
        return state;
    }

    bool SetMarkerExtra(Keyframe& marker, const editor::CameraMarkerExtra& e)
    {
        const std::uint32_t f = e.fields;
        const auto has = [f](std::uint32_t bit) { return (f & bit) != 0; };
        const auto finite3 = [](const editor::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        const auto zeroOr = [](float value, float low, float high) {
            return std::isfinite(value) && (value == 0.0f || (value >= low && value <= high));
        };
        if ((has(editor::CameraExtraAttachBone) && !bones::Valid(e.attachBone)) ||
            (has(editor::CameraExtraLookAtBone) && !bones::Valid(e.lookAtBone)) ||
            (has(editor::CameraExtraNearClip) && !zeroOr(e.nearClip, editor::kNearClipMin, editor::kNearClipMax)) ||
            (has(editor::CameraExtraDofFStop) && !zeroOr(e.dofFStop, editor::kDofFStopMin, editor::kDofFStopMax)) ||
            (has(editor::CameraExtraDofLens) && !zeroOr(e.dofLensMm, editor::kDofLensMin, editor::kDofLensMax)) ||
            (has(editor::CameraExtraRelPosition) &&
                (!finite3(e.relPosition) || std::fabs(e.relPosition.x) > 1000.0f || std::fabs(e.relPosition.y) > 1000.0f ||
                    std::fabs(e.relPosition.z) > 1000.0f)) ||
            (has(editor::CameraExtraRelRotation) && !finite3(e.relRotation)))
            return false;
        // A bone needs its target (a bone on no target is dropped).
        if (has(editor::CameraExtraAttachBone))
            marker.attachBone = marker.attachTarget ? e.attachBone : 0;
        if (has(editor::CameraExtraLookAtBone))
            marker.lookAtBone = marker.lookAtTarget ? e.lookAtBone : 0;
        if (has(editor::CameraExtraRelPosition))
            marker.relPosition = e.relPosition;
        if (has(editor::CameraExtraRelRotation))
        {
            marker.relRotation = {WrapDegrees(e.relRotation.x), WrapDegrees(e.relRotation.y), WrapDegrees(e.relRotation.z)};
            marker.relRotation.x = std::max(-89.5f, std::min(89.5f, marker.relRotation.x)); // pitch limit
        }
        if (has(editor::CameraExtraNearClip))
            marker.nearClip = e.nearClip;
        if (has(editor::CameraExtraDofFStop))
            marker.dofFStop = e.dofFStop;
        if (has(editor::CameraExtraDofLens))
            marker.dofLensMm = e.dofLensMm;
        marker.editedMask |= editor::CameraOptionExtra;
        if (has(editor::CameraExtraAttachBone) || has(editor::CameraExtraLookAtBone) || has(editor::CameraExtraRelPosition) ||
            has(editor::CameraExtraRelRotation))
            ClearBorrowed(marker);
        return true;
    }

    void FillSnapshotExtra(const Keyframe& marker, editor::CameraMarkerExtra& out)
    {
        out = {};
        out.attachBone = marker.attachBone;
        out.lookAtBone = marker.lookAtBone;
        out.nearClip = marker.nearClip;
        out.dofFStop = marker.dofFStop;
        out.dofLensMm = marker.dofLensMm;
        out.relPosition = marker.relPosition;
        out.relRotation = marker.relRotation;
    }

    float MarkerNearClip(const Keyframe& marker)
    {
        return marker.nearClip > 0.0f
            ? std::max(editor::kNearClipMin, std::min(editor::kNearClipMax, marker.nearClip))
            : editor::kNearClipDefault;
    }

    float NearClipAt(const Track& track, const SegmentPlan& plan)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        const int count = static_cast<int>(keys.size());
        if (!plan.valid || plan.current < 0 || plan.current >= count)
            return editor::kNearClipDefault;
        const float a = MarkerNearClip(keys[plan.current]);
        if (!plan.blending || plan.next < 0 || plan.next >= count)
            return a;
        return Lerp(a, MarkerNearClip(keys[plan.next]), Clamp01(plan.phase));
    }

    float DofLensMm(float fovDegrees, float intensity, float lensOverride)
    {
        float lens = lensOverride;
        if (!(lens > 0.0f) || !std::isfinite(lens))
        {
            const float fov = std::max(5.0f, std::min(130.0f, std::isfinite(fovDegrees) ? fovDegrees : 45.0f));
            const float base = 12.0f / std::tan(fov * 0.5f * kDegreesToRadians);
            const float amount = std::max(0.0f, std::min(100.0f, std::isfinite(intensity) ? intensity : 50.0f)) / 100.0f;
            lens = base * Lerp(kDofLensMultiplierMin, kDofLensMultiplierMax, amount);
        }
        return std::max(editor::kDofLensMin, std::min(editor::kDofLensMax, lens));
    }

    void InFocusRange(float fstop, float lensMm, float focusMeters, float& nearMeters, float& farMeters)
    {
        nearMeters = farMeters = 0.0f;
        if (!(fstop > 0.0f) || !(lensMm > 0.0f) || !(focusMeters > 0.0f))
            return;
        constexpr float kCircleMm = 0.03f;
        const double f = lensMm;
        const double s = static_cast<double>(focusMeters) * 1000.0;
        const double hyperMm = f * f / (static_cast<double>(fstop) * kCircleMm) + f;
        nearMeters = static_cast<float>(s * (hyperMm - f) / (hyperMm + s - 2.0 * f) / 1000.0);
        farMeters = s < hyperMm ? static_cast<float>(s * (hyperMm - f) / (hyperMm - s) / 1000.0) : 0.0f;
    }

    EntityState EntityFromRecorded(const editor::Vec3& position, const float q[4], const editor::Vec3* head)
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        Mat3 r{};
        r.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
        r.m[0][1] = 2.0f * (x * y - w * z);
        r.m[0][2] = 2.0f * (x * z + w * y);
        r.m[1][0] = 2.0f * (x * y + w * z);
        r.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
        r.m[1][2] = 2.0f * (y * z - w * x);
        r.m[2][0] = 2.0f * (x * z - w * y);
        r.m[2][1] = 2.0f * (y * z + w * x);
        r.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
        const Vec3 euler = ToEuler(r);
        EntityState state;
        state.position = position;
        state.pitch = euler.x;
        state.roll = euler.y;
        state.heading = euler.z;
        state.hasHead = head != nullptr;
        if (head)
            state.head = *head;
        return state;
    }

    PrefetchTarget PlanPrefetch(const Track& track, const SegmentPlan& plan, std::uint64_t timeMs,
        const editor::CameraState& camera, const Scene& scene, std::uint32_t leadMs, float minMeters)
    {
        PrefetchTarget out;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (!plan.valid || plan.blending || plan.next < 0 || plan.next >= static_cast<int>(keys.size()))
            return out;
        const Keyframe& next = keys[plan.next];
        if (next.timeMs <= timeMs || next.timeMs - timeMs > leadMs)
            return out;
        const LivePose live = Resolve(next, next.timeMs, scene);
        const Vec3 at = live.pose.position;
        const Vec3 delta = Sub(at, camera.position);
        const float meters = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (!(meters >= minMeters))
            return out;
        out.want = true;
        out.marker = plan.next;
        out.cutMs = next.timeMs;
        out.meters = meters;
        out.view = {at, Track::ToEulerDegrees(live.pose.orientation), live.pose.fov};
        return out;
    }
}

// ----------------------------------------------------------------------
// Marker tools.
namespace replay::camera
{
    bool Track::Retime(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& times, std::uint64_t limitMs, bool pushAfter)
    {
        if (times.empty())
            return false;
        const std::vector<Keyframe> before = keyframes_;
        std::size_t lastIndex = 0;
        for (const auto& entry : times)
        {
            const std::size_t index = IndexOf(entry.first);
            if (index >= keyframes_.size())
            {
                keyframes_ = before;
                return false;
            }
            keyframes_[index].timeMs = entry.second;
            lastIndex = std::max(lastIndex, index);
        }
        bool ok = !pushAfter || KeepSpacingAfter(lastIndex, limitMs);
        for (std::size_t n = 1; ok && n < keyframes_.size(); ++n)
        {
            const Keyframe& previous = keyframes_[n - 1];
            if (keyframes_[n].timeMs < previous.timeMs + MarkerSpacingMs(previous.speedIndex))
                ok = false;
        }
        for (std::size_t n = 0; ok && n < keyframes_.size(); ++n)
        {
            if (keyframes_[n].timeMs > limitMs && keyframes_[n].timeMs != before[n].timeMs)
                ok = false;
        }
        if (!ok)
        {
            keyframes_ = before;
            return false;
        }
        ApplyLastMarkerRule(*this);
        return true;
    }

    std::uint32_t Track::AddCopy(const Keyframe& marker)
    {
        Keyframe copy = marker;
        ClearBorrowed(copy);
        const std::uint32_t id = AddMarker(copy);
        if (id)
            ApplyLastMarkerRule(*this);
        return id;
    }

    namespace
    {
        bool Usable(const Keyframe& k) { return k.markerType != MarkerType::Anchor; }

        // Usable markers a and b (a before b) are joined in one move.
        bool Joined(const Keyframe& a, const Keyframe& b, const std::vector<std::uint32_t>* cuts)
        {
            if (a.blend == Blend::None || a.cameraMode != CameraMode::Free || b.cameraMode != CameraMode::Free)
                return false;
            if (cuts)
                for (const std::uint32_t cut : *cuts)
                    if (cut > a.timeMs && cut <= b.timeMs)
                        return false;
            return true;
        }
    }

    MoveRange MoveOf(const Track& track, std::uint32_t id, const std::vector<std::uint32_t>* cuts)
    {
        MoveRange range;
        const std::vector<Keyframe>& keys = track.Keyframes();
        int at = -1;
        for (std::size_t n = 0; n < keys.size(); ++n)
            if (keys[n].id == id)
                at = static_cast<int>(n);
        if (at < 0)
            return range;
        const auto previousUsable = [&](int from) {
            for (int n = from - 1; n >= 0; --n)
                if (Usable(keys[n]))
                    return n;
            return -1;
        };
        const auto nextUsable = [&](int from) {
            for (int n = from + 1; n < static_cast<int>(keys.size()); ++n)
                if (Usable(keys[n]))
                    return n;
            return -1;
        };
        // An Anchor belongs to the move around it: start from the usable
        // marker at or before it.
        int start = Usable(keys[at]) ? at : previousUsable(at);
        if (start < 0)
            start = nextUsable(at);
        if (start < 0)
            return range;
        range.first = range.last = start;
        for (int n = previousUsable(range.first); n >= 0 && Joined(keys[n], keys[range.first], cuts); n = previousUsable(n))
            range.first = n;
        for (int n = nextUsable(range.last); n >= 0 && Joined(keys[range.last], keys[n], cuts); n = nextUsable(n))
            range.last = n;
        return range;
    }

    std::uint32_t DuplicateMarker(Track& track, std::uint32_t id, std::uint64_t limitMs)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        std::size_t index = keys.size();
        for (std::size_t n = 0; n < keys.size(); ++n)
            if (keys[n].id == id)
                index = n;
        if (index >= keys.size())
            return 0;
        const Keyframe source = keys[index];
        std::uint64_t at = 0;
        if (index + 1 < keys.size())
            at = source.timeMs + (keys[index + 1].timeMs - source.timeMs) / 2;
        else
        {
            at = source.timeMs + kDuplicateAfterLastMs;
            if (at > limitMs)
                return 0;
        }
        if (!WithinClipBounds(track, at))
            return 0;
        Keyframe copy = source;
        copy.timeMs = at;
        copy.transition = 0; // a fade belongs to the clip's first or last marker
        return track.AddCopy(copy);
    }

    namespace
    {
        // The inner times of a run: first and last kept, the weights between
        // consecutive markers share the span.
        std::vector<std::uint64_t> Spread(std::uint64_t first, std::uint64_t last, const std::vector<double>& weights)
        {
            std::vector<std::uint64_t> out;
            double total = 0.0;
            for (const double w : weights)
                total += w;
            if (!(total > 0.0))
                return out;
            double sum = 0.0;
            for (std::size_t n = 0; n + 1 < weights.size(); ++n)
            {
                sum += weights[n];
                out.push_back(first + static_cast<std::uint64_t>(std::llround(static_cast<double>(last - first) * sum / total)));
            }
            return out;
        }
    }

    bool DistributeMove(Track& track, std::uint32_t id, bool byDistance, const Scene& scene)
    {
        const MoveRange range = MoveOf(track, id, scene.cuts);
        if (!range.Valid())
            return false;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (range.last - range.first < 2)
            return false;
        // One weight per gap between consecutive markers (Anchors included:
        // an Anchor sits on the path where its time puts it, so it splits
        // the distance of the gap it is in evenly).
        std::vector<double> weights;
        std::vector<Vec3> at;
        for (int n = range.first; n <= range.last; ++n)
            at.push_back(Usable(keys[n]) ? Resolve(keys[n], keys[n].timeMs, scene).pose.position : Vec3{});
        for (int n = range.first; n < range.last; ++n)
        {
            double w = 1.0;
            if (byDistance)
            {
                // From the usable marker at or before n to the next usable one.
                int a = n, b = n + 1;
                while (a > range.first && !Usable(keys[a]))
                    --a;
                while (b < range.last && !Usable(keys[b]))
                    ++b;
                int inGap = 0;
                for (int m = a; m < b; ++m)
                    ++inGap;
                const Vec3 d = Sub(at[b - range.first], at[a - range.first]);
                w = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y + static_cast<double>(d.z) * d.z) /
                    std::max(1, inGap);
            }
            weights.push_back(w);
        }
        double total = 0.0;
        for (const double w : weights)
            total += w;
        if (!(total > 0.001))
            std::fill(weights.begin(), weights.end(), 1.0); // a move that stands still: even times
        const std::vector<std::uint64_t> inner = Spread(keys[range.first].timeMs, keys[range.last].timeMs, weights);
        std::vector<std::pair<std::uint32_t, std::uint64_t>> times;
        for (int n = range.first + 1; n < range.last; ++n)
            times.push_back({keys[n].id, inner[static_cast<std::size_t>(n - range.first - 1)]});
        return track.Retime(times);
    }

    bool StretchMove(Track& track, std::uint32_t id, std::uint32_t percent, std::uint64_t limitMs, const Scene& scene)
    {
        if (percent < 25 || percent > 400)
            return false;
        const MoveRange range = MoveOf(track, id, scene.cuts);
        if (!range.Valid() || range.last == range.first)
            return false;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (IsClipBound(track, keys[range.last].id))
            return false;
        const int last = range.last;
        const std::uint64_t origin = keys[range.first].timeMs;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> times;
        for (int n = range.first + 1; n <= last; ++n)
        {
            const std::uint64_t offset = keys[n].timeMs - origin;
            times.push_back({keys[n].id, origin + (offset * percent + 50) / 100});
        }
        return track.Retime(times, limitMs, true);
    }

    int LockMarkers(Track& track, std::uint32_t id, bool everyMarker, std::uint32_t target, AttachType type, const Scene& scene)
    {
        std::vector<std::uint32_t> ids;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (everyMarker)
        {
            for (const Keyframe& k : keys)
                ids.push_back(k.id);
        }
        else
        {
            const MoveRange range = MoveOf(track, id, scene.cuts);
            if (!range.Valid())
                return 0;
            for (int n = range.first; n <= range.last; ++n)
                ids.push_back(keys[n].id);
        }
        int changed = 0;
        for (const std::uint32_t markerId : ids)
        {
            Keyframe* marker = track.FindMutable(markerId);
            if (!marker || marker->cameraMode != CameraMode::Free || marker->markerType == MarkerType::Anchor)
                continue;
            if (marker->attachTarget == target && (target == 0 || marker->attachType == type))
                continue;
            EntityState state;
            if (target != 0 && !(scene.lookup && scene.lookup(target, marker->timeMs, state)))
                continue; // the target is not in the clip at that marker: left as it was
            const LivePose live = Resolve(*marker, marker->timeMs, scene);
            const editor::CameraState view{live.pose.position, Track::ToEulerDegrees(live.pose.orientation), live.pose.fov};
            SetAttach(*marker, target);
            marker->attachBone = 0; // a lock is to the character's root
            if (target != 0)
                SetAttachType(*marker, type);
            SetMarkerView(*marker, view, scene);
            marker->editedMask |= editor::CameraOptionAttachTarget | editor::CameraOptionAttachType;
            ++changed;
        }
        if (changed)
            ApplyLastMarkerRule(track);
        return changed;
    }
}
