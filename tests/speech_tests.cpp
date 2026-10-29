// Speech, pure parts: rule 2 in legacy mode leaves the speech's sound
// controller alone (puppet_audio_rule.h LegacyRefuseOwnPersistent); a created
// line that never became audible (speech_identity.h JudgeLine, SilentWindowMs,
// PlayheadJumped, LineCounts); the speech_silent anomaly of the QA ring
// (qa_health.h ClassifyLine).
#include "puppet_audio_rule.h"
#include "qa_health.h"
#include "speech_identity.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
    int g_failuresSpeech = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failuresSpeech;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    // The line's own sound is not refused in legacy; the rest of rule 2 holds.
    void TestLegacyExemption()
    {
        using replay::puppetaudio::LegacyRefuseOwnPersistent;
        using replay::puppetaudio::RefuseOwnPersistent;
        // The controller's 0x800004004366e8 at 0x2FA10CF on a silenced puppet,
        // no tracker, carried by the clip, outside any replay call (the line
        // data's prepare runs in the line update).
        Check(RefuseOwnPersistent(true, false, false, true, false, true), "before: rule 2 refused the line's sound");
        Check(!LegacyRefuseOwnPersistent(0x2FA10CFu, true, false, false, true, false, true),
            "legacy: the controller's sound at 0x2FA10CF (prepare, state 2) passes");
        Check(!LegacyRefuseOwnPersistent(0x2FA0DCCu, true, false, false, true, false, true),
            "legacy: the controller's sound at 0x2FA0DCC (state 3) passes");
        // Same answer as the packets mode for the same call sites.
        Check(!replay::puppetaudio::BlockPuppetCreate(true, true, false, false, 0x2FA10CFu),
            "packets: the same exemption");
        // The foley duplicates rule 2 was made for stay refused.
        Check(LegacyRefuseOwnPersistent(0x2588C2Fu, true, false, false, true, false, true),
            "legacy: a carried foley call site is still refused");
        Check(!LegacyRefuseOwnPersistent(0x2588C2Fu, true, false, false, true, false, false),
            "legacy: an uncarried call site passes as before");
        Check(!LegacyRefuseOwnPersistent(0x2588C2Fu, false, false, false, true, false, true),
            "legacy: switched off refuses nothing");
        Check(!LegacyRefuseOwnPersistent(0x2588C2Fu, true, true, false, true, false, true),
            "legacy: the replay's own create passes");
    }

    // The puppet's own speech request is refused at the core; the replay's
    // own requests and other peds pass.
    void TestPuppetOwnSpeech()
    {
        using replay::puppetaudio::RefusePuppetOwnSpeech;
        Check(RefusePuppetOwnSpeech(true, true, false), "own speech: a puppet's AI line is refused");
        Check(!RefusePuppetOwnSpeech(true, true, true), "own speech: the replay's line on the puppet passes");
        Check(!RefusePuppetOwnSpeech(true, false, false), "own speech: a world ped is not touched");
        Check(!RefusePuppetOwnSpeech(false, true, false), "own speech: switched off refuses nothing");
        Check(replay::puppetaudio::kPuppetOwnSpeechRefusal == 0x3F, "own speech: the core's own early refusal code");
    }

    replay::speech::LineWatch Watch()
    {
        replay::speech::LineWatch watch;
        watch.state = 2;
        watch.windowMs = replay::speech::SilentWindowMs(0);
        return watch;
    }

    // The judge.
    void TestJudge()
    {
        using namespace replay::speech;
        Check(SilentWindowMs(0) == kSilentWindowMinMs && SilentWindowMs(900) == kSilentWindowMinMs,
            "window: at least 1500 ms");
        Check(SilentWindowMs(1400) == 1900, "window: the measured load plus 500 ms");

        LineWatch watch = Watch();
        Check(JudgeLine(watch) == LineOutcome::Pending, "a fresh line loading is pending");
        watch.started = true;
        Check(JudgeLine(watch) == LineOutcome::Started, "the detour saw its start");
        watch = Watch();
        watch.state = kLineStateAudible;
        Check(JudgeLine(watch) == LineOutcome::Started, "state 5 on the poll is a start");
        // The R2 death: state 1 -> 2, stop reason 4 one or two frames later.
        watch = Watch();
        watch.listed = false;
        watch.audioAgeMs = 33;
        watch.playAgeMs = 33;
        Check(JudgeLine(watch) == LineOutcome::Died && Silent(JudgeLine(watch)), "killed while loading: silent at once");
        watch.started = true;
        Check(JudgeLine(watch) == LineOutcome::Started, "a line that started and ended is heard");
        // Still listed, no start.
        watch = Watch();
        watch.audioAgeMs = 1499;
        watch.playAgeMs = 1499;
        Check(JudgeLine(watch) == LineOutcome::Pending, "inside the window: pending");
        watch.audioAgeMs = 1500;
        watch.playAgeMs = 1500;
        Check(JudgeLine(watch) == LineOutcome::TimedOut && Silent(JudgeLine(watch)), "window gone by: silent");
        watch.playAgeMs = 200; // paused at 200 ms of playhead: the audio clock ran on
        Check(JudgeLine(watch) == LineOutcome::Pending, "paused: the playhead clock holds the judge");
        watch.playAgeMs = 1500;
        watch.normalPlay = false;
        Check(JudgeLine(watch) == LineOutcome::Pending, "paused or scrubbing: no timeout");
        watch.listed = false;
        Check(JudgeLine(watch) == LineOutcome::Abandoned && !Silent(JudgeLine(watch)),
            "left the list while paused: not judged");
        watch = Watch();
        watch.listed = false;
        watch.jumped = true;
        Check(JudgeLine(watch) == LineOutcome::Abandoned, "left the list across a jump: not judged");
        watch = Watch();
        watch.startObserved = false;
        watch.listed = false;
        Check(JudgeLine(watch) == LineOutcome::Pending, "no start detour: nothing judged");

        Check(PlayheadJumped(5000, 4000, 10), "the playhead went back");
        Check(!PlayheadJumped(5000, 5033, 33), "a tick of normal play");
        Check(PlayheadJumped(5000, 9000, 50), "a seek forward");
        Check(!PlayheadJumped(5000, 7000, 1000, 2.0), "double speed is not a jump");
    }

    void TestCounts()
    {
        using namespace replay::speech;
        LineCounts counts;
        // Humans created and dead, animals heard.
        for (int n = 0; n < 3; ++n)
        {
            counts.Created(false);
            counts.Judged(false, LineOutcome::Died);
        }
        counts.Created(false);
        counts.Judged(false, LineOutcome::TimedOut);
        counts.Created(true);
        counts.Judged(true, LineOutcome::Started);
        counts.Created(true);
        counts.Judged(true, LineOutcome::Died);
        ++counts.remade;
        counts.Created(true);
        counts.Judged(true, LineOutcome::Abandoned);
        const std::string text = counts.Text(2, true);
        Check(Has(text, ",speech_created=7,speech_started=1,speech_silent=5,"), "totals: created, started, silent");
        Check(Has(text, ",created_human=4,started_human=0,silent_human=4,killed_loading_human=3,"),
            "humans: started_human=0 shows the QA case");
        Check(Has(text, ",created_animal=3,started_animal=1,silent_animal=1,killed_loading_animal=1,"), "animals");
        Check(Has(text, ",silent_remade=1,started_remade=0,speech_unjudged=3,start_observer=1"),
            "remade, unjudged (abandoned + pending)");
        ++counts.startedRemade; // an animal line started on its retry
        Check(Has(counts.Text(0, true), ",started_remade=1,"), "remade: started on the retry's line");
        Check(text.front() == ',', "appended to the total line");
    }

    void TestAnomaly()
    {
        using namespace replay::qa;
        Check(std::string(KindText(Kind::SpeechSilent)) == "speech_silent", "kind name");
        Check(CountsText(Counts()).find("speech_silent=0") != std::string::npos, "in the summary counts");
        Anomaly anomaly;
        const std::string line =
            "POSE_V2_SPEECH_SILENT,ped=1:2,line=0x1a2b,kind=human,why=died,path=direct,voice_status=pinned_before,state=-1,"
            "stop_reason=4,stop_state=2,alive_ms=33,window_ms=1500,remade=0,context=0x5d3c,voice=0x53fbefa2,event_ms=50120,"
            "playhead_ms=49900";
        Check(ClassifyLine(line, 0, anomaly), "the silent line is an anomaly");
        Check(anomaly.kind == Kind::SpeechSilent && anomaly.entity == "1:2" && anomaly.playheadMs == 49900,
            "kind, entity and playhead");
        Check(anomaly.detail ==
                "kind=human;why=died;voice_status=pinned_before;path=direct;stop_reason=4;stop_state=2;context=0x5d3c;voice=0x53fbefa2",
            "the whole detail, nothing cut");
        // The total line is not an anomaly.
        Check(!ClassifyLine("POSE_V2_PLAYBACK_SPEECH_TOTAL,recorded=3,speech_silent=2", 0, anomaly), "the total is not");
    }
}

int RunSpeechTests()
{
    g_failuresSpeech = 0;
    TestLegacyExemption();
    TestPuppetOwnSpeech();
    TestJudge();
    TestCounts();
    TestAnomaly();
    if (g_failuresSpeech == 0)
        std::puts("Speech (legacy exemption, silent lines) tests passed.");
    return g_failuresSpeech;
}
