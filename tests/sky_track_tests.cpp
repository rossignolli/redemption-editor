// sky_track.h, the clouds of the clip over time. The state at a playhead is a
// function of the track alone (two playbacks, a seek back and forth, and a
// pause give the same values at the same playhead); a wrap of the engine's fmod
// between two samples is interpolated the short way round, not back across the
// period; a channel with no known period is held; the buffer's trim and the
// window cut keep the state at the base.
#include "sky_track.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    using replay::SkyPeriods;
    using replay::SkySample;
    namespace sky = replay::sky;
    namespace skytrack = replay::skytrack;

    int g_skyFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_skyFailures;
    }

    bool Near(float a, float b, float tolerance = 1e-3f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    SkyPeriods Periods()
    {
        SkyPeriods p;
        p.value[sky::kPeriodNoise] = 4000.0f;
        p.value[sky::kPeriodPosition] = 3000.0f;
        p.value[sky::kPeriodLayer] = 1.0f;
        p.value[sky::kPeriodDrift] = 1.0f;
        p.value[sky::kPeriodRegion] = 7000.0f;
        return p;
    }

    // The engine's own integration: x -= v * dt, fmod by the period.
    SkySample Engine(std::uint32_t timeMs, const SkyPeriods& p)
    {
        SkySample s;
        s.timeMs = timeMs;
        const float t = static_cast<float>(timeMs) / 1000.0f;
        for (std::uint32_t c = 0; c < sky::kChannels; ++c)
        {
            const float period = p.value[sky::GroupOf(c)];
            const float speed = period * 0.07f * static_cast<float>(c % 3 + 1); // a wrap every few seconds
            s.value[c] = std::fmod(-speed * t + period * 0.4f, period);
        }
        s.unpinned[0] = -2.0f * t;
        return s;
    }

    std::vector<SkySample> Recorded(const SkyPeriods& p, std::uint32_t durationMs)
    {
        std::vector<SkySample> track;
        for (std::uint32_t t = 0; t <= durationMs; t += skytrack::kSampleMs)
            skytrack::Add(track, Engine(t, p));
        return track;
    }

    void TestFollowsTheEngine()
    {
        const SkyPeriods p = Periods();
        const std::vector<SkySample> track = Recorded(p, 20000);
        Check(track.size() == 201, "one sample every 100 ms over 20 s");
        bool all = true;
        for (std::uint32_t t = 0; t <= 20000; t += 37)
        {
            SkySample at;
            if (!skytrack::At(track, p, t, at))
            {
                all = false;
                break;
            }
            const SkySample truth = Engine(t, p);
            // Between samples the engine is linear: the same value round the period.
            if (skytrack::Distance(at, truth, p) > 0.01f)
                all = false;
        }
        Check(all, "the state between samples is the engine's, across every wrap");
    }

    void TestWrapIsShortWay()
    {
        SkyPeriods p;
        p.value[sky::kPeriodLayer] = 1.0f;
        std::vector<SkySample> track(2);
        track[0].timeMs = 0;
        track[0].value[sky::kLayer0X] = -0.95f; // about to wrap
        track[1].timeMs = 100;
        track[1].value[sky::kLayer0X] = -0.05f; // wrapped: -0.95 - 0.1 = -1.05 -> -0.05
        SkySample at;
        skytrack::At(track, p, 50, at);
        // Halfway is -1.0, which fmod makes 0 (or -0): never -0.5 (the long way).
        Check(Near(std::fabs(at.value[sky::kLayer0X]), 0.0f, 1e-4f) || Near(std::fabs(at.value[sky::kLayer0X]), 1.0f, 1e-4f),
            "a wrap between two samples goes the short way");
        skytrack::At(track, p, 25, at);
        Check(Near(at.value[sky::kLayer0X], -0.975f), "a quarter of the way before the wrap");
        skytrack::At(track, p, 75, at);
        Check(Near(at.value[sky::kLayer0X], -0.025f), "a quarter of the way after the wrap, in the engine's range");
        Check(Near(skytrack::WrapDelta(-0.95f, -0.05f, 1.0f), -0.1f), "the short difference across a wrap");
        Check(Near(skytrack::WrapDelta(10.0f, 30.0f, 0.0f), 20.0f), "no period: the plain difference");
    }

    void TestUnknownPeriodHolds()
    {
        SkyPeriods p; // all unknown
        std::vector<SkySample> track(2);
        track[0].timeMs = 0;
        track[0].value[sky::kNoiseX] = 10.0f;
        track[1].timeMs = 100;
        track[1].value[sky::kNoiseX] = 20.0f;
        SkySample at;
        skytrack::At(track, p, 60, at);
        Check(at.value[sky::kNoiseX] == 10.0f, "a channel with no period holds the earlier sample");
        skytrack::At(track, p, 100, at);
        Check(at.value[sky::kNoiseX] == 20.0f, "and takes the next at its time");
    }

    void TestDeterministicAcrossPlaybacks()
    {
        const SkyPeriods p = Periods();
        const std::vector<SkySample> track = Recorded(p, 20000);
        // Two playbacks at different frame rates, a pause, a seek back: the
        // state at a playhead depends on the playhead only.
        const std::uint32_t probes[] = {0u, 1234u, 5000u, 9999u, 15050u, 20000u};
        bool same = true;
        for (std::uint32_t probe : probes)
        {
            SkySample first, second, afterSeek;
            std::uint32_t playhead = 0;
            for (; playhead < probe; playhead += 16) // 60 fps
                skytrack::At(track, p, playhead, first);
            skytrack::At(track, p, probe, first);
            for (playhead = 0; playhead < probe; playhead += 33) // 30 fps
                skytrack::At(track, p, playhead, second);
            for (int pause = 0; pause < 10; ++pause)
                skytrack::At(track, p, probe, second);
            skytrack::At(track, p, 20000, afterSeek);
            skytrack::At(track, p, probe, afterSeek);
            for (std::uint32_t c = 0; c < sky::kChannels; ++c)
                if (first.value[c] != second.value[c] || first.value[c] != afterSeek.value[c])
                    same = false;
        }
        Check(same, "the same playhead gives the same clouds in every playback (bit for bit)");
    }

    void TestEndsAndAdd()
    {
        const SkyPeriods p = Periods();
        std::vector<SkySample> track;
        SkySample at;
        Check(!skytrack::At(track, p, 0, at), "an empty track (an older clip) applies nothing");
        Check(skytrack::Add(track, Engine(100, p)), "first sample");
        Check(!skytrack::Add(track, Engine(50, p)), "a sample back in time refused");
        SkySample stop = Engine(100, p);
        stop.value[sky::kNoiseX] = 1.0f;
        Check(skytrack::Add(track, stop) && track.size() == 1 && track[0].value[sky::kNoiseX] == 1.0f,
            "a sample at the same time replaces the last (the stop's read)");
        SkySample bad = Engine(200, p);
        bad.value[sky::kRegionY] = std::nanf("");
        Check(!skytrack::Add(track, bad), "a value that is not a number refused");
        skytrack::At(track, p, 0, at);
        Check(at.value[sky::kNoiseX] == 1.0f && at.timeMs == 0, "before the first sample: the first");
        skytrack::At(track, p, 99999, at);
        Check(at.value[sky::kNoiseX] == 1.0f, "after the last sample: the last");
        SkyPeriods negative = p;
        negative.value[sky::kPeriodNoise] = -1.0f;
        Check(skytrack::Valid(p) && !skytrack::Valid(negative), "periods: negative refused");
    }

    void TestCutAndTrim()
    {
        const SkyPeriods p = Periods();
        const std::vector<SkySample> track = Recorded(p, 20000);
        const std::vector<SkySample> cut = skytrack::Cut(track, p, 5050, 12000);
        Check(!cut.empty() && cut.front().timeMs == 0, "the cut starts at 0");
        SkySample inCut, inTrack;
        bool same = true;
        for (std::uint32_t t = 0; t <= 6950; t += 41)
        {
            skytrack::At(cut, p, t, inCut);
            skytrack::At(track, p, 5050 + t, inTrack);
            if (skytrack::Distance(inCut, inTrack, p) > 1e-3f)
                same = false;
        }
        Check(same, "the cut plays the clouds of the window");
        Check(cut.back().timeMs <= 12000 - 5050, "nothing past the window's end");

        std::vector<SkySample> ring = track;
        const std::size_t dropped = skytrack::DropBefore(ring, 7000);
        Check(dropped == 70 && ring.front().timeMs == 7000, "the trim keeps the sample in force at the head");
        skytrack::At(ring, p, 7050, inCut);
        skytrack::At(track, p, 7050, inTrack);
        Check(skytrack::Distance(inCut, inTrack, p) == 0.0f, "the trimmed ring plays as before past the head");
    }
}

int RunSkyTrackTests()
{
    g_skyFailures = 0;
    TestFollowsTheEngine();
    TestWrapIsShortWay();
    TestUnknownPeriodHolds();
    TestDeterministicAcrossPlaybacks();
    TestEndsAndAdd();
    TestCutAndTrim();
    if (g_skyFailures == 0)
        std::printf("sky track tests passed\n");
    return g_skyFailures;
}
