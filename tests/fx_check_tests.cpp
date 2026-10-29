// The fx check tally (recording against playback by effect hash).
#include "fx_check_core.h"

#include <cstdio>
#include <string>

namespace
{
    int g_fxCheckFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_fxCheckFailures;
    }

    using replay::fxcheck::Maker;
    using replay::fxcheck::Stored;
    using replay::fxcheck::Tally;

    void TestRawBasis()
    {
        Tally tally;
        tally.BeginRecord();
        for (int n = 0; n < 14; ++n)
            tally.NoteRecord(0x581efe72, Stored::Stored, false, 0x145495d);
        tally.NoteRecord(0x51e9893e, Stored::Shot, false, 0x1452671);
        tally.NoteRecord(0x7125c257, Stored::Registered, true, 0x1463b16);
        Check(tally.HasRecord(), "a recording leaves its tally");
        tally.ResetPlay();
        for (int n = 0; n < 14; ++n)
            tally.NoteClip(0x581efe72, false);
        tally.NoteClip(0x7125c257, true);
        // The replay plays the 14 splashes, the puppet makes 12 more of its own.
        for (int n = 0; n < 14; ++n)
            tally.NotePlay(0x581efe72, Maker::Ours, false, 0);
        for (int n = 0; n < 12; ++n)
            tally.NotePlay(0x581efe72, Maker::EnginePuppet, false, 0x145495d);
        tally.NotePlay(0x51e9893e, Maker::Ours, false, 0);
        const auto raw = tally.Summarize(true);
        Check(raw.rec == 16, "recording basis counts every start and creation");
        Check(raw.play == 27, "playback counts every maker");
        Check(raw.missing == 1, "the registered fire not played is missing");
        Check(raw.extra == 12, "the puppet's own splashes are extra");
        Check(raw.enginePuppet == 12 && raw.ours == 15, "makers split");
        const auto clip = tally.Summarize(false);
        Check(clip.rec == 15, "clip basis counts the clip's content (the shot is not in the fx track)");
        Check(clip.extra == 13, "clip basis: the shot's muzzle and the doubles are extra");
        const std::string line = tally.SummaryLine("x.rpclip", true, "none");
        Check(line.find("FX_CHECK,clip=x.rpclip,basis=recording") == 0, "summary line head");
        Check(line.find("top_extra=0x581efe72:12") != std::string::npos, "top extra names the splash");
        Check(line.find("top_missing=0x7125c257:1") != std::string::npos, "top missing names the fire");
    }

    void TestRefusedAndReset()
    {
        Tally tally;
        tally.BeginRecord();
        tally.NoteRecord(0x1, Stored::Stored, false, 0);
        tally.ResetPlay();
        tally.NoteClip(0x1, false);
        tally.NotePlay(0x1, Maker::Ours, false, 0);
        tally.NoteRefused(0x1);
        tally.NoteRefused(0x1);
        const auto s = tally.Summarize(true);
        Check(s.refused == 2 && s.extra == 0 && s.missing == 0, "refused starts are neither play nor extra");
        const std::string report = tally.Report("x", "v", "now", true, {});
        Check(report.find("SUMMARY,effects=1,rec=1,play=1") != std::string::npos, "report summary");
        tally.Reset();
        Check(!tally.HasRecord() && tally.Rows().empty(), "reset forgets the recording");
        const auto empty = tally.Summarize(false);
        Check(empty.rec == 0 && empty.MissingPct() == 0.0, "empty tally");
    }
}

int RunFxCheckTests()
{
    TestRawBasis();
    TestRefusedAndReset();
    return g_fxCheckFailures;
}
