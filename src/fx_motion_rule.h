#pragma once

// Pure rules for replayed particle effects: recreation, the relocate mark,
// and the stretches without a call.
//
// Engine facts, build 1491.50:
// - Only two writers set bit 0x2 of an instance's +0x584 ("relocated"):
//   0x143C1E2 (in 0x143C180, every instance attached to an entity whose
//   transform a script native set, SET_ENTITY_COORDS_NO_OFFSET through the
//   vfunc +0x250 at 0xFA00B5) and 0x2C4F1CC (in 0x2C4F0E4, when the parent
//   moved more than the instance's +0x3A8 in one update, states 2..4). The
//   replay places every puppet with that native every tick, so every effect
//   attached to a puppet is likely marked relocated every frame (unconfirmed;
//   FX_RELOCATE_PROBE logs it), a jump for the emitter.
// - The registry update 0x2C52554 stops softly (0x2C52635) an entry not got
//   this frame and restarts (0x29E7644 at 0x2C5268A) an entry got again in
//   state 3..5: the hoof dust is a burst per call run, not a stream.
// - The attach update 0x2C52254 frees an entry (0x2C52FE0) whose entity
//   reference is null, whose bone matrix has a zero X axis outside the table
//   0x2C52A4C, or whose entity's draw data [[e+0x28]+8] is null (vfunc +0xA8
//   0x1405B94); the next get of the key creates the instance again.
//
// Pure (no game); unit tested in tests/particle_replay_tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "entity_events.h"

namespace replay::fxmotion
{
    constexpr std::uint32_t kRelocatedBit = 0x2;  // +0x584 bit 1 (0x143C1E2, 0x2C4F1CC)
    constexpr std::uint32_t kMovedBit = 0x10;      // +0x584 bit 4 (evidence: the game's own moving effects)
    // The hoof site's distance (0x146C1F9 writes +0x3A8 = 1.0 every call).
    constexpr float kHoofSiteTeleportM = 1.0f;
    constexpr std::uint32_t kHoofRegisteredSite = 0x146BF6F; // return address of its registered call (RGFX callerSite)
    // The registry restart's return address: an instance start from there is
    // a key got again after a stretch without a call (0x2C5268F).
    constexpr std::uint32_t kRegistryRestartSite = 0x2C5268F;

