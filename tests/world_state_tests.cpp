// The clip's map state (world_state.h, world_notice.h, ipl_candidates.h): the
// candidate list, the camp check, the state at a time, the cut and the buffer
// trim, the area test, the restore decision and the notice text.
#include "ipl_candidates.h"
#include "replay_buffer.h"
#include "world_notice.h"
#include "world_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using namespace replay::world;

    int g_worldFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_worldFailures;
    }

    EntitySetChange Set(std::uint32_t timeMs, std::uint16_t interior, const char* name, bool active)
    {
        EntitySetChange change;
        change.timeMs = timeMs;
        change.interior = interior;
        change.active = active;
        std::snprintf(change.name, sizeof(change.name), "%s", name);
        return change;
    }

    void TestCandidates()
    {
        Check(kIplCandidateCount > 1000, "candidates: more than a thousand hashes from the scripts");
        Check(kIplDirectLiteralCount > 100 && kIplDirectLiteralCount < kIplCandidateCount, "candidates: direct literals counted");
        bool sorted = true;
        for (std::size_t n = 1; n < kIplCandidateCount; ++n)
            sorted = sorted && kIplCandidates[n - 1] < kIplCandidates[n];
        Check(sorted, "candidates: sorted and unique");
        const auto has = [](std::uint32_t hash) {
            return std::binary_search(kIplCandidates, kIplCandidates + kIplCandidateCount, hash);
        };
        Check(has(Joaat("DES_STR_JAIL")), "candidates: DES_STR_JAIL (the bench's toggle)");
        bool signatures = true;
        for (const CampSignature& signature : kCampSignatures)
            signatures = signatures && has(signature.hash);
        Check(signatures, "candidates: every camp signature IPL is a candidate");
        Check(Joaat("player_zero") == 0x0D7114C9u, "joaat matches the game's hash");
        Check(kEntitySetNameCount > 100, "entity set names from the scripts");
        bool beecher = false;
        for (std::size_t n = 0; n < kEntitySetNameCount; ++n)
            beecher = beecher || std::strcmp(kEntitySetNames[n], "bee_01_house_stage06") == 0;
        Check(beecher, "entity set names: Beecher's Hope house stage");
    }

    void TestCamp()
    {
        int none[9] = {};
        CampVerdict v = JudgeCamp(true, 2, none);
        Check(v.camp == 2 && v.chapter == 3 && (v.flags & kFlagCampRead) && !(v.flags & kFlagCampConfirmed),
            "camp: global alone, no camp IPL on: kept, unconfirmed");
        int clemens[9] = {};
        clemens[2] = 5;
        v = JudgeCamp(true, 2, clemens);
        Check(v.camp == 2 && v.chapter == 3 && (v.flags & kFlagCampConfirmed), "camp: global and its IPLs agree");
        v = JudgeCamp(true, 1, clemens);
        Check(v.camp == kCampUnknown && v.chapter == kChapterUnknown, "camp: a later camp's IPLs on: unknown");
        v = JudgeCamp(true, 3, clemens);
        Check(v.camp == 3 && v.chapter == 4 && !(v.flags & kFlagCampConfirmed), "camp: an earlier camp's IPLs left on: kept");
        int shadyBelle[9] = {};
        shadyBelle[3] = 1;
        v = JudgeCamp(true, 8, shadyBelle);
        Check(v.camp == 8 && v.chapter == kChapterEpilogue2, "camp: live 100% save (global 8, Shady Belle IPLs on): epilogue 2");
        v = JudgeCamp(false, -1, clemens);
        Check(v.camp == 2 && (v.flags & kFlagCampFromIpls) && v.chapter == 3, "camp: global unreadable, from the IPLs");
        int two[9] = {};
        two[1] = 1;
        two[6] = 2;
        v = JudgeCamp(false, -1, two);
        Check(v.camp == kCampUnknown, "camp: two camps' IPLs on and no global: unknown");
        v = JudgeCamp(true, 42, none);
        Check(v.camp == kCampUnknown && !(v.flags & kFlagCampRead), "camp: a value out of 0..8 is not a camp");
        v = JudgeCamp(true, 4, none);
        Check(v.camp == 4 && v.chapter == 5 && (v.flags & kFlagGuarma), "camp: Guarma flagged");
        Check(ChapterFromCamp(0) == 1 && ChapterFromCamp(5) == 6 && ChapterFromCamp(6) == 6 &&
                ChapterFromCamp(7) == kChapterEpilogue1 && ChapterFromCamp(8) == kChapterEpilogue2 &&
                ChapterFromCamp(kCampUnknown) == kChapterUnknown,
            "camp to chapter");
    }

    void TestNotice()
    {
        char text[160]{};
        Check(NoticeText(3, 5, 0, false, text, sizeof(text)) &&
                std::string(text) == "Recorded in Chapter 3, you are in Chapter 5: the world may look different",
            "notice: chapter differs");
        Check(NoticeText(3, 5, 4, true, text, sizeof(text)) &&
                std::string(text) == "Recorded in Chapter 3, you are in Chapter 5: the world was restored for this replay",
            "notice: chapter differs, restored");
        Check(NoticeText(kChapterEpilogue2, 2, 1, false, text, sizeof(text)) &&
                std::string(text) == "Recorded in Epilogue 2, you are in Chapter 2: the world may look different",
            "notice: epilogue name");
        Check(NoticeText(4, 4, 2, false, text, sizeof(text)) &&
                std::string(text) == "Recorded at a different story point: some buildings may differ",
            "notice: same chapter, the area differs");
        Check(NoticeText(kChapterUnknown, 4, 2, true, text, sizeof(text)) &&
                std::string(text) == "Recorded at a different story point: the world was restored for this replay",
            "notice: unknown chapter, restored");
        Check(!NoticeText(4, 4, 0, false, text, sizeof(text)) && text[0] == '\0', "notice: nothing to say");
        Check(!NoticeText(kChapterUnknown, 4, 0, false, text, sizeof(text)), "notice: unknown chapter alone says nothing");
        char name[24]{};
        ChapterName(kChapterEpilogue1, name, sizeof(name));
        Check(std::string(name) == "Epilogue 1", "chapter name: epilogue");
        ChapterName(kChapterUnknown, name, sizeof(name));
        Check(name[0] == '\0', "chapter name: unknown is empty");
    }

    WorldState SampleWorld()
    {
        WorldState world;
        world.recorded = true;
        world.camp = 2;
        world.chapter = 3;
        world.ipls = {{0, 10u, true}, {0, 20u, true}, {0, 30u, true}, {500, 20u, false}, {800, 40u, true},
            {1200, 20u, true}, {1500, 10u, false}};
        world.spheres = {{10u, {0, 0, 0}, 5.0f}, {20u, {1, 1, 1}, 5.0f}, {30u, {2, 2, 2}, 5.0f}, {40u, {3, 3, 3}, 5.0f}};
        world.interiors = {{0xABCDu, {100.0f, 200.0f, 30.0f}}};
        world.entitySets = {Set(0, 0, "bee_01_house_stage06", true), Set(700, 0, "bee_01_masterBR_bed01", false),
            Set(900, 0, "BEE_01_HOUSE_STAGE06", false), Set(1300, 0, "bee_01_masterBR_bed01", true)};
        return world;
    }

    void TestStateAt()
    {
        const WorldState world = SampleWorld();
        Check(IplsOnAt(world, 0) == std::vector<std::uint32_t>({10u, 20u, 30u}), "state: the start");
        Check(IplsOnAt(world, 600) == std::vector<std::uint32_t>({10u, 30u}), "state: an IPL turned off");
        Check(IplsOnAt(world, 1300) == std::vector<std::uint32_t>({10u, 20u, 30u, 40u}), "state: on again, a new one");
        Check(IplsOnAt(world, 2000) == std::vector<std::uint32_t>({20u, 30u, 40u}), "state: the end");
        Check(!IplOnAt(world, 40u, 799) && IplOnAt(world, 40u, 800), "state: an IPL without an event before is off");
        std::vector<SetState> sets = EntitySetsAt(world, 0);
        Check(sets.size() == 2 && sets[0].active && !sets[1].active,
            "sets: a set found later holds its first state before it (case folded key)");
        sets = EntitySetsAt(world, 1000);
        Check(sets.size() == 2 && !sets[0].active && !sets[1].active, "sets: changes by then");
        sets = EntitySetsAt(world, 1400);
        Check(sets.size() == 2 && !sets[0].active && sets[1].active, "sets: the last change");
        Check(TimelineIndex(world, 0) == 0 && TimelineIndex(world, 600) == 1 && TimelineIndex(world, 1000) == 4 &&
                TimelineIndex(world, 99999) == 7,
            "timeline index: changes after the start crossed");
        Check(TimedIpls(world) == std::vector<std::uint32_t>({10u, 20u, 40u}), "timed IPLs");
        WorldState same = world;
        Check(Fingerprint(same) == Fingerprint(world), "fingerprint: stable");
        same.ipls[2].hash = 31u;
        Check(Fingerprint(same) != Fingerprint(world), "fingerprint: moves with the start state");
        Check(FindSphere(world, 30u) && FindSphere(world, 30u)->center[0] == 2.0f && !FindSphere(world, 99u),
            "spheres: found by hash");
        WorldState grow;
        AddSphere(grow, {50u, {}, 1.0f});
        AddSphere(grow, {5u, {}, 1.0f});
        AddSphere(grow, {50u, {}, 2.0f});
        Check(grow.spheres.size() == 2 && grow.spheres[0].hash == 5u && grow.spheres[1].radius == 2.0f,
            "spheres: kept sorted, replaced by hash");
    }

    void TestCutAndTrim()
    {
        const WorldState world = SampleWorld();
        WorldState cut;
        CutWorld(world, 600, 1400, cut);
        Check(cut.recorded && cut.camp == 2 && cut.chapter == 3 && cut.interiors.size() == 1, "cut: info kept");
        Check(IplsOnAt(cut, 0) == IplsOnAt(world, 600), "cut: the state at base is the clip start");
        Check(IplsOnAt(cut, 800 - 600) == IplsOnAt(world, 800), "cut: a change inside the window, shifted");
        Check(IplsOnAt(cut, 1400 - 600) == IplsOnAt(world, 1400), "cut: at the end");
        bool noOffAtZero = true, noneAfterEnd = true;
        for (const IplChange& change : cut.ipls)
        {
            noOffAtZero = noOffAtZero && !(change.timeMs == 0 && !change.active);
            noneAfterEnd = noneAfterEnd && change.timeMs <= 800;
        }
        Check(noOffAtZero, "cut: an 'off' folded to the start is dropped (off by default)");
        Check(noneAfterEnd, "cut: nothing after the end (the 1500 ms change goes)");
        std::vector<SetState> sets = EntitySetsAt(cut, 0);
        Check(sets.size() == 2 && sets[0].active && !sets[1].active, "cut: the sets at base");
        sets = EntitySetsAt(cut, 1300 - 600);
        Check(sets.size() == 2 && !sets[0].active && sets[1].active, "cut: set changes inside the window, shifted");
        Check(!FindSphere(cut, 99u) && FindSphere(cut, 40u), "cut: spheres of the kept IPLs");

        WorldState trimmed = world;
        const std::size_t dropped = TrimWorldBefore(trimmed, 1000);
        Check(dropped > 0, "trim: events before the head go");
        for (const std::uint32_t t : {1000u, 1200u, 1500u, 5000u})
            Check(IplsOnAt(trimmed, t) == IplsOnAt(world, t), "trim: the state from the head on is unchanged");
        const auto late = std::find_if(trimmed.ipls.begin(), trimmed.ipls.end(), [](const IplChange& c) { return c.hash == 40u; });
        Check(late != trimmed.ipls.end() && late->timeMs == 800, "trim: times stay on the recording clock");
        // Compared by key: the order of first appearance may change.
        const auto byKey = [](const std::vector<SetState>& sets) {
            std::vector<std::pair<std::string, bool>> out;
            for (const SetState& set : sets)
                out.emplace_back(SetKey(set.interior, set.name), set.active);
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto before = byKey(EntitySetsAt(world, 1300));
        const auto after = byKey(EntitySetsAt(trimmed, 1300));
        Check(after.size() == 2 && before == after, "trim: set state after the head unchanged");
        // Trim, then cut at a later base: the same as cutting the original.
        WorldState both, direct;
        CutWorld(trimmed, 1100, 2000, both);
        CutWorld(world, 1100, 2000, direct);
        Check(IplsOnAt(both, 0) == IplsOnAt(direct, 0) && IplsOnAt(both, 900) == IplsOnAt(direct, 900),
            "trim then cut equals cut");

        // The ring (replay_buffer.h) trims the map state with the rest.
        Recording clip;
        clip.world = world;
        clip.lifecycle.Begin(500); // the block holding the head starts at it
        for (std::uint32_t t = 0; t <= 1500; t += 500)
            clip.lifecycle.Advance(t);
        const buffer::TrimStats stats = buffer::TrimBefore(clip, 1000);
        Check(stats.worldEvents == dropped && IplsOnAt(clip.world, 1200) == IplsOnAt(world, 1200),
            "buffer trim: the map state keeps the state at the head");
    }

    void TestArea()
    {
        Recording clip;
        Check(!ClipArea(clip).valid, "area: nothing recorded, no area");
        // Away from the map origin (cliparea::PositionValid refuses a
        // position within 5 m of it); the same box shifted 1000 m north.
        clip.ExtendArea(0.0f, 0.0f, 0.0f);
        Check(!clip.hasArea && clip.areaRejected == 1, "area: the map origin is refused");
        clip.ExtendArea(0.0f, 1000.0f, 0.0f);
        clip.ExtendArea(10.0f, 1020.0f, 5.0f);
        Box box = ClipArea(clip, 100.0f);
        Check(box.valid && box.min[0] == -100.0f && box.max[1] == 1120.0f, "area: recorded area plus margin");
        const float nearPoint[3] = {150.0f, 1010.0f, 0.0f};
        const float farPoint[3] = {400.0f, 1010.0f, 0.0f};
        Check(SphereTouches(box, nearPoint, 60.0f) && !SphereTouches(box, nearPoint, 35.0f), "area: a sphere reaching in");
        Check(!SphereTouches(box, farPoint, 100.0f), "area: a far sphere");
        Check(PointInside(box, {5.0f, 1005.0f, 5.0f}) && !PointInside(box, farPoint), "area: point inside");
        camera::GameSample sample{};
        sample.position = {1000.0f, 1000.0f, 0.0f};
        clip.gameCamera.push_back(sample);
        box = ClipArea(clip, 100.0f);
        Check(box.max[0] == 1100.0f, "area: the game camera counts");
    }

    void TestRestoreDecision()
    {
        Check(ShouldRestore(true, true, false), "restore: still as the replay left it");
        Check(!ShouldRestore(false, true, false), "restore: the world moved on (a save loaded): left alone");
        Check(!ShouldRestore(false, false, false), "restore: already as it was");
    }
}

