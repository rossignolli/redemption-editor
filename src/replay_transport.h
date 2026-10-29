#pragma once

// Playback transport state (pure data, unit tested in ReplayCoreTests).
//
// One object owns the playhead: whether playback is on, paused, holding
// (entities still loading) or scrubbing, the timeline duration and the
// wall-clock bookkeeping between frames. Each frame the script feeds it the
// scrub keys and gets back the events it has to log or show (a scrub that
// started or ended, a boundary reached, a pause the transport decided on).
// The arithmetic is playback_time.h: +1x in normal play, cursor speed while a
// scrub key is held, paused at the end of a scrub or at the end of the
// timeline.

#include "playback_time.h"

#include <cstdint>

namespace replay::transport
{
    struct ScrubState
    {
        bool fine = false;
        int direction = 0; // -1 backward, +1 forward
        std::uint64_t fromMs = 0;
        std::uint64_t toMs = 0; // the playhead the scrub has reached
        std::uint64_t startedAt = 0;
        std::uint32_t frames = 0;
        bool atBoundary = false;
    };

    // Events of one Update, as bits.
    enum Event : std::uint32_t
    {
        kScrubStarted = 1u << 0, // Scrub() describes the new scrub
        kScrubEnded = 1u << 1,   // EndedScrub() describes it
        kReachedStart = 1u << 2, // a scrub hit the timeline start (once)
        kReachedEnd = 1u << 3,   // a scrub or normal play hit the end (once)
        kPaused = 1u << 4,       // the transport paused itself
    };

    struct Input
    {
        int scrubDirection = 0; // -1, 0, +1 from the held keys
        bool fineScrub = false;
        bool hold = false; // entities loading: time does not advance
    };

    class Transport
    {
    public:
        bool Playing() const { return m_playing; }
        bool Paused() const { return m_paused; }
        bool Scrubbing() const { return m_scrubbing; }
        std::uint64_t PlayheadMs() const { return m_playheadMs; }
        std::uint64_t DurationMs() const { return m_durationMs; }
        const ScrubState& Scrub() const { return m_scrub; }
        const ScrubState& EndedScrub() const { return m_endedScrub; }

        // The timeline length (the last recording's wall clock).
        void SetDurationMs(std::uint64_t durationMs) { m_durationMs = durationMs; }

        // Starts at 0, playing; false when there is no timeline.
        bool Start(std::uint64_t now);
        void Stop();
        // Moves the playhead without playing (clamped to the timeline).
        void Seek(std::uint64_t timeMs);
        // The earliest playhead a seek or scrub reaches: where the recording's
        // opening scene is complete (reset by Stop).
        void SetStartMs(std::uint64_t startMs) { m_startMs = startMs < m_durationMs ? startMs : 0; }
        std::uint64_t StartMs() const { return m_startMs; }
        // False when not playing.
        bool SetPaused(bool paused, std::uint64_t now);

        std::uint32_t Update(std::uint64_t now, const Input& input);
        // Playhead speed in normal play (1 = real time); fractions of a
        // millisecond carry over between frames.
        void SetRate(double rate) { m_rate = rate > 0.0 ? rate : 1.0; }
        double Rate() const { return m_rate; }

    private:
        std::uint32_t EndScrub(std::uint64_t now);

        bool m_playing = false;
        bool m_paused = false;
        bool m_scrubbing = false;
        std::uint64_t m_durationMs = 0;
        std::uint64_t m_startMs = 0;
        std::uint64_t m_playheadMs = 0;
        std::uint64_t m_lastUpdateAt = 0;
        double m_rate = 1.0;
        double m_fractionMs = 0.0;
        ScrubState m_scrub;
        ScrubState m_endedScrub;
    };
}
