#include "camera_shake.h"
#include "camera_shake_custom.h"

#include <algorithm>
#include <cmath>

namespace
{
    using replay::camera::shake::ChannelCount;
    using replay::editor::ShakeType;

    constexpr double kTwoPi = 6.283185307179586;

    // A band of fractal gradient noise: `hz` is where the first octave's
    // energy peaks, each further octave x kLacunarity in frequency and
    // x persistence in amplitude (0.5 ~ brown, 0.7 ~ pink). amp ~ the
    // typical peak (the RMS is about a third of it).
    struct Band
    {
        float hz = 0.0f;
        float amp = 0.0f;
        std::uint8_t octaves = 1;
        float persistence = 0.5f;
    };

    struct Axis
    {
        Band band[3];
        float bob = 0.0f;       // at the gait rate (one per step / stride)
        float bobPhase = 0.0f;  // cycles
        float sway = 0.0f;      // at half the gait rate (left / right)
        float swayPhase = 0.0f; // cycles
        float impact = 0.0f;    // damped strike at the start of each gait cycle
    };

    struct Profile
    {
        Axis axis[ChannelCount];
        float gaitHz = 0.0f;
        float gaitJitter = 0.0f;   // cycles of timing wander
        float impactDecay = 0.05f; // seconds
        float impactRingHz = 7.0f;
        float gustHz = 0.0f;       // slow envelope over the noise bands
        float gustDepth = 0.0f;    // 0 steady .. 1 fully intermittent (scales the gait too)
    };

    // Octave spacing, not 2.0 so the lattices of the octaves never line up.
    constexpr float kLacunarity = 1.97f;
    // 1D Perlin noise at lattice rate r has its energy peak near 0.5 r
    // (measured): band hz maps to lattice rate 2 hz.
    constexpr double kLatticePerHz = 2.0;
    // The fade in of the continuous types, in shake seconds.
    constexpr double kFadeInSeconds = 0.25;

