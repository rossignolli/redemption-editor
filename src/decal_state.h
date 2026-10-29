#pragma once

// Decal state helpers, pure data (no natives, no engine calls), shared by the
// recorder, the clip cut and the tests.
//
// Aging a rage decal settings copy to a playhead. A blood pool is one decal
// that is born at its final dims and grows by itself (UV multiplier); when the
// playback re-adds it after a jump, or the recording takes it as initial
// state, the copy must look as old as it is then: the pool is recreated at
// the size of the moment and without its fade. Fields of the settings
// (0x125CCF4 and the clamp 0x125F9EC): +0x44 life, +0x48 fade in, +0x4C fade
// out, +0x50 current/final growth (min 0.001), +0x54 slot +0x2C (growth time
// or rate, hypothesis: seconds), +0x60 = 1.0. Units of +0x44 and +0x54 are a
// hypothesis (seconds, like the engine's timers); POSE_V2_POOL_SAMPLE logs
// them at record time to confirm.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace replay::decal
{
    constexpr std::size_t kSettingsLife = 0x44;
    constexpr std::size_t kSettingsFadeIn = 0x48;
    constexpr std::size_t kSettingsGrowth = 0x50;
    constexpr std::size_t kSettingsGrowthTime = 0x54;
    constexpr std::size_t kSettingsGrowthEnd = 0x60;

    inline float SettingsFloat(const std::uint8_t* settings, std::size_t offset)
    {
        float value = 0.0f;
        std::memcpy(&value, settings + offset, sizeof(value));
        return value;
    }

    inline void SetSettingsFloat(std::uint8_t* settings, std::size_t offset, float value)
    {
        std::memcpy(settings + offset, &value, sizeof(value));
    }

    // A pool still growing at this age (+0x54 > 0 and age below it).
    inline bool SettingsGrowing(const std::uint8_t* settings, float ageSeconds)
    {
        const float growth = SettingsFloat(settings, kSettingsGrowthTime);
        return std::isfinite(growth) && growth > 0.0f && ageSeconds < growth;
    }

    // The growth value (+0x50) a copy aged by ageSeconds gets from AgeSettings
    // (the recorded +0x50 when it does not grow).
    inline float AgedGrowth(const std::uint8_t* settings, float ageSeconds)
    {
        const float from = SettingsFloat(settings, kSettingsGrowth);
        const float growth = SettingsFloat(settings, kSettingsGrowthTime);
        const float to = SettingsFloat(settings, kSettingsGrowthEnd);
        if (!(ageSeconds > 0.0f) || !std::isfinite(growth) || !(growth > 0.0f) || !std::isfinite(from) || !std::isfinite(to))
            return from;
        float fraction = ageSeconds / growth;
        fraction = fraction > 1.0f ? 1.0f : fraction;
        return from + (to - from) * fraction;
    }

    // A re-add of a decal already in place only to change its size (a regrow in
    // a reconcile) is worth it when the aged growth at the new playhead differs
    // from the one last applied by at least kRegrowStep of its range. A decal
    // whose growth cannot change (+0x50 == +0x60, e.g. splashes with growth 1->1
    // and +0x54 > 0) never regrows; otherwise it would be re-added on every scrub
    // tick and flood the core's request ring.
    constexpr float kRegrowStep = 0.05f;
    inline bool RegrowNeeded(const std::uint8_t* settings, float ageNow, float ageThen)
    {
        if (!SettingsGrowing(settings, ageNow) && !SettingsGrowing(settings, ageThen))
            return false;
        const float from = SettingsFloat(settings, kSettingsGrowth);
        const float to = SettingsFloat(settings, kSettingsGrowthEnd);
        if (!std::isfinite(from) || !std::isfinite(to))
            return false;
        const float range = std::fabs(to - from);
        if (!(range > 0.0f))
            return false;
        return std::fabs(AgedGrowth(settings, ageNow) - AgedGrowth(settings, ageThen)) >= kRegrowStep * range;
    }

    // Bounded retry of a refused regrow. The core refuses when its request ring
    // is full (0x2C2EF60: pending count [mgr+0xD75D8] >= 0x400); the decal
    // already in place stays as it was, so a refused regrow is tried again after
    // a delay, at most kRegrowMaxRefusals times, then the chain stops regrowing
    // until its event changes.
    constexpr std::uint32_t kRegrowMaxRefusals = 3;
    constexpr std::uint64_t kRegrowRetryMs = 250;
    struct RegrowRetry
    {
        std::uint32_t refusals = 0;
        std::uint64_t retryAtMs = 0;
    };
    inline bool RegrowAllowed(const RegrowRetry& retry, std::uint64_t nowMs)
    {
        return retry.refusals < kRegrowMaxRefusals && nowMs >= retry.retryAtMs;
    }
    // Returns true when this refusal makes the chain give up.
    inline bool NoteRegrowRefused(RegrowRetry& retry, std::uint64_t nowMs)
    {
        ++retry.refusals;
        retry.retryAtMs = nowMs + kRegrowRetryMs;
        return retry.refusals == kRegrowMaxRefusals;
    }
    inline void ResetRegrow(RegrowRetry& retry)
    {
        retry = RegrowRetry{};
    }

    // Ages the copy by ageSeconds (> 0): fade in 0; life -= age when life is
    // a positive finite value (0 or less is left alone: no evidence it means
    // a timed decal); +0x54 > 0: fraction = min(age / +0x54, 1), +0x50 moves
    // toward +0x60 by the fraction and +0x54 shrinks by it. False when the
    // decal has expired by then (the caller skips it).
    inline bool AgeSettings(std::uint8_t* settings, float ageSeconds)
    {
        if (!(ageSeconds > 0.0f))
            return true;
        SetSettingsFloat(settings, kSettingsFadeIn, 0.0f);
        const float life = SettingsFloat(settings, kSettingsLife);
        if (std::isfinite(life) && life > 0.0f)
        {
            if (ageSeconds >= life)
                return false;
            SetSettingsFloat(settings, kSettingsLife, life - ageSeconds);
        }
        const float growth = SettingsFloat(settings, kSettingsGrowthTime);
        if (std::isfinite(growth) && growth > 0.0f)
        {
            float fraction = ageSeconds / growth;
            fraction = fraction > 1.0f ? 1.0f : fraction;
            const float from = SettingsFloat(settings, kSettingsGrowth);
            const float to = SettingsFloat(settings, kSettingsGrowthEnd);
            if (std::isfinite(from) && std::isfinite(to))
                SetSettingsFloat(settings, kSettingsGrowth, from + (to - from) * fraction);
            SetSettingsFloat(settings, kSettingsGrowthTime, growth * (1.0f - fraction));
        }
        return true;
    }
}
