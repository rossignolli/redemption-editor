#pragma once

// Frozen poses: a ped that walks while its recorded pose repeats exactly from
// one capture frame to the next. The body moves on (the entity matrix is read
// every frame) but the skeleton is the one of the frame before, so the replay
// plays the walk as a slide with a pose that jumps now and then ("laggy").
// The engine stops animating a ped every frame when it is off the game
// camera; the recording has to keep it animated, and in 1.0 it did not: the
// share of frozen pairs off camera went from 0 to 0.4% to 7 to 18%, with the
// peds in the camera's view unchanged. No test saw it (the core tests run no
// game, the benches ran the developer ini).
//
// The measure, per ped, on the pose track's own frames (30 Hz capture, about
// 33 ms apart):
//  - a pair is two consecutive frames kMinPairMs to kMaxPairMs apart (a
//    longer gap is a hole in the track, not a frame step);
//  - it counts when the ped moves at kMinSpeedMps or more (over the frames
//    around the pair, about 100 ms: one frame's step is noisy) and stands
//    within maxDistM of the game camera on the ground plane;
//  - a carried body (a wagon seat, a saddle) does not count: it moves while
//    its pose may rightly hold (a wagon driver of 1051-Valentine repeats
//    100% of his pairs, on camera as well);
//  - it is frozen when every bone's object space transform is the same as
//    in the frame before (SamePose, within the codec's epsilons doubled: a
//    relative keyframe re-sends a held value with its rounding);
//  - it is on camera when the ped lies inside the recorded game camera's
//    horizontal field of view at the later frame (InView: the vertical FOV
//    widened to 16:9, heading 0 faces +Y, counter-clockwise; the pitch is
//    left out, a ped above or below the frame still counts as in view).
// The verdict fails when the off camera share is over maxPct (kDefaultMaxPct).
//
// Pure: no engine, no clip file. ReplayClipTool frozen-poses feeds it the
// frames of every ped and the recorded game camera; unit tested in
// tests/frozen_pose_tests.cpp.

