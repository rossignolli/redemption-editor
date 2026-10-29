// Missing sound in a replay: the step complement that would double the steps,
// the census pairs (flag bit, same request with another sound, the recorded
// player's lines said by the hidden player), the speech rules a replayed line
// passes and the dead lines in the report's speech section.
#include "horse_sound_report.h"
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"
#include "speech_replay_rules.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::soundgeneric;
    using namespace replay::soundgeneric::player;
    namespace owner = replay::soundowner;
    namespace census = replay::census;
    namespace rules = replay::speech::rules;
    namespace hs = replay::horsesound;
    using census::OwnerClass;

    int g_missingSoundFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_missingSoundFailures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint32_t kPlayer = (3u << 16) | 4u;      // 3:4
    constexpr std::uint32_t kWeapon = (123u << 16) | 124u; // 123:124
    constexpr std::uint32_t kObject = (43u << 16) | 225u;  // 43:225

    // ------------------------------------------------------------ complement

    struct FakeEngine : Engine
    {
        census::OwnCreates own;
        std::uint64_t next = 1;
        std::vector<std::uint64_t> created;
        bool Create(const CreatePacket& packet, bool, std::uint64_t& handle, std::string& status, bool& retry) override
        {
            retry = false;
            handle = next++;
            created.push_back(packet.rawRef);
            status = "playing";
            return true;
        }
        bool Prepare(std::uint64_t) override { return true; }
        bool Play(std::uint64_t) override { return true; }
        void Stop(std::uint64_t, EndReason) override {}
        bool Alive(std::uint64_t) override { return true; }
        void Adjust(std::uint64_t, const AdjustPacket&) override {}
        bool MadeByOther(const CreatePacket& packet, std::uint32_t nowMs) override
        {
            return own.Take(packet.owner, owner::PacketSoundKey(packet.stableId, packet.rawRef), packet.timeMs, nowMs, 300);
        }
        bool MadeSameSetByOther(const CreatePacket& packet, std::uint32_t nowMs) override
        {
            return own.TakeSet(packet.owner, owner::PacketSoundKey(packet.stableId, packet.rawRef), packet.timeMs, nowMs, 300);
        }
        bool Made(std::uint64_t raw) const
        {
            for (const std::uint64_t r : created)
                if (r == raw)
                    return true;
            return false;
        }
    };

    CreatePacket Packet(std::uint32_t index, std::uint32_t t, std::uint64_t raw, std::uint32_t caller)
    {
        CreatePacket p;
        p.timeMs = t;
        p.index = index;
        p.rawRef = raw;
        const StableId id = StableIdOf(raw, false);
        p.stableId = id.id;
        p.idKind = id.kind;
        p.owner = kPlayer;
        p.ownerClass = OwnerClass::Player;
        p.caller = caller;
        p.hasParams = true;
        p.hasPosition = true;
        return p;
    }

    player::Config PacketsConfig()
    {
        player::Config c;
        c.packets = true;
        c.enabled = true;
        return c;
    }

    owner::Overrides PacketsOverrides()
    {
        owner::Overrides o;
        o.packets = true;
        return o;
    }

    // 60 ticks a second from 500 ms; the step call's creates of tick N reach
    // the census on tick N + 1, as in the mod.
    void Play(Player& player, FakeEngine& engine, const std::vector<std::pair<std::uint32_t, std::uint64_t>>& callMakes)
    {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> pending;
        std::size_t next = 0;
        for (std::uint32_t tick = 0; tick <= 60; ++tick)
        {
            const std::uint32_t now = 500 + (tick * 50) / 3;
            for (const auto& made : pending)
                engine.own.Add(kPlayer, owner::SoundKey(made.second), made.first);
            pending.clear();
            player.Tick(now, true, engine);
            while (next < callMakes.size() && callMakes[next].first <= now)
                pending.push_back({now, callMakes[next++].second});
        }
    }

    const SoundPlan* PlanOf(const Player& player, const Track& track, std::uint64_t raw)
    {
        for (const SoundPlan& plan : player.Plans())
            if (track.creates[plan.create].rawRef == raw)
                return &plan;
        return nullptr;
    }

    // A step of the player recorded as entry 6 of set 0x342bab and entry 0 of set
    // 0x33a352, plus the layer 0x1d9db9. The replayed step call makes the two sets
    // in other entries (0x19 and 6) and no layer: the two yield to the call, the
    // layer still plays.
    void TestStepComplementBySet()
    {
        Track track;
        track.creates.push_back(Packet(1, 600, 0x80000600342babull, 0x318E828u));
        track.creates.push_back(Packet(2, 600, 0x8000000033a352ull, 0x318DAFEu));
        track.creates.push_back(Packet(3, 600, 0x800000001d9db9ull, 0x318D0A0u));
        track.nextIndex = 4;
        owner::FootWindows feet;
        feet.Add(kPlayer, 600, 0);
        feet.Seal();
        owner::Cover cover;
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 500, &feet);
        Check(player.Stats().complement == 3, "1167 step: three complements");
        FakeEngine engine;
        Play(player, engine, {{601, 0x80001900342babull}, {601, 0x8000060033a352ull}});
        const SoundPlan* main = PlanOf(player, track, 0x80000600342babull);
        const SoundPlan* second = PlanOf(player, track, 0x8000000033a352ull);
        const SoundPlan* layer = PlanOf(player, track, 0x800000001d9db9ull);
        Check(main && main->lastOutcome == Outcome::Yielded && second && second->lastOutcome == Outcome::Yielded,
            "1167 step: a set the call made in another entry is not played again (no double step)");
        Check(!engine.Made(0x80000600342babull) && !engine.Made(0x8000000033a352ull),
            "1167 step: the generic player creates neither recorded entry");
        Check(layer && layer->lastOutcome == Outcome::Played && engine.Made(0x800000001d9db9ull),
            "1167 step: the layer the call never makes (1160) still plays");
        const PlayStats& s = player.Stats();
        Check(s.complementPlayed == 1 && s.complementYielded == 2 && s.complementYieldedSet == 2,
            "1167 step: one played, two yielded by set");
        Check(Has(FormatPlayLogLine(s, "stop", 1500, 0, 0), ",complement_yielded_set=2"), "1167 step: the log field");
        Check(Has(FormatSection(s, PacketsOverrides(), std::string()), "another entry of the same sound set: 2"),
            "1167 step: the report's line");
    }

    // The same entry still yields by MadeByOther (counted once, not by set).
    void TestStepComplementSameEntry()
    {
        Track track;
        track.creates.push_back(Packet(1, 600, 0x80000600342babull, 0x318E828u));
        track.nextIndex = 2;
        owner::FootWindows feet;
        feet.Add(kPlayer, 600, 0);
        feet.Seal();
        owner::Cover cover;
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 500, &feet);
        FakeEngine engine;
        Play(player, engine, {{601, 0x80000600342babull}});
        Check(player.Stats().complementYielded == 1 && player.Stats().complementYieldedSet == 0,
            "same entry: yielded by the exact sound");
    }

    // OwnCreates::TakeSet: one create answers one packet, owner and time kept.
    void TestTakeSet()
    {
        census::OwnCreates own;
        own.Add(kPlayer, owner::SoundKey(0x80001900342babull), 600);
        Check(!own.TakeSet(kWeapon, owner::SoundKey(0x80000600342babull), 600, 700, 300), "TakeSet: another owner");
        Check(!own.TakeSet(kPlayer, owner::SoundKey(0x8000060033a352ull), 600, 700, 300), "TakeSet: another set");
        Check(!own.TakeSet(kPlayer, owner::SoundKey(0x80000600342babull), 1000, 1100, 300), "TakeSet: too old");
        Check(own.TakeSet(kPlayer, owner::SoundKey(0x80000600342babull), 600, 700, 300), "TakeSet: same set, other entry");
        Check(!own.TakeSet(kPlayer, owner::SoundKey(0x80000600342babull), 600, 700, 300), "TakeSet: used once");
    }

    // ------------------------------------------------------------ census

    struct Rows
    {
        census::Summary summary;
        census::Accumulator acc;
        explicit Rows(bool play) { acc.Attach(&summary, play); }
        void Add(double ms, const census::SummaryKey& key) { acc.Add(ms, key, false); }
        void Cover(double from, double to) { acc.Cover(from, to); }
    };

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
        census::DiffResult Run() const { return census::Diff(rec.summary, play.summary, play.acc.Passes(), census::IgnoreList{}); }
    };

    census::SummaryKey Key(std::uint32_t who, OwnerClass cls, std::uint64_t ref, std::uint32_t a, std::uint32_t b,
        std::uint32_t c, std::uint32_t d, std::uint32_t e)
    {
        census::SummaryKey key;
        key.owner = who;
        key.cls = cls;
        key.ref = ref;
        key.path[0] = a;
        key.path[1] = b;
        key.path[2] = c;
        key.path[3] = d;
        key.path[4] = e;
        return key;
    }

    census::SummaryKey Shot(std::uint64_t ref)
    {
        return Key(kWeapon, OwnerClass::Weapon, ref, 0x2588eda, 0x2588db0, 0x2efaeaf, 0x2f0b883, 0x2f0dea0);
    }

    const census::Finding* Find(const census::DiffResult& diff, census::FindingKind kind, std::uint64_t ref)
    {
        for (const census::Finding& f : diff.findings)
            if (f.kind == kind && f.ref == ref)
                return &f;
        return nullptr;
    }

    // 43:225: recorded 0x7000ff81a, the generic player made it again with the
    // flag bit (0x800007000ff81a): one sound.
    void TestFlagBitIsOneSound()
    {
        Check(census::PairRef(0x800007000ff81aull) == 0x7000ff81aull && census::PairRef(0x7000ff81aull) == 0x7000ff81aull,
            "PairRef: the flag bit is dropped on a known form ref");
        Check(census::PairRef(0xfffff0118fe9eull) == 0xfffff0118fe9eull && census::PairRef(0xffffffff00493359ull) == 0xffffffff00493359ull,
            "PairRef: any other ref is whole");
        Pair p;
        p.CoverBoth(0, 4000);
        const census::SummaryKey rec = Key(kObject, OwnerClass::Object, 0x7000ff81aull, 0x251688d, 0x2589214, 0, 0x2589030, 0);
        census::SummaryKey play = rec;
        play.ref = 0x800007000ff81aull;
        play.path[3] = 0;
        p.rec.Add(2000, rec);
        p.play.Add(2010, play);
        const census::DiffResult d = p.Run();
        Check(d.findings.empty(), "43:225: the object sound made again with the flag is not missing nor extra");
    }

    // 123:124: the shot layer at 0x2EFAEAF, recorded fe9e on one shot and
    // f975 on two; every replayed shot made f975 (three, and two more
    // shots whose live layer was none). The lost fe9e is a variant; an
    // excess far over the loss keeps its own MORE line.
    void TestShotLayerVariant()
    {
        Pair p;
        p.CoverBoth(0, 30000);
        p.rec.Add(21000, Shot(0xfffff0118f975ull));
        p.rec.Add(22800, Shot(0xfffff0118f975ull));
        p.rec.Add(24600, Shot(0xfffff0118fe9eull));
        for (double ms : {21010.0, 22810.0, 24610.0, 26000.0, 27000.0})
            p.play.Add(ms, Shot(0xfffff0118f975ull));
        const census::DiffResult d = p.Run();
        const census::Finding* lost = Find(d, census::FindingKind::Variant, 0xfffff0118fe9eull);
        Check(lost && lost->lostSide && lost->sameRequest && lost->otherRef == 0xfffff0118f975ull,
            "123:124: the lost shot layer is a variant of the same call");
        Check(!Find(d, census::FindingKind::Missing, 0xfffff0118fe9eull), "123:124: not missing");

        // The other side below a finding (one over per pass is no EXTRA line).
        Pair q;
        q.CoverBoth(0, 30000);
        q.rec.Add(10000, Shot(0xfffff01192940ull));
        q.rec.Add(12000, Shot(0xfffff01192940ull));
        q.rec.Add(20700, Shot(0xfffff01192992ull));
        for (double ms : {10010.0, 12010.0, 20710.0})
            q.play.Add(ms, Shot(0xfffff01192940ull));
        const census::DiffResult e = q.Run();
        Check(Find(e, census::FindingKind::Variant, 0xfffff01192992ull) != nullptr,
            "a variant whose other side is no finding is still paired");

        // A loss with nothing made at its time stays MISSING.
        Pair r;
        r.CoverBoth(0, 30000);
        r.rec.Add(20700, Shot(0xfffff01192992ull));
        r.rec.Add(5000, Shot(0xfffff01192940ull));
        r.play.Add(5010, Shot(0xfffff01192940ull));
        r.play.Add(9000, Shot(0xfffff01192940ull));
        const census::DiffResult f = r.Run();
        Check(Find(f, census::FindingKind::Missing, 0xfffff01192992ull) != nullptr,
            "a shot layer with no other sound of the call at its time is missing");

        // The excess far over the loss keeps its MORE line.
        Pair m;
        m.CoverBoth(0, 30000);
        m.rec.Add(21000, Shot(0xfffff0118f975ull));
        m.rec.Add(24600, Shot(0xfffff0118fe9eull));
        for (double ms : {21010.0, 24610.0, 25000.0, 25500.0, 26000.0, 26500.0, 27000.0, 27500.0})
            m.play.Add(ms, Shot(0xfffff0118f975ull));
        const census::DiffResult g = m.Run();
        Check(Find(g, census::FindingKind::Variant, 0xfffff0118fe9eull) && Find(g, census::FindingKind::More, 0xfffff0118f975ull),
            "the loss is a variant and the excess stays MORE");
        const std::string report = census::FormatReport(g, census::ReportHeader{});
        Check(Has(report, "over at: "), "a MORE line says where the replay made more");
    }

    // The recorded player's lines said by the hidden player.
    void TestSpeechProxy()
    {
        census::SummaryKey wave = Key(census::kNoOwner, OwnerClass::LivePlayer, 0x80000400438196ull, 0x251688d, 0x2589214, 0,
            0x2fa10cf, 0x2fa02fa);
        census::SummaryKey key = wave;
        census::ProxySpeechKey(false, kPlayer, key);
        Check(key.cls == OwnerClass::LivePlayer, "proxy off: the hidden player's");
        census::ProxySpeechKey(true, kPlayer, key);
        Check(key.cls == OwnerClass::Player && key.owner == kPlayer, "proxy on: the line's wave is the recorded player's");
        census::SummaryKey layer = Key(census::kNoOwner, OwnerClass::LivePlayer, 0x80000200436a9cull, 0x251688d, 0x2589214, 0,
            0x2fa0dcc, 0x2fb44a6);
        census::ProxySpeechKey(true, kPlayer, layer);
        Check(layer.owner == kPlayer, "proxy on: a type 2 layer too");
        census::SummaryKey step = Key(census::kNoOwner, OwnerClass::LivePlayer, 0x80000600342babull, 0x2588eda, 0x258a469,
            0x2f1d23e, 0x2ee86b1, 0x25a74db);
        census::ProxySpeechKey(true, kPlayer, step);
        Check(step.cls == OwnerClass::LivePlayer, "proxy on: the hidden player's own step stays his");
        census::SummaryKey puppet = wave;
        puppet.cls = OwnerClass::Ped;
        puppet.owner = kWeapon;
        census::ProxySpeechKey(true, kPlayer, puppet);
        Check(puppet.owner == kWeapon, "proxy on: a puppet's line stays the puppet's");

        Pair p;
        p.CoverBoth(0, 30000);
        census::SummaryKey recorded = wave;
        recorded.owner = kPlayer;
        recorded.cls = OwnerClass::Player;
        p.rec.Add(21700, recorded);
        census::SummaryKey hidden = wave;
        census::ProxySpeechKey(true, kPlayer, hidden);
        p.play.Add(21720, hidden);
        Check(p.Run().findings.empty(), "proxy on: the heard line is not missing");
    }

    // ------------------------------------------------------------ speech rules

    void TestRules()
    {
        Check(rules::Decide(true, true, true, 28) == rules::kAccepted && rules::Decide(true, true, true, 41) == rules::kAccepted &&
                  rules::Decide(true, true, true, 48) == rules::kAccepted,
            "rules: a replayed line passes 28, 41 and 48");
        Check(rules::Decide(true, true, true, 18) == 18 && rules::Decide(true, true, true, 0x23) == 0x23 &&
                  rules::Decide(true, true, true, 0x2F) == 0x2F,
            "rules: 18 (the speaker's own line), 0x23 (no ped) and 0x2F (dead) stay");
        Check(rules::Decide(true, true, false, 48) == 48, "rules: a world line keeps the engine's code");
        Check(rules::Decide(false, true, true, 48) == 48, "rules: SpeechReplayRules=0 keeps the engine's code");
        Check(rules::Decide(true, false, true, 48) == 48, "rules: outside a playback nothing changes");
        Check(rules::Decide(true, true, true, rules::kAccepted) == rules::kAccepted, "rules: an accepted line stays accepted");
        Check(rules::Failed(48) && rules::Failed(28) && !rules::Failed(rules::kAccepted) && !rules::Failed(0),
            "rules: below 0x46 stops the line");
        static rules::ReplayLines lines;
        lines.Clear();
        lines.Note(0x1b23aea3cc0ull);
        Check(lines.Has(0x1b23aea3cc0ull) && !lines.Has(0x1b23aea52c0ull) && !lines.Has(0), "lines: noted and not noted");
        for (std::uint64_t n = 1; n <= rules::ReplayLines::kSize; ++n)
            lines.Note(0x10000ull + n * 0x2C0ull);
        Check(!lines.Has(0x1b23aea3cc0ull) && lines.Has(0x10000ull + rules::ReplayLines::kSize * 0x2C0ull),
            "lines: the ring keeps the newest");
        lines.Clear();
        Check(!lines.Has(0x10000ull + rules::ReplayLines::kSize * 0x2C0ull), "lines: cleared at a playback's start");
    }

    // ------------------------------------------------------------ dead lines

    void TestDeadLines()
    {
        hs::Tallies tallies;
        hs::SpeechTally& t = tallies.speech["92:93"];
        t.cls = hs::SpeakerClass::Person;
        t.recorded = 8;
        t.played = 8;
        t.direct = 8;
        for (int n = 0; n < 8; ++n)
            hs::NoteDied(t, "revalidation", 48);
        Check(t.died == 8 && t.played == 0 && hs::SpeechProblem(t), "92:93: eight dead lines, none played, silent");
        Check(t.reasons["died_revalidation_code_48"] == 8, "92:93: the reason with the rules' code");
        const std::string section = hs::FormatSection(tallies);
        Check(Has(section, "SILENT  owner 92:93 (person)") && Has(section, "died before the start 8") &&
                  Has(section, "died_revalidation_code_48 8") && Has(section, "8 died before their start"),
            "92:93: the speech section");
        Check(Has(hs::SummaryLine(tallies), "SPEECH OF PEOPLE: 8 lines created by the replay died before their start"),
            "92:93: the summary line");
        Check(!Has(section, "\xE2\x80\x94") && !Has(section, "\xE2\x80\x93"), "no dashes in the report");
        hs::SpeechTally quiet;
        hs::NoteDied(quiet, nullptr, -1);
        Check(quiet.reasons["died_unknown"] == 1 && quiet.played == 0, "a dead line without a cause or a code");
    }
}

int RunMissingSoundTests()
{
    TestStepComplementBySet();
    TestStepComplementSameEntry();
    TestTakeSet();
    TestFlagBitIsOneSound();
    TestShotLayerVariant();
    TestSpeechProxy();
    TestRules();
    TestDeadLines();
    return g_missingSoundFailures;
}
