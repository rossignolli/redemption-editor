// Which camera owns the streaming focus while the replay owns the world
// (world_focus_core.h).
#include "world_focus_core.h"

#include <cstdio>

namespace
{
    using namespace replay::worldfocus;

    int g_focusFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_focusFailures;
    }

    Inputs Base()
    {
        Inputs in;
        in.playing = true;
        in.replayCam = 11;
        in.freeCam = 0;
        return in;
    }

    void TestChoose()
    {
        Inputs in = Base();
        in.playing = false;
        in.renderingCam = 11;
        Check(Choose(in) == Source::None, "not playing: the replay does not own the focus");

        in = Base();
        in.renderingCam = 11;
        Check(Choose(in) == Source::Replay, "replay camera renders");
        in.keyed = true;
        Check(Choose(in) == Source::Marker, "replay camera on markers");

        in = Base();
        in.freeCam = 22;
        in.freeCamEnabled = true;
        in.renderingCam = 22;
        Check(Choose(in) == Source::FreeCam, "free camera renders");
        in.editingMarker = true;
        Check(Choose(in) == Source::Edit, "Edit Camera");

        // Another mod's camera renders.
        in = Base();
        in.renderingCam = 5384;
        Check(Choose(in) == Source::Lent, "another mod's camera: lent");
        in.freeCam = 22;
        in.freeCamEnabled = true;
        Check(Choose(in) == Source::Lent, "another mod's camera over our free camera is still lent");

        // The engine switches one frame late.
        in = Base();
        in.renderingCam = -1;
        in.freeCam = 22;
        in.freeCamEnabled = true;
        Check(Choose(in) == Source::FreeCam, "free camera just activated, gameplay still rendering");
        in = Base();
        in.renderingCam = 0;
        in.replayCamPlaced = true;
        Check(Choose(in) == Source::Replay, "replay camera just activated");
        in = Base();
        in.renderingCam = -1;
        Check(Choose(in) == Source::Game, "clip without a game camera: the live gameplay camera");
    }

    void TestTracker()
    {
        Tracker t;
        Check(Update(t, Source::Replay), "first owner is a change");
        Check(!Update(t, Source::Replay), "same owner: no line");
        Check(Update(t, Source::Lent), "lend is a change");
        Check(Update(t, Source::FreeCam), "free camera is a change");
        Check(t.changes == 3, "three changes");

        Check(SampleDue(t, 1000), "first sample");
        Check(!SampleDue(t, 1500), "not before a second");
        Check(SampleDue(t, 2000), "a second later");
        Tracker capped;
        Check(SampleDue(capped, 0, 1) && !SampleDue(capped, 5000, 1), "cap");
        Reset(t);
        Check(t.last == Source::None && !t.owned && t.changes == 0, "reset");
        Check(UsesOwnPose(Source::FreeCam) && !UsesOwnPose(Source::Lent) && !UsesOwnPose(Source::Game), "own pose");
    }
}

int RunWorldFocusTests()
{
    TestChoose();
    TestTracker();
    return g_focusFailures;
}
