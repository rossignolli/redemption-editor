#pragma once

// Render export: the export clock.
//
// While a clip is exported, time is not the wall clock.
// Every game frame ("tick") is one sub-sample of one output frame, and the
// clock says, for each tick, the output time it shows and the time step the
// game must simulate to get there. The game then runs slower than real time
// (a 4K frame with 16 motion blur samples may take a second) and no frame is
// ever dropped or duplicated: frame f of the video is exactly f * 1/fps of
// clip time after the start, whatever the machine does.
//
// Motion blur is sub-frame accumulation: N samples per output frame spread
// over the open part of the shutter (angle / 360 of the frame interval),
// centred on the frame's instant, each with weight 1/N (box shutter). The
// closed part of the shutter is simulated in one longer step and never
// rendered, so no rendered frame is wasted. Samples use the midpoint rule:
// sample i of N sits at (i + 0.5) / N of the open interval.
//
// Pure data (unit tested in ReplayCoreTests, tests/export_schedule_tests.cpp).
// Times are signed nanoseconds of output time, 0 at the export's first frame;
// preroll ticks have negative times.

#include <cmath>
#include <cstdint>
#include <vector>

namespace replay::exporting
{
    constexpr std::uint32_t kMaxSamples = 64;
    constexpr std::int64_t kNsPerSecond = 1000000000ll;

    // A rational frame rate (30000/1001 for 29.97).
    struct FrameRate
    {
        std::uint32_t num = 60;
        std::uint32_t den = 1;
    };

    constexpr bool ValidRate(FrameRate rate) noexcept
    {
        return rate.num > 0 && rate.den > 0 && rate.den <= 1001 && rate.num <= 1000u * rate.den;
    }

    // Start of output frame `frame` in ns, exact (floor), without overflow
    // for any frame count a video can have.
    constexpr std::int64_t FrameStartNs(FrameRate rate, std::int64_t frame) noexcept
    {
        if (!ValidRate(rate))
            return 0;
        const bool negative = frame < 0;
        const std::uint64_t f = static_cast<std::uint64_t>(negative ? -frame : frame);
        const std::uint64_t whole = f / rate.num;
        const std::uint64_t rest = f % rate.num;
        const std::uint64_t perNum = static_cast<std::uint64_t>(kNsPerSecond) * rate.den; // ns in `num` frames
        const std::uint64_t ns = whole * perNum + rest * perNum / rate.num;
        return negative ? -static_cast<std::int64_t>(ns) : static_cast<std::int64_t>(ns);
    }

    struct Shutter
    {
        std::uint32_t samples = 1; // 1 = no motion blur
        float angleDegrees = 180.0f; // 0..360, the film camera's shutter angle
    };

    constexpr std::uint32_t ClampSamples(std::uint32_t samples) noexcept
    {
        return samples < 1 ? 1 : samples > kMaxSamples ? kMaxSamples : samples;
    }

    // The open part of frame `frame`'s interval, ns (0 without blur).
    constexpr std::int64_t OpenNs(FrameRate rate, Shutter shutter, std::int64_t frame) noexcept
    {
        if (ClampSamples(shutter.samples) <= 1 || shutter.angleDegrees <= 0.0f)
            return 0;
        const float angle = shutter.angleDegrees > 360.0f ? 360.0f : shutter.angleDegrees;
        const std::int64_t interval = FrameStartNs(rate, frame + 1) - FrameStartNs(rate, frame);
        return static_cast<std::int64_t>(static_cast<double>(interval) * static_cast<double>(angle) / 360.0 + 0.5);
    }

    // Output time of sample `sample` of frame `frame`: centred on the frame
    // start, midpoint rule. Without blur it is the frame start itself.
    constexpr std::int64_t SampleTimeNs(FrameRate rate, Shutter shutter, std::int64_t frame, std::uint32_t sample) noexcept
    {
        const std::uint32_t n = ClampSamples(shutter.samples);
        const std::int64_t start = FrameStartNs(rate, frame);
        const std::int64_t open = OpenNs(rate, shutter, frame);
        if (open == 0)
            return start;
        const std::int64_t s = sample >= n ? n - 1 : sample;
        return start - open / 2 + (open * (2 * s + 1)) / (2 * static_cast<std::int64_t>(n));
    }

