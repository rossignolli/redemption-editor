#include "profiler.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_profilerFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_profilerFailures;
    }

    bool Contains(const std::string& text, const char* needle)
    {
        return text.find(needle) != std::string::npos;
    }

    const replay::prof::ZoneStats* FindZone(const std::vector<replay::prof::ZoneStats>& zones, replay::prof::ZoneId zone)
    {
        for (const replay::prof::ZoneStats& stats : zones)
        {
            if (stats.name == replay::prof::Name(zone))
                return &stats;
        }
        return nullptr;
    }

    double SlowZoneUs(const replay::prof::SlowFrame& frame, replay::prof::ZoneId zone)
    {
        for (std::size_t index = 0; index < frame.zoneCount; ++index)
        {
            if (frame.zones[index] == replay::prof::Name(zone))
                return frame.zoneUs[index];
        }
        return -1.0;
    }

    bool SameZoneStats(const replay::prof::ZoneStats* a, const replay::prof::ZoneStats* b)
    {
        return a && b && a->calls == b->calls && a->totalUs == b->totalUs && a->avgUs == b->avgUs &&
            a->maxUs == b->maxUs && a->framePct == b->framePct;
    }

    // A zone's calls
    // spread over the thread stripes report exactly what the same calls on
    // one stripe report: calls, total, max, average and share in the window
    // and the session, the slow tick breakdown, and every reset.
    void TestZoneStripes()
    {
        using namespace replay::prof;
        SetTicksPerSecondForTests(1000000); // 1 tick = 1 us
        ResetSession();
        SetSlowFrameUs(50000.0);
        const ZoneId one = Register("test.stripes_one");
        const ZoneId spread = Register("test.stripes_spread");
        Check(one != kInvalidZone && spread != kInvalidZone && one != spread, "stripe test zones register");

        // A tie for the max on two stripes, a zero, the max after smaller ones.
        const std::uint64_t ticks[] = {700, 13, 40000, 1, 0, 999, 25000, 40000, 12345, 3, 77, 39999, 5};
        const std::size_t count = sizeof(ticks) / sizeof(ticks[0]);
        std::uint64_t total = 0;
        std::uint64_t largest = 0;
        std::size_t stripesUsed = 0;
        bool used[kZoneStripes] = {};
        for (std::size_t n = 0; n < count; ++n)
        {
            const std::size_t stripe = (n * 3 + 1) % kZoneStripes;
            AddToStripeForTests(one, 0, ticks[n]);
            AddToStripeForTests(spread, stripe, ticks[n]);
            stripesUsed += used[stripe] ? 0u : 1u;
            used[stripe] = true;
            total += ticks[n];
            largest = (std::max)(largest, ticks[n]);
        }
        Check(stripesUsed == kZoneStripes, "the spread zone's calls land on every stripe");
        AddToStripeForTests(spread, kZoneStripes, 1000000); // no such stripe: ignored
        EndFrame(250000); // slow: the tick's breakdown is kept

        const Snapshot window = TakeWindow();
        const ZoneStats* windowOne = FindZone(window.zones, one);
        const ZoneStats* windowSpread = FindZone(window.zones, spread);
        Check(windowOne && windowOne->calls == count && windowOne->totalUs == static_cast<double>(total) &&
                windowOne->maxUs == static_cast<double>(largest),
            "one stripe: the calls, total and max");
        Check(SameZoneStats(windowOne, windowSpread), "window: several stripes report what one stripe reports");
        Check(window.slow.size() == 1 && SlowZoneUs(window.slow[0], one) == static_cast<double>(total) &&
                SlowZoneUs(window.slow[0], spread) == SlowZoneUs(window.slow[0], one),
            "slow tick: the stripes' ticks summed");
        Check(!FindZone(TakeWindow().zones, spread), "the window reset clears every stripe");

        Snapshot session = Session();
        Check(SameZoneStats(FindZone(session.zones, one), FindZone(session.zones, spread)) &&
                FindZone(session.zones, spread)->calls == count,
            "session: several stripes report what one stripe reports");

        // EndFrame cleared every stripe's tick ticks: the next slow tick
        // holds only its own.
        AddToStripeForTests(spread, 2, 60000);
        AddToStripeForTests(spread, 3, 1000);
        EndFrame(70000);
        const Snapshot next = TakeWindow();
        Check(next.slow.size() == 1 && SlowZoneUs(next.slow[0], spread) == 61000.0 && SlowZoneUs(next.slow[0], one) < 0.0,
            "the slow tick breakdown starts over on every stripe");
        Check(FindZone(next.zones, spread) && FindZone(next.zones, spread)->calls == 2 &&
                FindZone(next.zones, spread)->maxUs == 60000.0,
            "window after a reset: only the new calls");
        session = Session();
        Check(FindZone(session.zones, spread) && FindZone(session.zones, spread)->calls == count + 2 &&
                FindZone(session.zones, spread)->totalUs == static_cast<double>(total + 61000),
            "the session keeps every stripe's calls across window resets");

        // Real threads, each on its own stripe: no call lost.
        ResetSession();
        static constexpr int kThreads = 4;
        static constexpr int kCallsPerThread = 5000;
        std::vector<std::thread> threads;
        for (int thread = 0; thread < kThreads; ++thread)
        {
            threads.emplace_back([spread, thread]() {
                for (int n = 0; n < kCallsPerThread; ++n)
                    Add(spread, static_cast<std::uint64_t>((thread + 1) * 10 + n % 7));
            });
        }
        for (std::thread& thread : threads)
            thread.join();
        std::uint64_t threadTotal = 0;
        for (int thread = 0; thread < kThreads; ++thread)
            for (int n = 0; n < kCallsPerThread; ++n)
                threadTotal += static_cast<std::uint64_t>((thread + 1) * 10 + n % 7);
        const Snapshot threadedSession = Session();
        const ZoneStats* threaded = FindZone(threadedSession.zones, spread);
        Check(threaded && threaded->calls == static_cast<std::uint64_t>(kThreads) * kCallsPerThread &&
                threaded->totalUs == static_cast<double>(threadTotal) && threaded->maxUs == 46.0,
            "concurrent adds from several threads: every call, the exact total and max");

        ResetSession();
        Check(Session().zones.empty() && TakeWindow().zones.empty(), "ResetSession clears every stripe");
        std::printf("Profiler stripes: %zu calls on %zu stripes, total %.1f ms, max %.1f ms, same as one stripe; "
                    "%d threads x %d calls, 0 lost\n",
            count, stripesUsed, static_cast<double>(total) / 1000.0, static_cast<double>(largest) / 1000.0, kThreads,
            kCallsPerThread);
    }
}

