#pragma once

// The moon of the clip. The clock alone (environment_track.h) does not give
// it: the moon is not a function of the clock.
//
// The sky object keeps two double angles (moon turn and moon orbit) that the
// engine integrates from the game clock's per-frame delta and never saves, so
// the moon's place is session state. A clock jump (including the replay's own)
// moves them.
//
// What the replay does (script.cpp): a clip that sets the clock saves both
// angles when it opens and writes them back for a few frames after the clock
// is given back (also after a script reload and at unload). The recording
// keeps both angles (section MOON, 4 Hz, on change); with the Moon phase row
// Recorded, playback writes them at the playhead every tick and the engine
// computes the moon, shadows and sky from them.
//
// Evidence: offline dump, build 1491.50. No other reader or writer of the two
// fields was found.
// Research: docs/research/weather-wind-sky-timecycle.md
//
// Pure data and rules here.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay
{
    struct MoonSample
    {
        std::uint32_t timeMs = 0;
        double angle = 0.0; // sky +0xA80, radians
        double orbit = 0.0; // sky +0x8E8, radians
        // Evidence only (the log's comparison, never written): the drawn
        // sun +0xA90 and moon +0xAA0, and the clock's date.
        float sun[3] = {};
        float moon[3] = {};
        std::uint16_t year = 0;
        std::uint8_t month = 0; // GET_CLOCK_MONTH as returned
        std::uint8_t day = 0;   // GET_CLOCK_DAY_OF_MONTH
    };

    namespace moontrack
    {
        // 4 Hz, as the environment track; a sample is kept when an angle
        // moved (at night the turn angle moves every poll, by day only the
        // orbit, slowly).
        constexpr std::uint32_t kSampleMs = 250;
        // The update's fmod divisor: a double constant (2pi rounded to
        // a float, then widened).
        constexpr double kTurn = 6.2831854820251465;
        constexpr double kEpsilon = 1e-4;
        // Between two kept samples a move this large is a clock jump (a
        // mission's SET_CLOCK_TIME): held until the later sample, not swept.
        // The fastest normal move is the turn angle at night: 2pi / 9000 per
        // game second, about 0.005 rad per 250 ms at the default 30 game
        // seconds per second.
        constexpr double kJump = 0.25;
        // The engine keeps both inside (-2pi, 2pi); a read far outside is
        // not this field.
        constexpr double kLimit = 64.0;

        inline bool Finite(const MoonSample& s)
        {
            if (!std::isfinite(s.angle) || !std::isfinite(s.orbit))
                return false;
            for (int n = 0; n < 3; ++n)
                if (!std::isfinite(s.sun[n]) || !std::isfinite(s.moon[n]))
                    return false;
            return true;
        }

        inline bool Valid(const MoonSample& s)
        {
            return Finite(s) && std::fabs(s.angle) <= kLimit && std::fabs(s.orbit) <= kLimit && s.month <= 12 && s.day <= 31;
        }

        // b - a the short way round a turn (|d| <= pi).
        inline double WrapDelta(double a, double b) { return std::remainder(b - a, kTurn); }

        // Inside the engine's own range (fmod: the sign of the value).
        inline double Wrap(double v) { return std::fmod(v, kTurn); }

        inline bool SameDate(const MoonSample& a, const MoonSample& b)
        {
            return a.year == b.year && a.month == b.month && a.day == b.day;
        }

        inline bool Changed(const MoonSample& last, const MoonSample& now)
        {
            return std::fabs(WrapDelta(last.angle, now.angle)) >= kEpsilon || std::fabs(WrapDelta(last.orbit, now.orbit)) >= kEpsilon ||
                !SameDate(last, now);
        }

        // The recording side, as tctrack::Recorder: a change keeps the sample
        // and, when the state had held since an older kept sample, the last
        // unchanged poll before it (the interpolation does not stretch a move
        // back over the time the moon held).
        struct Recorder
        {
            MoonSample held;
            bool heldValid = false;

            void Reset() { heldValid = false; }

            // True when the sample was kept.
            bool Poll(std::vector<MoonSample>& track, const MoonSample& sample)
            {
                if (!Valid(sample))
                    return false;
                if (track.empty())
                {
                    track.push_back(sample);
                    heldValid = false;
                    return true;
                }
                if (sample.timeMs < track.back().timeMs)
                    return false;
                if (!Changed(track.back(), sample))
                {
                    held = sample;
                    heldValid = true;
                    return false;
                }
                if (heldValid && held.timeMs > track.back().timeMs && held.timeMs < sample.timeMs)
                    track.push_back(held);
                heldValid = false;
                if (sample.timeMs == track.back().timeMs)
                    track.back() = sample;
                else
                    track.push_back(sample);
                return true;
            }

            // The state at the stop is always the last sample.
            bool Finish(std::vector<MoonSample>& track, const MoonSample& sample)
            {
                if (Poll(track, sample))
                    return true;
                if (!Valid(sample) || track.empty() || sample.timeMs <= track.back().timeMs)
                    return false;
                track.push_back(sample);
                heldValid = false;
                return true;
            }
        };

        // The state at timeMs: both angles interpolated the short way round
        // (a wrap of the engine's fmod between two samples is not a turn
        // back) and kept in the engine's range; a jump (kJump) holds the
        // earlier sample. The directions are interpolated for the log, the
        // date is the earlier sample's. Before the first sample: the first;
        // after the last: the last. False with an empty track.
        inline bool At(const std::vector<MoonSample>& track, std::uint32_t timeMs, MoonSample& out)
        {
            if (track.empty())
                return false;
            const auto after = std::upper_bound(track.begin(), track.end(), timeMs,
                [](std::uint32_t t, const MoonSample& s) { return t < s.timeMs; });
            if (after == track.begin())
            {
                out = track.front();
                out.timeMs = timeMs;
                return true;
            }
            const MoonSample& a = *(after - 1);
            out = a;
            out.timeMs = timeMs;
            if (after == track.end())
                return true;
            const MoonSample& b = *after;
            const std::uint32_t span = b.timeMs - a.timeMs;
            if (span == 0)
                return true;
            const double dAngle = WrapDelta(a.angle, b.angle);
            const double dOrbit = WrapDelta(a.orbit, b.orbit);
            if (std::fabs(dAngle) > kJump || std::fabs(dOrbit) > kJump)
                return true;
            const double t = static_cast<double>(timeMs - a.timeMs) / static_cast<double>(span);
            out.angle = Wrap(a.angle + dAngle * t);
            out.orbit = Wrap(a.orbit + dOrbit * t);
            const float tf = static_cast<float>(t);
            for (int n = 0; n < 3; ++n)
            {
                out.sun[n] = a.sun[n] + (b.sun[n] - a.sun[n]) * tf;
                out.moon[n] = a.moon[n] + (b.moon[n] - a.moon[n]) * tf;
            }
            return true;
        }

        // Buffer trim and clip cut, as tctrack::Cut: the state at baseMs
        // first (at 0), the rest shifted by -baseMs, those after endMs dropped.
        inline std::vector<MoonSample> Cut(const std::vector<MoonSample>& in, std::uint32_t baseMs, std::uint32_t endMs)
        {
            std::vector<MoonSample> out;
            MoonSample atBase;
            if (At(in, baseMs, atBase))
            {
                atBase.timeMs = 0;
                out.push_back(atBase);
            }
            for (const MoonSample& sample : in)
            {
                if (sample.timeMs <= baseMs || sample.timeMs > endMs)
                    continue;
                MoonSample shifted = sample;
                shifted.timeMs = sample.timeMs - baseMs;
                out.push_back(shifted);
            }
            return out;
        }

        // The ring buffer's trim keeps times as they are: the samples before
        // keepFrom go, the one in force there stays.
        inline std::size_t DropBefore(std::vector<MoonSample>& track, std::uint32_t keepFrom)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.size() && track[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
                track.erase(track.begin(), track.begin() + static_cast<std::ptrdiff_t>(keep));
            return keep;
        }

        // Who owns the moon during a playback tick.
        enum class Source : std::uint8_t
        {
            Recorded,  // the clip's angles written every tick
            ClockLive, // "Game (live)" clock: the game's clock and the game's moon
            NoTrack,   // a clip from before the section: the session's moon (given back at the end)
            Off,       // MoonReplay=0
        };

        inline const char* SourceName(Source s)
        {
            switch (s)
            {
            case Source::Recorded: return "recorded";
            case Source::ClockLive: return "clock_live";
            case Source::NoTrack: return "no_track";
            default: return "off";
            }
        }

        // The live clock first, then the recorded track. (A Moon phase set
        // by the experimental row of a dev build is ignored: the recorded
        // moon plays.)
        inline Source PlanPlayback(bool enabled, bool haveTrack, bool clockLive)
        {
            if (!enabled)
                return Source::Off;
            if (clockLive)
                return Source::ClockLive;
            return haveTrack ? Source::Recorded : Source::NoTrack;
        }

        // The session's angles back after the clip. The clock goes back in
        // the same tick (SET_CLOCK_TIME); the jump is a delta of the next
        // clock step and moves the angles in the next update when it is
        // dark. So the saved values are written every tick until kFrames game
        // frames have passed since the clock went back (at least twice), with
        // a cap in ticks. The clock runs again meanwhile: the natural move
        // those frames would have made (under a game second) is lost.
        struct GiveBack
        {
            static constexpr std::uint32_t kFrames = 3;
            static constexpr std::uint32_t kMinTicks = 2;
            static constexpr std::uint32_t kMaxTicks = 30;
            bool armed = false;
            double angle = 0.0;
            double orbit = 0.0;
            std::int32_t armedFrame = 0;
            std::uint32_t ticks = 0;

            void Arm(double savedAngle, double savedOrbit, std::int32_t frame)
            {
                armed = true;
                angle = savedAngle;
                orbit = savedOrbit;
                armedFrame = frame;
                ticks = 0;
            }

            // One tick while armed: the values are written this tick; true
            // when this was the last write (disarmed).
            bool Step(std::int32_t frame)
            {
                if (!armed)
                    return true;
                ++ticks;
                const std::uint32_t passed = static_cast<std::uint32_t>(frame) - static_cast<std::uint32_t>(armedFrame);
                const bool done = (passed >= kFrames && passed < 0x80000000u && ticks >= kMinTicks) || ticks >= kMaxTicks;
                if (done)
                    armed = false;
                return done;
            }
        };

        // The game's own steps per game second (the after-clip check in the
        // log): the turn angle only while sun and moon are below the limit
        // (2pi / (2.5 * 3600)), the orbit always (the double 2pi / 86400
        // over the float 27.322 at +0x8C0).
        constexpr double kTurnPerDarkSecond = kTurn / 9000.0;
        constexpr double kOrbitPerSecond = 7.272205419010585e-05 / 27.322000503540039;

        // The elevation of a direction in degrees (z up); 0 when it is not one.
        inline float ElevationDeg(const float v[3])
        {
            const double l = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
            if (!(l > 1e-6) || !std::isfinite(l))
                return 0.0f;
            const double s = std::max(-1.0, std::min(1.0, double(v[2]) / l));
            return static_cast<float>(std::asin(s) * 180.0 / 3.14159265358979323846);
        }

        // The angle between two directions in degrees (the log's comparison
        // of the drawn moon); -1 when one is not a direction.
        inline float DirectionGapDeg(const float a[3], const float b[3])
        {
            const double la = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
            const double lb = std::sqrt(double(b[0]) * b[0] + double(b[1]) * b[1] + double(b[2]) * b[2]);
            if (!(la > 1e-6) || !(lb > 1e-6) || !std::isfinite(la) || !std::isfinite(lb))
                return -1.0f;
            double c = (double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]) / (la * lb);
            c = std::max(-1.0, std::min(1.0, c));
            return static_cast<float>(std::acos(c) * 180.0 / 3.14159265358979323846);
        }
    }
}
