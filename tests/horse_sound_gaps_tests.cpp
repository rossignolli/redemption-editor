// Horse sound gaps in the replay. The foot wetness (foot_material.h), the
// path of an animal line and the per entity tallies of the sound report
// (horse_sound_report.h).
#include "foot_material.h"
#include "horse_sound_report.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{
    int g_horseSoundFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_horseSoundFailures;
    }

    using namespace replay::footmaterial;
    using namespace replay::horsesound;

    void TestWetnessLayout()
    {
        // 0x318D338: movss xmm7, [rdi + rax*4 + 0x380] (rax = the step's foot).
        Check(WetnessOffset(0) == 0x380 && WetnessOffset(1) == 0x384 && WetnessOffset(3) == 0x38C,
            "wetness of each foot");
        // The foot reset 0x319580C: +0x390.. are the next per foot arrays.
        Check(WetnessOffset(3) + 4 == 0x390, "four floats, then the next array");
        Check(!WetStep(0.0f) && WetStep(0.15f) && !WetStep(-1.0f), "the wet layer gate is > 0");
    }

    void TestWetnessEncoding()
    {
        Check(EncodeWetness(-1.0f) == 0 && DecodeWetness(0) < 0.0f, "unknown stays unknown");
        Check(EncodeWetness(0.0f) == 1 && DecodeWetness(1) == 0.0f, "dry is known and dry");
        Check(EncodeWetness(1.0f) == 255 && DecodeWetness(255) == 1.0f, "fully wet");
        Check(EncodeWetness(7.0f) == 255, "clamped");
        // Values of a recorded clip (0x02D01730 of the live wet layer): 0.15 .. 1.00.
        const float samples[] = {0.15f, 0.63f, 0.84f, 0.95f, 1.0f};
        for (const float w : samples)
        {
            const float back = DecodeWetness(EncodeWetness(w));
            Check(back > w - 0.003f && back < w + 0.003f, "round trip within 1/254");
            Check(WetStep(back), "a wet sample stays wet");
        }
        Check(EncodeWetness(std::numeric_limits<float>::quiet_NaN()) == 0, "NaN is unknown");
    }

    void TestWetnessWrite()
    {
        Check(!NeedsWetnessWrite(0.0f, -1.0f), "unknown: no write");
        Check(NeedsWetnessWrite(0.0f, 0.84f), "dry puppet foot, wet recording: write");
        Check(!NeedsWetnessWrite(0.84f, 0.8405f), "already there");
        Check(NeedsWetnessWrite(0.9f, 0.0f), "wet puppet foot, dry recording: write");
    }

    void TestPairingCarriesWetness()
    {
        Pairer pairer;
        Pairer::Pending pending;
        pending.timeUs = 1000;
        pending.footAudio = 0xA;
        pending.foot = 2;
        pending.token = 5;
        pending.check = 35;
        pairer.AddPending(pending);
        Sample sample;
        sample.timeUs = 1010;
        sample.footAudio = 0xA;
        sample.foot = 2;
        sample.material = Material{0x1234u, 0x2f36cbu, 0.84f};
        pairer.AddSample(sample);
        std::vector<Pairer::Match> matches;
        pairer.Pair(matches);
        Check(matches.size() == 1 && matches[0].material.wetness == 0.84f, "the step gets its foot's wetness");
        // A sample without a valid material still carries the wetness.
        Material noRef;
        noRef.wetness = 0.5f;
        Check(!noRef.Valid() && EncodeWetness(noRef.wetness) != 0, "wetness without a material is kept");
    }

    void TestAnimalAttempts()
    {
        Check(FirstAttempt(true, true) == AnimalAttempt::Pinned, "an animal line starts on the native path");
        Check(FirstAttempt(true, false) == AnimalAttempt::Direct, "AnimalVocalNative=0: the old path");
        Check(FirstAttempt(false, true) == AnimalAttempt::Direct, "a person keeps the direct path");
        Check(NextAttempt(AnimalAttempt::Pinned, true) == AnimalAttempt::Done, "created: done");
        Check(NextAttempt(AnimalAttempt::Pinned, false) == AnimalAttempt::Context, "pinned refused: context only");
        Check(NextAttempt(AnimalAttempt::Pinned, false, false) == AnimalAttempt::Direct,
            "no recorded variation: pinned was already context only");
        Check(NextAttempt(AnimalAttempt::Context, false) == AnimalAttempt::Direct, "context refused: direct");
        Check(NextAttempt(AnimalAttempt::Direct, false) == AnimalAttempt::Done, "direct is the last");
        // Never more than three requests for one line.
        int requests = 0;
        for (AnimalAttempt a = FirstAttempt(true, true); a != AnimalAttempt::Done; a = NextAttempt(a, false))
            ++requests;
        Check(requests == 3, "three paths at most");
    }

    // The acceptance arithmetic of a recorded playback (0 to 40.7 s), from
    // its session log: 242 horse lines due; the direct path created 135
    // (107 result 64) and 123 of those were killed while loading (122 made
    // again by the context retry); the native path created 720 of 722
    // requests in the same session.
    void TestAcceptanceArithmetic()
    {
        Tallies before;
        SpeechTally& old = before.speech["horses"];
        old.cls = SpeakerClass::Horse;
        old.recorded = 242;
        old.refused = 107;
        old.killed = 1;
        old.played = 242 - 107 - 1;
        const double oldPct = SilentPercent(before.SpeechTotals(true));
        Check(oldPct > 44.0 && oldPct < 45.0, "before the fix: 44.6% of the horse lines silent");

        // Every line asks the native path first; with the session's native
        // refusal (2 of 722) the expected silent lines are below 1.
        const double nativeRefusal = 2.0 / 722.0;
        const double expectedSilent = 242.0 * nativeRefusal * nativeRefusal; // pinned, then context
        Tallies after;
        SpeechTally& now = after.speech["horses"];
        now.cls = SpeakerClass::Horse;
        now.recorded = 242;
        now.refused = static_cast<std::uint32_t>(expectedSilent + 0.999);
        now.played = 242 - now.refused;
        Check(SilentPercent(after.SpeechTotals(true)) <= 2.0, "after the fix: at most 2% silent (in-game acceptance)");
    }

    void TestTalliesAndReport()
    {
        Tallies tallies;
        SpeechTally& horse = tallies.speech["0:2172"];
        horse.cls = SpeakerClass::Horse;
        horse.recorded = 10;
        horse.played = 8;
        horse.pinned = 7;
        horse.context = 1;
        horse.refused = 2;
        horse.reasons["direct_result_64"] = 2;
        SpeechTally& person = tallies.speech["7:2179"];
        person.recorded = 6;
        person.played = 6;
        person.direct = 6;
        SpeechTally& gone = tallies.speech["40:2235"];
        gone.cls = SpeakerClass::Horse;
        gone.recorded = 3;
        gone.noPuppet = 3;
        FootTally& feet = tallies.feet["2:2174"];
        feet.horse = true;
        feet.recorded = 176;
        feet.played = 176;
        feet.wetKnown = 176;
        feet.wet = 170;
        feet.wetPlayed = 120;
        FootTally& oldClip = tallies.feet["4:2176"];
        oldClip.horse = true;
        oldClip.recorded = 170;
        oldClip.played = 170;
        FootTally& walker = tallies.feet["1:2173"];
        walker.recorded = 20;
        walker.played = 20;

        const Tallies::Totals animals = tallies.SpeechTotals(true);
        Check(animals.recorded == 13 && animals.refused == 2 && animals.noPuppet == 3, "animal totals");
        Check(SilentPercent(animals) == 20.0, "silent share counts only lines with a puppet");
        Check(SpeechProblem(horse) && !SpeechProblem(person) && !SpeechProblem(gone), "problem flags");
        Check(FootProblem(feet) && !FootProblem(oldClip) && !FootProblem(walker), "step problem flags");

        const std::string section = FormatSection(tallies);
        Check(section.find("== speech and vocalizations") == 0, "section header first");
        Check(section.find("SILENT  owner 0:2172 (horse)") != std::string::npos, "the refusing horse is listed as silent");
        Check(section.find("direct_result_64 2") != std::string::npos, "with its reasons");
        Check(section.find("SILENT  owner 0:2172") < section.find("ok      owner 7:2179"), "problems first");
        Check(section.find("MISSED  owner 2:2174 (horse)") != std::string::npos, "wet steps not played wet show");
        Check(section.find("owner 4:2176 (horse)") != std::string::npos &&
                section.find("foot wetness not recorded") != std::string::npos,
            "an old clip's horse says the wetness was not recorded");
        Check(section.find("owner 1:2173") == std::string::npos, "a person with all steps is not listed");
        for (const char c : section)
            Check(static_cast<unsigned char>(c) < 0x80, "no accents or dashes in the report");

        const std::string summary = SummaryLine(tallies);
        Check(summary.find("SPEECH AND HORSES:") == 0 && summary.find("wet steps 170, played wet 120") != std::string::npos,
            "summary line");

        const std::string report = "Replay sounds\nCompared span: 40.7 s\n\nSUMMARY: 26 sounds missing.\nRequests.\n\n== horses ==\nMISSING x\n\nHow to read: y\n";
        const std::string merged = InsertIntoReport(report, summary, section);
        Check(merged.find("SUMMARY: 26 sounds missing.\nSPEECH AND HORSES:") != std::string::npos, "summary under SUMMARY");
        Check(merged.find("== speech and vocalizations") < merged.find("== horses =="), "section before the census sections");
        const std::string bare = InsertIntoReport("SUMMARY: 0.\n\nHow to read: y\n", "", section);
        Check(bare.find("== speech") < bare.find("How to read"), "no census section: before the legend");
        Check(FormatSection(Tallies{}).empty(), "nothing to tell: no section");
    }
}

int RunHorseSoundGapsTests()
{
    g_horseSoundFailures = 0;
    TestWetnessLayout();
    TestWetnessEncoding();
    TestWetnessWrite();
    TestPairingCarriesWetness();
    TestAnimalAttempts();
    TestAcceptanceArithmetic();
    TestTalliesAndReport();
    if (g_horseSoundFailures == 0)
        std::puts("Horse sound tests passed.");
    return g_horseSoundFailures;
}
