// Manual part names, the buffer's real window and adaptive budget, the clip
// start's world readiness and the staggered puppet start.
#include "buffer_coverage.h"
#include "clip_compress.h"
#include "clip_start_gate.h"

#include <cstdio>

namespace
{
    int g_gmFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_gmFailures;
    }

    void TestPartNumbers()
    {
        using replay::buffercov::FinalPartNumber;
        Check(FinalPartNumber(0) == 0, "a recording that never split has no part suffix");
        Check(FinalPartNumber(1) == 2, "one part saved: the final save is part 2");
        // Parts p1..p4 saved by the ring: the stop must save p5, not p4 again.
        Check(FinalPartNumber(4) == 5, "four parts saved: the final save is p5, not p4");
    }

    // The manual part's limit (ManualMaxMB), with the buffer off and inside it.
    void TestManualParts()
    {
        using namespace replay::buffercov;
        constexpr std::size_t MB = 1024 * 1024;
        // The largest part plus a 2 s block of a heavy scene (512 MB parts closed
        // up to 31 MB past the limit) still opens.
        static_assert(static_cast<std::uint64_t>(kManualMaxMB + 64) * MB <= replay::clipcompress::kMaxClipBytes,
            "the largest manual part and its overshoot fit a clip file");
        Check(ClampManualMB(900) == 900 && ClampManualMB(8) == 32 && ClampManualMB(4096) == 960, "ManualMaxMB 32 to 960");
        // ReplayBufferMB: the same ceiling, so a saved buffer opens again.
        static_assert(static_cast<std::uint64_t>(kBufferMaxMB + 64) * MB <= replay::clipcompress::kMaxClipBytes,
            "the largest buffer and its overshoot fit a clip file");
        Check(ClampBufferMB(128) == 128 && ClampBufferMB(4) == 16 && ClampBufferMB(1024) == 960 && ClampBufferMB(4096) == 960,
            "ReplayBufferMB 16 to 960 (ini and Replay Settings)");
        // Buffer off: ManualMaxMB.
        Check(ManualPartBytes(900, 0) == 900 * MB, "no buffer: the part closes at ManualMaxMB");
        Check(ManualPartBytes(5000, 0) == 960 * MB, "an ini value past the clip cap is held under it");
        // Buffer on: the larger of ManualMaxMB and the ring's budget, under the cap.
        Check(ManualPartBytes(900, 128 * MB) == 900 * MB, "the default 128 MB ring no longer cuts a manual part at 128 MB");
        Check(ManualPartBytes(64, 256 * MB) == 256 * MB, "a ring larger than ManualMaxMB holds the part to the ring");
        Check(ManualPartBytes(900, 2048 * MB) == 960 * MB, "a 2 GB ring still splits under the clip cap");
        // The ring: its own budget before the mark, the part's at the mark.
        Check(!HeadAtManualMark(false, 50000, 60000), "no mark");
        Check(!HeadAtManualMark(true, 50000, 60000), "the head 10 s before the mark: the buffer's seconds");
        Check(HeadAtManualMark(true, 58001, 60000) && HeadAtManualMark(true, 60000, 60000), "within a block of the mark");
        const std::size_t part = ManualPartBytes(900, 128 * MB);
        Check(RingLimitBytes(false, 128 * MB, part) == 128 * MB, "before the mark the ring keeps its budget");
        Check(RingLimitBytes(true, 128 * MB, part) == 900 * MB, "at the mark the manual part may reach ManualMaxMB");
        Check(RingLimitBytes(true, 2048 * MB, ManualPartBytes(900, 2048 * MB)) == 960 * MB, "at the mark never past the clip cap");
        // ManualMaxSeconds, from the part's start.
        Check(!ManualTimeUp(0, 999999, 0), "0: no time limit");
        Check(!ManualTimeUp(20, 19999, 0) && ManualTimeUp(20, 20000, 0), "20 s without the buffer");
        Check(!ManualTimeUp(20, 50000, 31000) && ManualTimeUp(20, 51000, 31000), "20 s from a mark at 31 s");
        Check(!ManualTimeUp(20, 1000, 31000), "a clock before the mark is not a part");
    }

    void TestCoverage()
    {
        using namespace replay::buffercov;
        // Now 126294, head 98000, 60 s window at 128 MB.
        Coverage c = Evaluate(126294, 98000, 60, false, 0);
        Check(c.coveredMs == 28294 && c.targetMs == 60000, "covered and target ms");
        Check(c.limit == Limit::Memory, "a head past zero short of the window is the memory budget");
        Check(ShortWindowNotice(c), "28 of 60 s tells the player");
        c = Evaluate(70000, 10000, 60, false, 0);
        Check(c.limit == Limit::Time && !ShortWindowNotice(c), "the whole window");
        c = Evaluate(59000, 1000, 60, false, 0);
        Check(c.limit == Limit::Time, "within one block of the window counts as whole");
        c = Evaluate(20000, 0, 60, false, 0);
        Check(c.limit == Limit::Start && !ShortWindowNotice(c), "a buffer started 20 s ago is short by its start");
        c = Evaluate(200000, 50000, 60, true, 50000);
        Check(c.limit == Limit::Manual && !ShortWindowNotice(c), "a manual mark holds the head past the window");
        c = Evaluate(100000, 46000, 60, false, 0);
        Check(c.limit == Limit::Memory && !ShortWindowNotice(c), "54 of 60 s is short but above 90 %: no notice");
    }

    void TestAdaptive()
    {
        using namespace replay::buffercov;
        constexpr std::size_t MB = 1024 * 1024;
        AdaptiveConfig config;
        config.baseMB = 128;
        config.maxMB = 512;
        config.reserveMB = 2048;
        Budget b = AdaptiveBudget(config, 100 * MB, 16000);
        Check(b.bytes == 128 * MB && !b.grown, "off: the configured budget");
        config.enabled = true;
        b = AdaptiveBudget(config, 100 * MB, 16000);
        Check(b.bytes == 512 * MB && b.grown && !b.memoryLow, "on with plenty of memory: the cap");
        b = AdaptiveBudget(config, 128 * MB, 1500);
        Check(b.bytes == 128 * MB && b.memoryLow, "on below the reserve: the configured budget");
        // 300 MB used (172 grown), 1976 MB free: it had 2148 before growing,
        // room 100 above the reserve, so the cap is 228 MB.
        b = AdaptiveBudget(config, 300 * MB, 1976);
        Check(b.bytes == 228 * MB && b.grown && b.memoryLow, "growth limited by the free memory it already took");
        config.maxMB = 64;
        b = AdaptiveBudget(config, 0, 16000);
        Check(b.bytes == 128 * MB && !b.grown, "a cap below the base keeps the base");
    }

    void TestMegabytesFor()
    {
        using replay::buffercov::MegabytesFor;
        Check(MegabytesFor(60, 6.5) == 512, "Saint Denis 6.5 MB/s x 60 s rounds to 512");
        Check(MegabytesFor(60, 7.44) == 512, "the worst 7.44 MB/s still fits 512");
        Check(MegabytesFor(60, 2.0) == 128, "2 MB/s fits the default");
        Check(MegabytesFor(300, 7.0) == 2048, "past the steps");
    }

    void TestWorldReady()
    {
        using namespace replay::clipstart;
        WorldFacts facts;
        Check(WorldReady(facts, true), "all facts true");
        facts.collisionLoaded = false;
        Check(!WorldReady(facts, true) && WorldReady(facts, false), "the collision only with the world wait");
        facts = WorldFacts{};
        facts.iplsApplied = false;
        Check(!WorldReady(facts, false), "IPLs not applied yet");
        Config config;
        Check(StartWaitTimeoutMs(config, 8000) == 8000, "off: the old cap");
        config.worldWait = true;
        Check(StartWaitTimeoutMs(config, 8000) == 20000, "world wait: its cap");
        config.stagger = true;
        Check(StartWaitTimeoutMs(config, 8000) == 31000, "stagger: world cap + settle cap + preparation");
    }

    void TestGate()
    {
        using namespace replay::clipstart;
        Config config;
        Gate gate;
        gate.Begin(1000, config);
        Check(gate.CurrentPhase() == Phase::Off && !gate.Holding() && gate.CapBudget(4) == 4, "stagger off: nothing held");

        config.stagger = true;
        config.batch = 2;
        gate.Begin(1000, config);
        Check(gate.Holding() && gate.CapBudget(4) == 0, "held: no creates");
        WorldFacts facts;
        facts.iplsApplied = false;
        Check(!gate.Feed(1100, facts).release, "IPLs pending: held");
        facts.iplsApplied = true;
        facts.collisionLoaded = false;
        Check(!gate.Feed(1200, facts).release && gate.CurrentPhase() == Phase::WaitWorld, "no collision: held");
        facts.collisionLoaded = true;
        facts.streamingRequests = 40;
        Check(!gate.Feed(1300, facts).release && gate.CurrentPhase() == Phase::Settle, "world ready: settle");
        Check(!gate.Feed(1700, facts).release, "settle shorter than the minimum");
        Check(!gate.Feed(1900, facts).release, "streaming busy past the minimum: still settling");
        facts.streamingRequests = 3;
        Step step = gate.Feed(1900, facts);
        Check(step.release && !step.timedOut && gate.Batching(), "quiet streaming after the minimum: released");
        Check(gate.CapBudget(4) == 2 && gate.CapBudget(1) == 1, "batches: the smaller of the two");
        gate.PuppetsBuilt();
        Check(gate.CurrentPhase() == Phase::Done && gate.CapBudget(4) == 4, "built: the normal budget");

        gate.Begin(0, config);
        facts.streamingRequests = 50;
        gate.Feed(100, facts);
        step = gate.Feed(3000, facts);
        Check(!step.release, "the settle cap counts from the world ready, not from the begin");
        step = gate.Feed(100 + config.settleCapMs, facts);
        Check(step.release && !step.timedOut, "busy streaming: released at the settle cap");

        gate.Begin(0, config);
        facts.sceneLoaded = false;
        Check(!gate.Feed(config.worldCapMs - 1, facts).release, "before the world cap");
        step = gate.Feed(config.worldCapMs, facts);
        Check(step.release && step.timedOut && gate.TimedOut(), "the world cap releases anyway");

        gate.Begin(0, config);
        gate.Reset();
        Check(!gate.Holding() && !gate.Batching(), "a stop resets the gate");
    }

    void TestLogRule()
    {
        using namespace replay::clipstart;
        Check(LogThisTick(0, false, 0, 0), "the first tick logs");
        Check(LogThisTick(kEveryTickLines - 1, false, 10, 9), "every tick at first");
        Check(!LogThisTick(kEveryTickLines, false, 100, 50), "then sparse");
        Check(LogThisTick(kEveryTickLines, true, 100, 50), "a change logs");
        Check(LogThisTick(kEveryTickLines, false, 600, 50), "half a second logs");
        Check(!LogThisTick(kMaxLines, true, 600, 50), "bounded per start");
        WorldFacts facts;
        Check(FactsBits(facts) == 31u, "all bits");
        facts.collisionLoaded = false;
        Check(FactsBits(facts) == 27u, "collision bit");
    }
}

int RunWorldRecordingTests()
{
    TestPartNumbers();
    TestManualParts();
    TestCoverage();
    TestAdaptive();
    TestMegabytesFor();
    TestWorldReady();
    TestGate();
    TestLogRule();
    if (g_gmFailures == 0)
        std::puts("world recording tests passed.");
    return g_gmFailures;
}
