// The category state (category_state_core.h: sites, change rules, recorder,
// ring, cut, playback index, keep-live list, the window line), the scene pool
// (mix_scene_pool_core.h: the pointer decode, ready to delete, the drivers and
// their sites, naming, the engine's open scenes), the speech duck's plan
// (speech_duck_core.h) and the mix scene track's additions (mix_scene_core.h:
// the v3 flags, Claim, Wanted with owned and never).
#include "category_state_core.h"
#include "mix_scene_core.h"
#include "mix_scene_pool_core.h"
#include "speech_duck_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_categoryStateFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_categoryStateFailures;
    }

    bool Has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

    // ---- category state ----

    void TestCategorySites()
    {
        using namespace replay::catstate;
        Check(sizeof(kResolverSignature) == 61 && kResolverSignature[57] == 0x0F && kResolverSignature[58] == 0x58 &&
                  kResolverSignature[59] == 0x41 && kResolverSignature[60] == kCatVolume,
            "the resolver's prologue ends with addss xmm0,[rcx+48h] (the controllers' volume read)");
        Check(kResolverSignature[29] == (kCatParent & 0xFF) && kResolverSignature[30] == (kCatParent >> 8),
            "the prologue reads the parent at +0xB0");
        bool targets = true;
        for (const Site& site : kSites)
            if (site.ripAt >= 0 && SiteTarget(site) != site.target)
                targets = false;
        Check(targets, "every rip and call site reaches its target");
        std::size_t calls = 0;
        for (const Site& site : kSites)
            calls += site.ripAt == 1 && site.target == kResolverRva ? 1 : 0;
        Check(calls == 3, "the root, the dynamic hierarchies and the children call the resolver");
        Check(kSites[1].bytes[4] == (kCatIndex & 0xFF) && kSites[1].bytes[5] == (kCatIndex >> 8),
            "the resolved value is stored at the category's own index (+0xB2)");
        Check(kCatBlockSize == 0x46 && kMapEntryStride == 16, "the block and the map entry");
    }

    void TestCategoryValues()
    {
        using namespace replay::catstate;
        Values a;
        Check(a.volume == 0.0f && a.rolloff == 1.0f && a.x54 == 1.0f && a.lpf == 20000 && a.hpf == 0,
            "the default is a category without a controller (0x25395F0..0x253960C)");
        Values b = a;
        b.volume = 0.04f;
        Check(!Changed(a, b), "0.04 dB is no change");
        b.volume = -0.06f;
        Check(Changed(a, b), "0.06 dB is a change");
        b = a;
        b.lpf = 19900;
        Check(!Changed(a, b), "a low pass 0.5 % lower is no change");
        b.lpf = 19000;
        Check(Changed(a, b), "a low pass 5 % lower is a change");
        b = a;
        b.pitch = 1;
        Check(Changed(a, b), "any pitch change");
        b = a;
        b.volume = NAN;
        Check(!PlausibleValues(b), "a NaN volume is not plausible");
        Check(ParseFieldsAll("all") && ParseFieldsAll("ALL") && !ParseFieldsAll("volume") && !ParseFieldsAll(nullptr),
            "CategoryStateFields");
    }

    void TestRecorder()
    {
        using namespace replay::catstate;
        Track track;
        Recorder rec;
        Values v;
        v.volume = -3.0f;
        Check(rec.Offer(track, 0x11u, v, 0) && rec.Offer(track, 0x22u, Values{}, 0), "first sight stored");
        Check(!rec.Offer(track, 0x11u, v, 50), "no change: not stored");
        v.volume = -6.0f;
        Check(!rec.Offer(track, 0x11u, v, 20), "a change inside 50 ms waits");
        Check(rec.Offer(track, 0x11u, v, 60), "and is stored at the first offer after it");
        Check(track.samples.size() == 3 && track.samples[2].timeMs == 60 && track.samples[2].v.volume == -6.0f, "the samples");
        Check(!rec.Offer(track, 0, v, 100), "hash 0 refused");
        // A clock that went back drops the newer tail and stores everything again.
        Check(rec.Offer(track, 0x22u, Values{}, 30) && track.samples.size() == 3 && track.samples.back().timeMs == 30,
            "rewind: the tail after 30 goes, the category is stored again");
        Check(track.Valid(), "valid");
        Check(track.Categories() == 2, "two categories");
    }

    void TestTrackRingAndCut()
    {
        using namespace replay::catstate;
        Track track;
        track.staticCount = 300;
        track.mapCount = 320;
        Values v;
        auto add = [&track](std::uint32_t t, std::uint32_t h, float db) {
            Values x;
            x.volume = db;
            track.samples.push_back(Sample{t, h, x});
        };
        add(0, 0xA, -1.0f);
        add(0, 0xB, -2.0f);
        add(1000, 0xA, -4.0f);
        add(3000, 0xB, -8.0f);
        add(5000, 0xA, -9.0f);
        Track cut;
        track.CutInto(cut, 2000, 4000);
        Check(cut.staticCount == 300 && cut.mapCount == 320, "the cut keeps the counts");
        Check(cut.samples.size() == 3 && cut.samples[0].timeMs == 0 && cut.samples[1].timeMs == 0 &&
                  cut.samples[2].timeMs == 1000 && cut.samples[2].hash == 0xB && cut.samples[2].v.volume == -8.0f,
            "the values in force at the base move to 0, the change inside follows");
        PlayIndex index;
        index.Build(cut);
        Check(index.series.size() == 2, "two series");
        for (const Series& s : index.series)
        {
            if (s.hash == 0xA)
                Check(PlayIndex::At(s, 0)->volume == -4.0f && PlayIndex::At(s, 1500)->volume == -4.0f, "A in force");
            if (s.hash == 0xB)
                Check(PlayIndex::At(s, 999)->volume == -2.0f && PlayIndex::At(s, 1000)->volume == -8.0f, "B steps at 1000");
        }
        track.DropBefore(2000);
        Check(track.samples.size() == 4 && track.samples[0].timeMs == 2000 && track.samples[0].v.volume == -4.0f &&
                  track.samples[1].timeMs == 2000 && track.samples[1].v.volume == -2.0f && track.samples[2].timeMs == 3000 &&
                  track.samples[3].timeMs == 5000,
            "the ring keeps each category's value in force at the base");
        Check(track.Valid(), "the ring's result is valid");
        {
            // The ring's trim works on the time-ordered prefix (no sort); a
            // sample exactly at the base stays after the values moved there,
            // so it is the one in force.
            const auto at = [](std::uint32_t t, std::uint32_t h, float db) {
                Sample s;
                s.timeMs = t;
                s.hash = h;
                s.v.volume = db;
                return s;
            };
            Track ring;
            ring.samples = {at(100, 0xA, -1.0f), at(200, 0xB, -2.0f), at(300, 0xA, -3.0f), at(1000, 0xB, -4.0f),
                at(1000, 0xA, -5.0f), at(1500, 0xA, -6.0f)};
            ring.DropBefore(1000);
            Check(ring.samples.size() == 5 && ring.samples[0].timeMs == 1000 && ring.samples[0].hash == 0xA &&
                      ring.samples[0].v.volume == -3.0f && ring.samples[1].hash == 0xB && ring.samples[1].v.volume == -2.0f &&
                      ring.samples[2].v.volume == -4.0f && ring.samples[3].v.volume == -5.0f && ring.samples[4].timeMs == 1500 &&
                      ring.Valid(),
                "the trim keeps the order: the moved values first, then the rest as it was");
            PlayIndex index2;
            index2.Build(ring);
            for (const Series& s : index2.series)
                Check(PlayIndex::At(s, 1000)->volume == (s.hash == 0xA ? -5.0f : -4.0f), "the sample at the base is in force");
            ring.DropBefore(1000);
            Check(ring.samples.size() == 5, "a second trim at the same base changes nothing");
        }
        Track bad;
        bad.samples.push_back(Sample{10, 1, v});
        bad.samples.push_back(Sample{5, 1, v});
        Check(!bad.Valid(), "time going back is invalid");
    }

    void TestKeepLiveAndLines()
    {
        using namespace replay::catstate;
        const std::vector<std::uint32_t> keep = replay::mixscene::ParseHashList(kDefaultKeepLiveText);
        Check(keep.size() == 5 && KeepLive(keep, 0xBAD598C7u) && KeepLive(keep, 0x44E21C90u) && !KeepLive(keep, 0x9BBF7C4u),
            "the default keep-live list (SFX_SLIDER .. BASE)");
        bool same = true;
        for (std::size_t n = 0; n < keep.size(); ++n)
            same = same && keep[n] == kDefaultKeepLive[n];
        Check(same, "the text is the array");
        const std::vector<std::uint32_t> watch = replay::mixscene::ParseHashList(kDefaultWatchText);
        Check(watch.size() == 14 && watch[0] == 75 && watch[13] == 150, "the watched categories (indices)");
        Window w;
        w.calls = 1200;
        w.imposed = 900;
        w.raced = 2;
        w.maxDeltaDb = 3.914f;
        w.maxDeltaIndex = 75;
        const std::string line = WindowLine(12500, w, 412, 5);
        Check(Has(line, "CATEGORY_STATE,phase=play,event=window,clip_ms=12500,published=412,keep_live=5,calls=1200,imposed=900,"
                        "raced=2,faults=0,max_delta_db=3.91,max_delta_cat=75"),
            "the window line");
    }

    // ---- the scene pool ----

    void TestPoolDecode()
    {
        using namespace replay::mixscene::pool;
        const std::uint64_t keys[] = {0x0ull, 0x1234567890ABCDEFull, 0xFFFFFFFFFFFFFFFFull, 0x00000000DEADBEEFull, 0x8000000000000001ull};
        const std::uint64_t pointers[] = {0x00000235A1B2C3D0ull, 0x0000700000000000ull, 0x1ull};
        bool ok = true;
        for (std::uint64_t k : keys)
            for (std::uint64_t p : pointers)
                ok = ok && Decode(Encode(p, k), k) == p;
        Check(ok, "the pool pointer decodes what it encodes");
        // One step by hand: key 0 -> k = 0, shift 1: value v gives ~rol(rol(v, 32), 1).
        const std::uint64_t v = 0x0000000100000002ull;
        Check(Decode(v, 0) == ~Rol(Rol(v, 32), 1), "decode with a zero key");
        bool targets = true;
        for (const CodeSite& site : kSites)
            if (site.ripAt >= 0 && SiteTarget(site) != site.target)
                targets = false;
        Check(targets, "the pool's cells are what the start reads");
        Check(sizeof(kReadyToDeleteSignature) == 93 && kReadyToDeleteSignature[3] == kScenePatches &&
                  kReadyToDeleteSignature[10] == (kSceneSlots2 & 0xFF) && kReadyToDeleteSignature[60] == (kSceneList >> 8) &&
                  kReadyToDeleteSignature[66] == (kSceneListCount & 0xFF) && kReadyToDeleteSignature[92] == 0xC3,
            "ready to delete reads +0x70, +0x170, +0x270 and +0x2F0");
    }

    void TestPoolScene()
    {
        using namespace replay::mixscene::pool;
        std::vector<std::uint8_t> scene(kSceneSize, 0);
        Check(ReadyToDelete(scene.data()), "an empty scene is ready to delete");
        const std::uint64_t patch = 0x123456;
        std::memcpy(scene.data() + kScenePatches + 0x20, &patch, 8);
        Check(!ReadyToDelete(scene.data()) && Patches(scene.data()) == 1, "a patch keeps it");
        std::vector<std::uint8_t> listOnly(kSceneSize, 0);
        const std::int32_t count = 2;
        std::memcpy(listOnly.data() + kSceneListCount, &count, 4);
        std::memcpy(listOnly.data() + kSceneList + 8, &patch, 8);
        Check(!ReadyToDelete(listOnly.data()), "the list keeps it");
        const std::int32_t vars = 2;
        const std::uint32_t name0 = 0x11u, name1 = kApplyVar;
        const float value0 = 0.25f, value1 = 0.75f;
        std::memcpy(scene.data() + kSceneVarCount, &vars, 4);
        std::memcpy(scene.data() + kSceneVars, &name0, 4);
        std::memcpy(scene.data() + kSceneVars + 4, &value0, 4);
        std::memcpy(scene.data() + kSceneVars + 8, &name1, 4);
        std::memcpy(scene.data() + kSceneVars + 12, &value1, 4);
        float apply = 0.0f, missing = 0.0f;
        Check(VarOf(scene.data(), kApplyVar, apply) && apply == 0.75f && !VarOf(scene.data(), 0x22u, missing), "the variables");
        PoolScene direct;
        NameScene(direct, 0, 0x5BA5FE0);
        Check(direct.name == Name::Direct && direct.hash == 0x576520E1u && direct.driver && std::string(direct.driver->name) == "speech_duck",
            "a direct scene is named by its driver's reference");
        PoolScene wrapper;
        NameScene(wrapper, 0x24BFC688u, 0x5BA6038);
        Check(wrapper.name == Name::Wrapper && wrapper.hash == 0x24BFC688u, "+0x328 wins");
        PoolScene nameless;
        NameScene(nameless, 0, 0x5BA5F30);
        Check(nameless.name == Name::None && nameless.hash == 0, "0x5BA5F30 names nothing (the exclusive slot)");
        PoolScene ours = wrapper;
        ours.replayOwned = true;
        const std::vector<PoolScene> scenes = {direct, wrapper, nameless, ours};
        const std::vector<std::uint32_t> open = EngineOpen(scenes);
        Check(open.size() == 2 && open[0] == 0x576520E1u && open[1] == 0x24BFC688u, "the engine's open scenes leave the replay's out");
        // A scene the replay is running (a script scene with the script
        // slot's reference, replayOwned false) is left out by pointer.
        PoolScene script = wrapper;
        script.scene = 0x1234560;
        script.hash = 0x914C7229u;
        PoolScene engineTwin = script;
        engineTwin.scene = 0x7777770;
        const std::vector<PoolScene> withScript = {direct, script, engineTwin};
        const std::vector<std::uint32_t> openRunning = EngineOpen(withScript, {0x1234560});
        Check(openRunning.size() == 2 && openRunning[0] == 0x576520E1u && openRunning[1] == 0x914C7229u,
            "the replay's running scene is not the engine's; an engine scene of the same hash still is");
        Check(EngineOpen(withScript, {}).size() == 3, "no running scene: every named scene");
        Check(Duplicates(scenes, 0x24BFC688u) == 2 && Duplicates(scenes, 0x576520E1u) == 1, "duplicates");
    }

    void TestDrivers()
    {
        using namespace replay::mixscene::pool;
        bool consistent = true;
        std::size_t wrappers = 0, refusable = 0;
        for (const Driver& d : kDrivers)
        {
            consistent = consistent && DriverConsistent(d);
            wrappers += d.via == Via::Wrapper ? 1 : 0;
            refusable += Refusable(d) ? 1 : 0;
            if (d.via == Via::Direct)
                Check(!Refusable(d), "a direct driver is never refused (its start is not hooked)");
        }
        Check(consistent, "every driver's call, reference and hash agree with its bytes");
        Check(kDriverCount == 13 && wrappers == 2 && refusable == 2, "13 drivers, the two player state scenes refusable");
        for (std::size_t i = 0; i < kDriverCount; ++i)
            for (std::size_t j = i + 1; j < kDriverCount; ++j)
                Check(kDrivers[i].refRva != kDrivers[j].refRva && kDrivers[i].hash != kDrivers[j].hash, "unique references and hashes");
        Check(DriverByReturn(0x2F29985) && DriverByReturn(0x2F29985)->hash == 0x24BFC688u, "0x2F29985 is the player state start");
        Check(IsDirectCaller(0x2F2D2AE) && !IsDirectCaller(0x2F29985) && !IsDirectCaller(0x31C88EB), "direct callers");
        for (const Tolerance& t : kTolerances)
            Check(ToleranceTarget(t) == DriverByHash(t.hash)->refRva, "the tolerance reads the driver's reference");
        Check(kTolerances[0].bytes[7] == 0x74 && kTolerances[1].bytes[10] == 0x74, "a je follows each test");
    }

    // ---- the speech duck ----

    void TestDuckSites()
    {
        using namespace replay::speechduck;
        bool targets = true;
        for (const Site& site : kSites)
            if (site.ripAt >= 0 && SiteTarget(site) != site.target)
                targets = false;
        Check(targets, "every duck site reaches its global");
        Check(sizeof(kDriverSignature) == 72 && kDriverSignature[70] == 0x00 && kDriverSignature[66] == 0x0F &&
                  kDriverSignature[67] == 0x85,
            "the driver's prologue up to its first branch");
        const replay::mixscene::pool::Driver* duck = replay::mixscene::pool::DriverByHash(kDuckScene);
        Check(duck && duck->refRva == kSceneRefRva, "the duck's driver keeps it in 0x5BA5FE0");
        Check(replay::mixscene::pool::DriverByHash(kDuck2Scene)->refRva == kScene2RefRva, "the second duck in 0x5BA5FE8");
    }

    void TestDuckPlan()
    {
        using namespace replay::speechduck;
        using replay::mixscene::Instance;
        using replay::mixscene::Kind;
        using replay::mixscene::Track;
        Track empty;
        Check(Plan(empty, 0, kDefaultDerivedFlags).source == Source::None, "no MIXS: the engine's inputs stay");
        Track old;
        Instance convo;
        convo.id = 3;
        convo.hash = 0x5B9DE6B4u;
        convo.kind = Kind::Conversation;
        convo.startMs = 3995;
        convo.stopMs = 19362;
        old.instances.push_back(convo);
        Instance code;
        code.id = 4;
        code.hash = 0x24BFC688u;
        code.startMs = 0;
        old.instances.push_back(code);
        Input in = Plan(old, 5000, kDefaultDerivedFlags);
        Check(in.source == Source::Derived && in.level == 1.0f && in.flags == kFlagEde && !in.level2Known && in.instance == 3,
            "an older clip: level 1 in a recorded conversation, the EDE flag");
        in = Plan(old, 20000, kDefaultDerivedFlags);
        Check(in.source == Source::Derived && in.level == 0.0f && in.instance == 0, "and 0 after it");
        Check(Plan(old, 5000, 0x1F).flags == 0x0F, "the flags are four bits");
        Track v3 = old;
        v3.flags = replay::mixscene::kTrackPoolScanned;
        in = Plan(v3, 5000, kDefaultDerivedFlags);
        Check(in.source == Source::Recorded && in.level == 0.0f && in.level2Known && in.level2 == 0.0f,
            "a v3 clip without the duck scene: the duck is off (a conversation alone does not make it)");
        Instance duck;
        duck.id = 9;
        duck.hash = kDuckScene;
        duck.caller = 0x2F2D2AE;
        duck.startMs = 4000;
        duck.stopMs = 19000;
        v3.instances.push_back(duck);
        v3.vars.push_back(replay::mixscene::VarSample{4000, 9, kApply, 1.0f});
        v3.vars.push_back(replay::mixscene::VarSample{4000, 9, kVarEde, 1.0f});
        v3.vars.push_back(replay::mixscene::VarSample{4000, 9, kVarEdf, 0.0f});
        v3.vars.push_back(replay::mixscene::VarSample{4000, 9, kVarEdc, 1.0f});
        v3.vars.push_back(replay::mixscene::VarSample{6000, 9, kApply, 0.5f});
        Instance duck2;
        duck2.id = 10;
        duck2.hash = kDuck2Scene;
        duck2.startMs = 5000;
        v3.instances.push_back(duck2);
        v3.vars.push_back(replay::mixscene::VarSample{5000, 10, kVarLevel2, 0.3f});
        in = Plan(v3, 4500, kDefaultDerivedFlags);
        Check(in.source == Source::Recorded && in.level == 1.0f && in.flags == (kFlagEde | kFlagEdc) && in.instance == 9 &&
                  in.level2 == 0.0f,
            "the recorded apply and flags");
        in = Plan(v3, 7000, kDefaultDerivedFlags);
        Check(in.level == 0.5f && in.level2 == 0.3f, "the apply follows its variable, the second duck its level");
        in = Plan(v3, 19500, kDefaultDerivedFlags);
        Check(in.level == 0.0f && in.flags == 0 && in.level2 == 0.3f, "after the recorded stop the duck is off");
        // Only the ducks' variables are read (another active scene's apply,
        // here the state scene 4, changes nothing).
        v3.vars.push_back(replay::mixscene::VarSample{4100, 4, kApply, 0.25f});
        in = Plan(v3, 4500, kDefaultDerivedFlags);
        Check(in.level == 1.0f && in.instance == 9 && in.flags == (kFlagEde | kFlagEdc),
            "another active scene's variables are not the duck's");
        Check(FlagsText(kFlagEde) == "0x4", "the flags text");
    }

    // ---- the mix scene track ----

    void TestMixSceneAdditions()
    {
        using namespace replay::mixscene;
        Track track;
        track.flags = kTrackPoolScanned;
        Instance a;
        a.id = 1;
        a.hash = 0x24BFC688u;
        a.caller = 0x2F29985;
        a.kind = Kind::Code;
        a.startMs = 0;
        a.stopMs = 22397;
        track.instances.push_back(a);
        Instance b = a;
        b.id = 2;
        b.startMs = 37051;
        b.stopMs = kOpen;
        track.instances.push_back(b);
        Instance duck;
        duck.id = 3;
        duck.hash = 0x576520E1u;
        duck.kind = Kind::Code;
        duck.startMs = 1000;
        track.instances.push_back(duck);
        Instance perm;
        perm.id = 4;
        perm.hash = 0xA3657C2Cu;
        perm.kind = Kind::Code;
        track.instances.push_back(perm);
        Track cut;
        track.CutInto(cut, 100, 50000);
        Check(cut.flags == kTrackPoolScanned, "the cut keeps the flags");
        track.DropBefore(30000);
        Check(track.flags == kTrackPoolScanned, "the ring keeps the flags");
        Track t2;
        t2.instances = {a, b, duck, perm};
        const std::vector<std::uint32_t> engineOpen = {0x24BFC688u, 0xA3657C2Cu};
        const std::vector<std::uint32_t> owned = {0x24BFC688u};
        const std::vector<std::uint32_t> never = {0x576520E1u};
        std::vector<std::uint32_t> w = Wanted(t2, 1000, 16, kReplayDefault, engineOpen, {}, owned, never);
        Check(w.size() == 1 && w[0] == 1, "owned: started whatever the kinds and the engine; the duck never; the permanent stays the engine's");
        w = Wanted(t2, 30000, 16, kReplayDefault | kReplayCode, engineOpen, {}, owned, never);
        Check(w.empty(), "between the recorded stop and start nothing (the engine's permanent is open)");
        w = Wanted(t2, 30000, 16, kReplayDefault | kReplayCode, {}, {}, {}, never);
        Check(w.size() == 1 && w[0] == 4, "without the engine's scene the code scene is replayed (MixSceneReplayCode)");
        w = Wanted(t2, 40000, 16, kReplayDefault, engineOpen, {}, {}, never);
        Check(w.empty(), "not owned: the engine's 0x24BFC688 wins as before");
        Live live;
        live.Start(0x1000, 0xA3657C2Cu, 0, 0, nullptr, 0);
        Check(live.Claim(0x1000, 0xA3657C2Cu, 0x2F00D71) != 0 && live.open[0x1000].caller == 0x2F00D71,
            "an adopted scene is claimed by its start event");
        Check(live.Claim(0x1000, 0xA3657C2Cu, 0x2F00D71) == 0, "only once (the caller is set)");
        Check(live.Claim(0x2000, 0xA3657C2Cu, 0x2F00D71) == 0 && live.Claim(0x1000, 0x1u, 0) == 0, "another pointer or hash");
    }
}

int RunCategoryStateTests()
{
    TestCategorySites();
    TestCategoryValues();
    TestRecorder();
    TestTrackRingAndCut();
    TestKeepLiveAndLines();
    TestPoolDecode();
    TestPoolScene();
    TestDrivers();
    TestDuckSites();
    TestDuckPlan();
    TestMixSceneAdditions();
    if (g_categoryStateFailures == 0)
        std::printf("category state tests passed\n");
    return g_categoryStateFailures;
}
