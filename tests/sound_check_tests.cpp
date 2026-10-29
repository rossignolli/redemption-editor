// The always-on sound check and the census-level capture, pure
// part (sound_check_core.h): the summary a recording keeps, its cut and
// drop, the comparison with a playback on synthetic data, the report and
// summary line, the anti-double rule, the capture's choices and the cost of
// counting rows.
#include "sound_check_core.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay::census;

    int g_checkFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_checkFailures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint32_t kTrain = (0u << 16) | 912u;
    constexpr std::uint32_t kCar = (1u << 16) | 913u;
    constexpr std::uint32_t kArthur = (2u << 16) | 72u;
    constexpr std::uint32_t kHorse = (1u << 16) | 71u;
    constexpr std::uint64_t kWhistle = 0xfffff01170670ull;
    constexpr std::uint64_t kEngine = 0x8000030011bf0cdull;
    constexpr std::uint64_t kHoof = 0x800004004366e8ull;
    constexpr std::uint64_t kCloth = 0x80000200436739ull;
    constexpr std::uint64_t kWind = 0x1e4a8ull;

    SummaryKey Key(std::uint32_t owner, OwnerClass cls, std::uint64_t ref, std::uint32_t caller = 0x2f1d23e)
    {
        SummaryKey key;
        key.owner = owner;
        key.cls = cls;
        key.ref = ref;
        key.path[0] = 0x2588eda;
        key.path[1] = 0x258a469;
        key.path[2] = caller;
        return key;
    }

    // A summary from (ms, key) rows, covered from 0 to coverMs.
    struct Rows
    {
        Summary summary;
        Accumulator acc;
        explicit Rows(bool play) { acc.Attach(&summary, play); }
        void Add(double ms, const SummaryKey& key, bool refused = false) { acc.Add(ms, key, refused); }
        void Cover(double from, double to) { acc.Cover(from, to); }
    };

    void TestAccumulator()
    {
        Rows rec(false);
        rec.Cover(0, 0);
        for (int ms = 0; ms <= 2000; ms += 33)
            rec.Cover(ms > 33 ? ms - 33 : 0, ms);
        rec.Add(10, Key(kTrain, OwnerClass::Train, kWhistle));
        rec.Add(120, Key(kTrain, OwnerClass::Train, kWhistle));
        rec.Add(260, Key(kTrain, OwnerClass::Train, kWhistle));
        rec.Add(270, Key(kArthur, OwnerClass::Player, kCloth), true);
        rec.Add(240, Key(kTrain, OwnerClass::Train, kWhistle)); // a row a bit older than the open window
        const Summary& s = rec.summary;
        Check(s.keys.size() == 2, "accumulator: one key per owner, ref and path");
        Check(s.cells.size() == 3 && s.cells[0].window == 0 && s.cells[0].count == 3 && s.cells[1].window == 1 &&
                s.cells[1].count == 1 && s.cells[2].refused == 1,
            "accumulator: counts per window, the older row in its own window");
        bool ascending = true;
        for (std::size_t n = 1; n < s.cells.size(); ++n)
            ascending = ascending && s.cells[n - 1].window <= s.cells[n].window;
        Check(ascending, "accumulator: a recording's cells stay window ascending");
        Check(s.covered.size() == 8 && s.covered.front() == 0 && s.covered.back() == 7,
            "accumulator: 0 to 1980 ms covers windows 0 to 7");

        // A playback: two passes over the first second (a scrub back).
        Rows play(true);
        for (int ms = 0; ms <= 1000; ms += 33)
            play.Cover(ms > 33 ? ms - 33 : 0, ms);
        play.acc.Break();
        for (int ms = 0; ms <= 500; ms += 33)
            play.Cover(ms > 33 ? ms - 33 : 0, ms);
        const auto& passes = play.acc.Passes();
        Check(passes.count(0) && passes.at(0) == 2 && passes.count(3) && passes.at(3) == 1,
            "accumulator: a playback counts its passes per window");
        play.Add(100, Key(kTrain, OwnerClass::Train, kWhistle));
        play.Add(3000, Key(kTrain, OwnerClass::Train, kWhistle));
        play.Add(150, Key(kTrain, OwnerClass::Train, kWhistle));
        Check(play.summary.cells.size() == 2 && play.summary.cells[0].count == 2,
            "accumulator: a playback indexes every window (rows out of order)");

        // The table moving under the accumulator (buffer drop, compaction).
        rec.summary.DropBefore(250);
        rec.Add(2100, Key(kHorse, OwnerClass::Horse, kHoof));
        Check(rec.summary.cells.back().window == 8 && rec.summary.keys[rec.summary.cells.back().key].ref == kHoof,
            "accumulator: rows after a drop land on the moved table");
    }

    void TestCutDrop()
    {
        Summary s;
        Accumulator acc;
        acc.Attach(&s, false);
        for (int ms = 0; ms <= 10000; ms += 250)
            acc.Cover(ms, ms);
        for (int ms = 0; ms < 10000; ms += 500)
            acc.Add(ms + 10, Key(kTrain, OwnerClass::Train, kEngine), false);
        acc.Add(9000, Key(kCar, OwnerClass::Train, kWhistle), false);
        Summary cut;
        s.CutInto(cut, 4000, 8000);
        Check(cut.covered.front() == 0 && cut.covered.back() == 16, "cut: covered windows move to the clip's 0");
        Check(cut.keys.size() == 1 && cut.keys[0].ref == kEngine, "cut: only the keys inside the window");
        Check(cut.cells.front().window == 0 && cut.cells.size() == 9, "cut: the first cell at base is window 0");
        Summary shifted;
        s.CutInto(shifted, 4100, 8000);
        Check(shifted.cells.front().window == 0 && shifted.cells[1].window == 2,
            "cut: a base inside a window keeps each window once (by its middle)");
        s.DropBefore(6000);
        Check(s.cells.front().window == 24 && s.covered.front() == 24, "drop: windows before the base go");
        std::vector<SummaryKey> many;
        Summary big;
        Accumulator bigAcc;
        bigAcc.Attach(&big, false);
        for (int n = 0; n < 1500; ++n)
            bigAcc.Add(n * 10.0, Key(kArthur, OwnerClass::Ped, 0x1000 + n), false);
        big.DropBefore(14000);
        Check(big.keys.size() == 100 && big.cells.size() == 100 && big.keys[big.cells[0].key].ref == 0x1000 + 1400,
            "drop: a large key table is compacted to the keys still used");
    }

    struct Pair
    {
        Rows rec{false};
        Rows play{true};
        void CoverBoth(double from, double to)
        {
            for (double ms = from; ms <= to; ms += 33)
            {
                rec.Cover(ms - 33 < from ? from : ms - 33, ms);
                play.Cover(ms - 33 < from ? from : ms - 33, ms);
            }
        }
        DiffResult Run(const IgnoreList& ignore = IgnoreList{}) const
        {
            return Diff(rec.summary, play.summary, play.acc.Passes(), ignore);
        }
    };

    const Finding* Find(const DiffResult& diff, FindingKind kind, std::uint64_t ref)
    {
        for (const Finding& f : diff.findings)
            if (f.kind == kind && f.ref == ref)
                return &f;
        return nullptr;
    }

    void TestDiff()
    {
        Pair p;
        p.CoverBoth(0, 10000);
        // The train: whistle 3 times in the recording, never in the replay;
        // its engine loop 10 times recorded, 2 played.
        for (double ms : {1000.0, 4000.0, 7000.0})
            p.rec.Add(ms, Key(kTrain, OwnerClass::Train, kWhistle));
        for (int n = 0; n < 10; ++n)
            p.rec.Add(500.0 + n * 900.0, Key(kTrain, OwnerClass::Train, kEngine));
        p.play.Add(500.0, Key(kTrain, OwnerClass::Train, kEngine));
        p.play.Add(1400.0, Key(kTrain, OwnerClass::Train, kEngine));
        // The horse: hooves matched a window late (tolerance), cloth only
        // in the replay, 5 times.
        for (int n = 0; n < 8; ++n)
        {
            p.rec.Add(2000.0 + n * 400.0, Key(kHorse, OwnerClass::Horse, kHoof));
            p.play.Add(2000.0 + n * 400.0 + 240.0, Key(kHorse, OwnerClass::Horse, kHoof));
        }
        for (int n = 0; n < 5; ++n)
            p.play.Add(3000.0 + n * 100.0, Key(kHorse, OwnerClass::Horse, kCloth));
        // Arthur: 2 cloth sounds recorded, 9 in the replay (more).
        p.rec.Add(6000.0, Key(kArthur, OwnerClass::Player, 0x80000300000dd1b6ull));
        p.rec.Add(6100.0, Key(kArthur, OwnerClass::Player, 0x80000300000dd1b6ull));
        for (int n = 0; n < 9; ++n)
            p.play.Add(6000.0 + n * 50.0, Key(kArthur, OwnerClass::Player, 0x80000300000dd1b6ull));
        // A sound the engine refused in the replay most times.
        for (int n = 0; n < 4; ++n)
        {
            p.rec.Add(8000.0 + n * 100.0, Key(kCar, OwnerClass::Train, 0x77));
            p.play.Add(8000.0 + n * 100.0, Key(kCar, OwnerClass::Train, 0x77), n < 3);
        }
        // A variant: the same sound from another bank.
        p.rec.Add(9000.0, Key(kArthur, OwnerClass::Player, 0x80000200436465ull));
        p.play.Add(9000.0, Key(kArthur, OwnerClass::Player, 0x80000600436465ull));
        p.play.Add(9010.0, Key(kArthur, OwnerClass::Player, 0x80000600436465ull));
        // The world: wind differs, never judged.
        for (int n = 0; n < 20; ++n)
            p.rec.Add(100.0 + n * 400.0, Key(kNoOwner, OwnerClass::World, kWind));
        // Outside the replay's coverage: recorded at 12 s, never compared.
        p.rec.Cover(10000, 13000);
        p.rec.Add(12000.0, Key(kTrain, OwnerClass::Train, 0x99));

        const DiffResult d = p.Run();
        const Finding* whistle = Find(d, FindingKind::Missing, kWhistle);
        Check(whistle && whistle->owner == kTrain && whistle->recorded == 3.0 && whistle->spans.size() == 3 &&
                whistle->spans[0].first == 1000,
            "diff: a recorded sound never played is missing, with its times");
        const Finding* engine = Find(d, FindingKind::Fewer, kEngine);
        Check(engine && engine->recorded == 10.0 && engine->matched == 2.0, "diff: fewer when under half matched");
        Check(!Find(d, FindingKind::Missing, kHoof) && !Find(d, FindingKind::Fewer, kHoof),
            "diff: a window of delay still matches");
        Check(Find(d, FindingKind::Extra, kCloth) != nullptr, "diff: a sound only in the replay is extra");
        Check(Find(d, FindingKind::More, 0x80000300000dd1b6ull) != nullptr, "diff: over twice as many is more");
        const Finding* refused = Find(d, FindingKind::Refused, 0x77);
        Check(refused && refused->refused == 3 && refused->requests == 4, "diff: refused by the engine");
        Check(Find(d, FindingKind::Variant, 0x80000200436465ull) && Find(d, FindingKind::Variant, 0x80000600436465ull),
            "diff: another bank of the same sound is a variant, not missing");
        Check(!Find(d, FindingKind::Missing, kWind) && d.totals.recordedWorld == 20.0, "diff: the world is counted, not judged");
        Check(!Find(d, FindingKind::Missing, 0x99), "diff: what the replay did not cover is not compared");
        Check(d.totals.coveredMs == 10000 && d.totals.recordedMs > d.totals.coveredMs, "diff: covered time");
        Check(d.totals.counted[static_cast<int>(FindingKind::Missing)] == 1 &&
                d.totals.counted[static_cast<int>(FindingKind::Variant)] == 2,
            "diff: counts per kind");
        Check(!d.findings.empty() && d.findings.front().cls == OwnerClass::Train, "diff: the train first in the report");

        // The ignore list (ignore-list classes and refs added by hand).
        IgnoreList ignore = DefaultIgnoreList();
        ignore.AddText("# comment\nentity|0xfffff01170670\r\n0x77 # a note\n");
        const DiffResult quiet = p.Run(ignore);
        Check(!Find(quiet, FindingKind::Missing, kWhistle) && !Find(quiet, FindingKind::Refused, 0x77) &&
                quiet.totals.ignored == 2,
            "diff: keys on the ignore list are left out");

        // Passes: a window played twice with its sound twice is not "more".
        Pair twice;
        twice.CoverBoth(0, 1000);
        twice.play.acc.Break();
        for (double ms = 0; ms <= 1000; ms += 33)
            twice.play.Cover(ms - 33 < 0 ? 0 : ms - 33, ms);
        for (double ms : {300.0, 700.0})
        {
            twice.rec.Add(ms, Key(kCar, OwnerClass::Train, 0x55));
            twice.rec.Add(ms + 10, Key(kCar, OwnerClass::Train, 0x55));
            for (int pass = 0; pass < 2; ++pass)
            {
                twice.play.Add(ms, Key(kCar, OwnerClass::Train, 0x55));
                twice.play.Add(ms + 10, Key(kCar, OwnerClass::Train, 0x55));
            }
        }
        const DiffResult t = twice.Run();
        Check(t.findings.empty(), "diff: counts per pass (a scrubbed window played twice)");

        // No census in the clip.
        Summary empty;
        const DiffResult none = Diff(empty, p.play.summary, p.play.acc.Passes(), IgnoreList{});
        Check(!none.totals.recordedCensus && none.findings.empty(), "diff: a clip without census compares nothing");
    }

    void TestReport()
    {
        Pair p;
        p.CoverBoth(0, 5000);
        for (double ms : {1000.0, 1100.0, 3000.0})
            p.rec.Add(ms, Key(kTrain, OwnerClass::Train, kWhistle));
        p.rec.Add(2000.0, Key(kNoOwner, OwnerClass::World, kWind));
        const DiffResult d = p.Run();
        ReportHeader header;
        header.clip = "clip-20261001-001949.rpclip";
        header.when = "01/10/2026 01:00:00";
        header.version = "0.174.0";
        header.capture = "Sounds captured by the census in this clip: 3.";
        const std::string report = FormatReport(d, header);
        Check(Has(report, "clip-20261001-001949.rpclip") && Has(report, "== train ==") && Has(report, "MISSING") &&
                Has(report, "ref 0xfffff01170670") && Has(report, "owner 0:912") && Has(report, "when: 1.0 s; 3.0 s") &&
                Has(report, "path c=2588eda;258a469;2f1d23e;0;0") && Has(report, "Sounds captured by the census"),
            "report: the train's missing whistle with owner, ref, path and times");
        Check(Has(report, "SUMMARY: 1 sounds missing"), "report: the summary line");
        bool dashes = false;
        for (std::size_t n = 0; n + 2 < report.size(); ++n)
            dashes = dashes || (static_cast<unsigned char>(report[n]) == 0xE2 && static_cast<unsigned char>(report[n + 1]) == 0x80 &&
                (static_cast<unsigned char>(report[n + 2]) == 0x93 || static_cast<unsigned char>(report[n + 2]) == 0x94));
        Check(!dashes, "report: no en or em dash");
        const std::string line = FormatSummaryLine(d, header.clip, "ReplayDebug\\clip-20261001-001949.sound-check.txt");
        Check(line.rfind("SOUND_MISSING_SUMMARY,clip=clip-20261001-001949.rpclip,census=1,missing=1,", 0) == 0 &&
                Has(line, ",covered_ms=") && Has(line, ",top=missing:train:0:912:0xfffff01170670"),
            "summary line: fields and the first finding");
        Summary empty;
        const std::string old = FormatReport(Diff(empty, p.play.summary, p.play.acc.Passes(), IgnoreList{}), header);
        Check(Has(old, "without the sound census"), "report: a clip without census says so");
        Check(SpansText({{1000, 1250}, {2000, 3000}}) == "1.0 s; 2.0 s to 3.0 s", "report: time spans");
    }

    void TestOwnCreates()
    {
        OwnCreates own;
        own.Add(kTrain, kWhistle, 1000);
        Check(own.Take(kTrain, kWhistle, 1100, 1250, 300), "anti-double: the puppet's own create within the window");
        Check(!own.Take(kTrain, kWhistle, 1100, 1250, 300), "anti-double: each create answers for one sound");
        own.Add(kTrain, kWhistle, 700);
        Check(!own.Take(kTrain, kWhistle, 1100, 1250, 300), "anti-double: a create before the window is not it");
        own.Add(kTrain, kEngine, 1400);
        Check(!own.Take(kTrain, kEngine, 1100, 1250, 300), "anti-double: a create after now is not it yet");
        Check(!own.Take(kCar, kEngine, 1100, 1500, 300), "anti-double: another owner's create is not it");
        Check(own.Ever(kTrain, kEngine) && !own.Ever(kCar, kEngine), "anti-double: made at any time (a loop mid-span)");
        own.Forget(5000, 3000);
        Check(own.Size() == 0 && own.Ever(kTrain, kEngine), "anti-double: old creates go, the made-ever list stays");
        own.Add(kTrain, kWhistle, 4000);
        own.Forget(3000, 3000);
        Check(own.Size() == 0, "anti-double: a jump back forgets the creates after it");
    }

    void TestCaptureChoices()
    {
        Check(CaptureClass(OwnerClass::Train, 1) && CaptureClass(OwnerClass::Vehicle, 1) && CaptureClass(OwnerClass::Object, 1),
            "capture: trains, vehicles and objects by default");
        Check(!CaptureClass(OwnerClass::Ped, 1) && !CaptureClass(OwnerClass::Horse, 1) && !CaptureClass(OwnerClass::Weapon, 1) &&
                !CaptureClass(OwnerClass::Player, 1),
            "capture: owners with their own tracks only with SoundCapture=2");
        Check(CaptureClass(OwnerClass::Horse, 2) && !CaptureClass(OwnerClass::World, 2) &&
                !CaptureClass(OwnerClass::LivePlayer, 2) && !CaptureClass(OwnerClass::Train, 0),
            "capture: never the world or the live player; 0 is off");
        const std::uint32_t byRef[kPathDepth] = {0x251688D, 0x2589214, 0, 0x2F2A3F6, 0};
        const std::uint32_t byHash[kPathDepth] = {0x251688D, 0x25890E7, 0, 0, 0};
        const std::uint32_t queue[kPathDepth] = {0x2588EDA, 0x258A469, 0x2F1D23E, 0, 0};
        Check(PersistentCorePath(byRef) && PersistentCorePath(byHash) && !PersistentCorePath(queue),
            "capture: the persistent cores the entity sound track watches");

        // Where the audio entity hangs off the entity.
        std::uint8_t entity[0x1400] = {};
        const std::uint64_t audio = 0x1AC6501FE20ull;
        const std::uint64_t farther = audio - 0x1800;
        const std::uint64_t closer = audio - 0x40;
        std::memcpy(entity + 0x100, &farther, 8);
        std::memcpy(entity + 0x900, &closer, 8);
        const std::uint64_t lowValue = 0x7FF0;
        std::memcpy(entity + 0x10, &lowValue, 8);
        std::uint16_t field = 0;
        std::int32_t offset = 0;
        Check(FindFieldBase(entity, sizeof(entity), audio, field, offset) && field == 0x900 && offset == 0x40,
            "capture: the closest pointer below the audio entity is its base");
        Check(!FindFieldBase(entity, sizeof(entity), audio + 0x10000, field, offset), "capture: nothing within the span");

        GeneralSound sound;
        sound.entity = kTrain;
        sound.cls = OwnerClass::Train;
        sound.ref = kEngine;
        sound.base = AudioBase::VehicleAudio;
        sound.offset = 0x40;
        sound.vtableRva = 0x37F0000;
        Check(GeneralSoundValid(sound, 0x6000000, kParamsBytes), "capture: a train car sound is valid");
        GeneralSound bad = sound;
        bad.offset = static_cast<std::int32_t>(kVehicleAudioSpan);
        Check(!GeneralSoundValid(bad, 0x6000000, kParamsBytes), "capture: an offset past the base span is refused");
        bad = sound;
        bad.offset = 0x44;
        Check(!GeneralSoundValid(bad, 0x6000000, kParamsBytes), "capture: an unaligned offset is refused");
        bad = sound;
        bad.base = AudioBase::Image;
        bad.offset = 0x7000000;
        Check(!GeneralSoundValid(bad, 0x6000000, kParamsBytes), "capture: an image offset past the image is refused");
        bad = sound;
        bad.params.assign(12, 0);
        Check(!GeneralSoundValid(bad, 0x6000000, kParamsBytes), "capture: a short params block is refused");
        bad = sound;
        bad.entity = kNoOwner;
        Check(!GeneralSoundValid(bad, 0x6000000, kParamsBytes), "capture: a sound of no recorded owner is refused");
    }

    // The cost of the cheap mode's script side: rows counted per second
    // of game (a busy scene: 400 rows a second, 50 owners, 300 refs) and the
    // comparison of a 140 s clip.
    void TestCost()
    {
        Summary s;
        Accumulator acc;
        acc.Attach(&s, false);
        const int rows = 56000; // 140 s at 400 rows a second
        std::vector<SummaryKey> keys;
        for (int n = 0; n < 300; ++n)
            keys.push_back(Key((static_cast<std::uint32_t>(n % 50) << 16) | 7u, OwnerClass::Ped, 0x800000000000ull + n * 977));
        const auto start = std::chrono::steady_clock::now();
        for (int n = 0; n < rows; ++n)
        {
            const double ms = n * 2.5; // 400 rows a second
            acc.Cover(ms, ms);
            acc.Add(ms, keys[(n * 7919) % keys.size()], false);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        const double perRow = us / rows;
        // 400 rows a second at 60 frames a second: under 7 rows a frame.
        const double perFrame = perRow * 400.0 / 60.0;
        std::printf("Sound check cost: %.3f us per row, %.4f ms per frame at 400 rows/s (60 fps), %zu cells, %zu keys\n",
            perRow, perFrame / 1000.0, s.cells.size(), s.keys.size());
        Check(perFrame < 100.0, "cost: the counting stays under 0.1 ms a frame at 400 rows a second");

        Rows play(true);
        for (double ms = 0; ms < 140000; ms += 16.7)
            play.Cover(ms - 16.7 < 0 ? 0 : ms - 16.7, ms);
        for (int n = 0; n < rows; n += 2)
            play.Add(n * 2.5, keys[(n * 7919) % keys.size()]);
        const auto diffStart = std::chrono::steady_clock::now();
        Summary rec;
        s.CutInto(rec, 0, 140000);
        const DiffResult d = Diff(rec, play.summary, play.acc.Passes(), DefaultIgnoreList());
        const double diffMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diffStart).count();
        std::printf("Sound check report: %.2f ms to compare 140 s (%zu findings)\n", diffMs, d.findings.size());
        Check(diffMs < 50.0, "cost: the end of playback comparison is short");
    }

    // Finding 1 of the release review: a clip's static audio RVA (SNDS
    // globalAudio, ESND audioRva) becomes an engine object only when it is one
    // the recorder writes for this build, with the vtable it has there.
    void TestImageAudio()
    {
        // What the 116 QA clips name: every SNDS globalAudio
        // (kind 3 impacts) and ESND audioRva, and the (RVA, vtable) pairs of
        // their Image based SGEN creates. All of them must stay playable.
        const std::uint32_t named[] = {0x5C7CE90u, 0x5C10540u, 0x5C0F880u, 0x5C505C0u, 0x5BFB5B0u, 0x5C79E80u, // SNDS
            0x5CA7BB0u, 0x5B9F960u}; // ESND (and 0x5C10540)
        bool all = true;
        for (const std::uint32_t rva : named)
        {
            std::uint32_t vtable = 0;
            all = all && KnownImageAudio(rva, vtable) && vtable != 0;
        }
        Check(all, "image audio: every static audio the QA clips name is known");
        const ImageAudio recorded[] = {{0x5CA7BB0u, 0x383F010u}, {0x5C10540u, 0x3832238u}, {0x5C7CE90u, 0x3832148u},
            {0x5C90070u, 0x383D3B0u}, {0x5C0F880u, 0x3834948u}, {0x5B9F960u, 0x3833708u}, {0x5C9A660u, 0x383ECF0u},
            {0x5C505C0u, 0x3832418u}, {0x5C81C20u, 0x383D4A8u}, {0x5C79E80u, 0x38326C8u}, {0x5C77390u, 0x3833618u},
            {0x5C500B0u, 0x3832328u}};
        bool same = true;
        for (const ImageAudio& pair : recorded)
        {
            std::uint32_t vtable = 0;
            same = same && KnownImageAudio(pair.rva, vtable) && vtable == pair.vtableRva;
        }
        Check(same, "image audio: the vtable of each is the one the recorder stored (SGEN)");
        // Each entry: in the image's .data (0x3917000..0x5CB16F8), its vtable in
        // .rdata (0x32D8000..0x39163FF), 8-byte aligned, no duplicate.
        bool shaped = true;
        for (std::size_t n = 0; n < sizeof(kImageAudio) / sizeof(kImageAudio[0]); ++n)
        {
            const ImageAudio& a = kImageAudio[n];
            shaped = shaped && a.rva >= 0x3917000u && a.rva < 0x5CB16F8u && a.vtableRva >= 0x32D8000u &&
                a.vtableRva < 0x3916400u && (a.rva & 7u) == 0 && (a.vtableRva & 7u) == 0;
            for (std::size_t m = 0; m < n; ++m)
                shaped = shaped && kImageAudio[m].rva != a.rva;
        }
        Check(shaped, "image audio: entries are statics of .data with a vtable in .rdata");
        // Anything else is refused: a vtable, code, another static, 0.
        std::uint32_t vtable = 1;
        Check(!KnownImageAudio(0x3430988u, vtable) && vtable == 0 && !KnownImageAudio(0x2589A40u, vtable) &&
                  !KnownImageAudio(0x5C10548u, vtable) && !KnownImageAudio(0, vtable) &&
                  !KnownImageAudio(0x140000000ull + 0x5C10540u, vtable),
            "image audio: an RVA the recorder does not write is refused");
    }
}

int RunSoundCheckTests()
{
    TestImageAudio();
    TestAccumulator();
    TestCutDrop();
    TestDiff();
    TestReport();
    TestOwnCreates();
    TestCaptureChoices();
    TestCost();
    if (g_checkFailures == 0)
        std::printf("Sound check tests passed.\n");
    return g_checkFailures;
}
