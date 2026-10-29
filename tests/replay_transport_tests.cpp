#include "replay_transport.h"
#include "clip_load_wait.h"

#include <cstdio>

namespace
{
    using namespace replay::transport;

    int g_transportFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_transportFailures;
    }

    Input Keys(int direction, bool fine = false, bool hold = false)
    {
        Input input;
        input.scrubDirection = direction;
        input.fineScrub = fine;
        input.hold = hold;
        return input;
    }

    void TestStartNeedsTimeline()
    {
        Transport transport;
        Check(!transport.Start(1000) && !transport.Playing(), "no timeline: playback refused");
        Check(!transport.SetPaused(true, 1000), "pause ignored while not playing");
        Check(transport.Update(1000, {}) == 0 && transport.PlayheadMs() == 0, "idle update does nothing");

        transport.SetDurationMs(5000);
        Check(transport.Start(1000) && transport.Playing() && !transport.Paused() &&
                transport.PlayheadMs() == 0,
            "starts at zero, playing");
        transport.Seek(1500);
        Check(transport.PlayheadMs() == 1500, "seek moves the playhead");
        transport.Seek(9000);
        Check(transport.PlayheadMs() == 5000, "seek clamps to the timeline");
        transport.Stop();
        Check(!transport.Playing() && transport.PlayheadMs() == 0 && transport.DurationMs() == 5000,
            "stop resets the playhead and keeps the timeline");
    }

    void TestStartFloor()
    {
        Transport transport;
        transport.SetDurationMs(10000);
        transport.Start(1000);
        transport.SetStartMs(85);
        transport.Seek(0);
        Check(transport.PlayheadMs() == 85, "seek stops at the start floor");
        transport.Seek(4000);
        transport.SetPaused(true, 1000);
        std::uint32_t events = 0;
        for (int n = 1; n <= 400; ++n)
            events |= transport.Update(1000 + 16 * n, Keys(-1));
        Check(transport.PlayheadMs() == 85 && (events & kReachedStart), "backward scrub stops at the start floor");
        transport.Stop();
        Check(transport.StartMs() == 0, "stop clears the start floor");
        transport.SetStartMs(20000);
        Check(transport.StartMs() == 0, "a floor past the timeline is ignored");
    }

    void TestRate()
    {
        Transport transport;
        transport.SetDurationMs(100000);
        transport.Start(1000);
        transport.SetRate(10.0);
        transport.Update(1016, {});
        Check(transport.PlayheadMs() == 160, "rate 10: a 16 ms frame moves 160 ms");
        transport.SetRate(0.05);
        for (int n = 0; n < 20; ++n)
            transport.Update(1016 + 16 * (n + 1), {});
        Check(transport.PlayheadMs() == 176, "rate 0.05: fractions accumulate (20 frames of 0.8 ms)");
        transport.SetRate(1.0);
    }

    void TestNormalPlayAndHold()
    {
        Transport transport;
        transport.SetDurationMs(1000);
        transport.Start(1000);
        Check(transport.Update(1016, {}) == 0 && transport.PlayheadMs() == 16, "+1x by the frame delta");
        Check(transport.Update(1116, Keys(0, false, true)) == 0 && transport.PlayheadMs() == 16,
            "hold freezes the playhead");
        Check(transport.Update(1132, {}) == 0 && transport.PlayheadMs() == 32,
            "time after a hold counts from the hold's frame");
        transport.SetPaused(true, 1200);
        Check(transport.Update(1300, {}) == 0 && transport.PlayheadMs() == 32, "paused: no advance");
        transport.SetPaused(false, 1400);
        Check(transport.Update(1416, {}) == 0 && transport.PlayheadMs() == 48,
            "resume counts from the resume, not from the pause");
        // One frame moves 200 ms at most, so the end comes after a few stalled
        // frames, still once.
        std::uint32_t events = 0;
        std::uint64_t at = 1416;
        for (int frame = 0; frame < 10 && !transport.Paused(); ++frame)
        {
            at += 8000;
            events |= transport.Update(at, {});
        }
        Check(events == (kPaused | kReachedEnd) && transport.PlayheadMs() == 1000 && transport.Paused(),
            "end of the timeline pauses once");
        Check(transport.Update(at + 16, {}) == 0, "nothing more at the end");
    }

    // A stalled frame (a hitch) moves the playhead by kMaxNormalStepMs, never
    // past the event cursors' jump threshold, and drops the rest of the stall
    // instead of carrying it.
    void TestStalledFrameIsNotAJump()
    {
        static_assert(kMaxNormalStepMs < 250, "a normal step must stay under ForwardEventCursor::kMaxStepMs");
        Transport transport;
        transport.SetDurationMs(60000);
        transport.Start(1000);
        Check(transport.Update(1016, {}) == 0 && transport.PlayheadMs() == 16, "normal frame");
        transport.Update(2016, {}); // a 1 s hitch
        Check(transport.PlayheadMs() == 16 + kMaxNormalStepMs, "a 1 s hitch moves the playhead 200 ms");
        transport.Update(2032, {});
        Check(transport.PlayheadMs() == 32 + kMaxNormalStepMs, "the next frame plays its own 16 ms, no catch-up");
        transport.SetRate(5.0); // a recorded slow motion at 0.2 plays at game speed
        transport.Update(2048, {});
        Check(transport.PlayheadMs() == 32 + kMaxNormalStepMs + 80, "rate 5: 16 ms of wall clock is 80 ms");
        transport.Update(2548, {});
        Check(transport.PlayheadMs() == 32 + kMaxNormalStepMs * 2 + 80, "rate 5 and a hitch: still capped");
        Check(NormalStepMs(16) == 16 && NormalStepMs(5000) == kMaxNormalStepMs, "NormalStepMs caps only above the limit");
    }

    void TestScrubSpeedEndsPaused()
    {
        Transport transport;
        transport.SetDurationMs(10000);
        transport.Start(1000);
        transport.Seek(5000);
        std::uint32_t events = transport.Update(1010, Keys(-1));
        Check(events == kScrubStarted && transport.Scrubbing() && !transport.Paused(),
            "scrub starts from play without pausing");
        Check(transport.PlayheadMs() == 4980 && transport.Scrub().direction == -1 &&
                transport.Scrub().fromMs == 5000 && transport.Scrub().toMs == 4980 &&
                transport.Scrub().frames == 1 && transport.Scrub().startedAt == 1010,
            "2x backward by the frame delta");
        events = transport.Update(1020, Keys(-1));
        Check(events == 0 && transport.PlayheadMs() == 4960 && transport.Scrub().frames == 2,
            "held key keeps scrubbing silently");
        events = transport.Update(1030, Keys(0));
        Check(events == (kScrubEnded | kPaused) && !transport.Scrubbing() && transport.Paused(),
            "release ends the scrub paused");
        Check(transport.EndedScrub().fromMs == 5000 && transport.EndedScrub().toMs == 4960 &&
                transport.EndedScrub().frames == 2 && transport.EndedScrub().direction == -1,
            "the ended scrub is kept for the log");
        Check(transport.PlayheadMs() == 4960, "playhead stays where the scrub left it");
        Check(transport.Update(1046, {}) == 0 && transport.PlayheadMs() == 4960, "still paused");
    }

    void TestScrubGestureChangeAndFine()
    {
        Transport transport;
        transport.SetDurationMs(10000);
        transport.Start(1000);
        transport.Seek(5000);
        transport.Update(1010, Keys(1));
        Check(transport.PlayheadMs() == 5020, "2x forward");
        std::uint32_t events = transport.Update(1020, Keys(1, true));
        Check(events == (kScrubEnded | kPaused | kScrubStarted), "gesture change ends one scrub and starts another");
        Check(transport.EndedScrub().fine == false && transport.EndedScrub().toMs == 5020 &&
                transport.Scrub().fine && transport.Scrub().fromMs == 5020,
            "ended and new scrub described apart");
        Check(transport.PlayheadMs() == 5030 && transport.Scrub().frames == 1,
            "fine scrub steps 10 ms per frame whatever the delta");
        transport.Update(2020, Keys(1, true));
        Check(transport.PlayheadMs() == 5040, "fine step ignores wall time");
        events = transport.Update(2030, Keys(-1, true));
        Check((events & kScrubStarted) && transport.Scrub().direction == -1 && transport.PlayheadMs() == 5030,
            "direction change is a new scrub");
    }

    void TestScrubBoundaries()
    {
        Transport transport;
        transport.SetDurationMs(100);
        transport.Start(1000);
        transport.Seek(30);
        std::uint32_t events = transport.Update(1010, Keys(-1));
        Check(events == kScrubStarted && transport.PlayheadMs() == 10, "first frame back");
        events = transport.Update(1020, Keys(-1));
        Check(events == kReachedStart && transport.PlayheadMs() == 0 && transport.Scrub().atBoundary,
            "start reached once");
        events = transport.Update(1030, Keys(-1));
        Check(events == 0 && transport.PlayheadMs() == 0, "boundary reported only once per scrub");
        events = transport.Update(1040, Keys(1));
        Check(events == (kScrubEnded | kPaused | kScrubStarted) && transport.PlayheadMs() == 20,
            "turning around is a new scrub from the start");
        transport.Update(1100, Keys(1));
        Check(transport.PlayheadMs() == 100, "clamped at the end");
        events = transport.Update(1110, Keys(1));
        Check(events == 0, "end reached on the clamping frame, not again");
        transport.Update(1120, Keys(0));
        Check(transport.Paused() && transport.EndedScrub().atBoundary, "ended at the boundary, paused");
    }

    // The clip load presentation's rules.
    void TestClipLoadWait()
    {
        using namespace replay::clipload;
        Check(!Evaluate(100, true, true).done, "load wait: ready before the minimum black keeps black");
        Check(Evaluate(300, true, true).done && !Evaluate(300, true, true).timedOut, "load wait: ready after the minimum fades in");
        Check(!Evaluate(2000, false, true).done, "load wait: scene not loaded keeps black");
        Check(!Evaluate(2000, true, false).done, "load wait: puppets not built keep black");
        const Verdict late = Evaluate(8000, false, false);
        Check(late.done && late.timedOut, "load wait: the timeout fades in anyway");

        const Vec3 origin{};
        const Vec3 closeBy{30.0f, 0.0f, 0.0f};
        const Vec3 farAway{0.0f, 60.0f, 0.0f};
        Check(NeedsRetarget(false, origin, closeBy), "load wait: no scene yet starts one");
        Check(!NeedsRetarget(true, origin, closeBy), "load wait: a camera 30 m away keeps the scene");
        Check(NeedsRetarget(true, origin, farAway), "load wait: a camera 60 m away loads again there");

        Check(IsLongJump(true, origin, 1000, farAway, 9000, false), "jump: a seek landing 60 m away goes black");
        Check(!IsLongJump(false, origin, 1000, farAway, 9000, false), "jump: no previous camera, no jump");
        Check(!IsLongJump(true, origin, 1000, farAway, 9000, true), "jump: a scrub never goes black");
        Check(!IsLongJump(true, origin, 1000, farAway, 1033, false), "jump: a camera cut in normal play never goes black");
        Check(!IsLongJump(true, origin, 9000, closeBy, 1000, false), "jump: a seek landing 30 m away stays");
        Check(IsLongJump(true, origin, 9000, farAway, 1000, false), "jump: a seek back counts too");
    }
}

int RunReplayTransportTests()
{
    TestStartNeedsTimeline();
    TestStartFloor();
    TestRate();
    TestNormalPlayAndHold();
    TestStalledFrameIsNotAJump();
    TestScrubSpeedEndsPaused();
    TestScrubGestureChangeAndFine();
    TestScrubBoundaries();
    TestClipLoadWait();
    if (g_transportFailures == 0)
        std::printf("Replay transport tests passed.\n");
    return g_transportFailures;
}
