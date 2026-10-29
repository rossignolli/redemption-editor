// Tests:
//   speech after a jump (speech_resume.h): which line each speaker is inside
//   of at a playhead, its start offset, the end of a line from a clip with
//   or without SPCH v2, overlapping lines of one speaker, the jump watch;
//   the silent and cut causes of the QA report (qa_health.h);
//   the view lent to another mod's camera (camera_cut.h);
//   the other ASIs' threads given back after the theater's pause
//   (world_theatre.h).
#include "speech_resume.h"
#include "camera_cut.h"
#include "qa_health.h"
#include "world_theatre.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_failuresSpeechSeek = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failuresSpeechSeek;
    }

    using replay::entity::ReplayId;
    using replay::entity::SpeechEvent;
    namespace resume = replay::speech::resume;

    SpeechEvent Line(std::uint16_t speaker, std::uint32_t startMs, std::uint32_t endMs, std::uint32_t stopMs = 0)
    {
        SpeechEvent line;
        line.speaker = ReplayId{speaker, 1};
        line.timeMs = startMs;
        line.endMs = endMs;
        line.stopMs = stopMs;
        line.contextPartialHash = 0x1000u + startMs;
        return line;
    }

    const resume::Active* For(const std::vector<resume::Active>& active, std::size_t index)
    {
        for (const resume::Active& a : active)
            if (a.index == index)
                return &a;
        return nullptr;
    }

    // Pearson's campfire story: long lines of one speaker, a second speaker
    // answering in between.
    void TestActiveLines()
    {
        std::vector<SpeechEvent> events = {
            Line(7, 1000, 9000),   // 0: Pearson, 8 s
            Line(9, 9500, 11000),  // 1: Arthur answers
            Line(7, 12000, 20000), // 2: Pearson again
        };
        std::vector<resume::Active> active;
        const std::vector<std::uint32_t> noLead;
        const std::vector<std::uint32_t> noLearned;

        resume::ActiveAt(events, 4000, noLead, noLearned, active);
        Check(active.size() == 1 && active[0].index == 0, "inside the first line: it is active");
        Check(active.size() == 1 && active[0].offsetMs == 3000, "offset = playhead - recorded start");
        Check(active.size() == 1 && active[0].remainingMs == 5000 && active[0].end.known, "remaining to the recorded end");

        resume::ActiveAt(events, 1040, noLead, noLearned, active);
        Check(active.size() == 1 && active[0].offsetMs == 0, "under 66 ms the line plays from its start (reference)");

        resume::ActiveAt(events, 8800, noLead, noLearned, active);
        Check(active.empty(), "less than 250 ms left: not said again");

        resume::ActiveAt(events, 9200, noLead, noLearned, active);
        Check(active.empty(), "between lines: nothing");

        resume::ActiveAt(events, 10000, noLead, noLearned, active);
        Check(active.size() == 1 && active[0].index == 1 && active[0].offsetMs == 500, "the other speaker's line");

        resume::ActiveAt(events, 25000, noLead, noLearned, active);
        Check(active.empty(), "after the last end: nothing");
    }

    void TestOverlapSameSpeaker()
    {
        // The engine runs one line per speaker: the later start replaced the
        // earlier one (its recorded end may still be later).
        std::vector<SpeechEvent> events = {
            Line(7, 1000, 6000),
            Line(7, 3000, 5000),
            Line(8, 2000, 7000),
        };
        std::vector<resume::Active> active;
        resume::ActiveAt(events, 4000, {}, {}, active);
        Check(active.size() == 2, "one line per speaker");
        Check(For(active, 1) && For(active, 1)->offsetMs == 1000, "speaker 7: the later start wins");
        Check(!For(active, 0), "speaker 7: the replaced line is not said again");
        Check(For(active, 2) && For(active, 2)->offsetMs == 2000, "speaker 8 at the same time");

        resume::ActiveAt(events, 5500, {}, {}, active);
        Check(For(active, 0) && For(active, 0)->offsetMs == 4500, "after the later line ended the earlier one is the speaker's");
    }

    void TestLeadWindow()
    {
        // The playhead landed between the request (start minus the load
        // lead) and the audible start: asked now, from its start.
        std::vector<SpeechEvent> events = {Line(7, 5000, 8000)};
        std::vector<resume::Active> active;
        resume::ActiveAt(events, 4800, {300}, {}, active);
        Check(active.size() == 1 && active[0].offsetMs == 0, "inside the lead: asked from the start");
        resume::ActiveAt(events, 4600, {300}, {}, active);
        Check(active.empty(), "before the request: the cursor asks it");
    }

    void TestEndSources()
    {
        // A clip from before SPCH v2: no natural end.
        SpeechEvent old = Line(7, 1000, 0);
        resume::LineEnd end = resume::EndOf(old, 0, 4000);
        Check(!end.known && end.endMs == 4000 && std::string(end.source) == "next_line", "old clip: up to the speaker's next line");
        end = resume::EndOf(old, 0, 0);
        Check(!end.known && end.endMs == 1000 + resume::kUnknownEndCapMs && std::string(end.source) == "cap", "old clip: capped");
        end = resume::EndOf(old, 0, 1000 + resume::kUnknownEndCapMs + 5000);
        Check(std::string(end.source) == "cap", "a far next line does not stretch past the cap");
        end = resume::EndOf(old, 6200, 9000);
        Check(end.known && end.endMs == 6200 && std::string(end.source) == "learned", "the session heard its end");
        SpeechEvent stopped = Line(7, 1000, 0, 2500);
        end = resume::EndOf(stopped, 0, 0);
        Check(end.known && end.endMs == 2500 && std::string(end.source) == "stop", "an early stop ends it");
        SpeechEvent v2 = Line(7, 1000, 3300);
        end = resume::EndOf(v2, 9999, 0);
        Check(end.known && end.endMs == 3300 && std::string(end.source) == "recorded", "SPCH v2: the recorded end wins");

        // An old clip with the learned end, through ActiveAt.
        std::vector<SpeechEvent> events = {Line(7, 1000, 0), Line(7, 20000, 0)};
        std::vector<resume::Active> active;
        resume::ActiveAt(events, 7000, {}, {6200, 0}, active);
        Check(active.empty(), "learned end: past it nothing is said");
        resume::ActiveAt(events, 7000, {}, {}, active);
        Check(active.size() == 1 && active[0].index == 0 && !active[0].end.known, "unknown end: asked (the engine ends it)");
    }

    void TestWatch()
    {
        resume::Watch watch;
        using resume::Step;
        using resume::Why;
        Check(watch.Update(true, 1000, 0, 1.0) == Step::Resume && watch.LastWhy() == Why::First, "first tick: resume");
        Check(watch.Update(true, 1033, 33000, 1.0) == Step::Continue, "normal play");
        // A stalled frame: the clock went 400 ms, the playhead 200 (clamped).
        Check(watch.Update(true, 1233, 433000, 1.0) == Step::Continue, "a lagging playhead is not a jump");
        Check(watch.Update(false, 1233, 466000, 1.0) == Step::Leave, "pause: leave (stop the lines)");
        Check(watch.Update(false, 1233, 500000, 1.0) == Step::Idle, "paused: idle");
        Check(watch.Update(false, 900, 533000, 1.0) == Step::Idle, "a frame step while paused: idle");
        Check(watch.Update(true, 900, 566000, 1.0) == Step::Resume && watch.LastWhy() == Why::Play, "play again: resume");
        Check(watch.Update(true, 400, 600000, 1.0) == Step::Resume && watch.LastWhy() == Why::Back, "back in normal play");
        Check(watch.Update(true, 5000, 633000, 1.0) == Step::Resume && watch.LastWhy() == Why::Ahead, "a seek ahead");
        Check(watch.Update(true, 5066, 666000, 2.0) == Step::Continue, "x2: twice the clock is normal");
        watch.Reset();
        Check(watch.Update(false, 0, 0, 1.0) == Step::Idle, "a playback that starts paused: idle, then resume");
        Check(watch.Update(true, 0, 33000, 1.0) == Step::Resume, "...resume on play");
    }

    void TestCauses()
    {
        resume::StopFacts e1;
        e1.found = true;
        e1.reason = 4;
        e1.fieldsRead = true;
        e1.slot = true;
        e1.soundFlagsEE = 4;
        Check(std::string(resume::SilentCause(false, e1)) == "e1_wave_missing", "E1: wave missing in the slot");
        resume::StopFacts e2 = e1;
        e2.slot = false;
        e2.soundFlagsEE = 0;
        e2.dataType = 2;
        Check(std::string(resume::SilentCause(false, e2)) == "e2_no_slot", "E2: no wave slot");
        e2.dataType = 3;
        Check(std::string(resume::SilentCause(false, e2)) == "killed_loading", "type 3 needs no slot");
        resume::StopFacts reval;
        reval.found = true;
        reval.reason = 6;
        Check(std::string(resume::SilentCause(false, reval)) == "revalidation", "state 3 refusal");
        Check(std::string(resume::SilentCause(true, reval)) == "timeout", "timeout first");
        Check(std::string(resume::SilentCause(false, resume::StopFacts{})) == "no_stop", "no stop seen");

        Check(resume::Cut(true, 3, false), "an audible line stopped by the engine is cut");
        Check(!resume::Cut(true, 1, false), "the natural end is not a cut");
        Check(!resume::Cut(true, 2, true), "the replay's own stop is not a cut");
        Check(resume::EndedShort(1000, 4000) && !resume::EndedShort(3700, 4000) && !resume::EndedShort(100, 0),
            "a natural end well before the recorded end is short");
    }

    void TestQaLines()
    {
        using namespace replay::qa;
        Check(std::string(KindText(Kind::SpeechCut)) == "speech_cut", "speech_cut kind name");
        Anomaly anomaly;
        Check(ClassifyLine("POSE_V2_SPEECH_CUT,ped=7:1,line=0x1,kind=cut,reason=3,state=5,callers=0x0/0x0,heard_ms=2100,"
                           "expected_ms=6000,end_source=recorded,path=direct,resumed=0,offset_ms=0,normal=1,context=0x5d,"
                           "event_ms=12000,playhead_ms=14100",
                  0, anomaly),
            "a cut line is an anomaly");
        Check(anomaly.kind == Kind::SpeechCut && anomaly.entity == "7:1" && anomaly.playheadMs == 14100 &&
                anomaly.detail == "kind=cut;reason=3;heard_ms=2100;expected_ms=6000;path=direct;context=0x5d",
            "cut: kind, entity, detail");
        Check(ClassifyLine("POSE_V2_SPEECH_SILENT,ped=1:2,line=0x1,kind=human,why=died,cause=e2_no_slot,resumed=0,offset_ms=0,"
                           "path=direct,voice_status=pinned_before,state=-1,stop_reason=4,stop_state=2,context=0x5d,voice=0x53,"
                           "event_ms=50120,playhead_ms=49900",
                  0, anomaly) &&
                anomaly.detail.find("cause=e2_no_slot") != std::string::npos,
            "silent: the cause reaches the QA report");
    }

    void TestCameraRelease()
    {
        using namespace replay::camcut;
        Check(ReleaseFromIni(nullptr) == Release::Paused && ReleaseFromIni("") == Release::Paused, "default: paused");
        Check(ReleaseFromIni("OFF") == Release::Off && ReleaseFromIni("always") == Release::Always, "ini values");
        Check(std::string(ReleaseName(Release::Always)) == "always", "name");
        Check(LendView(Release::Paused, true, true), "paused and another camera renders: lent");
        Check(!LendView(Release::Paused, false, true), "playing: taken back");
        Check(LendView(Release::Always, false, true), "always: lent while playing too");
        Check(!LendView(Release::Off, true, true), "off: never lent");
        Check(!LendView(Release::Always, true, false), "no other camera: nothing to lend");
    }

    void TestScriptHookThreads()
    {
        using namespace replay::theatre;
        Check(Joaat("audiotest") == kScriptHookThreadHash, "ScriptHook's thread hash");
        ThreadView camera;
        camera.id = 71;
        camera.scriptHash = kScriptHookThreadHash;
        camera.paused = 1;
        camera.pausable = 1;
        Check(UnpauseForScriptHook(camera, false), "the camera mod's thread runs again");
        Check(!UnpauseForScriptHook(camera, true), "TheatrePauseScriptHook=1: it stays paused");
        ThreadView game = camera;
        game.scriptHash = Joaat("ambient_camp");
        Check(!UnpauseForScriptHook(game, false), "a game script stays paused");
        ThreadView idle = camera;
        idle.paused = 0;
        Check(!UnpauseForScriptHook(idle, false), "a thread the native left alone: nothing to write");
        ThreadView empty = camera;
        empty.id = 0;
        Check(!UnpauseForScriptHook(empty, false), "a free slot: nothing");
        Config config;
        Check(!config.pauseScriptHook, "default: the other ASIs are not paused");
    }
}

int RunSpeechSeekTests()
{
    g_failuresSpeechSeek = 0;
    TestActiveLines();
    TestOverlapSameSpeaker();
    TestLeadWindow();
    TestEndSources();
    TestWatch();
    TestCauses();
    TestQaLines();
    TestCameraRelease();
    TestScriptHookThreads();
    if (g_failuresSpeechSeek == 0)
        std::puts("Speech after a jump, camera release, ScriptHook threads tests passed.");
    return g_failuresSpeechSeek;
}
