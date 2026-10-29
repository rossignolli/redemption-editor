#pragma once

// The missing puppet detector.
//
// In normal play, every check (Config::checkMs) the playback lists the recorded
// bodies (ped, horse, vehicle) alive at the playhead, within radiusM of the
// camera, whose puppet is not shown, with why (the puppet's build state).
// PuppetWatch turns those observations into episodes: an entity missing for
// thresholdMs of wall time opens one (PUPPET_MISSING), and it closes when the
// entity shows (PUPPET_MISSING_END, with how long). Pause, scrub, a seek
// settle or a jump suspend the watch: a hold there is by design. The totals
// go in PUPPET_WATCH_SUMMARY at the end of the playback, which the bench
// regression report reads.
//
// Pure: no engine access; unit tested in ReplayCoreTests.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace replay::watch
{
    enum class Reason : std::uint8_t
    {
        NoPuppet = 0,  // alive in the clip, no puppet (or its entity is gone)
        ModelLoading,  // Loading, model not in memory
        CreateWait,    // Loading, model in memory: the create budget or a retry
        WaitSegment,   // Loading until a later pose segment starts
        Dressing,      // created, appearance still being built
        Failed,        // gave up (model timeout, no track, create failed)
        Unposed,       // Ready, hidden until the pose override is armed
        NoPose,        // Ready, hidden: no pose sample at the playhead
        Invisible,     // Ready and set visible, the engine says invisible
        ArmRefused,    // Ready, hidden: the pose override cannot be armed (no record
                       // and no skeleton map for its bones), which would show a T-pose
        Count,
    };

    inline const char* ReasonText(Reason reason)
    {
        switch (reason)
        {
        case Reason::NoPuppet:
            return "no_puppet";
        case Reason::ModelLoading:
            return "model_loading";
        case Reason::CreateWait:
            return "create_wait";
        case Reason::WaitSegment:
            return "wait_segment";
        case Reason::Dressing:
            return "dressing";
        case Reason::Failed:
            return "failed";
        case Reason::Unposed:
            return "unposed";
        case Reason::NoPose:
            return "no_pose";
        case Reason::Invisible:
            return "invisible";
        case Reason::ArmRefused:
            return "arm_refused";
        default:
            return "?";
        }
    }

    struct Config
    {
        float radiusM = 60.0f;
        std::uint32_t thresholdMs = 500;
        std::uint32_t checkMs = 100;
    };

    struct Observation
    {
        std::uint32_t id = 0; // packed ReplayId
        Reason reason = Reason::NoPuppet;
        float distanceM = 0.0f;
    };

    struct Event
    {
        bool begin = true;
        std::uint32_t id = 0;
        Reason reason = Reason::NoPuppet; // at the start (begin) or the last one seen (end)
        float distanceM = 0.0f;           // nearest seen
        std::uint64_t missingMs = 0;      // wall time missing so far (begin) or in all (end)
        bool suspended = false;           // end: closed by a pause, a scrub or a jump, not by showing
    };

    struct Summary
    {
        std::uint64_t checks = 0;
        std::uint32_t episodes = 0;
        std::uint32_t entities = 0; // distinct ids with at least one episode
        std::uint64_t worstMs = 0;
        std::uint64_t totalMs = 0;
        std::uint32_t perReason[static_cast<int>(Reason::Count)] = {};
    };

    class PuppetWatch
    {
    public:
        explicit PuppetWatch(const Config& config = {}) : m_config(config) {}

        const Config& Settings() const { return m_config; }

        void Reset()
        {
            m_open.clear();
            m_seen.clear();
            m_summary = {};
        }

        // `missing`: this check's due bodies near the camera that are not
        // shown. Episodes crossing the threshold begin; the ones whose
        // entity is no longer missing end.
        void Update(std::uint64_t now, const std::vector<Observation>& missing, std::vector<Event>& events)
        {
            ++m_summary.checks;
            for (std::size_t index = m_open.size(); index-- > 0;)
            {
                Entry& entry = m_open[index];
                const bool still = std::any_of(missing.begin(), missing.end(),
                    [&](const Observation& o) { return o.id == entry.id; });
                if (still)
                    continue;
                Close(entry, now, false, events);
                m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(index));
            }
            for (const Observation& observation : missing)
            {
                auto found = std::find_if(m_open.begin(), m_open.end(),
                    [&](const Entry& entry) { return entry.id == observation.id; });
                if (found == m_open.end())
                {
                    Entry entry;
                    entry.id = observation.id;
                    entry.since = now;
                    entry.firstReason = observation.reason;
                    entry.nearest = observation.distanceM;
                    m_open.push_back(entry);
                    found = m_open.end() - 1;
                }
                found->reason = observation.reason;
                found->nearest = (std::min)(found->nearest, observation.distanceM);
                if (!found->begun && now - found->since >= m_config.thresholdMs)
                {
                    found->begun = true;
                    ++m_summary.episodes;
                    ++m_summary.perReason[static_cast<int>(found->firstReason)];
                    if (std::find(m_seen.begin(), m_seen.end(), found->id) == m_seen.end())
                    {
                        m_seen.push_back(found->id);
                        ++m_summary.entities;
                    }
                    Event event;
                    event.begin = true;
                    event.id = found->id;
                    event.reason = found->firstReason;
                    event.distanceM = found->nearest;
                    event.missingMs = now - found->since;
                    events.push_back(event);
                }
            }
        }

        // Pause, scrub, settle, jump: open episodes end (suspended), the
        // timers of the others start again after it.
        void Suspend(std::uint64_t now, std::vector<Event>& events)
        {
            for (Entry& entry : m_open)
                Close(entry, now, true, events);
            m_open.clear();
        }

        // End of playback.
        void Finish(std::uint64_t now, std::vector<Event>& events) { Suspend(now, events); }

        const Summary& Stats() const { return m_summary; }
        std::size_t Open() const { return m_open.size(); }

    private:
        struct Entry
        {
            std::uint32_t id = 0;
            std::uint64_t since = 0;
            Reason firstReason = Reason::NoPuppet;
            Reason reason = Reason::NoPuppet;
            float nearest = 0.0f;
            bool begun = false;
        };

        void Close(const Entry& entry, std::uint64_t now, bool suspended, std::vector<Event>& events)
        {
            if (!entry.begun)
                return;
            const std::uint64_t missingMs = now - entry.since;
            m_summary.totalMs += missingMs;
            m_summary.worstMs = (std::max)(m_summary.worstMs, missingMs);
            Event event;
            event.begin = false;
            event.id = entry.id;
            event.reason = entry.reason;
            event.distanceM = entry.nearest;
            event.missingMs = missingMs;
            event.suspended = suspended;
            events.push_back(event);
        }

        Config m_config;
        std::vector<Entry> m_open;
        std::vector<std::uint32_t> m_seen;
        Summary m_summary;
    };

    // The hidden unposed body as episodes, not ticks. A ped or horse without
    // an armed pose override is hidden (it would show its bind pose); that is
    // normal for a few ticks after it is created (until the first arm) and
    // after a segment switch or a skeleton rebuild releases the override
    // (re-arm). Such transients last 1 to 20 ticks (17 to 330 ms).
    // An episode is reported once, when it has lasted both persistMs of
    // wall time and persistTicks ticks (a body that never arms crosses both
    // in half a second at 60 fps); shorter ones are counted as transient in
    // the summary only. Ticks matter as well as time: the arm takes ticks,
    // so at 30 fps the same 20 ticks last twice as long.
    enum class UnposedCause : std::uint8_t
    {
        Create = 0, // never posed yet in this playback (first arm)
        Rearm,      // posed before: a segment switch, a rebuild, a re-dress
        Count,
    };

    inline const char* UnposedCauseText(UnposedCause cause)
    {
        return cause == UnposedCause::Create ? "create" : cause == UnposedCause::Rearm ? "rearm" : "?";
    }

    struct UnposedRule
    {
        std::uint32_t persistMs = 500;   // the puppet watch's threshold
        std::uint32_t persistTicks = 24; // above the 20 ticks of the longest transient seen
    };

    struct UnposedReport
    {
        std::uint32_t id = 0;
        UnposedCause cause = UnposedCause::Create;
        std::uint64_t hiddenMs = 0;  // wall time hidden so far
        std::uint32_t ticks = 0;     // ticks hidden so far
        std::uint32_t sincePlayheadMs = 0;
    };

    struct UnposedSummary
    {
        std::uint32_t episodes = 0;
        std::uint32_t transient = 0;  // ended before the rule
        std::uint32_t persistent = 0; // reported
        std::uint32_t perCause[static_cast<int>(UnposedCause::Count)] = {};
        std::uint32_t persistentPerCause[static_cast<int>(UnposedCause::Count)] = {};
        std::uint64_t worstTransientMs = 0;
        std::uint32_t worstTransientTicks = 0;
        std::uint64_t worstMs = 0;
    };

    class UnposedTracker
    {
    public:
        explicit UnposedTracker(const UnposedRule& rule = {}) : m_rule(rule) {}

        const UnposedRule& Rule() const { return m_rule; }

        void Reset()
        {
            m_open.clear();
            m_posed.clear();
            m_summary = {};
        }

        // Before the bodies of a tick are noted: an episode whose body was
        // not noted in the last tick (deleted, back to loading, playback
        // paused in another path) ends at its last noted time.
        void BeginTick()
        {
            for (std::size_t index = m_open.size(); index-- > 0;)
            {
                if (m_open[index].noted)
                {
                    m_open[index].noted = false;
                    continue;
                }
                Close(m_open[index]);
                m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }

        // One body this tick. True once per episode, when it crosses the
        // rule: `out` is the episode to report.
        bool Note(std::uint32_t id, bool unposed, std::uint64_t wallMs, std::uint32_t playheadMs, UnposedReport& out)
        {
            auto found = std::find_if(m_open.begin(), m_open.end(), [&](const Entry& e) { return e.id == id; });
            if (!unposed)
            {
                if (found != m_open.end())
                {
                    Close(*found);
                    m_open.erase(found);
                }
                if (std::find(m_posed.begin(), m_posed.end(), id) == m_posed.end())
                    m_posed.push_back(id);
                return false;
            }
            if (found == m_open.end())
            {
                Entry entry;
                entry.id = id;
                entry.since = wallMs;
                entry.last = wallMs;
                entry.sincePlayheadMs = playheadMs;
                entry.cause = std::find(m_posed.begin(), m_posed.end(), id) == m_posed.end() ? UnposedCause::Create
                                                                                           : UnposedCause::Rearm;
                ++m_summary.episodes;
                ++m_summary.perCause[static_cast<int>(entry.cause)];
                m_open.push_back(entry);
                found = m_open.end() - 1;
            }
            Entry& entry = *found;
            entry.noted = true;
            entry.last = wallMs;
            ++entry.ticks;
            if (entry.reported || wallMs - entry.since < m_rule.persistMs || entry.ticks < m_rule.persistTicks)
                return false;
            entry.reported = true;
            ++m_summary.persistent;
            ++m_summary.persistentPerCause[static_cast<int>(entry.cause)];
            out.id = entry.id;
            out.cause = entry.cause;
            out.hiddenMs = wallMs - entry.since;
            out.ticks = entry.ticks;
            out.sincePlayheadMs = entry.sincePlayheadMs;
            return true;
        }

        // End of playback, or a jump that rebuilds the puppets.
        void Finish()
        {
            for (const Entry& entry : m_open)
                Close(entry);
            m_open.clear();
        }

        const UnposedSummary& Stats() const { return m_summary; }
        std::size_t Open() const { return m_open.size(); }

    private:
        struct Entry
        {
            std::uint32_t id = 0;
            std::uint64_t since = 0;
            std::uint64_t last = 0;
            std::uint32_t sincePlayheadMs = 0;
            std::uint32_t ticks = 0;
            UnposedCause cause = UnposedCause::Create;
            bool noted = false;
            bool reported = false;
        };

        void Close(const Entry& entry)
        {
            const std::uint64_t ms = entry.last - entry.since;
            m_summary.worstMs = (std::max)(m_summary.worstMs, ms);
            if (entry.reported)
                return;
            ++m_summary.transient;
            if (ms > m_summary.worstTransientMs)
                m_summary.worstTransientMs = ms;
            m_summary.worstTransientTicks = (std::max)(m_summary.worstTransientTicks, entry.ticks);
        }

        UnposedRule m_rule;
        std::vector<Entry> m_open;
        std::vector<std::uint32_t> m_posed;
        UnposedSummary m_summary;
    };

    // A body that shows for the first time close to the camera, right after
    // it entered the recording (not after a jump onto it): it pops in, the
    // recording found it late (PUPPET_POP_IN).
    struct PopInRule
    {
        std::uint32_t graceMs = 1000;    // after the playback's start time
        std::uint32_t freshMs = 1000;    // shown within this of its recorded entry
        float nearMeters = 40.0f;
    };

    inline bool IsPopIn(std::uint32_t entryMs, std::uint32_t playheadMs, std::uint32_t startMs, float cameraMeters,
        const PopInRule& rule = {})
    {
        return entryMs > startMs + rule.graceMs && playheadMs >= entryMs && playheadMs - entryMs <= rule.freshMs &&
            cameraMeters >= 0.0f && cameraMeters < rule.nearMeters;
    }
}
