#pragma once

// The rules of the clip load presentation (product mode), pure so the core
// tests cover them. The replay stays behind a black screen until the load
// scene at the first rendered camera reports loaded and the puppets are built,
// with a minimum black time (no flicker) and a timeout (never stuck).

#include <cstdint>

namespace replay::clipload
{
    struct Config
    {
        std::uint64_t minBlackMs = 300;   // shorter than this reads as a flicker
        std::uint64_t timeoutMs = 8000;   // fade in anyway after this
        float retargetMeters = 50.0f;     // the camera moved: load again there
        float jumpMeters = 50.0f;         // a jump this far goes black first
        std::uint64_t jumpPlayheadMs = 1000; // more than a tick's advance even at 2x in a Dead Eye stretch: a seek
    };

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline float DistanceSq(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    // The wait ends once the minimum black time passed and either the scene
    // and the puppets are ready or the timeout ran out.
    struct Verdict
    {
        bool done = false;
        bool timedOut = false;
    };

    inline Verdict Evaluate(std::uint64_t elapsedMs, bool sceneLoaded, bool puppetsReady, const Config& config = {})
    {
        Verdict verdict;
        if (elapsedMs < config.minBlackMs)
            return verdict;
        if (sceneLoaded && puppetsReady)
        {
            verdict.done = true;
            return verdict;
        }
        if (elapsedMs >= config.timeoutMs)
        {
            verdict.done = true;
            verdict.timedOut = true;
        }
        return verdict;
    }

    // The load scene follows the camera while the screen is black: a marker
    // camera resolves only once its puppets exist and may stand elsewhere.
    inline bool NeedsRetarget(bool sceneActive, const Vec3& sceneAt, const Vec3& camera, const Config& config = {})
    {
        return !sceneActive || DistanceSq(sceneAt, camera) > config.retargetMeters * config.retargetMeters;
    }

    // Not used by the script: a jump's far test is the seek settle's
    // (seek_settle.h FarBitsOf: 100 m, the collision, the model window), with the
    // frozen picture instead of the black screen.
    // A long jump: the playhead moved by more than a tick's advance (a seek, a
    // marker jump, a trim or a loop, not a scrub) and the camera landed far away.
    // A camera cut in normal play moves the camera but not the playhead, so it
    // never goes black.
    inline bool IsLongJump(bool hadPrevious, const Vec3& previousCamera, std::uint64_t previousPlayheadMs,
        const Vec3& camera, std::uint64_t playheadMs, bool scrubbing, const Config& config = {})
    {
        if (!hadPrevious || scrubbing)
            return false;
        const std::uint64_t moved =
            playheadMs > previousPlayheadMs ? playheadMs - previousPlayheadMs : previousPlayheadMs - playheadMs;
        if (moved <= config.jumpPlayheadMs)
            return false;
        return DistanceSq(previousCamera, camera) > config.jumpMeters * config.jumpMeters;
    }
}
