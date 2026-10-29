#pragma once

// Realistic procedural camera shake (pure, deterministic, seedless).
//
// Every type is a profile over six channels (pitch, roll, heading in degrees;
// x, y, z in world metres) built from:
//   - fractal gradient noise bands (1D Perlin, hash based, octaves with a
//     pink-ish persistence), which give the drift, sway and tremor;
//   - a gait clock (steps / strides with a little timing jitter) driving a
//     periodic bob, a half rate side sway and a damped heel strike impulse;
//   - an optional slow gust envelope (turbulence, rough road bumps);
//   - a one shot damped kick with a decaying rattle (Explosion).
// The result depends only on (type, gain, time): scrub equals play. Every
// channel is continuous in time, and continuous types fade in over the first
// 250 ms of shake time so a shake never pops when it starts.

#include "editor_protocol.h"

#include <cstdint>

namespace replay::camera::shake
{
    using editor::ShakeType;

    enum Channel : int
    {
        Pitch = 0, // degrees
        Roll = 1,  // degrees
        Yaw = 2,   // degrees (heading)
        X = 3,     // world metres
        Y = 4,     // world metres
        Z = 5,     // world metres (up)
        ChannelCount = 6
    };

    struct Sample
    {
        float value[ChannelCount] = {};
    };

    // Perceptual gain from the 1..10 intensity, trauma style (squared):
    // ((i + 1) / 6)^2, so 1 -> 0.11, 5 -> 1.0 (the profile tables), 10 -> 3.36.
    float Gain(std::uint8_t intensity);

    // The shake at `shakeTimeSeconds` (the chain warped shake clock; the
    // shake speed is already in it), scaled by `gain`.
    Sample Evaluate(ShakeType type, float gain, double shakeTimeSeconds);

    // Hash based 1D gradient noise in about [-1, 1], zero at integer x, C2.
    float Noise(std::uint32_t seed, double x);

    // A one shot type: an impulse from its start that settles (Explosion:
    // below 1 % of its peak after about 3 s; Gunshot: a recoil kick, below
    // 1 % after about 0.3 s), not a pattern that goes on.
    // A marker that sets one fires it there (mshake::FiresImpulse).
    constexpr bool OneShot(ShakeType type)
    {
        return type == ShakeType::Explosion || type == ShakeType::Gunshot;
    }

    // The frequency (Hz) of the profile's dominant rhythm, 0 when it is
    // broadband only (tests and the bench HUD).
    float GaitHz(ShakeType type);
}
