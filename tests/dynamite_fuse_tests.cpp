// The dynamite fuse (a script looped effect kept alive by a per-frame get
// without a hash, which must not be recorded for one tick only) and the reins
// of a wagon (the draft reins rest mode puts itself back over the recorded
// hand pins every frame).
#include "rope_track.h"
#include "script_fx_loop.h"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace
{
    int g_dynamiteFuseFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_dynamiteFuseFailures;
    }

    using namespace replay::scriptfx;

    void TestResolveHashless()
    {
        constexpr std::uint32_t kFuse = 0xC8C11A94; // proj_dynamite_trail
        constexpr std::uint64_t kInst = 0x1A99CC70E40;
        // A call with its hash is left as it is.
        HashlessResult r = ResolveHashless(kFuse, kInst, false, 0, false, 0);
        Check(r.how == Hashless::NotHashless && r.hash == kFuse && r.Keep(), "a hashed call keeps its hash");
        // The keep-alive get of the fuse: the key is live (its start was
        // recorded at 49786 ms), the instance holds the fuse effect.
        r = ResolveHashless(0, kInst, true, kFuse, true, kFuse);
        Check(r.how == Hashless::LiveKey && r.hash == kFuse && r.Keep(), "keep-alive call takes its live key's effect");
        // The instance unreadable: the live key still decides.
        r = ResolveHashless(0, kInst, true, kFuse, false, 0);
        Check(r.how == Hashless::LiveKey && r.hash == kFuse, "unreadable instance, live key decides");
        // The key's instance now holds another effect (pooled, reused): dropped.
        r = ResolveHashless(0, kInst, true, kFuse, true, 0x5B7159D9);
        Check(r.how == Hashless::Mismatch && !r.Keep(), "another effect in the instance is dropped");
        // A script effect started before the recording: no live key, the
        // instance's own effect.
        r = ResolveHashless(0, kInst, false, 0, true, kFuse);
        Check(r.how == Hashless::Identity && r.hash == kFuse && r.Keep(), "no live key: the instance's effect");
        // Nothing to go on.
        r = ResolveHashless(0, kInst, false, 0, false, 0);
        Check(r.how == Hashless::Unresolved && !r.Keep(), "no key and no readable instance is dropped");
        r = ResolveHashless(0, 0, true, kFuse, false, 0);
        Check(r.how == Hashless::Unresolved && !r.Keep(), "no instance is dropped");
    }

    void TestLoopTally()
    {
        // The fuse recorded for one tick: the two fuse tracks end at their start.
        LoopTally before;
        LoopTrackPlay a;
        a.recStartMs = a.recEndMs = 49786;
        a.entered = true;
        a.firstMs = a.lastMs = 49790;
        a.passedEnd = true;
        before.Add(a);
        LoopTrackPlay b = a;
        b.recStartMs = b.recEndMs = 50337;
        before.Add(b);
        Check(before.tracks == 2 && before.oneTick == 2, "132 before the fix: two one-tick tracks");
        Check(std::string_view(before.Verdict()) == "one_tick", "132 before the fix: verdict one_tick");

        // The fuse recorded whole: 49786 to 59370 (the stop before the blast),
        // played from 49790 to 59360 in normal play.
        LoopTally after;
        LoopTrackPlay c;
        c.recStartMs = 49786;
        c.recEndMs = 59370;
        c.entered = true;
        c.firstMs = 49790;
        c.lastMs = 59360;
        c.passedEnd = true;
        after.Add(c);
        Check(after.complete == 1 && after.shortPlay == 0 && after.oneTick == 0, "fixed fuse played whole");
        Check(after.recMs == 9584 && after.playedMs == 9570, "fixed fuse spans");
        Check(std::string_view(after.Verdict()) == "ok", "fixed fuse: verdict ok");

        // Requests stopped 2 s early (the stick's puppet gone): short.
        LoopTally early;
        LoopTrackPlay d = c;
        d.lastMs = 57370;
        early.Add(d);
        Check(early.shortPlay == 1 && std::string_view(early.Verdict()) == "short", "requests stopping early are short");

        // A seek into the middle or a stop before the end is not judged.
        LoopTally partial;
        LoopTrackPlay e = c;
        e.firstMs = 55000;
        partial.Add(e);
        LoopTrackPlay f = c;
        f.passedEnd = false;
        f.lastMs = 52000;
        partial.Add(f);
        LoopTrackPlay g = c;
        g.entered = false;
        partial.Add(g);
        Check(partial.tracks == 3 && partial.entered == 2 && partial.complete == 0 && partial.shortPlay == 0,
            "partial plays are not judged");
        Check(std::string_view(partial.Verdict()) == "ok", "partial plays: verdict ok");
        Check(std::string_view(LoopTally{}.Verdict()) == "none", "no script track: none");
    }

    void TestPinTally()
    {
        using namespace replay::ropes;
        // A hand pin written at 9940, 9957 and 9973 ms and undone each time
        // by the rest mode; yielded at 9990.
        SlotGuard guard;
        PinTally tally;
        SlotWrite decision = DecideSlotWrite(guard, 9932, false);
        tally.Note(false, decision);
        decision = DecideSlotWrite(guard, 9932, false);
        tally.Note(false, decision);
        decision = DecideSlotWrite(guard, 9932, false);
        tally.Note(false, decision);
        decision = DecideSlotWrite(guard, 9932, false);
        tally.Note(false, decision);
        Check(decision == SlotWrite::Yield, "182: the fourth undone write yields");
        Check(tally.yields == 1 && tally.held == 0, "182: the yield is counted");
        Check(std::string_view(tally.Verdict()) == "engine_owned", "182 before the fix: engine_owned");

        // One write at the state change, held after.
        SlotGuard guard2;
        PinTally fixed;
        fixed.Note(false, DecideSlotWrite(guard2, 9932, false));
        for (int tick = 0; tick < 99; ++tick)
            fixed.Note(true, DecideSlotWrite(guard2, 9932, true));
        Check(fixed.slotTicks == 100 && fixed.held == 99 && fixed.yields == 0, "fixed pins held");
        Check(std::string_view(fixed.Verdict()) == "ok", "fixed pins: ok");

        // Held only part of the time without a yield (the engine wins every
        // other frame): engine_fights.
        PinTally fights;
        for (int tick = 0; tick < 10; ++tick)
            fights.Note(tick % 2 == 0, SlotWrite::Hold);
        Check(std::string_view(fights.Verdict()) == "engine_fights", "half held: engine_fights");
        Check(std::string_view(PinTally{}.Verdict()) == "none", "no pin: none");
    }
}

int RunDynamiteFuseTests()
{
    g_dynamiteFuseFailures = 0;
    TestResolveHashless();
    TestLoopTally();
    TestPinTally();
    std::printf("dynamite fuse tests: %d failure(s)\n", g_dynamiteFuseFailures);
    return g_dynamiteFuseFailures;
}