    // ------------------------------------------------------------------
    // Motion blur in slow motion.
    //
    // The engine clamps every game step (the frame step times the world time
    // scale) to a minimum, [0x3CC8388] in build 1491.50: 1/3000 s. 32 samples
    // at 60 fps, 180 degrees, are 260 us apart, already under that minimum at
    // normal speed, and at a 20 % marker 52 us asked against 333 us run: the
    // world 6.4 times too fast between samples.
    //
    // So each frame takes the most samples whose world step stays at or
    // above the minimum: the shutter (its angle and its centre) stays
    // exact, only the number of sub-frames drops where the world is slow.
    // At 60 fps, 180 degrees: 25 samples at 100 %, 5 at 20 %, 1 at 5 %.
    // ------------------------------------------------------------------

    // The dialog's estimate of the clamp (the probe uses the value it reads
    // from the game when it locks the timer).
    constexpr double kEngineMinStepS = 1.0 / 3000.0;

    // Most samples (1..asked) of frame `frame` whose spacing, times the
    // world scale, is at least minStepNs. 0.1 % of slack: a spacing within
    // float rounding of the minimum is not changed by the clamp.
    inline std::uint32_t SamplesForWorldScale(FrameRate rate, Shutter shutter, std::int64_t frame, double worldScale, std::int64_t minStepNs) noexcept
    {
        const std::uint32_t asked = shutter.samples < 1 ? 1 : shutter.samples > kMaxSamples ? kMaxSamples : shutter.samples;
        if (asked <= 1 || minStepNs <= 0 || !(worldScale > 0.0) || shutter.angleDegrees <= 0.0f)
            return asked;
        const float angle = shutter.angleDegrees > 360.0f ? 360.0f : shutter.angleDegrees;
        const double interval = static_cast<double>(FrameStartNs(rate, frame + 1) - FrameStartNs(rate, frame));
        const double open = interval * static_cast<double>(angle) / 360.0;
        const double fit = open * worldScale / (static_cast<double>(minStepNs) * 0.999);
        if (fit >= static_cast<double>(asked))
            return asked;
        return fit < 2.0 ? 1u : static_cast<std::uint32_t>(fit);
    }

    // Even one sample per frame is below the minimum (a 5 % marker under a
    // 5 % playback speed): the world runs fast there whatever the blur.
    inline bool FrameStepBelowMinimum(FrameRate rate, std::int64_t frame, double worldScale, std::int64_t minStepNs) noexcept
    {
        const double interval = static_cast<double>(FrameStartNs(rate, frame + 1) - FrameStartNs(rate, frame));
        return minStepNs > 0 && worldScale > 0.0 && interval * worldScale < static_cast<double>(minStepNs) * 0.999;
    }

    // A stretch of the playhead at one playback speed (a marker's speed
    // times the global playback speed). The world's time scale there is the
    // speed when below 1 (script.cpp UpdatePlayback), 1 otherwise; the
    // playhead advances `speed` ms per ms of video.
    struct SpeedSpan
    {
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        double speed = 1.0;
    };

    // A marker as the dialog knows it: from timeMs on, this speed (until the
    // next one). Sorted by time.
    struct MarkerSpeed
    {
        std::uint64_t timeMs = 0;
        double speed = 1.0;
    };

    // The speed spans of [startMs, endMs): before the first marker 100 %.
    inline std::vector<SpeedSpan> SpeedSpansFromMarkers(const std::vector<MarkerSpeed>& markers, std::uint64_t startMs, std::uint64_t endMs,
        double globalSpeed)
    {
        std::vector<SpeedSpan> out;
        if (endMs <= startMs)
            return out;
        double speed = 1.0;
        std::uint64_t at = startMs;
        for (const MarkerSpeed& marker : markers)
        {
            if (marker.timeMs <= startMs)
            {
                speed = marker.speed;
                continue;
            }
            if (marker.timeMs >= endMs)
                break;
            out.push_back({at, marker.timeMs, speed * globalSpeed});
            at = marker.timeMs;
            speed = marker.speed;
        }
        out.push_back({at, endMs, speed * globalSpeed});
        return out;
    }

