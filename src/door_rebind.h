#pragma once

// The map doors of the replay after the world clear.
//
// The world clear (CLEAR_AREA bit 0x200, core 0x7F9264) turns every map object
// back into a dummy, doors included; the engine's object population makes them
// again during the replay with a new handle. A door resolved once at the
// playback's start would, when made again, appear in the middle of the action
// in the map's own state. Like the recording (deferred map objects), the
// playback looks for each door by its door hash again every rebindMs and plays
// the track on the new handle in the same tick; and, as a replay's preparation
// before the playhead runs, the start waits (bounded) for the map objects to
// come back and the doors near the first camera to be found.
//
// Pure: unit tested in tests/lod_doors_camera_tests.cpp.

#include <cstddef>
#include <cstdint>

namespace replay::doors
{
    struct Config
    {
        std::uint32_t rebindMs = 250;   // DoorRebindMs (0: resolved once)
        bool keepOpenRatio = true;      // DoorKeepOpenRatio: closed theater only
        std::uint32_t mapWaitMs = 3000; // MapObjectsWaitMs (0: no wait)
    };

    // Due to look for lost or missing doors again.
    inline bool RebindDue(std::uint64_t nowMs, std::uint64_t& nextAt, std::uint32_t intervalMs)
    {
        if (intervalMs == 0 || nowMs < nextAt)
            return false;
        nextAt = nowMs + intervalMs;
        return true;
    }

    // The start's wait for the map objects: done once the object count has
    // held (within kTolerance) for kStableMs after kMinMs and no door near
    // the first camera is still missing, or after the cap.
    class MapSettle
    {
    public:
        static constexpr std::uint32_t kMinMs = 500;
        static constexpr std::uint32_t kStableMs = 750;
        static constexpr std::uint32_t kTolerance = 4;

        void Begin(std::uint64_t nowMs, std::uint32_t capMs)
        {
            *this = MapSettle{};
            m_active = capMs > 0;
            m_start = nowMs;
            m_capMs = capMs;
        }
        bool Active() const { return m_active; }

        struct Verdict
        {
            bool done = false;
            bool timedOut = false;
        };

        Verdict Feed(std::uint64_t nowMs, std::uint32_t objects, std::uint32_t doorsMissing)
        {
            Verdict verdict;
            if (!m_active)
            {
                verdict.done = true;
                return verdict;
            }
            if (!m_has || objects > m_base + kTolerance || objects + kTolerance < m_base)
            {
                m_has = true;
                m_base = objects;
                m_changedAt = nowMs;
            }
            m_last = objects;
            m_doorsMissing = doorsMissing;
            const std::uint64_t elapsed = nowMs - m_start;
            if (elapsed >= kMinMs && nowMs - m_changedAt >= kStableMs && doorsMissing == 0)
                verdict.done = true;
            else if (elapsed >= m_capMs)
            {
                verdict.done = true;
                verdict.timedOut = true;
            }
            if (verdict.done)
            {
                m_active = false;
                m_endMs = elapsed;
                m_timedOut = verdict.timedOut;
            }
            return verdict;
        }

        std::uint32_t LastObjects() const { return m_last; }
        std::uint32_t DoorsMissing() const { return m_doorsMissing; }
        std::uint64_t EndMs() const { return m_endMs; }
        bool TimedOut() const { return m_timedOut; }

    private:
        bool m_active = false;
        bool m_has = false;
        std::uint64_t m_start = 0;
        std::uint32_t m_capMs = 0;
        std::uint32_t m_base = 0;
        std::uint64_t m_changedAt = 0;
        std::uint32_t m_last = 0;
        std::uint32_t m_doorsMissing = 0;
        std::uint64_t m_endMs = 0;
        bool m_timedOut = false;
    };

    // A door track with one sample is the door's state for the whole clip (the
    // state of every door is recorded, not only of the moving ones): it is held,
    // not dropped as "never moved", so a barn door recorded open plays open. Like
    // every track, its first sample holds before its time.
    enum class TrackPlay : std::uint8_t
    {
        Skip,   // no sample
        Held,   // one sample: held for the whole clip
        Moving, // two or more: played along the playhead
    };

    inline TrackPlay ClassifyTrack(std::size_t samples)
    {
        return samples == 0 ? TrackPlay::Skip : samples == 1 ? TrackPlay::Held : TrackPlay::Moving;
    }

    // A held door is placed when it is bound (or bound again) and then again
    // every refreshMs (it is frozen; the refresh covers the door system
    // moving it), not on every tick: a clip watches a hundred doors or more.
    constexpr std::uint32_t kHeldRefreshMs = 1000;
    inline bool HeldApplyDue(bool appliedSinceBind, std::uint64_t nowMs, std::uint64_t lastApplyMs, std::uint32_t refreshMs)
    {
        return !appliedSinceBind || nowMs - lastApplyMs >= refreshMs;
    }

    struct RebindTotals
    {
        std::uint32_t tracks = 0;      // door tracks played (moving + held)
        std::uint32_t moving = 0;      // tracks of two or more samples
        std::uint32_t heldSingle = 0;  // one-sample tracks held for the whole clip
        std::uint32_t heldApplies = 0; // placements of held doors (bind + refresh)
        std::uint32_t atStart = 0;     // found at the playback start
        std::uint32_t late = 0;        // found later (missing at the start)
        std::uint32_t rebound = 0;     // a new handle after the old one went
        std::uint32_t lost = 0;        // handles that stopped existing
        std::uint32_t scans = 0;       // rebind passes
        std::uint32_t ratioWrites = 0; // DOOR_SYSTEM_SET_OPEN_RATIO calls at a loss
    };
}