    std::uint32_t Hash(std::uint32_t x)
    {
        // lowbias32 (Chris Wellons).
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    std::uint32_t Hash2(std::uint32_t seed, std::int64_t i)
    {
        const std::uint64_t u = static_cast<std::uint64_t>(i);
        return Hash(seed ^ Hash(static_cast<std::uint32_t>(u) ^ Hash(static_cast<std::uint32_t>(u >> 32) + 0x9e3779b9U)));
    }

    float Unit(std::uint32_t h) // [-1, 1]
    {
        return static_cast<float>(h >> 8) * (2.0f / 16777215.0f) - 1.0f;
    }

    std::uint32_t Seed(ShakeType type, int channel, int band, int octave)
    {
        return Hash(0x5ca1ab1eU + static_cast<std::uint32_t>(type) * 7919U + static_cast<std::uint32_t>(channel) * 104729U +
                    static_cast<std::uint32_t>(band) * 1299709U + static_cast<std::uint32_t>(octave) * 15485863U);
    }

    float Fractal(std::uint32_t seedBase, const Band& band, double t)
    {
        if (band.amp == 0.0f || band.hz <= 0.0f)
            return 0.0f;
        double rate = band.hz * kLatticePerHz;
        float weight = 1.0f;
        float total = 0.0f;
        float norm = 0.0f;
        for (int octave = 0; octave < band.octaves; ++octave)
        {
            total += weight * replay::camera::shake::Noise(Hash(seedBase + static_cast<std::uint32_t>(octave) * 0x68e31da4U), rate * t);
            norm += weight;
            weight *= band.persistence;
            rate *= kLacunarity;
        }
        return band.amp * total / norm;
    }

    // Amplitudes at intensity 5 (gain 1). Channels: pitch, roll, yaw in
    // degrees, then x, y, z in metres. Starting values, to tune.
    Profile MakeProfile(ShakeType type)
    {
        Profile p;
        Axis& pitch = p.axis[0];
        Axis& roll = p.axis[1];
        Axis& yaw = p.axis[2];
        Axis& x = p.axis[3];
        Axis& y = p.axis[4];
        Axis& z = p.axis[5];
        switch (type)
        {
        case ShakeType::Handheld:
            // Standing operator, camera in hand / on shoulder: rotation
            // dominant pink drift below 2 Hz, breathing, a faint tremor.
            pitch.band[0] = {0.18f, 0.70f, 4, 0.60f};
            pitch.band[1] = {0.27f, 0.12f, 1, 0.5f}; // breathing
            pitch.band[2] = {9.5f, 0.030f, 2, 0.5f};  // physiological tremor
            yaw.band[0] = {0.15f, 0.90f, 4, 0.60f};
            yaw.band[2] = {10.5f, 0.025f, 2, 0.5f};
            roll.band[0] = {0.12f, 0.45f, 3, 0.55f};
            roll.band[2] = {8.7f, 0.020f, 2, 0.5f};
            x.band[0] = {0.10f, 0.010f, 2, 0.5f};
            y.band[0] = {0.11f, 0.010f, 2, 0.5f};
            z.band[0] = {0.27f, 0.005f, 1, 0.5f}; // breathing
            z.band[1] = {0.10f, 0.005f, 2, 0.5f};
            break;
        case ShakeType::Walking:
            // Handheld operator walking: step bob, heel strike nod, side sway.
            p.gaitHz = 2.0f;
            p.gaitJitter = 0.05f;
            p.impactDecay = 0.05f;
            p.impactRingHz = 7.0f;
            pitch.band[0] = {0.20f, 0.60f, 4, 0.60f};
            pitch.band[2] = {9.5f, 0.030f, 2, 0.5f};
            pitch.bob = 0.25f;
            pitch.bobPhase = 0.30f;
            pitch.impact = -0.45f;
            yaw.band[0] = {0.16f, 0.70f, 4, 0.60f};
            yaw.band[2] = {10.5f, 0.025f, 2, 0.5f};
            yaw.sway = 0.35f;
            yaw.swayPhase = 0.25f;
            roll.band[0] = {0.14f, 0.30f, 3, 0.55f};
            roll.sway = 0.70f;
            x.band[0] = {0.12f, 0.010f, 2, 0.5f};
            x.sway = 0.020f;
            y.band[0] = {0.12f, 0.010f, 2, 0.5f};
            z.bob = 0.020f;
            z.bobPhase = 0.25f;
            z.impact = -0.006f;
            break;
        case ShakeType::Running:
            p.gaitHz = 2.9f;
            p.gaitJitter = 0.05f;
            p.impactDecay = 0.04f;
            p.impactRingHz = 9.0f;
            pitch.band[0] = {0.30f, 0.80f, 4, 0.60f};
            pitch.band[2] = {9.5f, 0.05f, 2, 0.5f};
            pitch.bob = 0.60f;
            pitch.bobPhase = 0.30f;
            pitch.impact = -1.0f;
            yaw.band[0] = {0.25f, 0.90f, 4, 0.60f};
            yaw.sway = 0.80f;
            yaw.swayPhase = 0.25f;
            roll.band[0] = {0.22f, 0.50f, 3, 0.55f};
            roll.sway = 1.30f;
            x.band[0] = {0.20f, 0.015f, 2, 0.5f};
            x.sway = 0.025f;
            y.band[0] = {0.20f, 0.015f, 2, 0.5f};
            z.bob = 0.045f;
            z.bobPhase = 0.25f;
            z.impact = -0.015f;
            break;
        case ShakeType::Horseback:
            // Rider at a gallop (2 Hz strides; canter 1.5 Hz at 75 % speed):
            // one rocking bounce per stride, the strike of the leading
            // foreleg, the horse wandering in heading.
            p.gaitHz = 2.0f;
            p.gaitJitter = 0.03f;
            p.impactDecay = 0.06f;
            p.impactRingHz = 6.0f;
            pitch.band[0] = {0.25f, 0.30f, 3, 0.55f};
            pitch.bob = 1.20f;
            pitch.bobPhase = 0.15f;
            pitch.impact = -0.70f;
            yaw.band[0] = {0.12f, 0.60f, 3, 0.55f};
            yaw.sway = 0.30f;
            roll.band[0] = {0.20f, 0.25f, 3, 0.55f};
            roll.bob = 0.40f;
            roll.bobPhase = 0.40f;
            roll.sway = 0.30f;
            x.sway = 0.010f;
            y.bob = 0.016f;
            y.bobPhase = 0.10f;
            z.bob = 0.045f;
            z.bobPhase = 0.25f;
            z.impact = -0.012f;
            break;
        case ShakeType::Steadicam:
            // Isolated rig: slow float, small residual step bob, no tremor.
            p.gaitHz = 1.8f;
            p.gaitJitter = 0.05f;
            pitch.band[0] = {0.10f, 0.12f, 2, 0.5f};
            yaw.band[0] = {0.08f, 0.18f, 2, 0.5f};
            roll.band[0] = {0.07f, 0.10f, 2, 0.5f};
            x.band[0] = {0.07f, 0.020f, 2, 0.5f};
            x.sway = 0.003f;
            y.band[0] = {0.08f, 0.020f, 2, 0.5f};
            z.band[0] = {0.12f, 0.006f, 2, 0.5f};
            z.bob = 0.004f;
            z.bobPhase = 0.25f;
            break;
        case ShakeType::Wagon:
            // Camera on a wagon / stagecoach / train: body bounce and pitch
            // near 1 to 1.5 Hz, roll sway, road rattle, intermittent bumps.
            p.gustHz = 0.35f;
            p.gustDepth = 0.60f;
            pitch.band[0] = {1.1f, 0.35f, 2, 0.5f};
            pitch.band[1] = {11.0f, 0.06f, 2, 0.5f};
            roll.band[0] = {0.6f, 0.60f, 2, 0.5f};
            roll.band[1] = {12.0f, 0.04f, 2, 0.5f};
            yaw.band[0] = {0.25f, 0.30f, 2, 0.5f};
            x.band[0] = {0.5f, 0.006f, 2, 0.5f};
            y.band[0] = {0.4f, 0.006f, 2, 0.5f};
            z.band[0] = {1.3f, 0.018f, 1, 0.5f};
            z.band[1] = {13.0f, 0.002f, 2, 0.5f};
            break;
        case ShakeType::Drunk:
            // Very slow, wide roll and heading sway, the body drifting.
            p.gustHz = 0.05f;
            p.gustDepth = 0.30f;
            pitch.band[0] = {0.09f, 1.5f, 2, 0.5f};
            roll.band[0] = {0.07f, 4.0f, 2, 0.45f};
            yaw.band[0] = {0.06f, 3.0f, 2, 0.5f};
            x.band[0] = {0.05f, 0.06f, 2, 0.5f};
            y.band[0] = {0.06f, 0.05f, 2, 0.5f};
            z.band[0] = {0.12f, 0.02f, 2, 0.5f};
            break;
        case ShakeType::GroundVibration:
            // Engine, train, stampede: vertical, narrowband, pulsing. At
            // first a fifth of this rotation (0.075 deg at most, under 2 px
            // at 1080p): it did not show. Now about 0.35 deg, near Handheld's.
            p.gaitHz = 19.0f; // the rumble carrier
            p.gaitJitter = 0.4f;
            p.gustHz = 0.4f;
            p.gustDepth = 0.5f;
            pitch.band[0] = {17.0f, 0.25f, 2, 0.5f};
            roll.band[0] = {15.0f, 0.20f, 2, 0.5f};
            yaw.band[0] = {13.0f, 0.10f, 1, 0.5f};
            x.band[0] = {16.0f, 0.0010f, 1, 0.5f};
            y.band[0] = {16.0f, 0.0010f, 1, 0.5f};
            z.band[0] = {22.0f, 0.0040f, 2, 0.5f};
            z.bob = 0.0040f;
            pitch.bob = 0.15f;
            pitch.bobPhase = 0.2f;
            break;
        case ShakeType::AirTurbulence:
            // Wind buffeting: broadband 0.5 to 6 Hz, gusty, roll first.
            p.gustHz = 0.18f;
            p.gustDepth = 0.80f;
            pitch.band[0] = {0.9f, 0.50f, 3, 0.6f};
            pitch.band[1] = {5.0f, 0.08f, 1, 0.5f};
            roll.band[0] = {0.7f, 0.70f, 3, 0.6f};
            yaw.band[0] = {0.6f, 0.35f, 3, 0.6f};
            x.band[0] = {0.5f, 0.010f, 2, 0.5f};
            y.band[0] = {0.5f, 0.010f, 2, 0.5f};
            z.band[0] = {0.8f, 0.020f, 2, 0.5f};
            break;
        default:
            break;
        }
        return p;
    }

    struct Tables
    {
        Profile profile[replay::editor::kShakeTypeCount];
        Tables()
        {
            for (std::uint8_t n = 0; n < replay::editor::kShakeTypeCount; ++n)
                profile[n] = MakeProfile(static_cast<ShakeType>(n));
        }
    };

    const Tables& GetTables()
    {
        static const Tables tables;
        return tables;
    }

    float SmoothStep01(double v)
    {
        const double c = std::clamp(v, 0.0, 1.0);
        return static_cast<float>(c * c * (3.0 - 2.0 * c));
    }

    // Explosion: a damped directional kick (the blast pushing the camera
    // and the operator recovering), a fast decaying high frequency rattle
    // and a slower wobble, one shot from the chain start. Below 1 % of the
    // peak after about 3 s.
    replay::camera::shake::Sample Explosion(float gain, double t)
    {
        replay::camera::shake::Sample out;
        if (t <= 0.0)
            return out;
        // kick, rattle, wobble per channel at intensity 5
        static constexpr float kick[ChannelCount] = {2.0f, 1.2f, 0.8f, 0.010f, 0.010f, 0.030f};
        static constexpr float rattle[ChannelCount] = {0.50f, 0.40f, 0.40f, 0.004f, 0.004f, 0.008f};
        static constexpr float wobble[ChannelCount] = {0.60f, 0.80f, 0.50f, 0.006f, 0.006f, 0.010f};
        static constexpr float kickPhase[ChannelCount] = {0.0f, 0.5f, 0.25f, 0.1f, 0.35f, 0.0f};
        const double kickEnv = std::exp(-t / 0.35);
        const double rattleEnv = std::exp(-t / 0.18);
        const double wobbleEnv = std::exp(-t / 0.55);
        for (int c = 0; c < ChannelCount; ++c)
        {
            // Sine from zero, phase offsets as a time delay per channel
            // (the rise is still from zero: sin(w t) with a softened start).
            const double k = std::sin(kTwoPi * 2.4 * t + kTwoPi * kickPhase[c]) - std::sin(kTwoPi * kickPhase[c]) * std::exp(-t / 0.02);
            const Band r{18.0f, rattle[c], 2, 0.5f};
            const Band w{3.0f, wobble[c], 2, 0.5f};
            const std::uint32_t seed = Seed(ShakeType::Explosion, c, 0, 0);
            out.value[c] = gain * static_cast<float>(kick[c] * kickEnv * k + rattleEnv * Fractal(seed, r, t) +
                                                     wobbleEnv * Fractal(seed ^ 0xa5a5a5a5U, w, t));
        }
        return out;
    }

    // Gunshot: the recoil of a shot fired near the camera, a short sharp
    // kick. Per channel a damped sine from zero (an upward nod first, a
    // little heading and roll, the camera pushed back and up a few mm) and
    // a fast 24 Hz rattle. At intensity 5 the nod peaks near 0.5 deg (about
    // a third of Explosion's kick); below 1 % of the peak after 0.3 s.
    replay::camera::shake::Sample Gunshot(float gain, double t)
    {
        replay::camera::shake::Sample out;
        if (t <= 0.0)
            return out;
        // kick (degrees, metres) and rattle per channel at intensity 5
        static constexpr float kick[ChannelCount] = {1.00f, 0.25f, 0.30f, 0.0015f, -0.0060f, 0.0030f};
        static constexpr float rattle[ChannelCount] = {0.06f, 0.05f, 0.04f, 0.0003f, 0.0003f, 0.0005f};
        static constexpr float kickHz[ChannelCount] = {6.0f, 7.5f, 5.0f, 6.0f, 5.5f, 6.0f};
        const double kickEnv = std::exp(-t / 0.055);
        const double rattleEnv = std::exp(-t / 0.03);
        for (int c = 0; c < ChannelCount; ++c)
        {
            const std::uint32_t seed = Seed(ShakeType::Gunshot, c, 0, 0);
            const Band r{24.0f, rattle[c], 1, 0.5f};
            out.value[c] = gain * static_cast<float>(kick[c] * kickEnv * std::sin(kTwoPi * kickHz[c] * t) +
                                                     rattleEnv * Fractal(seed, r, t));
        }
        return out;
    }
}

namespace replay::camera::shake
{
    float Gain(std::uint8_t intensity)
    {
        const float i = static_cast<float>(std::clamp<int>(intensity, 1, 10));
        const float g = (i + 1.0f) / 6.0f;
        return g * g;
    }

