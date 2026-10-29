// The recorder's speech matcher (speech_record_match.h), pure rules:
//   rule 1, a line created at an address drops what the recorder held there;
//   rule 2, an audible start is checked by voice, context and variation;
//   rule 3, a line only prepared never becomes a speech event;
//   the three detour rings merged in time order.
#include "speech_record_match.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_failuresPromptGhost = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failuresPromptGhost;
    }

    using replay::speech::rec::LineKey;
    using replay::speech::rec::Policy;
    using replay::speech::rec::StartResult;

    struct Line
    {
        std::string ped;
        std::uint32_t coreMs = 0;
    };
    using Matcher = replay::speech::rec::LineMatcher<Line>;

    LineKey Key(std::uint32_t voice, std::uint32_t context, std::int32_t variation)
    {
        LineKey key;
        key.voice = voice;
        key.context = context;
        key.variation = variation;
        return key;
    }

    // Clip 1051, address 0x1d522618180: the player's prompt option
    // 0x126ff966 var 6 created at 10560 ms and released without a stop; the
    // line of 72:73 0x654d191b var 1 created there at 38431 and started at
    // 38447.
    constexpr std::uint64_t kAddress = 0x1d522618180ull;
    const LineKey kOption = Key(0xe1b02e5bu, 0x126ff966u, 6);
    const LineKey kReal = Key(0x09268e6fu, 0x654d191bu, 1);

    void Create(Matcher& matcher, std::uint64_t address, const LineKey& key, const char* ped, std::uint32_t coreMs,
        std::vector<Matcher::Drop>* dropsOut = nullptr, std::vector<Matcher::StartedDrop>* startedOut = nullptr)
    {
        std::vector<Matcher::Drop> drops;
        std::vector<Matcher::StartedDrop> started;
        matcher.Created(address, drops, started);
        if (dropsOut)
            *dropsOut = drops;
        if (startedOut)
            *startedOut = started;
        matcher.AddPending(address, key, Line{ped, coreMs});
    }

    void TestGhostOfClip1051()
    {
        // Legacy policy: the start is given to the old option (the ghost).
        {
            Matcher legacy(Policy::Legacy);
            Create(legacy, kAddress, kOption, "6:7", 10560);
            Create(legacy, kAddress, kReal, "72:73", 38431);
            Matcher::Entry entry;
            std::vector<Matcher::Drop> drops;
            Check(legacy.Start(kAddress, true, kReal, entry, drops) == StartResult::Matched, "legacy: the start matches");
            Check(entry.value.ped == "6:7", "legacy: the ghost, the player's old option takes the start");
            Matcher::Entry stopped;
            Check(legacy.StopPending(kAddress, stopped) && stopped.value.ped == "72:73",
                "legacy: the real line's natural stop drops it as unheard");
        }
        // Default policy: the creation at the address drops the option (reused),
        // the start goes to the real line.
        {
            Matcher matcher;
            Create(matcher, kAddress, kOption, "6:7", 10560);
            std::vector<Matcher::Drop> drops;
            Create(matcher, kAddress, kReal, "72:73", 38431, &drops);
            Check(drops.size() == 1 && drops[0].entry.value.ped == "6:7" &&
                      drops[0].reason == replay::speech::rec::DropReason::Reused,
                "rule 1: the player's option is dropped as reused");
            Check(std::string(replay::speech::rec::DropReasonName(drops[0].reason)) == "reused", "rule 1: logged as reused");
            Matcher::Entry entry;
            std::vector<Matcher::Drop> startDrops;
            Check(matcher.Start(kAddress, true, kReal, entry, startDrops) == StartResult::Matched,
                "rule 2: the start matches the real line");
            Check(entry.value.ped == "72:73" && startDrops.empty(), "rule 2: recorded as 72:73");
            Check(matcher.Count().reusedPending == 1 && matcher.Count().startMismatch == 0, "counters: one reuse");
        }
    }

    void TestStartMismatch()
    {
        // The address is reused by a ped the clip does not record (its
        // creation is not a pending; rule 1 still runs for it in the mod, but
        // if that creation was missed, the start's identity decides).
        Matcher matcher;
        Create(matcher, kAddress, kOption, "6:7", 10560);
        Matcher::Entry entry;
        std::vector<Matcher::Drop> drops;
        const LineKey other = Key(0x11111111u, 0x22222222u, 3);
        Check(matcher.Start(kAddress, true, other, entry, drops) == StartResult::Mismatch,
            "rule 2: another identity at the start is a mismatch");
        Check(drops.size() == 1 && drops[0].reason == replay::speech::rec::DropReason::StartMismatch,
            "rule 2: the old pending is dropped");
        Check(matcher.Pendings().empty() && matcher.Count().startMismatch == 1, "rule 2: counted, nothing left");
        // A start where nothing waits.
        Check(matcher.Start(kAddress, true, other, entry, drops) == StartResult::NoPending, "no pending: not recorded");
        // The identity unreadable at the start: the address decides, counted.
        Create(matcher, kAddress, kReal, "72:73", 38431);
        Check(matcher.Start(kAddress, false, LineKey{}, entry, drops) == StartResult::Matched && entry.value.ped == "72:73",
            "unread identity: matched by address");
        Check(matcher.Count().startUnread == 1, "unread identity: counted");
    }

    void TestPromptGroupAndFlush()
    {
        // The prompt prepares three options at once; one is chosen and
        // starts, one is stopped (reason 2), one is released without a stop.
        Matcher matcher;
        Create(matcher, 0x100, Key(1, 0x948e57f4u, 1), "6:7", 1000);
        Create(matcher, 0x200, Key(1, 0x24713b9du, 2), "6:7", 1000);
        Create(matcher, 0x300, Key(1, 0xd3aeb520u, 3), "6:7", 1000);
        Matcher::Entry entry;
        std::vector<Matcher::Drop> drops;
        Check(matcher.Start(0x200, true, Key(1, 0x24713b9du, 2), entry, drops) == StartResult::Matched,
            "prompt: the chosen option starts");
        matcher.Started(0x200, 0);
        Check(matcher.StopPending(0x100, entry), "prompt: the stopped option is unheard");
        // Rule 3: the released one is never an event; at the end it is handed
        // back as only prepared.
        const std::vector<Matcher::Entry> left = matcher.TakePendings();
        Check(left.size() == 1 && left[0].line == 0x300, "rule 3: the option never started stays out of the clip");
        Check(matcher.StartedCount() == 0, "end of recording: started lines cleared");
    }

    void TestStartedReused()
    {
        // A recorded line whose stop was not seen (natural end): a new line
        // at the address must not inherit its stop.
        Matcher matcher;
        Create(matcher, kAddress, kReal, "72:73", 38431);
        Matcher::Entry entry;
        std::vector<Matcher::Drop> drops;
        matcher.Start(kAddress, true, kReal, entry, drops);
        matcher.Started(kAddress, 7);
        std::vector<Matcher::StartedDrop> started;
        Create(matcher, kAddress, Key(5, 6, 1), "32:238", 85000, nullptr, &started);
        Check(started.size() == 1 && started[0].eventIndex == 7, "rule 1: the started line at the address is released");
        std::size_t index = 0;
        Check(!matcher.StopStarted(kAddress, index), "rule 1: the new line's stop does not reach the old event");
        Check(matcher.StopPending(kAddress, entry) && entry.value.ped == "32:238", "the stop drops the new pending");
        Check(matcher.Count().reusedStarted == 1, "counter: reused_started");
    }

    void TestTimeline()
    {
        using replay::speech::rec::EventKind;
        using replay::speech::rec::TimedRef;
        // One tick: the core ring holds a creation at 150, the start ring a
        // start at 100 (the old line), the stop ring a stop at 120.
        std::vector<TimedRef> refs = {
            {150, EventKind::Create, 0},
            {100, EventKind::Start, 0},
            {120, EventKind::Stop, 0},
            {100, EventKind::Create, 1},
        };
        replay::speech::rec::SortTimeline(refs);
        Check(refs[0].kind == EventKind::Create && refs[0].index == 1, "timeline: equal times, creation first");
        Check(refs[1].kind == EventKind::Start && refs[2].kind == EventKind::Stop && refs[3].kind == EventKind::Create,
            "timeline: time order across the rings");
    }

    void TestSuspectGap()
    {
        using replay::speech::rec::SuspectStartGap;
        Check(SuspectStartGap(10560, 38447, true), "gap: a ghost of clip 1051 (27.9 s) is suspect");
        Check(!SuspectStartGap(9524, 9859, true), "gap: a real line (335 ms) is not");
        Check(!SuspectStartGap(12959, 13962, true), "gap: a chosen prompt option (1 s) is not");
        Check(!SuspectStartGap(10560, 38447, false), "gap: a line timed by its request is not judged");
    }
}

int RunSpeechPromptGhostTests()
{
    TestGhostOfClip1051();
    TestStartMismatch();
    TestPromptGroupAndFlush();
    TestStartedReused();
    TestTimeline();
    TestSuspectGap();
    return g_failuresPromptGhost;
}
