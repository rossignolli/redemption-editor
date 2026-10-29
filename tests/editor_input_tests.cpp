// The editor's input rules (editor_input.h): a key tap that falls inside one
// long frame is still a press, and a value stepped twice before the snapshot
// comes back builds on the one sent.
#include "editor_input.h"

#include <cstdio>

namespace
{
    using replay::editor::input::KeyEdges;
    using replay::editor::input::PendingValue;

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

    constexpr unsigned kEnter = 0x0D;

    // One frame as ReShade reports it: the level at the frame's end and
    // whether an up came during it (is_key_released).
    struct Frame
    {
        bool down;
        bool released;
    };

    void TestTaps()
    {
        // A normal press: down for a few frames, then up. ImGui saw it down;
        // the release is not a second press.
        {
            KeyEdges keys;
            const Frame frames[] = {{false, false}, {true, false}, {true, false}, {false, true}, {false, false}};
            int missed = 0;
            for (const Frame& f : frames)
            {
                keys.BeginFrame(kEnter, f.down);
                missed += keys.MissedTap(kEnter, f.released) ? 1 : 0;
            }
            Check(missed == 0, "taps: a press seen down is never counted again at its release");
        }
        // The whole press inside one frame (a stall): never down at a
        // frame's end, released in it. That is a press.
        {
            KeyEdges keys;
            keys.BeginFrame(kEnter, false);
            Check(!keys.MissedTap(kEnter, false), "taps: nothing pressed, nothing missed");
            keys.BeginFrame(kEnter, false);
            Check(keys.MissedTap(kEnter, true), "taps: down and up inside one frame is a press");
            keys.BeginFrame(kEnter, false);
            Check(!keys.MissedTap(kEnter, false), "taps: the missed press counts once");
        }
        // A press seen down at the frame before, released now: the end of a
        // seen press (the 5 s stuck-key reset of ReShade looks the same).
        {
            KeyEdges keys;
            keys.BeginFrame(kEnter, true);
            keys.BeginFrame(kEnter, false);
            Check(!keys.MissedTap(kEnter, true), "taps: the release of a press seen down is not a tap");
        }
        // Two quick taps in two long frames: two presses.
        {
            KeyEdges keys;
            int taps = 0;
            for (int n = 0; n < 2; ++n)
            {
                keys.BeginFrame(kEnter, false);
                taps += keys.MissedTap(kEnter, true) ? 1 : 0;
            }
            Check(taps == 2, "taps: two quick taps in two long frames are two presses");
        }
        {
            KeyEdges keys;
            Check(!keys.MissedTap(0, true) && !keys.MissedTap(300, true), "taps: keys out of range are never pressed");
        }
        // The left mouse button kept at VK_LBUTTON (1): a click inside a
        // long frame is a click; a held button released is not.
        {
            KeyEdges keys;
            constexpr unsigned kLeftButton = 0x01;
            keys.BeginFrame(kLeftButton, false);
            Check(keys.MissedTap(kLeftButton, true), "clicks: down and up inside one frame is a click");
            keys.BeginFrame(kLeftButton, true);
            keys.BeginFrame(kLeftButton, false);
            Check(!keys.MissedTap(kLeftButton, true), "clicks: the end of a drag is not a click");
        }
    }

    void TestPendingValues()
    {
        PendingValue<unsigned> speed;
        Check(speed.Base(0, 4, 1000) == 4, "pending: nothing sent, the snapshot's value");
        speed.Sent(0, 5, 1000);
        Check(speed.Base(0, 4, 1010) == 5, "pending: a second step before the snapshot catches up builds on the value sent");
        Check(speed.Base(0, 5, 1010) == 5, "pending: the snapshot caught up, the same value");
        Check(speed.Base(0, 4, 1000 + PendingValue<unsigned>::kWindowMs) == 4,
            "pending: after the window the snapshot wins (a refused value does not stick)");
        Check(speed.Base(7, 4, 1010) == 4, "pending: another marker steps from its own snapshot value");
        Check(speed.Base(0, 4, 900) == 4, "pending: a clock before the send is not trusted");
        speed.Clear();
        Check(speed.Base(0, 4, 1010) == 4, "pending: cleared (paste, undo, another clip), the snapshot again");

        // A held arrow at 30 repeats a second while the snapshot lags two
        // frames: every step lands one further (no value sent twice, no step
        // lost).
        PendingValue<unsigned> fov;
        unsigned snapshot = 50;
        unsigned sent = 0;
        std::uint64_t now = 0;
        for (int n = 0; n < 6; ++n, now += 33)
        {
            const unsigned value = fov.Base(1, snapshot, now) + 1;
            fov.Sent(1, value, now);
            sent = value;
            if (n % 2 == 1)
                snapshot = value; // the core's answer arrives every other step
        }
        Check(sent == 56, "pending: six quick steps move the value six times");
    }
}

int RunEditorInputTests()
{
    TestTaps();
    TestPendingValues();
    std::printf("  editor input: %d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0)
        std::puts("Editor input tests passed.");
    return g_failures;
}