namespace
{
    // The environment track (environment_track.h).
    void TestEnvironmentTrack()
    {
        std::vector<EnvironmentSample> track;
        EnvironmentSample s;
        s.timeMs = 0;
        s.weatherFrom = s.weatherTo = 1;
        s.clockS = 86400 - 20;
        Check(envtrack::Add(track, s), "env: first sample kept");
        s.timeMs = 250;
        s.clockS = 86400 - 15;
        Check(!envtrack::Add(track, s), "env: a small clock step is not a change");
        s.timeMs = 500;
        s.weatherTo = 2;
        s.weatherMix = 0.0f;
        s.clockS = 86400 - 10;
        Check(envtrack::Add(track, s), "env: a new weather pair is a change");
        s.timeMs = 1500;
        s.weatherMix = 1.0f;
        s.rain = 1.0f;
        s.clockS = 10; // across midnight
        Check(envtrack::Add(track, s), "env: the mix moved");
        s.timeMs = 1000;
        Check(!envtrack::Add(track, s), "env: time never goes back");

        EnvironmentSample at;
        Check(envtrack::At(track, 1000, at) && at.weatherTo == 2 && at.weatherMix > 0.49f && at.weatherMix < 0.51f &&
                  at.rain > 0.49f && at.rain < 0.51f && at.clockS == 0,
            "env: interpolated inside a pair, clock across midnight");
        Check(envtrack::At(track, 400, at) && at.weatherTo == 1 && at.rain == 0.0f,
            "env: no blend across a change of pair");
        Check(envtrack::At(track, 9000, at) && at.weatherMix == 1.0f && at.clockS == 10, "env: after the last sample");

        const std::vector<EnvironmentSample> cut = envtrack::Cut(track, 1000, 1400);
        Check(cut.size() == 1 && cut[0].timeMs == 0 && cut[0].weatherMix > 0.49f, "env: cut keeps the state at its base");
        std::vector<EnvironmentSample> trimmed = track;
        Check(envtrack::DropBefore(trimmed, 1000) == 1 && trimmed.size() == 2 && trimmed[0].timeMs == 500,
            "env: trim keeps the sample in force");
    }
}

int RunWorldStateTests()
{
    TestEnvironmentTrack();
    TestCandidates();
    TestCamp();
    TestNotice();
    TestStateAt();
    TestCutAndTrim();
    TestArea();
    TestRestoreDecision();
    if (g_worldFailures == 0)
        std::puts("World state tests passed.");
    return g_worldFailures;
}
