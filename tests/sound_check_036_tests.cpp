// Defects the automatic sound check found on recorded clip 036, pure parts:
//   an animal line a native attempt created never goes on to the direct path
//   (horse_sound_report.h RunsDirect);
//   the same request resolving to another sound is a variant
//   (sound_check_core.h SameRequestPath, SubstituteCounts, SpansMeet);
//   a world sound of no entity and no position is "unplaced", not refused,
//   and the engine refusals carry their status (sound_generic_player_core.h);
//   the report's coverage block, slow motion compared, the rest by span and
//   why (sound_check_core.h PlayCoverage, FormatCoverage);
//   the ESND track per family (entity_sound_tally.h).
#include "entity_sound_tally.h"
#include "horse_sound_report.h"
#include "puppet_audio_rule.h"
#include "sound_check_core.h"
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_failures036 = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures036;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    // ------------------------------------------------------------ native then direct

    // The loop of PlayPoseV2Speech (replay_events.cpp) over the pure steps:
    // the native attempts, then the direct one only when RunsDirect says.
    struct LineRun
    {
        int nativeRequests = 0;
        bool direct = false;
    };

    LineRun RunLine(bool animal, bool nativeOn, bool hasVariation, bool pinnedCreates, bool contextCreates)
    {
        using namespace replay::horsesound;
        LineRun run;
        AnimalAttempt attempt = FirstAttempt(animal, nativeOn);
        while (attempt == AnimalAttempt::Pinned || attempt == AnimalAttempt::Context)
        {
            ++run.nativeRequests;
            const bool created = attempt == AnimalAttempt::Pinned ? pinnedCreates : contextCreates;
            if (created)
            {
                attempt = NextAttempt(attempt, true);
                break;
            }
            attempt = NextAttempt(attempt, false, hasVariation);
        }
        run.direct = RunsDirect(attempt);
        return run;
    }

    void TestAnimalLineOnce()
    {
        using namespace replay::horsesound;
        // 036, horse 0:1: the native path with identity created all 26 lines
        // (result 73); the direct path must not be asked again.
        const LineRun native = RunLine(true, true, true, true, false);
        Check(native.nativeRequests == 1 && !native.direct, "item 2: a line the native path created is not asked again");
        const LineRun context = RunLine(true, true, true, false, true);
        Check(context.nativeRequests == 2 && !context.direct, "item 2: created by the context-only attempt: no direct");
        const LineRun none = RunLine(true, true, true, false, false);
        Check(none.nativeRequests == 2 && none.direct, "item 2: no native line: the direct path is the last resort");
        const LineRun noVariation = RunLine(true, true, false, false, false);
        Check(noVariation.nativeRequests == 1 && noVariation.direct, "item 2: no recorded variation: pinned, then direct");
        const LineRun person = RunLine(false, true, true, true, true);
        Check(person.nativeRequests == 0 && person.direct, "item 2: a person keeps the direct path");
        const LineRun off = RunLine(true, false, true, true, true);
        Check(off.nativeRequests == 0 && off.direct, "item 2: AnimalVocalNative=0: direct only");
        // A created line left at Pinned: still no direct for it (only Direct
        // goes on).
        Check(!RunsDirect(AnimalAttempt::Pinned) && !RunsDirect(AnimalAttempt::Context) && !RunsDirect(AnimalAttempt::Done) &&
                RunsDirect(AnimalAttempt::Direct),
            "item 2: only Direct goes on to the direct path");
    }

    // The line's own sound on the ESND track (puppet_audio_rule.h
    // SpeechRowRule): 036, horse 0:1, 31 rows of 0x800004004366e8 from the
    // speech controller at the ms of its lines, played by the ESND track
    // and again by the replayed line (census 75 for 31).
    void TestSpeechControllerRows()
    {
        using namespace replay::puppetaudio;
        Check(SpeechControllerCaller(0x2FA10CF) && SpeechControllerCaller(0x2FA0DCC) && !SpeechControllerCaller(0x2588C2F),
            "item 2: the speech controller's call sites");
        Check(SpeechRowRule(true, true, LineState::Played, 0, 250) == SpeechRow::LeaveToLine,
            "item 2: the line played: its sound is the line's");
        Check(SpeechRowRule(true, true, LineState::Refused, 0, 250) == SpeechRow::Play,
            "item 2: the line refused: the recorded sound plays over silence");
        Check(SpeechRowRule(true, true, LineState::Unknown, 100, 250) == SpeechRow::Wait &&
                SpeechRowRule(true, true, LineState::Unknown, 250, 250) == SpeechRow::Play,
            "item 2: no answer yet: wait, then play");
        Check(SpeechRowRule(true, false, LineState::Played, 0, 250) == SpeechRow::NotSpeech &&
                SpeechRowRule(false, true, LineState::Played, 0, 250) == SpeechRow::NotSpeech,
            "item 2: no line at the row, or another call site: the ESND rules as before");
    }

    // ------------------------------------------------------------ variants

    using namespace replay::census;

    SummaryKey Key(std::uint32_t owner, OwnerClass cls, std::uint64_t ref, const std::uint32_t (&path)[kPathDepth])
    {
        SummaryKey key;
        key.owner = owner;
        key.cls = cls;
        key.ref = ref;
        for (std::uint32_t n = 0; n < kPathDepth; ++n)
            key.path[n] = path[n];
        return key;
    }

    struct Pair
    {
        Summary recSummary;
        Summary playSummary;
        Accumulator rec;
        Accumulator play;
        Pair()
        {
            rec.Attach(&recSummary, false);
            play.Attach(&playSummary, true);
        }
        void CoverBoth(double from, double to)
        {
            for (double ms = from; ms <= to; ms += 33)
            {
                rec.Cover(ms - 33 < from ? from : ms - 33, ms);
                play.Cover(ms - 33 < from ? from : ms - 33, ms);
            }
        }
        DiffResult Run() const { return Diff(recSummary, playSummary, play.Passes(), IgnoreList{}); }
    };

    const Finding* Find(const DiffResult& diff, FindingKind kind, std::uint32_t owner, std::uint64_t ref)
    {
        for (const Finding& f : diff.findings)
            if (f.kind == kind && f.ref == ref && f.owner == owner)
                return &f;
        return nullptr;
    }

    constexpr std::uint32_t kWeapon66 = (66u << 16) | 307u;
    constexpr std::uint32_t kWeapon19 = (19u << 16) | 374u;
    constexpr std::uint32_t kPed24 = (24u << 16) | 256u;
    constexpr std::uint64_t kFe9e = 0xfffff0118fe9eull;
    constexpr std::uint64_t kF975 = 0xfffff0118f975ull;
    constexpr std::uint32_t kWeaponPath[kPathDepth] = {0x2588eda, 0x2588db0, 0x2efaeaf, 0x2f0b883, 0x2f0dea0};
    constexpr std::uint32_t kQueuePath[kPathDepth] = {0x2588eda, 0x258a469, 0x2f1d23e, 0x2ee86b1, 0x25a74db};

    void TestSameRequestVariant()
    {
        Check(SameRequestPath(kWeaponPath, kWeaponPath), "item 3: the weapon fire path is one request");
        Check(!SameRequestPath(kQueuePath, kQueuePath), "item 3: the shared create-and-play queue path is not");
        std::uint32_t other[kPathDepth] = {0x2588eda, 0x2588db0, 0x2efaeaf, 0x2f0b883, 0x2f0deaa};
        Check(!SameRequestPath(kWeaponPath, other), "item 3: another frame is another request");
        Check(SubstituteCounts(6.0, 6.0) && SubstituteCounts(5.0, 5.0) && !SubstituteCounts(6.0, 1.0) &&
                !SubstituteCounts(2.0, 9.0) && !SubstituteCounts(0.0, 3.0),
            "item 3: about the same number");

        // 036: 66:307 recorded fe9e 6 times, the replay made
        // f975 6 times at the same shots; 19:374 recorded fe9e 5 and f975 2,
        // the replay f975 7.
        Pair p;
        p.CoverBoth(42000, 72000);
        const double shots66[] = {66500, 67000, 68250, 68500, 70000, 70250};
        for (const double ms : shots66)
        {
            p.rec.Add(ms, Key(kWeapon66, OwnerClass::Weapon, kFe9e, kWeaponPath), false);
            p.play.Add(ms + 20, Key(kWeapon66, OwnerClass::Weapon, kF975, kWeaponPath), false);
        }
        const double shots19[] = {67750, 68000, 68750, 69250, 69500};
        for (const double ms : shots19)
        {
            p.rec.Add(ms, Key(kWeapon19, OwnerClass::Weapon, kFe9e, kWeaponPath), false);
            p.play.Add(ms + 20, Key(kWeapon19, OwnerClass::Weapon, kF975, kWeaponPath), false);
        }
        for (const double ms : {70800.0, 71000.0})
        {
            p.rec.Add(ms, Key(kWeapon19, OwnerClass::Weapon, kF975, kWeaponPath), false);
            p.play.Add(ms + 20, Key(kWeapon19, OwnerClass::Weapon, kF975, kWeaponPath), false);
        }
        // A ped whose steps (queue path) are missing while another ref on
        // the same queue path is extra: not one request, both stay.
        for (int n = 0; n < 6; ++n)
        {
            p.rec.Add(62000 + n * 250.0, Key(kPed24, OwnerClass::Ped, 0x80000d0033c2c9ull, kQueuePath), false);
            p.play.Add(62000 + n * 250.0, Key(kPed24, OwnerClass::Ped, 0xfffff00398bbeull, kQueuePath), false);
        }
        const DiffResult d = p.Run();
        const Finding* lost66 = Find(d, FindingKind::Variant, kWeapon66, kFe9e);
        Check(lost66 && lost66->sameRequest && lost66->otherRef == kF975 && lost66->lostSide,
            "item 3: 66:307 fe9e replayed as f975 is a variant of the same request");
        Check(Find(d, FindingKind::Variant, kWeapon66, kF975) != nullptr, "item 3: its pair is a variant too");
        Check(Find(d, FindingKind::Variant, kWeapon19, kFe9e) && Find(d, FindingKind::Variant, kWeapon19, kF975),
            "item 3: 19:374 too (5 short, 5 over with the 2 recorded matched)");
        Check(!Find(d, FindingKind::Missing, kWeapon66, kFe9e) && !Find(d, FindingKind::Extra, kWeapon66, kF975),
            "item 3: no missing or extra left for the weapons");
        Check(Find(d, FindingKind::Missing, kPed24, 0x80000d0033c2c9ull) && Find(d, FindingKind::Extra, kPed24, 0xfffff00398bbeull),
            "item 3: the shared queue path pairs nothing");
        ReportHeader header;
        header.clip = "036-HennigansStead.rpclip";
        const std::string report = FormatReport(d, header);
        Check(Has(report, "== variants (not counted: the engine picked another sound for the same request) ==") &&
                Has(report, "variant  owner 66:307 (weapon)  recorded 0xfffff0118fe9e (6, replay 0), replay made 0xfffff0118f975 instead; same caller path at the same times") &&
                Has(report, "path c=2588eda;2588db0;2efaeaf;2f0b883;2f0dea0"),
            "item 3: the report lists each pair");
        Check(!Has(report, "variant  owner 66:307 (weapon)  recorded 0xfffff0118f975"), "item 3: each pair once, recorded side");
    }

    // ------------------------------------------------------------ unplaced

    namespace sg = replay::soundgeneric;
    namespace sgp = replay::soundgeneric::player;

    struct CountingEngine : sgp::Engine
    {
        int creates = 0;
        bool refuse = false;
        bool Create(const sg::CreatePacket&, bool, std::uint64_t& handle, std::string& status, bool& retry) override
        {
            ++creates;
            retry = false;
            if (refuse)
            {
                status = "not_created:audio_ok";
                return false;
            }
            handle = static_cast<std::uint64_t>(creates);
            status = "playing";
            return true;
        }
        bool Prepare(std::uint64_t) override { return true; }
        bool Play(std::uint64_t) override { return true; }
        void Stop(std::uint64_t, sg::EndReason) override {}
        bool Alive(std::uint64_t) override { return true; }
        void Adjust(std::uint64_t, const sg::AdjustPacket&) override {}
        bool MadeByOther(const sg::CreatePacket&, std::uint32_t) override { return false; }
    };

    sg::CreatePacket WorldPacket(sg::Track& track, std::uint32_t timeMs, bool hasPosition)
    {
        sg::CreatePacket p;
        p.timeMs = timeMs;
        p.index = track.nextIndex++;
        p.stableId = 0x0300123456ull;
        p.idKind = sg::IdKind::RefSet;
        p.rawRef = 0x0080000300123456ull;
        p.owner = kNoOwner;
        p.ownerClass = OwnerClass::World;
        p.source = sg::Source::Queue;
        p.hasPosition = hasPosition;
        p.hasParams = true;
        p.caller = 0x2EFA7E0;
        track.creates.push_back(p);
        return p;
    }

    sgp::Config TestConfig()
    {
        sgp::Config c;
        c.preloadMs = 500;
        c.preloadDelayMs = 100;
        c.holdMs = 150;
        c.loopMs = 1000;
        c.maxStepMs = 250;
        c.oneShotWindowMs = 500;
        c.doubleToleranceMs = 250;
        c.enabled = true;
        return c;
    }

    void TestUnplacedWorldSounds()
    {
        sg::Track track;
        WorldPacket(track, 1000, false); // a 2D sound of nobody
        WorldPacket(track, 1100, true);  // a placed world sound
        CountingEngine engine;
        sgp::Player player;
        player.Begin(track, TestConfig(), replay::soundowner::Cover(), replay::soundowner::Overrides(), 0);
        for (std::uint32_t t = 0; t <= 2000; t += 50)
            player.Tick(t, true, engine);
        const sgp::PlayStats& s = player.Stats();
        Check(engine.creates == 1 && s.played == 1 && s.unplaced == 1 && s.refused == 0,
            "item 4: the 2D sound of nobody is unplaced, never asked of the engine, not refused");
        const auto world = static_cast<std::uint8_t>(replay::soundowner::Family::World);
        Check(s.families[world].unplaced == 1 && s.families[world].refused == 0, "item 4: per family too");
        Check(sgp::OutcomeOfSkip(sgp::SkipReason::Unplaced) == sgp::Outcome::Unplaced &&
                std::string(sgp::OutcomeName(sgp::Outcome::Unplaced)) == "unplaced" &&
                std::string(sgp::SkipName(sgp::SkipReason::Unplaced)) == "unplaced",
            "item 4: names");

        sg::Track refusedTrack;
        WorldPacket(refusedTrack, 1000, true);
        WorldPacket(refusedTrack, 1300, true);
        CountingEngine refusing;
        refusing.refuse = true;
        sgp::Player second;
        second.Begin(refusedTrack, TestConfig(), replay::soundowner::Cover(), replay::soundowner::Overrides(), 0);
        for (std::uint32_t t = 0; t <= 2500; t += 50)
            second.Tick(t, true, refusing);
        const sgp::PlayStats& r = second.Stats();
        const auto status = r.refusedStatus.find("not_created:audio_ok");
        Check(r.refused == 2 && status != r.refusedStatus.end() && status->second == 2,
            "item 4: the refusals are counted by the engine side's status, once per packet");
        const std::string section = sgp::FormatSection(r, replay::soundowner::Overrides(), std::string());
        Check(Has(section, "Refused by the engine, by status: not_created:audio_ok 2.\n"), "item 4: the report says why");
        const std::string firstSection = sgp::FormatSection(s, replay::soundowner::Overrides(), std::string());
        Check(Has(firstSection, "Not placed (2D sounds of no entity: interface, frontend, ambience beds; the live world makes its own): 1.\n"),
            "item 4: the report counts the unplaced apart");
        Check(Has(sgp::FormatPlayLogLine(s, "stop", 2000, 0, 0), ",refused=0,unplaced=1,"), "item 4: the log line");
    }

    // ------------------------------------------------------------ coverage

    void TestCoverage()
    {
        // 036 as played: trim 42.5 to 71.1 s, slow motion (x0.2)
        // from 45.7 to 47.3 s, a pause at 48.1 s and a seek to 48.3 s.
        Summary recorded;
        for (std::uint32_t w = 0; w < 832; ++w) // 208 s
            recorded.covered.push_back(w);
        PlayCoverage cover;
        Summary played;
        Accumulator acc;
        acc.Attach(&played, true);
        double last = 42486;
        const auto play = [&](double to, double rate) {
            for (double ms = last; ms < to; ms += 16.0 * rate)
            {
                const double next = ms + 16.0 * rate;
                acc.Cover(ms, next);
                cover.Note(ms, next, RateFlag(rate), rate);
            }
            last = to;
        };
        play(45693, 1.0);
        play(47306, 0.2);
        play(48148, 1.0);
        acc.Break();
        cover.Note(48148, 48148, kCoverPaused);
        cover.Note(48148, 48303, kCoverScrub); // the seek forward
        last = 48303;
        play(71053, 1.0);
        Check(RateFlag(0.2) == kCoverSlow && RateFlag(1.0) == kCoverNormal && RateFlag(4.0) == kCoverFast, "item 5: rate classes");
        const std::string text = FormatCoverage(recorded, cover, acc.Passes());
        Check(Has(text, "Compared span: 29.0 s of 208.0 s recorded; 179.0 s left out"),
            "item 5: the compared seconds are the trim the playhead went through (not the old 29.0 s), the rest left out");
        Check(Has(text, "  compared in slow motion (") && Has(text, "playhead down to x0.20): 45.5 s to 47.5 s"),
            "item 5: slow motion is compared and shown with its span");
        Check(Has(text, "  left out, never reached by the playhead (outside the clip's trim, or not played before Stop) (") &&
                Has(text, "0.0 s to 42.2 s; 71.2 s to 208.0 s"),
            "item 5: the never reached spans with their times");
        Check(Has(text, "slow motion included; a sound counts once per pass"), "item 5: the rule in the text");
        ReportHeader header;
        header.coverage = text;
        DiffResult empty;
        empty.totals.recordedCensus = true;
        const std::string report = FormatReport(empty, header);
        Check(Has(report, "Compared span: 29.0 s") && !Has(report, "only what played at normal speed"),
            "item 5: the block replaces the old one line");
        Check(ClassOfWindow(kCoverPaused, false) == CoverClass::Stood && ClassOfWindow(0, false) == CoverClass::Never &&
                ClassOfWindow(kCoverNormal | kCoverSlow, true) == CoverClass::Slow,
            "item 5: window classes");
    }

    // ------------------------------------------------------------ ESND track

    void TestEntitySoundTally()
    {
        using namespace replay::esndtally;
        Tally tally;
        // The skid of 036: horse 0:1, call site 0x2588C2F, sets 0x2f4361 and
        // 0x2f5642 entry 0x0E, in slow motion.
        const std::uint32_t horse = 1u; // 0:1
        const FamilyKey skid = KeyOf(horse, 0x2588c2f, 0x80000e002f4361ull);
        Check(skid.set == 0x2f4361 && skid.caller == 0x2588c2f, "item 6: the family key");
        Check(KeyOf(horse, 0x2588c2f, 0x800019002f4361ull).set == skid.set, "item 6: entries of one set share the family");
        for (int n = 0; n < 50; ++n)
            tally.Note(skid, Outcome::Played, 45541 + n * 30, true);
        for (int n = 0; n < 5; ++n)
            tally.Note(skid, Outcome::ByPuppet, 46500 + n * 30, true);
        for (int n = 0; n < 48; ++n)
            tally.LengthStop(skid);
        const FamilyKey slide = KeyOf(horse, 0x2588c2f, 0x80000e002f5642ull);
        tally.Note(slide, Outcome::Refused, 45640, true, "not_created");
        tally.Note(slide, Outcome::Passed, 45659, true);
        const Tally::Totals sum = tally.Sum();
        Check(sum.due == 57 && sum.slow == 57 && sum.outcomes[0] == 50 && sum.outcomes[1] == 5 && sum.outcomes[2] == 1 &&
                sum.outcomes[4] == 1 && sum.lengthStops == 48,
            "item 6: totals");
        const std::string section = FormatSection(tally);
        Check(Has(section, "== entity sounds per family (ESND track: persistent sounds of an entity's own audio) ==\n"),
            "item 6: the header");
        Check(Has(section, "Rows due 57 (57 in slow motion): played 50, left to the puppet 5, refused 1, too far 0, passed without a start 1, to the generic player 0; cut at their recorded length 48, ended by the engine 0."),
            "item 6: the totals line");
        const std::size_t shortAt = section.find("SHORT   owner 0:1  caller 0x2588c2f set 0x2f5642");
        const std::size_t okAt = section.find("ok      owner 0:1  caller 0x2588c2f set 0x2f4361  due 55 (55 in slow motion), played 50, by the puppet 5");
        Check(shortAt != std::string::npos && okAt != std::string::npos && shortAt < okAt,
            "item 6: a family with a problem first, then the others");
        Check(Has(section, "reasons: not_created 1"), "item 6: the refusal status");
        Check(Has(section, "cut at recorded length 48, ended by the engine 0; 45.5 s to 47.0 s"), "item 6: the stops and the span");
        const std::string log = LogLine(skid, tally.Families().at(skid));
        Check(log.rfind("POSE_V2_ENTITY_SOUND_FAMILY,id=0:1,caller=0x2588c2f,set=0x2f4361,due=55,slow=55,played=50,by_puppet=5,", 0) == 0,
            "item 6: the log line");
        Check(FormatSection(Tally{}).empty(), "item 6: nothing to tell, no section");
    }
}

int RunSoundCheck036Tests()
{
    g_failures036 = 0;
    TestAnimalLineOnce();
    TestSpeechControllerRows();
    TestSameRequestVariant();
    TestUnplacedWorldSounds();
    TestCoverage();
    TestEntitySoundTally();
    if (g_failures036 == 0)
        std::puts("Sound check 036 tests passed.");
    return g_failures036;
}
