#pragma once

// The recording clock's pause spans (pose_capture.cpp, under g_pauseMutex), in
// a header the core tests compile too (tests/pause_spans_tests.cpp).
//
// Every recording time (NowMs, about a hundred call sites, and every drained
// pose frame) is the time since the start minus the paused ticks before it
// (each pause, cut and script stall of a second or more). The spans keep the
// running total of the spans before each: a query at or after the last span's
// start (nearly all of them) is O(1), an older one is a binary search, and the
// result equals a walk over every span.
//
// What the callers keep true (SetClockPaused, ExcludeScriptStall): spans are
// added in order of start, a span starts no earlier than the end of the one
// before (they may touch), and only the last one may be open (end 0: a span
// is only added while the clock runs, and the pause menu's span is closed
// before the clock runs again).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::pose::capture
{
    // In QPC ticks.
    struct PauseSpan
    {
        std::uint64_t start = 0;
        std::uint64_t end = 0; // 0 while open
    };

    class PauseSpans
    {
    public:
        void Clear()
        {
            m_spans.clear();
            m_before.clear();
        }

        bool Empty() const { return m_spans.empty(); }
        std::size_t Size() const { return m_spans.size(); }
        const PauseSpan& Back() const { return m_spans.back(); }
        const std::vector<PauseSpan>& Spans() const { return m_spans; }

        // Adds a span after the others (open with end 0). The span before it
        // is closed (see the top), so its length joins the running total.
        void Push(const PauseSpan& span)
        {
            m_before.push_back(m_spans.empty() ? 0 : m_before.back() + (m_spans.back().end - m_spans.back().start));
            m_spans.push_back(span);
        }

        // Closes the last span (the clock runs again).
        void CloseBack(std::uint64_t end) { m_spans.back().end = end; }

        // The ticks paused before `qpc`: every span that starts before qpc,
        // an open one up to qpc and a closed one up to its end or qpc,
        // whichever comes first. Every such span but the last one taken is
        // closed and ends at or before the next one's start, so before qpc:
        // whole, which is the running total; the last one is cut at qpc.
        std::uint64_t PausedBefore(std::uint64_t qpc) const
        {
            std::size_t taken = m_spans.size();
            if (taken == 0)
                return 0;
            if (m_spans.back().start >= qpc)
            {
                // Before the last span's start: the first span that starts
                // at or after qpc (starts are in order).
                taken = static_cast<std::size_t>(std::lower_bound(m_spans.begin(), m_spans.end(), qpc,
                    [](const PauseSpan& span, std::uint64_t value) { return span.start < value; }) - m_spans.begin());
                if (taken == 0)
                    return 0;
            }
            const PauseSpan& last = m_spans[taken - 1];
            const std::uint64_t end = last.end ? last.end : qpc;
            return m_before[taken - 1] + ((end < qpc ? end : qpc) - last.start);
        }

    private:
        std::vector<PauseSpan> m_spans;
        // m_before[i]: the total length of spans [0, i).
        std::vector<std::uint64_t> m_before;
    };
}
