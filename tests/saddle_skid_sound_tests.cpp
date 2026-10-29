// The packets model (SoundGenericMode=packets) and the sound check's notice,
// pure parts:
//   the mode key (sound_generic_core.h PacketsModeOfText);
//   the owner table with packets (sound_owner_table.h DefaultTaker, TakerOf);
//   the rows a packet carries (sound_owner_table.h PacketIndex): 036's skid,
//   038's rows;
//   the port rule (puppet_audio_rule.h BlockPuppetCreate): 038's line;
//   the blocked creates in the report (sound_generic_player_core.h
//   BlockedTally, FormatBlockedSection): 038's extra;
//   the ESND rows given to the generic player (entity_sound_tally.h);
//   the on-screen notice (sound_check_core.h FormatNotice): 038's report.
#include "entity_sound_tally.h"
#include "puppet_audio_rule.h"
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
    int g_failuresSaddle = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failuresSaddle;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    namespace owner = replay::soundowner;
    using owner::Family;
    using owner::Taker;

    constexpr std::uint32_t kHorse036 = 0x00000001u;  // 0:1
    constexpr std::uint32_t kJohn038 = 0x000102C7u;   // 1:711
    constexpr std::uint32_t kHorse038 = 0x000202C8u;  // 2:712
    constexpr std::uint32_t kAnimal038 = 0x0026030Fu; // 38:783

    void TestModeKey()
    {
        using replay::soundgeneric::PacketsModeOfText;
        Check(PacketsModeOfText("packets") && PacketsModeOfText("") && PacketsModeOfText(nullptr) && PacketsModeOfText("Packets "),
            "mode: packets, empty and unknown are packets");
        Check(!PacketsModeOfText("legacy") && !PacketsModeOfText(" LEGACY\r\n"), "mode: legacy in any case");
        replay::soundgeneric::Config config;
        Check(config.packets, "mode: packets is the default");
    }

    void TestTakersWithPackets()
    {
        // Recorded means the generic player, except the tracks that replay
        // the recorded event through the engine.
        const Family generic[] = {Family::World, Family::Owned, Family::Movement, Family::Collision, Family::EntitySound,
            Family::SoundRequest, Family::Captured};
        bool all = true;
        for (const Family family : generic)
            all = all && owner::DefaultTaker(family, true) == Taker::Generic;
        Check(all, "packets: world, owned, movement, collision, entity sound, sound request and capture go to the generic player");
        const Family kept[] = {Family::Speech, Family::Footstep, Family::HorseStep, Family::Melee, Family::Gunshot, Family::Explosion};
        all = true;
        for (const Family family : kept)
            all = all && owner::DefaultTaker(family, true) == Taker::Specific;
        Check(all, "packets: speech, steps, wet hoof, melee, shots and explosions keep their tracks");
        Check(owner::DefaultTaker(Family::EntitySound) == Taker::Specific && owner::DefaultTaker(Family::Movement) == Taker::Puppet,
            "legacy: the old table");
        owner::Overrides overrides = owner::ParseOverrides("footstep:generic");
        overrides.packets = true;
        Check(owner::TakerOf(Family::Footstep, overrides) == Taker::Generic &&
                owner::TakerOf(Family::EntitySound, overrides) == Taker::Generic &&
                owner::TakerOf(Family::Gunshot, overrides) == Taker::Specific,
            "packets: the ini still overrides a family");
        Check(owner::FormatTable(overrides).find("footstep:generic(ini)") != std::string::npos, "packets: the table marks the override");
    }

    void TestPacketIndex()
    {
        owner::PacketIndex index;
        // 036: the skid's packet (id set + variant, raw ref with bit 55).
        index.Add(kHorse036, 0xe002f4361ull, 0x0080000e002f4361ull, 45541);
        // 038: a sound of no stable id (kind none: id 0, the raw ref).
        index.Add(kJohn038, 0, 0x000fffff000da025ull, 5703);
        index.Add(replay::census::kNoOwner, 0x30012345ull, 0x0080000300012345ull, 100); // a world sound: not indexed
        index.Seal();
        Check(index.Size() == 2, "index: owned packets only");
        Check(index.Covers(kHorse036, 0x0080000e002f4361ull, 45541), "index: the ESND row of the skid (same ms) is the packet's");
        Check(index.Covers(kHorse036, 0x0080000e002f4361ull, 45541 + 400) && !index.Covers(kHorse036, 0x0080000e002f4361ull, 45541 + 401),
            "index: 400 ms of tolerance");
        Check(!index.Covers(kHorse036, 0x0080000f002f4361ull, 45541), "index: another variant of the set is another sound");
        Check(!index.Covers(kJohn038, 0x0080000e002f4361ull, 45541), "index: another owner");
        Check(index.Covers(kJohn038, 0x000fffff000da025ull, 5710), "index: a raw ref without stable id matches by the raw ref");
        Check(!index.Covers(replay::census::kNoOwner, 0x0080000300012345ull, 100), "index: no owner, no row to leave");
        owner::PacketIndex unsealed;
        unsealed.Add(kHorse036, 0xe002f4361ull, 0x0080000e002f4361ull, 45541);
        Check(!unsealed.Covers(kHorse036, 0x0080000e002f4361ull, 45541), "index: not sealed, nothing covered");
    }

    void TestPortRule()
    {
        using replay::puppetaudio::BlockPuppetCreate;
        // 038: the line's own sound at 0x2FA0DCC, while the line played.
        Check(!BlockPuppetCreate(true, true, false, false, 0x2FA0DCCu) && !BlockPuppetCreate(true, true, false, false, 0x2FA10CFu),
            "port: the speech's sound controller is never refused");
        // 038's extra: John's own loop through the persistent wrapper.
        Check(BlockPuppetCreate(true, true, false, false, 0x2588C2Fu), "port: a puppet's own create is refused");
        Check(!BlockPuppetCreate(true, true, true, false, 0x2588C2Fu), "port: the replay's own (ReplayEmitScope) passes");
        Check(!BlockPuppetCreate(true, true, false, true, 0x318E828u), "port: a replayed footstep or melee call passes");
        Check(!BlockPuppetCreate(true, false, false, false, 0x2588C2Fu), "port: an audio entity of no puppet passes");
        Check(!BlockPuppetCreate(false, true, false, false, 0x2588C2Fu), "port: legacy refuses nothing new");
        using replay::census::OwnerClass;
        Check(owner::PortsBlockClass(OwnerClass::Player) && owner::PortsBlockClass(OwnerClass::Ped) &&
                owner::PortsBlockClass(OwnerClass::Animal) && owner::PortsBlockClass(OwnerClass::Horse),
            "port: peds, the player, animals and horses (the audio holders) are refused at the ports");
        Check(!owner::PortsBlockClass(OwnerClass::Vehicle) && !owner::PortsBlockClass(OwnerClass::Object) &&
                !owner::PortsBlockClass(OwnerClass::Weapon) && !owner::PortsBlockClass(OwnerClass::Train) &&
                !owner::PortsBlockClass(OwnerClass::World),
            "port: vehicles, objects, weapons and trains are not (their packets keep the hold and its anti-double wait)");
    }

    void TestBlockedSection()
    {
        // The verdicts of the ports (skid_hoof_sound_tests.cpp has 036's);
        // here 038's extra: John's loop 0xda07f, never recorded.
        using namespace replay::soundgeneric::player;
        using owner::LifeVerdict;
        BlockedTally tally;
        for (std::uint32_t n = 0; n < 7; ++n) // 038: 0x800003000da07f, 7 times, no packet: passed
            tally.Note(kJohn038, 0x00800003000da07full, 0x2588C2F, 5, 6400 + 300 * n, LifeVerdict::Passed, false);
        tally.Note(kHorse038, 0x00800004004366e8ull, 0x2588C2F, 5, 2100, LifeVerdict::Recorded, true);
        tally.Note(replay::census::kNoOwner, 0x00800005000da07full, 0x318CB59, 3, 1000, LifeVerdict::Passed, true);
        tally.Note(kAnimal038, 0x00800003000e195full, 0x310E700, 4, 3000, LifeVerdict::Recorded, true);
        Check(tally.Total() == 10 && tally.WithPacket() == 2 && tally.WithoutPacket() == 7 && tally.Hidden() == 1,
            "blocked: totals");
        Check(tally.ByPort(3) == 1 && tally.ByPort(4) == 1 && tally.ByPort(5) == 8 && tally.ByPort(6) == 0, "blocked: by port");
        Check(tally.Refused() == 3 && tally.Passed() == 7 && tally.RunsWithoutPacket() == 7, "blocked: refused, passed, runs (300 ms apart)");
        const std::string section = FormatBlockedSection(tally, true);
        Check(Has(section, "== the puppets' own creates (SoundGenericMode=packets, SoundGenericBlock=packet) =="), "blocked: the header");
        Check(Has(section, "By port: S03 create-and-play 1 (the puppets' own one-shots, refused), S04 deferred request 1, S05 persistent by ref 8, S06 persistent by hash 0; a vehicle's or train car's own create at the creators: S01 0, S02 0 (SoundGenericVehicleBlock)."),
            "blocked: the ports line");
        const std::size_t noPacket = section.find("NO PACKET  owner 1:711  sound 0x3000da07f  attempts 7 in 7 runs, refused 0  (S05, ref 0x800003000da07f at 0x2588c2f)  6.4 s to 8.2 s");
        const std::size_t withPacket = section.find("recorded   owner 2:712");
        Check(noPacket != std::string::npos && withPacket != std::string::npos && noPacket < withPacket,
            "blocked: the rows without a packet first");
        Check(FormatBlockedSection(tally, false).empty(), "blocked: legacy, no section");
        Check(BlockedLogLine(tally) == "SOUND_GENERIC_BLOCKED_SUMMARY,block=packet,attempts=10,refused=3,passed=7,recorded=2,alternative=0,no_packet=7,no_packet_runs=7,hidden_player=1,s03=1,s04=1,s05=8,s06=0,step_unmade=0,unrecorded=0,s01=0,s02=0",
            "blocked: the log line");
    }

    void TestEsndToGeneric()
    {
        using namespace replay::esndtally;
        Tally tally;
        const FamilyKey skid = KeyOf(kHorse036, 0x2588c2f, 0x80000e002f4361ull);
        for (std::uint32_t n = 0; n < 69; ++n)
            tally.Note(skid, Outcome::ToGeneric, 45541 + 16 * n, true);
        const std::string section = FormatSection(tally);
        Check(Has(section, "Rows due 69 (69 in slow motion): played 0, left to the puppet 0, refused 0, too far 0, passed without a start 0, to the generic player 69;"),
            "esnd: the totals count the rows given to the generic player");
        Check(Has(section, "ok      owner 0:1  caller 0x2588c2f set 0x2f4361  due 69 (69 in slow motion), played 0, by the puppet 0, refused 0, too far 0, passed 0, to the generic player 69;"),
            "esnd: a family given to the generic player is not a problem");
        Check(LogLine(skid, tally.Families().at(skid)).find(",passed=0,to_generic=69,") != std::string::npos, "esnd: the log line");
    }

    replay::census::Finding Finding(replay::census::FindingKind kind, std::uint32_t ownerId, replay::census::OwnerClass cls,
        std::uint32_t fromMs)
    {
        replay::census::Finding f;
        f.kind = kind;
        f.owner = ownerId;
        f.cls = cls;
        if (fromMs != 0xFFFFFFFFu)
            f.spans.push_back({fromMs, fromMs + 250});
        return f;
    }

    void TestNotice()
    {
        using namespace replay::census;
        // 038's report: 5 missing (John 4 at 5.2 s, the animal 1 at 5.0 s), 1 extra (John, no span).
        DiffResult diff;
        diff.findings.push_back(Finding(FindingKind::Missing, kAnimal038, OwnerClass::Animal, 5000));
        for (int n = 0; n < 4; ++n)
            diff.findings.push_back(Finding(FindingKind::Missing, kJohn038, OwnerClass::Player, 5250 - 50 * n));
        diff.findings.push_back(Finding(FindingKind::Extra, kJohn038, OwnerClass::Player, 0xFFFFFFFFu));
        diff.findings.push_back(Finding(FindingKind::Variant, kJohn038, OwnerClass::Player, 1000));
        Check(FormatNotice(diff) ==
                "Sound check: 5 missing (player 1:711 at 0:05.1: 4; animal 38:783 at 0:05.0: 1), 1 extra (player 1:711: 1). Details in ReplayDebug.",
            "notice: 038's report");
        Check(FormatNotice(diff, 7).empty(), "notice: below the threshold, nothing");
        Check(FormatNotice(diff, 0).empty(), "notice: 0 turns it off");
        DiffResult many;
        many.findings.push_back(Finding(FindingKind::Fewer, 0x00030001u, OwnerClass::Horse, 61700));
        many.findings.push_back(Finding(FindingKind::Missing, 0x00040001u, OwnerClass::Ped, 62000));
        many.findings.push_back(Finding(FindingKind::Missing, 0x00050001u, OwnerClass::Ped, 63000));
        many.findings.push_back(Finding(FindingKind::More, 0x00030001u, OwnerClass::Horse, 70000));
        Check(FormatNotice(many) ==
                "Sound check: 3 missing (horse 3:1 at 1:01.7: 1; ped 4:1 at 1:02.0: 1; +1 more), 1 extra (horse 3:1 at 1:10.0: 1). Details in ReplayDebug.",
            "notice: the earliest owners first among equals, the rest counted; fewer and more count");
        DiffResult refusedOnly;
        for (int n = 0; n < 5; ++n)
            refusedOnly.findings.push_back(Finding(FindingKind::Refused, kJohn038, OwnerClass::Player, 1000));
        Check(FormatNotice(refusedOnly).empty(), "notice: refusals alone are not missing or extra");
    }
}

int RunSaddleSkidSoundTests()
{
    g_failuresSaddle = 0;
    TestModeKey();
    TestTakersWithPackets();
    TestPacketIndex();
    TestPortRule();
    TestBlockedSection();
    TestEsndToGeneric();
    TestNotice();
    if (g_failuresSaddle == 0)
        std::puts("Saddle and skid (packets model) tests passed.");
    return g_failuresSaddle;
}
