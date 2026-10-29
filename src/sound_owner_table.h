#pragma once

// The owner table of the generic sound player, pure part (unit tested in
// tests/sound_generic_player_tests.cpp).
//
// One rule for who plays a recorded sound in the replay, so nothing plays
// twice: the puppet by itself (the engine makes the sound when the puppet
// animates), the specific track that already carries it (speech, footstep,
// melee, shot, explosion, collision, entity sound, sound request, the census
// capture), or the generic player (the SGEN packets). Never two.
//
// A packet is given a family, in this order:
// 1. a specific track of the clip carries the same sound (same owner, same
//    sound, within kCoverToleranceMs: the Cover index built from the clip's
//    own tracks), so the family is that track's;
// 2. the sound set names it (the wet hoof layer set);
// 3. the family caller (the first return address outside the audio
//    entity's wrappers, CreatePacket::caller) falls in a function of a known
//    system (kCallerRanges: build 1491.50 function extents read from the
//    image's .pdata, of the entries and helpers the mod's own detours and
//    evidence logs already name);
// 4. the audio entity's base (the weapon's audio: a shot);
// 5. the owner class (no owner: world; else owned).
//
// Each family has a default taker and an ini override
// (SoundGenericTake=family:taker;family:taker, takers puppet, specific,
// generic). A family that already has a specific track keeps it; a family
// the puppet's own engine update makes (the movement foley) is the puppet's;
// the rest goes to the generic player. The generic player still yields at
// run time to a sound the puppet (or the live world, or another track) makes
// by itself around the packet's time (the anti-double rule of the capture,
// sound_generic_player_core.h), which is the "puppet" taker decided per
// sound instead of per family.
//
// SoundGenericMode=packets (the default; legacy keeps the table above): a
// replay entity sounds only what its recording carries, as a replay editor
// usually works. The engine's own sounds of the replay's puppets are refused
// at the creation ports (audio_observer.cpp: the persistent cores S05/S06 and
// the deferred request S04 join the create-and-play refusal of S03), so every
// SGEN packet with a recorded owner is the generic player's, with its
// recorded adjusts, stops and release, and the tracks that replay a sound by
// its ref (ESND, SNDS, CSND, GSND) leave a row to the generic player when a
// packet carries it (PacketIndex below). The exceptions keep their track
// (DefaultTaker with packets): speech (the recorder skips the line's sounds,
// the speech track asks the line again), the footstep and wet hoof steps (the
// footstep track replays the recorded foot event through the engine's own
// step: material, wetness, audio LOD), and melee, gunshot and explosion
// (their tracks replay the recorded event, whose sounds the engine makes
// inside the replay's call along with the effects; their packets would
// double them).
//
// A sound is its set and entry (SoundKey), not its set alone: a horse's
// surface sets are shared by the hoof steps (entries 5 and 6), a skid's
// grains (entry 0x0E) and its body (entry 0x2B). The step exception covers
// only the packets the step track remakes (FootWindows: near a recorded foot
// event of that owner); the rest of the foot system (slide, drag, braking)
// goes to the generic player.

