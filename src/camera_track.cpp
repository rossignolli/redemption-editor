#include "camera_track_internal.h"

namespace
{
    constexpr float kRadiansToDegrees = 180.0f / kPi;
    constexpr std::uint64_t kReplaceToleranceMs = 1;
}

    float Clamp01(float value)
    {
        return std::max(0.0f, std::min(1.0f, value));
    }

namespace
{

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

namespace
{

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

namespace
{
    Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
}
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

namespace
{

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

    bool InsideBlend(const Track& track, std::uint64_t timeMs)
    {
        const SegmentPlan plan = Plan(track, timeMs);
        return plan.valid && plan.blending && plan.next >= 0 && track.Keyframes()[static_cast<std::size_t>(plan.next)].timeMs > timeMs;
    }

    bool InheritFrom(Keyframe& fresh, const Keyframe& previous, bool insideBlend)
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
        if (previous.cameraMode == CameraMode::Free && insideBlend)
        {
            fresh.blend = previous.blend;
            fresh.ease = previous.ease;
        }
        else
        {
            // No Blend (a cut) is the default blend.
            fresh.blend = Blend::None;
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
    }

        void ClearBorrowed(Keyframe& k)
        {
            k.editedMask &= ~(kEditedBorrowedView | kBorrowHashMask);
        }

    namespace
    {

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

    namespace
    {

    // ------------------------------------------------------------------
    // Evaluator.

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
}
