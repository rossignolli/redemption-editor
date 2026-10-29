// src/pause_spans.h against the plain walk over the pause spans (kept
// here as the reference): the same integer for every query. Span lists
// are built the way the capture builds them (SetClockPaused opens and
// closes a span, ExcludeScriptStall adds a closed one; in order of start,
// each starting no earlier than the previous end, only the last one
// open), with spans that touch and empty ones, the last open or not, and
// queried before, on, inside, between and after the spans, also while
// the list is being built. Then the cost of the query the capture makes
// most (now, after every span) in a long session.
#include "pause_spans.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

namespace
{
    using replay::pose::capture::PauseSpan;
    using replay::pose::capture::PauseSpans;

    int g_spanFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_spanFailures;
    }

    // The reference walk.
    std::uint64_t ReferencePausedBefore(const std::vector<PauseSpan>& pauses, std::uint64_t qpc)
    {
        std::uint64_t paused = 0;
        for (const PauseSpan& span : pauses)
        {
            if (span.start >= qpc)
                break;
            const std::uint64_t end = span.end ? span.end : qpc;
            paused += (end < qpc ? end : qpc) - span.start;
        }
        return paused;
    }

    struct Tally
    {
        std::uint64_t queries = 0;
        std::uint64_t afterLastStart = 0; // the O(1) case
        std::uint64_t differ = 0;
    };

    void Compare(const PauseSpans& spans, const std::vector<PauseSpan>& plain, std::uint64_t qpc, Tally& tally)
    {
        ++tally.queries;
        if (!plain.empty() && qpc > plain.back().start)
            ++tally.afterLastStart;
        if (spans.PausedBefore(qpc) != ReferencePausedBefore(plain, qpc))
            ++tally.differ;
    }

    // Queries on and around every edge, between the spans, and anywhere.
    void QueryAll(const PauseSpans& spans, const std::vector<PauseSpan>& plain, std::mt19937_64& rng, Tally& tally)
    {
        constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();
        for (const std::uint64_t qpc : {std::uint64_t{0}, std::uint64_t{1}, kMax - 1, kMax})
            Compare(spans, plain, qpc, tally);
        for (const PauseSpan& span : plain)
        {
            for (const std::uint64_t qpc : {span.start - 1, span.start, span.start + 1})
                Compare(spans, plain, qpc, tally);
            const std::uint64_t end = span.end ? span.end : span.start + 1 + rng() % 5000000u;
            Compare(spans, plain, span.start + (end - span.start) / 2, tally);
            for (const std::uint64_t qpc : {end - 1, end, end + 1, end + 1 + rng() % 3000000u})
                Compare(spans, plain, qpc, tally);
        }
        if (!plain.empty())
        {
            const std::uint64_t from = plain.front().start > 4000000u ? plain.front().start - 4000000u : 0u;
            const std::uint64_t to = (plain.back().end ? plain.back().end : plain.back().start) + 4000000u;
            for (int index = 0; index < 24; ++index)
                Compare(spans, plain, from + rng() % (to - from + 1), tally);
        }
    }

    void TestRandomSpans()
    {
        std::mt19937_64 rng(1760);
        Tally tally;
        std::uint64_t lists = 0;
        std::uint64_t spansTotal = 0;
        std::uint64_t openLast = 0;
        std::uint64_t touching = 0;
        std::uint64_t empty = 0;
        bool sameSpans = true;
        for (int round = 0; round < 20000; ++round)
        {
            PauseSpans spans;
            std::vector<PauseSpan> plain;
            // QPC values are never 0 (the walk and the spans read end 0 as
            // open); a session start anywhere up to ~2^41 ticks.
            std::uint64_t now = 1 + rng() % (std::uint64_t{1} << 41);
            const int count = static_cast<int>(rng() % 14);
            for (int index = 0; index < count; ++index)
            {
                // The next span starts at or after the previous end.
                if (index > 0)
                {
                    if (rng() % 5 == 0)
                        ++touching;
                    else
                        now += 1 + rng() % 40000000u;
                }
                const std::uint64_t length = rng() % 9 == 0 ? 0 : 1 + rng() % 60000000u;
                empty += length == 0 ? 1u : 0u;
                const bool last = index == count - 1;
                const std::uint32_t how = static_cast<std::uint32_t>(rng() % 3);
                if (last && how == 0)
                {
                    // The pause menu, still open.
                    spans.Push({now, 0});
                    plain.push_back({now, 0});
                    ++openLast;
                }
                else if (how == 1)
                {
                    // The pause menu, opened, queried, closed.
                    spans.Push({now, 0});
                    plain.push_back({now, 0});
                    Compare(spans, plain, now + rng() % (length + 2), tally);
                    spans.CloseBack(now + length);
                    plain.back().end = now + length;
                }
                else
                {
                    // A script stall excluded after the fact (closed).
                    spans.Push({now, now + length});
                    plain.push_back({now, now + length});
                }
                now += length;
                Compare(spans, plain, now + rng() % 2000000u, tally); // the clock reads between the spans
            }
            QueryAll(spans, plain, rng, tally);
            sameSpans = sameSpans && spans.Size() == plain.size();
            for (std::size_t index = 0; sameSpans && index < plain.size(); ++index)
                sameSpans = spans.Spans()[index].start == plain[index].start && spans.Spans()[index].end == plain[index].end;
            ++lists;
            spansTotal += plain.size();
            spans.Clear();
            plain.clear();
            Compare(spans, plain, now, tally); // cleared (BeginRecording)
        }
        std::printf("  pause spans against the reference walk: %llu lists (%llu spans, %llu with the last one open, "
                    "%llu touching, %llu empty), %llu queries (%llu after the last start), %llu differ\n",
            static_cast<unsigned long long>(lists), static_cast<unsigned long long>(spansTotal),
            static_cast<unsigned long long>(openLast), static_cast<unsigned long long>(touching),
            static_cast<unsigned long long>(empty), static_cast<unsigned long long>(tally.queries),
            static_cast<unsigned long long>(tally.afterLastStart), static_cast<unsigned long long>(tally.differ));
        Check(tally.differ == 0, "4d: PauseSpans gives the walk's paused ticks for every query");
        Check(sameSpans, "4d: PauseSpans keeps the spans as they were added");
        Check(tally.afterLastStart > 0 && tally.queries > tally.afterLastStart, "4d: both query cases were exercised");
    }

    void TestEdges()
    {
        PauseSpans spans;
        Check(spans.PausedBefore(0) == 0 && spans.PausedBefore(12345) == 0, "4d: no span, nothing paused");
        spans.Push({100, 0}); // open
        Check(spans.PausedBefore(100) == 0, "4d: a query at an open span's start counts nothing");
        Check(spans.PausedBefore(150) == 50, "4d: an open span counts up to the query");
        spans.CloseBack(200);
        Check(spans.PausedBefore(150) == 50 && spans.PausedBefore(200) == 100 && spans.PausedBefore(999) == 100,
            "4d: a closed span counts up to its end");
        spans.Push({200, 260}); // touching
        spans.Push({260, 260}); // empty, touching
        spans.Push({300, 0});   // open
        const std::vector<PauseSpan> plain = {{100, 200}, {200, 260}, {260, 260}, {300, 0}};
        for (std::uint64_t qpc = 0; qpc < 420; ++qpc)
            Check(spans.PausedBefore(qpc) == ReferencePausedBefore(plain, qpc), "4d: touching, empty and open spans");
    }

    // The query the capture makes most: NowMs and every drained frame, at the
    // end of the session's spans.
    void BenchPausedBefore()
    {
        PauseSpans spans;
        std::vector<PauseSpan> plain;
        std::uint64_t now = 1000000;
        for (int index = 0; index < 2000; ++index)
        {
            spans.Push({now, now + 10000});
            plain.push_back({now, now + 10000});
            now += 50000;
        }
        constexpr int kQueries = 200000;
        volatile std::uint64_t sink = 0;
        const auto walkStart = std::chrono::steady_clock::now();
        for (int index = 0; index < kQueries; ++index)
            sink = sink + ReferencePausedBefore(plain, now + static_cast<std::uint64_t>(index));
        const auto walkEnd = std::chrono::steady_clock::now();
        for (int index = 0; index < kQueries; ++index)
            sink = sink + spans.PausedBefore(now + static_cast<std::uint64_t>(index));
        const auto spansEnd = std::chrono::steady_clock::now();
        const double walkNs = std::chrono::duration<double, std::nano>(walkEnd - walkStart).count() / kQueries;
        const double spansNs = std::chrono::duration<double, std::nano>(spansEnd - walkEnd).count() / kQueries;
        std::printf("  pause spans, 2000 spans (a long session), query after the last: walk %.1f ns -> %.1f ns\n",
            walkNs, spansNs);
        Check(sink != 0, "4d: benchmark ran");
    }
}

int RunPauseSpansTests()
{
    TestEdges();
    TestRandomSpans();
    BenchPausedBefore();
    if (g_spanFailures == 0)
        std::puts("Pause spans tests passed.");
    return g_spanFailures;
}
