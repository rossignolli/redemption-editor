// The replay's music stop (music_stop.h). Enter and exit pair up whatever the
// order of calls (two entries, two exits, a stop in the middle, the unload),
// the sensor samples once a second, music past the fade asks for a new stop and
// is reported as music_in_replay (qa_health.h).
#include "log_policy.h"
#include "music_stop.h"
#include "qa_health.h"

#include <cstdio>

namespace
{
    using namespace replay::music;

    int g_musicFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_musicFailures;
    }

    void TestEnterExitPairs()
    {
        Guard guard;
        Check(guard.Exit().already, "exit without entry does nothing");
        Plan enter = guard.Enter(Mode::Natives, 1000);
        Check(enter.setFlag && enter.flagValue && enter.triggerStop && !enter.stopScenes && !enter.already,
            "entry sets the flag and fires the stop event");
        Check(guard.On(), "on after entry");
        Check(guard.Enter(Mode::Natives, 2000).already, "a second entry does nothing");
        Plan exit = guard.Exit();
        Check(exit.setFlag && !exit.flagValue && exit.stopScenes && !exit.triggerStop && !exit.already,
            "exit clears the flag and stops the scenes");
        Check(!guard.On(), "off after exit");
        Check(guard.Exit().already, "a second exit does nothing");
        // StartPlayback runs StopPlayback first: exit (nothing) then entry.
        for (int round = 0; round < 3; ++round)
        {
            Check(guard.Exit().already, "the stop before each playback does nothing when off");
            Check(!guard.Enter(Mode::Natives, 5000 + round).already, "each playback enters");
            Check(!guard.Exit().already, "each stop exits");
        }
    }

    void TestOffMode()
    {
        Guard guard;
        Check(ModeFromIni(0) == Mode::Off && ModeFromIni(1) == Mode::Natives && ModeFromIni(2) == Mode::Natives,
            "MusicStop 0 off, 1 and 2 natives");
        Plan enter = guard.Enter(Mode::Off, 1000);
        Check(enter.already && !enter.setFlag && !enter.triggerStop, "off: no flag, no event");
        Check(!guard.On() && guard.Exit().already, "off: nothing to undo");
        int sensed = 0;
        Check(guard.Watch(5000, [&] { ++sensed; return true; }) == WatchResult::Idle && sensed == 0, "off: no sensor");
    }

    void TestStartup()
    {
        const Plan plan = Guard::Startup();
        Check(plan.setFlag && !plan.flagValue && !plan.stopScenes && !plan.triggerStop, "startup clears the flag only");
    }

    void TestWatchFadeAndRestop()
    {
        Guard guard;
        guard.Enter(Mode::Natives, 0);
        int sensed = 0;
        bool playing = true;
        const auto sense = [&] { ++sensed; return playing; };
        Check(guard.Watch(500, sense) == WatchResult::Idle && sensed == 0, "no sample before a second");
        Check(guard.Watch(1000, sense) == WatchResult::Changed && sensed == 1, "first sample is a change");
        Check(guard.Watch(1500, sense) == WatchResult::Idle, "one sample a second");
        // Still on during the 8 s fade: not an anomaly.
        for (std::uint64_t t = 2000; t <= 10000; t += 1000)
            Check(guard.Watch(t, sense) != WatchResult::Restop, "the fade is not an anomaly");
        Check(guard.Watch(11000, sense) == WatchResult::Restop, "on 2 s past the grace: stop again");
        Check(guard.Restops() == 1, "one restop");
        for (std::uint64_t t = 12000; t <= 21000; t += 1000)
            Check(guard.Watch(t, sense) != WatchResult::Restop, "a whole new grace after a restop");
        Check(guard.Watch(22000, sense) == WatchResult::Restop, "still on: stop again");

        // Faded in time: never an anomaly.
        Guard quiet;
        quiet.Enter(Mode::Natives, 0);
        playing = true;
        for (std::uint64_t t = 1000; t <= 7000; t += 1000)
            quiet.Watch(t, sense);
        playing = false;
        Check(quiet.Watch(8000, sense) == WatchResult::Changed, "music gone is a change");
        for (std::uint64_t t = 9000; t <= 60000; t += 1000)
            Check(quiet.Watch(t, sense) == WatchResult::Sampled, "quiet stays quiet");
        // A script fires music later: 2 s on means a restop.
        playing = true;
        Check(quiet.Watch(61000, sense) == WatchResult::Changed, "music back is a change");
        Check(quiet.Watch(62000, sense) == WatchResult::Sampled, "1 s on is not yet an anomaly");
        Check(quiet.Watch(63000, sense) == WatchResult::Restop, "2 s on is an anomaly");
        quiet.Exit();
        Check(quiet.Watch(70000, sense) == WatchResult::Idle, "no sensor after the exit");
    }

    void TestAnomalyLine()
    {
        replay::qa::Anomaly anomaly;
        Check(replay::qa::ClassifyLine("MUSIC_STOP,phase=restop,why=playing_again,t_ms=11000,event_ok=1", 4200, anomaly) &&
                  anomaly.kind == replay::qa::Kind::MusicInReplay && anomaly.playheadMs == 4200 && anomaly.entity.empty() &&
                  anomaly.detail == "why=playing_again;t_ms=11000;event_ok=1",
            "restop line is music_in_replay");
        Check(!replay::qa::ClassifyLine("MUSIC_STOP,phase=enter,mode=1,playing_before=1,flag=1,event_ok=1", 0, anomaly),
            "entry line is not an anomaly");
        Check(replay::log::EssentialLine("MUSIC_STOP,phase=exit,why=playback_stop,reload=0,flag=0\n") &&
                  replay::log::EssentialLine("MUSIC_WATCH,t_ms=1000,playing=1\n"),
            "the music lines are kept with Debug=0");
        Check(std::string(replay::qa::KindText(replay::qa::Kind::MusicInReplay)) == "music_in_replay", "kind text");
    }
}

int RunMusicStopTests()
{
    g_musicFailures = 0;
    TestEnterExitPairs();
    TestOffMode();
    TestStartup();
    TestWatchFadeAndRestop();
    TestAnomalyLine();
    return g_musicFailures;
}