    // Where the export's blur takes fewer samples than asked: video time
    // (seconds from the export's first frame), playhead range, world scale
    // and the samples per frame there. Neighbours with the same result merge.
    struct BlurSpan
    {
        double outStartS = 0.0;
        double outEndS = 0.0;
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        double worldScale = 1.0;
        std::uint32_t samples = 1;
        bool frameBelowMinimum = false; // even without blur the world runs fast
    };

    inline std::vector<BlurSpan> PlanBlurSpans(FrameRate rate, Shutter shutter, const std::vector<SpeedSpan>& spans, double minStepS)
    {
        std::vector<BlurSpan> out;
        const std::uint32_t asked = shutter.samples < 1 ? 1 : shutter.samples > kMaxSamples ? kMaxSamples : shutter.samples;
        const std::int64_t minStepNs = static_cast<std::int64_t>(std::llround(minStepS * 1e9));
        double outS = 0.0;
        for (const SpeedSpan& span : spans)
        {
            if (span.endMs <= span.startMs || !(span.speed > 0.0))
                continue;
            const double scale = span.speed < 1.0 ? span.speed : 1.0;
            const double seconds = static_cast<double>(span.endMs - span.startMs) / 1000.0 / span.speed;
            const std::uint32_t samples = asked > 1 ? SamplesForWorldScale(rate, shutter, 0, scale, minStepNs) : 1u;
            const bool below = FrameStepBelowMinimum(rate, 0, scale, minStepNs);
            if (samples < asked || below)
            {
                if (!out.empty() && out.back().samples == samples && out.back().worldScale == scale && std::fabs(out.back().outEndS - outS) < 1e-9)
                {
                    out.back().outEndS = outS + seconds;
                    out.back().endMs = span.endMs;
                }
                else
                    out.push_back({outS, outS + seconds, span.startMs, span.endMs, scale, samples, below});
            }
            outS += seconds;
        }
        return out;
    }

    // The output frames of the span [startMs, endMs): walked one video frame
    // at a time at the playhead rate of each place (`rateAt(ms)`, playhead ms
    // per ms of video). A frame counts when its whole step fits, so at 1x it
    // is the span's frame count; a slow motion stretches it. `cap` bounds a
    // rate near zero.
    template <typename RateAt>
    std::uint64_t SpanOutputFrames(std::uint64_t startMs, std::uint64_t endMs, FrameRate rate, RateAt rateAt, std::uint64_t cap)
    {
        if (!ValidRate(rate) || endMs <= startMs)
            return 0;
        const double frameMs = 1000.0 * static_cast<double>(rate.den) / static_cast<double>(rate.num);
        const double end = static_cast<double>(endMs) + 1e-6;
        double at = static_cast<double>(startMs);
        std::uint64_t frames = 0;
        while (frames < cap)
        {
            double step = frameMs * rateAt(static_cast<std::uint64_t>(at));
            if (!(step > 0.0))
                step = frameMs;
            if (at + step > end)
                break;
            at += step;
            ++frames;
        }
        return frames;
    }

    // timer_check (export_probe.cpp): the engine's step of this frame
    // (GET_FRAME_TIME) against the planned step times the world scale.
    //   Match     within 2 % (or 0.2 ms)
    //   Clamped   the plan was under the engine's minimum and the engine
    //             ran the minimum: the world went faster than the export
    //   Mismatch  anything else
    // A world scale set this frame may reach the engine's update one frame
    // later: on the frame the scale changed either scale matches.
    enum class TimerCheck : std::uint32_t
    {
        Match = 0,
        Mismatch = 1,
        Clamped = 2,
    };

