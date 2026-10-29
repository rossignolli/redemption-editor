// The volume probe's layout table, stage arithmetic, windows and lines; the
// voice list, the chain's split (bus, mix groups, mixer scenes), quad levels,
// the world groups, segments and the controls line.
#include "volume_probe_core.h"
#include "soundfx_probe_core.h" // the ports the project's detours hook
#include "wind_replay_core.h"   // the weather gate detour

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace
{
    using namespace replay::volumeprobe;

    int g_volumeFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_volumeFailures;
    }

    bool Near(double a, double b, double eps = 0.05) { return std::fabs(a - b) < eps; }

    float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

    bool Has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

    void TestSites()
    {
        Check(kSiteCount >= 100, "site count");
        std::set<std::string> names;
        std::uint32_t perPart[kPartCount] = {};
        for (const Site& site : kSites)
        {
            Check(site.size > 0 && site.size <= 12, "site size");
            Check(names.insert(site.name).second, "site names unique");
            ++perPart[static_cast<std::uint8_t>(site.part)];
        }
        for (std::uint8_t n = 0; n < kPartCount; ++n)
            Check(perPart[n] > 0, "every part has sites");
        Check(std::string(PartName(Part::Split)) == "split" && std::string(PartName(Part::Env)) == "env", "part names");
        Check(SiteIndex("list_count_zero") < kSiteCount && SiteIndex("nope") == kSiteCount, "site lookup");
        Check(Rip("cat_table") == kCategoryTableRva && Rip("pool_table") == kPoolTableRva, "rip targets");
        Check(Rip("weather_init") == kWeatherRva && Rip("music_ptr") == kMusicPtrRva && Rip("duck_scene") == kDuckSceneRva,
            "entity and control targets");
        const std::uint8_t good[12] = {0xF3, 0x0F, 0x11, 0x8F, 0x24, 0x02, 0x00, 0x00};
        const std::uint8_t bad[12] = {0xF3, 0x0F, 0x11, 0x8F, 0x28, 0x02, 0x00, 0x00};
        Check(SiteMatches(kSites[0], good) && !SiteMatches(kSites[0], bad) && !SiteMatches(kSites[0], nullptr), "site match");
        // The bus check's two sites cover the function's 22 bytes in a row.
        const Site& b1 = kSites[SiteIndex("bus_check")];
        const Site& b2 = kSites[SiteIndex("bus_check2")];
        Check(b1.rva + b1.size == b2.rva && b1.size + b2.size == 22, "bus check covered");
        Check(PoolSane(8, 1024, 0x260, 0x280) && !PoolSane(0, 1024, 0x260, 0x280) && !PoolSane(8, 1024, 0x10, 0x280) &&
                !PoolSane(8, 1024, 0x260, 0x250),
            "pool sanity (settings at least 0x262)");
        Check(SettingsCopyBytes(0x264) == 0x264 && SettingsCopyBytes(0x400) == kSettingsBytes, "settings copy size");
        Check(kSettingsVolume + kSettingsBufferStride + 4 <= kSettingsMinBytes && kSettingsEnvGroup + 8 <= kSettingsMinBytes &&
                kSettingsPosMode < kSettingsMinBytes && kSettingsSpeakerFlags + kSettingsBufferStride + 2 <= kSettingsMinBytes &&
                kSettingsGroupIds + 6 * kGroupSlots <= kSettingsMinBytes,
            "settings copy covers the fields");
        Check(kEnvFinal + 4 <= kEnvBytes && kEnvCategory + 2 <= kEnvBytes && kEnvMixGroups + kEnvMixGroupCount <= kEnvBytes &&
                kEnvListIndex + 2 <= kEnvBytes && kEnvVoiceIndex + 2 <= kEnvBytes && kEnvSceneCount + 4 <= kEnvBytes &&
                kEnvTags + 2 * kGroupSlots <= kEnvBytes && kEnvBus < kEnvBytes && kEnvTopSettings + 2 <= kEnvBytes,
            "env copy covers the fields");
        Check(kEnvPrevGroups == kEnvMixGroups + kGroupSlots && kMaxSceneIds == 52, "group and scene layout");
        // The moved sites read the same globals and fields.
        Check(kSites[SiteIndex("quad_index")].rva == 0x2536EEC && Rip("quad_index") == kGameIndexRva, "quad_index after the S17 entry");
        Check(kSites[SiteIndex("env_group")].rva == 0x253696F && kSites[SiteIndex("env_group")].bytes[3] == 0x48 &&
                kSites[SiteIndex("env_group")].bytes[4] == 0x02,
            "env_group: the store of +0x248");
        Check(kSites[SiteIndex("posmode_set")].bytes[2] == 0x5F && kSites[SiteIndex("posmode_set")].bytes[3] == 0x02, "posmode +0x25F");
        Check(kSites[SiteIndex("metric_list_reset")].bytes[4] == (kEnvListIndex & 0xFF) &&
                kSites[SiteIndex("metric_list_reset")].bytes[5] == (kEnvListIndex >> 8) &&
                kSites[SiteIndex("voice_free_index")].bytes[3] == (kEnvVoiceIndex & 0xFF),
            "the metric reset writes +0x242, FreeVoice +0x240");
        // No site may sit on the first 5 bytes (MinHook's jump) of a function
        // a detour of the project hooks: the sound ports (the SoundGeneric /
        // SoundFxProbe detours and the observers they tap) and the weather
        // gate. A site there turns the probe's part off (VOLUME_PROBE_CHECK,ok=0).
        for (const Site& site : kSites)
        {
            for (const auto& port : replay::soundfx::kPorts)
                Check(site.rva + site.size <= port.rva || site.rva >= port.rva + 5u,
                    (std::string("site on a hooked prologue: ") + site.name + " / " + port.id).c_str());
            Check(site.rva + site.size <= replay::windreplay::kGateRva || site.rva >= replay::windreplay::kGateRva + 5u,
                (std::string("site on the weather gate's prologue: ") + site.name).c_str());
        }
        Check(kWeatherRva + kWindBlock + kWindStrength == kWeatherRva + 0x4F44, "wind strength at W+0x4F44");
    }

    void TestList()
    {
        bool partial = false;
        Check(ListUsable(10, 12, partial) == 10 && !partial, "list grew during the copy: the first count");
        Check(ListUsable(10, 3, partial) == 3 && partial, "list reset during the copy: partial, the second count");
        Check(ListUsable(0x300, 0x300, partial) == kMaxVoicesPerBucket && !partial, "list capped at 0x200");
        Check(EntryOf(0xFFFF, 4096) == Entry::Hole && EntryOf(5000, 4096) == Entry::OutOfRange &&
                EntryOf(17, 4096) == Entry::Ok,
            "list entries");
        // i registered, 0xFFFF mixed (the metric builder's reset), another
        // index a newer registration.
        Check(LinkOf(7, 7) == Link::Pending && LinkOf(kNoIndex, 7) == Link::Mixed && LinkOf(8, 7) == Link::Other &&
                LinkOf(0, 0) == Link::Pending,
            "voice list link");
        SlotSet seen;
        seen.Reset(130);
        Check(seen.Take(0) && seen.Take(64) && seen.Take(129) && !seen.Take(64) && !seen.Take(130) && !seen.Take(0),
            "a sound slot is taken once per bucket walk (list_dup)");
        seen.Reset(70);
        Check(seen.Take(64) && !seen.Take(64) && !seen.Take(129), "reset clears and bounds the set");
        // The count read again after the walk.
        Check(ListChangeOf(12, 12) == ListChange::Settled && ListChangeOf(12, 30) == ListChange::Grew &&
                ListChangeOf(12, 3) == ListChange::Reset && ListChangeOf(0, 0) == ListChange::Settled,
            "list settled, being rebuilt, reset by a new frame");
        Check(kListAttempts >= 2, "the walk repeats an unsettled bucket");
    }

    void TestSplit()
    {
        // 0x25313D0 tests 0x11..0x20, 0x22, 0x23 on the byte masked with 0x1F
        // (0x256CC42): only 0x11..0x1F can reach it.
        Check(!BusSkipsCategory(0x10) && BusSkipsCategory(0x11) && BusSkipsCategory(0x1F) && !BusSkipsCategory(0x20) &&
                !BusSkipsCategory(0x22) && !BusSkipsCategory(0x04) && BusSkipsCategory(0x31) && BusSkipsCategory(0xF5),
            "bus set (0x25313D0 on the low 5 bits)");
        Check(CatLinear(-100.0f) == 0.0f && Near(CatLinear(-6.0f), 0.501, 0.01) && Near(CatLinear(0.0f), 1.0), "cat linear");
        // No fade: the current group does nothing, the previous one is full.
        Check(Near(GroupSlotFactor(false, false, 0.0f, true, -12.0f, 0), 1.0), "current group, fade 0");
        Check(Near(LinToDb(GroupSlotFactor(false, false, 0.0f, true, -12.0f, 255)), -12.0), "current group, fade 255");
        Check(Near(LinToDb(GroupSlotFactor(false, true, -10.0f, false, 0.0f, 0)), -10.0), "previous group, fade 0");
        Check(Near(GroupSlotFactor(false, true, -10.0f, false, 0.0f, 255), 1.0), "previous group faded out");
        Check(GroupSlotFactor(true, true, -10.0f, true, -12.0f, 255) == 1.0f, "skipping bus: no group");
        const float half = GroupSlotFactor(false, false, 0.0f, true, -100.0f, 128);
        Check(Near(half, 1.0f - 128.0f / 255.0f, 0.001), "fading into silence");

        const std::int16_t tags[6] = {5, -1, -1, 9, -1, -1};
        Check(SceneMatches(-1, tags, 6) && SceneMatches(9, tags, 6) && !SceneMatches(7, tags, 6) && !SceneMatches(-2, tags, 6),
            "scene tag filter");
        SceneInfo a, b, c, d;
        a.volume = 0.5f;          // mode 0, all
        b.volume = 0.5f;          // mode 0, tag 9
        b.tag = 9;
        c.volume = 0.1f;          // mode 0, tag 7 (no match)
        c.tag = 7;
        d.volume = 0.2f;          // mode 1 (minimum)
        d.mode = 1;
        Check(Near(SceneTerm({&a, &b, &c}, tags, 6), 0.25, 0.001), "scene multiply, filtered");
        Check(Near(SceneTerm({&a, &d}, tags, 6), 0.2, 0.001), "scene minimum after the product");
        SceneInfo e;
        e.volume = 0.9f;
        e.mode = 1;
        Check(Near(SceneTerm({&a, &e}, tags, 6), 0.5, 0.001), "minimum above the product does nothing");
        Check(SceneTerm({}, tags, 6) == 1.0f, "no scene");
        SceneInfo m2;
        m2.volume = 0.1f;
        m2.mode = 2;
        Check(SceneTerm({&m2}, tags, 6) == 1.0f, "unknown mode ignored");
    }

    void TestClasses()
    {
        Check(SpeechCls(2, 1) == Cls::SpeechScript && SpeechCls(4, 2) == Cls::SpeechScript && SpeechCls(4, 1) == Cls::SpeechAmbient &&
                SpeechCls(3, 1) == Cls::SpeechAmbient,
            "speech class by type and source");
        Check(std::string(ClsName(Cls::World)) == "world" && std::string(ClsName(Cls::SpeechScript)) == "speech_script" &&
                std::string(ClsName(Cls::Wind)) == "wind" && std::string(ClsName(Cls::Hoof)) == "hoof" &&
                std::string(ClsName(Cls::Music)) == "music",
            "names");
        Check(ClsOfEntityRva(kWeatherRva, 0x1F) == Cls::Weather && ClsOfEntityRva(kAmbientRva, 0x1F) == Cls::Ambient &&
                ClsOfEntityRva(kEmitterRva, 0x1F) == Cls::Emitter && ClsOfEntityRva(kPtfxRva, 0x1F) == Cls::Ptfx &&
                ClsOfEntityRva(kScriptAudioRva, 0x1F) == Cls::Script && ClsOfEntityRva(0x1234, 0x1F) == Cls::World,
            "entity classes");
        Check(ClsOfEntityRva(kAmbientRva, 0x1F & ~2u) == Cls::World, "unproven entity: world");
        Check(ClsOfBorn(BornKind::Footstep, true, true) == Cls::Hoof && ClsOfBorn(BornKind::Footstep, true, false) == Cls::Footstep &&
                ClsOfBorn(BornKind::HorseStep, true, false) == Cls::Hoof && ClsOfBorn(BornKind::Generic, true, true) == Cls::Horse &&
                ClsOfBorn(BornKind::Generic, true, false) == Cls::Owned && ClsOfBorn(BornKind::Generic, false, false) == Cls::World &&
                ClsOfBorn(BornKind::Movement, true, true) == Cls::Horse && ClsOfBorn(BornKind::Shot, true, false) == Cls::Shot,
            "born classes");
        Check(WorldSide(Cls::Wind) && WorldSide(Cls::World) && WorldSide(Cls::Music) && !WorldSide(Cls::SpeechScript) &&
                !WorldSide(Cls::Hoof),
            "world side");
        Check(RvaOf(0, 0x1000, 0x9000) == kRvaNone && RvaOf(0x1500, 0x1000, 0x9000) == 0x500 &&
                RvaOf(0x20000, 0x1000, 0x9000) == kRvaHeap,
            "rva of");
        Check(RvaText(kRvaNone) == "none" && RvaText(kRvaHeap) == "heap" && RvaText(0x5c90070) == "0x5c90070", "rva text");
    }

    Voice MakeVoice(float volDb, float postDb, float catDb, float residDb, float wetdryDb = 0.0f)
    {
        Voice v;
        v.voiceRead = true;
        v.volume = DbToLin(volDb - kVolumeBiasDb);
        v.postDb = postDb;
        v.catDb = catDb;
        v.catKnown = true;
        v.chain = DbToLin(postDb + catDb + residDb);
        v.dry = DbToLin(wetdryDb);
        v.wet = 0.0f;
        v.final = DbToLin(volDb + wetdryDb + postDb + catDb + residDb);
        return v;
    }

    void TestStages()
    {
        const Voice v = MakeVoice(-6.0f, -1.0f, -3.0f, -10.0f);
        const Stages s = StagesOf(v);
        Check(Near(s.volumeDb, -6.0) && Near(s.chainDb, -14.0) && Near(s.residDb, -10.0) && Near(s.finalDb, -20.0), "stages");
        Check(s.consistent, "consistent voice");
        Check(Near(s.restDb, -10.0) && s.groupDb == 0.0f && s.sceneDb == 0.0f, "rest without a split");
        Voice split = v;
        split.group = DbToLin(-4.0f);
        split.groupKnown = true;
        split.scene = DbToLin(-5.0f);
        split.sceneKnown = true;
        const Stages ss = StagesOf(split);
        Check(Near(ss.groupDb, -4.0) && Near(ss.sceneDb, -5.0) && Near(ss.restDb, -1.0), "resid split into groups, scenes, rest");
        // A skipping bus: post and category are not in the chain.
        Voice bus = MakeVoice(0.0f, 0.0f, 0.0f, -8.0f);
        bus.bus = true;
        bus.postDb = -3.0f;
        bus.catDb = -10.0f;
        Check(Near(StagesOf(bus).residDb, -8.0), "skipping bus: resid is the chain");
        Check(StagesOf(bus).postDb == 0.0f && StagesOf(bus).catDb == 0.0f, "skipping bus: post and category not applied");
        // The chain's post is the combine buffer's (requested post
        // plus what the hierarchy adds), not S+0x124 alone: a metadata post of
        // -3 dB on a requested -1 dB is -4 dB of post, not 3 dB of "rest".
        Voice meta = MakeVoice(-6.0f, -4.0f, -3.0f, -10.0f); // the chain holds a post of -4
        meta.postDb = -1.0f;                                  // S+0x124
        meta.combPostDb = -4.0f;                              // combine +4
        meta.combRead = true;
        const Stages ms = StagesOf(meta);
        Check(Near(ms.postDb, -4.0) && Near(ms.residDb, -10.0) && Near(ms.restDb, -10.0), "post from the combine buffer");
        Voice noComb = meta;
        noComb.combRead = false;
        Check(Near(StagesOf(noComb).postDb, -1.0) && Near(StagesOf(noComb).residDb, -13.0),
            "no combine read: the requested post (and the rest keeps the difference)");
        Voice quad = MakeVoice(0.0f, 0.0f, 0.0f, -6.0f);
        quad.quad = true;
        quad.quadDb = 3.0f;
        Check(Near(StagesOf(quad).effDb, -3.0) && Near(StagesOf(quad).finalDb, -6.0), "quad levels in the effective level");
        Voice bad = v;
        bad.final = DbToLin(-3.0f);
        Check(!StagesOf(bad).consistent, "inconsistent voice flagged");
        Voice silent;
        silent.voiceRead = true;
        Check(StagesOf(silent).consistent && StagesOf(silent).finalDb <= kSilentDb, "silent voice");
        Check(LinToDb(0.0f) == kSilentDb && Near(LinToDb(1.0f), 0.0) && PowerToDb(0.0) == kSilentDb, "dB helpers");
        const float equal[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Check(Near(QuadPowerDb(equal), 0.0) && QuadPowerDb(none) == kSilentDb, "quad power");
    }

    void TestWindow()
    {
        Window w;
        w.Reset(3);
        w.info.snaps = 2;
        Voice a = MakeVoice(0.0f, 0.0f, 0.0f, -6.0f); // -6 dB
        a.cls = Cls::Wind;
        a.category = 11;
        a.quad = true;
        a.quadDb = 0.0f;
        a.entityRva = kWeatherRva;
        a.topVtRva = 0x1111;
        Voice b = MakeVoice(0.0f, 0.0f, 0.0f, -6.0f);
        b.cls = Cls::World;
        b.category = 11;
        b.distM = 8.0f;
        b.entityRva = kRvaHeap;
        b.topVtRva = 0x2222;
        Voice c = MakeVoice(0.0f, 0.0f, -12.0f, 0.0f); // -12 dB speech
        c.cls = Cls::SpeechScript;
        c.category = 20;
        c.mixGroups = 1;
        c.combDb = -3.0f;
        c.combRead = true;
        w.Add(a);
        w.Add(b);
        w.Add(c);
        const Cell& wind = w.cls[static_cast<int>(Cls::Wind)];
        Check(wind.n == 1 && Near(wind.maxDb, -6.0) && wind.quadVoices == 1, "wind cell");
        const Cell& world = w.cls[static_cast<int>(Cls::World)];
        Check(Near(PowerToDb(world.power / 2.0), -9.0), "power per snapshot");
        Check(Near(world.wDist / world.wDistW, 8.0), "power weighted distance");
        Check(w.world.size() == 2, "world groups by entity and type (speech left out)");
        const std::vector<WorldKey> keys = TopWorld(w, 8);
        Check(keys.size() == 2, "top world");
        const std::string wl = WorldLine("play", 2, 1, 1500, keys[0], w.world[keys[0]], w.info);
        Check(wl.find("VOLUME_PROBE_WORLD,phase=play,pass=2,seg=1,clip_ms=1500,ent=") == 0 && Has(wl, ",category=11,"),
            "world line head");
        WorldKey windKey;
        windKey.entity = kWeatherRva;
        windKey.topVt = 0x1111;
        windKey.category = 11;
        const std::string wk = WorldLine("rec", 1, 0, 1500, windKey, w.world[windKey], w.info);
        Check(Has(wk, ",ent=0x5c90070,top_vt=0x1111,") && Has(wk, ",cls=wind,") && Has(wk, ",quad=1.00") &&
                Has(wk, ",dist_m=na"),
            "world line of the wind");
        const std::vector<std::int32_t> cats = TopCategories(w, 1);
        Check(cats.size() == 1 && cats[0] == 11, "loudest category first");
        const std::string line = ClassLine("rec", 1, 0, 1500, 500, "world", world, w.info);
        Check(line.find("VOLUME_PROBE,phase=rec,pass=1,seg=0,clip_ms=1500,win_ms=500,cat=world,snaps=2,count=0.50,sum_db=-9.0") == 0,
            "class line head");
        Check(Has(line, ",resid_db=-6.0") && Has(line, ",dist_m=8.0") && Has(line, ",listener_pos=na") &&
                Has(line, ",grp_db=0.0,scn_db=0.0,rest_db=-6.0") && Has(line, ",positional=1.00"),
            "class line fields");
        const Cell& speech = w.cls[static_cast<int>(Cls::SpeechScript)];
        const std::string sl = ClassLine("play", 2, 0, 1500, 500, "speech_script", speech, w.info);
        Check(Has(sl, ",cat_db=-12.0") && Has(sl, ",mg=1.00") && Has(sl, ",comb_db=-3.0"), "speech line");
        const std::string cl = CategoryLine("rec", 1, 0, 1500, 11, w.categories[11], w.info);
        Check(cl.find("VOLUME_PROBE_CATEGORY,phase=rec,pass=1,seg=0,clip_ms=1500,category=11,count=1.00") == 0,
            "category line");
        PassTotals totals;
        totals.Add(w);
        Check(totals.windows == 1 && Has(totals.Text(), "wind:-9.0") && Has(totals.Text(true), "speech_script:"), "pass totals");
        Window empty;
        Check(empty.Empty(), "empty window");
        Check(WindowOf(1499.0, 500) == 2 && WindowOf(1500.0, 500) == 3 && WindowOf(-5.0, 500) == 0, "window index");
    }

    void TestSegments()
    {
        Segments s;
        Check(s.Next(0) == 0 && s.Next(1) == 0 && s.Next(5) == 0, "forward: one segment");
        Check(s.Next(2) == 1 && s.Next(3) == 1, "back: a new segment");
        Check(s.Next(0) == 2, "loop again");
        s.Reset();
        Check(s.Next(9) == 0, "reset");
    }

    void TestEnv()
    {
        EnvWindow e;
        EnvSample s;
        s.weather = true;
        s.windStrength = 0.5f;
        s.windTypeOverride = 1;
        s.windTypeOld = 0xAABB;
        s.windTypeNew = 0xCCDD;
        s.windSlots = 12;
        s.windSlotsPlaying = 8;
        s.interiorRead = true;
        s.interior = 0;
        s.interiorRatio = 0.0f;
        s.exposureRead = true;
        s.exposure[0] = 1.0f;
        s.exposure[1] = 0.5f;
        s.duckRead = true;
        s.duckFlag = 1;
        s.duckLevel = 0.8f;
        s.duckScene = true;
        s.duckSceneHash = 0x576520E1;
        s.duckSceneApply = 0.3f;
        s.duckSceneWeather = 0.25f;
        s.flagsRead = true;
        s.flags[65] = 1;
        e.Add(s);
        s.windStrength = 1.5f;
        s.duckFlag = 0;
        e.Add(s);
        std::uint32_t hashes[kAudioFlagCount] = {};
        hashes[65] = 0xF09ABB54;
        const std::string line = EnvLine("play", 3, 1, 2000, e, hashes, 7, "41:0.300:0:-1:0x0");
        Check(line.find("VOLUME_PROBE_ENV,phase=play,pass=3,seg=1,clip_ms=2000,snaps=2,weather=1,wind_str=1.000,wind_str_max=1.500") == 0,
            "env line head");
        Check(Has(line, ",wind_type_ovr=1,wind_type=0xaabb;0xccdd,") && Has(line, ",wind_slots=12,wind_playing=8"), "wind fields");
        Check(Has(line, ",interior=0.00,") && Has(line, ",expo=1.000;0.500;0.000;0.000"), "interior and exposure");
        Check(Has(line, ",duck_flag=0.50,duck_level=0.800,") && Has(line, ",duck_scene=0x576520e1:0.300"), "speech duck");
        Check(Has(line, ",duck_scene=0x576520e1:0.300,duck_weather=0.250,duck_sfx=none,"), "speech duck variables");
        Check(kWeatherApplyHash == 0x965062BEu && kSfxApplyHash == 0x934A8049u, "weatherApply and sfxApply hashes");
        Check(Has(line, ",flags=65:0xf09abb54:1,") && Has(line, ",mix_scenes=7,scenes_low=41:0.300:0:-1:0x0"), "flags and scenes");
        EnvWindow empty;
        const std::string none = EnvLine("rec", 1, 0, 0, empty, nullptr, 0, "");
        Check(Has(none, ",weather=0,interior=na") && Has(none, ",flags=na,") && Has(none, ",scenes_low=none"), "env line empty");
        Check(Has(none, ",duck_scene=na,duck_weather=na,duck_sfx=na,"), "env line empty: duck variables");
        std::vector<SceneInfo> scenes(3);
        scenes[0].id = 1;
        scenes[0].volume = 1.0f;
        scenes[1].id = 2;
        scenes[1].volume = 0.25f;
        scenes[2].id = 3;
        scenes[2].volume = 0.5f;
        scenes[2].mode = 1;
        scenes[2].key = 0xAB;
        Check(LowScenesText(scenes, 8) == "2:0.250:0:-1:0x0;3:0.500:1:-1:0xab" && LowScenesText(scenes, 1) == "2:0.250:0:-1:0x0",
            "scenes below unity, quietest first");
        const std::int32_t counts[3] = {0, 2, 0};
        Check(FlagsText(counts, nullptr, 3) == "1:0x00000000:2", "flags text");
    }
}

int RunVolumeProbeTests()
{
    TestSites();
    TestList();
    TestSplit();
    TestClasses();
    TestStages();
    TestWindow();
    TestSegments();
    TestEnv();
    if (g_volumeFailures == 0)
        std::printf("volume probe tests passed\n");
    return g_volumeFailures;
}
