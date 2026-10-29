// The engine sound census, pure part (sound_census_core.h): the
// init params fields, the placement class, the owner table, the clip clock
// between two ticks and the SOUND_CENSUS line a comparing tool parses.
#include "sound_census_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    using namespace replay::census;

    int g_censusFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_censusFailures;
    }

    bool Has(const std::string& line, const char* field)
    {
        return line.find(field) != std::string::npos;
    }

    void TestParams()
    {
        std::uint8_t params[kParamsBytes] = {};
        const float position[3] = {1.0f, 2.0f, 3.0f};
        const float volume = -6.0f;
        const std::int16_t pitch = 120;
        const std::int16_t pan = -1;
        const std::uint64_t tracker = 0x7FF612340450ull;
        const std::uint64_t category = 0x7FF6AA000040ull;
        std::memcpy(params + kParamsPosition, position, sizeof(position));
        std::memcpy(params + kParamsVolume, &volume, 4);
        std::memcpy(params + kParamsPitch, &pitch, 2);
        std::memcpy(params + kParamsPan, &pan, 2);
        std::memcpy(params + kParamsTracker, &tracker, 8);
        std::memcpy(params + kParamsCategory, &category, 8);
        params[kParamsFlags] = 0x20;
        Row row;
        ReadParams(row, params);
        Check(row.hasParams && row.volume == -6.0f && row.pitch == 120 && row.pan == -1, "params: value fields");
        Check(row.tracker == tracker && row.category == category && row.flags == 0x20, "params: pointers and flags");
        Check(row.position[2] == 3.0f, "params: position");
        Check(Classify(row) == Placement::Tracker, "placement: a tracker wins over a position");
        row.tracker = 0;
        Check(Classify(row) == Placement::Position, "placement: a position without tracker is 3D fixed");
        row.position[0] = row.position[1] = row.position[2] = 0.0f;
        Check(Classify(row) == Placement::Entity, "placement: no position, no tracker, pan -1");
        row.pan = 90;
        Check(Classify(row) == Placement::Panned, "placement: a pan is 2D");
        Row none;
        ReadParams(none, nullptr);
        Check(!none.hasParams && Classify(none) == Placement::NoParams, "placement: no params");
    }

    void TestOwners()
    {
        OwnerTable table;
        Owner ped;
        ped.kind = OwnerKind::Recorded;
        ped.id = (0u << 16) | 507u;
        ped.player = true;
        ped.lo = 0x1000000;
        ped.hi = ped.lo + 0x4820; // audio holder span
        table.audio.push_back(ped);
        Owner weapon = ped;
        weapon.id = (61u << 16) | 572u;
        weapon.player = false;
        weapon.lo = 0x1002000; // weapon audio inside the ped's span
        weapon.hi = weapon.lo + 0x100;
        table.audio.push_back(weapon);
        Owner object = weapon;
        object.id = (70u << 16) | 1u;
        object.lo = 0x5000000;
        object.hi = object.lo + 0x1000;
        table.tracker.push_back(object);

        Match match = Match::None;
        const Owner* found = table.Find(0x1000100, 0, match);
        Check(found && found->id == ped.id && match == Match::Audio, "owners: audio entity in the ped's holder");
        found = table.Find(0x1002010, 0, match);
        Check(found && found->id == weapon.id, "owners: the narrowest range wins (weapon inside ped span)");
        found = table.Find(0x1000100, 0x5000450, match);
        Check(found && found->id == object.id && match == Match::Tracker, "owners: the tracker wins over the audio");
        found = table.Find(0x9000000, 0x9000000, match);
        Check(!found && match == Match::None, "owners: outside every range");
        found = table.Find(0x1000000 + 0x4820, 0, match);
        Check(!found, "owners: the span end is exclusive (next ped's block)");
        Check(PackedIdText(weapon.id) == "61:572", "owners: id text as IdText");
        // A weapon's own audio names the weapon even when the
        // params track its ped (the replayed SND kind 4).
        Owner carried = weapon;
        carried.id = (64u << 16) | 65u;
        carried.base = AudioBase::WeaponAudio;
        carried.lo = 0x6000000; // CWeapon; its audio at +0x100
        carried.hi = carried.lo + 0x200;
        table.audio.push_back(carried);
        Owner pedTracker = ped;
        pedTracker.lo = 0x7000000;
        pedTracker.hi = pedTracker.lo + 0x10;
        table.tracker.push_back(pedTracker);
        found = table.Find(0x6000100, 0x7000000, match);
        Check(found && found->id == carried.id && match == Match::Audio, "owners: the weapon audio wins over its ped's tracker");
        found = table.Find(0x1000100, 0x7000000, match);
        Check(found && found->id == ped.id && match == Match::Tracker, "owners: other audio keeps the tracker rule");
        Check(BaseSpan(AudioBase::WeaponAudio) == 0x200, "owners: the weapon base spans the audio at +0x100");
    }

    void TestClock()
    {
        Check(ClipMsAt(1000000, 100.0, 1016000, 116.0, 1008000) == 108.0, "clock: halfway between ticks");
        Check(ClipMsAt(1000000, 100.0, 1016000, 116.0, 900000) == 100.0, "clock: older than the previous tick");
        Check(ClipMsAt(1000000, 100.0, 1016000, 116.0, 1020000) == 116.0, "clock: newer than this tick");
        Check(ClipMsAt(1000000, 250.0, 1500000, 250.0, 1200000) == 250.0, "clock: paused, the frozen time");
        Check(ClipMsAt(1000000, 0.0, 1000000, 40.0, 1000000) == 40.0, "clock: first tick of a phase");
    }

    void TestLine()
    {
        Row row;
        row.timeUs = 123456;
        row.core = 1;
        row.ref = 0x800000001234ull;
        row.audio = 0x1000100;
        row.hasParams = true;
        row.volume = -3.5f;
        row.pan = -1;
        row.tracker = 0x5000450;
        row.sound = 0xABC;
        row.callers[0] = 0x2516888;
        row.callers[2] = 0x2F2A3F6;
        Owner owner;
        owner.kind = OwnerKind::Puppet;
        owner.id = (3u << 16) | 9u;
        owner.player = true;
        LineContext context;
        context.phase = "play";
        context.clipMs = 1234.5;
        context.normal = true;
        context.owner = &owner;
        context.match = Match::Tracker;
        const std::string line = FormatRow(row, context);
        Check(line.rfind("SOUND_CENSUS,ph=play,t=1234.5,np=1,", 0) == 0, "line: prefix, phase, clip time");
        Check(Has(line, ",core=b,") && Has(line, ",ref=0x800000001234,") && Has(line, ",ok=1,"), "line: core, ref, ok");
        Check(Has(line, ",own=pup,id=3:9,pl=1,by=tr,") && Has(line, ",d=3t,"), "line: owner and placement");
        Check(Has(line, ",vol=-3.50,") && Has(line, ",c=2516888;0;2f2a3f6;0"), "line: volume and callers");
        context.owner = nullptr;
        context.match = Match::None;
        row.sound = 0;
        const std::string orphan = FormatRow(row, context);
        Check(Has(orphan, ",ok=0,own=none,id=-,pl=0,by=-,"), "line: refused, no owner");
    }
}

int RunSoundCensusTests()
{
    TestParams();
    TestOwners();
    TestClock();
    TestLine();
    if (g_censusFailures == 0)
        std::printf("Sound census tests passed.\n");
    return g_censusFailures;
}
