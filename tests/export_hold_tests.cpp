// Render export: the screens that cancel or hold an export
// (src/export_hold.h) and the clock's Hold ticks (src/export_schedule.h).
#include "export_hold.h"
#include "export_schedule.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    namespace eh = replay::exporthold;
    namespace ex = replay::exporting;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    eh::Config SmallConfig()
    {
        eh::Config c;
        c.releaseTicks = 3;
        c.resyncMinTicks = 2;
        c.resyncMaxTicks = 6;
        return c;
    }

    void TestLoadingHoldAndRelease()
    {
        eh::Machine m(SmallConfig());
        Check(!m.Update(0, false).hold, "no signal: the clock runs");
        eh::Decision d = m.Update(eh::SignalLoading, false);
        Check(d.hold && d.started && d.phase == eh::Phase::Held, "a loading screen holds on its first tick (no debounce on the way in)");
        Check(d.signals == eh::SignalLoading, "the reason is the loading screen");
        d = m.Update(eh::SignalLoading, false);
        Check(d.hold && !d.started, "still held");
        // Off: releaseTicks ticks held, then the resync.
        d = m.Update(0, false);
        Check(d.hold && d.phase == eh::Phase::Held, "off 1: still held (debounce)");
        d = m.Update(0, false);
        Check(d.hold && d.phase == eh::Phase::Held, "off 2: still held");
        d = m.Update(0, false);
        Check(d.hold && d.resyncStarted && d.phase == eh::Phase::Resync, "off 3: the resync starts");
        Check(!d.releasing, "the resync waits for the add-on");
        d = m.Update(0, true);
        Check(d.hold && d.releasing && !d.timedOut, "acknowledged after the minimum: the last Hold tick");
        Check(!m.Update(0, true).hold, "then the clock runs");
        Check(m.Reasons() == eh::SignalLoading, "the hold's reasons");
    }

    void TestFlickerRestartsDebounce()
    {
        eh::Machine m(SmallConfig());
        m.Update(eh::SignalLoading, false);
        m.Update(0, false);
        m.Update(0, false);
        eh::Decision d = m.Update(eh::SignalLoading, false); // back before the release
        Check(d.hold && d.phase == eh::Phase::Held, "a signal inside the debounce keeps the hold");
        m.Update(0, false);
        m.Update(0, false);
        d = m.Update(0, false);
        Check(d.resyncStarted, "the debounce counts from the last signal");
        d = m.Update(eh::SignalLoading, true);
        Check(d.hold && d.phase == eh::Phase::Held && !d.releasing, "a loading screen in the resync holds again");
        for (int i = 0; i < 3; ++i)
            m.Update(0, false);
        d = m.Update(0, true);
        Check(d.releasing, "released after a second resync");
    }

    void TestPausedOnlyKeeps()
    {
        eh::Machine m(SmallConfig());
        Check(!m.Update(eh::SignalPaused, false).hold, "a paused game alone never starts a hold");
        m.Update(eh::SignalLoading, false);
        for (int i = 0; i < 10; ++i)
            Check(m.Update(eh::SignalPaused, false).phase == eh::Phase::Held, "a paused game keeps a hold");
        Check(m.Update(0, false).phase == eh::Phase::Held, "off: debounce");
    }

    void TestResyncTimeout()
    {
        eh::Machine m(SmallConfig());
        m.Update(eh::SignalLoading, false);
        eh::Decision d;
        int ticks = 0;
        do
        {
            d = m.Update(0, false);
            ++ticks;
        } while (!d.releasing && ticks < 50);
        Check(d.releasing && d.timedOut, "no acknowledgement: released at resyncMaxTicks, flagged");
        Check(ticks == 3 - 1 + 6, "the timeout counts resync ticks (debounce, then resyncMaxTicks)");
    }

    void TestScreensThatWaitCancel()
    {
        Check(eh::CancelReason(eh::SignalAlert) != nullptr && eh::CancelReason(eh::SignalMenu) != nullptr &&
                eh::CancelReason(eh::SignalClose) != nullptr,
            "the quit alert, the pause menu and a close request cancel the export");
        Check(eh::CancelReason(eh::SignalClose | eh::SignalAlert) == std::string("window_close"), "a close request names the cancel first");
        Check(eh::CancelReason(eh::SignalLoading) == nullptr && eh::CancelReason(eh::SignalPaused) == nullptr && eh::CancelReason(0) == nullptr,
            "a loading screen or a paused game never cancel");
        // The hold machine ignores them (the probe cancels before it).
        eh::Machine m(SmallConfig());
        Check(!m.Update(eh::SignalAlert | eh::SignalMenu | eh::SignalClose, false).hold, "the cancel signals never hold");
    }

    void TestRebaseMath()
    {
        Check(eh::FullSerialOfTag(1000, 1000 & 0xFF) == 1000, "the newest serial's own tag");
        Check(eh::FullSerialOfTag(1000, (1000 - 5) & 0xFF) == 995, "a tag a few ticks old");
        Check(eh::FullSerialOfTag(1000, (1000 - 255) & 0xFF) == 745, "the oldest the tag can name");
        Check(eh::FullSerialOfTag(300, 250) == 250 && eh::FullSerialOfTag(300, 40) == 296, "low bits across a wrap of the tag");
        // The count slipped by 7 presents during the hold: tick 995 shows at
        // count 995 + 2 + 7 with latency 2.
        Check(eh::RebaseDelta(1004, 995, 2) == -7, "the count goes back by the slip");
        Check(eh::RebaseDelta(990, 995, 2) == 7, "or forward");
        eh::RebaseVote vote;
        Check(!vote.Add(-7) && !vote.Add(-7), "two agreeing tags are not enough");
        Check(vote.Add(-7), "three agree");
        vote.Reset();
        vote.Add(-7);
        vote.Add(40); // a tag half covered by the menu fading out
        Check(!vote.Add(-7) && !vote.Add(-7) && vote.Add(-7), "a wrong read restarts the vote");
        Check(eh::HoldStepNs(0) == 1000000 && eh::HoldStepNs(500000) == 500000, "the hold step is the engine's minimum (1 ms unknown)");
        Check(eh::SignalNames(eh::SignalClose | eh::SignalMenu) == "menu+close" && eh::SignalNames(0) == "none", "the reason names");
    }

    struct Seen
    {
        ex::Phase phase;
        std::int64_t frame;
        std::uint32_t sample;
        std::uint32_t samples;
        std::int64_t timeNs;
        std::int64_t stepNs;
        bool first;
        bool last;
    };

    // The ticks a clock emits, with Hold ticks spliced in where `holdAt`
    // says (the Hold ticks themselves are left out of the result).
    std::vector<Seen> RunClock(std::uint32_t samples, const std::vector<std::uint64_t>& holdAt, std::uint32_t holdLength, std::uint64_t& lastSerial,
        bool& serialsOk, std::uint64_t& frames)
    {
        ex::Settings settings;
        settings.rate = {30, 1};
        settings.shutter = {samples, 180.0f};
        settings.prerollFrames = 4;
        settings.flushTicks = 3;
        settings.maxFrames = 20;
        ex::Clock clock;
        clock.Begin(settings);
        std::vector<Seen> out;
        std::uint64_t realTicks = 0;
        std::uint64_t expectSerial = 0;
        serialsOk = true;
        for (int guard = 0; guard < 10000; ++guard)
        {
            for (const std::uint64_t at : holdAt)
            {
                if (at != realTicks)
                    continue;
                for (std::uint32_t h = 0; h < holdLength; ++h)
                {
                    const ex::Tick hold = clock.HoldTick();
                    serialsOk = serialsOk && hold.serial == expectSerial++ && hold.phase == ex::Phase::Hold && hold.stepNs == 0;
                }
            }
            const ex::Tick tick = clock.Next();
            if (tick.phase == ex::Phase::Done)
                break;
            serialsOk = serialsOk && tick.serial == expectSerial++;
            lastSerial = tick.serial;
            ++realTicks;
            out.push_back({tick.phase, tick.frame, tick.sample, tick.samples, tick.timeNs, tick.stepNs, tick.firstOfFrame, tick.lastOfFrame});
        }
        frames = clock.FramesEmitted();
        return out;
    }

    void TestClockUnchangedAcrossHolds()
    {
        for (const std::uint32_t samples : {1u, 4u})
        {
            std::uint64_t lastA = 0, lastB = 0, framesA = 0, framesB = 0;
            bool okA = false, okB = false;
            const std::vector<Seen> plain = RunClock(samples, {}, 0, lastA, okA, framesA);
            // Holds in the preroll, mid frame (with 4 samples), on a frame
            // boundary and in the flush.
            const std::vector<Seen> held = RunClock(samples, {2, 7, 9, 15, plain.size() - 2}, 25, lastB, okB, framesB);
            Check(okA && okB, "serials run 0, 1, 2 ... through the Hold ticks");
            Check(framesA == framesB && framesA == 20, "the frame count is the same with holds");
            Check(plain.size() == held.size(), "the same real ticks with holds");
            bool same = plain.size() == held.size();
            for (std::size_t i = 0; same && i < plain.size(); ++i)
            {
                const Seen& a = plain[i];
                const Seen& b = held[i];
                same = a.phase == b.phase && a.frame == b.frame && a.sample == b.sample && a.samples == b.samples && a.timeNs == b.timeNs &&
                    a.stepNs == b.stepNs && a.first == b.first && a.last == b.last;
            }
            Check(same, "every real tick (frame, sample, time, step) is what it is without holds");
            Check(lastB == lastA + 5 * 25, "the Hold ticks only add serials");
        }
        // A Hold tick stands where the clock stands.
        ex::Settings settings;
        settings.rate = {60, 1};
        settings.shutter = {4, 180.0f};
        settings.prerollFrames = 0;
        ex::Clock clock;
        clock.Begin(settings);
        const ex::Tick a = clock.Next();
        const ex::Tick b = clock.Next();
        const ex::Tick hold = clock.HoldTick();
        Check(hold.frame == b.frame && hold.timeNs == b.timeNs && hold.sample == 2 && hold.samples == 4,
            "a Hold tick mid frame: the frame, the time of the last tick, the next sample");
        Check(!hold.firstOfFrame && !hold.lastOfFrame && hold.weight == 0.0f, "a Hold tick never touches the accumulator");
        const ex::Tick c = clock.Next();
        Check(c.sample == 2 && c.stepNs == c.timeNs - b.timeNs, "after it the next sample, its step from the last real tick");
        (void)a;
    }
}

int RunExportHoldTests()
{
    g_failures = 0;
    TestLoadingHoldAndRelease();
    TestFlickerRestartsDebounce();
    TestPausedOnlyKeeps();
    TestResyncTimeout();
    TestScreensThatWaitCancel();
    TestRebaseMath();
    TestClockUnchangedAcrossHolds();
    std::printf("export hold tests: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures;
}
