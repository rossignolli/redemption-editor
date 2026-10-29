// The Spline blend with REPlus's options. The zoom as a monotone cubic, the
// natural end, the numeric ease on time, the three pacings, the curve shape,
// the camera weight, the channels, the per marker overrides, CAMR v7 and the
// tools' text.
#include "camera_spline.h"
#include "camera_track.h"
#include "clip_file.h"
#include "old_camera_payload.h"

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
    using editor::SplinePacing;
    using editor::SplineTuning;
    using editor::Vec3;
    namespace sp = camera::spline;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL (camera-spline): %s\n", message);
        ++g_failures;
    }

    float Distance(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    Keyframe FreeAt(std::uint64_t t, const Vec3& at, float heading = 0.0f, float fov = 45.0f)
    {
        Keyframe k;
        k.timeMs = t;
        k.cameraMode = CameraMode::Free;
        k.pose = {at, Track::FromEulerDegrees({0.0f, 0.0f, heading}), fov};
        k.blend = Blend::Spline;
        return k;
    }

    Track Chain(const std::vector<std::uint64_t>& times, const std::vector<Vec3>& points, const SplineTuning* tuning = nullptr)
    {
        Track track;
        for (std::size_t n = 0; n < times.size(); ++n)
        {
            Keyframe k = FreeAt(times[n], points[n]);
            if (tuning)
                k.spline = *tuning;
            track.AddMarker(k);
        }
        return track;
    }

    CameraState View(const Track& track, std::uint64_t t, const camera::Scene& scene = {})
    {
        CameraState view{};
        camera::EvaluateMarkersRaw(track, t, scene, view);
        return view;
    }

    Vec3 At(const Track& track, std::uint64_t t, const camera::Scene& scene = {})
    {
        return View(track, t, scene).position;
    }

    float Speed(const Track& track, std::uint64_t t, int side, const camera::Scene& scene = {})
    {
        const std::uint64_t a = side < 0 ? t - 6 : t + 1;
        const std::uint64_t b = side < 0 ? t - 1 : t + 6;
        return Distance(At(track, a, scene), At(track, b, scene)) / (static_cast<float>(b - a) / 1000.0f);
    }

    bool SameBits(const CameraState& a, const CameraState& b)
    {
        return std::memcmp(&a, &b, sizeof(CameraState)) == 0;
    }

    SplineTuning With(std::uint8_t overrides, float alpha, float easeIn, float easeOut, float weight, SplinePacing pacing,
        std::uint8_t channels = editor::SplineChannelAll)
    {
        SplineTuning t = editor::DefaultSplineTuning();
        t.alpha = alpha;
        t.easeIn = easeIn;
        t.easeOut = easeOut;
        t.weight = weight;
        t.pacing = static_cast<std::uint8_t>(pacing);
        t.channels = channels;
        t.overrides = overrides;
        return t;
    }

    // ---- 1. The scalar pieces.
    void TestMath()
    {
        Check(sp::Ease(0.37, 0.0, 0.0) == 0.37, "ease: none is the identity");
        Check(std::fabs(sp::Ease(0.0, 0.3, 0.3)) < 1e-12 && std::fabs(sp::Ease(1.0, 0.3, 0.3) - 1.0) < 1e-12, "ease: 0 and 1 kept");
        double previous = 0.0;
        bool monotone = true;
        for (int n = 1; n <= 1000; ++n)
        {
            const double v = sp::Ease(n / 1000.0, 0.25, 0.4);
            monotone = monotone && v >= previous;
            previous = v;
        }
        Check(monotone, "ease: never goes back");
        // Top speed: 1 / (1 - (in + out) / 2); slope 0 at the start.
        const double top = (sp::Ease(0.5 + 1e-6, 0.25, 0.25) - sp::Ease(0.5 - 1e-6, 0.25, 0.25)) / 2e-6;
        Check(std::fabs(top - 1.0 / 0.75) < 1e-4, "ease: the top speed arrives on time");
        Check(sp::Ease(1e-4, 0.3, 0.0) < 1e-6, "ease: starts at rest");
        Check(std::fabs(sp::Ease(0.5, 0.8, 0.8) - 0.5) < 1e-9 && std::fabs(sp::Ease(1.0, 0.8, 0.8) - 1.0) < 1e-12,
            "ease: in + out over 1 scaled down");

        Check(sp::MonotoneInner(1.0, 1.0, 1.0, -1.0) == 0.0, "monotone: 0 at an extremum");
        const double m = sp::MonotoneInner(1.0, 2.0, 1.0, 3.0);
        Check(m > 1.0 && m < 3.0, "monotone: between the secants");
        Check(sp::MonotoneEnd(1.0, 1.0, 1.0, 5.0) == 0.0, "monotone end: held at 0 (no reverse)");

        double knots[6] = {-2, -1, 0, 1, 2, 3};
        double w[4];
        sp::BSplineWeights(knots, 0.0, w);
        Check(std::fabs(w[0] - 1.0 / 6.0) < 1e-12 && std::fabs(w[1] - 4.0 / 6.0) < 1e-12 && std::fabs(w[2] - 1.0 / 6.0) < 1e-12 &&
                  std::fabs(w[3]) < 1e-12,
            "b-spline: the uniform weights at a knot (1, 4, 1) / 6");
        const double uneven[6] = {-3.0, -1.0, 0.0, 0.4, 2.0, 2.5};
        sp::BSplineWeights(uneven, 0.17, w);
        Check(std::fabs(w[0] + w[1] + w[2] + w[3] - 1.0) < 1e-12 && w[0] >= 0 && w[1] >= 0 && w[2] >= 0 && w[3] >= 0,
            "b-spline: weights sum to 1, none negative");

        // Effective: the overridden fields over the defaults.
        SplineTuning defaults = With(0, 0.5f, 0.1f, 0.2f, 0.3f, SplinePacing::Continuous, editor::SplineChannelAll);
        SplineTuning own = With(editor::SplineOverrideEaseOut | editor::SplineOverrideRotation, 1.0f, 0.9f, 0.6f, 1.0f,
            SplinePacing::PerSegment, editor::SplineChannelPosition);
        const SplineTuning e = sp::Effective(own, defaults);
        Check(e.alpha == 0.5f && e.easeIn == 0.1f && e.easeOut == 0.6f && e.weight == 0.3f &&
                  e.pacing == static_cast<std::uint8_t>(SplinePacing::Continuous) &&
                  e.channels == (editor::SplineChannelPosition | editor::SplineChannelFov),
            "effective: the marker's fields over the defaults, per channel");

        // The tools' text.
        SplineTuning parsed = editor::DefaultSplineTuning();
        bool known = false;
        std::string error;
        Check(sp::ParseField("pacing", "continuous", parsed, known, error) && sp::ParseField("alpha", "centripetal", parsed, known, error) &&
                  sp::ParseField("ease_in", "0.25", parsed, known, error) && sp::ParseField("weight", "0.8", parsed, known, error) &&
                  sp::ParseField("sp_fov", "0", parsed, known, error),
            "parse: the keys");
        Check(!sp::ParseField("alpha", "1.5", parsed, known, error) && known, "parse: alpha out of range refused");
        Check(!sp::ParseField("tension", "1", parsed, known, error) && !known, "parse: an unknown key");
        Check(sp::Describe(parsed) == "pacing=continuous alpha=0.5 ease_in=0.25 weight=0.8 sp_fov=0", "describe: the overridden fields");
        Check(sp::ParseField("weight", "global", parsed, known, error) && !(parsed.overrides & editor::SplineOverrideWeight),
            "parse: global drops the override");
        SplineTuning ini = editor::DefaultSplineTuning();
        sp::IniRead(ini, 0, "Segment");
        sp::IniRead(ini, 1, "0.25");
        sp::IniRead(ini, 7, "0");
        sp::IniRead(ini, 4, "nonsense");
        Check(ini.pacing == static_cast<std::uint8_t>(SplinePacing::PerSegment) && ini.alpha == 0.25f &&
                  !(ini.channels & editor::SplineChannelFov) && ini.weight == 0.0f && ini.overrides == 0 &&
                  sp::IniValue(ini, 0) == "segment" && sp::IniValue(ini, 1) == "0.25",
            "ini: read and written back");
    }

    // ---- 2. The zoom never passes its target and comes back.
    void TestFovMonotone()
    {
        Track track;
        const std::uint64_t times[] = {0, 3000, 3300, 6000};
        const float fovs[] = {30.0f, 60.0f, 61.0f, 20.0f};
        for (int n = 0; n < 4; ++n)
        {
            Keyframe k = FreeAt(times[n], {static_cast<float>(n), 0, 0}, 0.0f, fovs[n]);
            track.AddMarker(k);
        }
        // The plain time weighted Hermite would pass 61 between 3000 and 3300.
        const double plainSlope1 = (61.0 - 30.0) / 3300.0, plainSlope2 = (20.0 - 60.0) / 3000.0;
        double plainPeak = 0.0;
        for (int n = 0; n <= 100; ++n)
            plainPeak = std::max(plainPeak, sp::Hermite(60.0, 61.0, plainSlope1, plainSlope2, 300.0, n / 100.0));
        Check(plainPeak > 61.05, "fov: the plain curve overshoots there (the case is real)");
        bool inside = true;
        for (int segment = 0; segment < 3; ++segment)
        {
            const float low = std::min(fovs[segment], fovs[segment + 1]) - 1e-3f;
            const float high = std::max(fovs[segment], fovs[segment + 1]) + 1e-3f;
            for (std::uint64_t t = times[segment]; t <= times[segment + 1]; t += 5)
            {
                const float fov = View(track, t).fov;
                inside = inside && fov >= low && fov <= high;
            }
        }
        Check(inside, "fov: every segment stays between its two markers' values");
        Check(std::fabs(View(track, 3300).fov - 61.0f) < 1e-3f, "fov: the marker's value at its time");
    }

    // ---- 3. Markers hit on time with ease and with every pacing.
    void TestOnTime()
    {
        const std::vector<std::uint64_t> times{0, 1000, 1700, 4000, 4500};
        const std::vector<Vec3> points{{0, 0, 0}, {10, 0, 0}, {14, 9, 0}, {0, 20, 3}, {-2, 21, 3}};
        const SplinePacing pacings[] = {SplinePacing::Natural, SplinePacing::Continuous, SplinePacing::PerSegment};
        for (const SplinePacing pacing : pacings)
        {
            for (int eased = 0; eased < 2; ++eased)
            {
                const SplineTuning t = With(editor::SplineOverridePacing | editor::SplineOverrideEaseIn | editor::SplineOverrideEaseOut,
                    0.5f, eased ? 0.3f : 0.0f, eased ? 0.45f : 0.0f, 0.0f, pacing);
                const Track track = Chain(times, points, &t);
                bool hit = true;
                for (std::size_t n = 0; n < times.size(); ++n)
                    hit = hit && Distance(At(track, times[n]), points[n]) < 1e-3f;
                char message[96];
                std::snprintf(message, sizeof(message), "on time: pacing %d, ease %d, each marker at its time", static_cast<int>(pacing), eased);
                Check(hit, message);
                // And arriving: just before a marker it is near it, not still far.
                Check(Distance(At(track, 999), points[1]) < 0.1f, "on time: arrives continuously");
            }
        }
        // With an ease the move starts and ends at rest in that segment.
        const SplineTuning t = With(editor::SplineOverrideEaseIn | editor::SplineOverrideEaseOut, 1.0f, 0.4f, 0.4f, 0.0f, SplinePacing::Natural);
        const Track track = Chain({0, 2000}, {{0, 0, 0}, {20, 0, 0}}, &t);
        Check(Speed(track, 0, +1) < 0.3f && Speed(track, 2000, -1) < 0.3f, "ease: at rest at both markers");
        Check(std::fabs(Speed(track, 1000, +1) - 10.0f / 0.6f) < 0.3f, "ease: the trapezoid's top speed in the middle");
    }

    // ---- 4. Continuous: one speed curve, C1 through markers of uneven
    // spacing, near constant over a move of even speed.
    void TestContinuous()
    {
        const SplineTuning t = With(editor::SplineOverridePacing, 0.5f, 0.0f, 0.0f, 0.0f, SplinePacing::Continuous);
        // Legs of 5, 4.5, 9 and 8 m over 1, 0.5, 1.5 and 1.2 s (5, 9, 6 and
        // 6.7 m/s) with turns: the speed changes, but with no step at a marker.
        const std::vector<std::uint64_t> times{0, 1000, 1500, 3000, 4200};
        const std::vector<Vec3> points{{0, 0, 0}, {5, 0, 0}, {9, 2.06f, 0}, {9, 11.06f, 0}, {17, 11.06f, 0}};
        const Track track = Chain(times, points, &t);
        // The speed on each side of t from the narrowest windows (2 ms) and
        // the one before them, extrapolated to t (the pace bends hard here).
        const auto side = [&track](std::uint64_t at, int dir) {
            const auto window = [&](std::uint64_t a, std::uint64_t b) {
                return Distance(At(track, a), At(track, b)) / (static_cast<float>(b - a) / 1000.0f);
            };
            const float nearSpeed = dir < 0 ? window(at - 3, at - 1) : window(at + 1, at + 3);
            const float farSpeed = dir < 0 ? window(at - 6, at - 4) : window(at + 4, at + 6);
            return nearSpeed + (nearSpeed - farSpeed) * (2.0f / 3.0f);
        };
        for (std::size_t n = 1; n + 1 < times.size(); ++n)
        {
            const float before = side(times[n], -1), after = side(times[n], +1);
            char message[96];
            std::snprintf(message, sizeof(message), "continuous: speed continuous at marker %zu (%.2f / %.2f)", n, before, after);
            Check(std::fabs(before - after) / std::max(before, after) < 0.03f, message);
        }
        // Even speed (10 m per second on every leg, a corner): near constant.
        const Track even = Chain({0, 1000, 2000, 3000}, {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {20, 10, 0}}, &t);
        float low = 1e9f, high = 0.0f;
        for (std::uint64_t at = 100; at <= 2900; at += 50)
        {
            const float s = Speed(even, at, +1);
            low = std::min(low, s);
            high = std::max(high, s);
        }
        Check(high / low < 1.15f, "continuous: near one speed over an even move");
        // Natural on the same corners slows into them (the curve's own speed).
        float nlow = 1e9f, nhigh = 0.0f;
        const Track natural = Chain({0, 1000, 2000, 3000}, {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {20, 10, 0}});
        for (std::uint64_t at = 100; at <= 2900; at += 50)
        {
            const float s = Speed(natural, at, +1);
            nlow = std::min(nlow, s);
            nhigh = std::max(nhigh, s);
        }
        Check(nhigh / nlow > high / low, "continuous: steadier than Natural");
        // Per Segment: constant inside a segment.
        const SplineTuning ps = With(editor::SplineOverridePacing, 0.5f, 0.0f, 0.0f, 0.0f, SplinePacing::PerSegment);
        const Track segment = Chain({0, 1000, 2000, 3000}, {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {20, 10, 0}}, &ps);
        float slow = 1e9f, shigh = 0.0f;
        for (std::uint64_t at = 1050; at <= 1940; at += 30)
        {
            const float s = Speed(segment, at, +1);
            slow = std::min(slow, s);
            shigh = std::max(shigh, s);
        }
        Check(shigh / slow < 1.03f, "per segment: constant speed inside a segment");
        // The rotation and the zoom by time, not by the distance: the same as Natural's.
        Track turn = Chain({0, 1000, 2500}, {{0, 0, 0}, {3, 0, 0}, {30, 0, 0}}, &t);
        Track turnNatural = Chain({0, 1000, 2500}, {{0, 0, 0}, {3, 0, 0}, {30, 0, 0}});
        for (Track* tr : {&turn, &turnNatural})
        {
            tr->FindMutable(tr->Keyframes()[1].id)->pose.orientation = Track::FromEulerDegrees({0, 0, 40});
            tr->FindMutable(tr->Keyframes()[2].id)->pose.fov = 70.0f;
        }
        Check(std::fabs(View(turn, 1700).rotation.z - View(turnNatural, 1700).rotation.z) < 1e-4f &&
                  std::fabs(View(turn, 1700).fov - View(turnNatural, 1700).fov) < 1e-4f,
            "continuous: rotation and zoom follow time");
    }

    // ---- 5. Curve shape: alpha changes the path off Natural only.
    void TestShape()
    {
        const std::vector<std::uint64_t> times{0, 1000, 2000, 3000};
        const std::vector<Vec3> points{{0, 0, 0}, {10, 0, 0}, {11, 1, 0}, {11, 20, 0}};
        const SplineTuning u = With(editor::SplineOverridePacing | editor::SplineOverrideAlpha, 0.0f, 0, 0, 0, SplinePacing::PerSegment);
        const SplineTuning c = With(editor::SplineOverridePacing | editor::SplineOverrideAlpha, 1.0f, 0, 0, 0, SplinePacing::PerSegment);
        const SplineTuning nu = With(editor::SplineOverrideAlpha, 0.0f, 0, 0, 0, SplinePacing::Natural);
        const SplineTuning nc = With(editor::SplineOverrideAlpha, 1.0f, 0, 0, 0, SplinePacing::Natural);
        // The path's farthest point from the chord 1-2 (where a loose curve swings out).
        const auto swing = [&](const Track& track) {
            float widest = 0.0f;
            for (std::uint64_t t = 1000; t <= 2000; t += 10)
                widest = std::max(widest, Distance(At(track, t), Vec3{10.5f, 0.5f, 0.0f}));
            return widest;
        };
        Check(swing(Chain(times, points, &c)) < swing(Chain(times, points, &u)), "shape: Chordal holds a short leg tighter than Uniform");
        Check(SameBits(View(Chain(times, points, &nu), 1500), View(Chain(times, points, &nc), 1500)),
            "shape: no effect under Natural (no knot spacing)");
    }

    // ---- 6. Natural end.
    void TestNaturalEnd()
    {
        // Collinear at one speed: the path is the line at that speed (what
        // the one sided tangent gave): nothing changes.
        const Track line = Chain({0, 1000, 2000, 3000}, {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}});
        bool same = true;
        for (std::uint64_t t = 0; t <= 3000; t += 37)
            same = same && Distance(At(line, t), Vec3{t / 100.0f, 0, 0}) < 1e-3f;
        Check(same, "natural end: collinear at one speed, unchanged");
        // Collinear at two speeds: still on the line (only the pace differs).
        const Track paced = Chain({0, 1000, 1500}, {{0, 0, 0}, {10, 0, 0}, {30, 0, 0}});
        bool onLine = true;
        for (std::uint64_t t = 0; t <= 1500; t += 25)
        {
            const Vec3 p = At(paced, t);
            onLine = onLine && std::fabs(p.y) < 1e-4f && std::fabs(p.z) < 1e-4f;
        }
        Check(onLine, "natural end: a collinear move stays on its line");
        // A corner: the first segment leaves already turning (no bend at the
        // first marker), unlike the one sided tangent.
        const Track corner = Chain({0, 1000, 2000}, {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}});
        const Vec3 natural = At(corner, 500);
        const double m2y = (10.0 - 0.0) / 2.0; // the inner tangent at marker 1 (per second), y
        const double m2x = (10.0 - 0.0) / 2.0;
        const double oneSidedX = sp::Hermite(0.0, 10.0, 10.0, m2x, 1.0, 0.5);
        const double oneSidedY = sp::Hermite(0.0, 0.0, 0.0, m2y, 1.0, 0.5);
        Check(Distance(natural, Vec3{static_cast<float>(oneSidedX), static_cast<float>(oneSidedY), 0}) > 0.2f,
            "natural end: a corner's first segment changes");
        // No bend at the end markers: the second difference there is far
        // smaller than inside.
        const auto bend = [&](std::uint64_t t) {
            const Vec3 a = At(corner, t), b = At(corner, t + 20), c = At(corner, t + 40);
            return Distance(Vec3{a.x + c.x - 2 * b.x, a.y + c.y - 2 * b.y, 0}, Vec3{0, 0, 0});
        };
        Check(bend(0) < 0.25f * bend(980), "natural end: no bend at the first marker");
    }

    // ---- 7. The camera weight.
    void TestWeight()
    {
        const std::vector<std::uint64_t> times{0, 1000, 2200, 3000};
        const std::vector<Vec3> points{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {20, 10, 0}};
        const SplineTuning zero = With(editor::SplineOverrideWeight, 1.0f, 0, 0, 0.0f, SplinePacing::Natural);
        const SplineTuning full = With(editor::SplineOverrideWeight, 1.0f, 0, 0, 1.0f, SplinePacing::Natural);
        camera::Scene playing;
        playing.splineWeight = true;
        const Track plain = Chain(times, points);
        const Track weightZero = Chain(times, points, &zero);
        const Track heavy = Chain(times, points, &full);
        bool identical = true, paused = true;
        for (std::uint64_t t = 0; t <= 3000; t += 7)
        {
            identical = identical && SameBits(View(plain, t, playing), View(weightZero, t, playing)) &&
                SameBits(View(plain, t, playing), View(plain, t));
            paused = paused && SameBits(View(heavy, t), View(plain, t));
        }
        Check(identical, "weight: 0 is bit for bit the curve without weight");
        Check(paused, "weight: not applied while paused (scene without it)");
        // Inside the corner, never past the marker.
        const Vec3 cut = At(heavy, 1000, playing);
        Check(Distance(cut, points[1]) > 0.5f && cut.x <= 10.0f + 1e-3f && cut.y >= -1e-3f, "weight: cuts inside the corner");
        Check(Distance(At(heavy, 0, playing), points[0]) < 1e-3f && Distance(At(heavy, 3000, playing), points[3]) < 1e-3f,
            "weight: the curve's first and last marker are still hit");
        Check(Distance(At(heavy, 999, playing), At(heavy, 1001, playing)) < 0.05f, "weight: continuous through a marker");
        const float before = Speed(heavy, 2200, -1, playing), after = Speed(heavy, 2200, +1, playing);
        Check(std::fabs(before - after) / std::max(before, after) < 0.05f, "weight: speed continuous through a marker (uneven times)");
        // Rotation (quaternion space) and zoom too.
        Track turn = Chain(times, points, &full);
        const float headings[] = {0.0f, 90.0f, 0.0f, 0.0f};
        const float fovs[] = {40.0f, 70.0f, 40.0f, 40.0f};
        for (std::size_t n = 0; n < 4; ++n)
        {
            Keyframe* k = turn.FindMutable(turn.Keyframes()[n].id);
            k->pose.orientation = Track::FromEulerDegrees({0, 0, headings[n]});
            k->pose.fov = fovs[n];
        }
        const CameraState heavyView = View(turn, 1000, playing);
        const CameraState light = View(turn, 1000);
        Check(std::fabs(light.rotation.z - 90.0f) < 0.01f && heavyView.rotation.z < 85.0f && heavyView.rotation.z > 30.0f,
            "weight: the turn is cut short in quaternion space");
        Check(std::fabs(light.fov - 70.0f) < 0.01f && heavyView.fov < 69.0f && heavyView.fov > 40.0f, "weight: the zoom too");
        Check(std::fabs(View(turn, 0, playing).rotation.z) < 0.01f, "weight: the first marker's rotation kept");
        // Continuous on the weighted curve still reaches it on time.
        const SplineTuning cont = With(editor::SplineOverrideWeight | editor::SplineOverridePacing, 1.0f, 0, 0, 1.0f, SplinePacing::Continuous);
        const Track heavyCont = Chain(times, points, &cont);
        const float cb = Speed(heavyCont, 1000, -1, playing), ca = Speed(heavyCont, 1000, +1, playing);
        Check(std::fabs(cb - ca) / std::max(cb, ca) < 0.04f, "weight + continuous: speed continuous");
        Check(Distance(At(heavyCont, 1000, playing), At(heavy, 1000, playing)) < 0.6f, "weight + continuous: on the weighted curve");
    }

    // ---- 8. Ease 0 is no ease; channels; every channel eases together.
    void TestEaseAndChannels()
    {
        const std::vector<std::uint64_t> times{0, 1000, 2600, 3000};
        const std::vector<Vec3> points{{0, 0, 0}, {10, 0, 0}, {14, 9, 0}, {0, 20, 3}};
        const SplineTuning zero = With(editor::SplineOverrideEaseIn | editor::SplineOverrideEaseOut, 1.0f, 0.0f, 0.0f, 0, SplinePacing::Natural);
        const Track plain = Chain(times, points);
        const Track eased0 = Chain(times, points, &zero);
        bool same = true;
        for (std::uint64_t t = 0; t <= 3000; t += 11)
            same = same && SameBits(View(plain, t), View(eased0, t));
        Check(same, "ease: 0 and 0 is bit for bit no ease");
        // Channels off: straight on the eased phase, together.
        const SplineTuning off = With(editor::SplineOverridePosition | editor::SplineOverrideFov | editor::SplineOverrideEaseIn,
            1.0f, 0.5f, 0.0f, 0.0f, SplinePacing::Natural, editor::SplineChannelRotation);
        Track track = Chain({0, 2000, 4000}, {{0, 0, 0}, {20, 0, 0}, {20, 20, 0}}, &off);
        track.FindMutable(track.Keyframes()[1].id)->pose.fov = 85.0f;
        const CameraState v = View(track, 600);
        const float positionPhase = v.position.x / 20.0f;
        const float fovPhase = (v.fov - 45.0f) / 40.0f;
        Check(std::fabs(v.position.y) < 1e-4f && std::fabs(positionPhase - fovPhase) < 1e-4f &&
                  std::fabs(positionPhase - static_cast<float>(sp::Ease(0.3, 0.5, 0.0))) < 1e-4f,
            "channels: position and zoom off go straight, on the same eased phase");
        camera::SegmentPlan plan;
        CameraState unused{};
        camera::EvaluateMarkersRaw(track, 600, {}, unused, &plan);
        Check(std::fabs(plan.phase - positionPhase) < 1e-4f, "ease: the plan's phase is the eased one (DOF, framing)");
    }

    // ---- 9. Defaults and formats.
    void TestDefaultsAndFormats()
    {
        Keyframe fresh;
        Check(fresh.blend == Blend::Spline && fresh.spline.overrides == 0, "default: a new marker is Spline with the global tuning");
        Keyframe game;
        game.cameraMode = CameraMode::Game;
        game.blend = Blend::Linear;
        Keyframe after;
        camera::InheritFrom(after, game);
        Check(after.blend == Blend::Spline, "default: after a Game marker, Spline");
        Keyframe free = FreeAt(0, {0, 0, 0});
        free.blend = Blend::Smooth;
        free.spline = With(editor::SplineOverrideWeight, 1.0f, 0, 0, 0.7f, SplinePacing::Natural);
        Keyframe afterFree;
        camera::InheritFrom(afterFree, free);
        Check(afterFree.blend == Blend::Smooth && afterFree.spline.weight == 0.7f, "inherit: a Free marker's blend and tuning");
        editor::SplineTuning bad = editor::DefaultSplineTuning();
        bad.alpha = NAN;
        Check(!camera::SetSplineTuning(free, bad), "set tuning: a non finite number refused");
        bad.alpha = 3.0f;
        Check(camera::SetSplineTuning(free, bad) && free.spline.alpha == 1.0f, "set tuning: held to the range");

        // CAMR v7 round trip and the per marker tuning.
        const SplineTuning t = With(editor::SplineOverridePacing | editor::SplineOverrideAlpha | editor::SplineOverrideWeight |
                editor::SplineOverrideFov, 0.25f, 0.1f, 0.2f, 0.6f, SplinePacing::Continuous, editor::SplineChannelPosition | editor::SplineChannelRotation);
        const Track track = Chain({0, 1000, 2000}, {{0, 0, 0}, {5, 0, 0}, {5, 5, 0}}, &t);
        // CAMR 8 adds 20 bytes per marker after the tuning; CAMR 9 is current.
        // The v7 and v6 payloads as the old builds wrote them
        // (old_camera_payload.h).
        Check(clipfile::CameraSectionVersion() == 9, "CAMR: version 9");
        Track back;
        std::string error;
        const std::vector<std::uint8_t> bytes = testpayload::OldCamera(track, 7); // the v7 payload
        const std::size_t header = 4 + 1 + 8, per = (bytes.size() - header) / 3;
        bool kept = clipfile::DecodeCameraTrack(bytes.data(), bytes.size(), 7, back, error) && back.Keyframes().size() == 3;
        for (std::size_t n = 0; kept && n < 3; ++n)
            kept = sp::Same(back.Keyframes()[n].spline, track.Keyframes()[n].spline) && back.Keyframes()[n].blend == Blend::Spline;
        Check(kept, "CAMR v7: round trip with the tuning");
        // v6: the same payload without the 20 bytes per marker reads with the defaults.
        const std::vector<std::uint8_t> v6 = testpayload::OldCamera(track, 6);
        Track old;
        Check(clipfile::DecodeCameraTrack(v6.data(), v6.size(), 6, old, error) && old.Keyframes().size() == 3 &&
                  old.Keyframes()[1].spline.overrides == 0 && sp::Same(old.Keyframes()[1].spline, editor::DefaultSplineTuning()),
            "CAMR v6 still reads (the tuning from the defaults)");
        Check(!clipfile::DecodeCameraTrack(v6.data(), v6.size(), 7, old, error), "CAMR: a v6 payload is not read as v7");
        // A tuning out of range is refused.
        std::vector<std::uint8_t> broken = bytes;
        const float alpha = 2.0f;
        std::memcpy(broken.data() + header + per - 20, &alpha, sizeof(alpha));
        Check(!clipfile::DecodeCameraTrack(broken.data(), broken.size(), 7, old, error), "CAMR v7: a tuning out of range refused");
        Check(!camera::SameMarkers(track, Chain({0, 1000, 2000}, {{0, 0, 0}, {5, 0, 0}, {5, 5, 0}})),
            "same markers: the tuning counts (undo, unsaved changes)");
    }

    // ---- 10. The defaults from the scene, per marker overrides.
    void TestOverrides()
    {
        const std::vector<std::uint64_t> times{0, 1000, 1500, 3000};
        const std::vector<Vec3> points{{0, 0, 0}, {5, 0, 0}, {20, 4, 0}, {26, 12, 0}};
        camera::Scene continuous;
        continuous.splineDefaults = With(0, 0.5f, 0, 0, 0, SplinePacing::Continuous);
        const SplineTuning own = With(editor::SplineOverridePacing | editor::SplineOverrideAlpha, 0.5f, 0, 0, 0, SplinePacing::Continuous);
        const Track global = Chain(times, points);
        const Track marker = Chain(times, points, &own);
        bool same = true, differs = false;
        for (std::uint64_t t = 0; t <= 3000; t += 13)
        {
            same = same && SameBits(View(global, t, continuous), View(marker, t));
            differs = differs || Distance(At(global, t), At(marker, t)) > 0.05f;
        }
        Check(same, "overrides: the defaults from the scene equal the same fields on the markers");
        Check(differs, "overrides: Continuous differs from Natural");
    }
}

int RunCameraSplineTests()
{
    g_failures = 0;
    TestMath();
    TestFovMonotone();
    TestOnTime();
    TestContinuous();
    TestShape();
    TestNaturalEnd();
    TestWeight();
    TestEaseAndChannels();
    TestDefaultsAndFormats();
    TestOverrides();
    if (g_failures == 0)
        std::puts("camera-spline (pchip zoom, natural end, ease, pacing, shape, weight, channels, overrides, CAMR v7) tests passed");
    return g_failures;
}
