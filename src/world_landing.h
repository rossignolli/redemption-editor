#pragma once

// World entities landing after a replay. While a clip plays, the world's
// entities the replay could not delete stay hidden, frozen and without
// collision where they were, and the streaming focus follows the replay camera,
// so the map collision around them may go. Each one is shown at once when the
// playback stops but stays frozen without collision until the map collision at
// its position is loaded (or a time limit passes, logged), then gets its
// collision back on one tick and its freeze state on the next (no tick where
// gravity acts before the collision shape is in). A rider waits for its mount
// when the mount is in the queue too. The player's own wait (PlayerLanding in
// script.cpp) uses the same verdict (Decide).
//
// Pure (no engine call): unit tested in ReplayCoreTests.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace replay::landing
{
    // What a re-hide of a hidden world entity has to put back (bits as
    // pose_playback.h kRehide*: 1 visible, 2 frozen, 4 collision) may be
    // changed by its owner while the replay holds it (a mission script showing
    // enemies it spawned hidden for an ambush, a fade-in, unfreezing them):
    // that change is the owner's latest wish and becomes the state given back
    // at the end, else enemies shown by the mission during the replay would
    // come back invisible. Returns the bits that changed the state to give
    // back. Pure, tested.
    inline std::uint32_t NoteOwnerChange(bool& visible, bool& frozen, bool& collisionDisabled, std::uint32_t fixed)
    {
        std::uint32_t changed = 0;
        if ((fixed & 1u) && !visible)
        {
            visible = true;
            changed |= 1u;
        }
        if ((fixed & 2u) && frozen)
        {
            frozen = false;
            changed |= 2u;
        }
        if ((fixed & 4u) && collisionDisabled)
        {
            collisionDisabled = false;
            changed |= 4u;
        }
        return changed;
    }

    // The player's wait, kept for every entity.
    constexpr unsigned long long kLandingTimeoutMs = 10000;
    // Collision requests per tick (each one a streaming request; a camp
    // gives back dozens of entities at once).
    constexpr std::uint32_t kRequestsPerTick = 8;

    enum class Verdict : std::uint8_t
    {
        Wait,
        Land,
        TimedOut,
    };

    inline Verdict Decide(bool collisionLoaded, bool waitsOnOther, unsigned long long nowMs, unsigned long long deadlineMs)
    {
        if (nowMs >= deadlineMs)
            return Verdict::TimedOut;
        return collisionLoaded && !waitsOnOther ? Verdict::Land : Verdict::Wait;
    }

    enum class Action : std::uint8_t
    {
        CollisionOn, // collision back (the entity still frozen)
        Release,     // the freeze state and the rest back: done
    };

    struct Stats
    {
        std::uint32_t queued = 0;
        std::uint32_t waited = 0;   // not loaded at the first check
        std::uint32_t timedOut = 0;
        std::uint32_t gone = 0;     // deleted while waiting
        std::uint32_t landed = 0;   // released (loaded or timed out)
        unsigned long long maxWaitMs = 0;
        unsigned long long totalWaitMs = 0; // of the ones that waited
    };

    template <typename Payload>
    class Queue
    {
    public:
        struct Entry
        {
            std::int32_t handle = 0;
            std::int32_t after = 0; // lands only once this one is not waiting (a rider's mount)
            unsigned long long since = 0;
            unsigned long long deadline = 0;
            bool collisionOn = false;
            bool checked = false;   // first collision check done
            bool waited = false;
            bool timedOut = false;
            Payload payload{};
        };

        bool Empty() const { return m_entries.empty(); }
        // A ScriptHook restart (session reset): the handles are the old world's.
        void Clear() { m_entries.clear(); }
        std::size_t Size() const { return m_entries.size(); }
        const Stats& Totals() const { return m_stats; }
        void ResetStats() { m_stats = {}; }

        void Add(std::int32_t handle, std::int32_t after, unsigned long long nowMs, const Payload& payload,
            unsigned long long timeoutMs = kLandingTimeoutMs)
        {
            Entry entry;
            entry.handle = handle;
            entry.after = after == handle ? 0 : after;
            entry.since = nowMs;
            entry.deadline = nowMs + timeoutMs;
            entry.payload = payload;
            m_entries.push_back(entry);
            ++m_stats.queued;
        }

        // Still frozen without collision, waiting for the map.
        bool Waiting(std::int32_t handle) const
        {
            return handle && std::any_of(m_entries.begin(), m_entries.end(),
                                 [&](const Entry& e) { return e.handle == handle && !e.collisionOn; });
        }

        // Up to `count` waiting entries, rotating, for the collision requests.
        template <typename Fn>
        void ForRequests(std::uint32_t count, Fn fn)
        {
            if (m_entries.empty())
                return;
            const std::size_t size = m_entries.size();
            std::uint32_t done = 0;
            for (std::size_t step = 0; step < size && done < count; ++step)
            {
                Entry& entry = m_entries[(m_cursor + step) % size];
                if (entry.collisionOn)
                    continue;
                fn(entry);
                ++done;
            }
            m_cursor = (m_cursor + count) % size;
        }

        // One tick. exists(handle): the entity is still there; loaded(entry):
        // the map collision around it is in; act(entry, action, waitedMs).
        template <typename Exists, typename Loaded, typename Act>
        void Tick(unsigned long long nowMs, Exists exists, Loaded loaded, Act act)
        {
            std::vector<Entry> keep;
            keep.reserve(m_entries.size());
            for (Entry& entry : m_entries)
            {
                if (!exists(entry.handle))
                {
                    ++m_stats.gone;
                    continue;
                }
                if (entry.collisionOn)
                {
                    const unsigned long long waitedMs = nowMs > entry.since ? nowMs - entry.since : 0;
                    act(entry, Action::Release, waitedMs);
                    ++m_stats.landed;
                    if (entry.waited)
                    {
                        m_stats.totalWaitMs += waitedMs;
                        m_stats.maxWaitMs = (std::max)(m_stats.maxWaitMs, waitedMs);
                    }
                    continue;
                }
                const bool waitsOnOther = entry.after && Waiting(entry.after);
                const Verdict verdict = Decide(loaded(entry), waitsOnOther, nowMs, entry.deadline);
                if (!entry.checked)
                {
                    entry.checked = true;
                    entry.waited = verdict != Verdict::Land;
                    if (entry.waited)
                        ++m_stats.waited;
                }
                if (verdict == Verdict::Wait)
                {
                    keep.push_back(entry);
                    continue;
                }
                if (verdict == Verdict::TimedOut)
                {
                    entry.timedOut = true;
                    ++m_stats.timedOut;
                }
                entry.collisionOn = true;
                act(entry, Action::CollisionOn, nowMs > entry.since ? nowMs - entry.since : 0);
                keep.push_back(entry);
            }
            m_entries.swap(keep);
            if (m_cursor >= m_entries.size())
                m_cursor = 0;
        }

        // A new playback takes the entities back before they landed: the
        // payloads, the queue empties.
        std::vector<Payload> TakeAll()
        {
            std::vector<Payload> out;
            out.reserve(m_entries.size());
            for (const Entry& entry : m_entries)
                out.push_back(entry.payload);
            m_entries.clear();
            m_cursor = 0;
            return out;
        }

    private:
        std::vector<Entry> m_entries;
        std::size_t m_cursor = 0;
        Stats m_stats;
    };
}
