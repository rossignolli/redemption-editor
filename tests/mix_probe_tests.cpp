// The mix probe's pure part (mix_probe_core.h). Kinds from the owner and the
// callers, distances and the engine metric, the clip track's trim and cut,
// the sample and line budgets, the pass totals, the coverage, and the
// comparison's verdicts in a weak replay (camera far, player bit lost on the
// hooves, NPC shot variants, LOD up, music stopped) and in a faithful one.
#include "mix_probe_core.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::mixprobe;

    int g_mixFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_mixFailures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    void TestKinds()
    {
        const std::uint32_t step[3] = {0x02516D2Fu, 0x0318E828u, 0x01000000u};
        const std::uint32_t movement[2] = {0x03193D20u, 0x0318E828u};
        const std::uint32_t speech[1] = {0x02FA10CFu - 0x100u};
        const std::uint32_t none[2] = {0x01000000u, 0x01000010u};
        Check(KindOf(true, false, 0x1234, none, 2) == Kind::Shot, "kind: the weapon's audio is a shot");
        Check(KindOf(false, true, 0x80000600000000ull | replay::soundowner::kWetHoofSet, none, 2) == Kind::HorseStep,
            "kind: the wet hoof set of a horse is a horse step");
        Check(KindOf(false, false, 0x80000c00000000ull | replay::soundowner::kWetHoofSet, none, 2) == Kind::Footstep,
            "kind: the wet hoof set of a person is a step (recorded peds 27:107 and 32:112)");
        Check(KindOf(false, false, 0x55, step, 3) == Kind::Footstep, "kind: a person's step");
        Check(KindOf(false, true, 0x55, step, 3) == Kind::HorseStep, "kind: a horse's step");
        Check(KindOf(false, true, 0x55, movement, 2) == Kind::Movement, "kind: the innermost known caller wins (movement)");
        Check(KindOf(false, false, 0x55, speech, 1) == Kind::Speech, "kind: the line's sound");
        Check(KindOf(false, false, 0x55, none, 2) == Kind::Generic, "kind: no known caller");
        Check(std::string(RoleName(Role::Main, false)) == "player" && std::string(RoleName(Role::Main, true)) == "puppet_player" &&
                  std::string(RoleName(Role::MainHorse, true)) == "horse_player",
            "role names per pass");
    }

    // The ped steps reach the creator
    // through the S03 queue: the census frames are the drain's; the family
    // is on the queued request's frames.
    void TestQueuedKinds()
    {
        const std::uint32_t drain[3] = {0x0258A469u, 0x02F1D23Eu, 0x02EE86B1u};
        const std::uint32_t pedStep[4] = {0x02589A60u, 0x0318D0A0u, 0x0318DAFEu, 0};
        const std::uint32_t mainSound[4] = {0x0318E828u, 0, 0, 0};
        const std::uint32_t unknown[4] = {0x01000000u, 0x01000010u, 0, 0};
        Check(KindOf(false, false, 0x800006001d9a86ull, drain, 3) == Kind::Generic, "queued: the drain's frames name nothing");
        const Kind step = Kind::Footstep;
        Check(KindOf(false, false, 0x800006001d9a86ull, drain, 3, &step) == Kind::Footstep, "queued: the request's kind is taken");
        RequestFamilies requests;
        requests.Note(0xA000, 0x800006001d9a86ull, 1000, pedStep, 4);
        requests.Note(0xA000, 0x8000000033fd85ull, 1000, mainSound, 4);
        requests.Note(0xA000, 0x55, 1000, unknown, 4);
        Check(requests.noted == 2 && requests.Pending() == 2, "queued: only requests whose frames name a system are kept");
        replay::soundowner::Family family = replay::soundowner::Family::Owned;
        Check(!requests.Take(0xB000, 0x800006001d9a86ull, family), "queued: another audio entity does not match");
        Check(requests.Take(0xA000, 0x800006001d9a86ull, family) && family == replay::soundowner::Family::Footstep,
            "queued: the step's family");
        Check(!requests.Take(0xA000, 0x800006001d9a86ull, family), "queued: consumed once");
        Kind kind = Kind::Generic;
        Check(KindOfFamily(replay::soundowner::Family::Footstep, true, kind) && kind == Kind::HorseStep,
            "queued: a horse's step family is a horse step");
        requests.Prune(1000 + RequestFamilies::kRequestKeepUs + 1);
        Check(requests.Pending() == 0 && requests.expired == 1 && requests.matched == 1, "queued: old requests expire");
    }

    void TestDistances()
    {
        Check(ToDm(3.44) == 34 && ToDm(-1.0) == kNoDistance && ToDm(1e9) == kMaxDistanceDm, "distance in decimetres");
        Check(ListenerDm(900.0f) == 300 && ListenerDm(-1.0f) == kNoDistance, "engine metric: square root (30 m from 900)");
        const float a[3] = {0.0f, 0.0f, 0.0f};
        const float b[3] = {3.0f, 4.0f, 0.0f};
        Check(Distance(a, b) == 5.0, "distance");
    }

    Sample MakeSample(std::uint32_t t, std::uint32_t owner, Role role, Kind kind, std::uint64_t ref, double dCam,
        std::uint8_t lod, std::uint8_t flags, double dPlayer = 1.0, double dLst = -1.0)
    {
        Sample s;
        s.timeMs = t;
        s.owner = owner;
        s.source = owner;
        s.role = static_cast<std::uint8_t>(role);
        s.kind = static_cast<std::uint8_t>(kind);
        s.ref = ref;
        s.dCamDm = ToDm(dCam);
        s.dPlayerDm = ToDm(dPlayer);
        s.dListenerDm = dLst < 0.0 ? kNoDistance : ToDm(dLst);
        s.lod = lod;
        s.flags = flags;
        return s;
    }

    void TestTrackCutAndTrim()
    {
        Track track;
        track.mainId = 0x00010001u;
        for (std::uint32_t t = 0; t < 5000; t += 1000)
        {
            track.samples.push_back(MakeSample(t, 7, Role::Npc, Kind::Shot, 1, 10.0, 0, 0));
            Listener l;
            l.timeMs = t;
            track.listeners.push_back(l);
        }
        Track cut;
        track.CutInto(cut, 1500, 3500);
        Check(cut.mainId == track.mainId && cut.samples.size() == 2 && cut.samples[0].timeMs == 500 &&
                  cut.samples[1].timeMs == 1500 && cut.listeners.size() == 2 && cut.listeners[1].timeMs == 1500,
            "track: a window keeps its samples, shifted to its base");
        track.DropBefore(2000);
        Check(track.samples.size() == 3 && track.samples[0].timeMs == 2000 && track.listeners.size() == 3,
            "track: the buffer drops what is older");
        Check(!track.Empty() && track.ApproxBytes() > 0, "track: not empty");
        track.Clear();
        Check(track.Empty() && track.mainId == replay::census::kNoOwner, "track: cleared");
    }

    void TestBudgets()
    {
        SampleBudget samples;
        std::uint32_t kept = 0;
        for (std::uint32_t n = 0; n < 500; ++n)
            kept += samples.Take(1000 + n) ? 1 : 0;
        Check(kept == kMaxSamplesPerSecond, "sample budget: capped per clip second");
        Check(samples.Take(2000), "sample budget: the next second starts again");

        LineBudget lines(false);
        std::uint32_t shots = 0, generic = 0;
        for (int n = 0; n < 50; ++n)
        {
            shots += lines.Take(Kind::Shot, 10000) ? 1 : 0;
            generic += lines.Take(Kind::Generic, 10000) ? 1 : 0;
        }
        Check(shots == LineBudget::PerSecond(Kind::Shot, false) && generic == LineBudget::PerSecond(Kind::Generic, false),
            "line budget: per kind per wall second");
        Check(lines.Suppressed() == 100 - shots - generic && lines.Take(Kind::Generic, 11000), "line budget: counted and re-armed");
        LineBudget play(true);
        Check(LineBudget::PerSecond(Kind::Shot, true) > LineBudget::PerSecond(Kind::Shot, false), "line budget: the replay logs more");
    }

    void TestTotals()
    {
        Totals totals;
        for (int n = 0; n < 9; ++n)
            totals.Add(MakeSample(n * 10, 1, Role::Main, Kind::Shot, 5, 2.0 + n, n < 6 ? 0 : 3,
                kFlagPlayerAudio | kFlagBitKnown | kFlagPlayerBit));
        totals.Add(MakeSample(100, 2, Role::Npc, Kind::Footstep, 6, 50.0, kNoLod, kFlagRefused));
        totals.world = 4;
        const Cell* cell = totals.Find(Role::Main, Kind::Shot);
        Check(cell && cell->count == 9 && cell->playerAudio == 9 && cell->lod[0] == 6 && cell->lod[3] == 3,
            "totals: counts, flags and LOD histogram");
        Check(cell && cell->cam.Median() == 6.5 && cell->cam.Mean() > 5.9 && cell->cam.Mean() < 6.1, "totals: median (1 m bins) and mean");
        const std::vector<std::string> lines = totals.Lines("record");
        Check(lines.size() == 2 && Has(lines[0], "MIX_TOTAL,phase=record,role=player,kind=shot,count=9") &&
                  Has(lines[0], "lod=0:6|1:0|2:0|3:3|4:0|?:0") && Has(lines[1], "role=npc,kind=footstep,count=1,refused=1"),
            "totals: one line per role and kind");
        Check(Has(totals.Lines("play")[0], "role=puppet_player"), "totals: the main role is the puppet in the replay");
    }

    void TestCoverage()
    {
        Coverage cover;
        cover.Note(0.0, 0.0);
        cover.Note(0.0, 16.0);
        for (double t = 16.0; t < 2000.0; t += 16.0)
            cover.Note(t, t + 16.0);
        Check(cover.Contains(0) && cover.Contains(1999) && !cover.Contains(2300), "coverage: the windows played");
        Check(cover.CoveredSeconds() > 1.9 && cover.CoveredSeconds() < 2.3 && cover.PlaySeconds() > 1.9, "coverage: seconds");
        cover.Note(2000.0, 9000.0); // a jump: only its ends
        Check(cover.Contains(9000) && !cover.Contains(5000), "coverage: a jump marks its ends only");
    }

    // A made-up scene: a gunfight on horseback. Recording: the
    // camera behind Arthur (3 m), Arthur's shots with the player variant,
    // his horse's hooves with the player bit, NPC shots of ref A at 20 m,
    // LOD 0, music on. Replay: the free camera 40 m away, the shots still
    // the player's (the detour), the hooves without the bit, NPC shots of
    // ref B, LOD 3, no music.
    void BuildScene(Track& rec, std::vector<Sample>& play, std::vector<Listener>& playL, Coverage& cover, bool weak)
    {
        const std::uint32_t arthur = 0x00000001u, horse = 0x00010002u, gun = 0x00020003u, npcGun = 0x00030004u;
        rec.mainId = arthur;
        for (std::uint32_t t = 0; t < 10000; t += 200)
        {
            const std::uint8_t player = kFlagPlayerAudio | kFlagBitKnown | kFlagPlayerBit;
            rec.samples.push_back(MakeSample(t, gun, Role::Main, Kind::Shot, 0x100, 3.0, 0, player, 0.5, 3.0));
            rec.samples.push_back(MakeSample(t + 50, horse, Role::MainHorse, Kind::HorseStep, 0x200, 3.5, 0,
                kFlagPlayerAudio | kFlagBitKnown | kFlagPlayerBit, 1.5, 3.5));
            rec.samples.push_back(MakeSample(t + 60, horse, Role::MainHorse, Kind::Movement, 0x300, 3.5, 0,
                kFlagPlayerAudio | kFlagBitKnown | kFlagPlayerBit, 1.5, 3.5));
            rec.samples.push_back(MakeSample(t + 100, npcGun, Role::Npc, Kind::Shot, 0xA, 20.0, 0, kFlagBitKnown, 20.0, 20.0));
            if (t % 1000 == 0)
            {
                Listener l;
                l.timeMs = t;
                l.main[0] = 3.0f;
                l.hasMain = 1;
                l.music = 1;
                rec.listeners.push_back(l);
            }
        }
        for (std::uint32_t t = 0; t < 10000; t += 200)
        {
            const double cam = weak ? 40.0 : 3.0;
            const std::uint8_t lod = weak ? 3 : 0;
            play.push_back(MakeSample(t + 10, gun, Role::Main, Kind::Shot, 0x100, cam, lod,
                kFlagPlayerAudio | kFlagBitKnown | (weak ? 0 : kFlagPlayerBit), 300.0, cam));
            play.push_back(MakeSample(t + 55, horse, Role::MainHorse, Kind::HorseStep, 0x200, cam + 0.5, lod,
                (weak ? 0 : kFlagPlayerAudio | kFlagPlayerBit) | kFlagBitKnown, 300.0, cam));
            play.push_back(MakeSample(t + 65, horse, Role::MainHorse, Kind::Movement, 0x300, cam + 0.5, lod,
                (weak ? 0 : kFlagPlayerAudio | kFlagPlayerBit) | kFlagBitKnown, 300.0, cam));
            play.push_back(MakeSample(t + 110, npcGun, Role::Npc, Kind::Shot, weak ? 0xB : 0xA, weak ? 50.0 : 20.0, lod,
                kFlagBitKnown, 300.0, weak ? 50.0 : 20.0));
            if (t % 1000 == 0)
            {
                Listener l;
                l.timeMs = t;
                l.main[0] = weak ? 40.0f : 3.0f;
                l.hasMain = 1;
                l.music = weak ? 0 : 1;
                playL.push_back(l);
            }
        }
        for (double t = 0.0; t < 10000.0; t += 16.0)
            cover.Note(t, t + 16.0);
    }

    void TestCompareWeak()
    {
        Track rec;
        std::vector<Sample> play;
        std::vector<Listener> playL;
        Coverage cover;
        BuildScene(rec, play, playL, cover, true);
        const Comparison c = Compare(rec, play, playL, cover);
        Check(c.recorded && c.recSamples == 200 && c.playSamples == 200, "weak: every sample compared");
        Check(c.verdicts[static_cast<int>(Cause::Listener)].level == Level::Likely, "weak: listener distance is likely");
        Check(Has(c.playListenerFollows, "camera ("), "weak: the engine distance follows the camera in the replay");
        Check(c.verdicts[static_cast<int>(Cause::PlayerTreatment)].level == Level::Likely &&
                  Has(c.verdicts[static_cast<int>(Cause::PlayerTreatment)].why, "horse_player"),
            "weak: the player treatment lost on the hooves is likely");
        Check(c.verdicts[static_cast<int>(Cause::Variant)].level == Level::Likely && c.swaps.size() == 1 &&
                  c.swaps[0].recRef == 0xA && c.swaps[0].playRef == 0xB && c.swaps[0].count == 50,
            "weak: the NPC shot variant is likely, with its pair");
        Check(c.verdicts[static_cast<int>(Cause::Lod)].level == Level::Likely, "weak: the LOD is likely");
        Check(c.verdicts[static_cast<int>(Cause::Music)].level == Level::Likely && c.recMusic == 1.0 && c.playMusic == 0.0,
            "weak: the music is likely");
        Check(c.recCamMain == 3.0 && c.playCamMain == 40.0, "weak: camera to main character");
        const std::string section = FormatSection(c);
        Check(Has(section, "== mix probe") && Has(section, "1. listener distance") && Has(section, "likely") &&
                  Has(section, "Variant pairs") && Has(section, "horse_player"),
            "weak: the report section");
        const std::vector<std::string> verdicts = VerdictLines(c);
        Check(verdicts.size() == kCauseCount && Has(verdicts[0], "MIX_VERDICT,cause=listener,level=likely,why=") &&
                  verdicts[0].find(',', verdicts[0].find("why=")) == std::string::npos,
            "weak: verdict lines (no comma inside why)");
        const std::vector<std::string> rows = RowLines(c, "MIX_MARK_ROW", ",clip_ms=5000");
        Check(!rows.empty() && Has(rows[0], "MIX_MARK_ROW,clip_ms=5000,role="), "weak: row lines with the extra field");
        // The 3 s before a mark at 5 s.
        const Comparison mark = Compare(rec, play, playL, cover, 2000, 5000);
        Check(mark.recSamples > 0 && mark.recSamples < 80 && mark.verdicts[static_cast<int>(Cause::Listener)].level == Level::Likely,
            "weak: a mark's window");
    }

    void TestCompareFaithful()
    {
        Track rec;
        std::vector<Sample> play;
        std::vector<Listener> playL;
        Coverage cover;
        BuildScene(rec, play, playL, cover, false);
        const Comparison c = Compare(rec, play, playL, cover);
        for (std::uint8_t n = 0; n < kCauseCount; ++n)
            Check(c.verdicts[n].level == Level::No,
                (std::string("faithful: no for ") + CauseName(static_cast<Cause>(n)) + " (" + c.verdicts[n].why + ")").c_str());
        std::uint32_t same = 0;
        for (const Row& row : c.rows)
            same += row.same;
        Check(same == 200 && c.swaps.empty(), "faithful: every replayed sound meets its recorded one");
    }

    void TestVariantBySet()
    {
        Track rec;
        std::vector<Sample> play;
        Coverage cover;
        const std::uint32_t npc = 0x00240074u;
        for (std::uint32_t t = 0; t < 5000; t += 100)
        {
            rec.samples.push_back(MakeSample(t, npc, Role::Npc, Kind::Generic, 0x800006000dc393ull, 10.0, 1, 0));
            play.push_back(MakeSample(t + 20, npc, Role::Npc, Kind::Generic, 0x800003000dc393ull, 10.0, 1, 0));
        }
        for (double t = 0.0; t < 5000.0; t += 16.0)
            cover.Note(t, t + 16.0);
        const Comparison c = Compare(rec, play, {}, cover);
        Check(c.rows.size() == 1 && c.rows[0].same == 50 && c.rows[0].entry == 50 && c.rows[0].swapped == 0 && c.swaps.empty(),
            "variant: same set, other entry byte is the same sound set (36:116 x27 in clip 078)");
        Check(c.verdicts[static_cast<int>(Cause::Variant)].level == Level::No &&
                  Has(c.verdicts[static_cast<int>(Cause::Variant)].why, "50 same set with another entry"),
            "variant: no, the entry swaps said apart");
    }

    void TestLodBothWays()
    {
        Track rec;
        std::vector<Sample> play;
        Coverage cover;
        for (std::uint32_t t = 0; t < 5000; t += 100)
        {
            rec.samples.push_back(MakeSample(t, 7, Role::Npc, Kind::Footstep, 0x10, 15.0, 2, 0));
            play.push_back(MakeSample(t + 10, 7, Role::Npc, Kind::Footstep, 0x10, 15.0, 0, 0));
        }
        for (double t = 0.0; t < 5000.0; t += 16.0)
            cover.Note(t, t + 16.0);
        const Comparison c = Compare(rec, play, {}, cover);
        Check(c.verdicts[static_cast<int>(Cause::Lod)].level == Level::Likely &&
                  Has(c.verdicts[static_cast<int>(Cause::Lod)].why, "more detailed"),
            "lod: a replay more detailed than live is likely too (npc 1.7 -> 0.4 in clip 078)");
    }

    void TestRefusedAndCount()
    {
        Track rec;
        std::vector<Sample> play;
        Coverage cover;
        const std::uint32_t wagon = 0x00020052u, horse = 0x00030010u, rider = 0x00040011u;
        for (std::uint32_t t = 0; t < 10000; t += 100)
        {
            rec.samples.push_back(MakeSample(t, wagon, Role::Vehicle, Kind::Generic, 0x11620c4, 10.0, kNoLod, 0));
            play.push_back(MakeSample(t + 5, wagon, Role::Vehicle, Kind::Generic, 0x11620c4, 10.0, kNoLod, 0));
            // Three of every ten replayed wagon requests refused (ref 0xffffffff).
            if (t % 1000 < 300)
                play.push_back(MakeSample(t + 6, wagon, Role::Vehicle, Kind::Generic, 0xffffffffull, 10.0, kNoLod,
                    kFlagRefused | kFlagMine));
        }
        // Hooves: 60 a second recorded, 18 replayed (free camera).
        for (std::uint32_t t = 0; t < 10000; t += 16)
            rec.samples.push_back(MakeSample(t, horse, Role::Horse, Kind::HorseStep, 0x2f49ac, 6.0, 1, 0));
        for (std::uint32_t t = 0; t < 10000; t += 55)
            play.push_back(MakeSample(t + 3, horse, Role::Horse, Kind::HorseStep, 0x2f49ac, 6.0, 1, 0));
        // A rider's steps: generic in the recording (a clip without the queued
        // kind), footstep in the replay, the same rate.
        for (std::uint32_t t = 0; t < 10000; t += 200)
        {
            rec.samples.push_back(MakeSample(t, rider, Role::Npc, Kind::Generic, 0x1d9a86, 6.0, 1, 0));
            play.push_back(MakeSample(t + 3, rider, Role::Npc, Kind::Footstep, 0x1d9a86, 6.0, 1, 0));
        }
        for (double t = 0.0; t < 10000.0; t += 16.0)
            cover.Note(t, t + 16.0);
        const Comparison c = Compare(rec, play, {}, cover);
        const Row* vehicle = nullptr;
        for (const Row& row : c.rows)
            if (row.role == Role::Vehicle)
                vehicle = &row;
        Check(vehicle && vehicle->play.count == 100 && vehicle->play.refused == 30 && vehicle->play.perSecond < 10.1 &&
                  vehicle->same == 100 && vehicle->unmatched == 0,
            "refused: out of the count, the rate and the matching, counted apart (107 wagon requests in clip 078)");
        Check(Has(RowLines(c, "MIX_COMPARE")[0], "play_refused="), "refused: in the row line");
        const Verdict& count = c.verdicts[static_cast<int>(Cause::Count)];
        Check(count.level == Level::Likely && Has(count.why, "horse horse_step"), "count: the missing hooves are likely");
        // Only the reclassified steps: the role keeps its rate, possible at most.
        Track recSteps;
        std::vector<Sample> playSteps;
        for (const Sample& s : rec.samples)
            if (s.owner == rider)
                recSteps.samples.push_back(s);
        for (const Sample& s : play)
            if (s.owner == rider)
                playSteps.push_back(s);
        const Comparison r = Compare(recSteps, playSteps, {}, cover);
        Check(r.verdicts[static_cast<int>(Cause::Count)].level == Level::Possible &&
                  Has(r.verdicts[static_cast<int>(Cause::Count)].why, "classified apart"),
            "count: a kind classified apart on one side is only possible");
        Check(Has(FormatSection(c), "6. sound count") && Has(FormatSection(c), "refused"), "count: in the section");
    }

    void TestCompareWithoutRecording()
    {
        Track rec;
        std::vector<Sample> play;
        std::vector<Listener> playL;
        Coverage cover;
        Track ignored;
        BuildScene(ignored, play, playL, cover, true);
        const Comparison c = Compare(rec, play, playL, cover);
        Check(!c.recorded && c.verdicts[static_cast<int>(Cause::Listener)].level == Level::Unknown &&
                  Has(c.verdicts[static_cast<int>(Cause::Listener)].why, "no recording-side"),
            "no recording: unknown");
        Check(Has(FormatSection(c), "only the replay side"), "no recording: said in the section");
        // Recorded samples outside the covered span are left out.
        Track partial;
        BuildScene(partial, play, playL, cover, true);
        Coverage half;
        for (double t = 0.0; t < 4900.0; t += 16.0)
            half.Note(t, t + 16.0);
        const Comparison p = Compare(partial, play, playL, half);
        Check(p.recSamples == 100, "coverage: only the recorded samples where the playback went");
    }
}

int RunMixProbeTests()
{
    g_mixFailures = 0;
    TestKinds();
    TestQueuedKinds();
    TestDistances();
    TestTrackCutAndTrim();
    TestBudgets();
    TestTotals();
    TestCoverage();
    TestCompareWeak();
    TestCompareFaithful();
    TestCompareWithoutRecording();
    TestVariantBySet();
    TestLodBothWays();
    TestRefusedAndCount();
    if (!g_mixFailures)
        std::printf("Mix probe tests passed.\n");
    return g_mixFailures;
}
