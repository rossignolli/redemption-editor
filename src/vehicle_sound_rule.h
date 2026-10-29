#pragma once

// The vehicle and train car sounds of the generic sound player, pure part
// (unit tested in ReplayCoreTests: tests/vehicle_sound_tests.cpp).
//
// A vehicle's audio entity ([vehicle + 0x900]) creates at the creators
// directly (e.g. sound 0x1056365 by creator B 0x2516900 through the audio
// entity wrapper 0x25167D0, return 0x25167F1, every 33 to 50 ms for a
// rolling wagon), past every port the ped/weapon block watches
// (soundowner::PortsBlockClass). In packets mode the generic player replays
// the recorded packets and the puppet vehicle's own audio makes the same
// sounds at the same call site, so they double and repeat.
//
// The recording's packets own a vehicle's recorded sound: the puppet's own
// vehicle audio alone measures about 9 to 11 dB under the recording, the
// packets on the puppet's audio entity about 3 dB under; the own copy adds
// at most 0.8 dB and is what repeats. So the peds' rule applies to vehicles
// and train cars: an own create of a sound whose recorded packet is alive
// then (Recorded) or of another set at that entry (Alternative) is refused;
// the rest (Passed: the recording never had it then) is left to the puppet.
// The refusal is the one the creator itself makes when its slot pool is
// full (return 0, the out slot untouched; the wrapper 0x25167D0 then zeroes
// it), behind the ini key SoundGenericVehicleBlock (default 1) because it
// changes what a creator detour does.
//
// Trains: the train cars' own vehicle audio makes nothing in a replay, so
// the packets are the train's whole sound. The per-owner slot cap (6: a ped
// controller's foley layers) would steal the cars' sounds and the 120 m
// limit would refuse the locomotive; a wagon can have 13 recorded sounds
// alive at once. Vehicles and train cars get their own cap, and train cars
// their own audible distance (LimitsFor).

