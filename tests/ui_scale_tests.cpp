// src/ui_scale.h: Replay Settings' "Interface scale" (InterfaceScale in
// RedemptionEditor.ini): the steps, the ini value clamped, and the factor in
// use on each display (the width and pixel floors).
#include "ui_scale.h"

#include <cmath>
#include <cstdio>

namespace
{
    using namespace replay::editor::uiscale;

    int g_failures = 0;
    int g_checks = 0;

    void Check(bool condition, const char* message)
    {
        ++g_checks;
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

    void TestClamp()
    {
        Check(Clamp(100) == 100, "100 is kept");
        Check(Clamp(0) == kDefault && Clamp(-5) == kDefault, "0 or negative (a value that did not parse) reads as 100");
        Check(Clamp(50) == kMin && Clamp(300) == kMax, "out of range is clamped");
        Check(Clamp(105) == 105, "a value between the steps is kept");
        Check(kMin == 80 && kMax == 125, "the range is 80 to 125");
    }

    void TestStep()
    {
        Check(Step(100, 1, false) == 110 && Step(110, 1, false) == 125, "right goes to the next step");
        Check(Step(100, -1, false) == 90 && Step(90, -1, false) == 80, "left goes to the previous step");
        Check(Step(125, 1, false) == 125 && Step(80, -1, false) == 80, "the arrows stop at both ends");
        Check(Step(125, 1, true) == 80, "a click wraps to the smallest");
        Check(Step(105, 1, false) == 110 && Step(105, -1, false) == 100, "a value between the steps goes to its neighbours");
    }

    void TestEffective()
    {
        // 100% is what every display had before the setting.
        const float sizes[][2] = {{1280, 720}, {1366, 768}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}, {1024, 768}, {1280, 800}};
        for (const auto& size : sizes)
            Check(Near(Effective(100, size[0], size[1]), 1.0f), "100% is 1 on every display");
        Check(Near(Effective(125, 1920, 1080), 1.25f) && Near(Effective(125, 3840, 2160), 1.25f), "16:9 takes 125%");
        Check(Near(Effective(125, 3440, 1440), 1.25f), "ultrawide takes 125%");
        Check(Near(Effective(125, 1024, 768), 1.0f) && Near(Effective(110, 1024, 768), 1.0f), "4:3 keeps 1440 units: 100% at most");
        Check(Near(Effective(125, 1280, 800), 1.2f), "16:10 stops at 1440 units wide (120%)");
        Check(Near(Effective(80, 1920, 1080), 0.8f) && Near(Effective(80, 3840, 2160), 0.8f), "80% from 1080 lines up");
        Check(Near(Effective(80, 1280, 720), 1.0f), "1280 x 720 does not go below 100%");
        Check(Near(Effective(80, 1366, 768), 720.0f / 768.0f), "768 lines: a unit keeps 2/3 of a pixel");
        // The layout: the units the screen holds stay inside what the harness checks.
        for (const auto& size : sizes)
            for (const int pct : kSteps)
            {
                const float factor = Effective(pct, size[0], size[1]);
                const float unitsHigh = 1080.0f / factor;
                const float unitsWide = 1080.0f * size[0] / size[1] / factor;
                const float pixelsPerUnit = size[1] / 1080.0f * factor;
                Check(unitsHigh >= kMinUnitsHigh - 0.5f, "never fewer than 864 units of height");
                Check(unitsWide >= kMinUnitsWide - 0.5f || factor <= 1.0f, "never under 1440 units wide when enlarged");
                Check(pixelsPerUnit >= kMinPixelsPerUnit - 0.001f || factor >= 1.0f, "never fewer pixels a unit than 1280 x 720");
            }
    }
}

int RunUiScaleTests()
{
    TestClamp();
    TestStep();
    TestEffective();
    std::printf("ui_scale_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
