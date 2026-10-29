#pragma once

// The scalar maths of the Spline camera blend, pure and engine free. The
// evaluator (camera_track.cpp, EvaluateSpline) puts them together per channel.

//
// The ideas follow the REPlus mod's spline (crxhvrd/REPlus, GPLv3; read for
// the maths only, this is our own code):
//  - a cubic Hermite whose tangents come from the neighbouring markers, with
//    the natural end condition (no bend, C'' = 0) at the first and last
//    marker of a curve instead of a one sided tangent;
//  - Fritsch-Carlson tangents (a monotone cubic) where a value must not pass
//    its target and come back: the zoom, and time to distance in Continuous;
//  - a trapezoid of speed for the ease (it remaps time and still arrives on
//    time);
//  - the cubic B-spline basis (Cox-de Boor over the real knots) the camera
//    weight pulls the curve toward.

#include "editor_protocol.h"

#include <cstdint>
#include <string>

namespace replay::camera::spline
{
    using editor::SplinePacing;
    using editor::SplineTuning;

    constexpr float kAlphaMin = 0.0f;
    constexpr float kAlphaMax = 1.0f;
    constexpr float kEaseMax = 1.0f;
    constexpr float kWeightMax = 1.0f;

    // The tuning a marker plays with: its overridden fields over the
    // defaults. The result has every field set (its overrides are the
    // marker's, kept for display).
    SplineTuning Effective(const SplineTuning& marker, const SplineTuning& defaults);
    // Held to the ranges (a non finite number becomes the default's), the
    // pacing and channels to known values.
    SplineTuning Clamped(const SplineTuning& tuning);
    // Every field finite and in range (the clip reader refuses others).
    bool Valid(const SplineTuning& tuning);
    bool Same(const SplineTuning& a, const SplineTuning& b);

    // The trapezoid ease: the fraction of the segment covered at `phase`
    // (0..1) when it speeds up over the first easeIn of it and slows down over
    // the last easeOut, at a top speed that makes it arrive at 1 on time.
    // easeIn + easeOut over 1 are scaled down to sum 1. Both 0: phase itself.
    double Ease(double phase, double easeIn, double easeOut);

    // The cubic Hermite on an interval of length h from p1 (slope m1) to p2
    // (slope m2), at x = 0..1 of it. Slopes are per unit of the interval's
    // parameter (so the h00..h11 basis takes h x m).
    double Hermite(double p1, double p2, double m1, double m2, double h, double x);
    // Its derivative per unit of the parameter.
    double HermiteSlope(double p1, double p2, double m1, double m2, double h, double x);

    // The natural end: the slope at a curve's free end that leaves it with no
    // bend there (C'' = 0), given the chord slope (p2 - p1) / h of the end
    // segment and the slope at its other end. For three collinear markers
    // passed at a constant speed it is the chord slope (nothing changes).
    constexpr double NaturalEnd(double chordSlope, double otherEndSlope)
    {
        return 1.5 * chordSlope - 0.5 * otherEndSlope;
    }

    // Fritsch-Carlson slope at an inner knot between an interval hPrev with
    // slope dPrev and an interval hNext with slope dNext: 0 at a local
    // extremum (the curve never passes a knot's value), otherwise the
    // weighted harmonic mean that keeps the cubic monotone.
    double MonotoneInner(double hPrev, double hNext, double dPrev, double dNext);
    // The slope at an end knot (the three point formula held to the
    // monotone range): h0/d0 the end interval, h1/d1 the one after it.
    double MonotoneEnd(double h0, double h1, double d0, double d1);

    // The four cubic B-spline weights at u, inside [knots[2], knots[3]], of
    // the six knots around that span (Cox-de Boor, any spacing). They sum to
    // 1 and are not negative.
    void BSplineWeights(const double knots[6], double u, double weights[4]);

    // The camera weight between two markers' weights at x (0..1 of the
    // segment): a smoothstep, so the weight's own change adds no kick at the
    // markers.
    double WeightBetween(double from, double to, double x);

    // Names for the editor and the tools.
    const char* PacingName(SplinePacing pacing);
    // "Uniform", "Centripetal", "Chordal" at 0, 0.5 and 1; empty otherwise.
    const char* ShapeName(float alpha);

    // One key of a marker line in the bench and the clip tool (sets the
    // override bit): pacing=natural|continuous|segment|global, alpha=0..1|
    // uniform|centripetal|chordal|global, ease_in=, ease_out=, weight=
    // (0..1|global), sp_pos=, sp_rot=, sp_fov= (1|0|global). `known` is false
    // for any other key; false with `error` set for a bad value.
    bool ParseField(const std::string& key, const std::string& value, SplineTuning& tuning, bool& known, std::string& error);
    // The overridden fields of a marker as such keys (empty without overrides).
    std::string Describe(const SplineTuning& tuning);

    // RedemptionEditor.ini: the defaults' keys and their text.
    constexpr const char* kIniKeys[] = {"SplinePacing", "SplineAlpha", "SplineEaseIn", "SplineEaseOut", "SplineWeight",
                                        "SplinePosition", "SplineRotation", "SplineFov"};
    constexpr std::size_t kIniKeyCount = sizeof(kIniKeys) / sizeof(kIniKeys[0]);
    std::string IniValue(const SplineTuning& defaults, std::size_t keyIndex);
    // One key read back (unknown text leaves the field as it was).
    void IniRead(SplineTuning& defaults, std::size_t keyIndex, const char* text);
}
