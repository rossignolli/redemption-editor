#include "replay_transport.h"

#include <algorithm>

namespace replay::transport
{
    bool Transport::Start(std::uint64_t now)
    {
        if (m_durationMs == 0)
            return false;
        Stop();
        m_playing = true;
        m_lastUpdateAt = now;
        return true;
    }

    void Transport::Stop()
    {
        m_playing = false;
        m_paused = false;
        m_scrubbing = false;
        m_playheadMs = 0;
        m_startMs = 0;
        m_lastUpdateAt = 0;
        m_scrub = {};
        m_endedScrub = {};
        m_fractionMs = 0.0;
    }

    void Transport::Seek(std::uint64_t timeMs)
    {
        m_playheadMs = std::max(std::min(timeMs, m_durationMs), m_startMs);
    }

    bool Transport::SetPaused(bool paused, std::uint64_t now)
    {
        if (!m_playing)
            return false;
        m_paused = paused;
        m_lastUpdateAt = now;
        return true;
    }

    // A scrub ends paused at the playhead it reached.
    std::uint32_t Transport::EndScrub(std::uint64_t now)
    {
        (void)now;
        m_endedScrub = m_scrub;
        m_endedScrub.toMs = m_playheadMs;
        m_scrub = {};
        m_scrubbing = false;
        m_paused = true;
        return kScrubEnded | kPaused;
    }

    std::uint32_t Transport::Update(std::uint64_t now, const Input& input)
    {
        if (!m_playing)
            return 0;
        if (input.hold)
        {
            m_lastUpdateAt = now;
            return 0;
        }
        if (m_lastUpdateAt == 0)
            m_lastUpdateAt = now;
        const std::uint64_t deltaMs = now - m_lastUpdateAt;
        m_lastUpdateAt = now;

        std::uint32_t events = 0;
        if (input.scrubDirection != 0)
        {
            const int direction = input.scrubDirection < 0 ? -1 : 1;
            if (m_scrubbing &&
                (direction != m_scrub.direction || input.fineScrub != m_scrub.fine))
            {
                events |= EndScrub(now);
            }
            if (!m_scrubbing)
            {
                m_scrubbing = true;
                m_scrub = {};
                m_scrub.fine = input.fineScrub;
                m_scrub.direction = direction;
                m_scrub.fromMs = m_playheadMs;
                m_scrub.toMs = m_playheadMs;
                m_scrub.startedAt = now;
                events |= kScrubStarted;
            }
            const std::int64_t stepMs = m_scrub.fine
                ? FineScrubDeltaMs(direction)
                : ScrubDeltaMs(deltaMs, static_cast<float>(direction) * kScrubSpeed);
            MoveResult moved = Move(m_playheadMs, stepMs, m_durationMs);
            if (moved.timeMs <= m_startMs && direction < 0)
            {
                moved.timeMs = m_startMs;
                moved.reachedStart = true;
            }
            ++m_scrub.frames;
            m_playheadMs = moved.timeMs;
            m_scrub.toMs = moved.timeMs;
            if ((moved.reachedStart || moved.reachedEnd) && !m_scrub.atBoundary)
            {
                m_scrub.atBoundary = true;
                events |= moved.reachedStart ? kReachedStart : kReachedEnd;
            }
            return events;
        }
        if (m_scrubbing)
            return EndScrub(now);
        if (m_paused)
            return 0;

        const double scaled = static_cast<double>(deltaMs) * m_rate + m_fractionMs;
        const std::uint64_t wholeMs = static_cast<std::uint64_t>(scaled);
        // A stalled frame moves the playhead by kMaxNormalStepMs at most
        // (playback_time.h); the rest of the stall is dropped, not carried, so the
        // next frames play at their normal step.
        const std::uint64_t stepMs = NormalStepMs(wholeMs);
        m_fractionMs = stepMs == wholeMs ? scaled - static_cast<double>(stepMs) : 0.0;
        const AdvanceResult advance = Advance(m_playheadMs, stepMs, m_durationMs);
        m_playheadMs = advance.timeMs;
        if (advance.reachedBoundary)
        {
            m_paused = true;
            events |= kPaused | kReachedEnd;
        }
        return events;
    }
}
