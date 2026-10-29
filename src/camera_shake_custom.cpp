#include "camera_shake_custom.h"

#include <algorithm>
#include <cmath>

namespace replay::camera::shake
{
    namespace
    {
        constexpr double kTwoPi = 6.283185307179586;
        constexpr float kFadeInSeconds = 0.25f;

        std::uint32_t Mix(std::uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }

        float Unit01(std::uint32_t h) { return static_cast<float>(h >> 8) / 16777215.0f; }
    }

    float CustomNoise(double phase, float seed)
    {
        const double s = seed;
        return static_cast<float>(std::sin(phase + s) * 0.5714 + std::sin(phase * 2.03 + s * 1.7) * 0.2857 +
            std::sin(phase * 4.11 + s * 2.3) * 0.1429);
    }

    void CustomPattern(std::uint16_t seed, float (&phaseSeed)[ChannelCount], float (&multiplier)[ChannelCount])
    {
        // Simple Camera's own table for pattern 0 (its channels x, y, z,
        // pitch, yaw, roll, here in this file's channel order).
        static const float kSeeds[ChannelCount] = {53.83f, 97.41f, 11.27f, 17.31f, 41.07f, 73.59f};
        static const float kMultipliers[ChannelCount] = {0.893f, 1.211f, 1.131f, 1.000f, 1.073f, 0.927f};
        for (int c = 0; c < ChannelCount; ++c)
        {
            if (seed == 0)
            {
                phaseSeed[c] = kSeeds[c];
                multiplier[c] = kMultipliers[c];
                continue;
            }
            const std::uint32_t base = Mix(0xC0FFEEu + static_cast<std::uint32_t>(seed) * 2654435761u);
            phaseSeed[c] = Unit01(Mix(base + static_cast<std::uint32_t>(c) * 0x9E3779B9u)) * 1000.0f;
            multiplier[c] = 0.80f + Unit01(Mix(base ^ (0x85EBCA6Bu + static_cast<std::uint32_t>(c) * 0xC2B2AE35u))) * 0.45f;
        }
    }

    Sample EvaluateCustom(const ShakeCustom& c, float gain, double cycles, float speedNorm, float still, double runSeconds)
    {
        Sample out;
        if (!(gain > 0.0f) || !(c.amp > 0.0f) || !(runSeconds > 0.0) || !(still > 0.0f))
            return out;
        float seeds[ChannelCount];
        float multipliers[ChannelCount];
        CustomPattern(c.seed, seeds, multipliers);
        const float speed = std::clamp(speedNorm, 0.0f, 1.0f);
        const float effective = c.amp * (1.0f + c.speedAmp * speed);
        const float fade = static_cast<float>(std::clamp(runSeconds / kFadeInSeconds, 0.0, 1.0));
        const float smooth = fade * fade * (3.0f - 2.0f * fade);
        const float scale = gain * smooth * std::clamp(still, 0.0f, 1.0f);
        const float rotation = effective * 1.0f * c.rotWeight * scale;   // degrees
        const float position = effective * 0.05f * c.posWeight * scale;  // metres
        const double phase = cycles * kTwoPi;
        for (int ch = 0; ch < ChannelCount; ++ch)
        {
            const float n = CustomNoise(phase * multipliers[ch], seeds[ch]);
            out.value[ch] = n * (ch < X ? rotation : position);
        }
        return out;
    }
}