#include "pose_math.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::frozenpose
{
    constexpr float kMinSpeedMps = 0.8f;
    constexpr std::uint32_t kMinPairMs = 20;
    constexpr std::uint32_t kMaxPairMs = 50;
    // pose_stream.h StreamConfig: 0.0005 m and 0.002 rad, doubled.
    constexpr float kSameM = 0.001f;
    constexpr float kSameRad = 0.004f;
    constexpr float kDefaultMaxPct = 2.0f;
    constexpr float kDefaultMaxDistM = 40.0f;
    constexpr float kAspect = 16.0f / 9.0f;
    constexpr float kPi = 3.14159265358979323846f;

    enum Side : int
    {
        kOff = 0,
        kOn = 1,
    };

    inline float QuatAngle(const pose::Quat& a, const pose::Quat& b)
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

    // Two poses of the same skeleton (object space) are the same pose.
    inline bool SamePose(const pose::BoneQts* a, const pose::BoneQts* b, std::size_t bones)
    {
        if (!a || !b || bones == 0)
            return false;
        for (std::size_t n = 0; n < bones; ++n)
        {
            for (int axis = 0; axis < 3; ++axis)
                if (!(std::fabs(a[n].translation[axis] - b[n].translation[axis]) <= kSameM))
                    return false;
            if (!(QuatAngle(a[n].rotation, b[n].rotation) <= kSameRad))
                return false;
        }
        return true;
    }

    // The recorded game camera at a moment.
    struct View
    {
        float pos[3] = {};
        float yawDeg = 0.0f; // heading 0 faces +Y, counter-clockwise
        float fovDeg = 0.0f; // vertical
        bool valid = false;
    };

    inline float GroundDistance(const View& view, const float* p)
    {
        const float dx = p[0] - view.pos[0], dy = p[1] - view.pos[1];
        return std::sqrt(dx * dx + dy * dy);
    }

    // The point lies inside the camera's horizontal field of view.
    inline bool InView(const View& view, const float* p, float aspect = kAspect)
    {
        if (!view.valid || !(view.fovDeg > 0.0f))
            return false;
        const float dx = p[0] - view.pos[0], dy = p[1] - view.pos[1];
        const float d = std::sqrt(dx * dx + dy * dy);
        if (!(d > 1e-4f))
            return true; // at the camera
        const float yaw = view.yawDeg * kPi / 180.0f;
        float c = (dx * -std::sin(yaw) + dy * std::cos(yaw)) / d;
        c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
        const float halfH = std::atan(std::tan(view.fovDeg * 0.5f * kPi / 180.0f) * aspect);
        return std::acos(c) < halfH;
    }

    // One pose track frame of a ped.
    struct Frame
    {
        std::uint32_t timeMs = 0;
        float pos[3] = {};      // entity origin, world
        bool samePose = false;  // the pose of the frame before in the list (false for the first)
        bool carried = false;   // sits in a vehicle seat or rides a mount
    };

    struct Counts
    {
        std::uint32_t pairs[2] = {};  // walking pairs, [kOff] and [kOn]
        std::uint32_t frozen[2] = {}; // of those, the frozen ones
        double distanceSum = 0.0;     // camera distance over the counted pairs
        void Add(const Counts& other)
        {
            for (int side = 0; side < 2; ++side)
            {
                pairs[side] += other.pairs[side];
                frozen[side] += other.frozen[side];
            }
            distanceSum += other.distanceSum;
        }
        std::uint32_t Pairs() const { return pairs[kOff] + pairs[kOn]; }
        float MeanDistance() const { return Pairs() ? static_cast<float>(distanceSum / Pairs()) : 0.0f; }
    };

    inline float Distance3(const float* a, const float* b)
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Speed at the pair (index - 1, index), over one more frame on each side
    // when the track has it without a hole.
    inline float PairSpeed(const std::vector<Frame>& frames, std::size_t index)
    {
        std::size_t lo = index - 1, hi = index;
        if (lo > 0 && frames[lo].timeMs - frames[lo - 1].timeMs <= kMaxPairMs)
            --lo;
        if (hi + 1 < frames.size() && frames[hi + 1].timeMs - frames[hi].timeMs <= kMaxPairMs)
            ++hi;
        const std::uint32_t dt = frames[hi].timeMs - frames[lo].timeMs;
        return dt ? Distance3(frames[hi].pos, frames[lo].pos) * 1000.0f / static_cast<float>(dt) : 0.0f;
    }

    // Adds a ped's frames (sorted by time) to `counts`. viewAt(timeMs) gives
    // the View at that moment.
    template <class ViewAt>
    void CountPairs(const std::vector<Frame>& frames, ViewAt&& viewAt, float maxDistM, Counts& counts)
    {
        for (std::size_t n = 1; n < frames.size(); ++n)
        {
            const std::uint32_t dt = frames[n].timeMs - frames[n - 1].timeMs;
            if (frames[n].timeMs < frames[n - 1].timeMs || dt < kMinPairMs || dt > kMaxPairMs)
                continue;
            if (frames[n].carried || frames[n - 1].carried)
                continue;
            if (!(PairSpeed(frames, n) >= kMinSpeedMps))
                continue;
            const View view = viewAt(frames[n].timeMs);
            if (!view.valid)
                continue;
            const float distance = GroundDistance(view, frames[n].pos);
            if (!(distance <= maxDistM))
                continue;
            const int side = InView(view, frames[n].pos) ? kOn : kOff;
            ++counts.pairs[side];
            if (frames[n].samePose)
                ++counts.frozen[side];
            counts.distanceSum += distance;
        }
    }

    inline float Percent(std::uint32_t part, std::uint32_t whole)
    {
        return whole ? 100.0f * static_cast<float>(part) / static_cast<float>(whole) : 0.0f;
    }

    inline float OffPercent(const Counts& counts) { return Percent(counts.frozen[kOff], counts.pairs[kOff]); }
    inline float OnPercent(const Counts& counts) { return Percent(counts.frozen[kOn], counts.pairs[kOn]); }

    // The check fails when the off camera share is over maxPct. No walking
    // pair off camera passes (nothing to judge).
    inline bool Fails(const Counts& total, float maxPct) { return OffPercent(total) > maxPct; }
}
