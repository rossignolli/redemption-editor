#pragma once

// Pure rules of the clip start's world readiness and of the staggered creation
// of the puppets, so the core tests cover them.
//
// A clip opened kilometres from the player can reach the start wait's cap with
// the world not ready. ClipWorldLog writes the readiness facts per tick while
// the start waits (CLIP_WORLD_READY: IPLs applied, load scene, collision at the
// camera, interiors, map objects, the engine's streaming requests);
// ClipWorldWait adds the collision to the wait and raises its cap to
// ClipWorldWaitMs.
//
// A clip switch with dozens of IPLs toggled and ~130 entities created at once
// was followed by a crash at RDR2.exe+0x363068 (a DrawableStore eviction). A
// replay editor loads the world before it releases the entities and creates
// them in batches: ClipStartStagger holds the puppets' start (no model request,
// no create) until the clip's IPLs are applied and the world at the camera is
// ready, lets the streaming settle, then creates at most ClipStartBatch
// entities per tick until the opening census is built.

#include <algorithm>
#include <cstdint>

namespace replay::clipstart
{
    struct Config
    {
        bool log = true;                 // ClipWorldLog: CLIP_WORLD_READY lines while the start waits
        bool worldWait = false;          // ClipWorldWait: the collision joins the wait, cap worldCapMs
        std::uint64_t worldCapMs = 20000; // ClipWorldWaitMs
        bool stagger = false;            // ClipStartStagger
        std::uint32_t batch = 2;         // ClipStartBatch: creates per tick while staggering
        std::uint64_t settleMinMs = 500; // ClipStartSettleMs: the world ready this long before the first create
        std::uint64_t settleCapMs = 3000; // the streaming may stay busy; never longer than this
        int streamingMax = 8;            // ClipStartStreamingMax: requests the settle accepts (-1: ignore)
    };

    struct WorldFacts
    {
        bool iplsApplied = true;     // the clip's map state went in (product mode: after the fade)
        bool sceneLoaded = true;     // the load scene at the camera (or none needed)
        bool collisionLoaded = true; // HAS_COLLISION_LOADED_AROUND_POSITION at the camera
        bool interiorsReady = true;  // replay::world::Ready
        bool mapSettled = true;      // the map objects and stands of the closed theater
        int streamingRequests = 0;   // GET_NUMBER_OF_STREAMING_REQUESTS
    };

    // The world at the camera, as the stagger and ClipWorldWait judge it.
    inline bool WorldReady(const WorldFacts& facts, bool withCollision)
    {
        return facts.iplsApplied && facts.sceneLoaded && facts.interiorsReady && facts.mapSettled &&
            (!withCollision || facts.collisionLoaded);
    }

    // The start wait's cap: the old one, or long enough for the world cap,
    // the settle and the puppets' own preparation after the release.
    inline std::uint64_t StartWaitTimeoutMs(const Config& config, std::uint64_t baseMs)
    {
        if (!config.worldWait && !config.stagger)
            return baseMs;
        return (std::max)(baseMs, config.worldCapMs + (config.stagger ? config.settleCapMs + baseMs : 0));
    }

    enum class Phase
    {
        Off,       // no stagger: the puppets start with the playback
        WaitWorld, // held: no model request, no create
        Settle,    // held: the world is ready, the streaming settles
        Batches,   // released: at most `batch` creates per tick
        Done,      // the opening census is built: the normal budget
    };

    inline const char* PhaseText(Phase phase)
    {
        switch (phase)
        {
        case Phase::Off: return "off";
        case Phase::WaitWorld: return "wait_world";
        case Phase::Settle: return "settle";
        case Phase::Batches: return "batches";
        default: return "done";
        }
    }

    struct Step
    {
        bool release = false;  // start the puppets now
        bool timedOut = false; // released by the cap, not by the world
    };

    class Gate
    {
    public:
        void Begin(std::uint64_t nowMs, const Config& config)
        {
            m_config = config;
            m_beganAt = nowMs;
            m_readyAt = 0;
            m_releasedAt = 0;
            m_timedOut = false;
            m_phase = config.stagger ? Phase::WaitWorld : Phase::Off;
        }

        void Reset() { m_phase = Phase::Off; }

        Step Feed(std::uint64_t nowMs, const WorldFacts& facts)
        {
            Step step;
            if (m_phase == Phase::WaitWorld)
            {
                if (WorldReady(facts, true))
                {
                    m_phase = Phase::Settle;
                    m_readyAt = nowMs;
                }
                else if (nowMs - m_beganAt >= m_config.worldCapMs)
                {
                    m_timedOut = true;
                    return Release(nowMs, step, true);
                }
            }
            if (m_phase == Phase::Settle)
            {
                const std::uint64_t settled = nowMs - m_readyAt;
                const bool quiet = m_config.streamingMax < 0 || facts.streamingRequests <= m_config.streamingMax;
                if (settled >= m_config.settleMinMs && (quiet || settled >= m_config.settleCapMs))
                    return Release(nowMs, step, false);
            }
            return step;
        }

        // The opening census is built (playback::Preparing went false).
        void PuppetsBuilt()
        {
            if (m_phase == Phase::Batches)
                m_phase = Phase::Done;
        }

        bool Holding() const { return m_phase == Phase::WaitWorld || m_phase == Phase::Settle; }
        bool Batching() const { return m_phase == Phase::Batches; }
        Phase CurrentPhase() const { return m_phase; }
        std::uint64_t BeganAt() const { return m_beganAt; }
        std::uint64_t ReadyAt() const { return m_readyAt; }
        std::uint64_t ReleasedAt() const { return m_releasedAt; }
        bool TimedOut() const { return m_timedOut; }

        // The create budget of this tick: the batch while batching, else
        // the caller's (0 while held: nothing is created).
        std::uint32_t CapBudget(std::uint32_t base) const
        {
            if (Holding())
                return 0;
            if (m_phase == Phase::Batches)
                return (std::min)(base, (std::max)(1u, m_config.batch));
            return base;
        }

    private:
        Step Release(std::uint64_t nowMs, Step& step, bool timedOut)
        {
            m_phase = Phase::Batches;
            m_releasedAt = nowMs;
            step.release = true;
            step.timedOut = timedOut;
            return step;
        }

        Config m_config;
        Phase m_phase = Phase::Off;
        std::uint64_t m_beganAt = 0;
        std::uint64_t m_readyAt = 0;
        std::uint64_t m_releasedAt = 0;
        bool m_timedOut = false;
    };

    // CLIP_WORLD_READY lines: every tick for the first ticks of a start
    // (the measure the bench needs), then on a change of the facts or every
    // half second, at most kMaxLines per start.
    constexpr std::uint32_t kEveryTickLines = 90;
    constexpr std::uint64_t kSparseEveryMs = 500;
    constexpr std::uint32_t kMaxLines = 600;

    inline bool LogThisTick(std::uint32_t linesSoFar, bool changed, std::uint64_t nowMs, std::uint64_t lastLogMs)
    {
        if (linesSoFar >= kMaxLines)
            return false;
        if (linesSoFar < kEveryTickLines || changed)
            return true;
        return nowMs - lastLogMs >= kSparseEveryMs;
    }

    inline std::uint32_t FactsBits(const WorldFacts& facts)
    {
        return (facts.iplsApplied ? 1u : 0u) | (facts.sceneLoaded ? 2u : 0u) | (facts.collisionLoaded ? 4u : 0u) |
            (facts.interiorsReady ? 8u : 0u) | (facts.mapSettled ? 16u : 0u);
    }
}
