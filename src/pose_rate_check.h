#pragma once

// Pose rate: was a ped's animation recorded at the capture rate, or at a lower
// one (the engine updated the skeleton only every 2nd or 3rd frame: anim LOD,
// timeslicing)? And does the recorded pose flip for a single frame (a bone
// that leaves and comes back, e.g. an expression switched off for one frame)?
//
// The measure, per bone, on three consecutive recorded frames A, B, C (local
// rotations, the bone's own animation):
//  - the triple counts when the bone turned more than kMinSpanRad from A to C
//    (five times the codec's rotation epsilon, so a bone the encoder held
//    under its epsilon does not count);
//  - it is held when one of the two steps (A to B, B to C) is under
//    kHeldShare of the span: the bone stood still for a frame while it moved
//    over the two. A full rate animation holds a few percent (bones that
//    start, stop or turn back); one updated every 2nd or 3rd frame holds
//    every counted triple (stair steps: A A C, A C C);
//  - it is a flip when both steps exceed kFlipRad and A and C are within
//    kFlipShare of the smaller step: B is a one frame excursion.
// A window is stair stepped when its held share is kStairPct or more.
//
// Measured on marybeth.rpclip (story mode camp, a scripted scene played with
// the gameplay camera from about 36 to 85 s, 30 Hz capture): 2 to 8% held
// for Arthur, Marybeth and John inside the scene and out of it (15% at most
// in a near still window), 4.1% over the 21 peds of the clip; the step sizes
// in proportion to the frame time (17, 33 and 50 ms steps alike); one frame
// flips no more frequent in the scene than out of it.
//
// Pure: no engine, no clip file. ReplayClipTool pose-rate feeds it the frames
// of every ped; unit tested in tests/pose_rate_tests.cpp.

#include "pose_math.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace replay::poserate
{
    constexpr float kMinSpanRad = 0.01f;
    constexpr float kHeldShare = 0.1f;
    constexpr float kFlipRad = 0.05f;
    constexpr float kFlipShare = 0.3f;
    constexpr float kStairPct = 30.0f;
    // Frames further apart than this are a hole in the track, not a step.
    constexpr std::uint32_t kMaxStepMs = 100;

    inline float Angle(const pose::Quat& a, const pose::Quat& b)
    {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        dot = dot < 0.0f ? -dot : dot;
        const float na = a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w;
        const float nb = b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w;
        if (!(na > 1e-8f) || !(nb > 1e-8f))
            return 0.0f;
        dot /= std::sqrt(na * nb);
        return dot >= 1.0f ? 0.0f : 2.0f * std::acos(dot);
    }

    struct Counts
    {
        std::uint64_t triples = 0; // frame triples measured
        std::uint64_t moving = 0;  // bone triples that turned more than kMinSpanRad
        std::uint64_t held = 0;    // of those, a step under kHeldShare of the span
        std::uint64_t flips = 0;   // bone triples with a one frame excursion
        std::uint64_t flipFrames = 0; // frame triples with at least one flip

        void Add(const Counts& other)
        {
            triples += other.triples;
            moving += other.moving;
            held += other.held;
            flips += other.flips;
            flipFrames += other.flipFrames;
        }
        float HeldPercent() const { return moving ? 100.0f * static_cast<float>(held) / static_cast<float>(moving) : 0.0f; }
        bool Stairs() const { return moving > 0 && HeldPercent() >= kStairPct; }
    };

    // Three consecutive frames of one skeleton (local rotations, `bones` each,
    // BoneQts rows). Frames more than kMaxStepMs apart are not measured.
    inline void MeasureTriple(const pose::BoneQts* a, const pose::BoneQts* b, const pose::BoneQts* c, std::size_t bones,
        std::uint32_t timeA, std::uint32_t timeB, std::uint32_t timeC, Counts& counts)
    {
        if (!a || !b || !c || bones == 0 || timeB <= timeA || timeC <= timeB || timeB - timeA > kMaxStepMs ||
            timeC - timeB > kMaxStepMs)
        {
            return;
        }
        ++counts.triples;
        bool flipped = false;
        for (std::size_t n = 0; n < bones; ++n)
        {
            const float first = Angle(a[n].rotation, b[n].rotation);
            const float second = Angle(b[n].rotation, c[n].rotation);
            const float span = Angle(a[n].rotation, c[n].rotation);
            if (span > kMinSpanRad)
            {
                ++counts.moving;
                if ((first < second ? first : second) < kHeldShare * span)
                    ++counts.held;
            }
            if (first > kFlipRad && second > kFlipRad && span < kFlipShare * (first < second ? first : second))
            {
                ++counts.flips;
                flipped = true;
            }
        }
        if (flipped)
            ++counts.flipFrames;
    }
}
