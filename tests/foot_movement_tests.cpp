// foot_movement_core.h: the movement volume result per step, recorded and
// given back.
#include "foot_movement_core.h"

#include <cmath>
#include <cstdio>

namespace
{
    using namespace replay::footmovement;

    int g_footMovementFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_footMovementFailures;
    }

    Sample M(std::uint32_t owner, std::uint32_t t, std::uint8_t foot, float result)
    {
        Sample s;
        s.owner = owner;
        s.timeMs = t;
        s.foot = foot;
        s.result = result;
        return s;
    }

    void TestTrack()
    {
        Track t;
        Check(t.Add(M(1, 100, 0, 2.06f)) && t.Add(M(1, 150, 1, 2.0f)) && t.Add(M(2, 160, 0, -1.97f)), "adds");
        Check(!t.Add(M(1, 170, 4, 0.0f)), "foot out of range refused");
        Check(!t.Add(M(1, 170, 0, std::nanf(""))), "nan refused");
        Check(!t.Add(M(1, 170, 0, 99.0f)), "implausible refused");
        Check(t.Add(M(1, 120, 2, 1.0f)) && t.samples.back().timeMs == 160, "late time clamped");
        Check(t.Valid(), "valid");
        Track cut;
        t.CutInto(cut, 120, 160, [](std::uint32_t owner) { return owner == 1; });
        Check(cut.samples.size() == 2 && cut.samples[0].timeMs == 30 && cut.samples[1].foot == 2, "cut keeps the owner inside");
        Check(t.DropBefore(150) == 1 && t.samples.front().timeMs == 150, "drop before");
    }

    void TestNearest()
    {
        Track t;
        t.Add(M(1, 1000, 0, 2.0f));
        t.Add(M(1, 1300, 0, 2.1f));
        t.Add(M(1, 1310, 1, 1.5f));
        t.Add(M(2, 1320, 0, -2.0f));
        float v = 0.0f;
        Check(Nearest(t, 1, 0, 1100, v) && v == 2.0f, "nearest before");
        Check(Nearest(t, 1, 0, 1250, v) && v == 2.1f, "nearest after");
        Check(Nearest(t, 1, 1, 1200, v) && v == 1.5f, "the foot's own");
        Check(!Nearest(t, 1, 2, 1300, v), "no such foot");
        Check(!Nearest(t, 1, 0, 1600, v), "outside the window");
        Check(Nearest(t, 2, 0, 1320, v) && v == -2.0f, "the owner's own");
        Check(!Nearest(Track{}, 1, 0, 0, v), "empty");
        Check(UseRecorded(true, true) && !UseRecorded(true, false) && !UseRecorded(false, true), "use rule");
    }
}

int RunFootMovementTests()
{
    TestTrack();
    TestNearest();
    return g_footMovementFailures;
}
