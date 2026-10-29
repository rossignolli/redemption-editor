#pragma once

// Recording of speech lines: which audible start belongs to which created
// line (pure, unit tested, replayed on a session log by ReplayClipTool
// speech-log).
//
// Matching an audible start with the oldest pending line at the same line
// address is wrong: the interaction prompt (greet, antagonize) prepares the
// player's options in groups of 2 or 3 when a ped is focused and the speech
// entity's update releases the unused ones through 0x2FA67FC -> 0x2FA167C,
// which never calls the stop 0x2FBCCD4 the recorder observes (build
// 1491.50). The pending option would stay in the recorder; seconds later
// another line is created at the same address and its audible start would be
// recorded as the player's option (ghost lines, real lines lost).
//
// A replay editor usually keys a line by voice, context and variation, never
// by a pointer. Here:
//   1. a line created at an address drops every pending (and every started
//      line) still held at that address: that line no longer exists
//      ("reused");
//   2. an audible start is checked against the pending's voice, context and
//      variation, read from the line data at the start (+0x1D8 -> +0x110,
//      +0x114, +0x118, written only by the data init 0x2FA92C0 at creation;
//      the line release 0x2FC1524 clears +0x1D8 and the next line init
//      0x2FABBAC allocates new data). A mismatch drops the pending
//      ("start_mismatch"): the start belongs to another line;
//   3. a line only prepared (created, never started) never becomes a speech
//      event: the caller logs it as unheard.
//
// The Legacy policy (oldest pending, no reuse drop, no identity check) is
// kept only so the clip tool can show the counts of both on a recorded
// session log; the mod uses Identity.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::speech::rec
{
    struct LineKey
    {
        std::uint32_t voice = 0;     // final hash (data +0x110)
        std::uint32_t context = 0;   // partial hash (data +0x114)
        std::int32_t variation = 0;  // data +0x118
    };

    inline bool SameLine(const LineKey& a, const LineKey& b)
    {
        return a.voice == b.voice && a.context == b.context && a.variation == b.variation;
    }

    enum class Policy : std::uint8_t
    {
        Identity = 0, // reuse drop, identity check, newest entry of an address
        Legacy = 1,   // oldest pending of an address, no checks
    };

    enum class DropReason : std::uint8_t
    {
        Reused = 0,        // another line was created at the same address
        StartMismatch = 1, // the start at the address carried another identity
    };

    inline const char* DropReasonName(DropReason reason)
    {
        return reason == DropReason::Reused ? "reused" : "start_mismatch";
    }

    enum class StartResult : std::uint8_t
    {
        Matched = 0,       // the start belongs to the returned pending
        NoPending = 1,     // no pending at that address (a line of a ped not recorded)
        Mismatch = 2,      // pendings at the address, none with the start's identity
    };

    struct Counters
    {
        std::uint64_t created = 0;
        std::uint64_t reusedPending = 0;  // pendings dropped by rule 1
        std::uint64_t reusedStarted = 0;  // started lines whose stop was never seen, address reused
        std::uint64_t startMatched = 0;
        std::uint64_t startMismatch = 0;  // pendings dropped by rule 2
        std::uint64_t startUnread = 0;    // matched by address only: the identity was not readable at the start
        std::uint64_t startNoPending = 0;
    };

    template <typename Pending>
    class LineMatcher
    {
    public:
        struct Entry
        {
            std::uint64_t line = 0;
            LineKey key;
            Pending value{};
        };
        struct Drop
        {
            DropReason reason = DropReason::Reused;
            Entry entry;
        };
        struct StartedDrop
        {
            std::uint64_t line = 0;
            std::size_t eventIndex = 0;
        };

        explicit LineMatcher(Policy policy = Policy::Identity) : m_policy(policy) {}

        Policy GetPolicy() const { return m_policy; }
        const Counters& Count() const { return m_counters; }
        const std::vector<Entry>& Pendings() const { return m_pending; }
        std::size_t StartedCount() const { return m_started.size(); }

        void Clear()
        {
            m_pending.clear();
            m_started.clear();
            m_counters = Counters{};
        }

        // Rule 1. Any line the core created at `line`, recorded speaker or
        // not: what the recorder still holds at that address is gone.
        void Created(std::uint64_t line, std::vector<Drop>& drops, std::vector<StartedDrop>& startedDrops)
        {
            ++m_counters.created;
            if (m_policy == Policy::Legacy || !line)
                return;
            for (std::size_t n = 0; n < m_pending.size();)
            {
                if (m_pending[n].line != line)
                {
                    ++n;
                    continue;
                }
                drops.push_back({DropReason::Reused, m_pending[n]});
                ++m_counters.reusedPending;
                m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
            }
            for (std::size_t n = 0; n < m_started.size();)
            {
                if (m_started[n].line != line)
                {
                    ++n;
                    continue;
                }
                startedDrops.push_back(m_started[n]);
                ++m_counters.reusedStarted;
                m_started.erase(m_started.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }

        // A line of a recorded speaker that waits for its audible start
        // (call Created first, for every created line).
        void AddPending(std::uint64_t line, const LineKey& key, const Pending& value)
        {
            m_pending.push_back({line, key, value});
        }

        // Rule 2. `keyRead` false: the start's identity could not be read
        // (guarded read failed); the address alone decides, counted.
        StartResult Start(std::uint64_t line, bool keyRead, const LineKey& key, Entry& out, std::vector<Drop>& drops)
        {
            if (m_policy == Policy::Legacy)
            {
                for (std::size_t n = 0; n < m_pending.size(); ++n)
                {
                    if (m_pending[n].line != line)
                        continue;
                    out = m_pending[n];
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
                    ++m_counters.startMatched;
                    return StartResult::Matched;
                }
                ++m_counters.startNoPending;
                return StartResult::NoPending;
            }
            bool any = false;
            // Newest first: the live line at an address is the last created.
            for (std::size_t n = m_pending.size(); n-- > 0;)
            {
                if (m_pending[n].line != line)
                    continue;
                any = true;
                if (!keyRead || SameLine(m_pending[n].key, key))
                {
                    out = m_pending[n];
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
                    ++m_counters.startMatched;
                    if (!keyRead)
                        ++m_counters.startUnread;
                    return StartResult::Matched;
                }
            }
            if (!any)
            {
                ++m_counters.startNoPending;
                return StartResult::NoPending;
            }
            for (std::size_t n = 0; n < m_pending.size();)
            {
                if (m_pending[n].line != line)
                {
                    ++n;
                    continue;
                }
                drops.push_back({DropReason::StartMismatch, m_pending[n]});
                ++m_counters.startMismatch;
                m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
            }
            return StartResult::Mismatch;
        }

        // A recorded line that started: its stop may set the event's stop.
        void Started(std::uint64_t line, std::size_t eventIndex)
        {
            if (line)
                m_started.push_back({line, eventIndex});
        }

        // A stop at `line`: the pending there was never heard.
        bool StopPending(std::uint64_t line, Entry& out)
        {
            const std::size_t n = Find(m_pending, line);
            if (n == kNone)
                return false;
            out = m_pending[n];
            m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
            return true;
        }

        // A stop at `line`: the started line there ends.
        bool StopStarted(std::uint64_t line, std::size_t& eventIndex)
        {
            const std::size_t n = Find(m_started, line);
            if (n == kNone)
                return false;
            eventIndex = m_started[n].eventIndex;
            m_started.erase(m_started.begin() + static_cast<std::ptrdiff_t>(n));
            return true;
        }

        // A recorded line already started at `line` (its stop not seen yet):
        // a second start there is not a new line.
        bool IsStarted(std::uint64_t line) const { return line && Find(m_started, line) != kNone; }

        // End of recording: the lines only prepared (never heard).
        std::vector<Entry> TakePendings()
        {
            std::vector<Entry> out;
            out.swap(m_pending);
            m_started.clear();
            return out;
        }

    private:
        static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

        // Legacy: the first (oldest) entry; Identity: the newest.
        template <typename T>
        std::size_t Find(const std::vector<T>& list, std::uint64_t line) const
        {
            if (m_policy == Policy::Legacy)
            {
                for (std::size_t n = 0; n < list.size(); ++n)
                    if (list[n].line == line)
                        return n;
                return kNone;
            }
            for (std::size_t n = list.size(); n-- > 0;)
                if (list[n].line == line)
                    return n;
            return kNone;
        }

        Policy m_policy;
        std::vector<Entry> m_pending;
        std::vector<StartedDrop> m_started;
        Counters m_counters;
    };

    // Event kinds of one recording tick, merged in time order so a creation,
    // a start and a stop at the same address are applied as they happened
    // (the three detour rings are separate).
    enum class EventKind : std::uint8_t
    {
        Create = 0,
        Start = 1,
        Stop = 2,
    };

    struct TimedRef
    {
        std::uint64_t timeUs = 0;
        EventKind kind = EventKind::Create;
        std::size_t index = 0;
    };

    // Stable order by time; on equal times creation, then start, then stop.
    inline void SortTimeline(std::vector<TimedRef>& refs)
    {
        std::stable_sort(refs.begin(), refs.end(), [](const TimedRef& a, const TimedRef& b) {
            return a.timeUs != b.timeUs ? a.timeUs < b.timeUs : static_cast<int>(a.kind) < static_cast<int>(b.kind);
        });
    }

    // A scripted conversation asks all its lines at once when it begins (nine
    // lines created at the same core_ms, started one by one over the next
    // 15 s). A conversation that began before F4 had its lines created before
    // the recording: the recorder never held them as pending, so their audible
    // starts during the recording find nothing (NoPending). A replay editor
    // usually does not key speech on the request at all: the speech entity's
    // state is recorded every frame (voice, context, variation, start offset
    // -1 until the line plays), so a line asked before the recording is
    // recorded as soon as it plays. Here an audible start with no pending
    // line, on a recorded speaker, is adopted from the line itself at that
    // start (identity and request read from the line, as the adoption of a
    // line already playing does). The creations the core ring still holds
    // from before the recording tell which of them were asked before it
    // ("queued") from any other start without a seen request ("unrequested").
    enum class AdoptWhy : std::uint8_t
    {
        Playing = 0,     // already audible when the recording began
        Queued = 1,      // created before the recording, audible start during it
        Unrequested = 2, // audible start during the recording, its creation not seen
    };

    inline const char* AdoptWhyName(AdoptWhy why)
    {
        switch (why)
        {
        case AdoptWhy::Playing: return "playing";
        case AdoptWhy::Queued: return "queued";
        case AdoptWhy::Unrequested: return "unrequested";
        }
        return "unknown";
    }

    // An audible start the recorder adopts from the line: no pending line at
    // its address (a Mismatch keeps rule 2: the start belongs to no line the
    // recorder knows, nothing is recorded as it), no recorded line already
    // started there, its identity read and the line playing (state 5) after
    // the start.
    inline bool AdoptsStart(StartResult result, bool alreadyStarted, bool identityRead, std::int32_t stateAfter)
    {
        return result == StartResult::NoPending && !alreadyStarted && identityRead && stateAfter == 5;
    }

    // The lines the core created before the recording that were neither
    // started nor stopped by its first tick: the queue of a conversation
    // that began before F4. Fed in time order from the rings' contents at
    // the recording start; an entry leaves on its start (Take), on a stop
    // or on another creation at its address (Drop).
    struct EarlyLine
    {
        std::uint64_t line = 0;
        LineKey key;
        std::uint64_t createdUs = 0;
        std::uint32_t requestedContext = 0; // partial hash the request asked
        std::uint32_t path = 0;             // the core's return address (PathName)
    };

    class EarlyLines
    {
    public:
        static constexpr std::size_t kMax = 128;

        void Clear() { m_lines.clear(); }
        const std::vector<EarlyLine>& Lines() const { return m_lines; }

        // A creation at an address replaces what was held there (rule 1).
        void Created(const EarlyLine& early)
        {
            if (!early.line)
                return;
            Drop(early.line);
            if (m_lines.size() >= kMax)
                m_lines.erase(m_lines.begin());
            m_lines.push_back(early);
        }

        // A start before the recording (the adoption of lines already
        // playing takes it), a stop, or a creation during the recording.
        void Drop(std::uint64_t line)
        {
            for (std::size_t n = 0; n < m_lines.size();)
            {
                if (m_lines[n].line == line)
                    m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(n));
                else
                    ++n;
            }
        }

        // The start of `line` with identity `key`: the early line it plays,
        // removed. A held line with another identity at that address is not
        // it (the address was reused unseen): dropped, false.
        bool Take(std::uint64_t line, const LineKey& key, EarlyLine& out)
        {
            for (std::size_t n = 0; n < m_lines.size(); ++n)
            {
                if (m_lines[n].line != line)
                    continue;
                const bool same = SameLine(m_lines[n].key, key);
                if (same)
                    out = m_lines[n];
                m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(n));
                return same;
            }
            return false;
        }

    private:
        std::vector<EarlyLine> m_lines;
    };

    // A recorded line timed by its start this long after its request is
    // suspect (ghost lines showed 23 to 31 s, real lines 15 ms to 4 s;
    // scripted conversations may wait longer, so this only flags).
    constexpr std::uint32_t kSuspectStartGapMs = 5000;
    inline bool SuspectStartGap(std::uint32_t coreMs, std::uint32_t startMs, bool timedByStart)
    {
        return timedByStart && startMs > coreMs && startMs - coreMs > kSuspectStartGapMs;
    }
}
