// The generic sound effect recorder's pure part (sound_generic_core.h): the
// stable id, the family caller, the event ring, the recorder (requests matched
// to creates, the sampler with its trailing sample, the ends), the budget
// governor, the buffer trim, the window cut and the report text
// tools/sound_generic_report.py parses. The clip section (SGEN) round trip is
// in clip_file_tests.cpp.
#include "sound_generic_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace replay::soundgeneric;
    using replay::census::OwnerClass;
    using replay::census::kNoOwner;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint64_t kHoofRef = 0x008000040029934aull; // the wet hoof set, variant 4, flag bit 55
    constexpr std::uint32_t kGameCaller = 0x2EFA7E0;        // a game site above the create-and-play wrapper
    constexpr std::uint32_t kWrapper = 0x2588996;           // inside the audio entity's create-and-play wrapper
    constexpr std::uint32_t kDrain = 0x258A469;             // inside the queue's drain

    LifeEvent Enqueue(std::uint64_t audio, std::uint64_t ref, std::uint64_t timeUs, Source source = Source::Queue)
    {
        LifeEvent e;
        e.kind = LifeKind::Enqueue;
        e.source = source;
        e.audio = audio;
        e.ref = ref;
        e.timeUs = timeUs;
        e.callers[0] = kWrapper;
        e.callers[1] = kGameCaller;
        return e;
    }

    LifeEvent Create(std::uint64_t audio, std::uint64_t ref, std::uint64_t sound, std::uint64_t timeUs, bool throughDrain)
    {
        LifeEvent e;
        e.kind = LifeKind::Create;
        e.core = 0;
        e.audio = audio;
        e.ref = ref;
        e.sound = sound;
        e.refused = sound == 0;
        e.timeUs = timeUs;
        e.hasParams = true;
        e.params.volume = -6.0f;
        e.params.pitch = 0;
        e.params.pan = -1;
        if (throughDrain)
        {
            e.callers[0] = 0x2588EDA;
            e.callers[1] = kDrain;
            e.callers[2] = 0x2F1CEF8;
        }
        else
        {
            e.callers[0] = 0x251686C;
            e.callers[1] = 0x2589214;
            e.callers[2] = 0x313C5E8;
        }
        return e;
    }

    LifeEvent End(LifeKind kind, std::uint64_t sound, std::uint64_t timeUs)
    {
        LifeEvent e;
        e.kind = kind;
        e.sound = sound;
        e.timeUs = timeUs;
        return e;
    }

    AdjustEvent Adjusted(Adjust kind, std::uint64_t sound, std::uint64_t timeUs, float v0, float v1 = 0.0f, float v2 = 0.0f,
        std::uint32_t hash = 0)
    {
        AdjustEvent e;
        e.kind = kind;
        e.sound = sound;
        e.timeUs = timeUs;
        e.hash = hash;
        e.value[0] = v0;
        e.value[1] = v1;
        e.value[2] = v2;
        return e;
    }

    OwnerInfo Horse()
    {
        OwnerInfo owner;
        owner.found = true;
        owner.id = 0x00030002u;
        owner.cls = OwnerClass::Horse;
        return owner;
    }

    void TestStableId()
    {
        const StableId hoof = StableIdOf(kHoofRef, false);
        Check(hoof.kind == IdKind::RefSet && hoof.id == 0x40029934aull, "stable id: set and variant of a ref of the known form");
        Check(RefOfStableId(hoof.id) == kHoofRef, "stable id: the ref comes back with the flag bit");
        const StableId other = StableIdOf(0x0080000c0029934aull, false);
        Check(other.kind == IdKind::RefSet && other.id == 0xc0029934aull && other.id != hoof.id,
            "stable id: another variant of the same set is another id");
        Check(StableIdOf(0x29934aull, false).kind == IdKind::RefSet, "stable id: a ref without the flag bit still has the form");
        const StableId odd = StableIdOf(0x008100040029934aull, false);
        Check(odd.kind == IdKind::None && odd.id == 0, "stable id: a bit outside set, variant and flag means no stable id");
        Check(StableIdOf(0, false).kind == IdKind::None, "stable id: ref 0 has none");
        Check(StableIdOf(0x0080000400000000ull, false).kind == IdKind::None, "stable id: an empty set has none");
        const StableId hash = StableIdOf(0xDEADBEEFull, true);
        Check(hash.kind == IdKind::Hash && hash.id == 0xDEADBEEFull, "stable id: creator B keeps the hash");
        Check(StableIdOf(0, true).kind == IdKind::None, "stable id: hash 0 has none");
    }

    void TestFamilyCaller()
    {
        const std::uint32_t queued[kCallerFrames] = {kWrapper, kGameCaller, 0, 0};
        Check(FamilyCaller(queued, kCallerFrames) == kGameCaller, "family: the first frame outside the wrappers");
        const std::uint32_t direct[kCallerFrames] = {0x251686C, 0x2589214, 0x313C5E8, 0};
        Check(FamilyCaller(direct, kCallerFrames) == 0x313C5E8, "family: the creators' entries are skipped too");
        const std::uint32_t only[kCallerFrames] = {kWrapper, kDrain, 0, 0};
        Check(FamilyCaller(only, kCallerFrames) == kWrapper, "family: all inside the wrappers: the nearest");
        const std::uint32_t none[kCallerFrames] = {};
        Check(FamilyCaller(none, kCallerFrames) == 0, "family: no frame, no caller");
        Check(InAudioEntityWrappers(0x258A348) && InAudioEntityWrappers(0x312D8BC) && !InAudioEntityWrappers(0x2F1CEF8),
            "family: the wrapper ranges");
    }

    void TestRing()
    {
        EventRing<AdjustEvent, 8> ring;
        for (std::uint32_t n = 0; n < 5; ++n)
            ring.Push(Adjusted(Adjust::Volume, n + 1, n, static_cast<float>(n)));
        Check(ring.Pending() == 5 && ring.Pushed() == 5, "ring: five pending");
        std::vector<std::uint64_t> seen;
        std::uint64_t dropped = 0;
        const std::uint64_t read = ring.Drain([&](const AdjustEvent& e) { seen.push_back(e.sound); }, dropped);
        Check(read == 5 && dropped == 0 && seen.size() == 5 && seen[0] == 1 && seen[4] == 5, "ring: drained in order");
        Check(ring.Pending() == 0, "ring: nothing pending after the drain");
        for (std::uint32_t n = 0; n < 11; ++n)
            ring.Push(Adjusted(Adjust::Volume, 100 + n, n, 0.0f));
        seen.clear();
        dropped = 0;
        ring.Drain([&](const AdjustEvent& e) { seen.push_back(e.sound); }, dropped);
        Check(dropped == 3 && seen.size() == 8 && seen.front() == 103 && seen.back() == 110,
            "ring: eleven pushes into eight slots drop the three oldest");
        ring.Push(Adjusted(Adjust::Volume, 1, 0, 0.0f));
        ring.Skip();
        Check(ring.Pending() == 0 && ring.Cursor() == ring.Pushed(), "ring: skip forgets what was pushed");
        // Two writers, one reader: every event read is whole, and what was
        // not read was counted as dropped.
        EventRing<AdjustEvent, 1024> shared;
        std::atomic<bool> go{false};
        const auto writer = [&](std::uint64_t id) {
            while (!go.load())
            {
            }
            for (std::uint32_t n = 0; n < 20000; ++n)
                shared.Push(Adjusted(Adjust::Position, id, n, static_cast<float>(id), static_cast<float>(id), static_cast<float>(id)));
        };
        std::thread a(writer, 7), b(writer, 9);
        go.store(true);
        std::uint64_t whole = 0;
        std::uint64_t torn = 0;
        std::uint64_t lost = 0;
        for (int rounds = 0; rounds < 4000; ++rounds)
        {
            shared.Drain([&](const AdjustEvent& e) {
                const float id = static_cast<float>(e.sound);
                if ((e.sound == 7 || e.sound == 9) && e.value[0] == id && e.value[1] == id && e.value[2] == id)
                    ++whole;
                else
                    ++torn;
            }, lost);
        }
        a.join();
        b.join();
        shared.Drain([&](const AdjustEvent& e) {
            const float id = static_cast<float>(e.sound);
            if ((e.sound == 7 || e.sound == 9) && e.value[0] == id && e.value[1] == id && e.value[2] == id)
                ++whole;
            else
                ++torn;
        }, lost);
        Check(torn == 0 && whole + lost == 40000, "ring: two writers, every event read is whole, the rest counted");
    }

    void TestRecorderCreates()
    {
        Recorder recorder;
        Track track;
        Config config;
        recorder.Begin(config, &track, 0);
        // A queued request answered 35 ms later.
        recorder.Life(Enqueue(0xA, kHoofRef, 1000000), 100, OwnerInfo{});
        Check(recorder.Pending() == 1, "creates: the request waits");
        recorder.Life(Create(0xA, kHoofRef, 0x1000, 1035000, true), 135, Horse());
        Check(track.creates.size() == 1 && recorder.Pending() == 0, "creates: the create took its request");
        if (track.creates.size() == 1)
        {
            const CreatePacket& p = track.creates[0];
            Check(p.timeMs == 100 && p.createDelayMs == 35, "creates: the packet's time is the request's, the delay kept");
            Check(p.source == Source::Queue && p.caller == kGameCaller && p.path[0] == kWrapper && p.path[1] == kGameCaller,
                "creates: the request's caller path names the family");
            Check(p.stableId == 0x40029934aull && p.idKind == IdKind::RefSet && p.rawRef == kHoofRef, "creates: the stable id and the raw ref");
            Check(p.owner == 0x00030002u && p.ownerClass == OwnerClass::Horse && p.index == 0, "creates: the owner and the first index");
            Check(p.hasParams && p.params.volume == -6.0f && p.params.pan == -1 && !p.hasPosition, "creates: the init params");
        }
        // A direct create (no request): the creator's instant, the census path.
        recorder.Life(Create(0xB, 0x0080000100001234ull, 0x2000, 2000000, false), 400, OwnerInfo{});
        Check(track.creates.size() == 2 && track.creates[1].source == Source::Direct && track.creates[1].timeMs == 400 &&
                track.creates[1].caller == 0x313C5E8 && track.creates[1].index == 1 && track.creates[1].owner == kNoOwner,
            "creates: a direct create at the creator's time with the census path");
        // A refused create takes its request away and makes no packet.
        recorder.Life(Enqueue(0xC, kHoofRef, 3000000), 500, OwnerInfo{});
        recorder.Life(Create(0xC, kHoofRef, 0, 3001000, true), 501, OwnerInfo{});
        Check(track.creates.size() == 2 && recorder.Pending() == 0 && recorder.StatsNow().createsRefused == 1,
            "creates: a refusal drops the request");
        // Speech and the mod's own sounds stay out.
        LifeEvent speech = Create(0xD, 0x00800004004366e8ull, 0x3000, 4000000, false);
        speech.speech = true;
        recorder.Life(speech, 600, OwnerInfo{});
        LifeEvent mine = Create(0xE, kHoofRef, 0x4000, 4000000, false);
        mine.mine = true;
        recorder.Life(mine, 600, OwnerInfo{});
        Check(track.creates.size() == 2 && recorder.StatsNow().createsSpeech == 1 && recorder.StatsNow().createsMine == 1,
            "creates: speech and the mod's own sounds are skipped");
        // Position owner, no owner, no stable id, by hash.
        LifeEvent placed = Create(0xF, 0x0081000400001111ull, 0x5000, 5000000, false);
        placed.params.position[0] = 100.0f;
        recorder.Life(placed, 700, OwnerInfo{});
        LifeEvent byHash = Create(0x10, 0xABCD, 0x6000, 5000000, false);
        byHash.core = 1;
        byHash.hasParams = false;
        recorder.Life(byHash, 700, OwnerInfo{});
        const Stats& stats = recorder.StatsNow();
        Check(track.creates.size() == 4 && track.creates[2].hasPosition && track.creates[2].idKind == IdKind::None &&
                track.creates[3].idKind == IdKind::Hash && track.creates[3].stableId == 0xABCD,
            "creates: a placed sound without a stable id, a sound by hash");
        Check(stats.createsRecorded == 4 && stats.createsNoStableId == 1 && stats.createsByHash == 1 && stats.ownerEntity == 1 &&
                stats.ownerPosition == 1 && stats.ownerNone == 2 && stats.createsFromQueue == 1 && stats.createsDirect == 3,
            "creates: the counts");
        // A deferred request matches a create that did not come through the queue's drain.
        recorder.Life(Enqueue(0x11, kHoofRef, 6000000, Source::Deferred), 800, OwnerInfo{});
        recorder.Life(Create(0x11, kHoofRef, 0x7000, 6002000, false), 802, OwnerInfo{});
        Check(track.creates.size() == 5 && track.creates[4].source == Source::Deferred && track.creates[4].timeMs == 800 &&
                stats.createsFromDeferred == 1,
            "creates: the deferred queue's request");
        // Times never go backwards (events of several threads in one drain).
        recorder.Life(Create(0x12, kHoofRef, 0x8000, 6002000, false), 790, OwnerInfo{});
        Check(track.creates.back().timeMs == 800, "creates: a packet time is clamped to the previous one (the deferred packet took its request's 800)");
        // Requests nobody answered expire.
        recorder.Life(Enqueue(0x13, kHoofRef, 7000000), 900, OwnerInfo{});
        recorder.Tick(7000000 + Recorder::kPendingExpireUs, 5900);
        Check(recorder.Pending() == 0 && stats.pendingExpired == 1, "creates: a request older than 5 s expires");
        Check(stats.bytes == 6 * kCreatePacketBytesV4Min, "creates: the bytes are the clip's"); // SGEN v4
        recorder.End();
        Check(stats.liveAtStop == 6 && !recorder.Active(), "creates: the sounds alive at the stop");
    }

    void TestSampler()
    {
        Recorder recorder;
        Track track;
        Config config;
        config.updateHz = 20; // 50 ms
        recorder.Begin(config, &track, 0);
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1000, -3.0f), 1);
        Check(track.adjusts.empty() && recorder.StatsNow().adjustUnknown == 1, "sampler: a sound without a create is counted, not kept");
        recorder.Life(Create(0xA, kHoofRef, 0x1000, 1000000, false), 100, OwnerInfo{});
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1000000, -3.0f), 100);
        Check(track.adjusts.size() == 1 && track.adjusts[0].kind == Adjust::Volume && track.adjusts[0].value[0] == -3.0f &&
                track.adjusts[0].index == 0 && track.adjusts[0].timeMs == 100,
            "sampler: the first value of a setting is recorded");
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1010000, -3.0f), 110);
        Check(track.adjusts.size() == 1 && recorder.StatsNow().adjustUnchanged == 1, "sampler: the same value again is not");
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1020000, -4.0f), 120);
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1030000, -5.0f), 130);
        Check(track.adjusts.size() == 1 && recorder.StatsNow().adjustThrottled == 2, "sampler: a change within 50 ms waits");
        recorder.Tick(1040000, 140);
        Check(track.adjusts.size() == 1, "sampler: the tick before the interval keeps waiting");
        recorder.Tick(1050000, 150);
        Check(track.adjusts.size() == 2 && track.adjusts[1].value[0] == -5.0f && track.adjusts[1].timeMs == 150 &&
                recorder.StatsNow().adjustTrailing == 1,
            "sampler: the last value of the burst is recorded at the tick after the interval");
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1200000, -8.0f), 300);
        Check(track.adjusts.size() == 3 && track.adjusts[2].value[0] == -8.0f, "sampler: a change after the interval goes at once");
        // Other kinds are sampled apart; a position keeps its three values.
        recorder.AdjustCall(Adjusted(Adjust::Position, 0x1000, 1200000, 1.0f, 2.0f, 3.0f), 300);
        recorder.AdjustCall(Adjusted(Adjust::Pitch, 0x1000, 1200000, -200.0f), 300);
        Check(track.adjusts.size() == 5 && track.adjusts[3].kind == Adjust::Position && track.adjusts[3].value[2] == 3.0f &&
                track.adjusts[4].kind == Adjust::Pitch && track.adjusts[4].value[0] == -200.0f,
            "sampler: each setting has its own sampler");
        // Sound variables: one sampler per hash, up to eight.
        for (std::uint32_t hash = 1; hash <= 9; ++hash)
            recorder.AdjustCall(Adjusted(Adjust::Variable, 0x1000, 1200000, 0.5f, 0.0f, 0.0f, hash), 300);
        Check(track.adjusts.size() == 14 && track.adjusts[5].hash == 1 && recorder.StatsNow().adjustVariableOverflow == 1,
            "sampler: nine hashes, the ninth recorded without a sampler");
        recorder.AdjustCall(Adjusted(Adjust::Variable, 0x1000, 1210000, 0.5f, 0.0f, 0.0f, 1), 310);
        Check(track.adjusts.size() == 14, "sampler: the same variable value is not recorded again");
        const Stats& stats = recorder.StatsNow();
        Check(stats.adjustCalls == 18 && stats.adjustRecorded == 14 && stats.adjustCallsByKind[static_cast<int>(Adjust::Variable)] == 10 &&
                stats.adjustRecordedByKind[static_cast<int>(Adjust::Volume)] == 3,
            "sampler: the counts per kind");
        Check(stats.bytes == kCreatePacketBytesV4Min + 3 * AdjustPacketBytes(Adjust::Volume) + AdjustPacketBytes(Adjust::Position) +
                    AdjustPacketBytes(Adjust::Pitch) + 9 * AdjustPacketBytes(Adjust::Variable),
            "sampler: the bytes");
        Check(AdjustPacketBytes(Adjust::Position) == 21 && AdjustPacketBytes(Adjust::Volume) == 13 &&
                AdjustPacketBytes(Adjust::Variable) == 17 && kCreatePacketBytes == 113 && kCreatePacketBytesV1 == 102 &&
                kEndPacketBytes == 9,
            "packets: the clip bytes per kind (SGEN v2: the create packet grew by 11 bytes)");
    }

    void TestEnds()
    {
        Recorder recorder;
        Track track;
        recorder.Begin(Config{}, &track, 0);
        recorder.Life(End(LifeKind::Release, 0x9999, 1000), 1, OwnerInfo{});
        Check(track.ends.empty() && recorder.StatsNow().endsUnknown == 1, "ends: a release of an unknown sound is counted");
        recorder.Life(Create(0xA, kHoofRef, 0x1000, 1000000, false), 100, OwnerInfo{});
        recorder.Life(Create(0xB, kHoofRef, 0x2000, 1000000, false), 100, OwnerInfo{});
        recorder.Life(End(LifeKind::StopAndForget, 0x1000, 1500000), 600, OwnerInfo{});
        recorder.Life(End(LifeKind::Stop, 0x1000, 1600000), 700, OwnerInfo{});
        Check(track.ends.size() == 1 && track.ends[0].reason == EndReason::StopAndForget && track.ends[0].timeMs == 600 &&
                track.ends[0].index == 0,
            "ends: one stop packet per sound, the first of S13 and S14");
        recorder.Life(End(LifeKind::Release, 0x1000, 2000000), 1100, OwnerInfo{});
        Check(track.ends.size() == 2 && track.ends[1].reason == EndReason::Release && recorder.Live() == 1,
            "ends: the release is the end, the sound leaves the live table");
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 2100000, -1.0f), 1200);
        Check(track.adjusts.empty() && recorder.StatsNow().adjustUnknown == 1, "ends: an adjust after the release is unknown");
        recorder.Life(End(LifeKind::Release, 0x2000, 2200000), 1300, OwnerInfo{});
        const Stats& stats = recorder.StatsNow();
        Check(stats.endsStop == 0 && stats.endsStopAndForget == 1 && stats.endsRelease == 2 && stats.endsReleaseAfterStop == 1 &&
                stats.livePeak == 2 && recorder.Live() == 0,
            "ends: the counts");
        Check(stats.bytes == 2 * kCreatePacketBytesV4Min + 3 * kEndPacketBytes, "ends: the bytes"); // SGEN v4
    }

    void TestGovernor()
    {
        Check(EffectiveHz(20, 0) == 20 && EffectiveHz(20, 1) == 10 && EffectiveHz(20, 3) == 2 && EffectiveHz(1, 2) == 1 &&
                EffectiveHz(20, kGovernorAdjustsOff) == 0,
            "governor: the rate per level");
        Governor governor;
        governor.Begin(1.0, 0); // 1 MB a minute
        // 200 KB a second for three seconds: 12 MB a minute over the window, the level rises each second.
        std::uint64_t now = 0;
        for (int second = 1; second <= 3; ++second)
        {
            governor.Note(200 * 1024);
            now = static_cast<std::uint64_t>(second) * 1000000ull;
            governor.Tick(now);
        }
        Check(governor.Level() == 3 && governor.Changes() == 3 && governor.RateMBPerMin() > 1.0, "governor: the level rises while over the cap");
        for (int second = 4; second <= 6; ++second)
        {
            governor.Note(200 * 1024);
            now = static_cast<std::uint64_t>(second) * 1000000ull;
            governor.Tick(now);
        }
        Check(governor.Level() == kGovernorAllOff && governor.AdjustsOff() && governor.AllOff(), "governor: the last level turns everything off");
        // Quiet: after the ten second window empties and ten calm seconds, one level back.
        for (int second = 7; second <= 40; ++second)
        {
            now = static_cast<std::uint64_t>(second) * 1000000ull;
            governor.Tick(now);
        }
        Check(governor.Level() < kGovernorAllOff && governor.Level() >= 2, "governor: it recovers one level per ten calm seconds");
        Check(governor.SecondsAtLevel(0) == 1 && governor.SecondsAtLevel(kGovernorAllOff) >= 10, "governor: the seconds per level");
        // The recorder under the budget: adjusts off, then creates and ends off.
        Recorder recorder;
        Track track;
        Config config;
        config.maxMBPerMin = 0.0001; // about 105 bytes a minute: one create is over it
        recorder.Begin(config, &track, 0);
        recorder.Life(Create(0xA, kHoofRef, 0x1000, 1000, false), 1, OwnerInfo{});
        for (int second = 1; second <= 4; ++second)
            recorder.Tick(static_cast<std::uint64_t>(second) * 1000000ull, static_cast<std::uint32_t>(second) * 1000);
        Check(recorder.StatsNow().governorLevel == kGovernorAdjustsOff, "governor: the recorder reached the adjusts-off level");
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 4000000, -1.0f), 4000);
        Check(track.adjusts.empty() && recorder.StatsNow().adjustBudget == 1, "governor: adjusts dropped by the budget");
        recorder.Life(Create(0xB, kHoofRef, 0x2000, 4000000, false), 4000, OwnerInfo{});
        Check(track.creates.size() == 2, "governor: creates still recorded at the adjusts-off level");
        recorder.Tick(5000000, 5000);
        Check(recorder.StatsNow().governorLevel == kGovernorAllOff, "governor: the recorder reached the all-off level");
        recorder.Life(Create(0xC, kHoofRef, 0x3000, 5000000, false), 5000, OwnerInfo{});
        recorder.Life(End(LifeKind::Release, 0x1000, 5000000), 5000, OwnerInfo{});
        Check(track.creates.size() == 2 && track.ends.empty() && recorder.StatsNow().createsBudget == 1 && recorder.StatsNow().endsBudget == 1,
            "governor: creates and ends dropped at the last level");
        Check(recorder.Live() == 1, "governor: a dropped release still leaves the live table");
        Check(Config{}.updateHz == 20 && Config{}.maxMBPerMin == 8.0, "config: the defaults");
    }

    Track SampleTrack()
    {
        Track track;
        track.nextIndex = 3;
        for (std::uint32_t n = 0; n < 3; ++n)
        {
            CreatePacket c;
            c.timeMs = 1000 * n;
            c.index = n;
            c.stableId = 0x40029934aull + n;
            c.idKind = IdKind::RefSet;
            c.rawRef = kHoofRef + n;
            c.caller = 0x2EFA7E0 + (n == 2 ? 0x100 : 0);
            c.owner = n == 1 ? kNoOwner : 0x00030002u;
            c.ownerClass = n == 1 ? OwnerClass::World : OwnerClass::Horse;
            track.creates.push_back(c);
        }
        const auto adjust = [&](std::uint32_t timeMs, std::uint32_t index, Adjust kind, float v0) {
            AdjustPacket a;
            a.timeMs = timeMs;
            a.index = index;
            a.kind = kind;
            a.value[0] = v0;
            track.adjusts.push_back(a);
        };
        adjust(100, 0, Adjust::Volume, -1.0f);
        adjust(500, 0, Adjust::Volume, -2.0f);
        adjust(1200, 1, Adjust::Pitch, 100.0f);
        adjust(1500, 0, Adjust::Volume, -3.0f);
        adjust(2500, 2, Adjust::Volume, -4.0f);
        const auto end = [&](std::uint32_t timeMs, std::uint32_t index, EndReason reason) {
            EndPacket e;
            e.timeMs = timeMs;
            e.index = index;
            e.reason = reason;
            track.ends.push_back(e);
        };
        end(1400, 1, EndReason::StopAndForget);
        end(1800, 1, EndReason::Release);
        end(2900, 2, EndReason::Release);
        return track;
    }

    void TestTrimAndCut()
    {
        {
            Track track = SampleTrack();
            const std::size_t dropped = TrimBefore(track, 2000);
            // Sound 1 was released at 1800: it goes with its adjust and its two ends. Sound 0 is still alive: it stays whole.
            Check(dropped == 4 && track.creates.size() == 2 && track.adjusts.size() == 4 && track.ends.size() == 1 &&
                    track.creates[0].index == 0 && track.creates[1].index == 2 && track.ends[0].index == 2,
                "trim: packets of a sound released before the base go, a live sound keeps its state");
            Track stale = SampleTrack();
            stale.ends.clear();
            TrimBefore(stale, kUnendedGraceMs + 1500);
            Check(stale.creates.size() == 1 && stale.creates[0].index == 2, "trim: a create older than the grace without an end goes");
        }
        {
            Track out;
            CutInto(SampleTrack(), out, 1300, 2600);
            // Sound 0 (created at 0, alive): preexisting at 0 with its latest volume (-2 at 500) at 0, then its adjust at 1500 -> 200.
            // Sound 1 (created at 1000, released at 1800): preexisting at 0 with its pitch at 0; its ends shifted (100, 500).
            // Sound 2 (created at 2000): shifted to 700; its adjust at 2500 -> 1200; its end at 2900 is past the window.
            Check(out.nextIndex == 3 && out.creates.size() == 3 && out.creates[0].preexisting && out.creates[0].timeMs == 0 &&
                    out.creates[1].preexisting && out.creates[2].timeMs == 700 && !out.creates[2].preexisting,
                "cut: creates in the window shifted, the ones alive at base at 0");
            Check(out.adjusts.size() == 4 && out.adjusts[0].timeMs == 0 && out.adjusts[0].index == 0 && out.adjusts[0].value[0] == -2.0f &&
                    out.adjusts[1].timeMs == 0 && out.adjusts[1].index == 1 && out.adjusts[1].kind == Adjust::Pitch &&
                    out.adjusts[2].timeMs == 200 && out.adjusts[2].value[0] == -3.0f && out.adjusts[3].timeMs == 1200,
                "cut: the latest setting before base at 0, the window's adjusts shifted");
            Check(out.ends.size() == 2 && out.ends[0].timeMs == 100 && out.ends[1].timeMs == 500 && out.ends[1].reason == EndReason::Release,
                "cut: ends in the window shifted, the one past the end dropped");
            Track later;
            CutInto(SampleTrack(), later, 1900, 3000);
            Check(later.creates.size() == 2 && later.creates[0].index == 0 && later.creates[1].index == 2 && later.ends.size() == 1,
                "cut: a sound released before base is left out");
        }
    }

    void TestReport()
    {
        Track track = SampleTrack();
        Stats stats;
        stats.recordedMs = 3000;
        stats.createsSeen = 5;
        stats.createsRecorded = 3;
        stats.createsRefused = 1;
        stats.createsSpeech = 1;
        stats.createsFromQueue = 2;
        stats.createsDirect = 1;
        stats.delayMsTotal = 70;
        stats.delayMsMax = 50;
        stats.adjustCalls = 300;
        stats.adjustRecorded = 5;
        stats.adjustCallsByKind[static_cast<int>(Adjust::Volume)] = 250;
        stats.adjustRecordedByKind[static_cast<int>(Adjust::Volume)] = 4;
        stats.endsRelease = 2;
        stats.endsStopAndForget = 1;
        stats.ownerEntity = 2;
        stats.ownerNone = 1;
        stats.bytes = track.FileBytes();
        stats.drainFrames = 180;
        stats.drainUsTotal = 1800.0;
        stats.drainUsMax = 40.0;
        stats.updateHz = 20;
        stats.maxMBPerMin = 8.0;
        stats.governorLevel = 0;
        stats.secondsAtLevel[0] = 3;
        ReportHeader header;
        header.clip = "042-Valentine.rpclip";
        header.version = "1.0.0-dev";
        header.written = "2026-10-07 01:00:00";
        header.durationMs = 3000;
        header.censusSounds = 4;
        const std::string text = FormatReport(track, stats, header);
        Check(Has(text, "SOUND_GENERIC_REPORT,format=1,version=1.0.0-dev,clip=042-Valentine.rpclip,"), "report: the header");
        Check(Has(text, ("PACKETS,create=3,adjust=5,end=3,bytes=" + std::to_string(track.FileBytes()) + ",").c_str()), "report: the packets");
        Check(Has(text, "CREATES,seen=5,recorded=3,refused=1,speech=1,") && Has(text, "delay_ms_mean=35,delay_ms_max=50"),
            "report: the creates and the queue delay");
        Check(Has(text, "CENSUS,sounds=4,refused=0"), "report: the census count");
        Check(Has(text, "ID,stable=3,hash=0,none=0") && Has(text, "OWNER,entity=2,position=0,none=1"), "report: ids and owners");
        Check(Has(text, "ADJUSTS,calls=300,recorded=5,") && Has(text, "calls_per_s=100,recorded_per_s=1.66667"), "report: the adjust rates");
        Check(Has(text, "ADJUST_KIND,kind=volume,port=S18,calls=250,recorded=4"), "report: a kind's line");
        Check(Has(text, "ENDS,release=2,stop=0,stop_and_forget=1,"), "report: the ends");
        Check(Has(text, "COST,drain_frames=180,drain_us_mean=10,drain_us_max=40,"), "report: the cost");
        Check(Has(text, "BUDGET,level=0,changes=0,rate_mb_per_min=0,seconds_at_level=3;0;0;0;0;0"), "report: the budget");
        Check(Has(text, "FAMILIES,count=2") && Has(text, "FAMILY,caller=0x2efa7e0,creates=2,adjusts=4,ends=2,bytes=") &&
                Has(text, "FAMILY,caller=0x2efa8e0,creates=1,adjusts=1,ends=1,") && Has(text, "classes=world:1;horse:1"),
            "report: families by caller, the largest first");
        Check(text.compare(text.size() - 4, 4, "END\n") == 0, "report: ends with END");
        const std::string line = FormatLogLine(stats, "record", 2, 1);
        Check(line.rfind("SOUND_GENERIC,phase=record,recorded_ms=3000,creates=3,adjusts=5,adjust_calls=300,ends=3,bytes=", 0) == 0 &&
                Has(line, ",live=2,pending=1,") && !Has(line, "\n"),
            "report: the log line");
    }

    // The init params' variable slots past the count are not taken (the game can
    // leave 0xFFFFFFFF there, a NaN the clip's reader refuses).
    void TestInitParamsSlots()
    {
        std::uint8_t params[kParamsBytes];
        std::memset(params, 0xFF, sizeof(params)); // the game's leftovers everywhere
        const float zero = 0.0f;
        for (std::size_t n = 0; n < 3; ++n)
            std::memcpy(params + kParamsPosition + n * 4, &zero, 4);
        const float volume = -6.0f;
        std::memcpy(params + kParamsVolume, &volume, 4);
        std::memcpy(params + kParamsVolume2, &zero, 4);
        const std::int32_t count = 1;
        std::memcpy(params + kParamsVariableCount, &count, 4);
        const std::uint32_t hash = 0x644dcf02u;
        const float value = 0.75f;
        std::memcpy(params + kParamsVariables, &hash, 4);
        std::memcpy(params + kParamsVariables + 4, &value, 4);
        const InitParams out = ReadInitParams(params);
        Check(out.variableCount == 1 && out.variables[0].hash == hash && out.variables[0].value == value,
            "init params: the named variable is taken");
        Check(out.variables[1].hash == 0 && out.variables[1].value == 0.0f && out.variables[2].hash == 0 && out.variables[2].value == 0.0f,
            "init params: the slots past the count are zeros, not the game's leftovers");
        const std::int32_t none = 0;
        std::memcpy(params + kParamsVariableCount, &none, 4);
        const InitParams empty = ReadInitParams(params);
        Check(empty.variableCount == 0 && empty.variables[0].hash == 0 && empty.variables[0].value == 0.0f,
            "init params: a count of zero takes no slot");
        const std::int32_t many = 7;
        std::memcpy(params + kParamsVariableCount, &many, 4);
        Check(ReadInitParams(params).variableCount == 3, "init params: the count is capped at the three slots");
    }
    // The create frame's values are recorded at once (the creator's 0 dB then the
    // owner's -88 dB microseconds later), and a value still waiting for its
    // interval is recorded at its sound's end.
    void TestStartFrameAndEndFlush()
    {
        Recorder recorder;
        Track track;
        Config config;
        config.updateHz = 20; // 50 ms
        recorder.Begin(config, &track, 0);
        recorder.Life(Create(0xA, kHoofRef, 0x1000, 1000000, false), 100, OwnerInfo{});
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1000010, 0.0f), 100);
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1000040, -88.6f), 100);
        Check(track.adjusts.size() == 2 && track.adjusts[1].value[0] == -88.6f && track.adjusts[1].timeMs == 100,
            "start frame: the owner's value in the create's frame is recorded at once (before: 50 ms later or never)");
        recorder.AdjustCall(Adjusted(Adjust::Volume, 0x1000, 1010000, -70.0f), 110);
        Check(track.adjusts.size() == 2 && recorder.StatsNow().adjustThrottled == 1, "start frame: after the frame the interval holds again");
        recorder.Life(End(LifeKind::StopAndForget, 0x1000, 1015000), 115, OwnerInfo{});
        Check(track.adjusts.size() == 3 && track.adjusts[2].value[0] == -70.0f && track.adjusts[2].timeMs == 115 &&
                recorder.StatsNow().adjustFlushedAtEnd == 1,
            "end flush: the waiting value is recorded at the stop, before the end packet");
        Check(track.ends.size() == 1 && track.ends[0].timeMs == 115, "end flush: the end follows");
        recorder.Life(End(LifeKind::Release, 0x1000, 1033000), 133, OwnerInfo{});
        recorder.Tick(1100000, 200);
        Check(track.adjusts.size() == 3, "end flush: nothing left for a later tick");
    }

    // SOUND_CLOCK samples.
    void TestClockSampler()
    {
        ClockSampler sampler(100, 25);
        ClockSample out[2];
        Check(sampler.Tick(0, 1000000, true, out) == 1 && out[0].clipMs == 0, "clock: the first tick is a sample");
        std::size_t total = 1;
        std::uint32_t clip = 0;
        std::uint64_t qpc = 1000000;
        for (int i = 0; i < 60; ++i) // 1 s of 16 ms ticks at 1x
        {
            clip += 16;
            qpc += 16000;
            total += sampler.Tick(clip, qpc, true, out);
        }
        Check(total >= 9 && total <= 11, "clock: about one sample per 100 ms at 1x");
        // A pause: the playhead stays, QPC goes on; the tick before and the tick of it.
        std::size_t n = sampler.Tick(clip + 16, qpc + 16000, true, out);
        n = sampler.Tick(clip + 16, qpc + 66000, false, out);
        Check(n >= 1 && out[n - 1].clipMs == clip + 16 && !out[n - 1].normal, "clock: a pause is sampled at once");
        // A scrub back: the tick before is logged first when it was not.
        sampler.Tick(clip + 16, qpc + 82000, false, out);
        n = sampler.Tick(2000, qpc + 98000, true, out);
        Check(n == 2 && out[0].clipMs == clip + 16 && out[1].clipMs == 2000, "clock: a jump logs the tick before and the tick of it");
        Check(FormatClockLine("play", out[1]) == "SOUND_CLOCK,phase=play,clip_ms=2000,qpc_us=" + std::to_string(qpc + 98000) + ",normal=1",
            "clock: line format");
        sampler.Reset();
        Check(sampler.Tick(5, 7, true, out) == 1, "clock: reset starts over");
    }
}

int RunSoundGenericTests()
{
    TestClockSampler();
    TestStartFrameAndEndFlush();
    TestStableId();
    TestInitParamsSlots();
    TestFamilyCaller();
    TestRing();
    TestRecorderCreates();
    TestSampler();
    TestEnds();
    TestGovernor();
    TestTrimAndCut();
    TestReport();
    if (g_failures)
        std::fprintf(stderr, "%d generic sound recorder test(s) failed.\n", g_failures);
    return g_failures;
}
