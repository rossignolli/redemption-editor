// The step and melee complement of the generic player and the melee hit
// check, with numbers from recorded clips (the player 1:481 running into the
// ped 2:482 at 0.59 s; a clip with six melee hits).
#include "melee_sound_check.h"
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::soundgeneric;
    using namespace replay::soundgeneric::player;
    namespace owner = replay::soundowner;
    namespace mc = replay::meleecheck;
    using replay::census::OwnerClass;

    int g_weaponButtFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_weaponButtFailures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint32_t kPlayer = (1u << 16) | 481u; // 1:481
    constexpr std::uint32_t kHorse = (3u << 16) | 483u;  // a horse of the clip

    // The fake engine: every create plays; the other sources' creates (the
    // step track, the melee track) are census rows in an OwnCreates keyed
    // by SoundKey, as sound_generic_player.cpp has them.
    struct FakeEngine : Engine
    {
        replay::census::OwnCreates own;
        std::uint64_t next = 1;
        std::vector<std::uint64_t> created; // raw refs, in order
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
        bool Made(std::uint64_t raw) const
        {
            for (const std::uint64_t r : created)
                if (r == raw)
                    return true;
            return false;
        }
    };

    CreatePacket Packet(std::uint32_t index, std::uint32_t t, std::uint64_t raw, std::uint32_t caller, std::uint32_t who,
        OwnerClass cls)
    {
        CreatePacket p;
        p.timeMs = t;
        p.index = index;
        p.rawRef = raw;
        const StableId id = StableIdOf(raw, false);
        p.stableId = id.id;
        p.idKind = id.kind;
        p.owner = who;
        p.ownerClass = cls;
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

    // At 0.587 s: the player's three step packets (the step track
    // remakes the main sound 0x342bab entry 5 and the raw 0x398be8, never
    // the layer 0x1d9db9 of 0x318CEB4), the ped impact of 0.653 s (a sound
    // request of the generic player already), and a horse's layer.
    Track ContactTrack()
    {
        Track track;
        track.creates.push_back(Packet(13, 587, 0xfffff00398be8ull, 0x318DAFEu, kPlayer, OwnerClass::Player));
        track.creates.push_back(Packet(14, 587, 0x800000001d9db9ull, 0x318D0A0u, kPlayer, OwnerClass::Player));
        track.creates.push_back(Packet(15, 587, 0x80000500342babull, 0x318E828u, kPlayer, OwnerClass::Player));
        track.creates.push_back(Packet(22, 653, 0x800004000004fcull, 0x319BC33u, kPlayer, OwnerClass::Player));
        track.creates.push_back(Packet(30, 700, 0x8000050033fd85ull, 0x318D0A0u, kHorse, OwnerClass::Horse));
        track.nextIndex = 31;
        return track;
    }

    owner::FootWindows ContactFeet()
    {
        owner::FootWindows feet;
        feet.Add(kPlayer, 587, 0);
        feet.Add(kHorse, 700, 0);
        feet.Seal();
        return feet;
    }

    const SoundPlan* PlanOf(const Player& player, const Track& track, std::uint64_t raw)
    {
        for (const SoundPlan& plan : player.Plans())
            if (track.creates[plan.create].rawRef == raw)
                return &plan;
        return nullptr;
    }

    // 60 ticks a second from 500 ms; the step track's creates of tick N reach
    // the census (the engine's OwnCreates) on tick N + 1, as in the mod.
    void PlayContact(Player& player, FakeEngine& engine, const std::vector<std::pair<std::uint32_t, std::uint64_t>>& trackMakes)
    {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> pending;
        std::size_t next = 0;
        std::uint32_t previous = 500;
        for (std::uint32_t tick = 0; tick <= 60; ++tick)
        {
            const std::uint32_t now = 500 + (tick * 50) / 3;
            for (const auto& made : pending)
                engine.own.Add(kPlayer, owner::SoundKey(made.second), made.first);
            pending.clear();
            player.Tick(now, true, engine);
            while (next < trackMakes.size() && trackMakes[next].first <= now)
            {
                if (trackMakes[next].first > previous || tick == 0)
                    pending.push_back({now, trackMakes[next].second});
                ++next;
            }
            previous = now;
        }
    }

    void TestComplementFamily()
    {
        Check(owner::ComplementFamily(owner::Family::Footstep, OwnerClass::Player), "a player's step packet completes its step");
        Check(owner::ComplementFamily(owner::Family::Footstep, OwnerClass::Ped), "a ped's step packet completes its step");
        Check(owner::ComplementFamily(owner::Family::Melee, OwnerClass::Ped), "a ped's melee packet completes its hit");
        Check(!owner::ComplementFamily(owner::Family::Footstep, OwnerClass::Horse), "a horse's step keeps its track alone");
        Check(!owner::ComplementFamily(owner::Family::HorseStep, OwnerClass::Ped), "the wet hoof layer keeps its track");
        Check(!owner::ComplementFamily(owner::Family::Gunshot, OwnerClass::Player), "a shot keeps its track");
        Check(!owner::ComplementFamily(owner::Family::Melee, OwnerClass::Animal), "an animal's melee keeps its track");
    }

    void TestContact1160()
    {
        const Track track = ContactTrack();
        const owner::FootWindows feet = ContactFeet();
        owner::Cover cover;
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 500, &feet);
        const SoundPlan* layer = PlanOf(player, track, 0x800000001d9db9ull);
        const SoundPlan* main = PlanOf(player, track, 0x80000500342babull);
        const SoundPlan* raw = PlanOf(player, track, 0xfffff00398be8ull);
        const SoundPlan* impact = PlanOf(player, track, 0x800004000004fcull);
        const SoundPlan* hoof = PlanOf(player, track, 0x8000050033fd85ull);
        Check(layer && layer->family == owner::Family::Footstep && layer->complement && layer->taker == owner::Taker::Generic &&
                  !layer->noHold && !layer->preload,
            "1160: the player's step layer at the foot event is a complement (generic, with the hold)");
        Check(main && main->complement && raw && raw->complement, "1160: the main step sounds are complements too");
        Check(impact && !impact->complement && impact->taker == owner::Taker::Generic,
            "1160: the ped impact (0x319BC33) is the generic player's as before, not a complement");
        Check(hoof && !hoof->complement && hoof->taker == owner::Taker::Specific, "1160: a horse's step layer stays with its track");
        Check(player.Stats().complement == 3, "1160: three complements (the player's step packets)");

        FakeEngine engine;
        // The step track remakes the main step and the raw sound at 588 ms.
        PlayContact(player, engine, {{588, 0x80000500342babull}, {588, 0xfffff00398be8ull}});
        Check(layer->lastOutcome == Outcome::Played, "1160: the layer the step call did not make plays from the recording");
        Check(main->lastOutcome == Outcome::Yielded && raw->lastOutcome == Outcome::Yielded,
            "1160: the sounds the step call made are not played twice");
        Check(engine.Made(0x800000001d9db9ull) && !engine.Made(0x80000500342babull) && !engine.Made(0xfffff00398be8ull),
            "1160: only the layer is created by the generic player");
        Check(engine.Made(0x800004000004fcull), "1160: the ped impact still plays");
        Check(!engine.Made(0x8000050033fd85ull), "1160: the horse's layer is not created by the generic player");
        const PlayStats& s = player.Stats();
        Check(s.complementPlayed == 1 && s.complementYielded == 2 && s.doubles == 0,
            "1160: one complement played, two left to the step call, no double");
        Check(player.Complements().size() == 1 && player.Complements()[0].ref == 0x800000001d9db9ull &&
                  player.Complements()[0].family == owner::Family::Footstep && player.Complements()[0].lateMs >= 150,
            "1160: the evidence names the layer and how late it started (the hold)");
        const std::string line = FormatPlayLogLine(s, "stop", 1500, 0, 0);
        Check(Has(line, ",kept_loops=0,complement=3,complement_played=1,complement_yielded=2,complement_melee_played=0"),
            "1160: the log line carries the complement");
        Check(Has(FormatComplementLine(player.Complements()[0]),
                  "SOUND_GENERIC_COMPLEMENT,owner=1:481,family=footstep,ref=0x800000001d9db9,caller=0x318d0a0,event_ms=587,"),
            "1160: the complement line");
        Check(Has(FormatSection(s, PacketsOverrides(), std::string()), "Played here because the track's call did not make them again: 1"),
            "1160: the report's line");
    }

    // The step call's create reaches the census one tick late: the
    // complement still waits for it (two ticks after its time), even with
    // a hold shorter than a tick.
    void TestComplementWaitsForTheCall()
    {
        Track track;
        track.creates.push_back(Packet(1, 600, 0x80000500342babull, 0x318E828u, kPlayer, OwnerClass::Player));
        track.nextIndex = 2;
        owner::FootWindows feet;
        feet.Add(kPlayer, 600, 0);
        feet.Seal();
        owner::Cover cover;
        player::Config config = PacketsConfig();
        config.holdMs = 0;
        Player player;
        player.Begin(track, config, cover, PacketsOverrides(), 500, &feet);
        FakeEngine engine;
        PlayContact(player, engine, {{600, 0x80000500342babull}});
        const SoundPlan* main = PlanOf(player, track, 0x80000500342babull);
        Check(main && main->lastOutcome == Outcome::Yielded && !engine.Made(0x80000500342babull),
            "no hold: the complement waits the ticks for the call's census row, no double");
    }

    // Mode c's lead (the queue asked ahead) does not move the decision before
    // the track's call; a jump back makes the next pass wait again.
    struct LeadEngine : FakeEngine
    {
        std::uint32_t LeadMs(const CreatePacket&, owner::Family) const override { return 200; }
    };

    void TestComplementLeadAndJump()
    {
        Track track;
        track.creates.push_back(Packet(1, 600, 0x80000500342babull, 0x318E828u, kPlayer, OwnerClass::Player));
        track.nextIndex = 2;
        owner::FootWindows feet;
        feet.Add(kPlayer, 600, 0);
        feet.Seal();
        owner::Cover cover;
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 500, &feet);
        LeadEngine engine;
        PlayContact(player, engine, {{600, 0x80000500342babull}});
        const SoundPlan* main = PlanOf(player, track, 0x80000500342babull);
        Check(main && main->lastOutcome == Outcome::Yielded && !engine.Made(0x80000500342babull),
            "lead: the complement is decided on the playhead, after the call");
        // Jump back to 500 ms (not a step: a pass wrap) and play again; the
        // step call of this pass reaches the census late again.
        player.Tick(400, true, engine);
        PlayContact(player, engine, {{600, 0x80000500342babull}});
        Check(main->lastOutcome == Outcome::Yielded && !engine.Made(0x80000500342babull),
            "jump: the second pass waits for its own call, no double");
        Check(player.Stats().complementYielded == 2 && player.Stats().complementPlayed == 0, "jump: yielded once per pass");
    }

    // A melee packet of the hit that the melee call (after the generic
    // player in the tick) did not make plays; the one it made yields.
    void TestMeleeComplement()
    {
        Track track;
        track.creates.push_back(Packet(1, 12977, 0x8000010031c5e0ull, 0x318A2A1u, kPlayer, OwnerClass::Player));
        track.creates.push_back(Packet(2, 12977, 0x8000020031c5e0ull, 0x318A2A1u, kPlayer, OwnerClass::Player));
        track.nextIndex = 3;
        owner::Cover cover;
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 12900, nullptr);
        Check(player.Stats().complement == 2, "melee: both hit packets are complements");
        FakeEngine engine;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> pending;
        for (std::uint32_t tick = 0; tick <= 40; ++tick)
        {
            const std::uint32_t now = 12900 + (tick * 50) / 3;
            for (const auto& made : pending)
                engine.own.Add(kPlayer, owner::SoundKey(made.second), made.first);
            pending.clear();
            player.Tick(now, true, engine);
            if (now >= 12977 && now < 12977 + 17) // the melee track plays after the generic player
                pending.push_back({now, 0x8000010031c5e0ull});
        }
        Check(!engine.Made(0x8000010031c5e0ull) && engine.Made(0x8000020031c5e0ull),
            "melee: the sound the call made is not doubled, the other plays from the recording");
        Check(player.Stats().complementMeleePlayed == 1, "melee: one melee complement played");
    }

    // The ini's taker for a family keeps it (no complement).
    void TestIniKeepsTaker()
    {
        const Track track = ContactTrack();
        const owner::FootWindows feet = ContactFeet();
        owner::Cover cover;
        owner::Overrides overrides = PacketsOverrides();
        overrides.set[static_cast<std::uint8_t>(owner::Family::Footstep)] = true;
        overrides.taker[static_cast<std::uint8_t>(owner::Family::Footstep)] = owner::Taker::Specific;
        Player player;
        player.Begin(track, PacketsConfig(), cover, overrides, 500, &feet);
        Check(player.Stats().complement == 0, "SoundGenericTake=footstep:specific keeps the steps with their track");
        player::Config legacy;
        legacy.packets = false;
        Player old;
        old.Begin(track, legacy, cover, owner::Overrides(), 500, &feet);
        Check(old.Stats().complement == 0, "legacy mode: no complement");
    }

    void TestMeleeCheck()
    {
        Check(mc::SoundCount("4|0x318E9F1/0x318A2A1/0x0") == 4 && mc::SoundCount("0") == 0 &&
                  mc::SoundCount("observer_off") == -1 && mc::SoundCount("none") == -1,
            "melee check: the call's sound count from the log text");
        Check(mc::Judge(true, true, 4) == mc::Outcome::Played && mc::Judge(true, true, 0) == mc::Outcome::Silent &&
                  mc::Judge(true, true, -1) == mc::Outcome::Played && mc::Judge(true, false, 0) == mc::Outcome::Refused &&
                  mc::Judge(false, false, -1) == mc::Outcome::NoPuppet,
            "melee check: played, silent, refused, no puppet");
        // Six hits played with 3 or 4 sounds each.
        mc::Tally played;
        played.Reset();
        const std::uint32_t player1159 = (1u << 16) | 170u;
        const std::uint32_t times[6] = {12977, 12983, 37948, 37954, 38150, 38150};
        for (const std::uint32_t t : times)
            played.Note(player1159, t, 0x7d365047u, mc::Outcome::Played, 4, "lookup:played");
        Check(played.Due() == 6 && played.Mute() == 0, "1159: six hits with sound");
        Check(played.Findings(player1159).empty(), "1159: no finding");
        const std::string section = played.Section(6, 0);
        Check(Has(section, "Recorded hits 6. Due in this playback 6: with sound 6, silent (the call made no sound) 0"),
            "1159: the section's count");
        Check(!Has(section, "SILENT ") && !Has(section, "\xE2\x80\x94") && !Has(section, "\xE2\x80\x93"),
            "1159: no row, and no dashes in the report");
        // A silent and a refused hit.
        mc::Tally mute;
        mute.Reset();
        mute.Note(player1159, 12977, 0x7d365047u, mc::Outcome::Silent, 0, "lookup:played");
        mute.Note((13u << 16) | 182u, 12983, 0x4b3172b7u, mc::Outcome::Refused, -1, "melee_audio_not_ready");
        Check(mute.Mute() == 2 && mute.Count(mc::Outcome::Silent) == 1, "mute: two hits without sound");
        const std::string text = mute.Section(2, 1);
        Check(Has(text, "SILENT   owner 1:170  at 13.0 s  move 0x7d365047  sounds 0") &&
                  Has(text, "REFUSED  owner 13:182") && Has(text, "played from the recording by the generic player: 1"),
            "mute: the rows and the complement line");
        const std::vector<replay::census::Finding> findings = mute.Findings(player1159);
        Check(findings.size() == 2 && findings[0].cls == OwnerClass::Player && findings[1].cls == OwnerClass::Ped &&
                  findings[0].kind == replay::census::FindingKind::Missing && findings[0].spans.size() == 1 &&
                  findings[0].spans[0].first == 12977,
            "mute: one MISSING finding per hit, for the notice at Stop");
        mc::Row row = mute.Rows()[0];
        Check(Has(mc::Tally::FlagLine(row), "MELEE_SOUND_CHECK_FLAG,kind=silent,ped=1:170,hash=0x7d365047,event_ms=12977,sounds=0,"),
            "mute: the flag line");
        Check(Has(mute.TotalFields(), ",due=2,with_sound=0,silent=1,call_refused=1,no_puppet=0"), "mute: the total's fields");
        // No hit was recorded (the melee entry was never called).
        mc::Tally none;
        none.Reset();
        Check(Has(none.Section(0, 0), "No melee hit in the recording"), "1160: the report says no hit was recorded");
    }
}

int RunWeaponButtTests()
{
    TestComplementFamily();
    TestContact1160();
    TestComplementWaitsForTheCall();
    TestComplementLeadAndJump();
    TestMeleeComplement();
    TestIniKeepsTaker();
    TestMeleeCheck();
    return g_weaponButtFailures;
}