int RunProfilerTests()
{
    using namespace replay::prof;
    SetTicksPerSecondForTests(1000000); // 1 tick = 1 us
    ResetSession();

    // Registration is idempotent by name and bounded.
    const ZoneId a = Register("test.a");
    const ZoneId b = Register("test.b");
    Check(a != kInvalidZone && b != kInvalidZone && a != b, "two zones register");
    Check(Register("test.a") == a, "same name returns the same zone");
    Check(std::string(Name(a)) == "test.a", "zone name kept");

    // Percentile: nearest rank on sorted samples.
    Check(Percentile({}, 0.5) == 0.0, "percentile of nothing is 0");
    Check(Percentile({10.0}, 0.95) == 10.0, "percentile of one sample");
    Check(Percentile({1.0, 2.0, 3.0, 4.0}, 0.5) == 2.0, "p50 nearest rank");
    Check(Percentile({1.0, 2.0, 3.0, 4.0}, 0.95) == 4.0, "p95 nearest rank");

    // Accumulation: calls, total, max, and share of the frame time.
    Add(a, 100);
    Add(a, 300);
    Add(b, 50);
    EndFrame(1000);
    EndFrame(3000);
    Snapshot window = TakeWindow();
    Check(window.frame.frames == 2, "two frames in the window");
    Check(window.frame.totalUs == 4000.0, "frame total");
    Check(window.frame.avgUs == 2000.0, "frame average");
    Check(window.frame.p50Us == 1000.0 && window.frame.p95Us == 3000.0, "frame percentiles");
    Check(window.frame.maxUs == 3000.0, "frame max");
    Check(window.zones.size() == 2, "two active zones");
    Check(window.zones[0].name == Name(a), "largest zone first");
    Check(window.zones[0].calls == 2 && window.zones[0].totalUs == 400.0 &&
            window.zones[0].avgUs == 200.0 && window.zones[0].maxUs == 300.0,
        "zone a counters");
    Check(window.zones[0].framePct == 10.0, "zone a share of frame time");
    Check(window.zones[1].calls == 1 && window.zones[1].totalUs == 50.0, "zone b counters");

    // The window resets, the session keeps everything.
    Snapshot empty = TakeWindow();
    Check(empty.frame.frames == 0 && empty.zones.empty(), "window reset after take");
    Snapshot session = Session();
    Check(session.frame.frames == 2 && session.zones.size() == 2 &&
            session.zones[0].calls == 2,
        "session keeps the totals");

    // Scope adds one call (ticks may be 0 off Windows).
    {
        Scope scope(b);
    }
    Check(Session().zones.size() == 2, "scope adds a call to an existing zone");
    bool bHasTwoCalls = false;
    for (const ZoneStats& zone : Session().zones)
    {
        if (zone.name == Name(b))
            bHasTwoCalls = zone.calls == 2;
    }
    Check(bHasTwoCalls, "scope counted");

    // Report lines carry the tags the log reader parses.
    Add(a, 250);
    EndFrame(500);
    std::ostringstream out;
    WriteWindowReport(out, "record", 5000);
    const std::string report = out.str();
    Check(Contains(report, "PROFILE_WINDOW,mode=record,window_ms=5000"), "window header");
    Check(Contains(report, "PROFILE_FRAME,mode=record,frames=1,avg_us=500.0"), "frame line");
    Check(Contains(report, "PROFILE_ZONE,mode=record,zone=test.a,calls=1,avg_us=250.0,max_us=250.0,total_ms=0.25,frame_pct=50.0"),
        "zone line");
    Check(Contains(LastWindowLine(), "tick p95 0.50 ms | test.a 0.25"), "overlay line");
    std::ostringstream total;
    WriteSessionReport(total, "record");
    Check(Contains(total.str(), "PROFILE_TOTAL_FRAME,mode=record,frames=3"), "session frame line");
    Check(Contains(total.str(), "PROFILE_TOTAL_ZONE,mode=record,zone=test.a,calls=3"), "session zone line");

    // A slow tick keeps its own zone breakdown, largest first;
    // a fast tick's zone time does not leak into the next slow one.
    ResetSession();
    SetSlowFrameUs(50000.0);
    const ZoneId c = Register("test.c");
    Add(a, 30000);
    EndFrame(40000); // fast: not kept
    Add(b, 1000);
    Add(c, 70000);
    Add(a, 5000);
    EndFrame(90000); // slow
    Snapshot slowWindow = TakeWindow();
    Check(slowWindow.slow.size() == 1, "one slow tick kept");
    Check(slowWindow.slow.size() == 1 && slowWindow.slow[0].frameUs == 90000.0 && slowWindow.slow[0].zoneCount == 3 &&
            slowWindow.slow[0].zones[0] == Name(c) && slowWindow.slow[0].zoneUs[0] == 70000.0 &&
            slowWindow.slow[0].zones[1] == Name(a) && slowWindow.slow[0].zoneUs[1] == 5000.0 &&
            slowWindow.slow[0].zones[2] == Name(b),
        "slow tick zones are that tick's, largest first");
    for (int n = 0; n < 12; ++n)
    {
        Add(c, 60000);
        EndFrame(60000);
    }
    std::ostringstream slowOut;
    WriteWindowReport(slowOut, "play", 5000);
    std::size_t slowLines = 0;
    for (std::size_t at = slowOut.str().find("PROFILE_SLOW_TICK"); at != std::string::npos;
         at = slowOut.str().find("PROFILE_SLOW_TICK", at + 1))
        ++slowLines;
    Check(slowLines == kMaxSlowFrames, "slow tick lines capped per window");
    Check(Contains(slowOut.str(), "PROFILE_SLOW_TICK,mode=play,tick_ms=60.0,zones=test.c:60.0"), "slow tick line");
    Check(TakeWindow().slow.empty(), "slow ticks cleared with the window");

    ResetSession();
    Check(Session().frame.frames == 0 && Session().zones.empty(), "session reset");

    // The game frame between ticks, by
    // mode; a stall is counted apart; ResetSession keeps the totals, the
    // session report writes and clears them.
    (void)TakeGameFrameTotals();
    std::uint64_t now = 5000000;
    MarkGameFrame(now, "idle"); // first mark: no interval yet
    for (int n = 0; n < 10; ++n)
        MarkGameFrame(now += 16000, "idle");
    MarkGameFrame(now += 400000, "idle"); // pause menu: a stall
    for (int n = 0; n < 19; ++n)
        MarkGameFrame(now += 20000, "buffer");
    MarkGameFrame(now += 60000, "buffer");
    ResetSession();
    std::vector<GameFrameStats> frames = TakeGameFrameTotals();
    Check(frames.size() == 2, "two modes with game frames");
    if (frames.size() == 2)
    {
        const GameFrameStats& idle = std::string(frames[0].mode) == "idle" ? frames[0] : frames[1];
        const GameFrameStats& buffer = std::string(frames[0].mode) == "idle" ? frames[1] : frames[0];
        Check(idle.frames == 10 && idle.stalls == 1 && idle.avgUs == 16000.0, "idle frames, stall apart");
        Check(idle.p95Us == 16000.0 && idle.maxUs == 16000.0, "idle percentiles at the bin edge, capped by max");
        Check(buffer.frames == 20 && buffer.stalls == 0 && buffer.avgUs == 22000.0, "buffer frames");
        Check(buffer.p50Us == 20020.0 && buffer.p95Us == 20020.0 && buffer.maxUs == 60000.0, "buffer percentiles");
        Check(buffer.fps > 45.4 && buffer.fps < 45.5, "buffer fps");
    }
    Check(TakeGameFrameTotals().empty(), "totals cleared once taken");
    MarkGameFrame(now += 30000, "record");
    std::ostringstream gameOut;
    WriteSessionReport(gameOut, "record");
    Check(Contains(gameOut.str(), "PROFILE_TOTAL_GAME_FRAME,mode=record,frames=1,avg_us=30000.0"), "total game frame line");
    MarkGameFrame(now += 25000, "play");
    std::ostringstream windowOut;
    WriteWindowReport(windowOut, "play", 5000);
    Check(Contains(windowOut.str(), "PROFILE_GAME_FRAME,mode=play,frames=1,avg_us=25000.0"), "window game frame line");
    ResetSession();
    (void)TakeGameFrameTotals();
    TestZoneStripes();
    if (g_profilerFailures == 0)
        std::printf("Profiler tests passed.\n");
    return g_profilerFailures;
}
