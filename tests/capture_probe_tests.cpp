// The capture probes' pure part (capture_probe_core.h): the probe, port,
// series and skipped tables, the signatures' text, the per-mode series
// statistics, the lightning tracker, the scrambled pool pointer, the cloth
// and vehicle helpers, the name table, the report text
// tools/capture_probe_report.py parses, the light tracker (went dark, came
// on, lit vanished with its samples) and the budgeted sweep over the pool.
#include "capture_probe_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace replay::captureprobe;
    namespace soundfx = replay::soundfx;

    int g_captureFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_captureFailures;
    }

    bool Has(const std::string& text, const char* part)
    {
        return text.find(part) != std::string::npos;
    }

    void TestTables()
    {
        // Probes 14 to 30, each once, each with a how and (unless
        // implemented) a note.
        std::set<int> numbers;
        int implemented = 0, partial = 0, skipped = 0;
        for (const ProbeInfo& probe : kProbes)
        {
            numbers.insert(probe.number);
            Check(probe.number >= kFirstProbe && probe.number <= kLastProbe, "probes: numbers 14 to 30");
            Check(probe.how && *probe.how, "probes: every probe says what it reads");
            if (probe.status != Status::Implemented)
                Check(probe.note && *probe.note, "probes: a partial or skipped probe says what is left out");
            implemented += probe.status == Status::Implemented ? 1 : 0;
            partial += probe.status == Status::Partial ? 1 : 0;
            skipped += probe.status == Status::Skipped ? 1 : 0;
        }
        Check(numbers.size() == 17 && sizeof(kProbes) / sizeof(kProbes[0]) == 17, "probes: 17 rows, 14 to 30");
        Check(implemented == 11 && partial == 6 && skipped == 0, "probes: 11 implemented, 6 partial, none skipped");

        // Ports: unique ids and addresses, a valid signature whose first
        // byte is compared, a probe of the list, a proof.
        std::set<std::string> ids;
        std::set<std::uint32_t> rvas;
        for (std::size_t i = 0; i < kPortCount; ++i)
        {
            const PortInfo& info = kPorts[i];
            Check(info.port == i, "ports: the table follows the enum");
            ids.insert(info.id);
            rvas.insert(info.rva);
            Check(numbers.count(info.probe) == 1, "ports: every port belongs to a listed probe");
            const soundfx::Signature signature = soundfx::ParseSignature(info.signature);
            Check(signature.valid && signature.bytes.size() >= 12 && signature.bytes.size() <= 64,
                "ports: a signature of 12 to 64 bytes");
            Check(info.proof && std::strlen(info.proof) > 20, "ports: every port carries its proof");
        }
        Check(ids.size() == kPortCount && rvas.size() == kPortCount && kPortCount == 30, "ports: 30 unique ports");
        Check(Info(kExplosion).rva == 0x1116B5C && std::string(Info(kExplosion).id) == "C10", "ports: the explosion core");
        Check(Info(kMusicEvent).probe == 28 && Info(kTvDraw).probe == 30, "ports: probe numbers");

        // Series: unique names, a source, a listed probe.
        std::set<std::string> names;
        for (std::size_t i = 0; i < kSeriesCount; ++i)
        {
            const SeriesInfo& info = kSeries[i];
            Check(info.series == i, "series: the table follows the enum");
            names.insert(info.name);
            Check(info.source && *info.source, "series: every series names its source");
            Check(numbers.count(info.probe) == 1, "series: every series belongs to a listed probe");
        }
        Check(names.size() == kSeriesCount, "series: unique names");

        // Every probe is covered by a port, a series or a skipped row.
        std::set<int> covered;
        for (const PortInfo& info : kPorts)
            covered.insert(info.probe);
        for (const SeriesInfo& info : kSeries)
            covered.insert(info.probe);
        Check(covered.size() == 17, "coverage: every probe has a port or a series");
        for (const SkippedInfo& row : kSkipped)
        {
            Check(numbers.count(row.probe) == 1, "skipped: a listed probe");
            Check(std::string(row.kind) == "skipped" || std::string(row.kind) == "indirect", "skipped: kind");
            Check(row.reason && *row.reason, "skipped: a reason");
        }
    }

    void TestSeriesStats()
    {
        SeriesStats stats;
        stats.Add(1, 0.5);
        stats.Add(1, 0.5);
        stats.Add(1, 0.7);
        stats.Add(2, 0.7);
        stats.Add(2, 0.1);
        stats.Add(2, std::nan(""));
        stats.Unread();
        Check(stats.Samples() == 5, "series: five finite samples");
        Check(stats.changes[1] == 1 && stats.changes[2] == 1, "series: a change counts in the mode it was seen");
        Check(stats.unread == 2, "series: NaN and Unread count as unread");
        Check(std::fabs(stats.byMode[1].max - 0.7) < 1e-9 && std::fabs(stats.byMode[2].min - 0.1) < 1e-9, "series: range per mode");
        SeriesStats flags;
        for (int i = 0; i < 10; ++i)
            flags.Add(0, i >= 7 ? 1.0 : 0.0);
        Check(flags.changes[0] == 1 && std::fabs(flags.byMode[0].Mean() - 0.3) < 1e-9, "series: a flag's mean is its share of time");
    }

    void TestLightning()
    {
        LightningTracker tracker;
        Check(!tracker.Step(0, false, 0) && !tracker.Step(0, false, 0), "lightning: nothing at rest");
        Check(tracker.Step(2, false, 0) && !tracker.Step(2, false, 0), "lightning: a cloud burst counts once while it lasts");
        Check(tracker.Step(3, true, 3), "lightning: a strike right after a cloud burst is a new lightning");
        tracker.Step(3, true, 3);
        tracker.Step(0, false, 0);
        Check(tracker.Step(1, false, 0), "lightning: a directional burst");
        tracker.Step(0, false, 0);
        Check(tracker.Step(9, false, 0) && tracker.otherType == 1, "lightning: an unknown type is counted apart");
        Check(tracker.started[1] == 1 && tracker.started[2] == 1 && tracker.started[3] == 1 && tracker.Total() == 4,
            "lightning: one of each");
        Check(tracker.requestRises == 1 && tracker.requestFrames == 2 && tracker.forcedFrames == 2,
            "lightning: the request flag rose once over two frames");
    }

    void TestPool()
    {
        // The game's scramble, applied backwards: the decoder must give the
        // plain pointer back for any key.
        const std::uint64_t plain = 0x00000211ABCDE000ull;
        for (std::uint64_t key : {0x1234567890ABCDEFull, 0ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000001ull})
        {
            const std::uint64_t mixed = Rol64(Rol64(key, 32), 61);
            std::uint64_t value = ~plain;
            value = Rol64(value, 64 - 32);
            value = Rol64(value, 64 - ((static_cast<unsigned>(mixed) & 0x1F) + 1));
            const std::uint64_t scrambled = value ^ mixed;
            Check(DecodePoolPointer(1, scrambled, key) == plain, "pool: the scramble is undone for any key");
        }
        Check(DecodePoolPointer(0, 0x1234, 0x5678) == 0, "pool: not ready gives 0");
        Check(Rol64(1, 64) == 1 && Rol64(0x8000000000000000ull, 1) == 1, "pool: rotation by 64 is identity, by 1 wraps");
        Check(PoolSlotLive(0x05) && !PoolSlotLive(0x80) && !PoolSlotLive(0xFF), "pool: bit 7 marks a free slot");
    }

    void TestHelpers()
    {
        const std::int32_t handles[6] = {-1, 3, -1, 0, 7, -1};
        Check(CountClothHandles(handles, 6) == 3 && CountClothHandles(nullptr, 6) == 0, "cloth: handles other than -1");
        Check(VehicleLightState(0xFB) == 3 && VehicleLightState(0xF8) == 0, "vehicle: the low 3 bits");
        const float xyz[3] = {3.0f, 4.0f, 12.0f};
        Check(std::fabs(Length3(xyz) - 13.0) < 1e-9 && Length3(nullptr) == 0.0, "length of a vector");
        // Jenkins one-at-a-time, lower case: "a" and "A" are the same key.
        Check(HashLower("a") == HashLower("A") && HashLower("a") == 0xCA2E9442u, "hash: the game's string hash");
        Check(HashLower("") == 0 && HashLower(nullptr) == 0, "hash: empty");
        Check(HashLower("a\\b") == HashLower("a/b"), "hash: a backslash is a slash");
    }

    void TestNames()
    {
        NameTable names(2);
        Check(names.Add("shake", "SMALL_EXPLOSION_SHAKE") && names.Add("shake", "small_explosion_shake"), "names: the same name twice");
        Check(names.Rows().size() == 1 && names.Rows()[0].count == 2, "names: one row counted twice");
        Check(names.Add("bank", "Script/Horse") && !names.Add("bank", "Another"), "names: the table fills up");
        Check(names.Overflow() == 1 && names.Rows()[1].text == "Script/Horse", "names: overflow counted, text kept");
        Check(NameTable::Clean("a,b=c d\n") == "a_b_c_d_", "names: separators and spaces are replaced");
        Check(NameTable::Clean(std::string(100, 'x').c_str()).size() == 63, "names: 63 characters at most");
    }

    LightSeen Seen(int handle, std::uint32_t model, float intensity, float x, float y, float z)
    {
        LightSeen object;
        object.handle = handle;
        object.model = model;
        object.intensity = intensity;
        object.xyz[0] = x;
        object.xyz[1] = y;
        object.xyz[2] = z;
        return object;
    }

    void TestLights()
    {
        // Sweep 1: two lit lamps (11, 12), one unlit crate (20).
        LightTracker lights;
        lights.Note(Seen(11, 0xAAAA, 2.0f, 10.0f, 0.0f, 0.0f));
        lights.Note(Seen(12, 0xBBBB, 1.5f, 50.0f, 0.0f, 0.0f));
        lights.Note(Seen(20, 0, 0.0f, 0.0f, 0.0f, 0.0f));
        Check(lights.Vanished().empty() && lights.wentDark == 0 && lights.cameOn == 0, "lights: the first sweep compares with nothing");
        lights.EndSweep();
        Check(lights.sweeps == 1 && lights.last.size() == 3 && lights.seen.empty(), "lights: the sweep became the last one");

        // Sweep 2: lamp 11 was shot: its handle is gone, a fragment (31)
        // appeared 0.5 m away; lamp 12 is still lit; crate 20 lit up (a
        // candle on it); a far object (40) appeared.
        lights.Note(Seen(12, 0xBBBB, 1.5f, 50.0f, 0.0f, 0.0f));
        lights.Note(Seen(20, 0xCCCC, 0.3f, 0.0f, 0.0f, 0.0f));
        lights.Note(Seen(31, 0, 0.0f, 10.0f, 0.5f, 0.0f));
        lights.Note(Seen(40, 0, 0.0f, 30.0f, 0.0f, 0.0f));
        Check(lights.cameOn == 1 && lights.wentDark == 0, "lights: the crate came on");
        const std::vector<LightSeen> vanished = lights.Vanished();
        Check(vanished.size() == 1 && vanished[0].handle == 11 && vanished[0].model == 0xAAAA &&
            std::fabs(vanished[0].intensity - 2.0) < 1e-6, "lights: the shot lamp vanished with its last state");
        float distance = -1.0f;
        Check(lights.NearestNew(vanished[0].xyz, distance) == 31 && std::fabs(distance - 0.5) < 1e-6,
            "lights: the fragment is the new object within 2 m");
        Check(lights.NearestNew(Seen(0, 0, 0, 30.0f, 5.0f, 0.0f).xyz, distance) == 0 && distance < 0.0f,
            "lights: nothing new within 2 m of a far point");
        Check(lights.NearestNew(Seen(0, 0, 0, 50.0f, 0.0f, 0.0f).xyz, distance) == 0,
            "lights: an object of the previous sweep is not new");
        Check(lights.Vanish(vanished[0], false) && lights.litVanished == 1 && lights.vanishedGone == 1 && lights.vanishedLeft == 0,
            "lights: a vanished handle that no longer exists is gone");
        LitVanishedSample sample;
        sample.model = vanished[0].model;
        sample.newModel = 0xF0F0;
        sample.newDistance = distance;
        lights.AddSample(sample);
        Check(lights.samples.size() == 1 && lights.samples[0].newModel == 0xF0F0, "lights: the sample was kept");
        lights.EndSweep();

        // Sweep 3: lamp 12 went dark (same handle), the candle left the
        // radius (handle exists), fragment 31 stays.
        lights.Note(Seen(12, 0xBBBB, 0.0f, 50.0f, 0.0f, 0.0f));
        lights.Note(Seen(31, 0, 0.0f, 10.0f, 0.5f, 0.0f));
        Check(lights.wentDark == 1, "lights: a light that goes dark on the same handle");
        const std::vector<LightSeen> left = lights.Vanished();
        Check(left.size() == 1 && left[0].handle == 20, "lights: the candle that left the radius vanished");
        Check(lights.Vanish(left[0], true) && lights.vanishedLeft == 1 && lights.litVanished == 2, "lights: still exists: left");
        lights.EndSweep();
        Check(lights.sweeps == 3 && lights.last.size() == 2, "lights: three sweeps");

        // The samples stop at 32; the counts do not.
        for (int i = 0; i < 40; ++i)
            lights.AddSample(sample);
        Check(lights.samples.size() == LightTracker::kSamples, "lights: 32 samples at most");
        for (int i = 0; i < 5; ++i)
            Check(lights.Vanish(left[0], false) == false, "lights: Vanish says no more samples when full");
        Check(lights.litVanished == 7 && lights.vanishedGone == 6, "lights: counted past the samples");

        // The sweep: 500 objects at 3 natives each do not fit one poll of
        // 300; they take two.
        LightSweep sweep;
        Check(!sweep.active && !sweep.Done() && !sweep.Walking(), "sweep: idle at start");
        sweep.Begin(500);
        int polls = 0;
        while (!sweep.Done())
        {
            sweep.NewPoll();
            ++polls;
            while (sweep.Walking() && sweep.HasBudget())
            {
                ++sweep.cursor;
                sweep.Spend(3);
            }
            Check(sweep.pollNatives <= LightSweep::kNativeBudget, "sweep: a poll never passes the budget");
        }
        Check(polls == 5 && sweep.polls == 5 && sweep.natives == 1500 && sweep.cursor == 500, "sweep: 500 objects in 5 polls");
        sweep.Spend(2); // the vanished check at the end
        Check(sweep.natives == 1502 && sweep.pollNatives == 302, "sweep: the end of the sweep is counted too");
        sweep.End();
        Check(!sweep.active, "sweep: ended");
        sweep.Begin(10);
        sweep.NewPoll();
        while (sweep.Walking() && sweep.HasBudget())
        {
            ++sweep.cursor;
            sweep.Spend(1);
        }
        Check(sweep.Done() && sweep.polls == 1 && sweep.natives == 10, "sweep: a small pool fits one poll");
        sweep.Begin(5000);
        Check(sweep.count == LightSweep::kCapacity, "sweep: the list is capped at the capacity");
        sweep.Begin(-1);
        Check(sweep.count == 0 && sweep.Done(), "sweep: an empty list is done at once");
    }

    void TestReport()
    {
        ReportData data;
        data.version = "1.0.0-dev";
        data.started = "2026-10-05 22:00:00";
        data.written = "2026-10-05 22:01:00";
        data.final = true;
        data.elapsedS = 60.0;
        data.modeS[0] = 30.0;
        data.modeS[1] = 20.0;
        data.modeS[2] = 10.0;
        data.frames = 3600;
        data.build = "1491.50";
        PortStats stats;
        stats.Note(1, 7, 0xF905C4 + 0x15D);
        stats.Note(1, 7, 0xF905C4 + 0x15D);
        stats.values[0].Add(13.0);
        soundfx::Sample sample;
        sample.args[0] = 13;
        sample.caller = 0xF90721;
        sample.thread = 7;
        sample.value = 101.5f;
        stats.samples.Add(sample);
        PortRow explosion;
        explosion.port = kExplosion;
        explosion.data = soundfx::Snapshot(soundfx::kS01, stats, "installed");
        data.ports.push_back(explosion);
        PortRow tv;
        tv.port = kTvDraw;
        tv.data = soundfx::Snapshot(soundfx::kS01, PortStats(), "signature_mismatch");
        data.ports.push_back(tv);
        data.threads.push_back({7, "script"});
        SeriesRow rain;
        rain.series = kRainGlobal;
        rain.status = "ok";
        rain.stats.Add(1, 0.0);
        rain.stats.Add(1, 0.8);
        rain.stats.Add(2, 0.8);
        data.series.push_back(rain);
        SeriesRow tvChannel;
        tvChannel.series = kTvChannel;
        tvChannel.status = "site_mismatch";
        data.series.push_back(tvChannel);
        SeriesRow fov;
        fov.series = kFinalFov;
        fov.status = "ok";
        data.series.push_back(fov);
        KeyTableRows table;
        table.name = "explosions";
        KeyRow row;
        row.key = 13;
        row.count = 2;
        row.flagged = 1;
        table.rows.push_back(row);
        data.tables.push_back(table);
        NameRow name;
        name.table = "shake";
        name.hash = 0x1234;
        name.text = "SMALL_EXPLOSION_SHAKE";
        name.count = 3;
        data.names.push_back(name);
        data.sites.push_back({"lightning_manager.manager", 0x733E90, true});
        data.lightning.Step(3, true, 3);
        data.lights.wentDark = 2;
        data.lights.litVanished = 1;
        data.lights.vanishedGone = 1;
        data.lights.sweeps = 7;
        LitVanishedSample vanished;
        vanished.model = 0xABCD;
        vanished.xyz[0] = 1.5f;
        vanished.xyz[1] = -2.0f;
        vanished.xyz[2] = 100.0f;
        vanished.intensity = 2.5f;
        vanished.exists = false;
        vanished.newModel = 0xEF01;
        vanished.newDistance = 0.25f;
        data.lights.AddSample(vanished);

        const std::string text = FormatReport(data);
        Check(Has(text, "CAPTURE_PROBE_REPORT,format=1,version=1.0.0-dev,started=2026-10-05 22:00:00,") &&
            Has(text, ",final=1,elapsed_s=60,frames=3600,idle_s=30,rec_s=20,play_s=10,build=1491.50,"), "report: the header");
        Check(Has(text, "PROBE,n=14,status=partial,name=game camera beyond pose and FOV,how=") &&
            Has(text, "PROBE,n=28,status=implemented,"), "report: the 17 probes");
        Check(Has(text, "SKIPPED,probe=24,kind=skipped,what=breakable glass creation,reason="), "report: the skipped rows");
        Check(Has(text, "SITE,name=lightning_manager.manager,rva=0x733e90,status=ok"), "report: the site checks");
        Check(Has(text, "PORT,id=C10,probe=23,rva=0x1116b5c,name=explosion_core,access=detour,status=installed,calls=2,"
                        "calls_idle=0,calls_rec=2,calls_play=0,per_s=0.0333333,per_frame=0.000555556,max_frame=0,threads=7,"
                        "thread_overflow=0,callers=1,caller_overflow=0,proof=ADD_EXPLOSION body"), "report: a port line");
        Check(Has(text, "VALUE,id=C10,slot=0,label=tag,count=1,min=13,max=13,"), "report: a port's histogram");
        Check(Has(text, "CALLER,id=C10,rva=0xf90721,calls=2") && Has(text, "SAMPLE,id=C10,n=0,caller=0xf90721,thread=7,a0=0xd,"),
            "report: callers and samples");
        Check(Has(text, "PORT,id=C28,probe=30,rva=0xf95214,name=tv_channel_draw,access=detour,status=signature_mismatch,calls=0,"),
            "report: a port that stayed off");
        Check(Has(text, "THREAD,id=7,name=script"), "report: threads");
        Check(Has(text, "SERIES,probe=15,name=rain_global,status=ok,samples=3,unread=0,source=0x4576250 (GET_RAIN_LEVEL reads it)") &&
            Has(text, "SERIES_MODE,name=rain_global,mode=rec,count=2,min=0,max=0.8,mean=0.4,changes=1") &&
            Has(text, "SERIES_MODE,name=rain_global,mode=play,count=1,min=0.8,max=0.8,mean=0.8,changes=0"), "report: a series per mode");
        Check(Has(text, "SERIES,probe=30,name=tv_channel,status=site_mismatch,samples=0,"), "report: a series behind a failed site");
        Check(Has(text, "SERIES,probe=14,name=final_fov,status=not_seen,samples=0,"), "report: an empty series is not_seen");
        Check(Has(text, "LIGHTNING,started=1,directional=0,cloud=0,strike=1,other_type=0,request_rises=1,request_frames=1,forced_frames=1"),
            "report: lightning");
        Check(!Has(text, "WATER_GPU"), "report: no water gpu line (the probe was removed)");
        Check(Has(text, "LIGHTS,went_dark=2,came_on=0,lit_vanished=1,vanished_gone=1,vanished_left=0,sweeps=7,samples=1\n") &&
            Has(text, "RAYFIRE,state_changes=0"), "report: lights and rayfire");
        Check(Has(text, "LIGHT_VANISHED,n=0,model=0xabcd,x=1.5,y=-2,z=100,intensity=2.5,exists=0,distance=-1,new_model=0xef01,"
                        "new_distance=0.25\n"), "report: a vanished light sample");
        Check(Has(text, "TABLE,name=explosions,rows=1,overflow=0") && Has(text, "KEY,table=explosions,key=0xd,count=2,flagged=1,mask=0\n"),
            "report: a key table");
        Check(Has(text, "NAMES,rows=1,overflow=0") && Has(text, "NAME,table=shake,hash=0x1234,count=3,text=SMALL_EXPLOSION_SHAKE"),
            "report: names");
        Check(text.rfind("END\n") == text.size() - 4, "report: ends with END");
        Check(!Has(text, "ReplayPoC") && !Has(text, "ReplayProbe"), "report: no old name");
        Check(ReportFileName(2026, 10, 5, 22, 0, 0) == "capture-probe-20261005-220000.txt", "report: the file name");
    }
}

int RunCaptureProbeTests()
{
    g_captureFailures = 0;
    TestTables();
    TestSeriesStats();
    TestLightning();
    TestLights();
    TestPool();
    TestHelpers();
    TestNames();
    TestReport();
    if (g_captureFailures)
        std::fprintf(stderr, "capture probe tests: %d failure(s)\n", g_captureFailures);
    else
        std::printf("capture probe tests: ok\n");
    return g_captureFailures;
}
