#include "camera_overlay.h"

#include <algorithm>
#include <cmath>

namespace replay::camera::overlay
{
    namespace
    {
        using editor::Vec2;
        using editor::Vec3;
        namespace oc = replay::overlaychannel;

        constexpr float kDeg = 3.14159265358979323846f / 180.0f;

        Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Length(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

        // The engine's direction of a rotation (script.cpp RotationToDirection).
        Vec3 Direction(const Vec3& rotation)
        {
            const float p = rotation.x * kDeg, h = rotation.z * kDeg;
            const float horizontal = std::fabs(std::cos(p));
            return {-std::sin(h) * horizontal, std::cos(h) * horizontal, std::sin(p)};
        }

        bool Blends(const Keyframe& a, const Keyframe& b, const Scene& scene)
        {
            if (a.blend == Blend::None || a.cameraMode != CameraMode::Free || b.cameraMode != CameraMode::Free)
                return false;
            if (scene.cuts)
                for (const std::uint32_t cut : *scene.cuts)
                    if (cut > a.timeMs && cut <= b.timeMs)
                        return false;
            return true;
        }

        // Screen units of the picture's height: y -1..1 bottom to top, x as
        // far (-aspect..aspect left to right). The add-on scales by the
        // display's height, so the probe needs no aspect (the engine's
        // screen resolution native gives a virtual one).
        Vec2 ScreenOf(const Vec3& local, float tanV)
        {
            return {local.x / local.y / tanV, local.z / local.y / tanV};
        }
    }

    WorldPath BuildPath(const Track& track, const Scene& scene)
    {
        WorldPath path;
        const std::vector<Keyframe>& keys = track.Keyframes();
        for (const Keyframe& k : keys)
        {
            MarkerPoint m;
            m.id = k.id;
            m.timeMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(k.timeMs, 0xFFFFFFFFull));
            if (k.markerType != MarkerType::Anchor)
            {
                const LivePose live = Resolve(k, k.timeMs, scene);
                ++path.evaluations;
                m.at = live.pose.position;
                m.forward = Direction(Track::ToEulerDegrees(live.pose.orientation));
            }
            if (k.attachTarget != 0)
                m.flags |= oc::FlagAttached;
            if (k.markerType == MarkerType::Anchor)
                m.flags |= oc::FlagAnchor;
            if (k.cameraMode != CameraMode::Free)
                m.flags |= oc::FlagNotFree;
            if (IsClipBound(track, k.id))
                m.flags |= oc::FlagClipBound;
            if (k.markerType != MarkerType::Anchor)
                path.markers.push_back(m);
        }

