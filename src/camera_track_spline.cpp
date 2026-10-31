// camera_track_spline.cpp: the markers' evaluation: the spline runs and their arc tables, the curve
// speed, the shake signature and coupling, and EvaluateMarkers.
// Split from camera_track.cpp (moved code); shared internals in camera_track_internal.h.

#include "camera_track_internal.h"

namespace replay::camera
{

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
        // around t, without springs or shake: scrub equals play. The window
        // never spans a jump: a marker reached without a blend or a
        // recording cut ends it on t's side, else a direct cut a few metres
        // long reads as a fast move and the shake swells (or, with Stop When
        // Still, starts) in the 40 ms before and after the cut.
        float CurveSpeed(const Track& track, const Scene& scene, std::int64_t at)
        {
            const std::uint64_t now = static_cast<std::uint64_t>(std::max<std::int64_t>(0, at));
            std::uint64_t from = static_cast<std::uint64_t>(std::max<std::int64_t>(0, at - kShakeMotionProbeMs));
            std::uint64_t to = static_cast<std::uint64_t>(std::max<std::int64_t>(0, at + kShakeMotionProbeMs));
            const auto cutAt = [&](std::uint64_t jump) {
                if (now < jump)
                    to = std::min(to, jump - 1);
                else
                    from = std::max(from, jump);
            };
            if (scene.cuts)
                for (const std::uint32_t cut : *scene.cuts)
                    if (cut > from && cut <= to)
                        cutAt(cut);
            editor::CameraState a{}, b{};
            SegmentPlan planA, planB;
            // A few markers closer than the window: one jump at a time.
            for (int pass = 0; pass < 4; ++pass)
            {
                if (to <= from || !EvaluateMarkersImpl(track, from, scene, a, nullptr, 0.0f, &planA, false) ||
                    !EvaluateMarkersImpl(track, to, scene, b, nullptr, 0.0f, &planB, false))
                    return 0.0f;
                if (planA.current == planB.current || planA.blending || planB.current < 0)
                    break;
                cutAt(track.Keyframes()[static_cast<std::size_t>(planB.current)].timeMs);
            }
            if (to <= from)
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
                cache.segment.resize(static_cast<std::size_t>(n) + 1, -1);
                cache.blends.resize(static_cast<std::size_t>(n) + 1, 0);
            }
            if (!cache.known[n])
            {
                editor::CameraState view{};
                SegmentPlan plan;
                EvaluateMarkersImpl(track, runStartMs + n * kShakePhaseGridMs, scene, view, nullptr, 0.0f, &plan, false);
                cache.points[n] = view.position;
                cache.segment[n] = plan.current;
                cache.blends[n] = plan.blending ? 1 : 0;
                cache.known[n] = 1;
            }
            return cache.points[n];
        };
        // A cell across a jump (a marker reached without a blend, a
        // recording cut) is not a move: it adds no coupling (CurveSpeed).
        const auto jumps = [&](std::uint64_t n) {
            point(n);
            point(n + 1);
            if (cache.segment[n] != cache.segment[n + 1] && !cache.blends[n])
                return true;
            const std::uint64_t a = runStartMs + n * kShakePhaseGridMs, b = a + kShakePhaseGridMs;
            if (scene.cuts)
                for (const std::uint32_t cut : *scene.cuts)
                    if (cut > a && cut <= b)
                        return true;
            return false;
        };
        // One cell's coupling rate (cycles per second): hz x speedHz x the
        // normalised speed over the cell.
        const auto cellRate = [&](std::uint64_t n) {
            float rate = 0.0f, speedRef = 30.0f;
            CouplingAt(track, source, runStartMs + n * kShakePhaseGridMs, rate, speedRef);
            if (rate == 0.0f || jumps(n))
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