    inline TimerCheck CheckTimerStep(double plannedS, double scale, double previousScale, double frameTimeS, double minStepS) noexcept
    {
        const auto within = [frameTimeS](double expected) {
            const double tolerance = expected * 0.02 > 0.0002 ? expected * 0.02 : 0.0002;
            return std::fabs(frameTimeS - expected) <= tolerance;
        };
        const double expected = plannedS * scale;
        const bool scaleChanged = previousScale != scale;
        if (minStepS > 0.0 && expected < minStepS * 0.98 && std::fabs(frameTimeS - minStepS) <= minStepS * 0.02 &&
            !(scaleChanged && plannedS * previousScale >= minStepS * 0.98 && within(plannedS * previousScale)))
            return TimerCheck::Clamped;
        if (within(expected))
            return TimerCheck::Match;
        if (scaleChanged && within(plannedS * previousScale))
            return TimerCheck::Match;
        return TimerCheck::Mismatch;
    }

    enum class Phase : std::uint32_t
    {
        Idle = 0,
        Preroll = 1, // the world settles before the first frame; not captured
        Capture = 2, // a sub-sample of an output frame
        Flush = 3,   // after the last frame: the frames still in the render pipeline reach the present
        Done = 4
    };

    struct Settings
    {
        FrameRate rate;
        Shutter shutter;
        std::uint32_t prerollFrames = 30;   // output frames of world warm-up before frame 0
        std::uint32_t flushTicks = 4;       // ticks after the last sample (>= the render latency)
        std::uint64_t maxFrames = 0;        // 0 = until Finish()
    };

    struct Tick
    {
        Phase phase = Phase::Idle;
        std::int64_t frame = 0;      // output frame (negative in preroll)
        std::uint32_t sample = 0;    // sub-sample index in the frame
        std::uint32_t samples = 1;   // sub-samples of this frame
        std::int64_t timeNs = 0;     // output time this tick shows
        std::int64_t stepNs = 0;     // the game's time step for this tick (time since the previous tick)
        float weight = 0.0f;         // accumulation weight (0 outside Capture)
        bool firstOfFrame = false;   // the accumulator clears before adding
        bool lastOfFrame = false;    // the accumulator resolves after adding
        std::uint64_t serial = 0;    // 0, 1, 2 ... over the whole export (ticks, not frames)
    };

    // The stateful clock. Begin, then Next once per game frame (script tick)
    // until it returns Phase::Done. Finish() ends the capture at the end of
    // the frame being sampled (the playhead reached the export's end).
    class Clock
    {
    public:
        void Begin(const Settings& settings) noexcept
        {
            m_settings = settings;
            m_settings.shutter.samples = ClampSamples(settings.shutter.samples);
            m_phase = m_settings.prerollFrames ? Phase::Preroll : Phase::Capture;
            m_frame = -static_cast<std::int64_t>(m_settings.prerollFrames);
            m_sample = 0;
            m_flushed = 0;
            m_serial = 0;
            m_finishRequested = false;
            m_started = false;
            m_emitted = 0;
            m_cap = kMaxSamples;
            m_frameSamples = m_settings.shutter.samples;
            // Before the first tick the clock stands at the first tick's time
            // (the transport's reference point).
            m_lastTimeNs = m_phase == Phase::Preroll ? FrameStartNs(m_settings.rate, m_frame)
                                                     : SampleTimeNs(m_settings.rate, m_settings.shutter, 0, 0);
        }

        void Finish() noexcept { m_finishRequested = true; }
        // The most samples the next frame may take (SamplesForWorldScale with
        // the world scale ahead). Read when a frame starts (its first sample),
        // so a frame never changes count midway.
        void SetSampleCap(std::uint32_t cap) noexcept { m_cap = cap < 1 ? 1 : cap; }
        std::uint32_t SampleCap() const noexcept { return m_cap; }
        bool Active() const noexcept { return m_phase != Phase::Idle && m_phase != Phase::Done; }
        Phase CurrentPhase() const noexcept { return m_phase; }
        const Settings& Config() const noexcept { return m_settings; }
        std::uint64_t FramesEmitted() const noexcept { return m_emitted; }

