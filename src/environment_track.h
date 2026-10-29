#pragma once

// The clip's environment over time: the weather types and their transition
// mix, the clock, the wind and the rain and snow levels, sampled a few times
// per second and kept when they change, so rain starting or the light of a
// long clip changing plays back as recorded. Playback reads the state at the
// playhead (scrub and jump safe: nothing accumulates). Pure data and rules;
// the natives live in script.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace replay
{
    struct EnvironmentSample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t weatherFrom = 0;
        std::uint32_t weatherTo = 0;
        float weatherMix = 0.0f; // 0: weatherFrom, 1: weatherTo
        std::uint32_t clockS = 0; // seconds since midnight
        float windSpeed = 0.0f;
        float windDirection = 0.0f; // radians, from GET_WIND_DIRECTION (x, y)
        float rain = 0.0f;
        float snow = 0.0f;
        // The weather manager's transition "over time" (SET_WEATHER_TYPE with
        // a time, as the missions do), which GET_CURR_WEATHER_STATE does not
        // return: the target type (hash, 0 none), the total and the time left
        // in the engine's own unit (milliseconds, offline 0x7340BC multiplies
        // the seconds by 1000.0) and the manager's flag byte. transitionKnown
        // is false in an ENVT v1 clip or when the recorder could not read the
        // manager: the replay then leaves the engine's transition alone.
        std::uint32_t transitionTo = 0;
        float transitionTotalMs = 0.0f;
        float transitionLeftMs = 0.0f;
        std::uint8_t transitionFlag = 0;
        bool transitionKnown = false;
    };

    namespace envtrack
    {
        constexpr std::uint32_t kSampleMs = 250; // 4 Hz
        constexpr std::uint32_t kDaySeconds = 24u * 60u * 60u;

        // Kept when the weather or a level moved, or the clock moved on by
        // more than this (the clock runs all the time: 2 s of game time per
        // real second at the default rate).
        constexpr std::uint32_t kClockStepS = 15;

        inline std::uint32_t ClockDelta(std::uint32_t from, std::uint32_t to)
        {
            return (to + kDaySeconds - from % kDaySeconds) % kDaySeconds;
        }

        // The time left of a transition runs down by itself (the engine takes
        // the frame time off every frame, offline 0x75FFEC); a sample is kept
        // when it strays from that straight line by this much (a pause, slow
        // motion, a script restarting it), so a 40 s transition costs its first
        // and its last sample.
        constexpr float kTransitionDriftMs = 250.0f;

        inline bool TransitionChanged(const EnvironmentSample& last, const EnvironmentSample& now)
        {
            if (last.transitionKnown != now.transitionKnown || last.transitionTo != now.transitionTo ||
                last.transitionFlag != now.transitionFlag)
                return true;
            if (!now.transitionKnown || now.transitionTo == 0)
                return false;
            if (std::fabs(last.transitionTotalMs - now.transitionTotalMs) >= 1.0f)
                return true;
            const float elapsed = static_cast<float>(now.timeMs - last.timeMs);
            return std::fabs(last.transitionLeftMs - elapsed - now.transitionLeftMs) >= kTransitionDriftMs;
        }

        inline bool Changed(const EnvironmentSample& last, const EnvironmentSample& now)
        {
            if (now.timeMs >= last.timeMs && TransitionChanged(last, now))
                return true;
            if (last.weatherFrom != now.weatherFrom || last.weatherTo != now.weatherTo)
                return true;
            if (std::fabs(last.weatherMix - now.weatherMix) >= 0.005f)
                return true;
            if (std::fabs(last.windSpeed - now.windSpeed) >= 0.1f || std::fabs(last.windDirection - now.windDirection) >= 0.05f)
                return true;
            if (std::fabs(last.rain - now.rain) >= 0.01f || std::fabs(last.snow - now.snow) >= 0.01f)
                return true;
            const std::uint32_t moved = ClockDelta(last.clockS, now.clockS);
            // A clock set backwards (a script, the bench) wraps to a big delta: a change too.
            return moved >= kClockStepS;
        }

        // Appends when it changed (the first sample always).
        inline bool Add(std::vector<EnvironmentSample>& track, const EnvironmentSample& sample)
        {
            if (!track.empty() && sample.timeMs < track.back().timeMs)
                return false;
            if (!track.empty() && !Changed(track.back(), sample))
                return false;
            track.push_back(sample);
            return true;
        }

        inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

        inline float LerpAngle(float a, float b, float t)
        {
            const float pi = 3.14159265f;
            float d = std::fmod(b - a, 2.0f * pi);
            if (d > pi)
                d -= 2.0f * pi;
            if (d < -pi)
                d += 2.0f * pi;
            return a + d * t;
        }

        // The same transition in two samples (target, total and flag), so the
        // time left between them is a straight line.
        inline bool SameTransition(const EnvironmentSample& a, const EnvironmentSample& b)
        {
            return a.transitionKnown && b.transitionKnown && a.transitionTo != 0 && a.transitionTo == b.transitionTo &&
                a.transitionFlag == b.transitionFlag && std::fabs(a.transitionTotalMs - b.transitionTotalMs) < 1.0f;
        }

        // The time left at timeMs after sample `a` (b the next sample, or
        // null after the last): run down at the engine's pace (1 ms a ms)
        // from a, never below 0, and inside the same transition never below
        // b's (the recorder keeps a sample as soon as the time left strays
        // from that line, so the line is right up to the stray: a straight
        // interpolation from a to b would spread it over the whole span). A
        // transition that ended (the engine made the target the weather and
        // cleared the fields) is the next sample's new pair; until then the
        // last one keeps running.
        inline float TransitionLeftAt(const EnvironmentSample& a, const EnvironmentSample* b, std::uint32_t timeMs)
        {
            if (!a.transitionKnown || a.transitionTo == 0 || timeMs <= a.timeMs)
                return a.transitionLeftMs;
            float left = a.transitionLeftMs - static_cast<float>(timeMs - a.timeMs);
            if (left < 0.0f)
                left = 0.0f;
            if (b && SameTransition(a, *b) && b->transitionLeftMs <= a.transitionLeftMs && left < b->transitionLeftMs)
                left = b->transitionLeftMs;
            return left;
        }

        // The state at timeMs: the samples around it interpolated (the
        // clock forward across midnight; the mix only between samples of
        // the same pair of weather types, otherwise the earlier sample's).
        // False with an empty track.
        inline bool At(const std::vector<EnvironmentSample>& track, std::uint32_t timeMs, EnvironmentSample& out)
        {
            if (track.empty())
                return false;
            const auto after = std::upper_bound(track.begin(), track.end(), timeMs,
                [](std::uint32_t t, const EnvironmentSample& s) { return t < s.timeMs; });
            if (after == track.begin())
            {
                out = track.front();
                out.timeMs = timeMs;
                return true;
            }
            const EnvironmentSample& a = *(after - 1);
            out = a;
            out.timeMs = timeMs;
            // Before the pair check below (the transition ends in a new pair,
            // and runs until then).
            out.transitionLeftMs = TransitionLeftAt(a, after == track.end() ? nullptr : &*after, timeMs);
            if (after == track.end())
                return true;
            const EnvironmentSample& b = *after;
            const std::uint32_t span = b.timeMs - a.timeMs;
            if (span == 0)
                return true;
            const float t = static_cast<float>(timeMs - a.timeMs) / static_cast<float>(span);
            // A new pair of weather types is a switch (a script set it):
            // nothing blends across it.
            if (a.weatherFrom != b.weatherFrom || a.weatherTo != b.weatherTo)
                return true;
            out.weatherMix = Lerp(a.weatherMix, b.weatherMix, t);
            out.windSpeed = Lerp(a.windSpeed, b.windSpeed, t);
            out.windDirection = LerpAngle(a.windDirection, b.windDirection, t);
            out.rain = Lerp(a.rain, b.rain, t);
            out.snow = Lerp(a.snow, b.snow, t);
            const std::uint32_t moved = ClockDelta(a.clockS, b.clockS);
            // A jump back (clock set by a script) is not interpolated.
            if (moved < kDaySeconds / 2)
                out.clockS = (a.clockS + static_cast<std::uint32_t>(static_cast<float>(moved) * t + 0.5f)) % kDaySeconds;
            return true;
        }

        // Buffer trim and clip cut: the samples before `baseMs` go but the
        // one in force at the base (moved to it), the rest shifted by
        // -baseMs and those after endMs dropped.
        inline std::vector<EnvironmentSample> Cut(const std::vector<EnvironmentSample>& in, std::uint32_t baseMs, std::uint32_t endMs)
        {
            std::vector<EnvironmentSample> out;
            EnvironmentSample atBase;
            if (At(in, baseMs, atBase))
            {
                atBase.timeMs = 0;
                out.push_back(atBase);
            }
            for (const EnvironmentSample& sample : in)
            {
                if (sample.timeMs <= baseMs || sample.timeMs > endMs)
                    continue;
                EnvironmentSample shifted = sample;
                shifted.timeMs = sample.timeMs - baseMs;
                out.push_back(shifted);
            }
            return out;
        }

        // The buffer's trim keeps times as they are (no shift): the
        // samples before keepFrom go, the one in force there stays.
        inline std::size_t DropBefore(std::vector<EnvironmentSample>& track, std::uint32_t keepFrom)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.size() && track[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
                track.erase(track.begin(), track.begin() + static_cast<std::ptrdiff_t>(keep));
            return keep;
        }

        inline bool Valid(const EnvironmentSample& s)
        {
            return std::isfinite(s.weatherMix) && std::isfinite(s.windSpeed) && std::isfinite(s.windDirection) &&
                std::isfinite(s.rain) && std::isfinite(s.snow) && s.clockS < kDaySeconds &&
                std::isfinite(s.transitionTotalMs) && std::isfinite(s.transitionLeftMs) && s.transitionTotalMs >= 0.0f;
        }

        // What the replay writes into the manager's transition fields for a
        // recorded total and time left, every frame. The progress is the
        // engine's own (1 - min(left / total, 1), offline 0x75FFEC). The engine
        // takes the frame time off the time left every frame and, at 0, makes
        // the target the weather and clears the fields (0x733F34): the time
        // left written never goes below kMinLeftMs while the clip plays, and the
        // total grows with it so the progress stays the recorded one (capped at
        // kMaxProgress).
        constexpr float kMinLeftMs = 1000.0f;
        constexpr float kMaxProgress = 0.999f;

        struct TransitionWrite
        {
            float totalMs = 0.0f;
            float leftMs = 0.0f;
            float progress = 0.0f;
        };

        inline float TransitionProgress(float totalMs, float leftMs)
        {
            if (!(totalMs > 0.0f))
                return 1.0f;
            const float ratio = (leftMs > 0.0f ? leftMs : 0.0f) / totalMs;
            return 1.0f - (ratio < 1.0f ? ratio : 1.0f);
        }

        inline TransitionWrite TransitionFields(float totalMs, float leftMs)
        {
            TransitionWrite out;
            float progress = TransitionProgress(totalMs, leftMs);
            if (progress > kMaxProgress)
                progress = kMaxProgress;
            out.progress = progress;
            const float keep = 1.0f - progress; // > 0
            out.totalMs = totalMs > 0.0f ? totalMs : kMinLeftMs;
            out.leftMs = out.totalMs * keep;
            if (out.leftMs < kMinLeftMs)
            {
                out.leftMs = kMinLeftMs;
                out.totalMs = kMinLeftMs / keep;
            }
            return out;
        }

        // SET_WIND_DIRECTION takes degrees (offline 0x73A01C multiplies by
        // pi/180) and stores the vector (-sin d, -cos d, 0) (0x10594BC); a
        // negative value releases it. The track keeps GET_WIND_DIRECTION's
        // vector as atan2(y, x) (radians), so the degrees that give that vector
        // back, in [0, 360).
        constexpr float kPi = 3.14159265358979f;

        inline float WindDegreesFromVector(float x, float y)
        {
            float d = std::atan2(-x, -y) * (180.0f / kPi);
            if (d < 0.0f)
                d += 360.0f;
            if (!(d < 360.0f) || !(d >= 0.0f))
                d = 0.0f;
            return d;
        }

        inline float WindDegreesFromHeading(float headingRad)
        {
            return WindDegreesFromVector(std::cos(headingRad), std::sin(headingRad));
        }
    }
}
