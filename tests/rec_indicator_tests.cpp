// The recording indicator (src/rec_indicator.h): the mode's ini words and
// steps, the fallback of the hidden mode, the exclusive fullscreen test, the
// window's scale and the brief mode's clock.
#include "rec_indicator.h"

#include <cmath>
#include <cstdio>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    namespace ri = replay::recindicator;

    void TestModes()
    {
        Check(ri::Parse("normal") == ri::Mode::Normal && ri::Parse("brief") == ri::Mode::Brief && ri::Parse("hidden") == ri::Mode::Hidden &&
                ri::Parse("off") == ri::Mode::Off,
            "every ini word reads back");
        Check(ri::Parse("HIDDEN") == ri::Mode::Hidden && ri::Parse("Brief") == ri::Mode::Brief, "the words in any case");
        Check(ri::Parse("2") == ri::Mode::Hidden && ri::Parse("3") == ri::Mode::Off, "the numbers too");
        Check(ri::Parse("") == ri::Mode::Normal && ri::Parse("hide") == ri::Mode::Normal && ri::Parse("hiddenx") == ri::Mode::Normal &&
                ri::Parse("9") == ri::Mode::Normal && ri::Parse(nullptr) == ri::Mode::Normal,
            "anything else is normal (the default)");
        for (std::uint32_t n = 0; n < ri::kModeCount; ++n)
            Check(ri::Parse(ri::IniValue(static_cast<ri::Mode>(n))) == static_cast<ri::Mode>(n), "IniValue and Parse agree");
        Check(ri::FromValue(7) == ri::Mode::Normal && ri::FromValue(1) == ri::Mode::Brief, "a snapshot value out of range is normal");
        // The settings row: arrows stop at the ends, a click wraps forward.
        Check(ri::Step(ri::Mode::Normal, 1, false) == ri::Mode::Brief && ri::Step(ri::Mode::Brief, 1, false) == ri::Mode::Hidden &&
                ri::Step(ri::Mode::Hidden, 1, false) == ri::Mode::Off,
            "right steps through the values in order");
        Check(ri::Step(ri::Mode::Off, 1, false) == ri::Mode::Off && ri::Step(ri::Mode::Normal, -1, false) == ri::Mode::Normal, "the arrows stop at the ends");
        Check(ri::Step(ri::Mode::Off, 1, true) == ri::Mode::Normal && ri::Step(ri::Mode::Brief, -1, true) == ri::Mode::Normal, "a click wraps forward");
    }

    void TestFallback()
    {
        ri::Environment ok;
        for (std::uint32_t n = 0; n < ri::kModeCount; ++n)
        {
            ri::Environment bad;
            bad.windowsSupports = false;
            bad.exclusiveFullscreen = true;
            const ri::Mode mode = static_cast<ri::Mode>(n);
            if (mode != ri::Mode::Hidden)
                Check(ri::Choose(mode, bad).mode == mode && ri::Choose(mode, bad).why == ri::Fallback::None, "only hidden ever falls back");
        }
        Check(ri::Choose(ri::Mode::Hidden, ok).mode == ri::Mode::Hidden && ri::Choose(ri::Mode::Hidden, ok).why == ri::Fallback::None,
            "hidden where its window shows");
        ri::Environment env;
        env.windowsSupports = false;
        Check(ri::Choose(ri::Mode::Hidden, env).mode == ri::Mode::Brief && ri::Choose(ri::Mode::Hidden, env).why == ri::Fallback::OldWindows,
            "older Windows: brief");
        env = ri::Environment{};
        env.exclusiveFullscreen = true;
        Check(ri::Choose(ri::Mode::Hidden, env).mode == ri::Mode::Brief && ri::Choose(ri::Mode::Hidden, env).why == ri::Fallback::ExclusiveFullscreen,
            "exclusive fullscreen: brief");
        env = ri::Environment{};
        env.affinityFailed = true;
        Check(ri::Choose(ri::Mode::Hidden, env).why == ri::Fallback::AffinityFailed && ri::Choose(ri::Mode::Hidden, env).mode == ri::Mode::Brief,
            "the display affinity refused: brief");
        env = ri::Environment{};
        env.windowFailed = true;
        Check(ri::Choose(ri::Mode::Hidden, env).why == ri::Fallback::WindowFailed, "no window: brief");
        env.windowsSupports = false;
        env.exclusiveFullscreen = true;
        Check(ri::Choose(ri::Mode::Hidden, env).why == ri::Fallback::OldWindows, "the first reason is told");
        Check(ri::WindowsSupports(10, 19041) && ri::WindowsSupports(10, 26100) && !ri::WindowsSupports(10, 19040) && !ri::WindowsSupports(6, 30000) &&
                ri::WindowsSupports(11, 0),
            "WDA_EXCLUDEFROMCAPTURE from Windows 10 2004 (build 19041)");
    }

    void TestExclusiveFullscreen()
    {
        const ri::Rect monitor{0, 0, 2560, 1440};
        const ri::Rect second{2560, 0, 6400, 2160};
        Check(ri::CoversMonitor({0, 0, 2560, 1440}, monitor), "a window as large as its monitor covers it");
        Check(ri::CoversMonitor({-8, -8, 2568, 1448}, monitor), "a maximised frame past the edges covers it");
        Check(!ri::CoversMonitor({0, 0, 1920, 1080}, monitor) && !ri::CoversMonitor({2560, 0, 6400, 2160}, monitor), "a smaller or other window does not");
        Check(ri::CoversMonitor({2560, 0, 6400, 2160}, second), "on a second monitor");
        Check(!ri::CoversMonitor({0, 0, 10, 10}, {0, 0, 0, 0}), "no monitor: no cover");
        Check(!ri::ExclusiveFullscreen(true, false, false), "borderless (covers, no fullscreen state): not exclusive");
        Check(ri::ExclusiveFullscreen(true, true, false), "a DXGI swap chain in fullscreen state: exclusive");
        Check(ri::ExclusiveFullscreen(true, false, true), "the shell's exclusive Direct3D full screen: exclusive");
        Check(!ri::ExclusiveFullscreen(false, true, true), "a window that does not cover its monitor is never exclusive");
    }

    void TestScale()
    {
        Check(std::fabs(ri::Scale(1080) - 1.0f) < 1e-6f && std::fabs(ri::Scale(1440) - 1.3333334f) < 1e-5f && std::fabs(ri::Scale(2160) - 2.0f) < 1e-6f,
            "the window's scale: the client height is 1080 units (1080p, 1440p, 4K)");
        Check(ri::Scale(0) == 0.0f, "no client area: no scale");
        Check(ri::RowWidth(30.0f, 0.0f) == 250.0f && ri::RowWidth(40.0f, 90.0f) == 250.0f, "a short row is 250 units wide (Notice)");
        Check(std::fabs(ri::RowWidth(300.0f, 0.0f) - 354.0f) < 1e-4f && std::fabs(ri::RowWidth(150.0f, 100.0f) - 332.0f) < 1e-4f,
            "a long row grows with its texts (Notice)");
        Check(ri::KindOf(false, false, 1200) == ri::Kind::Rec && ri::KindOf(true, true, 1200) == ri::Kind::Rec && ri::KindOf(true, false, 1200) == ri::Kind::Replay &&
                ri::KindOf(true, true, 0) == ri::Kind::Replay && ri::KindOf(false, false, 0) == ri::Kind::None,
            "the row's kind follows DrawRecordingNotices");
    }

    void TestBrief()
    {
        using K = ri::Kind;
        // A manual recording: shown 3 s from its start, then hidden.
        ri::BriefTimer t;
        t.Update(1000, K::None, 0, 5);
        Check(!t.Visible(1000), "nothing recording: nothing shown");
        t.Update(2000, K::Rec, 16, 5);
        Check(t.Visible(2000) && t.Visible(3000) && t.Visible(4999) && t.Shown() == K::Rec && !t.Stopped(), "shown for 3 s from the start");
        t.Update(3000, K::Rec, 1016, 5);
        Check(t.Visible(3000), "1 s after the start: shown");
        t.Update(5000, K::Rec, 3016, 5);
        Check(!t.Visible(5000), "3 s after the start: hidden");
        t.Update(7000, K::Rec, 5016, 5);
        Check(!t.Visible(7000), "5 s after the start: hidden");
        // The stop: the row of the recording that ended, its clock held.
        t.Update(9000, K::None, 0, 5);
        Check(t.Visible(9000) && t.Shown() == K::Rec && t.Stopped() && t.endedMs == 5016, "the stop shows the ended recording's row and clock");
        t.Update(11000, K::None, 0, 6);
        Check(t.Visible(11000) && t.until == 12000, "the save of a stopped recording does not show it again");
        Check(!t.Visible(12000), "3 s after the stop: hidden");

        // The replay buffer: its start, a save while it goes on.
        ri::BriefTimer b;
        b.Update(0, K::Replay, 0, 1);
        Check(b.Visible(0) && b.Shown() == K::Replay, "the first frame seen recording counts as a start");
        b.Update(3500, K::Replay, 0, 1);
        Check(!b.Visible(3500), "the buffer's tag hides after 3 s");
        b.Update(10000, K::Replay, 0, 2);
        Check(b.Visible(10000) && b.Visible(12999) && !b.Visible(13000) && b.Shown() == K::Replay, "a buffer save shows the tag for 3 s");
        // A manual mark inside the buffer and its end.
        b.Update(20000, K::Rec, 10, 2);
        Check(b.Visible(20000) && b.Shown() == K::Rec, "a manual recording inside the buffer: REC for 3 s");
        b.Update(30000, K::Replay, 0, 2);
        Check(b.Visible(30000) && b.Shown() == K::Replay && !b.Stopped(), "its end: the buffer's tag for 3 s");

        // A start right after a stop takes over.
        ri::BriefTimer c;
        c.Update(0, K::None, 0, 0);
        c.Update(100, K::Rec, 5, 0);
        c.Update(200, K::None, 0, 0);
        c.Update(300, K::Rec, 5, 0);
        Check(c.Visible(3299) && !c.Visible(3300) && c.Shown() == K::Rec && !c.Stopped(), "a new start restarts the 3 s with the live row");
    }
}

int RunRecIndicatorTests()
{
    g_failures = 0;
    TestModes();
    TestFallback();
    TestExclusiveFullscreen();
    TestScale();
    TestBrief();
    std::printf("rec indicator tests: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures;
}