        // The clock for the playback transport, ms. Transport::Update takes
        // deltas only; `baseMs` keeps the value positive for preroll times.
        std::uint64_t TransportNowMs(std::uint64_t baseMs) const noexcept
        {
            const std::int64_t prerollNs = FrameStartNs(m_settings.rate, static_cast<std::int64_t>(m_settings.prerollFrames) + 1);
            return baseMs + static_cast<std::uint64_t>((m_lastTimeNs + prerollNs) / 1000000);
        }

        Tick Next() noexcept
        {
            Tick tick;
            tick.serial = m_serial++;
            switch (m_phase)
            {
            case Phase::Idle:
            case Phase::Done:
                tick.phase = m_phase;
                return tick;
            case Phase::Preroll:
            {
                // One tick per output frame at the frame rate (a steady world).
                tick.phase = Phase::Preroll;
                tick.frame = m_frame;
                tick.timeNs = FrameStartNs(m_settings.rate, m_frame);
                if (++m_frame >= 0)
                    m_phase = Phase::Capture;
                break;
            }
            case Phase::Capture:
            {
                if (m_sample == 0)
                    m_frameSamples = m_settings.shutter.samples < m_cap ? m_settings.shutter.samples : m_cap;
                const std::uint32_t n = m_frameSamples;
                const Shutter shutter{n, m_settings.shutter.angleDegrees}; // the frame's count, the same angle
                tick.phase = Phase::Capture;
                tick.frame = m_frame;
                tick.sample = m_sample;
                tick.samples = n;
                tick.timeNs = SampleTimeNs(m_settings.rate, shutter, m_frame, m_sample);
                tick.weight = 1.0f / static_cast<float>(n);
                tick.firstOfFrame = m_sample == 0;
                tick.lastOfFrame = m_sample + 1 == n;
                if (tick.lastOfFrame)
                {
                    ++m_emitted;
                    m_sample = 0;
                    ++m_frame;
                    const bool reachedMax = m_settings.maxFrames != 0 && m_emitted >= m_settings.maxFrames;
                    if (m_finishRequested || reachedMax)
                        m_phase = Phase::Flush;
                }
                else
                {
                    ++m_sample;
                }
                break;
            }
            case Phase::Flush:
            {
                tick.phase = Phase::Flush;
                tick.frame = m_frame;
                tick.timeNs = FrameStartNs(m_settings.rate, m_frame + static_cast<std::int64_t>(m_flushed));
                if (++m_flushed >= m_settings.flushTicks)
                    m_phase = Phase::Done;
                break;
            }
            }
            tick.stepNs = m_started ? tick.timeNs - m_lastTimeNs : FrameStartNs(m_settings.rate, 1);
            m_started = true;
            m_lastTimeNs = tick.timeNs;
            return tick;
        }

    private:
        Settings m_settings;
        Phase m_phase = Phase::Idle;
        std::int64_t m_frame = 0;
        std::uint32_t m_sample = 0;
        std::uint32_t m_flushed = 0;
        std::uint64_t m_serial = 0;
        std::uint64_t m_emitted = 0;
        std::int64_t m_lastTimeNs = 0;
        bool m_finishRequested = false;
        bool m_started = false;
        std::uint32_t m_cap = kMaxSamples;   // SetSampleCap
        std::uint32_t m_frameSamples = 1;    // the samples of the frame being sampled
    };

    // Frame tag (latency calibration):
    // during preroll the probe draws the low bits of the tick serial as a row
    // of black and white cells in the top left corner; the add-on reads the
    // row at the capture point and learns how many presents separate a
    // script tick from its picture. Only preroll and flush ticks are tagged
    // (their pictures are never encoded).
    constexpr std::uint32_t kTagBits = 8;
    constexpr bool TagBit(std::uint64_t serial, std::uint32_t bit) noexcept
    {
        return ((serial >> bit) & 1u) != 0;
    }
    // The present latency from a decoded tag: presents counted by the add-on
    // since the export began minus the tagged serial (mod 2^kTagBits).
    constexpr std::uint32_t LatencyFromTag(std::uint64_t presentsSinceBegin, std::uint32_t tag) noexcept
    {
        return static_cast<std::uint32_t>((presentsSinceBegin - tag) & ((1u << kTagBits) - 1u));
    }
}
