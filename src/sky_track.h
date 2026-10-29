#pragma once

// The clouds of the clip over time. A replay editor records the weather
// every frame and plays with the game timers set to the recorded time, so
// the sky it draws is a function of the clip. RDR2's volumetric clouds move
// by accumulators the sky update integrates every frame from the game frame
// time and the wind (0x5F8640, build 1491.50): x -= wind * speed * frame
// time, then wrapped with fmod by a period. They start wherever the session
// left them, so without this two playbacks of a clip draw different clouds.
// The replay records them (guarded reads, sky_state.cpp) and pins them at the
// playhead every tick through the game's own setters (natives in
// script.cpp). Nothing accumulates: scrub and jump safe. Pure data and rules
// here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace replay
{
    // The accumulators, flat. Each belongs to a period group (Period below).
    namespace sky
    {
        enum Channel : std::uint32_t
        {
            kNoiseX, kNoiseY, kNoiseZ,          // _SET_CLOUD_NOISE      0x4078F60
            kPositionX, kPositionY, kPositionZ, // _SET_CLOUD_POSITION   0x3F29D10
            kLayer0X, kLayer0Y,                 // _SET_CLOUD_LAYER 0    0x4054160
            kLayer1X, kLayer1Y,                 // _SET_CLOUD_LAYER 1    0x4054170
            kDriftX, kDriftY,                   // 0x085C5B61A0114F32    0x4054180
            kRegionX, kRegionY,                 // 0x1FF8731BE1DFC0C0    0x3F28FB0
            kChannels
        };

        enum PeriodGroup : std::uint32_t
        {
            kPeriodNoise, kPeriodPosition, kPeriodLayer, kPeriodDrift, kPeriodRegion, kPeriodGroups
        };

        constexpr PeriodGroup GroupOf(std::uint32_t channel)
        {
            return channel <= kNoiseZ ? kPeriodNoise
                : channel <= kPositionZ ? kPeriodPosition
                : channel <= kLayer1Y ? kPeriodLayer
                : channel <= kDriftY ? kPeriodDrift
                : kPeriodRegion;
        }
    }

    struct SkySample
    {
        std::uint32_t timeMs = 0;
        float value[sky::kChannels] = {};
        // 0x40651E0: a third offset the same update integrates, with no
        // setter in the game: recorded to compare in the log, never applied.
        float unpinned[3] = {};
    };

    // The fmod period of each group, read once per recording (0: unknown,
    // the channel is then held, not interpolated).
    struct SkyPeriods
    {
        float value[sky::kPeriodGroups] = {};
    };

    namespace skytrack
    {
        // 10 Hz: the offsets move all the time (the wind never stops), so
        // every sample is kept; linear between them (the wind changes slowly).
        constexpr std::uint32_t kSampleMs = 100;

        inline bool Finite(const SkySample& s)
        {
            for (float v : s.value)
                if (!std::isfinite(v))
                    return false;
            for (float v : s.unpinned)
                if (!std::isfinite(v))
                    return false;
            return true;
        }

        inline bool Valid(const SkyPeriods& p)
        {
            for (float v : p.value)
                if (!std::isfinite(v) || v < 0.0f)
                    return false;
            return true;
        }

        // b - a the short way round a period (|d| <= period / 2); the plain
        // difference with no period.
        inline float WrapDelta(float a, float b, float period)
        {
            const float d = b - a;
            if (!(period > 0.0f))
                return d;
            return std::remainder(d, period);
        }

        // Appends when time moved on (the first sample always). A sample at
        // the same time replaces the last one (the stop's forced read).
        inline bool Add(std::vector<SkySample>& track, const SkySample& sample)
        {
            if (!Finite(sample))
                return false;
            if (!track.empty())
            {
                if (sample.timeMs < track.back().timeMs)
                    return false;
                if (sample.timeMs == track.back().timeMs)
                {
                    track.back() = sample;
                    return true;
                }
            }
            track.push_back(sample);
            return true;
        }

        // The state at timeMs: the samples around it interpolated, each
        // channel the short way round its period (a wrap of the engine's
        // fmod between two samples is not a jump back across the sky). A
        // channel whose period is unknown takes the earlier sample. Before
        // the first sample: the first; after the last: the last. False with
        // an empty track.
        inline bool At(const std::vector<SkySample>& track, const SkyPeriods& periods, std::uint32_t timeMs, SkySample& out)
        {
            if (track.empty())
                return false;
            const auto after = std::upper_bound(track.begin(), track.end(), timeMs,
                [](std::uint32_t t, const SkySample& s) { return t < s.timeMs; });
            if (after == track.begin())
            {
                out = track.front();
                out.timeMs = timeMs;
                return true;
            }
            const SkySample& a = *(after - 1);
            out = a;
            out.timeMs = timeMs;
            if (after == track.end())
                return true;
            const SkySample& b = *after;
            const std::uint32_t span = b.timeMs - a.timeMs;
            if (span == 0)
                return true;
            const float t = static_cast<float>(timeMs - a.timeMs) / static_cast<float>(span);
            for (std::uint32_t c = 0; c < sky::kChannels; ++c)
            {
                const float period = periods.value[sky::GroupOf(c)];
                if (!(period > 0.0f))
                    continue;
                float v = a.value[c] + WrapDelta(a.value[c], b.value[c], period) * t;
                // Kept inside the engine's own range (fmod: the sign of the value).
                v = std::fmod(v, period);
                out.value[c] = v;
            }
            for (std::uint32_t c = 0; c < 3; ++c)
                out.unpinned[c] = a.unpinned[c] + (b.unpinned[c] - a.unpinned[c]) * t;
            return true;
        }

        // The largest difference between two states over the channels, each
        // the short way round its period (the log's comparison).
        inline float Distance(const SkySample& a, const SkySample& b, const SkyPeriods& periods)
        {
            float worst = 0.0f;
            for (std::uint32_t c = 0; c < sky::kChannels; ++c)
                worst = std::max(worst, std::fabs(WrapDelta(a.value[c], b.value[c], periods.value[sky::GroupOf(c)])));
            return worst;
        }

        // Buffer trim and clip cut, as envtrack::Cut: the state at `baseMs`
        // first (at 0), the rest shifted by -baseMs, those after endMs dropped.
        inline std::vector<SkySample> Cut(const std::vector<SkySample>& in, const SkyPeriods& periods, std::uint32_t baseMs,
            std::uint32_t endMs)
        {
            std::vector<SkySample> out;
            SkySample atBase;
            if (At(in, periods, baseMs, atBase))
            {
                atBase.timeMs = 0;
                out.push_back(atBase);
            }
            for (const SkySample& sample : in)
            {
                if (sample.timeMs <= baseMs || sample.timeMs > endMs)
                    continue;
                SkySample shifted = sample;
                shifted.timeMs = sample.timeMs - baseMs;
                out.push_back(shifted);
            }
            return out;
        }

        // The buffer's trim keeps times as they are: the samples before
        // keepFrom go, the one in force there stays.
        inline std::size_t DropBefore(std::vector<SkySample>& track, std::uint32_t keepFrom)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.size() && track[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
                track.erase(track.begin(), track.begin() + static_cast<std::ptrdiff_t>(keep));
            return keep;
        }
    }
}
