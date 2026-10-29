// The read probes' pure part (soundfx_probe_core.h): the port table, the
// signature text, the value histogram, the thread, caller and sample sets, the
// key table, the per-frame count, the live sound table with the owner check,
// the queue of create-and-play requests and the report text
// tools/soundfx_probe_report.py parses.
#include "soundfx_probe_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace replay::soundfx;

    int g_probeFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_probeFailures;
    }

    bool Has(const std::string& text, const char* part)
    {
        return text.find(part) != std::string::npos;
    }

    void TestPortTable()
    {
        std::set<std::string> ids;
        std::set<std::uint32_t> rvas;
        int detours = 0;
        int taps = 0;
        int skipped = 0;
        for (std::size_t i = 0; i < kPortCount; ++i)
        {
            const PortInfo& info = kPorts[i];
            Check(info.port == i, "ports: the table follows the enum");
            ids.insert(info.id);
            rvas.insert(info.rva);
            detours += info.access == Access::Detour ? 1 : 0;
            taps += info.access == Access::Tap ? 1 : 0;
            skipped += info.access == Access::Skipped ? 1 : 0;
            if (info.access != Access::Detour)
                Check(info.note && *info.note, "ports: a tap names its detour and a skipped port its reason");
        }
        Check(ids.size() == kPortCount && rvas.size() == kPortCount, "ports: ids and addresses are unique");
        Check(detours == 36 && taps == 9 && skipped == 0, "ports: 36 detours, 9 taps, none skipped");
        // The five addresses proved in the dump later are hooked like the others.
        for (const Port port : {kS31, kS32, kS40, kP17, kP18})
            Check(Info(port).access == Access::Detour, "ports: the five proved ports are detours");
        Check(std::string(Info(kS31).name) == "pan_from_init_param_AE" && Info(kS31).value == Value::S16 &&
            Info(kS40).value == Value::HashF32 && std::string(Info(kS40).label1) == "create_if_missing", "ports: the proved ports' rows");
        Check(std::string(Info(kS18).name) == "volume" && Info(kS18).rva == 0x2537168, "ports: the volume port");
        int implemented = 0;
        for (const ProbeInfo& probe : kProbes)
            implemented += probe.implemented ? 1 : 0;
        Check(sizeof(kProbes) / sizeof(kProbes[0]) == 13 && implemented == 12, "probes: 13 listed, only 9 skipped");
        Check(!kProbes[8].implemented && kProbes[11].implemented, "probes: the A/B is the skipped one, probe 12 is in");
    }

    void TestSignature()
    {
        const Signature signature = ParseSignature("8B 05 ?? ?? ?? ?? 48 C1 e0 06");
        Check(signature.valid && signature.bytes.size() == 10, "signature: ten bytes");
        Check(signature.mask[0] == 1 && signature.mask[2] == 0 && signature.mask[5] == 0 && signature.mask[9] == 1,
            "signature: ?? is not compared");
        const std::uint8_t same[] = {0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0x48, 0xC1, 0xE0, 0x06};
        const std::uint8_t other[] = {0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0x48, 0xC1, 0xE0, 0x07};
        Check(MatchSignature(same, signature), "signature: any displacement matches");
        Check(!MatchSignature(other, signature), "signature: another byte does not");
        Check(!ParseSignature("").valid && !ParseSignature("8B 0").valid && !ParseSignature("8B0548").valid &&
            !ParseSignature("ZZ 05").valid, "signature: malformed text is refused");
        Check(!ParseSignature("?? 8B").valid, "signature: the first byte is always compared");
        Check(!MatchSignature(same, Signature{}), "signature: an invalid signature matches nothing");
    }

    void TestHistogram()
    {
        Check(BinOf(0.0) == 8 && BinOf(0.5) == 11 && BinOf(1.0) == 12 && BinOf(-6.0) == 4 && BinOf(-100.0) == 2,
            "histogram: decade bins");
        Check(BinOf(20000.0) == 16 && BinOf(-20000.0) == 0 && BinOf(0.001) == 9 && BinOf(-0.001) == 7,
            "histogram: the outer and inner bins");
        Check(std::string(BinLabel(4)) == "-10..-1" && std::string(BinLabel(11)) == "0.1..1", "histogram: bin labels");
        Histogram plain;
        AtomicHistogram atomic;
        const double values[] = {-6.0, -12.5, 0.0, 0.25, 1.0, 1200.0};
        for (const double value : values)
        {
            plain.Add(value);
            atomic.Add(value);
        }
        plain.Add(std::numeric_limits<double>::quiet_NaN());
        atomic.Add(std::numeric_limits<double>::infinity());
        Check(plain.count == 6 && plain.nonFinite == 1 && plain.min == -12.5 && plain.max == 1200.0, "histogram: range");
        Check(plain.integral == 4 && std::fabs(plain.Mean() - (1182.75 / 6.0)) < 1e-9, "histogram: integral values and mean");
        const Histogram snapshot = atomic.Snapshot();
        Check(snapshot.count == plain.count && snapshot.min == plain.min && snapshot.max == plain.max &&
            snapshot.nonFinite == 1 && snapshot.integral == plain.integral, "histogram: the atomic one counts the same");
        Check(std::memcmp(snapshot.bins, plain.bins, sizeof(plain.bins)) == 0, "histogram: the same bins");
        const Histogram empty = AtomicHistogram().Snapshot();
        Check(empty.count == 0 && empty.min == 0.0 && empty.max == 0.0, "histogram: empty is zero, not infinity");
    }

    void TestSets()
    {
        ThreadSet threads;
        threads.Note(4304);
        threads.Note(1200);
        threads.Note(4304);
        const std::vector<std::uint32_t> ids = threads.Snapshot();
        Check(ids.size() == 2 && ids[0] == 1200 && ids[1] == 4304, "threads: each id once, sorted");
        for (std::uint32_t id = 1; id <= 20; ++id)
            threads.Note(id * 4);
        Check(threads.Snapshot().size() == ThreadSet::kSlots && threads.overflow.load() == 14, "threads: the set is small");

        CallerSet callers;
        for (int i = 0; i < 5; ++i)
            callers.Note(0x2588996);
        callers.Note(0x257C833);
        callers.Note(0); // outside the image
        const std::vector<CallerRow> rows = callers.Snapshot();
        Check(rows.size() == 3 && rows[0].rva == 0x2588996 && rows[0].calls == 5, "callers: the busiest first");
        Check(rows[1].rva == 0 && rows[2].rva == 0x257C833, "callers: rva 0 is a caller too");
        for (std::uint32_t i = 0; i < 40; ++i)
            callers.Note(0x1000 + i);
        Check(callers.Snapshot().size() == CallerSet::kSlots && callers.overflow.load() == 11, "callers: 32 kept");

        SampleStore samples;
        Sample sample;
        sample.caller = 0x100;
        sample.args[1] = 7;
        Check(samples.Add(sample), "samples: the first call");
        Check(!samples.Add(sample), "samples: the same caller and value again is not kept");
        sample.args[1] = 8;
        Check(samples.Add(sample), "samples: another value");
        sample.caller = 0x200;
        Check(samples.Add(sample), "samples: another caller");
        for (std::uint64_t value = 100; value < 200; ++value)
        {
            sample.args[1] = value;
            samples.Add(sample);
        }
        Check(samples.Full() && samples.Snapshot().size() == SampleStore::kMax, "samples: the first 32 distinct");
        Check(samples.Snapshot()[1].args[1] == 8 && samples.Snapshot()[2].caller == 0x200, "samples: in arrival order");
    }

    void TestKeyTable()
    {
        KeyTable table(64);
        for (int i = 0; i < 3; ++i)
            table.Add(0x8000010029934Aull, false, 1);
        table.Add(0x8000010029934Aull, true, 2);
        table.Add(0, false, 1); // key 0 is a key
        table.AddValue(0x1234, -6.0);
        table.AddValue(0x1234, 3.5);
        const std::vector<KeyRow> rows = table.Snapshot();
        Check(rows.size() == 3 && rows[0].key == 0x8000010029934Aull && rows[0].count == 4, "keys: largest count first");
        Check(rows[0].flagged == 1 && rows[0].mask == 3 && !rows[0].hasValue, "keys: flag and mask");
        Check(rows[1].key == 0x1234 && rows[1].hasValue && rows[1].min == -6.0 && rows[1].max == 3.5, "keys: value range");
        Check(rows[2].key == 0 && rows[2].count == 1, "keys: key zero");
        // A full table counts what it could not keep.
        KeyTable small(8);
        int kept = 0;
        for (std::uint64_t key = 1; key <= 20; ++key)
            kept += small.Add(key) ? 1 : 0;
        Check(kept == 8 && small.Overflow() == 12 && small.Snapshot().size() == 8, "keys: overflow is counted");
        Check(small.Add(1) && small.Overflow() == 12, "keys: a known key still counts when full");
        const FxRow fx = FxRowOf(KeyRow{FxKey(0xDEADBEEF, 2, 1), 5, 0, 0, true, 0.5, 1.5});
        Check(fx.hash == 0xDEADBEEF && fx.mode == 2 && fx.field == 1 && fx.count == 5 && fx.max == 1.5, "keys: fx key");
        Check(std::string(FxFieldName(fx.field)) == "zoom", "keys: fx field name");
        // Tracked values count their changes in flagged.
        KeyTable tracked(16);
        tracked.AddValue(0x40, 0.5, true);
        tracked.AddValue(0x40, 0.5, true);
        tracked.AddValue(0x40, 0.75, true);
        tracked.AddValue(0x40, 0.75, true);
        tracked.AddValue(0x40, 0.5, true);
        const std::vector<KeyRow> trackedRows = tracked.Snapshot();
        Check(trackedRows.size() == 1 && trackedRows[0].count == 5 && trackedRows[0].flagged == 2, "keys: two changes in five tracked values");
        Check(trackedRows[0].min == 0.5 && trackedRows[0].max == 0.75, "keys: tracked values keep their range");
    }

    void TestPortStats()
    {
        PortStats stats;
        for (int i = 0; i < 3; ++i)
            stats.Note(static_cast<std::size_t>(Mode::Record), 100, 0x10);
        stats.EndFrame();
        stats.Note(static_cast<std::size_t>(Mode::Play), 104, 0x20);
        stats.EndFrame();
        stats.EndFrame();
        for (int i = 0; i < 7; ++i)
            stats.Note(static_cast<std::size_t>(Mode::Idle), 100, 0x10);
        stats.EndFrame();
        Check(stats.Total() == 11 && stats.maxPerFrame == 7, "port: total and the largest frame");
        stats.values[0].Add(-3.0);
        stats.linkOk.fetch_add(4);
        const PortReport report = Snapshot(kS18, stats, "installed");
        Check(report.calls[0] == 7 && report.calls[1] == 3 && report.calls[2] == 1 && report.Total() == 11, "port: per mode");
        Check(report.threads.size() == 2 && report.callers.size() == 2 && report.callers[0].calls == 10, "port: sets");
        Check(report.values[0].count == 1 && report.linkOk == 4 && report.maxPerFrame == 7, "port: snapshot");
        Check(PerSecond(120, 60.0) == 2.0 && PerSecond(5, 0.0) == 0.0, "port: per second");
    }

    void TestLiveTable()
    {
        LiveTable table(16);
        bool replaced = true;
        LiveSound* first = table.Insert(0x1000, replaced);
        Check(first && !replaced && table.Size() == 1, "live: a new sound");
        first->ref = 0xAA;
        first->audio = 0x5000;
        first->tracker = 0;
        Check(table.Find(0x1000) && table.Find(0x1000)->ref == 0xAA && !table.Find(0x2000) && !table.Find(0), "live: find");
        // The same address made again before its release was seen.
        LiveSound* again = table.Insert(0x1000, replaced);
        Check(again && replaced && again->ref == 0 && table.Size() == 1, "live: a reused address starts clean");
        // Erase with many colliding entries: everything else stays findable.
        std::vector<std::uint64_t> sounds;
        for (std::uint64_t i = 0; i < 11; ++i)
            sounds.push_back(0x7FF600000000ull + i * 0x100);
        for (const std::uint64_t sound : sounds)
        {
            LiveSound* live = table.Insert(sound, replaced);
            Check(live != nullptr, "live: room up to three quarters");
            if (live)
                live->ref = sound + 1;
        }
        Check(table.Size() == 12 && table.Insert(0x9999, replaced) == nullptr, "live: three quarters is full");
        LiveSound removed;
        for (std::size_t i = 0; i < sounds.size(); i += 2)
            Check(table.Erase(sounds[i], &removed) && removed.ref == sounds[i] + 1, "live: erase returns the entry");
        for (std::size_t i = 0; i < sounds.size(); ++i)
        {
            const LiveSound* live = table.Find(sounds[i]);
            Check(i % 2 ? (live && live->ref == sounds[i] + 1) : live == nullptr, "live: the others survive an erase");
        }
        Check(!table.Erase(sounds[0]) && table.Size() == 6, "live: erasing twice");

        LiveSound live;
        live.audio = 0x5000;
        live.tracker = 0x6000;
        OwnerCheck check = CheckOwner(live, 0x5000, 0x6000);
        Check(!check.audioChanged && !check.trackerChanged && live.ownerChanges == 0, "owner: unchanged");
        check = CheckOwner(live, 0x5800, 0x6000);
        Check(check.audioChanged && !check.trackerChanged && check.oldAudio == 0x5000 && live.audio == 0x5800 &&
            live.ownerChanges == 1, "owner: a new audio entity is one change");
        check = CheckOwner(live, 0x5800, 0x6000);
        Check(!check.audioChanged && live.ownerChanges == 1, "owner: the same change is not counted twice");
        check = CheckOwner(live, 0x5800, 0);
        Check(check.trackerChanged && check.oldTracker == 0x6000 && live.ownerChanges == 2, "owner: the tracker too");
    }

    void TestPendingQueue()
    {
        PendingQueue queue(4);
        std::uint64_t waited = 0;
        Check(queue.Push(0xA, 0x29934A, 1000) && queue.Push(0xA, 0x29934A, 2000) && queue.Push(0xB, 0x77, 2500), "queue: push");
        Check(!queue.Match(0xC, 0x77, 3000, waited), "queue: another entity does not match");
        Check(queue.Match(0xA, 0x29934A, 5000, waited) && waited == 4000, "queue: the oldest of the pair first");
        Check(queue.Match(0xA, 0x29934A, 5000, waited) && waited == 3000, "queue: then the next");
        Check(queue.Waiting() == 1 && queue.Matched() == 2, "queue: one waits");
        Check(queue.Expire(6000, 5000) == 0, "queue: young requests stay");
        Check(queue.Expire(8000, 5000) == 1 && queue.Expired() == 1 && queue.Waiting() == 0, "queue: an old one never arrived");
        for (std::uint64_t i = 0; i < 4; ++i)
            queue.Push(0xD, i, 9000 + i);
        Check(!queue.Push(0xD, 99, 9100) && queue.Overflow() == 1 && queue.Waiting() == 4, "queue: full drops the oldest");
        Check(!queue.Match(0xD, 0, 9200, waited) && queue.Match(0xD, 99, 9200, waited) && waited == 100, "queue: after the drop");
        Check(queue.Enqueued() == 8, "queue: every push counted");
    }

    void TestReport()
    {
        Check(ReportFileName(2026, 10, 5, 21, 3, 9) == "soundfx-probe-20261005-210309.txt", "report: file name");
        Check(Hex(0x2537168) == "0x2537168" && Number(0.5) == "0.5" && Number(-100.0) == "-100", "report: numbers");
        ReportData data;
        data.version = "1.0.0-dev";
        data.started = "2026-10-05 21:03:09";
        data.written = "2026-10-05 21:13:09";
        data.final = true;
        data.elapsedS = 600.0;
        data.modeS[0] = 100.0;
        data.modeS[1] = 400.0;
        data.modeS[2] = 100.0;
        data.frames = 36000;
        data.build = "1491.50";
        PortStats stats;
        for (int i = 0; i < 1200; ++i)
            stats.Note(1, 4304, 0x257C833);
        stats.EndFrame();
        stats.values[0].Add(-6.0);
        stats.values[0].Add(0.0);
        stats.values[1].Add(-3.0);
        stats.linkOk.store(1200);
        Sample sample;
        sample.args[0] = 0x7FF600001000ull;
        sample.args[1] = 0xC0C00000;
        sample.value = -6.0f;
        sample.sound = 0x7FF600002000ull;
        sample.ref = 0x8000010029934Aull;
        sample.owner = 0x7FF600003000ull;
        sample.caller = 0x257C833;
        sample.thread = 4304;
        stats.samples.Add(sample);
        data.ports.push_back(Snapshot(kS18, stats, "installed"));
        data.ports.push_back(Snapshot(kS40, PortStats(), "signature_mismatch"));
        KeyRow variable;
        variable.key = 0xDEADBEEF;
        variable.count = 5;
        variable.flagged = 2;
        variable.hasValue = true;
        variable.min = 0.0;
        variable.max = 1.0;
        data.globalVariables.push_back(variable);
        KeyRow field;
        field.key = 7;
        field.count = 3;
        data.fxField5CC.push_back(field);
        data.ports.push_back(Snapshot(kS13, PortStats(), "signature_mismatch"));
        data.threads.push_back({4304, "script"});
        data.refs.push_back(KeyRow{0x8000010029934Aull, 175, 2, 1, false, 0.0, 0.0});
        data.refs.push_back(KeyRow{0xABCD1234, 3, 0, 2, false, 0.0, 0.0});
        data.life.created = 500;
        data.life.releaseAfterForget = 300;
        data.life.releaseWithoutForget = 150;
        OwnerChange change;
        change.sound = 0x7FF600002000ull;
        change.ref = 0x77;
        change.oldAudio = 0x1;
        change.newAudio = 0x2;
        change.port = kS16;
        change.caller = 0x1234;
        change.ageS = 1.5;
        data.ownerChanges.push_back(change);
        data.queue.enqueued = 400;
        data.queue.matched = 390;
        data.queue.neverCreated = 10;
        data.speech.byRefFromSpeech = 40;
        data.speech.refs.push_back(KeyRow{0x55, 40, 0, 0, false, 0.0, 0.0});
        WetHoofSample hoof;
        hoof.ref = 0x8000010029934Aull;
        hoof.variableCount = 1;
        hoof.words[0] = 0x02D01730;
        hoof.words[1] = 0x3F000000;
        data.wetHoof.requests = 175;
        data.wetHoof.samples.push_back(hoof);
        data.construct.topLevel = 500;
        data.construct.child = 1500;
        data.soundVariables.push_back(KeyRow{0x833FF9C9, 12, 0, 0, true, 0.0, 1.0});
        data.fx.push_back(FxRowOf(KeyRow{FxKey(0xDEADBEEF, 1, 0), 9, 0, 0, true, 0.25, 1.0}));
        const std::string text = FormatReport(data);
        Check(text.rfind("SOUNDFX_PROBE_REPORT,format=1,version=1.0.0-dev,", 0) == 0, "report: the header is the first line");
        Check(Has(text, ",final=1,elapsed_s=600,frames=36000,idle_s=100,rec_s=400,play_s=100,build=1491.50\n"), "report: header fields");
        Check(Has(text, "PROBE,n=9,status=skipped,") && Has(text, "PROBE,n=1,status=implemented,"), "report: the 13 probes");
        Check(Has(text, "PORT,id=S18,rva=0x2537168,name=volume,access=detour,status=installed,calls=1200,calls_idle=0,"
                        "calls_rec=1200,calls_play=0,per_s=2,per_frame=0.0333333,max_frame=1200,threads=4304,"),
            "report: a port line");
        Check(Has(text, ",link_ok=1200,link_bad=0,link_unread=0\n"), "report: the block link");
        Check(Has(text, "VALUE,id=S18,slot=0,label=value,count=2,min=-6,max=0,mean=-3,integral=2,non_finite=0,bins=-10..-1:1|0:1\n"),
            "report: a value line");
        Check(Has(text, "VALUE,id=S18,slot=1,label=create_param_volume,count=1,"), "report: the second histogram");
        Check(Has(text, "CALLER,id=S18,rva=0x257c833,calls=1200\n"), "report: callers");
        Check(Has(text, "SAMPLE,id=S18,n=0,caller=0x257c833,thread=4304,a0=0x7ff600001000,a1=0xc0c00000,a2=0x0,a3=0x0,value=-6,"
                        "sound=0x7ff600002000,ref=0x8000010029934a,owner=0x7ff600003000\n"), "report: a sample");
        Check(Has(text, "PORT,id=S40,rva=0x25370b8,name=global_variable,access=detour,status=signature_mismatch,calls=0,"),
            "report: a detour port that stayed off (S40 is a detour)");
        Check(Has(text, "GLOBAL_VARIABLES,distinct=1,overflow=0\n") &&
            Has(text, "GLOBAL_VARIABLE,key=0xdeadbeef,count=5,changes=2,min=0,max=1\n") &&
            Has(text, "FX_FIELD_5CC,key=0x7,count=3\n"), "report: probe 12 and the particle fields");
        Check(Has(text, "PORT,id=S13,rva=0x2585be4,name=stop,access=detour,status=signature_mismatch,"), "report: a port that is off");
        Check(Has(text, "THREAD,id=4304,name=script\n"), "report: thread names");
        Check(Has(text, "LIFE,created=500,") && Has(text, ",release_after_forget=300,release_without_forget=150,"),
            "report: how sounds end");
        Check(Has(text, "OWNER_CHANGE,sound=0x7ff600002000,ref=0x77,port=S16,caller=0x1234,age_s=1.5,audio_old=0x1,audio_new=0x2,"),
            "report: an owner change");
        Check(Has(text, "QUEUE,requests=0,refused=0,enqueued=400,matched=390,never_created=10,"), "report: the queue");
        Check(Has(text, "SPEECH,by_ref_from_speech=40,") && Has(text, ",distinct_refs=1\n") &&
            Has(text, "SPEECH_REF,key=0x55,count=40\n"), "report: speech");
        Check(Has(text, "WET_HOOF_SAMPLE,ref=0x8000010029934a,caller=0x0,variables=1,words=0x2d01730;0x3f000000;0x0;0x0;0x0;0x0\n"),
            "report: wet hoof variables");
        Check(Has(text, "CONSTRUCT,top_level=500,child=1500,"), "report: construction");
        Check(Has(text, "SOUND_VARIABLE,key=0x833ff9c9,count=12,min=0,max=1\n"), "report: variables with their range");
        Check(Has(text, "FX,hash=0xdeadbeef,mode=rec,field=alpha,count=9,min=0.25,max=1\n"), "report: effect state");
        Check(Has(text, "REFS,distinct=2,overflow=0\n") && Has(text, "REF,ref=0x8000010029934a,core=A,count=175,refused=2\n") &&
            Has(text, "REF,ref=0xabcd1234,core=B,count=3,refused=0\n"), "report: the ref table");
        Check(text.size() > 4 && text.compare(text.size() - 4, 4, "END\n") == 0, "report: ends with END");
        // No internal project name in a file the player reads.
        Check(!Has(text, "ReplayPoC") && !Has(text, "ReplayProbe"), "report: only the product's name");
    }
}

int RunSoundFxProbeTests()
{
    TestPortTable();
    TestSignature();
    TestHistogram();
    TestSets();
    TestKeyTable();
    TestPortStats();
    TestLiveTable();
    TestPendingQueue();
    TestReport();
    if (g_probeFailures == 0)
        std::printf("soundfx probe tests passed\n");
    return g_probeFailures;
}
