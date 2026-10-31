#pragma once

// Pure rules of the clip start's world readiness, so the core tests cover them.
//
// A clip opened kilometres from the player can reach the start wait's cap with
// the world not ready. CLIP_WORLD_READY lines give the readiness facts per tick while
// the start waits (CLIP_WORLD_READY: IPLs applied, load scene, collision at the
// camera, interiors, map objects, the engine's streaming requests);
// ClipWorldWait adds the collision to the wait and raises its cap to
// ClipWorldWaitMs. (The staggered creation of the puppets, ClipStartStagger,
// left with its keys: the puppets start with the playback.)

#include <algorithm>
#include <cstdint>

namespace replay::clipstart
{
    struct Config
    {
        bool worldWait = false;          // ClipWorldWait: the collision joins the wait, cap worldCapMs
        std::uint64_t worldCapMs = 20000; // ClipWorldWaitMs
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

    // The world at the camera, as ClipWorldWait judges it.
    inline bool WorldReady(const WorldFacts& facts, bool withCollision)
    {
        return facts.iplsApplied && facts.sceneLoaded && facts.interiorsReady && facts.mapSettled &&
            (!withCollision || facts.collisionLoaded);
    }

    // The start wait's cap: the old one, or long enough for the world cap.
    inline std::uint64_t StartWaitTimeoutMs(const Config& config, std::uint64_t baseMs)
    {
        if (!config.worldWait)
            return baseMs;
        return (std::max)(baseMs, config.worldCapMs);
    }

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