#include "sound_owner_table.h"
#include "sound_slot_policy.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace replay::vehiclesound
{
    inline bool IsVehicleClass(census::OwnerClass cls)
    {
        return cls == census::OwnerClass::Vehicle || cls == census::OwnerClass::Train;
    }

    // ------------------------------------------------------------ slots

    constexpr std::uint32_t kVehicleOwnerCap = 16; // a wagon can have 13 recorded sounds alive at once
    constexpr float kVehicleMaxDistanceM = soundslots::kMaxDistanceM;
    constexpr float kTrainMaxDistanceM = 400.0f; // a consist of 16 cars is about 300 m long

    struct SlotLimits
    {
        std::uint32_t cap = soundslots::kPerOwnerCap;
        float maxDistanceM = soundslots::kMaxDistanceM;
    };

    // The cap and the audible distance of a packet: a world key keeps the
    // world cap (SoundGenericWorldCapCount) and the common distance.
    inline SlotLimits LimitsFor(census::OwnerClass cls, bool worldKey, std::uint32_t worldCap)
    {
        SlotLimits limits;
        if (worldKey)
        {
            limits.cap = worldCap;
            return limits;
        }
        if (cls == census::OwnerClass::Train)
        {
            limits.cap = kVehicleOwnerCap;
            limits.maxDistanceM = kTrainMaxDistanceM;
        }
        else if (cls == census::OwnerClass::Vehicle)
        {
            limits.cap = kVehicleOwnerCap;
            limits.maxDistanceM = kVehicleMaxDistanceM;
        }
        return limits;
    }

    // ------------------------------------------------------------ own creates

    // A create of the engine's own on a puppet vehicle's or train car's
    // audio entity: refused when the vehicle block is on (ini), packets mode
    // blocks the puppets' own sounds, the replay did not make it (the
    // generic player's census scope, a replay emit scope), it has a
    // persistent reference (queueBirth false: the game's queue drains a
    // request with none, the generic player's mode c births among them; the
    // vehicle's own one-shots pass a local slot), the packets' lives are
    // published, and the recording plays it then (Recorded, Alternative;
    // everything with SoundGenericBlock=all).
    inline bool RefuseVehicleOwnCreate(bool gate, bool packetsBlock, bool onPuppetVehicle, bool replayMade, bool queueBirth,
        bool hasLives, soundowner::LifeVerdict verdict, bool blockAll)
    {
        if (!gate || !packetsBlock || !onPuppetVehicle || replayMade || queueBirth || !hasLives)
            return false;
        return blockAll || verdict == soundowner::LifeVerdict::Recorded || verdict == soundowner::LifeVerdict::Alternative;
    }

    // Whether the generic player's packets of this owner class start at
    // their time (no anti-double hold): the owner's own copies are refused.
    inline bool OwnCopiesRefused(census::OwnerClass cls, bool vehicleBlock)
    {
        return soundowner::PortsBlockClass(cls) || (vehicleBlock && IsVehicleClass(cls));
    }

    // ------------------------------------------------------------ per vehicle

    // One vehicle or train car of the clip in a playback: its generic
    // packets by outcome, the doubles (another source made the same sound
    // within the detector's tolerance: `doubles` when it came first,
    // `lateDoubles` when it came after our start), its own creates at the
    // creators (refused, passed), the sounds the slots took from it and the
    // packets the distance refused.
    struct VehicleRow
    {
        std::uint32_t owner = census::kNoOwner;
        census::OwnerClass cls = census::OwnerClass::Vehicle;
        std::uint32_t packets = 0;
        std::uint32_t played = 0;
        std::uint32_t yielded = 0;
        std::uint32_t missed = 0;
        std::uint32_t refused = 0;
        std::uint32_t doubles = 0;
        std::uint32_t lateDoubles = 0;
        std::uint32_t ownRefused = 0;
        std::uint32_t ownPassed = 0;
        std::uint32_t stealsLost = 0;
        std::uint32_t tooFar = 0;
        std::uint32_t Doubled() const { return doubles + lateDoubles; }
    };

    // A vehicle whose doubles reach kDoubledMin and kDoubledPct of what it
    // played is reported as doubled.
    constexpr std::uint32_t kDoubledMin = 3;
    constexpr std::uint32_t kDoubledPct = 5;
    inline bool Doubled(std::uint32_t doubled, std::uint32_t played)
    {
        return doubled >= kDoubledMin && static_cast<std::uint64_t>(doubled) * 100 >= static_cast<std::uint64_t>(played) * kDoubledPct;
    }

    class VehicleTally
    {
    public:
        void Clear() { m_rows.clear(); }
        VehicleRow& At(std::uint32_t owner, census::OwnerClass cls)
        {
            VehicleRow& row = m_rows[owner];
            if (row.owner != owner)
            {
                row.owner = owner;
                row.cls = cls;
            }
            return row;
        }
        // One generic packet of the owner and how its last pass ended.
        void NotePlan(std::uint32_t owner, census::OwnerClass cls, bool played, bool yielded, bool missed, bool refused, bool doubled)
        {
            if (!IsVehicleClass(cls))
                return;
            VehicleRow& row = At(owner, cls);
            ++row.packets;
            row.played += played ? 1 : 0;
            row.yielded += yielded ? 1 : 0;
            row.missed += missed ? 1 : 0;
            row.refused += refused ? 1 : 0;
            row.doubles += doubled ? 1 : 0;
        }
        void NoteLateDouble(std::uint32_t owner, census::OwnerClass cls)
        {
            if (IsVehicleClass(cls))
                ++At(owner, cls).lateDoubles;
        }
        void NoteOwn(std::uint32_t owner, census::OwnerClass cls, bool refused)
        {
            if (!IsVehicleClass(cls))
                return;
            VehicleRow& row = At(owner, cls);
            ++(refused ? row.ownRefused : row.ownPassed);
        }
        void NoteStealLost(std::uint32_t owner, census::OwnerClass cls)
        {
            if (IsVehicleClass(cls))
                ++At(owner, cls).stealsLost;
        }
        void NoteTooFar(std::uint32_t owner, census::OwnerClass cls)
        {
            if (IsVehicleClass(cls))
                ++At(owner, cls).tooFar;
        }
        // Only the generic packets' part (a new pass recounts it).
        void ClearPlans()
        {
            for (auto& entry : m_rows)
            {
                VehicleRow& row = entry.second;
                row.packets = row.played = row.yielded = row.missed = row.refused = row.doubles = 0;
            }
        }
        const std::map<std::uint32_t, VehicleRow>& Rows() const { return m_rows; }

    private:
        std::map<std::uint32_t, VehicleRow> m_rows;
    };

    inline std::string FormatVehicleLine(const VehicleRow& r)
    {
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "SOUND_GENERIC_VEHICLE,owner=%u:%u,cls=%s,packets=%u,played=%u,yielded=%u,missed=%u,refused=%u,doubles=%u,"
            "late_doubles=%u,own_refused=%u,own_passed=%u,steals_lost=%u,too_far=%u,verdict=%s",
            r.owner >> 16, r.owner & 0xFFFFu, census::ClassToken(r.cls), r.packets, r.played, r.yielded, r.missed, r.refused,
            r.doubles, r.lateDoubles, r.ownRefused, r.ownPassed, r.stealsLost, r.tooFar,
            Doubled(r.Doubled(), r.played) ? "doubled" : "ok");
        return line;
    }

    // The total line: vehicles and train cars, their sums, how many came out
    // doubled (the automatic check: verdict=doubled).
    inline std::string FormatVehicleTotal(const VehicleTally& tally, bool vehicleBlock)
    {
        std::uint32_t vehicles = 0, trains = 0, packets = 0, played = 0, yielded = 0, missed = 0, doubles = 0, late = 0;
        std::uint32_t ownRefused = 0, ownPassed = 0, steals = 0, tooFar = 0, doubled = 0;
        for (const auto& entry : tally.Rows())
        {
            const VehicleRow& r = entry.second;
            ++(r.cls == census::OwnerClass::Train ? trains : vehicles);
            packets += r.packets;
            played += r.played;
            yielded += r.yielded;
            missed += r.missed;
            doubles += r.doubles;
            late += r.lateDoubles;
            ownRefused += r.ownRefused;
            ownPassed += r.ownPassed;
            steals += r.stealsLost;
            tooFar += r.tooFar;
            doubled += Doubled(r.Doubled(), r.played) ? 1 : 0;
        }
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "SOUND_GENERIC_VEHICLE_TOTAL,vehicle_block=%d,vehicles=%u,train_cars=%u,packets=%u,played=%u,yielded=%u,missed=%u,"
            "doubles=%u,late_doubles=%u,own_refused=%u,own_passed=%u,steals_lost=%u,too_far=%u,doubled_vehicles=%u,verdict=%s",
            vehicleBlock ? 1 : 0, vehicles, trains, packets, played, yielded, missed, doubles, late, ownRefused, ownPassed, steals,
            tooFar, doubled, doubled ? "doubled" : "ok");
        return line;
    }

    // The rows worth a line (any packet, own create or double), most packets
    // first, at most `limit`.
    inline std::vector<std::string> FormatVehicleLines(const VehicleTally& tally, std::size_t limit)
    {
        std::vector<const VehicleRow*> rows;
        for (const auto& entry : tally.Rows())
            rows.push_back(&entry.second);
        std::stable_sort(rows.begin(), rows.end(), [](const VehicleRow* a, const VehicleRow* b) {
            return a->packets + a->ownRefused + a->ownPassed > b->packets + b->ownRefused + b->ownPassed;
        });
        std::vector<std::string> out;
        for (const VehicleRow* row : rows)
        {
            if (out.size() >= limit)
                break;
            out.push_back(FormatVehicleLine(*row));
        }
        return out;
    }

    // ------------------------------------------------------------ per class

    // The slot policy's outcomes by owner class: the sounds placed, the
    // owner (or world) cap steals, the farther steals by the victim's class
    // and by the taker's, and the distance refusals (attempts, packets, the
    // farthest).
    struct ClassSlots
    {
        std::uint32_t placed = 0;
        std::uint32_t capSteals = 0;
        std::uint32_t fartherVictims = 0;
        std::uint32_t fartherTakers = 0;
        std::uint32_t tooFar = 0;
        std::uint32_t tooFarPackets = 0;
        float tooFarMaxM = 0.0f;
        bool Any() const { return placed || capSteals || fartherVictims || fartherTakers || tooFar; }
    };

    class ClassSlotTally
    {
    public:
        void Clear()
        {
            for (ClassSlots& c : m_classes)
                c = ClassSlots{};
        }
        ClassSlots& Of(census::OwnerClass cls)
        {
            const std::uint8_t n = static_cast<std::uint8_t>(cls);
            return m_classes[n < census::kOwnerClassCount ? n : 0];
        }
        const ClassSlots& Of(census::OwnerClass cls) const
        {
            const std::uint8_t n = static_cast<std::uint8_t>(cls);
            return m_classes[n < census::kOwnerClassCount ? n : 0];
        }
        void NoteTooFar(census::OwnerClass cls, float distance, bool newPacket)
        {
            ClassSlots& c = Of(cls);
            ++c.tooFar;
            c.tooFarPackets += newPacket ? 1 : 0;
            if (distance == distance && distance > c.tooFarMaxM)
                c.tooFarMaxM = distance;
        }

    private:
        ClassSlots m_classes[census::kOwnerClassCount];
    };

    // One line per class with any outcome: SOUND_GENERIC_CLASS_SLOTS.
    inline std::vector<std::string> FormatClassSlotLines(const ClassSlotTally& tally, std::uint32_t worldCap)
    {
        std::vector<std::string> out;
        for (std::uint8_t n = 0; n < census::kOwnerClassCount; ++n)
        {
            const census::OwnerClass cls = static_cast<census::OwnerClass>(n);
            const ClassSlots& c = tally.Of(cls);
            if (!c.Any())
                continue;
            const SlotLimits limits = LimitsFor(cls, cls == census::OwnerClass::World, worldCap);
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "SOUND_GENERIC_CLASS_SLOTS,class=%s,cap=%u,max_m=%d,placed=%u,cap_steals=%u,farther_victims=%u,farther_takers=%u,"
                "too_far=%u,too_far_packets=%u,too_far_max_m=%d",
                census::ClassToken(cls), limits.cap, static_cast<int>(limits.maxDistanceM), c.placed, c.capSteals,
                c.fartherVictims, c.fartherTakers, c.tooFar, c.tooFarPackets, static_cast<int>(c.tooFarMaxM));
            out.push_back(line);
        }
        return out;
    }
}
