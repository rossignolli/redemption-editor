#include "camera_spline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace replay::camera::spline
{
    namespace
    {
        float Hold(float value, float low, float high, float fallback)
        {
            if (!std::isfinite(value))
                return fallback;
            return std::max(low, std::min(high, value));
        }

        bool Has(const SplineTuning& t, std::uint8_t bit)
        {
            return (t.overrides & bit) != 0;
        }

        std::uint8_t ChannelFrom(const SplineTuning& marker, const SplineTuning& defaults, std::uint8_t channel, std::uint8_t bit)
        {
            const SplineTuning& from = Has(marker, bit) ? marker : defaults;
            return static_cast<std::uint8_t>(from.channels & channel);
        }

        bool ParseNumber(const std::string& text, float low, float high, float& out)
        {
            std::string value = text;
            std::replace(value.begin(), value.end(), ',', '.');
            char* end = nullptr;
            const double number = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end || !std::isfinite(number) || number < low || number > high)
                return false;
            out = static_cast<float>(number);
            return true;
        }

        std::string Number(float value)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
            return text;
        }
    }

    SplineTuning Effective(const SplineTuning& marker, const SplineTuning& defaults)
    {
        SplineTuning out = Clamped(defaults);
        const SplineTuning own = Clamped(marker);
        if (Has(own, editor::SplineOverrideAlpha))
            out.alpha = own.alpha;
        if (Has(own, editor::SplineOverrideEaseIn))
            out.easeIn = own.easeIn;
        if (Has(own, editor::SplineOverrideEaseOut))
            out.easeOut = own.easeOut;
        if (Has(own, editor::SplineOverrideWeight))
            out.weight = own.weight;
        if (Has(own, editor::SplineOverridePacing))
            out.pacing = own.pacing;
        out.channels = static_cast<std::uint8_t>(
            ChannelFrom(own, out, editor::SplineChannelPosition, editor::SplineOverridePosition) |
            ChannelFrom(own, out, editor::SplineChannelRotation, editor::SplineOverrideRotation) |
            ChannelFrom(own, out, editor::SplineChannelFov, editor::SplineOverrideFov));
        out.overrides = own.overrides;
        out.reserved = 0;
        return out;
    }

    SplineTuning Clamped(const SplineTuning& tuning)
    {
        const SplineTuning d = editor::DefaultSplineTuning();
        SplineTuning out = tuning;
        out.alpha = Hold(tuning.alpha, kAlphaMin, kAlphaMax, d.alpha);
        out.easeIn = Hold(tuning.easeIn, 0.0f, kEaseMax, d.easeIn);
        out.easeOut = Hold(tuning.easeOut, 0.0f, kEaseMax, d.easeOut);
        out.weight = Hold(tuning.weight, 0.0f, kWeightMax, d.weight);
        if (out.pacing >= editor::kSplinePacingCount)
            out.pacing = d.pacing;
        out.channels &= editor::SplineChannelAll;
        out.reserved = 0;
        return out;
    }

    bool Valid(const SplineTuning& t)
    {
        const auto in = [](float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; };
        return in(t.alpha, kAlphaMin, kAlphaMax) && in(t.easeIn, 0.0f, kEaseMax) && in(t.easeOut, 0.0f, kEaseMax) &&
            in(t.weight, 0.0f, kWeightMax) && t.pacing < editor::kSplinePacingCount && (t.channels & ~editor::SplineChannelAll) == 0;
    }

    bool Same(const SplineTuning& a, const SplineTuning& b)
    {
        return a.alpha == b.alpha && a.easeIn == b.easeIn && a.easeOut == b.easeOut && a.weight == b.weight &&
            a.pacing == b.pacing && a.channels == b.channels && a.overrides == b.overrides;
    }

    double Ease(double phase, double easeIn, double easeOut)
    {
        double in = easeIn > 0.0 ? easeIn : 0.0;
        double out = easeOut > 0.0 ? easeOut : 0.0;
        if (in <= 0.0 && out <= 0.0)
            return phase;
        const double p = std::max(0.0, std::min(1.0, phase));
        const double sum = in + out;
        if (sum > 1.0)
        {
            in /= sum;
            out /= sum;
        }
        // The top speed that covers the whole segment in its time: the area
        // of the trapezoid is top x (1 - (in + out) / 2).
        const double top = 1.0 / (1.0 - 0.5 * (in + out));
        if (in > 0.0 && p < in)
            return 0.5 * top * p * p / in;
        if (p <= 1.0 - out)
            return 0.5 * top * in + top * (p - in);
        const double left = 1.0 - p;
        return out > 0.0 ? 1.0 - 0.5 * top * left * left / out : 1.0;
    }

    double Hermite(double p1, double p2, double m1, double m2, double h, double x)
    {
        const double x2 = x * x;
        const double x3 = x2 * x;
        return (2.0 * x3 - 3.0 * x2 + 1.0) * p1 + (x3 - 2.0 * x2 + x) * h * m1 + (-2.0 * x3 + 3.0 * x2) * p2 +
            (x3 - x2) * h * m2;
    }

    double HermiteSlope(double p1, double p2, double m1, double m2, double h, double x)
    {
        if (h <= 0.0)
            return 0.0;
        const double x2 = x * x;
        return ((6.0 * x2 - 6.0 * x) * p1 + (3.0 * x2 - 4.0 * x + 1.0) * h * m1 + (-6.0 * x2 + 6.0 * x) * p2 +
                   (3.0 * x2 - 2.0 * x) * h * m2) / h;
    }

    double MonotoneInner(double hPrev, double hNext, double dPrev, double dNext)
    {
        if (dPrev * dNext <= 0.0 || hPrev <= 0.0 || hNext <= 0.0)
            return 0.0;
        const double w1 = 2.0 * hNext + hPrev;
        const double w2 = hNext + 2.0 * hPrev;
        return (w1 + w2) / (w1 / dPrev + w2 / dNext);
    }

    double MonotoneEnd(double h0, double h1, double d0, double d1)
    {
        if (h0 <= 0.0 || h1 <= 0.0)
            return d0;
        const double m = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
        if (m * d0 <= 0.0)
            return 0.0;
        if (d0 * d1 <= 0.0 && std::fabs(m) > 3.0 * std::fabs(d0))
            return 3.0 * d0;
        return m;
    }

    void BSplineWeights(const double knots[6], double u, double weights[4])
    {
        // Cox-de Boor, the triangle of the basis functions of degree 1 to 3
        // that are not zero on the span [knots[2], knots[3]].
        double left[4] = {};
        double right[4] = {};
        weights[0] = 1.0;
        weights[1] = weights[2] = weights[3] = 0.0;
        for (int degree = 1; degree <= 3; ++degree)
        {
            left[degree] = u - knots[3 - degree];
            right[degree] = knots[2 + degree] - u;
            double saved = 0.0;
            for (int r = 0; r < degree; ++r)
            {
                const double span = right[r + 1] + left[degree - r];
                const double share = std::fabs(span) > 1e-12 ? weights[r] / span : 0.0;
                weights[r] = saved + right[r + 1] * share;
                saved = left[degree - r] * share;
            }
            weights[degree] = saved;
        }
    }

    double WeightBetween(double from, double to, double x)
    {
        if (from == to)
            return from;
        const double t = std::max(0.0, std::min(1.0, x));
        return from + (to - from) * t * t * (3.0 - 2.0 * t);
    }

    const char* PacingName(SplinePacing pacing)
    {
        switch (pacing)
        {
        case SplinePacing::Continuous: return "Continuous";
        case SplinePacing::PerSegment: return "Per Segment";
        case SplinePacing::Natural:
        default: return "Natural";
        }
    }

    const char* ShapeName(float alpha)
    {
        if (std::fabs(alpha) < 0.001f)
            return "Uniform";
        if (std::fabs(alpha - 0.5f) < 0.001f)
            return "Centripetal";
        if (std::fabs(alpha - 1.0f) < 0.001f)
            return "Chordal";
        return "";
    }

    bool ParseField(const std::string& key, const std::string& value, SplineTuning& t, bool& known, std::string& error)
    {
        known = true;
        const bool global = value == "global";
        const auto number = [&](float& field, std::uint8_t bit, float high) {
            if (global)
            {
                t.overrides = static_cast<std::uint8_t>(t.overrides & ~bit);
                return true;
            }
            if (!ParseNumber(value, 0.0f, high, field))
            {
                error = "bad_" + key;
                return false;
            }
            t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
            return true;
        };
        const auto channel = [&](std::uint8_t channelBit, std::uint8_t bit) {
            if (global)
            {
                t.overrides = static_cast<std::uint8_t>(t.overrides & ~bit);
                return true;
            }
            if (value != "0" && value != "1" && value != "on" && value != "off")
            {
                error = "bad_" + key;
                return false;
            }
            const bool on = value == "1" || value == "on";
            t.channels = static_cast<std::uint8_t>(on ? (t.channels | channelBit) : (t.channels & ~channelBit));
            t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
            return true;
        };
        if (key == "pacing")
        {
            if (global)
            {
                t.overrides = static_cast<std::uint8_t>(t.overrides & ~editor::SplineOverridePacing);
                return true;
            }
            if (value == "natural")
                t.pacing = static_cast<std::uint8_t>(SplinePacing::Natural);
            else if (value == "continuous")
                t.pacing = static_cast<std::uint8_t>(SplinePacing::Continuous);
            else if (value == "segment" || value == "per_segment")
                t.pacing = static_cast<std::uint8_t>(SplinePacing::PerSegment);
            else
            {
                error = "bad_pacing";
                return false;
            }
            t.overrides = static_cast<std::uint8_t>(t.overrides | editor::SplineOverridePacing);
            return true;
        }
        if (key == "alpha" || key == "shape")
        {
            std::string text = value;
            if (value == "uniform")
                text = "0";
            else if (value == "centripetal")
                text = "0.5";
            else if (value == "chordal")
                text = "1";
            if (global)
                return number(t.alpha, editor::SplineOverrideAlpha, kAlphaMax);
            if (!ParseNumber(text, kAlphaMin, kAlphaMax, t.alpha))
            {
                error = "bad_alpha";
                return false;
            }
            t.overrides = static_cast<std::uint8_t>(t.overrides | editor::SplineOverrideAlpha);
            return true;
        }
        if (key == "ease_in")
            return number(t.easeIn, editor::SplineOverrideEaseIn, kEaseMax);
        if (key == "ease_out")
            return number(t.easeOut, editor::SplineOverrideEaseOut, kEaseMax);
        if (key == "weight")
            return number(t.weight, editor::SplineOverrideWeight, kWeightMax);
        if (key == "sp_pos")
            return channel(editor::SplineChannelPosition, editor::SplineOverridePosition);
        if (key == "sp_rot")
            return channel(editor::SplineChannelRotation, editor::SplineOverrideRotation);
        if (key == "sp_fov")
            return channel(editor::SplineChannelFov, editor::SplineOverrideFov);
        known = false;
        return false;
    }

    std::string Describe(const SplineTuning& t)
    {
        std::string out;
        const auto add = [&out](const std::string& text) {
            if (!out.empty())
                out += ' ';
            out += text;
        };
        if (Has(t, editor::SplineOverridePacing))
        {
            const SplinePacing pacing = static_cast<SplinePacing>(t.pacing);
            add(std::string("pacing=") + (pacing == SplinePacing::Continuous ? "continuous"
                                          : pacing == SplinePacing::PerSegment ? "segment" : "natural"));
        }
        if (Has(t, editor::SplineOverrideAlpha))
            add("alpha=" + Number(t.alpha));
        if (Has(t, editor::SplineOverrideEaseIn))
            add("ease_in=" + Number(t.easeIn));
        if (Has(t, editor::SplineOverrideEaseOut))
            add("ease_out=" + Number(t.easeOut));
        if (Has(t, editor::SplineOverrideWeight))
            add("weight=" + Number(t.weight));
        if (Has(t, editor::SplineOverridePosition))
            add(std::string("sp_pos=") + ((t.channels & editor::SplineChannelPosition) ? "1" : "0"));
        if (Has(t, editor::SplineOverrideRotation))
            add(std::string("sp_rot=") + ((t.channels & editor::SplineChannelRotation) ? "1" : "0"));
        if (Has(t, editor::SplineOverrideFov))
            add(std::string("sp_fov=") + ((t.channels & editor::SplineChannelFov) ? "1" : "0"));
        return out;
    }

    std::string IniValue(const SplineTuning& d, std::size_t keyIndex)
    {
        switch (keyIndex)
        {
        case 0:
        {
            const SplinePacing pacing = static_cast<SplinePacing>(d.pacing);
            return pacing == SplinePacing::Continuous ? "continuous" : pacing == SplinePacing::PerSegment ? "segment" : "natural";
        }
        case 1: return Number(d.alpha);
        case 2: return Number(d.easeIn);
        case 3: return Number(d.easeOut);
        case 4: return Number(d.weight);
        case 5: return (d.channels & editor::SplineChannelPosition) ? "1" : "0";
        case 6: return (d.channels & editor::SplineChannelRotation) ? "1" : "0";
        case 7: return (d.channels & editor::SplineChannelFov) ? "1" : "0";
        default: return "";
        }
    }

    void IniRead(SplineTuning& d, std::size_t keyIndex, const char* text)
    {
        if (!text || !text[0] || keyIndex >= kIniKeyCount)
            return;
        static const char* const kFields[] = {"pacing", "alpha", "ease_in", "ease_out", "weight", "sp_pos", "sp_rot", "sp_fov"};
        std::string value(text);
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        SplineTuning parsed = d;
        bool known = false;
        std::string error;
        if (ParseField(kFields[keyIndex], value, parsed, known, error))
        {
            parsed.overrides = 0;
            d = Clamped(parsed);
        }
    }
}
