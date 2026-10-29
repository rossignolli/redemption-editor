// The time scale that stuck under 1 after a slow motion (time_scale.h), the
// log stream's float format (log_format.h) and the unposed body episodes
// (puppet_watch.h UnposedTracker).
#include "log_format.h"
#include "puppet_watch.h"
#include "qa_health.h"
#include "time_scale.h"

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    int g_timeScaleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_timeScaleFailures;
    }

    bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

    void TestTimeScaleReturnsToOne()
    {
        using replay::timescale::Track;
        // The live ramp (live.log): 1, down to 0.58, back up, ending 0.97,
        // 0.98, 0.99 and then real time.
        Track track;
        const float ramp[] = {1.0f, 0.82f, 0.64f, 0.58f, 0.70f, 0.82f, 0.94f, 0.97f, 0.98f, 0.99f, 1.0f, 1.0f};
        std::uint32_t t = 0;
        for (const float scale : ramp)
        {
            track.Note(t, scale);
            t += 17;
        }
        Check(Near(track.Samples().back().scale, 1.0f), "ramp 0.58 -> 0.97 -> 0.98 -> 0.99 -> 1.0 ends stored at 1.0");
        Check(Near(track.At(60000), 1.0f), "after the ramp the clip plays at 1.0");

        // The exact float case: 0.98 then 1.0 (1 - 0.98 is 0.0199999 < 0.02
        // in float, a plain 2% rule would drop it).
        Track exact;
        exact.Note(0, 1.0f);
        exact.Note(100, 0.5f);
        exact.Note(200, 0.96f);
        Check(exact.Note(300, 1.0f), "the return to 1.0 is stored with any step");
        Check(Near(exact.At(300), 1.0f) && Near(exact.At(250), 0.96f), "0.96 holds until the stored 1.0");

        // A ratio at or above 0.97 is real time when recorded.
        Track near;
        near.Note(0, 0.985f);
        Check(near.Samples().size() == 1 && Near(near.Samples()[0].scale, 1.0f), "a first sample of 0.985 is stored as 1.0");
        Check(!near.Note(17, 0.99f) && !near.Note(34, 1.003f), "real time noise is not a change");
        near.Note(50, 0.5f);
        Check(near.Note(60, 0.975f) && Near(near.Samples().back().scale, 1.0f), "0.975 after a slow motion stores 1.0");

        // Clips with stuck samples in the file: At reads them as real time.
        Track old;
        auto& samples = const_cast<std::vector<replay::timescale::Sample>&>(old.Samples());
        samples.push_back({0, 1.0f});
        samples.push_back({35, 0.58f});
        samples.push_back({303, 0.99f});
        Check(Near(old.At(30897), 1.0f), "an old clip stuck at 0.99 plays at 1.0");
        samples.back().scale = 0.98f;
        Check(Near(old.At(30897), 1.0f), "an old clip stuck at 0.98 plays at 1.0 (was rate 1.0204)");
        samples.back().scale = 0.9863f;
        Check(Near(old.At(30897), 1.0f), "the 015-Valentine stuck 0.9863 plays at 1.0 (was rate 1.0139)");
        samples.back().scale = 0.97f;
        Check(Near(old.At(30897), 1.0f), "0.97 is the threshold, read as 1.0");
        samples.back().scale = 0.96f;
        Check(Near(old.At(30897), 0.96f), "0.96 stays a slow down (under the threshold)");
        Check(Near(old.At(100), 0.58f), "the ramp itself is unchanged");

        // Real slow motion is untouched: Dead Eye, the weapon wheel, a kill
        // cam and a bench SET_TIME_SCALE.
        Track slow;
        slow.Note(0, 1.0f);
        slow.Note(1000, 0.35f);  // Dead Eye
        slow.Note(5000, 1.0f);
        slow.Note(9000, 0.10f);  // weapon wheel
        slow.Note(9500, 1.0f);
        slow.Note(12000, 0.061f); // kill cam
        slow.Note(14000, 0.66f);
        slow.Note(15000, 0.9f);
        Check(Near(slow.At(2000), 0.35f) && Near(slow.At(9200), 0.10f) && Near(slow.At(13000), 0.061f) &&
                Near(slow.At(14500), 0.66f) && Near(slow.At(15500), 0.9f),
            "Dead Eye, wheel, kill cam and a 0.9 slow down play as recorded");
        Check(Near(slow.At(6000), 1.0f) && Near(slow.At(10000), 1.0f), "real time between them");
        Check(Near(replay::timescale::Effective(0.9699f), 0.9699f) && Near(replay::timescale::Effective(0.97f), 1.0f),
            "Effective: the threshold is inclusive at 0.97");

        // The 2% step still filters noise inside a slow motion.
        Track noise;
        noise.Note(0, 0.5f);
        Check(!noise.Note(17, 0.51f) && noise.Note(34, 0.53f), "2% step under 0.97");
        // NaN and zero are clamped, not stored as NaN.
        Track bad;
        bad.Note(0, std::nanf(""));
        Check(Near(bad.Samples()[0].scale, replay::timescale::kMinScale), "NaN ratio clamps to the minimum");
    }

    void TestLogFormatScope()
    {
        using namespace replay::logfmt;
        std::ostringstream log;
        {
            const FormatScope scope(log);
            log << std::fixed << std::setprecision(1) << 1.25;
        }
        log << ',' << 1.019 << ',' << 712.4f;
        Check(log.str() == "1.2,1.019,712.4", "precision 1 is not left behind (was rate=1 and camera=7e+02)");

        std::ostringstream leaked;
        leaked << std::setprecision(1) << 2.0;
        DefaultFloat(leaked) << ',' << 1.0139 << ',' << 712.4f;
        Check(leaked.str() == "2,1.0139,712.4", "DefaultFloat gives back 6 digits whatever was left");

        std::ostringstream mark;
        {
            const FormatScope scope(mark);
            DefaultFloat(mark) << "rate=" << std::setprecision(4) << std::fixed << 1.0 / 0.981 << std::setprecision(1)
                               << ",camera=" << 712.43f << ';' << -1234.56f;
        }
        mark << ",x=" << 0.5;
        Check(mark.str() == "rate=1.0194,camera=712.4;-1234.6,x=0.5", "the REPORT_MARK rate and camera format");
    }

    void TestUnposedEpisodes()
    {
        using namespace replay::watch;
        const UnposedRule rule; // 500 ms and 24 ticks
        UnposedTracker tracker(rule);
        UnposedReport report;
        std::uint64_t wall = 1000;
        std::uint32_t playhead = 0;
        int reports = 0;
        // A tick: every body of `bodies` noted with its unposed state.
        const auto tick = [&](const std::vector<std::pair<std::uint32_t, bool>>& bodies, std::uint64_t stepMs = 17) {
            tracker.BeginTick();
            for (const auto& body : bodies)
            {
                if (tracker.Note(body.first, body.second, wall, playhead, report))
                    ++reports;
            }
            wall += stepMs;
            playhead += static_cast<std::uint32_t>(stepMs);
        };

        // Creation: 20 ticks (330 ms) unposed, then armed: transient.
        for (int i = 0; i < 20; ++i)
            tick({{1, true}});
        tick({{1, false}});
        Check(reports == 0, "a 20 tick (330 ms) hole at creation is not reported");
        Check(tracker.Stats().episodes == 1 && tracker.Stats().transient == 1 &&
                tracker.Stats().perCause[static_cast<int>(UnposedCause::Create)] == 1,
            "it counts as one transient create episode");
        Check(tracker.Stats().worstTransientTicks == 20 && tracker.Stats().worstTransientMs == 19 * 17,
            "the transient's length is kept for the summary");

        // Segment switch: posed, released for 6 ticks, armed again.
        for (int i = 0; i < 6; ++i)
            tick({{1, true}});
        tick({{1, false}});
        Check(reports == 0 && tracker.Stats().perCause[static_cast<int>(UnposedCause::Rearm)] == 1,
            "a re-arm hole after a segment switch is transient, cause rearm");

        // Many short holes over a playback: none reported.
        for (int round = 0; round < 30; ++round)
        {
            for (int i = 0; i < 3; ++i)
                tick({{100u + static_cast<std::uint32_t>(round), true}});
            tick({{100u + static_cast<std::uint32_t>(round), false}});
        }
        Check(reports == 0 && tracker.Stats().transient == 32, "30 short holes: 0 anomalies, 32 transient in all");

        // A body that never arms: reported once, past both 500 ms and 24 ticks.
        int before = reports;
        for (int i = 0; i < 23; ++i)
            tick({{7, true}}, 30); // 30 fps: 23 ticks are 660 ms, still under 24 ticks
        Check(reports == before, "under 24 ticks: not yet, even past 500 ms");
        tick({{7, true}}, 30);
        Check(reports == before + 1 && report.id == 7 && report.cause == UnposedCause::Create && report.ticks == 24 &&
                report.hiddenMs == 23 * 30,
            "the 24th tick past 500 ms reports the episode");
        for (int i = 0; i < 100; ++i)
            tick({{7, true}});
        Check(reports == before + 1 && tracker.Stats().persistent == 1, "reported once per episode");

        // At 60 fps a stuck body is reported at 500 ms (30 ticks), not 24.
        before = reports;
        for (int i = 0; i < 30; ++i)
            tick({{8, true}});
        Check(reports == before, "60 fps: 30 ticks (493 ms) are under 500 ms");
        tick({{8, true}});
        Check(reports == before + 1 && report.id == 8 && report.hiddenMs == 30 * 17, "60 fps: reported at 510 ms");

        // A body deleted while unposed (not noted any more): the episode
        // ends at its last tick.
        UnposedTracker gone(rule);
        for (int i = 0; i < 5; ++i)
        {
            gone.BeginTick();
            gone.Note(9, true, 100 + static_cast<std::uint64_t>(i) * 17, 0, report);
        }
        gone.BeginTick();
        gone.BeginTick();
        Check(gone.Open() == 0 && gone.Stats().transient == 1 && gone.Stats().worstTransientMs == 4 * 17,
            "a body not noted in a tick closes its episode at its last time");

        // A body unposed after it had been posed, then stuck: a real
        // re-arm failure is reported with cause rearm.
        UnposedTracker stuck(rule);
        std::uint64_t at = 0;
        stuck.BeginTick();
        stuck.Note(11, false, at, 0, report);
        int stuckReports = 0;
        for (int i = 0; i < 40; ++i)
        {
            at += 17;
            stuck.BeginTick();
            stuckReports += stuck.Note(11, true, at, 0, report) ? 1 : 0;
        }
        Check(stuckReports == 1 && report.cause == UnposedCause::Rearm &&
                stuck.Stats().persistentPerCause[static_cast<int>(UnposedCause::Rearm)] == 1,
            "a body that loses its pose for good is reported, cause rearm");
        stuck.Finish();
        Check(stuck.Open() == 0 && stuck.Stats().worstMs == 40 * 17 - 17 && stuck.Stats().transient == 0,
            "Finish closes it; a reported episode is not transient");

        // The anomaly line: one per episode, with its cause and length.
        replay::qa::Anomaly a;
        Check(replay::qa::ClassifyLine("POSE_V2_UNPOSED_HIDDEN,id=7:3,kind=ped,cause=create,ms=690,ticks=24,"
                                       "since_playhead_ms=100,camera_m=12,on_screen=1,playhead_ms=790",
                  0, a) &&
                a.kind == replay::qa::Kind::PuppetUnposed && a.entity == "7:3" && a.playheadMs == 790 &&
                a.detail == "hidden_unposed;cause=create;ms=690;ticks=24;on_screen=1",
            "the episode line classifies with cause, ms, ticks and on_screen");
        Check(!replay::qa::ClassifyLine("POSE_V2_UNPOSED_SUMMARY,episodes=40,transient=40,persistent=0", 0, a),
            "the summary is not an anomaly");
    }
}

int RunTimeScaleTests()
{
    g_timeScaleFailures = 0;
    TestTimeScaleReturnsToOne();
    TestLogFormatScope();
    TestUnposedEpisodes();
    if (g_timeScaleFailures == 0)
        std::printf("time scale tests: OK\n");
    return g_timeScaleFailures;
}