    float Noise(std::uint32_t seed, double x)
    {
        const double fl = std::floor(x);
        const std::int64_t i = static_cast<std::int64_t>(fl);
        const float f = static_cast<float>(x - fl);
        const float g0 = Unit(Hash2(seed, i));
        const float g1 = Unit(Hash2(seed, i + 1));
        const float fade = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
        const float a = g0 * f;
        const float b = g1 * (f - 1.0f);
        return 2.0f * (a + (b - a) * fade);
    }

    float GaitHz(ShakeType type)
    {
        const auto index = static_cast<std::uint8_t>(type);
        return index < editor::kShakeTypeCount ? GetTables().profile[index].gaitHz : 0.0f;
    }

    Sample Evaluate(ShakeType type, float gain, double t)
    {
        Sample out;
        const auto index = static_cast<std::uint8_t>(type);
        if (type == ShakeType::None || index >= editor::kShakeTypeCount || gain == 0.0f || !(t > 0.0))
            return out;
        if (type == ShakeType::Explosion)
            return Explosion(gain, t);
        if (type == ShakeType::Gunshot)
            return Gunshot(gain, t);
        if (type == ShakeType::Custom)
        {
            // Without its numbers (a caller that only has the type), the
            // defaults at their own frequency, no speed.
            const editor::ShakeCustom d = editor::DefaultShakeCustom();
            return EvaluateCustom(d, gain, static_cast<double>(d.hz) * t, 0.0f, 1.0f, t);
        }

        const Profile& p = GetTables().profile[index];
        const float fade = SmoothStep01(t / kFadeInSeconds);

        float gust = 1.0f;
        if (p.gustDepth > 0.0f)
        {
            // Mostly calm with bursts: squared, shifted noise, mean near 1.
            const float n = 0.5f + 0.5f * Noise(Seed(type, 7, 0, 0), t * p.gustHz * kLatticePerHz) * 1.4f;
            const float u = std::clamp(n, 0.0f, 1.0f);
            gust = (1.0f - p.gustDepth) + p.gustDepth * 2.2f * u * u;
        }

        // The gait clock: steps with a slowly wandering tempo (monotonic:
        // the jitter slope stays well below the gait rate).
        double phase = 0.0;
        double stepAge = 0.0;
        float stepStrength = 1.0f;
        if (p.gaitHz > 0.0f)
        {
            phase = p.gaitHz * t + p.gaitJitter * Noise(Seed(type, 6, 0, 0), t * 0.35);
            const double cycle = std::floor(phase);
            stepAge = (phase - cycle) / p.gaitHz;
            stepStrength = 0.8f + 0.2f * Unit(Hash2(Seed(type, 6, 1, 0), static_cast<std::int64_t>(cycle)));
        }
        const float impactShape = p.gaitHz > 0.0f
            ? static_cast<float>(std::exp(-stepAge / p.impactDecay) * std::sin(kTwoPi * p.impactRingHz * stepAge)) * stepStrength
            : 0.0f;

        for (int c = 0; c < ChannelCount; ++c)
        {
            const Axis& a = p.axis[c];
            float noise = 0.0f;
            for (int b = 0; b < 3; ++b)
                noise += Fractal(Seed(type, c, b, 0), a.band[b], t);
            float gait = 0.0f;
            if (p.gaitHz > 0.0f)
            {
                if (a.bob != 0.0f)
                    gait += a.bob * static_cast<float>(std::sin(kTwoPi * (phase + a.bobPhase)));
                if (a.sway != 0.0f)
                    gait += a.sway * static_cast<float>(std::sin(kTwoPi * (0.5 * phase + a.swayPhase)));
                gait += a.impact * impactShape;
            }
            out.value[c] = gain * fade * gust * (noise + gait);
        }
        return out;
    }
}
