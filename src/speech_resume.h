#pragma once

// Speech after a jump of the playhead. A speech line is asked at its own start,
// so a playhead that lands inside a line (a rewind, a seek, a resume) would
// skip it until the speaker's next line.
//
// As a replay editor usually does it: the line is said again with the time
// already played as its start offset, in normal forward play only, unless it
// is already playing; an offset under 66 ms becomes 0 (the line from its
// start). A jump stops what played before it.
//
// The clip keeps one event per line with its start and its natural end (SPCH
// v2 endMs; an early stop has stopMs). On every jump, rewind, scrub, frame
// step, pause and resume the replay stops its lines (no echo, no doubles) and,
// when normal play goes on, says again the line each speaker is inside of at
// the playhead, with the start offset = playhead - recorded start. The engine
// takes the offset in the speech params +0x15C (build 1491.50).
//
// Pure: unit tested in tests/speech_seek_tests.cpp.

#include "entity_events.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace replay::speech::resume
{
    // The offset floor: a smaller offset plays the line from its start.
    constexpr std::uint32_t kMinOffsetMs = 66;
    // Ours: a line with less left than this is not said again (a blip).
    constexpr std::uint32_t kMinRemainingMs = 250;
    // A clip from before SPCH v2 has no natural end: the line is taken to
    // last until the speaker's next line, at most this long, unless the
    // session heard it end (LearnedEnd).
    constexpr std::uint32_t kUnknownEndCapMs = 12000;
    // The playhead moved more than the clock allows by this much: a jump.
    constexpr std::uint32_t kJumpSlackMs = 250;

    // Where a recorded line ends on the clip's clock, and whether that end
    // is known (recorded or heard) or estimated.
    struct LineEnd
    {
        std::uint32_t endMs = 0;
        bool known = false;
        const char* source = "none"; // "stop", "recorded", "learned", "next_line", "cap"
    };

    // `learnedEndMs`: the end the session heard (0 when none);
    // `nextSameSpeakerMs`: the speaker's next line start (0 when none).
    inline LineEnd EndOf(const replay::entity::SpeechEvent& line, std::uint32_t learnedEndMs,
        std::uint32_t nextSameSpeakerMs)
    {
        LineEnd end;
        if (line.stopMs > line.timeMs)
        {
            end.endMs = line.stopMs;
            end.known = true;
            end.source = "stop";
            if (line.endMs > line.timeMs && line.endMs < end.endMs)
                end.endMs = line.endMs;
            return end;
        }
        if (line.endMs > line.timeMs)
        {
            end.endMs = line.endMs;
            end.known = true;
            end.source = "recorded";
            return end;
        }
        if (learnedEndMs > line.timeMs)
        {
            end.endMs = learnedEndMs;
            end.known = true;
            end.source = "learned";
            return end;
        }
        // An adopted line had already played startedBeforeMs of the cap.
        const std::uint32_t capLeft = line.startedBeforeMs < kUnknownEndCapMs - kMinRemainingMs
            ? kUnknownEndCapMs - line.startedBeforeMs : kMinRemainingMs;
        const std::uint32_t cap = line.timeMs + capLeft;
        if (nextSameSpeakerMs > line.timeMs && nextSameSpeakerMs < cap)
        {
            end.endMs = nextSameSpeakerMs;
            end.source = "next_line";
            return end;
        }
        end.endMs = cap;
        end.source = "cap";
        return end;
    }

    // The start offset of a line asked at `playheadMs`: what it had played before
    // its event (startedBeforeMs, a line adopted already playing) plus the
    // playhead past its event; 0 under the floor.
    inline std::uint32_t OffsetAt(const replay::entity::SpeechEvent& line, std::uint32_t playheadMs)
    {
        const std::uint32_t offset = line.startedBeforeMs + (playheadMs > line.timeMs ? playheadMs - line.timeMs : 0);
        return offset < kMinOffsetMs ? 0 : offset;
    }

    // A line that began before a saved window's base and is still audible at it
    // (its known end, a recorded stop or natural end, more than kMinRemainingMs
    // past the base) is kept, adopted at the base: `out` has timeMs = 0 on the
    // window's clock and the time it had played added to startedBeforeMs; its
    // other times shifted. False when it ended before the base (or nearly), or
    // when its end is not known (a missed stop must not bring back an old line).
    inline bool AdoptAtBase(const replay::entity::SpeechEvent& line, std::uint32_t baseMs,
        std::uint32_t nextSameSpeakerMs, replay::entity::SpeechEvent& out)
    {
        if (line.timeMs >= baseMs)
            return false;
        const LineEnd end = EndOf(line, 0, nextSameSpeakerMs);
        if (!end.known || end.endMs <= baseMs || end.endMs - baseMs < kMinRemainingMs)
            return false;
        out = line;
        out.startedBeforeMs = line.startedBeforeMs + (baseMs - line.timeMs);
        out.timeMs = 0;
        out.coreMs = 0;
        out.stopMs = line.stopMs > baseMs ? line.stopMs - baseMs : 0;
        out.endMs = line.endMs > baseMs ? line.endMs - baseMs : 0;
        if (out.stopMs == 0 && line.stopMs)
            out.stopReason = 0;
        return true;
    }

    // A line to say again at the playhead.
    struct Active
    {
        std::size_t index = 0;    // into the clip's speech events
        std::uint32_t offsetMs = 0; // start offset to ask (0: from the start)
        std::uint32_t remainingMs = 0;
        LineEnd end;
    };

    // The lines each speaker is inside of at `playheadMs`: requested
    // (requestMs = start minus the load lead, `leadMs` per event, may be
    // empty for no lead) at or before the playhead and with at least
    // kMinRemainingMs left. One per speaker: when two lines of the same
    // speaker overlap at the playhead the later start wins (the engine runs
    // one line per speaker; the later one replaced the earlier).
    // `learnedEndMs` is indexed like `events` (may be shorter or empty).
    inline void ActiveAt(const std::vector<replay::entity::SpeechEvent>& events, std::uint32_t playheadMs,
        const std::vector<std::uint32_t>& leadMs, const std::vector<std::uint32_t>& learnedEndMs,
        std::vector<Active>& out)
    {
        out.clear();
        // The speaker's next line start, walking backwards.
        std::unordered_map<std::uint32_t, std::uint32_t> nextStart;
        std::unordered_map<std::uint32_t, std::size_t> chosen; // speaker -> position in out
        std::vector<std::uint32_t> nextOf(events.size(), 0);
        for (std::size_t n = events.size(); n-- > 0;)
        {
            const std::uint32_t speaker = events[n].speaker.Packed();
            const auto it = nextStart.find(speaker);
            nextOf[n] = it != nextStart.end() ? it->second : 0;
            nextStart[speaker] = events[n].timeMs;
        }
        for (std::size_t n = 0; n < events.size(); ++n)
        {
            const replay::entity::SpeechEvent& line = events[n];
            const std::uint32_t lead = n < leadMs.size() ? leadMs[n] : 0;
            const std::uint32_t requestMs = line.timeMs > lead ? line.timeMs - lead : 0;
            if (requestMs > playheadMs)
                continue; // leads differ per line: no early break
            const LineEnd end = EndOf(line, n < learnedEndMs.size() ? learnedEndMs[n] : 0, nextOf[n]);
            if (end.endMs <= playheadMs || end.endMs - playheadMs < kMinRemainingMs)
                continue;
            Active active;
            active.index = n;
            active.offsetMs = OffsetAt(line, playheadMs); // with startedBeforeMs
            active.remainingMs = end.endMs - playheadMs;
            active.end = end;
            const std::uint32_t speaker = line.speaker.Packed();
            const auto it = chosen.find(speaker);
            if (it == chosen.end())
            {
                chosen[speaker] = out.size();
                out.push_back(active);
            }
            else if (events[out[it->second].index].timeMs <= line.timeMs)
            {
                out[it->second] = active; // the later start of the same speaker
            }
        }
    }

    // What the speech track does this tick with the lines it replays.
    enum class Step : std::uint8_t
    {
        Continue = 0, // normal play goes on: nothing
        Idle = 1,     // paused, scrubbing or preparing, lines already stopped
        Leave = 2,    // normal play just stopped (pause, scrub, prepare): stop the lines
        Resume = 3,   // normal play (re)starts or the playhead jumped: stop, then say the lines at the playhead
    };

    inline const char* StepName(Step step)
    {
        switch (step)
        {
        case Step::Continue: return "continue";
        case Step::Idle: return "idle";
        case Step::Leave: return "leave";
        case Step::Resume: return "resume";
        default: return "?";
        }
    }

    // Why a Resume came.
    enum class Why : std::uint8_t
    {
        None = 0,
        First = 1,   // the first tick of the playback
        Play = 2,    // normal play again after a pause, scrub or prepare
        Back = 3,    // the playhead went back in normal play
        Ahead = 4,   // the playhead went ahead faster than the clock
    };

    inline const char* WhyName(Why why)
    {
        switch (why)
        {
        case Why::First: return "first";
        case Why::Play: return "play";
        case Why::Back: return "back";
        case Why::Ahead: return "ahead";
        default: return "none";
        }
    }

    // The playhead against the audio clock, one call per tick. A playhead
    // that lags the clock (a stalled frame: the transport clamps its step)
    // is not a jump: the lines keep playing.
    class Watch
    {
    public:
        void Reset() { *this = Watch{}; }

        Step Update(bool normalPlay, std::uint32_t playheadMs, std::uint64_t nowUs, double rate)
        {
            const bool first = !m_seen;
            const bool wasNormal = m_wasNormal;
            const std::uint32_t lastMs = m_lastMs;
            const std::uint64_t lastUs = m_lastUs;
            m_seen = true;
            m_wasNormal = normalPlay;
            m_lastMs = playheadMs;
            m_lastUs = nowUs;
            m_why = Why::None;
            if (!normalPlay)
                return wasNormal && !first ? Step::Leave : Step::Idle;
            if (first)
            {
                m_why = Why::First;
                return Step::Resume;
            }
            if (!wasNormal)
            {
                m_why = Why::Play;
                return Step::Resume;
            }
            if (playheadMs < lastMs)
            {
                m_why = Why::Back;
                return Step::Resume;
            }
            const double r = rate < 1.0 ? 1.0 : rate;
            const double clockMs = nowUs > lastUs ? static_cast<double>(nowUs - lastUs) / 1000.0 : 0.0;
            if (static_cast<double>(playheadMs - lastMs) > clockMs * r + kJumpSlackMs)
            {
                m_why = Why::Ahead;
                return Step::Resume;
            }
            return Step::Continue;
        }

        Why LastWhy() const { return m_why; }

    private:
        bool m_seen = false;
        bool m_wasNormal = false;
        std::uint32_t m_lastMs = 0;
        std::uint64_t m_lastUs = 0;
        Why m_why = Why::None;
    };

    // ---- why a replayed line was silent or cut ----

    // A created line that never became audible (POSE_V2_SPEECH_SILENT): the cause
    // named from the stop the engine logged (audio_observer.h SpeechLineStop):
    //   e1_wave_missing: killed in state 2 (reason 4) with a wave slot whose
    //     loaded bank lacks the wave (sound +0xEE bit 2);
    //   e2_no_slot: killed in state 2 (reason 4) without a wave slot (+0xD0
    //     still 0), data type not 3;
    //   revalidation: refused in state 3 (reason 6, the core's rules again:
    //     another active line of the speaker, codes 18 and 19);
    //   timeout: still listed, never audible in the window;
    //   killed_other: another stop reason; no_stop: no stop seen.
    struct StopFacts
    {
        bool found = false;
        std::uint32_t reason = 0;
        bool fieldsRead = false;
        bool slot = false;
        std::uint8_t soundFlagsEE = 0;
        std::uint32_t dataType = 0;
    };

    inline const char* SilentCause(bool timedOut, const StopFacts& stop)
    {
        if (timedOut)
            return "timeout";
        if (!stop.found)
            return "no_stop";
        if (stop.reason == 4 && stop.fieldsRead && stop.slot && (stop.soundFlagsEE & 4) != 0)
            return "e1_wave_missing";
        if (stop.reason == 4 && stop.fieldsRead && !stop.slot && stop.dataType != 3)
            return "e2_no_slot";
        if (stop.reason == 4)
            return "killed_loading";
        if (stop.reason == 6)
            return "revalidation";
        return "killed_other";
    }

    // A line that became audible and then left before its end: cut when the
    // stop is not the natural end (reason 1) and the replay did not ask it
    // (its own stop at the recorded stop, a jump, the same-context wait).
    // `heardMs` is how long it was audible; `expectedMs` what was left of
    // the recorded line when it started (0 when unknown).
    inline bool Cut(bool stopFound, std::uint32_t reason, bool stoppedByReplay)
    {
        return stopFound && reason != 1 && !stoppedByReplay;
    }
    // A natural end (reason 1) well before the recorded end is also short:
    // the engine ended the wave early (another wave, a voice change).
    constexpr std::uint32_t kShortEndSlackMs = 500;
    inline bool EndedShort(std::uint32_t heardMs, std::uint32_t expectedMs)
    {
        return expectedMs > 0 && heardMs + kShortEndSlackMs < expectedMs;
    }

    // ---- the copy still stopping ------------------------------------------------
    //
    // A resume asked right after the jump stop of the same line can die in its own
    // request (state 1 to 6 inside the call: e1_wave_missing or killed_loading). A
    // stopped line (0x2FBCCD4: state 6, its data's sounds released through data
    // vtable +0x28) stays alive until the speech update frees it: the line dtor
    // 0x2F93888 puts the line vtable (0x37FD5D0) back and the pool free 0x2B079E0
    // writes 0xFFFFFFFF over the element's first dword (build 1491.50). So a
    // resume of the same line (same speaker, voice, context, variation) waits
    // while the old copy still reads as a live line in state 6 with that
    // identity, at most kOldCopyWaitMs (the silent-line detector's window), and is
    // asked then with the start offset of that moment.

    enum class OldCopy : std::uint8_t
    {
        Gone = 0,     // freed (the vtable is not the line's any more)
        Stopping = 1, // live, state 6, the same line
        Active = 2,   // live, state 1 to 5 (the stop did not take)
        Other = 3,    // live, another line in the same pool element
        Unread = 4,   // the read failed
    };

    inline const char* OldCopyName(OldCopy copy)
    {
        switch (copy)
        {
        case OldCopy::Gone: return "gone";
        case OldCopy::Stopping: return "stopping";
        case OldCopy::Active: return "active";
        case OldCopy::Other: return "other";
        case OldCopy::Unread: return "unread";
        default: return "?";
        }
    }

    inline OldCopy ClassifyOldCopy(bool read, bool liveVtable, std::int32_t state, bool sameIdentity)
    {
        if (!read)
            return OldCopy::Unread;
        if (!liveVtable)
            return OldCopy::Gone;
        if (!sameIdentity)
            return OldCopy::Other;
        if (state == 6)
            return OldCopy::Stopping;
        if (state >= 1 && state <= 5)
            return OldCopy::Active;
        return OldCopy::Gone;
    }

    constexpr std::uint32_t kOldCopyWaitMs = 1500;

    enum class OldCopyStep : std::uint8_t
    {
        Ask = 0,          // nothing to wait for
        Wait = 1,         // the old copy is still stopping: ask on a later tick
        AskTimedOut = 2,  // waited the whole window: ask anyway
    };

    inline OldCopyStep StepForOldCopy(bool enabled, OldCopy copy, std::uint64_t waitedMs,
        std::uint32_t windowMs = kOldCopyWaitMs)
    {
        if (!enabled || copy != OldCopy::Stopping)
            return OldCopyStep::Ask;
        return waitedMs < windowMs ? OldCopyStep::Wait : OldCopyStep::AskTimedOut;
    }

    // A line the jump stop stopped (kept until a later jump stop, the window
    // past its stop or the playback's end).
    struct StoppedCopy
    {
        std::uint32_t speaker = 0; // ReplayId::Packed
        std::uint64_t line = 0;
        std::uint32_t voice = 0;
        std::uint32_t context = 0;
        std::int32_t variation = 0;
        std::uint64_t stopUs = 0;
        // The copy's own identity read just before the stop (data +0x110,
        // +0x114, +0x118): the voice can differ from the recorded one (the
        // retry with the puppet's own voice). Compared instead of the
        // recorded identity when read.
        bool ownRead = false;
        std::uint32_t ownVoice = 0;
        std::uint32_t ownContext = 0;
        std::int32_t ownVariation = 0;
    };

    // Whether the line read at the copy's address is still that copy.
    inline bool SameCopyIdentity(const StoppedCopy& copy, std::uint32_t voice, std::uint32_t context,
        std::int32_t variation)
    {
        if (copy.ownRead)
            return voice == copy.ownVoice && context == copy.ownContext && variation == copy.ownVariation;
        return voice == copy.voice && context == copy.context && variation == copy.variation;
    }

    // The latest stopped copy of this line, null when none.
    inline const StoppedCopy* FindStoppedCopy(const std::vector<StoppedCopy>& copies, std::uint32_t speaker,
        std::uint32_t voice, std::uint32_t context, std::int32_t variation)
    {
        const StoppedCopy* found = nullptr;
        for (const StoppedCopy& copy : copies)
            if (copy.speaker == speaker && copy.voice == voice && copy.context == context && copy.variation == variation &&
                (!found || copy.stopUs >= found->stopUs))
                found = &copy;
        return found;
    }

    // Drops the copies stopped more than `windowMs` ago (they do not hold a resume
    // back any more).
    inline void PruneStoppedCopies(std::vector<StoppedCopy>& copies, std::uint64_t nowUs,
        std::uint32_t windowMs = kOldCopyWaitMs)
    {
        const std::uint64_t windowUs = static_cast<std::uint64_t>(windowMs) * 1000;
        std::size_t kept = 0;
        for (std::size_t n = 0; n < copies.size(); ++n)
            if (nowUs < copies[n].stopUs || nowUs - copies[n].stopUs <= windowUs)
                copies[kept++] = copies[n];
        copies.resize(kept);
    }
}