        // The segments that blend (Free to Free, no cut), in time order.
        std::vector<std::pair<const Keyframe*, const Keyframe*>> segments;
        const Keyframe* previous = nullptr;
        std::uint64_t blendedMs = 0;
        for (const Keyframe& k : keys)
        {
            if (k.markerType == MarkerType::Anchor)
                continue;
            if (previous && Blends(*previous, k, scene) && k.timeMs > previous->timeMs)
            {
                segments.push_back({previous, &k});
                blendedMs += k.timeMs - previous->timeMs;
            }
            previous = &k;
        }
        if (segments.empty())
            return path;
        const std::uint64_t wanted = blendedMs / kMaxPathPoints + 1;
        const std::uint64_t step = std::min<std::uint64_t>(kMaxSampleMs, std::max<std::uint64_t>(kMinSampleMs, wanted));
        const Keyframe* lastEnd = nullptr;
        for (const auto& segment : segments)
        {
            const Keyframe& a = *segment.first;
            const Keyframe& b = *segment.second;
            bool first = true;
            for (std::uint64_t t = a.timeMs;; t += step)
            {
                const std::uint64_t at = std::min(t, b.timeMs);
                editor::CameraState view{};
                if (path.points.size() >= kMaxPathPoints * 2)
                    break;
                ++path.evaluations;
                if (EvaluateMarkersRaw(track, at, scene, view))
                {
                    PathPoint point;
                    point.at = view.position;
                    point.timeMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(at, 0xFFFFFFFFull));
                    point.sourceId = a.id;
                    point.spline = a.blend == Blend::Spline;
                    // A segment joins the one before when it starts where
                    // that one ended (a chain of blends).
                    point.breakBefore = first && lastEnd != &a;
                    path.points.push_back(point);
                    first = false;
                }
                if (at >= b.timeMs)
                    break;
            }
            lastEnd = &b;
        }
        return path;
    }

    std::vector<TrailPoint> BuildTrail(const EntityLookup& lookup, std::uint32_t target, std::uint64_t fromMs,
        std::uint64_t toMs, std::uint32_t stepMs)
    {
        std::vector<TrailPoint> trail;
        if (!lookup || target == 0 || toMs <= fromMs || stepMs == 0)
            return trail;
        bool missing = true;
        for (std::uint64_t t = fromMs; t <= toMs; t += stepMs)
        {
            EntityState state;
            if (!lookup(target, t, state))
            {
                missing = true;
                continue;
            }
            TrailPoint point;
            point.at = state.hasHead ? state.head : Vec3{state.position.x, state.position.y, state.position.z + 0.6f};
            point.timeMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(t, 0xFFFFFFFFull));
            point.breakBefore = missing;
            missing = false;
            trail.push_back(point);
        }
        return trail;
    }

    Vec3 ToCamera(const editor::CameraState& camera, const Vec3& point)
    {
        // Columns of Rz(heading) Rx(pitch) Ry(roll): the camera's right,
        // forward and up (camera_track.cpp FromEuler); the transpose takes
        // a world offset into camera space.
        const float p = camera.rotation.x * kDeg, r = camera.rotation.y * kDeg, h = camera.rotation.z * kDeg;
        const float cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r), ch = std::cos(h), sh = std::sin(h);
        const Vec3 right{ch * cr - sh * sp * sr, sh * cr + ch * sp * sr, -cp * sr};
        const Vec3 forward{-sh * cp, ch * cp, sp};
        const Vec3 up{ch * sr + sh * sp * cr, sh * sr - ch * sp * cr, cp * cr};
        const Vec3 d = Sub(point, camera.position);
        return {d.x * right.x + d.y * right.y + d.z * right.z, d.x * forward.x + d.y * forward.y + d.z * forward.z,
                d.x * up.x + d.y * up.y + d.z * up.z};
    }

    bool ProjectSegment(const editor::CameraState& camera, const Vec3& a, const Vec3& b, Vec2& sa, Vec2& sb)
    {
        Vec3 la = ToCamera(camera, a);
        Vec3 lb = ToCamera(camera, b);
        if (la.y < kNearMeters && lb.y < kNearMeters)
            return false;
        if (std::min(Length(la), Length(lb)) > kMaxDrawMeters)
            return false;
        // Clip the part behind the near plane.
        if (la.y < kNearMeters || lb.y < kNearMeters)
        {
            const float u = (kNearMeters - la.y) / (lb.y - la.y);
            const Vec3 cut = Add(la, Scale(Sub(lb, la), u));
            if (la.y < kNearMeters)
                la = cut;
            else
                lb = cut;
        }
        const float tanV = std::tan(camera.fov * 0.5f * kDeg);
        sa = ScreenOf(la, tanV);
        sb = ScreenOf(lb, tanV);
        // Far outside the screen on the same side: nothing to draw.
        const float wide = 4.0f * kMaxAspect;
        if ((sa.x < -wide && sb.x < -wide) || (sa.x > wide && sb.x > wide) || (sa.y < -4.0f && sb.y < -4.0f) ||
            (sa.y > 4.0f && sb.y > 4.0f))
            return false;
        return true;
    }

    ProjectReport Project(const WorldPath& path, const std::vector<TrailPoint>& trail, const editor::CameraState& camera,
        const ProjectSettings& settings, overlaychannel::Frame& frame)
    {
        ProjectReport report;
        const float tanV = std::tan(camera.fov * 0.5f * kDeg);
        const auto point = [&](const Vec3& world, Vec2& screen, float& meters) {
            const Vec3 local = ToCamera(camera, world);
            meters = Length(local);
            if (local.y < kNearMeters || meters > kMaxDrawMeters)
                return false;
            screen = ScreenOf(local, tanV);
            return std::fabs(screen.x) < 1.2f * kMaxAspect && std::fabs(screen.y) < 1.2f;
        };
        const auto line = [&](const oc::Line& l) { oc::AddLine(frame, l) ? ++report.lines : ++report.dropped; };
        const auto dot = [&](const oc::Dot& d) { oc::AddDot(frame, d) ? ++report.dots : ++report.dropped; };
        const auto label = [&](const oc::Label& l) { oc::AddLabel(frame, l) ? ++report.labels : ++report.dropped; };

        // The path.
        for (std::size_t n = 1; n < path.points.size(); ++n)
        {
            const PathPoint& a = path.points[n - 1];
            const PathPoint& b = path.points[n];
            if (b.breakBefore)
                continue;
            Vec2 sa{}, sb{};
            if (!ProjectSegment(camera, a.at, b.at, sa, sb))
                continue;
            std::uint16_t flags = b.spline ? oc::FlagSpline : 0;
            if (b.timeMs <= settings.playheadMs)
                flags |= oc::FlagPast;
            line({sa.x, sa.y, sb.x, sb.y, static_cast<std::uint16_t>(oc::LineKind::Path), flags, b.sourceId});
        }
        // Time labels where the path crosses each labelEveryMs.
        if (settings.labelEveryMs)
        {
            for (std::size_t n = 0; n < path.points.size(); ++n)
            {
                const PathPoint& p = path.points[n];
                const std::uint32_t slot = p.timeMs / settings.labelEveryMs;
                const bool crosses = n == 0 || p.breakBefore || slot != path.points[n - 1].timeMs / settings.labelEveryMs;
                if (!crosses || p.timeMs % settings.labelEveryMs > kMaxSampleMs)
                    continue;
                Vec2 s{};
                float meters = 0.0f;
                if (point(p.at, s, meters))
                    label({s.x, s.y, slot * settings.labelEveryMs, static_cast<std::uint16_t>(oc::LabelKind::PathTime),
                           static_cast<std::uint16_t>(p.timeMs <= settings.playheadMs ? oc::FlagPast : 0)});
            }
        }
        // The markers: a dot and the look arrow.
        for (const MarkerPoint& m : path.markers)
        {
            std::uint16_t flags = m.flags;
            if (m.id == settings.currentId)
                flags |= oc::FlagCurrent;
            if (m.id == settings.editingId)
                flags |= oc::FlagEditing;
            if (m.timeMs <= settings.playheadMs && m.id != settings.currentId)
                flags |= oc::FlagPast;
            Vec2 s{};
            float meters = 0.0f;
            if (point(m.at, s, meters))
                dot({s.x, s.y, meters, static_cast<std::uint16_t>(oc::DotKind::Marker), flags, m.id, m.timeMs});
            Vec2 sa{}, sb{};
            if (!(flags & oc::FlagNotFree) && ProjectSegment(camera, m.at, Add(m.at, Scale(m.forward, kArrowMeters)), sa, sb))
                line({sa.x, sa.y, sb.x, sb.y, static_cast<std::uint16_t>(oc::LineKind::Arrow), flags, m.id});
        }
        // The marker camera at the playhead (seen from the free camera).
        if (settings.showCamera)
        {
            Vec2 s{};
            float meters = 0.0f;
            if (point(settings.cameraAt, s, meters))
                dot({s.x, s.y, meters, static_cast<std::uint16_t>(oc::DotKind::Camera), 0,
                     0, static_cast<std::uint32_t>(std::min<std::uint64_t>(settings.playheadMs, 0xFFFFFFFFull))});
        }
        // The target's trail, a tick and a label each second, and the target.
        for (std::size_t n = 0; n < trail.size(); ++n)
        {
            const TrailPoint& p = trail[n];
            if (n > 0 && !p.breakBefore)
            {
                Vec2 sa{}, sb{};
                if (ProjectSegment(camera, trail[n - 1].at, p.at, sa, sb))
                    line({sa.x, sa.y, sb.x, sb.y, static_cast<std::uint16_t>(oc::LineKind::Trail),
                          static_cast<std::uint16_t>(p.timeMs <= settings.playheadMs ? oc::FlagPast : 0), settings.targetId});
            }
            if (p.timeMs % 1000 == 0 || (n > 0 && p.timeMs / 1000 != trail[n - 1].timeMs / 1000))
            {
                Vec2 s{};
                float meters = 0.0f;
                if (point(p.at, s, meters))
                {
                    const std::uint16_t past = static_cast<std::uint16_t>(p.timeMs <= settings.playheadMs ? oc::FlagPast : 0);
                    dot({s.x, s.y, meters, static_cast<std::uint16_t>(oc::DotKind::TrailTick), past, settings.targetId, p.timeMs});
                    if (settings.labelEveryMs)
                        label({s.x, s.y, p.timeMs / 1000 * 1000, static_cast<std::uint16_t>(oc::LabelKind::TrailTime), past});
                }
            }
        }
        if (settings.targetValid)
        {
            Vec2 s{};
            float meters = 0.0f;
            if (point(settings.targetAt, s, meters))
                dot({s.x, s.y, meters, static_cast<std::uint16_t>(oc::DotKind::Target), 0, settings.targetId, 0});
        }
        frame.flags = settings.dim ? oc::FrameDim : 0u;
        frame.playheadMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(settings.playheadMs, 0xFFFFFFFFull));
        return report;
    }
}