    inline bool Finite(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    // Whether the replay clears the relocate mark of an effect on a puppet it
    // placed this tick: only in normal play (a seek, a scrub, a rebuild keep the
    // mark: those are jumps), and only for a step below the engine's own limit
    // for that instance (its +0x3A8) and below 1 m (the hoof site's limit). A
    // step that is not a number keeps the mark.
    inline bool ShouldClearRelocate(bool normalPlay, float stepM, float instTeleportM)
    {
        if (!normalPlay || !Finite(stepM) || stepM < 0.0f)
            return false;
        float limit = 1.0f;
        if (Finite(instTeleportM) && instTeleportM > 0.0f)
            limit = (std::min)(instTeleportM, 1.0f);
        return stepM < limit;
    }

    // Clips without RGSS: the stretches without a call derived from the
    // frame times. The recorder stores a frame per called tick when the state
    // changed; a track on an entity that moves changes every tick, so its
    // frame times are its call times. Only a track sampled like that is
    // derived (median step at most kPerCallMedianMs and frames covering more
    // than half of its span at that step); a sparse track (a still object's
    // smoke) stays continuous. A step longer than kGapMs is a stretch. The
    // caller derives only for a track on a recorded entity (a puppet that
    // moves); a map object's track keeps the same state for ticks.
    constexpr std::uint32_t kPerCallMedianMs = 20;
    constexpr std::uint32_t kGapMs = 25;

    // minMissingTicks: the ticks a stretch must miss at the track's own step
    // (the playback's derived basis asks 2: a single missing frame of a clip
    // without RGSS is as often a tick the state did not change as a tick without
    // the call).
    inline std::vector<entity::RegisteredFxGap> DeriveCallGaps(const std::vector<std::uint32_t>& frameTimes,
        std::uint32_t minMissingTicks = 1)
    {
        std::vector<entity::RegisteredFxGap> gaps;
        if (frameTimes.size() < 4)
            return gaps;
        std::vector<std::uint32_t> steps;
        steps.reserve(frameTimes.size() - 1);
        for (std::size_t n = 1; n < frameTimes.size(); ++n)
            if (frameTimes[n] > frameTimes[n - 1])
                steps.push_back(frameTimes[n] - frameTimes[n - 1]);
        if (steps.size() < 3)
            return gaps;
        std::vector<std::uint32_t> sorted = steps;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        const std::uint32_t median = sorted[sorted.size() / 2];
        if (median == 0 || median > kPerCallMedianMs)
            return gaps;
        const std::uint32_t span = frameTimes.back() - frameTimes.front();
        if (span == 0 || static_cast<double>(frameTimes.size()) * median <= 0.5 * span)
            return gaps;
        // A missing tick at this track's own step (a 60 fps tick is 16 ms, the
        // limit 25 ms; a slower recording widens it to 1.5 steps).
        const std::uint32_t missing = minMissingTicks ? minMissingTicks : 1;
        const std::uint32_t limit = (std::max)(kGapMs, median * missing + median / 2 + 1);
        for (std::size_t n = 1; n < frameTimes.size(); ++n)
            if (frameTimes[n] > frameTimes[n - 1] + limit)
                gaps.push_back({frameTimes[n - 1], frameTimes[n]});
        return gaps;
    }

    // Playback: the playhead is inside a stretch without a call, or this tick
    // stepped over a whole one (at least one tick without the call per stretch,
    // so the registry stops and restarts the instance once). lastMs after timeMs
    // (a jump back) only checks containment. Returns the stretch's index, -1 none.
    inline int GapAt(const std::vector<entity::RegisteredFxGap>& gaps, std::uint32_t timeMs, std::uint32_t lastMs)
    {
        for (std::size_t n = 0; n < gaps.size(); ++n)
        {
            const entity::RegisteredFxGap& gap = gaps[n];
            if (gap.fromMs >= timeMs)
                break;
            if (timeMs < gap.toMs)
                return static_cast<int>(n);
            if (lastMs < timeMs && lastMs <= gap.fromMs && timeMs >= gap.toMs)
                return static_cast<int>(n);
        }
        return -1;
    }

    inline bool InGap(const std::vector<entity::RegisteredFxGap>& gaps, std::uint32_t timeMs, std::uint32_t lastMs)
    {
        return GapAt(gaps, timeMs, lastMs) >= 0;
    }

    // What a track did at a stretch without a call the
    // playback reached (FX_GAP_MISSED names those not entered). Ordered: a
    // later state overrides an earlier one except Entered, which is final.
    enum class GapSeen : std::uint8_t
    {
        NotSeen = 0,     // no tick of the track inside it
        NoEntry = 1,     // the track had no entry there (not created yet, owner missing or not ready)
        Hold = 2,        // paused or scrubbing there
        OwnerAbsent = 3, // the entity it rides left the replay there
        Entered = 4,     // entered: the registry stopped and restarted it
    };

    inline GapSeen MergeGapSeen(GapSeen before, GapSeen now)
    {
        return before == GapSeen::Entered ? before : now;
    }

    inline const char* GapSeenName(GapSeen value)
    {
        switch (value)
        {
        case GapSeen::NotSeen: return "not_seen";
        case GapSeen::NoEntry: return "no_entry";
        case GapSeen::Hold: return "hold";
        case GapSeen::OwnerAbsent: return "owner_absent";
        case GapSeen::Entered: return "entered";
        }
        return "?";
    }

    // What a creation of our registered instance means.
    enum class Recreate : std::uint8_t
    {
        First,          // the track's first creation in this stretch of play
        InSpan,         // made again in the same stretch: the engine freed the entry
        Reentered,      // made again after a jump, a clear or a scrub (expected)
        OwnerNotReady,  // made again while its entity was not ready (freed by the attach update)
    };

    // sameSpan: the track was already created since the last jump/clear.
    inline Recreate ClassifyRecreate(bool createdBefore, bool sameSpan, bool ownerReady)
    {
        if (!createdBefore)
            return Recreate::First;
        if (!sameSpan)
            return Recreate::Reentered;
        return ownerReady ? Recreate::InSpan : Recreate::OwnerNotReady;
    }

    inline const char* RecreateName(Recreate value)
    {
        switch (value)
        {
        case Recreate::First: return "first";
        case Recreate::InSpan: return "in_span";
        case Recreate::Reentered: return "reentered";
        case Recreate::OwnerNotReady: return "owner_not_ready";
        }
        return "?";
    }

    // After a recreation in the same stretch the track waits backoffMs
    // (wall clock) before it may get or create again; 0 turns it off.
    inline bool RecreateHeld(std::uint64_t nowMs, std::uint64_t lastRecreateMs, std::uint32_t backoffMs)
    {
        return backoffMs && lastRecreateMs && nowMs < lastRecreateMs + backoffMs;
    }

    // The attach update's three free conditions read from outside (the
    // entity exists, its draw data is there, its matrix has an X axis).
    inline bool OwnerReady(bool exists, bool drawData, const float axisX[3])
    {
        if (!exists || !drawData)
            return false;
        return axisX[0] != 0.0f || axisX[1] != 0.0f || axisX[2] != 0.0f;
    }

    // A puppet's velocity from two placements, zero across a jump (more
    // than 2 m at more than 50 m/s: ped_speed_core.h MotionEstimator's guard)
    // or when no time passed.
    inline void StepVelocity(const float from[3], const float to[3], float dtSeconds, float out[3])
    {
        out[0] = out[1] = out[2] = 0.0f;
        if (!(dtSeconds > 0.0f))
            return;
        const float dx = to[0] - from[0];
        const float dy = to[1] - from[1];
        const float dz = to[2] - from[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!Finite(distance) || (distance > 2.0f && distance / dtSeconds > 50.0f))
            return;
        out[0] = dx / dtSeconds;
        out[1] = dy / dtSeconds;
        out[2] = dz / dtSeconds;
    }

    // A step in metres between two placements.
    inline float StepMetres(const float from[3], const float to[3])
    {
        const float dx = to[0] - from[0];
        const float dy = to[1] - from[1];
        const float dz = to[2] - from[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}
