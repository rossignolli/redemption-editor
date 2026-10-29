#pragma once

// The game's time scale per recorded moment: Dead Eye, the weapon wheel (down
// to 0.1) and SET_TIME_SCALE slow the world while the recording clock keeps
// real time, so those stretches replay slowed. The recording stores the
// effective scale (scaled frame step / real frame step, public natives)
// whenever it changes; playback advances the playhead by 1/scale there, so the
// stretch plays at game speed (the time warp recorded per frame). Pure data,
// unit tested.
//
// 1.0 is absorbing. A ramp back from a slow motion can leave its last stored
// sample at 0.98 or 0.99 (1 - 0.98 is 0.0199999 in float, under the 2% step),
// and the rest of the clip would replay at 1/0.98 = 1.02: 1.2 s of drift per
// minute against any outside clock. So a ratio at or above kUnitScale is
// stored as 1.0, and the return to 1.0 is stored whatever the step; At reads a
// stored scale at or above kUnitScale as 1.0 (clips holding such samples play
// at 1). A real slow motion stays far below: Dead Eye and the weapon wheel
// record 0.1 to 0.6, a kill cam 0.06 to 0.7; between 0.97 and 1 only the one
// or two frames at the end of a ramp fall (each 17 ms frame then plays up to 3%
// faster, under a millisecond).

#include <cstdint>
#include <vector>

namespace replay::timescale
{
    constexpr float kMinScale = 0.02f;
    // At or above this a scale is real time (stored and read as 1.0).
    constexpr float kUnitScale = 0.97f;

    // The scale playback uses for a stored one (1.0 at or above kUnitScale).
    inline float Effective(float scale) { return scale >= kUnitScale ? 1.0f : scale; }

    struct Sample
    {
        std::uint32_t timeMs = 0;
        float scale = 1.0f;
    };

    class Track
    {
    public:
        void Begin() { m_samples.clear(); }
        // Stores a change of at least 2% (the first sample always), and the return to
        // 1.0 whatever the step.
        bool Note(std::uint32_t timeMs, float scale)
        {
            if (!(scale >= kMinScale)) // NaN too
                scale = kMinScale;
            scale = Effective(scale > 1.0f ? 1.0f : scale);
            if (!m_samples.empty())
            {
                const float last = m_samples.back().scale;
                if (scale == last)
                    return false;
                const float diff = scale > last ? scale - last : last - scale;
                if (diff < 0.02f && scale != 1.0f)
                    return false;
                if (timeMs < m_samples.back().timeMs)
                    timeMs = m_samples.back().timeMs;
            }
            m_samples.push_back({timeMs, scale});
            return true;
        }
        // The scale in effect at timeMs (1 before the first sample), read through
        // Effective (a stored 0.98 plays at 1).
        float At(std::uint32_t timeMs) const
        {
            float scale = 1.0f;
            for (const Sample& sample : m_samples)
            {
                if (sample.timeMs > timeMs)
                    break;
                scale = sample.scale;
            }
            return Effective(scale);
        }
        const std::vector<Sample>& Samples() const { return m_samples; }

    private:
        std::vector<Sample> m_samples;
    };
}