#include "sound_check_core.h" // OwnerClass, AudioBase, kNoOwner, RefLow

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace replay::soundowner
{
    enum class Family : std::uint8_t
    {
        World = 0,       // no recorded owner
        Owned = 1,       // an owned sound no track and no known system names
        Speech = 2,      // the speech track (never in the packets: the recorder skips the speech's sounds)
        Footstep = 3,    // the foot audio helpers (ped and horse steps, their layers): the footstep track and the foot material
        HorseStep = 4,   // the wet hoof layer set: the step with the recorded wetness
        Movement = 5,    // the ped movement audio update: the puppet runs it itself
        Melee = 6,       // melee hits and body impacts (the melee track)
        Gunshot = 7,     // shots (the weapon's audio entity: the shot track)
        Explosion = 8,   // explosions (the explosion track)
        Collision = 9,   // collision sounds (CSND)
        EntitySound = 10, // entity sounds (ESND)
        SoundRequest = 11, // ped sound requests (SNDS)
        Captured = 12,   // the census capture (GSND)
    };
    constexpr std::uint8_t kFamilyCount = 13;

    enum class Taker : std::uint8_t
    {
        Generic = 0,  // the generic player plays it (yielding to a puppet's own at run time)
        Specific = 1, // the specific track plays it; the generic player skips it
        Puppet = 2,   // nobody replays it: the puppet (or the live world) makes it
    };
    constexpr std::uint8_t kTakerCount = 3;

    inline const char* FamilyName(Family family)
    {
        static const char* const kNames[kFamilyCount] = {"world", "owned", "speech", "footstep", "horse_step", "movement",
            "melee", "gunshot", "explosion", "collision", "entity_sound", "sound_request", "captured"};
        return static_cast<std::uint8_t>(family) < kFamilyCount ? kNames[static_cast<std::uint8_t>(family)] : "?";
    }

    inline const char* TakerName(Taker taker)
    {
        switch (taker)
        {
        case Taker::Generic: return "generic";
        case Taker::Specific: return "specific";
        case Taker::Puppet: return "puppet";
        }
        return "?";
    }

    inline bool FamilyOfName(const std::string& name, Family& out)
    {
        for (std::uint8_t n = 0; n < kFamilyCount; ++n)
        {
            if (name == FamilyName(static_cast<Family>(n)))
            {
                out = static_cast<Family>(n);
                return true;
            }
        }
        return false;
    }

    inline bool TakerOfName(const std::string& name, Taker& out)
    {
        for (std::uint8_t n = 0; n < kTakerCount; ++n)
        {
            if (name == TakerName(static_cast<Taker>(n)))
            {
                out = static_cast<Taker>(n);
                return true;
            }
        }
        return false;
    }

    // The default taker: a family with a specific track keeps it; the movement
    // foley is the puppet's own (its movement update runs on the puppet); the
    // rest goes to the generic player.
    inline Taker DefaultTaker(Family family, bool packets = false)
    {
        // SoundGenericMode=packets: recorded means the generic player, except the
        // families whose track replays the recorded event through the engine
        // (header comment).
        if (packets)
        {
            switch (family)
            {
            case Family::Speech:
            case Family::Footstep:
            case Family::HorseStep:
            case Family::Melee:
            case Family::Gunshot:
            case Family::Explosion:
                return Taker::Specific;
            default:
                return Taker::Generic;
            }
        }
        switch (family)
        {
        case Family::World:
        case Family::Owned:
            return Taker::Generic;
        case Family::Movement:
            return Taker::Puppet;
        case Family::Speech:
        case Family::Footstep:
        case Family::HorseStep:
        case Family::Melee:
        case Family::Gunshot:
        case Family::Explosion:
        case Family::Collision:
        case Family::EntitySound:
        case Family::SoundRequest:
        case Family::Captured:
            return Taker::Specific;
        }
        return Taker::Generic;
    }

    // ------------------------------------------------------------ callers

    // Function extents (build 1491.50, the image's .pdata) of the entries and
    // helpers that reach the create wrappers from a known system. The family
    // caller of a packet is a return address inside the function that
    // called the wrapper, so [begin, end) of that function is the test. The
    // shot, explosion and animation event entries reach the wrappers through
    // paths the offline walk did not close (virtual calls); those families
    // come from the clip's tracks (Cover), the audio base (the weapon's) and,
    // at run time, from the anti-double rule.
    struct CallerRange
    {
        std::uint32_t begin;
        std::uint32_t end;
        Family family;
    };

    constexpr CallerRange kCallerRanges[] = {
        {0x02588FE8u, 0x02589056u, Family::Collision}, // the collision entry (audio_observer.h: calls the by-ref core itself)
        {0x02FA0D88u, 0x02FA10A4u, Family::Speech},    // the line's sound creation
        {0x0318A28Cu, 0x0318A2BEu, Family::Melee},     // the melee hit entry T08 (0x318A2A1 inside)
        {0x0318EC48u, 0x0318F00Du, Family::Melee},     // the body impact entry T09 (0x318EEB1 inside)
        {0x0318CEB4u, 0x0318D0CBu, Family::Footstep},  // the step layers (foot_material.h: the wet layer, 0x318D0A0 inside)
        {0x0318D52Cu, 0x0318E095u, Family::Footstep},  // the step (horse_audio_evidence.h: 0x318DAFE, 0x318DED9, 0x318DBE8 inside)
        {0x0318E6E8u, 0x0318E84Eu, Family::Footstep},  // the ped step's main sound (0x318E828 inside)
        {0x0318E9C4u, 0x0318EB8Du, Family::Footstep},  // a foot audio helper (replay_events.cpp IsPoseV2FootstepHelperCaller)
        {0x0318FC04u, 0x0318FC60u, Family::Footstep},  // another sound of the step
        {0x0318C150u, 0x0318C4C4u, Family::Footstep},  // a foot audio helper (IsPoseV2FootstepHelperCaller)
        {0x03193D14u, 0x03193EB3u, Family::Movement},  // the ped movement audio update (audio_observer.cpp kMovementUpdateRva)
        {0x03193F20u, 0x031942CEu, Family::Movement},  // its step queue
    };
    constexpr std::size_t kCallerRangeCount = sizeof(kCallerRanges) / sizeof(kCallerRanges[0]);

    inline bool FamilyOfCaller(std::uint32_t caller, Family& out)
    {
        if (!caller)
            return false;
        for (const CallerRange& range : kCallerRanges)
        {
            if (caller >= range.begin && caller < range.end)
            {
                out = range.family;
                return true;
            }
        }
        return false;
    }

    // The horse's wet hoof layer set: the step with the recorded wetness plays it
    // on the puppet; the family keeps its own key so it can be handed to the
    // generic player by an override.
    constexpr std::uint32_t kWetHoofSet = 0x0029934Au;

    // ------------------------------------------------------------ sound key

    // The sound a ref names, for the cover and the anti-double rule. A ref of the
    // known form (set in the low dword, the set's entry in bits 32 to 39, the flag
    // bit 55; sound_generic_core.h StableIdOf) is its set and entry:
    // 0x80000e002f4361, 0x800005002f4361 and 0x80002b002f36cb are three sounds.
    // The flag does not count (0x1000491933 and 0x80001000491933 are one sound in
    // two banks). Any other ref (a hash, a raw form) is its low dword.
    constexpr std::uint64_t kSoundSetMask = 0xFFFFFFFFull;
    constexpr std::uint64_t kSoundEntryMask = 0xFF00000000ull;
    constexpr std::uint64_t kSoundFlagMask = 1ull << 55;

    inline std::uint64_t SoundKey(std::uint64_t ref)
    {
        const std::uint64_t known = kSoundSetMask | kSoundEntryMask | kSoundFlagMask;
        if ((ref & ~known) == 0 && (ref & kSoundSetMask) != 0)
            return ref & (kSoundSetMask | kSoundEntryMask);
        return ref & kSoundSetMask;
    }

    // A packet's sound: its stable id (set and entry, or the hash), else its raw ref.
    inline std::uint64_t PacketSoundKey(std::uint64_t stableId, std::uint64_t rawRef)
    {
        return SoundKey(stableId ? stableId : rawRef);
    }

    // The entry of a sound key of the known form (bits 32 to 39), -1 otherwise.
    inline int SoundEntry(std::uint64_t key)
    {
        const std::uint64_t known = kSoundSetMask | kSoundEntryMask | kSoundFlagMask;
        if ((key & ~known) != 0 || (key & kSoundSetMask) == 0 || (key & kSoundEntryMask) == 0)
            return -1;
        return static_cast<int>((key & kSoundEntryMask) >> 32);
    }

    // ------------------------------------------------------------ cover

    // What the clip's specific tracks carry: (owner, sound, time) rows. A packet
    // of the same owner and sound (SoundKey: set and entry) within the tolerance
    // is theirs.
    constexpr std::uint32_t kCoverToleranceMs = 400; // the capture's own (sound_capture.cpp)

    struct CoverRow
    {
        std::uint32_t owner = census::kNoOwner;
        std::uint64_t sound = 0; // SoundKey
        std::uint32_t timeMs = 0;
        Family family = Family::EntitySound;
    };

    class Cover
    {
    public:
        void Clear() { m_rows.clear(); }
        void Add(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs, Family family)
        {
            CoverRow row;
            row.owner = owner;
            row.sound = SoundKey(ref);
            row.timeMs = timeMs;
            row.family = family;
            m_rows.push_back(row);
        }
        std::size_t Size() const { return m_rows.size(); }

        // `sound`: the packet's sound (PacketSoundKey; any ref is keyed again).
        bool Find(std::uint32_t owner, std::uint64_t sound, std::uint32_t timeMs, Family& out,
            std::uint32_t toleranceMs = kCoverToleranceMs) const
        {
            sound = SoundKey(sound);
            for (const CoverRow& row : m_rows)
            {
                if (row.owner != owner || row.sound != sound)
                    continue;
                const std::uint32_t gap = row.timeMs > timeMs ? row.timeMs - timeMs : timeMs - row.timeMs;
                if (gap <= toleranceMs)
                {
                    out = row.family;
                    return true;
                }
            }
            return false;
        }

    private:
        std::vector<CoverRow> m_rows;
    };

    // The owners whose own sounds packets mode refuses at the ports (the ped
    // audio holders: peds, the player, animals, horses).
    inline bool PortsBlockClass(census::OwnerClass cls)
    {
        return cls == census::OwnerClass::Player || cls == census::OwnerClass::Ped || cls == census::OwnerClass::Animal ||
            cls == census::OwnerClass::Horse;
    }

    // ------------------------------------------------------------ packet index

    // The packets the generic player takes in packets mode, for the tracks that
    // replay a sound by its ref: a row of the same owner and the same sound within
    // the tolerance is the generic player's and the track leaves it (the ESND,
    // SNDS, CSND and GSND rows). The same sound: the raw ref, or set and variant
    // (the low 40 bits: the ESND row 0x80000e002f4361 and the packet id
    // 0xe002f4361), or the hash.
    constexpr std::uint32_t kPacketRowToleranceMs = kCoverToleranceMs;

    struct IndexedPacket
    {
        std::uint32_t owner = census::kNoOwner;
        std::uint32_t refLow = 0;
        std::uint32_t timeMs = 0;
        std::uint64_t stableId = 0;
        std::uint64_t rawRef = 0;
        bool operator<(const IndexedPacket& o) const
        {
            if (owner != o.owner)
                return owner < o.owner;
            if (refLow != o.refLow)
                return refLow < o.refLow;
            return timeMs < o.timeMs;
        }
    };

    inline bool PacketSameSound(std::uint64_t rowRef, std::uint64_t stableId, std::uint64_t rawRef)
    {
        constexpr std::uint64_t kSetAndVariant = 0xFFFFFFFFFFull;
        return (rawRef && rowRef == rawRef) || (stableId && (rowRef & kSetAndVariant) == (stableId & kSetAndVariant));
    }

    class PacketIndex
    {
    public:
        void Clear() { m_rows.clear(); }
        void Add(std::uint32_t owner, std::uint64_t stableId, std::uint64_t rawRef, std::uint32_t timeMs)
        {
            if (owner == census::kNoOwner)
                return; // a world sound: no track row of an owner to leave
            IndexedPacket row;
            row.owner = owner;
            // an id of kind none is 0: the raw ref's low dword names it
            row.refLow = static_cast<std::uint32_t>((stableId ? stableId : rawRef) & 0xFFFFFFFFull);
            row.timeMs = timeMs;
            row.stableId = stableId;
            row.rawRef = rawRef;
            m_rows.push_back(row);
            // a raw ref whose low dword differs from the id's (a hash id):
            // indexed under the raw ref's low dword too
            if (stableId && rawRef && static_cast<std::uint32_t>(rawRef & 0xFFFFFFFFull) != row.refLow)
            {
                row.refLow = static_cast<std::uint32_t>(rawRef & 0xFFFFFFFFull);
                m_rows.push_back(row);
            }
            m_sorted = false;
        }
        void Seal()
        {
            std::sort(m_rows.begin(), m_rows.end());
            m_sorted = true;
        }
        std::size_t Size() const { return m_rows.size(); }

        // Whether a packet carries the row (owner, ref, time).
        bool Covers(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs,
            std::uint32_t toleranceMs = kPacketRowToleranceMs) const
        {
            if (!m_sorted || owner == census::kNoOwner)
                return false;
            IndexedPacket key;
            key.owner = owner;
            key.refLow = static_cast<std::uint32_t>(ref & 0xFFFFFFFFull);
            key.timeMs = timeMs > toleranceMs ? timeMs - toleranceMs : 0;
            for (auto it = std::lower_bound(m_rows.begin(), m_rows.end(), key); it != m_rows.end(); ++it)
            {
                if (it->owner != owner || it->refLow != key.refLow || it->timeMs > timeMs + toleranceMs)
                    break;
                if (PacketSameSound(ref, it->stableId, it->rawRef))
                    return true;
            }
            return false;
        }

    private:
        std::vector<IndexedPacket> m_rows;
        bool m_sorted = false;
    };

    // ------------------------------------------------------------ foot windows

    // What the step track remakes. The step and hoof tracks replay the recorded
    // foot events (FOOT) through the engine's own step, so a packet of the step
    // families is theirs when it falls near a recorded foot event of its owner:
    // from kFootBeforeMs before the event to kFootAfterMs after the event plus its
    // delay (arg7: the hooves of one gallop event land 0, 120, 330 and 430 ms
    // after it). The rest (a slide, a drag, a braking scrape, a layer of an owner
    // with no foot event) the step track never makes, so in packets mode the
    // generic player plays it.
    constexpr std::uint32_t kFootBeforeMs = 100;
    constexpr std::uint32_t kFootAfterMs = 150;
    constexpr std::uint32_t kFootMaxDelayMs = 1000; // a larger arg7 is not a delay

    class FootWindows
    {
    public:
        void Clear()
        {
            m_rows.clear();
            m_sorted = false;
        }
        void Add(std::uint32_t owner, std::uint32_t timeMs, std::uint32_t delayMs)
        {
            if (owner == census::kNoOwner)
                return;
            Row row;
            row.owner = owner;
            row.fromMs = timeMs > kFootBeforeMs ? timeMs - kFootBeforeMs : 0;
            row.toMs = timeMs + (delayMs <= kFootMaxDelayMs ? delayMs : 0) + kFootAfterMs;
            m_rows.push_back(row);
            m_sorted = false;
        }
        void Seal()
        {
            std::sort(m_rows.begin(), m_rows.end());
            m_sorted = true;
        }
        std::size_t Size() const { return m_rows.size(); }
        // A recorded foot event of `owner` whose window holds `timeMs`.
        bool Covers(std::uint32_t owner, std::uint32_t timeMs) const
        {
            if (!m_sorted)
                return false;
            constexpr std::uint32_t kReach = kFootBeforeMs + kFootMaxDelayMs + kFootAfterMs;
            Row key;
            key.owner = owner;
            key.fromMs = timeMs > kReach ? timeMs - kReach : 0;
            for (auto it = std::lower_bound(m_rows.begin(), m_rows.end(), key); it != m_rows.end(); ++it)
            {
                if (it->owner != owner || it->fromMs > timeMs)
                    break;
                if (timeMs <= it->toMs)
                    return true;
            }
            return false;
        }

    private:
        struct Row
        {
            std::uint32_t owner = census::kNoOwner;
            std::uint32_t fromMs = 0;
            std::uint32_t toMs = 0;
            bool operator<(const Row& o) const { return owner != o.owner ? owner < o.owner : fromMs < o.fromMs; }
        };
        std::vector<Row> m_rows;
        bool m_sorted = false;
    };

    // The families whose track replays the recorded foot event.
    inline bool StepFamily(Family family) { return family == Family::Footstep || family == Family::HorseStep; }

    // The families whose track replays a person's recorded event through the
    // engine (the foot event, the melee hit and body impact) and whose packets
    // complete that call: the generic player plays a packet the call did not make
    // again (sound_generic_player_core.h, SoundPlan::complement). Persons only
    // (the player and the peds): the hoof steps of horses and animals keep their
    // track alone. Example: the person's step layer 0x318CEB4 (return 0x318D0A0,
    // called by 0x318D3DE) makes set 0x1d9db9 in the recording that the replayed
    // step call does not make again.
    inline bool ComplementFamily(Family family, census::OwnerClass cls)
    {
        const bool person = cls == census::OwnerClass::Player || cls == census::OwnerClass::Ped;
        return person && (family == Family::Footstep || family == Family::Melee);
    }

    // ------------------------------------------------------------ shot windows

    // What the shot track remakes. A sound on a weapon object's audio (CWeapon
    // +0x100) is the gunshot family (taker specific: the shot track replays the
    // recorded fire event and the engine makes the shot's sounds again) only when
    // a recorded shot of that weapon is near (from kShotBeforeMs before to
    // kShotAfterMs after: the fire sound is created in the frame of the shot, the
    // echo 131 ms later, the tail and the casings up to about a second) or when a
    // named weapon code asked for it (the caller is outside the audio entity's
    // wrappers: the fire path, the weapon foley). A script sound (every frame
    // inside the wrappers) away from every shot of its weapon goes to the generic
    // player: e.g. a campfire's meat is a weapon object, and its cooking sizzle
    // (PLAY_SOUND_FROM_ENTITY "cook_meat_sizzle_loop") has no shot to replay it.
    constexpr std::uint32_t kShotBeforeMs = 200;
    constexpr std::uint32_t kShotAfterMs = 1500;

    // A recorded weapon id (slot 0xFFFF is "no recorded entity", whatever
    // the instance).
    inline bool KnownWeapon(std::uint32_t packed) { return (packed >> 16) != 0xFFFFu; }

    class ShotWindows
    {
    public:
        void Clear()
        {
            m_rows.clear();
            m_sorted = false;
        }
        void Add(std::uint32_t weapon, std::uint32_t timeMs)
        {
            if (!KnownWeapon(weapon))
                return;
            m_rows.push_back({weapon, timeMs});
            m_sorted = false;
        }
        void Seal()
        {
            std::sort(m_rows.begin(), m_rows.end());
            m_sorted = true;
        }
        std::size_t Size() const { return m_rows.size(); }
        // A recorded shot of `weapon` whose window holds `timeMs`.
        bool Covers(std::uint32_t weapon, std::uint32_t timeMs) const
        {
            if (!m_sorted)
                return false;
            // The first shot of `weapon` at or after timeMs - kShotAfterMs.
            const std::uint32_t from = timeMs > kShotAfterMs ? timeMs - kShotAfterMs : 0;
            const auto it = std::lower_bound(m_rows.begin(), m_rows.end(), std::make_pair(weapon, from));
            return it != m_rows.end() && it->first == weapon && it->second <= timeMs + kShotBeforeMs;
        }

    private:
        std::vector<std::pair<std::uint32_t, std::uint32_t>> m_rows; // weapon, shot ms
        bool m_sorted = false;
    };

    // ------------------------------------------------------------ packet lives

    // What the ports refuse in packets mode. A puppet's own create is refused when
    // the recording plays it (Recorded: a packet of the same owner and sound whose
    // life, from its start to its release, holds the time within kLifeToleranceMs,
    // or that starts within kLifeAheadMs: the recording starts it at its own time,
    // and a puppet can ask for its loop over a second before the recorded start) or
    // when it is proved a double (Alternative: the same owner and the same entry of
    // another set while a recorded packet at that entry is alive: the engine picked
    // another set for the slot the recording fills). The rest passes (Passed: the
    // recording never had that sound for that owner, for instance a loop alive
    // before the recording began) and is counted. Refusing every own create would
    // not work: the persistent wrapper asks again every frame while its ref is
    // empty, so most refusals would be retries.
    constexpr std::uint32_t kLifeToleranceMs = kCoverToleranceMs;
    constexpr std::uint32_t kLifeAheadMs = 3000;

    enum class LifeVerdict : std::uint8_t
    {
        Passed = 0,      // no packet: the puppet makes it
        Recorded = 1,    // the recording plays it: refused
        Alternative = 2, // another set at the entry a recorded packet fills: refused
        Unmade = 3,      // inside a replayed step, a set the owner never made in the clip: refused
        Unrecorded = 4,  // an own persistent create without counterpart in the clip: refused
    };
    constexpr std::uint8_t kLifeVerdictCount = 5;

    inline const char* LifeVerdictName(LifeVerdict verdict)
    {
        switch (verdict)
        {
        case LifeVerdict::Passed: return "passed";
        case LifeVerdict::Recorded: return "recorded";
        case LifeVerdict::Alternative: return "alternative";
        case LifeVerdict::Unmade: return "unmade";
        case LifeVerdict::Unrecorded: return "unrecorded";
        }
        return "?";
    }

    class PacketLives
    {
    public:
        void Clear()
        {
            m_bySound.clear();
            m_byEntry.clear();
            m_sets.clear();
            m_sorted = false;
        }
        // One packet: owner, sound (stable id, else raw ref), start, release
        // (0xFFFFFFFF: alive to the clip's end).
        void Add(std::uint32_t owner, std::uint64_t stableId, std::uint64_t rawRef, std::uint32_t startMs, std::uint32_t endMs)
        {
            if (owner == census::kNoOwner)
                return;
            Life life;
            life.owner = owner;
            life.sound = PacketSoundKey(stableId, rawRef);
            life.fromMs = startMs > kLifeToleranceMs ? startMs - kLifeToleranceMs : 0;
            if (endMs == 0xFFFFFFFFu || endMs > 0xFFFFFFFFu - kLifeToleranceMs)
                life.toMs = 0xFFFFFFFFu;
            else
                life.toMs = (endMs < startMs ? startMs : endMs) + kLifeToleranceMs;
            m_bySound.push_back(life);
            m_sets.emplace_back(owner, life.sound & kSoundSetMask);
            const int entry = SoundEntry(life.sound);
            if (entry >= 0)
            {
                Life slot = life;
                slot.sound = static_cast<std::uint64_t>(entry);
                m_byEntry.push_back(slot);
            }
            m_sorted = false;
        }
        void Seal()
        {
            std::sort(m_bySound.begin(), m_bySound.end());
            std::sort(m_byEntry.begin(), m_byEntry.end());
            std::sort(m_sets.begin(), m_sets.end());
            m_sets.erase(std::unique(m_sets.begin(), m_sets.end()), m_sets.end());
            m_sorted = true;
        }

        // Whether the clip has any packet of this owner, and any packet of this owner
        // in the sound's set (any entry, any time). A replayed step runs the puppet's
        // own step code, which can take a branch the live entity did not (e.g. set
        // 0x0d167c at 0x318DED9); the set the owner never made is refused.
        bool OwnerHasPackets(std::uint32_t owner) const
        {
            if (!m_sorted || owner == census::kNoOwner)
                return false;
            const auto it = std::lower_bound(m_sets.begin(), m_sets.end(), std::make_pair(owner, std::uint64_t{0}));
            return it != m_sets.end() && it->first == owner;
        }
        bool OwnerMadeSet(std::uint32_t owner, std::uint64_t ref) const
        {
            if (!m_sorted || owner == census::kNoOwner)
                return false;
            return std::binary_search(m_sets.begin(), m_sets.end(), std::make_pair(owner, SoundKey(ref) & kSoundSetMask));
        }
        std::size_t Size() const { return m_bySound.size(); }

        LifeVerdict Judge(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs) const
        {
            if (!m_sorted || owner == census::kNoOwner)
                return LifeVerdict::Passed;
            const std::uint64_t sound = SoundKey(ref);
            if (Holds(m_bySound, owner, sound, timeMs, true))
                return LifeVerdict::Recorded;
            const int entry = SoundEntry(sound);
            if (entry >= 0 && Holds(m_byEntry, owner, static_cast<std::uint64_t>(entry), timeMs, false))
                return LifeVerdict::Alternative;
            return LifeVerdict::Passed;
        }

    private:
        struct Life
        {
            std::uint32_t owner = census::kNoOwner;
            std::uint64_t sound = 0;
            std::uint32_t fromMs = 0;
            std::uint32_t toMs = 0;
            bool operator<(const Life& o) const
            {
                if (owner != o.owner)
                    return owner < o.owner;
                if (sound != o.sound)
                    return sound < o.sound;
                return fromMs < o.fromMs;
            }
        };

        // A life of (owner, sound) holding timeMs; `ahead`: one that starts
        // within kLifeAheadMs after timeMs counts too (the recording starts it then).
        static bool Holds(const std::vector<Life>& lives, std::uint32_t owner, std::uint64_t sound, std::uint32_t timeMs,
            bool ahead)
        {
            Life key;
            key.owner = owner;
            key.sound = sound;
            for (auto it = std::lower_bound(lives.begin(), lives.end(), key); it != lives.end(); ++it)
            {
                if (it->owner != owner || it->sound != sound)
                    break;
                if (it->fromMs <= timeMs && timeMs <= it->toMs)
                    return true;
                if (ahead && it->fromMs > timeMs && it->fromMs - timeMs <= kLifeAheadMs)
                    return true;
            }
            return false;
        }

        std::vector<Life> m_bySound;
        std::vector<Life> m_byEntry; // sound = the entry
        std::vector<std::pair<std::uint32_t, std::uint64_t>> m_sets; // (owner, set), sorted and unique
        bool m_sorted = false;
    };

    // ------------------------------------------------------------ table

    struct Overrides
    {
        bool set[kFamilyCount] = {};
        Taker taker[kFamilyCount] = {};
        std::string rejected; // tokens the parser refused (for the log)
        std::uint32_t applied = 0;
        bool packets = false; // SoundGenericMode=packets (the default takers change)
    };

    inline std::string Trim(std::string item)
    {
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r' || item.back() == '\n'))
            item.pop_back();
        return item;
    }

    // "family:taker;family:taker" (spaces around tokens and the colon
    // ignored, ',' as a separator too). An unknown family or taker is kept
    // in `rejected`.
    inline Overrides ParseOverrides(const std::string& text)
    {
        Overrides out;
        std::string token;
        const auto flush = [&]() {
            const std::string item = Trim(token);
            token.clear();
            if (item.empty())
                return;
            const std::size_t colon = item.find(':');
            Family family = Family::World;
            Taker taker = Taker::Generic;
            if (colon == std::string::npos || !FamilyOfName(Trim(item.substr(0, colon)), family) ||
                !TakerOfName(Trim(item.substr(colon + 1)), taker))
            {
                out.rejected += (out.rejected.empty() ? "" : ";") + item;
                return;
            }
            out.set[static_cast<std::uint8_t>(family)] = true;
            out.taker[static_cast<std::uint8_t>(family)] = taker;
            ++out.applied;
        };
        for (const char c : text)
        {
            if (c == ';' || c == ',')
                flush();
            else
                token += c;
        }
        flush();
        return out;
    }

    inline Taker TakerOf(Family family, const Overrides& overrides)
    {
        const std::uint8_t index = static_cast<std::uint8_t>(family);
        if (index < kFamilyCount && overrides.set[index])
            return overrides.taker[index];
        return DefaultTaker(family, overrides.packets);
    }

    // The family of a packet (the order of the header comment).
    struct PacketKey
    {
        std::uint32_t owner = census::kNoOwner;
        census::OwnerClass cls = census::OwnerClass::World;
        std::uint64_t stableId = 0; // set + variant, or the hash
        std::uint64_t rawRef = 0;   // the sound when the id is none
        std::uint32_t caller = 0;
        std::uint32_t timeMs = 0;
        bool audioKnown = false; // SGEN v2: the audio entity's base from the owner
        census::AudioBase audioBase = census::AudioBase::Image;
        // Every captured frame was inside the audio entity's wrappers
        // (soundgeneric::InAudioEntityWrappers of the caller): no named game code
        // asked for the sound.
        bool callerInWrappers = false;
    };

    // A weapon audio packet the shot track does not remake (ShotWindows above): no
    // named caller and no recorded shot of that weapon near it. False without the
    // shot list, and for a weapon the clip has no id for (its shots, if any, carry
    // no weapon id either, so no window can be checked: it stays the shot's).
    inline bool OffShot(const PacketKey& key, const ShotWindows* shots)
    {
        return shots && key.audioKnown && key.audioBase == census::AudioBase::WeaponAudio && key.callerInWrappers &&
            KnownWeapon(key.owner) && !shots->Covers(key.owner, key.timeMs);
    }

    // `shots`: the clip's recorded shots; null makes every weapon audio packet the shot's.
    inline Family FamilyOf(const PacketKey& key, const Cover& cover, const ShotWindows* shots = nullptr)
    {
        Family family = Family::World;
        if (cover.Find(key.owner, PacketSoundKey(key.stableId, key.rawRef), key.timeMs, family))
            return family;
        if (static_cast<std::uint32_t>(census::RefLow(key.stableId)) == kWetHoofSet)
            return Family::HorseStep;
        if (FamilyOfCaller(key.caller, family))
            return family;
        if (key.audioKnown && key.audioBase == census::AudioBase::WeaponAudio &&
            !OffShot(key, shots))
            return Family::Gunshot;
        if (key.owner == census::kNoOwner || key.cls == census::OwnerClass::World)
            return Family::World;
        return Family::Owned;
    }

    // One line per family for the log and the report.
    inline std::string FormatTable(const Overrides& overrides)
    {
        std::string out;
        for (std::uint8_t n = 0; n < kFamilyCount; ++n)
        {
            const Family family = static_cast<Family>(n);
            out += (n ? ";" : "") + std::string(FamilyName(family)) + ":" + TakerName(TakerOf(family, overrides)) +
                (overrides.set[n] ? "(ini)" : "");
        }
        return out;
    }
}
