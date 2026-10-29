#pragma once

// Playhead transport (pure data, unit tested in ReplayCoreTests).
//
// Normal play advances the playhead at +1x. While the scrub key is held the
// playhead moves at a fixed speed in either direction, and the transport ends
// paused when the key is released or a boundary is reached. A fine scrub moves
// a fixed number of milliseconds per frame. One-shot events (shots, sounds,
// speech) play only in normal play, never while scrubbing.

#include <cstdint>

namespace replay::transport
{
    struct AdvanceResult
    {
        std::uint64_t timeMs;
        bool reachedBoundary;
    };

    constexpr AdvanceResult Advance(
        std::uint64_t currentMs,
        std::uint64_t deltaMs,
        std::uint64_t durationMs) noexcept
    {
        if (currentMs >= durationMs || deltaMs >= durationMs - currentMs)
            return {durationMs, true};
        return {currentMs + deltaMs, false};
    }

    // The most one frame of normal play moves the playhead. Advancing by a
    // whole wall-clock stall (hitches of 0.4 to 2.5 s in large scenes) steps
    // past the event cursors' 250 ms (ForwardEventCursor::kMaxStepMs), which
    // is a jump; a jump reconciles decals, blood, gore and the trail map in
    // one frame and stalls the next frame again (a cascade). Clamping the
    // step, as replay editors usually do, makes a hitch slow the replay for
    // a frame instead of skipping it.
    constexpr std::uint64_t kMaxNormalStepMs = 200;

    // Normal play's step for one frame: the scaled wall-clock delta, capped.
    constexpr std::uint64_t NormalStepMs(std::uint64_t scaledStepMs) noexcept
    {
        return scaledStepMs > kMaxNormalStepMs ? kMaxNormalStepMs : scaledStepMs;
    }

    // Scrub speed while the key is held (a key has no axis, so one speed).
    constexpr float kScrubSpeed = 2.0f;
    // Fine scrub: milliseconds per frame while the key is held.
    constexpr std::int64_t kFineScrubStepMs = 10;

    struct MoveResult
    {
        std::uint64_t timeMs;
        bool reachedStart;
        bool reachedEnd;
    };

    // Moves the playhead by a signed delta, clamped to [0, durationMs].
    constexpr MoveResult Move(
        std::uint64_t currentMs,
        std::int64_t deltaMs,
        std::uint64_t durationMs) noexcept
    {
        if (deltaMs >= 0)
        {
            const AdvanceResult forward =
                Advance(currentMs, static_cast<std::uint64_t>(deltaMs), durationMs);
            return {forward.timeMs, false, forward.reachedBoundary};
        }
        const std::uint64_t back = static_cast<std::uint64_t>(-deltaMs);
        if (back >= currentMs)
            return {0, true, false};
        return {currentMs - back, false, false};
    }

    // Signed playhead delta for one frame of scrubbing: the wall-clock frame
    // delta scaled by the cursor speed (negative backward), rounded.
    constexpr std::int64_t ScrubDeltaMs(std::uint64_t frameDeltaMs, float speed) noexcept
    {
        const double scaled = static_cast<double>(frameDeltaMs) * static_cast<double>(speed);
        return static_cast<std::int64_t>(scaled + (scaled < 0.0 ? -0.5 : 0.5));
    }

    // Signed playhead delta for one frame of fine scrubbing.
    constexpr std::int64_t FineScrubDeltaMs(int direction) noexcept
    {
        return direction < 0 ? -kFineScrubStepMs : direction > 0 ? kFineScrubStepMs : 0;
    }
}
