// The puppet blink detector (puppet_blink.h): the per-tick classification,
// the grace of a body never shown, the episodes and what counts.
#include "puppet_blink.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_blinkFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_blinkFailures;
    }

    using namespace replay::blink;

    Frame Drawn()
    {
        Frame f;
        f.alive = f.recordedVisible = f.inTrack = true;
        f.ready = f.shown = f.armed = true;
        f.writesKnown = true;
        f.writes = 2;
        return f;
    }

    void TestClassify()
    {
        Frame f = Drawn();
        Check(Classify(f) == Kind::None, "a body drawn, armed and written is no blink");

        Frame hidden = f;
        hidden.shown = false;
        Check(Classify(hidden) == Kind::Hidden, "ready and recorded visible but hidden by the mod: hidden");
        Frame unposed = f;
        unposed.armed = false;
        Check(Classify(unposed) == Kind::Unposed, "shown without an override: unposed");
        Frame bind = f;
        bind.writes = 0;
        bind.mismatches = 1;
        Check(Classify(bind) == Kind::Bind && BindCause(bind) == Cause::BoneMismatch,
            "the detour refused on a bone mismatch: bind, bone_mismatch");
        bind.mismatches = 0;
        Check(Classify(bind) == Kind::Bind && BindCause(bind) == Cause::NoWrite, "no write at all: bind, no_write");
        bind.writesKnown = false;
        Check(Classify(bind) == Kind::None, "no baseline (first tick, paused, other slot): not judged");
        Frame missing = f;
        missing.ready = false;
        missing.shown = false;
        Check(Classify(missing) == Kind::Missing, "no puppet for a recorded visible body: missing");

        // Legitimately not drawn.
        Frame legit = hidden;
        legit.recordedVisible = false;
        Check(Classify(legit) == Kind::None, "recorded hidden (a cutscene, a mission hide)");
        legit = hidden;
        legit.alive = false;
        Check(Classify(legit) == Kind::None, "not alive at the playhead");
        legit = hidden;
        legit.inTrack = false;
        Check(Classify(legit) == Kind::None, "outside its pose track");
        legit = hidden;
        legit.intentional = true;
        Check(Classify(legit) == Kind::None, "the never animated hide");
    }

    void TestCauses()
    {
        ArmState s;
        Check(ArmCause(s) == Cause::Create, "never shown, nothing else: create");
        s.shownBefore = true;
        Check(ArmCause(s) == Cause::Rearm, "shown before: rearm");
        s.layerRetry = true;
        Check(ArmCause(s) == Cause::LayerRetry, "a layer retry");
        s.recordChanged = true;
        Check(ArmCause(s) == Cause::Segment, "the record changed this tick: segment");
        s.prefixHold = true;
        Check(ArmCause(s) == Cause::PrefixHold, "a gore prefix hold");
        s.armRefused = true;
        Check(ArmCause(s) == Cause::ArmRefused, "arm given up");
        s.sampled = false;
        Check(ArmCause(s) == Cause::NoSample, "no sample wins");
        Check(std::string(CauseText(Cause::LayerRetry)) == "layer_retry" && std::string(KindText(Kind::Bind)) == "bind",
            "names");
    }

    void TestBaseline()
    {
        WriteBaseline b;
        std::uint64_t dw = 0, dm = 0;
        Check(!b.Delta(3, 10, 0, dw, dm), "no baseline yet");
        b.Keep(3, true, 10, 0);
        Check(b.Delta(3, 11, 0, dw, dm) && dw == 1 && dm == 0, "one write since");
        Check(!b.Delta(4, 11, 0, dw, dm), "another slot");
        Check(!b.Delta(3, 5, 0, dw, dm), "the counters went back (slot reset)");
        b.Keep(3, false, 10, 0);
        Check(!b.Delta(3, 10, 0, dw, dm), "hidden last tick: the frame drew nothing to judge");
    }

    void TestEpisodes()
    {
        Tracker tracker;
        std::vector<Episode> closed;
        const Where near{12.0f, true};
        const Where far{140.0f, false};
        Frame drawn = Drawn();
        Frame hidden = drawn;
        hidden.shown = false;

        // Start of a stretch: body 7 shown, then hidden for 3 ticks (the gore blink), then shown.
        std::uint64_t wall = 1000;
        std::uint32_t play = 5000;
        tracker.BeginTick(play);
        Check(tracker.Judge(7, drawn, play, 0) == Kind::None, "shown");
        Check(tracker.ShownBefore(7), "remembered as shown");
        tracker.EndTick(wall, closed);
        for (int i = 0; i < 3; ++i)
        {
            wall += 16;
            play += 16;
            tracker.BeginTick(play);
            const Kind kind = tracker.Judge(7, hidden, play, 0);
            Check(kind == Kind::Hidden, "shown before: hidden at once");
            Check(tracker.WantsWhere(7, kind) == (i == 0), "the camera read on the episode's first tick only");
            tracker.Note(7, 0xABCu, kind, Cause::Segment, play, wall, i == 0 ? &near : nullptr, closed);
            tracker.EndTick(wall, closed);
        }
        Check(closed.empty() && tracker.Open() == 1, "open while it lasts");
        wall += 16;
        play += 16;
        tracker.BeginTick(play);
        Check(tracker.Judge(7, drawn, play, 0) == Kind::None, "shown again");
        tracker.Clear(7, wall, closed);
        tracker.EndTick(wall, closed);
        Check(closed.size() == 1, "one episode");
        const Episode& e = closed[0];
        Check(e.kind == Kind::Hidden && e.cause == Cause::Segment && e.frames == 3 && e.fromMs == 5016 && e.toMs == 5048,
            "kind, cause, frames, playhead span");
        Check(e.wallMs == 48 && e.onScreen && e.cameraM == 12.0f && e.counts && !e.cut, "wall time, where, counted");

        // A far off screen blink is logged, not counted.
        closed.clear();
        tracker.BeginTick(play);
        tracker.Note(8, 0, Kind::Hidden, Cause::Rearm, play, wall, &far, closed);
        tracker.EndTick(wall, closed);
        tracker.BeginTick(play);
        tracker.EndTick(wall + 16, closed); // 8 not noted: deleted, the episode ends
        Check(closed.size() == 1 && !closed[0].counts && closed[0].frames == 1, "far: not counted");

        // A no_write single frame does not count, two do.
        closed.clear();
        tracker.BeginTick(play);
        tracker.Note(9, 0, Kind::Bind, Cause::NoWrite, play, wall, &near, closed);
        tracker.Clear(9, wall, closed);
        Check(closed.size() == 1 && !closed[0].counts, "one frame without a write: the previous pose, not counted");
        Episode two = closed[0];
        two.frames = 2;
        Check(Counts(two, tracker.Settings()), "two frames without a write count");
        two.cause = Cause::BoneMismatch;
        two.frames = 1;
        Check(Counts(two, tracker.Settings()), "one refused write counts");

        // Another kind ends the episode and begins a new one.
        closed.clear();
        tracker.BeginTick(play);
        tracker.Note(10, 0, Kind::Unposed, Cause::Rearm, play, wall, &near, closed);
        tracker.Note(10, 0, Kind::Hidden, Cause::Rearm, play, wall, nullptr, closed);
        Check(closed.size() == 1 && closed[0].kind == Kind::Unposed && tracker.Open() == 1, "kind change");

        const Summary& s = tracker.Stats();
        Check(s.episodes == 4 && s.counted == 2, "summary totals");
        Check(s.perKind[static_cast<int>(Kind::Hidden)] == 2 && s.countedPerKind[static_cast<int>(Kind::Hidden)] == 1,
            "per kind");
        Check(!s.worst.empty() && s.worst[0].counts && s.worst[0].frames == 3, "the worst: counted, longest first");
        const std::string text = WorstText(s.worst, [](std::uint32_t id) { return std::to_string(id); });
        Check(text.rfind("7/hidden/segment/3/on/counted", 0) == 0, "worst text");

        // Suspend: cut, and every body gets its grace again.
        closed.clear();
        tracker.Suspend(wall, closed);
        Check(closed.size() == 1 && closed[0].cut && tracker.Open() == 0 && !tracker.Active(), "suspend cuts");
        Check(!tracker.ShownBefore(7), "the stretch forgets who was shown");
    }

    void TestGrace()
    {
        Tracker tracker;
        Frame missing;
        missing.alive = missing.recordedVisible = missing.inTrack = true;
        tracker.BeginTick(20000);
        Check(tracker.Judge(1, missing, 20000, 0) == Kind::None, "never shown, stretch just began: grace");
        Check(tracker.Judge(1, missing, 21499, 0) == Kind::None, "still in the grace");
        Check(tracker.Judge(1, missing, 21500, 0) == Kind::Missing, "past the grace: missing");
        Check(tracker.Judge(2, missing, 21500, 21000) == Kind::None, "entered the recording 500 ms ago: grace");
        Check(tracker.Judge(2, missing, 22600, 21000) == Kind::Missing, "1.6 s after its entry");
        Frame shown = missing;
        shown.ready = shown.shown = shown.armed = true;
        Check(tracker.Judge(3, shown, 21500, 0) == Kind::None, "3 shown");
        Check(tracker.Judge(3, missing, 21516, 0) == Kind::Missing, "3 deleted after it showed: missing at once");
        Check(tracker.Stats().judged == 7, "body ticks judged");
    }
}

int RunPuppetBlinkTests()
{
    TestClassify();
    TestCauses();
    TestBaseline();
    TestEpisodes();
    TestGrace();
    if (g_blinkFailures == 0)
        std::puts("Puppet blink tests passed.");
    return g_blinkFailures;
}
