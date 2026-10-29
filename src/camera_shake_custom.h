#pragma once

// The Custom shake: Simple Camera's numbers (crxhvrd, MIT; camera.cpp
// ComputeShakeOffsets) for the replay. Six channels, each a sum of three sines
// (octaves 1, 2.03 and 4.11, weights 4/7, 2/7, 1/7) at the shared phase times
// a per axis multiplier (0.80..1.25, incommensurate, so the pattern never
// repeats), with a per axis phase seed. Rotation reaches amp x 1 degree x
// rotWeight, position amp x 5 cm x posWeight (world metres), as there.
//
// Deterministic, as the profiles: the result depends only on the numbers, the
// pattern seed, the integrated phase (cycles since the run of Custom shake
// started) and the camera's speed at t. Simple Camera re-rolls its seeds with
// Randomize; here `seed` picks the pattern (0: its own table), so a scrub
// shows what the play shows.
//
// The header part (defaults, ranges, presets, the key=value fields) is inline
// so the editor add-on uses it without linking the core.

#include "camera_shake.h"
#include "editor_protocol.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace replay::camera::shake
{
    using editor::ShakeCustom;

    // The ranges the editor and the files hold the numbers to.
    struct CustomRange
    {
        float minimum;
        float maximum;
    };
    constexpr CustomRange kCustomAmp{0.0f, 5.0f};
    constexpr CustomRange kCustomHz{0.1f, 30.0f};
    constexpr CustomRange kCustomCoupling{0.0f, 2.0f};
    constexpr CustomRange kCustomWeight{0.0f, 2.0f};
    constexpr CustomRange kCustomSpeedRef{1.0f, 100.0f};

    inline float ClampTo(float value, CustomRange range)
    {
        if (!std::isfinite(value))
            return range.minimum;
        return value < range.minimum ? range.minimum : value > range.maximum ? range.maximum : value;
    }

    inline bool Within(float value, CustomRange range)
    {
        return std::isfinite(value) && value >= range.minimum && value <= range.maximum;
    }

    inline bool ValidCustom(const ShakeCustom& c)
    {
        return Within(c.amp, kCustomAmp) && Within(c.hz, kCustomHz) && Within(c.speedAmp, kCustomCoupling) &&
            Within(c.speedHz, kCustomCoupling) && Within(c.rotWeight, kCustomWeight) && Within(c.posWeight, kCustomWeight) &&
            Within(c.speedRef, kCustomSpeedRef) && c.still <= 1;
    }

    inline ShakeCustom ClampCustom(ShakeCustom c)
    {
        c.amp = ClampTo(c.amp, kCustomAmp);
        c.hz = ClampTo(c.hz, kCustomHz);
        c.speedAmp = ClampTo(c.speedAmp, kCustomCoupling);
        c.speedHz = ClampTo(c.speedHz, kCustomCoupling);
        c.rotWeight = ClampTo(c.rotWeight, kCustomWeight);
        c.posWeight = ClampTo(c.posWeight, kCustomWeight);
        c.speedRef = ClampTo(c.speedRef, kCustomSpeedRef);
        c.still = c.still ? 1 : 0;
        c.reserved = 0;
        return c;
    }

    inline bool SameCustom(const ShakeCustom& a, const ShakeCustom& b)
    {
        return a.amp == b.amp && a.hz == b.hz && a.speedAmp == b.speedAmp && a.speedHz == b.speedHz &&
            a.rotWeight == b.rotWeight && a.posWeight == b.posWeight && a.speedRef == b.speedRef && a.seed == b.seed &&
            a.still == b.still;
    }

    // Between two Custom settings (a ramp or a blend): the numbers linear,
    // the pattern and the still switch from `a`.
    inline ShakeCustom LerpCustom(const ShakeCustom& a, const ShakeCustom& b, float u)
    {
        const auto mix = [u](float x, float y) { return x + (y - x) * u; };
        ShakeCustom out = a;
        out.amp = mix(a.amp, b.amp);
        out.hz = mix(a.hz, b.hz);
        out.speedAmp = mix(a.speedAmp, b.speedAmp);
        out.speedHz = mix(a.speedHz, b.speedHz);
        out.rotWeight = mix(a.rotWeight, b.rotWeight);
        out.posWeight = mix(a.posWeight, b.posWeight);
        out.speedRef = mix(a.speedRef, b.speedRef);
        return out;
    }

    // "Copy From Preset": the numbers nearest a profile (an approximation:
    // the profiles are band limited noise plus a gait, this is one noise).
    // amp from the profile's rotation size at intensity 5 (degrees), hz from
    // its gait (the step or stride rate) or its main band, the weights from
    // its position size against its rotation (5 cm per amp unit).
    inline ShakeCustom CustomFromPreset(editor::ShakeType type)
    {
        ShakeCustom c = editor::DefaultShakeCustom();
        c.speedAmp = 0.5f;
        c.speedHz = 0.3f;
        switch (type)
        {
        case editor::ShakeType::Handheld: c.amp = 0.8f; c.hz = 0.6f; c.posWeight = 0.25f; break;
        case editor::ShakeType::Drunk: c.amp = 3.0f; c.hz = 0.1f; c.posWeight = 0.4f; break;
        case editor::ShakeType::GroundVibration: c.amp = 0.05f; c.hz = 17.0f; c.posWeight = 0.8f; break;
        case editor::ShakeType::AirTurbulence: c.amp = 0.6f; c.hz = 0.8f; c.posWeight = 0.6f; break;
        case editor::ShakeType::Explosion: c.amp = 2.0f; c.hz = 9.0f; c.speedAmp = 0.0f; c.speedHz = 0.0f; break;
        case editor::ShakeType::Gunshot: c.amp = 0.5f; c.hz = 6.0f; c.speedAmp = 0.0f; c.speedHz = 0.0f; break;
        case editor::ShakeType::Walking: c.amp = 0.6f; c.hz = 2.0f; c.posWeight = 0.6f; break;
        case editor::ShakeType::Running: c.amp = 0.9f; c.hz = 2.9f; break;
        case editor::ShakeType::Horseback: c.amp = 1.2f; c.hz = 2.0f; c.posWeight = 0.8f; c.speedAmp = 1.0f; break;
        case editor::ShakeType::Steadicam: c.amp = 0.1f; c.hz = 0.15f; c.posWeight = 1.4f; break;
        case editor::ShakeType::Wagon: c.amp = 0.6f; c.hz = 1.2f; c.posWeight = 0.6f; break;
        default: return editor::DefaultShakeCustom();
        }
        return c;
    }

    // One key=value of a Custom shake, as the director and the bench write
    // them: shake_amp, shake_hz, shake_spdamp, shake_spdhz, shake_rot,
    // shake_pos, shake_ref, shake_still (0/1), shake_seed. `known` false when
    // the key is not one of them; an out of range value is an error.
    inline bool ParseCustomField(const std::string& key, const std::string& value, ShakeCustom& c, bool& known,
        std::string& error)
    {
        known = true;
        char* end = nullptr;
        const auto number = [&](float& field, CustomRange range) {
            const float v = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end || !Within(v, range))
            {
                error = key + " out of range: " + value;
                return false;
            }
            field = v;
            return true;
        };
        if (key == "shake_amp")
            return number(c.amp, kCustomAmp);
        if (key == "shake_hz")
            return number(c.hz, kCustomHz);
        if (key == "shake_spdamp")
            return number(c.speedAmp, kCustomCoupling);
        if (key == "shake_spdhz")
            return number(c.speedHz, kCustomCoupling);
        if (key == "shake_rot")
            return number(c.rotWeight, kCustomWeight);
        if (key == "shake_pos")
            return number(c.posWeight, kCustomWeight);
        if (key == "shake_ref")
            return number(c.speedRef, kCustomSpeedRef);
        if (key == "shake_still" || key == "shake_seed")
        {
            const long v = std::strtol(value.c_str(), &end, 10);
            const long top = key == "shake_still" ? 1 : 65535;
            if (end == value.c_str() || *end || v < 0 || v > top)
            {
                error = key + " out of range: " + value;
                return false;
            }
            if (key == "shake_still")
                c.still = static_cast<std::uint8_t>(v);
            else
                c.seed = static_cast<std::uint16_t>(v);
            return true;
        }
        known = false;
        return true;
    }

    // The 3 octave sum of one axis (Simple Camera's ShakeNoise1D).
    float CustomNoise(double phase, float seed);

    // The per axis phase seed and frequency multiplier of a pattern.
    void CustomPattern(std::uint16_t seed, float (&phaseSeed)[ChannelCount], float (&multiplier)[ChannelCount]);

    // The Custom shake at a phase of `cycles` (the integrated frequency since
    // the run started, already including the speed coupling), scaled by
    // `gain` (the intensity's), with the camera's normalised speed now
    // (0..1, for the amplitude coupling) and the still gate (0..1). It fades
    // in over the first 250 ms of `runSeconds` so it never pops.
    Sample EvaluateCustom(const ShakeCustom& c, float gain, double cycles, float speedNorm, float still, double runSeconds);
}
