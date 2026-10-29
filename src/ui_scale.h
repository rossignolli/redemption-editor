#pragma once

// The editor's interface scale (Replay Settings "Interface scale",
// RedemptionEditor.ini InterfaceScale, in percent).
//
// The editor lays out in units: at 100% the display height is 1080 units at
// every resolution (replay_editor_addon.cpp g_scale), so the interface keeps
// its size relative to the picture at 1080p, 1440p and 4K. The scale
// multiplies the pixels of a unit: at 125% a unit is 1.25 times larger and
// the screen holds 1080 / 1.25 = 864 units of height (and as much less
// width). The steps stop where every screen still passes the UI harness
// (tools/ui_harness, --scale) at every resolution:
//   - 125% is the largest. At 133% (a 16:9 screen past its width floor) and
//     150% the key help's columns run off the right edge and the dialogs'
//     1280 x 720 stage has no room left; Replay Settings' list scrolls from
//     about 110% on a short key bar.
//   - The edit screen's panels, the key help and the timeline need 1440
//     units of width, what a 4:3 screen has at 100%: a narrower screen gets
//     the largest scale that keeps them (4:3 stays at 100%, 16:10 at 120%).
//   - Smaller, a unit never gets fewer pixels than at 1280 x 720 and 100%
//     (2/3 of a pixel): the small labels already stand at the fonts'
//     readable minimum there and would outgrow their boxes. So 80% shows as
//     about 94% on a 768-line screen and as 80% from 900 lines up.
// The ini value is clamped into [kMin, kMax]; a value between the steps is
// kept as it is (the arrows step to the next one).

#include <algorithm>
#include <cstdint>

namespace replay::editor::uiscale
{
    inline constexpr int kDefault = 100;
    inline constexpr int kSteps[] = {80, 90, 100, 110, 125};
    inline constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
    inline constexpr int kMin = kSteps[0];
    inline constexpr int kMax = kSteps[kStepCount - 1];

    // The layout's floor, in units at the chosen scale (see above).
    inline constexpr float kMinUnitsWide = 1440.0f;
    inline constexpr float kMinUnitsHigh = 1080.0f * 100.0f / static_cast<float>(kMax);
    // The fewest pixels a unit gets (1280 x 720 at 100%).
    inline constexpr float kMinPixelsPerUnit = 720.0f / 1080.0f;

    // The ini value as the editor uses it: in [kMin, kMax] (0 or a value
    // that did not parse reads as the default).
    inline int Clamp(int percent)
    {
        if (percent <= 0)
            return kDefault;
        return std::clamp(percent, kMin, kMax);
    }

    // The next step in `direction` (+1 larger, -1 smaller). The arrows stop
    // at both ends; a click (`wrap`) goes forward and wraps to the smallest.
    inline int Step(int percent, int direction, bool wrap)
    {
        percent = Clamp(percent);
        if (direction > 0)
        {
            for (int n = 0; n < kStepCount; ++n)
                if (kSteps[n] > percent)
                    return kSteps[n];
            return wrap ? kSteps[0] : kSteps[kStepCount - 1];
        }
        if (direction < 0)
        {
            for (int n = kStepCount - 1; n >= 0; --n)
                if (kSteps[n] < percent)
                    return kSteps[n];
            return kSteps[0];
        }
        return percent;
    }

    // The factor a unit's pixels are multiplied by on a display of this size:
    // the chosen percent, never so large that the screen holds fewer than
    // kMinUnitsWide x kMinUnitsHigh units (a 4:3 screen keeps 100% then), nor
    // so small that a unit gets fewer than kMinPixelsPerUnit pixels. Neither
    // limit goes past 100%: the default is what every display has today.
    inline float Effective(int percent, float displayWidth, float displayHeight)
    {
        const float wanted = static_cast<float>(Clamp(percent)) / 100.0f;
        if (displayWidth <= 0.0f || displayHeight <= 0.0f)
            return wanted;
        if (wanted < 1.0f)
            return std::min(1.0f, std::max(wanted, kMinPixelsPerUnit * 1080.0f / displayHeight));
        // Units at 100%: 1080 high, as wide as the aspect.
        const float unitsWide = 1080.0f * displayWidth / displayHeight;
        const float fit = std::min(unitsWide / kMinUnitsWide, 1080.0f / kMinUnitsHigh);
        return std::max(1.0f, std::min(wanted, fit));
    }
}
