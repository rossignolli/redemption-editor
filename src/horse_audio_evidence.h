#pragma once

// Evidence for "the player's horse sounds like an NPC horse in the replay"
// (the player's hoofstep refs, low part 0x2f7ab1 and family, replaced by the
// NPC one 0x2f36cb). Logging only: guarded reads (rung 2) of fields whose
// readers were disassembled in build 1491.50, no write, no call, no behaviour
// change. HorseAudioEvidence=1 in the ini.
//
// Offline (build 1491.50, .text dump, capstone):
//   - the queued hoofsteps (foot audio holder +0x1560, queue +0x70, count
//     +0xB0, filled by the footstep entry 0x317A340) are played by the
//     movement sounds 0x3193F20, one call of 0x318D52C per step; the main
//     step sound is the create-and-play at 0x318DAF9 (return 0x318DAFE) with
//     the ref from 0x317FC9C (foot +0x428 settings, when component row slot
//     +0x00 is set) or 0x3182A98 (the ped audio's +0xE00 object, settings at
//     its +0x650 = ped audio +0x1450, when slot +0x00 is empty). Both pick
//     the hash at settings +4 * step type and resolve it (0x252B408).
//   - the +0xE00 object's +0x650 is written by 0x319E8D8 for slot 10 only
//     (0x3182A2C: game object type 0x16 from the slot 10 hash at +0x498,
//     fallback 0x3D857DBD); the eleven slot hashes +0x470..+0x498 come from
//     0x317C8D4 (the ped's audio component, vtable +0x80, 0x3276F78: tag per
//     slot of the ped's outfit entries; empty slot 10 without row +0x00 ->
//     0xDE27F155 or 0xDE27F155 + 0x863F3CF1 by 0xF0AB1C). The update that
//     refreshes them (0x3191324, vtable 0x3839338 slot +0x18) first runs the
//     player horse branch choice 0x319EEE8.
//   - 0x319EEE8 (same object): +0x700 set, row +0xE0 component +0x400 bit 6
//     clear and ped audio +0x45E8 <= 1, then with row +0xA8 component C:
//     0x8B7498(C) || 0x8B69B0(C) || 0x8B4A88(C) || ped audio +0x4662 bit 0
//     -> 0x319CF14 (player horse), else 0x3198574; no C -> 0x31A1900.
//     0x8B69B0: [C+0xB0] entity, its row +0xB8 component +0x40: bit 1 set,
//     bit 0 clear. 0x8B4A88: [C+0xA8] entity has row +0xF0. 0x8B7498:
//     E1 = [C+8] & ~0xF, C0 = row(E1) +0x00, E2 = [C0+8] & ~0xF, then an
//     obfuscated pointer of E2 (0x2B1990) and a task list (0x737B78) whose
//     entity has row +0xB8 +0x40 bit 1 (any player). Only E1 and C0 are read
//     here; the rest is decoded by the engine only.
//   - movement sounds 0x3193F20 set init params +0x138 = 8 when 0x8B69B0
//     says yes (0x31941F9), the params every queued step plays with.
//
// Pure part (no game): the field table, the line text and the comparison;
// unit tested in ReplayCoreTests.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace replay::horseaudio
{
    // The fields compared between the live horse (record) and its puppet
    // (play). Pointers to game objects compare by value (one process, one
    // game object store); the identity pointers are not compared.
    enum Field : std::size_t
    {
        kRowMask,        // bit i: component row slot kRowSlots[i] set
        kOwnFlags40,     // row +0xB8 component +0x40 (the horse's own player bits)
        kLinkIsPlayer,   // 0x8B69B0 replica on row +0xA8: 1 yes, 0 no, 2 no link, 3 unreadable
        kLinkE1,         // 0x8B7498 first step: [C+8]&~F is 1 the horse, 2 the rider ([C+0xB0]), 3 other, 0 none
        kLinkC0,         // row(E1) +0x00 set
        kPred8B4A88,     // 0x8B4A88 replica: [C+0xA8] entity has row +0xF0
        kAudio4662,      // ped audio +0x4662 byte (bit 0 read by 0x319F012)
        kAudio45E8,      // ped audio +0x45E8 dword (> 1: NPC branch, 0x319EF5A)
        kAudio4608,      // ped audio +0x4608 settings object
        kAudio4608H10,   // its +0x10 hash (foot +0x420 lookup, type 0x6E)
        kAudio4608H14,   // its +0x14 hash (foot +0x428 lookup, type 0x61)
        kAudio1450,      // ped audio +0x1450 = +0xE00 object +0x650 (step settings, slot 0 empty)
        kAudio0774,      // ped audio +0x774 byte (0x318DF3B)
        kExt0700,        // +0xE00 object +0x700 byte (0x319EEF2)
        kExt0708,        // +0xE00 object +0x708 set (a sound)
        kExtSlot0,       // +0xE00 object +0x470 + 4 * i, i = 0..10 (slot 10 = +0x498)
        kExtSlot10 = kExtSlot0 + 10,
        kFoot0420,       // foot +0x420 (type 0x6E object)
        kFoot0428,       // foot +0x428 (footstep settings when row +0x00 is set)
        kFoot0580,       // foot +0x580 hash (0x31940DC)
        kFoot05A0,       // foot +0x5A0 byte (0x318DBF9)
        kFoot05A7,       // foot +0x5A7 byte (footstep entry gate)
        kFoot044C,       // foot +0x44C dword (step id remap 0x31814D4)
        kFoot0599,       // foot +0x599 byte (footstep audio slot)
        kFoot0450,       // foot +0x450 byte
        kFoot0024,       // foot +0x24 byte (enabled)
        kRowE0Flags,     // row +0xE0 component +0x400 dword (bit 3: 0x3193FB2, bit 6: 0x319EF45)
        // The horse's persistent loops, 1 while their slot holds a sound (the
        // live horse at record start against its puppet). Offline, build
        // 1491.50:
        //   - +0xE00 object +0x30 and +0x48: the saddle weapon loops of
        //     0x31A0F30 (called by the +0xE00 update 0x3191324 at 0x31914A6;
        //     it walks the ped attachments 0xCC8C58 and keeps one slot per
        //     carried weapon, created through 0x319D96C and the wrapper
        //     0x2588BE0, stopped with 0x2585C2C when the weapon is gone);
        //   - foot +0x596: the tack loops' switch, set by 0x3197C78 at
        //     0x3198099 (from the game code 0x8EECE8, state 10), cleared by
        //     the foot update 0x31925AC at 0x3192824 and by 0x31986BC;
        //   - foot +0x4D8 and +0x4E0: the tack loops of 0x319F8A4 (foot
        //     vtable 0x38389B0 slot +0x80 through 0x319CBF0), entry
        //     0x63D72B3C of the sets at foot +0x240 and +0x280, created
        //     through the wrapper (returns 0x319FC53 and 0x319FEB0) while
        //     +0x596 is set, then moved and driven every update (0x2537168,
        //     0x2536C70, 0x2536E48, variables 0x833FF9C9, 0x48FE569D and
        //     0x41F750D5 = foot +0x580 is 0x41F750D5);
        //   - foot +0x480 and +0x5A5: the urge loop of 0x31A1420 (called by
        //     the foot update at 0x319290B when 0x8B69B0 says yes), created
        //     through the wrapper (return 0x31A1593) while +0x5A5 is set.
        kExt0030,        // +0xE00 object +0x30 set (saddle weapon loop 0)
        kExt0048,        // +0xE00 object +0x48 set (saddle weapon loop 1)
        kFoot0596,       // foot +0x596 byte (tack loops on)
        kFoot04D8,       // foot +0x4D8 set (tack loop A)
        kFoot04E0,       // foot +0x4E0 set (tack loop B)
        kFoot0480,       // foot +0x480 set (urge loop)
        kFoot05A5,       // foot +0x5A5 byte (urge loop request)
        kFieldCount
    };

    // Component row slots summarised in kRowMask (the ones the step path,
    // the branch choice and the predicates test).
    constexpr std::size_t kRowSlots[] = {0x00, 0x30, 0x50, 0x88, 0x90, 0xA8, 0xB0, 0xB8, 0xE0, 0xF0};
    constexpr std::size_t kRowSlotCount = sizeof(kRowSlots) / sizeof(kRowSlots[0]);

    inline const char* FieldName(std::size_t field)
    {
        static const char* const kNames[kFieldCount] = {
            "row_mask", "own_flags40", "link_is_player", "link_e1", "link_c0", "p8b4a88",
            "a4662", "a45e8", "a4608", "a4608_h10", "a4608_h14", "a1450", "a0774",
            "x0700", "x0708", "x0470", "x0474", "x0478", "x047c", "x0480", "x0484", "x0488",
            "x048c", "x0490", "x0494", "x0498",
            "f0420", "f0428", "f0580", "f05a0", "f05a7", "f044c", "f0599", "f0450", "f0024", "row_e0_400",
            "x0030", "x0048", "f0596", "f04d8", "f04e0", "f0480", "f05a5"};
        return field < kFieldCount ? kNames[field] : "?";
    }

    struct State
    {
        bool ok = false;               // the ped audio and the foot were found
        std::uint64_t values[kFieldCount] = {};
        std::uint8_t read[kFieldCount] = {}; // 1 read, 0 not readable
        // Identity (not compared): the ped, its ped audio holder, the
        // layout checks (bit 0 foot +0x10 == holder, bit 1 foot +0x18 ==
        // ped, bit 2 ext +0x10 == holder, bit 3 ext +0x18 == ped) and the
        // +0xE00 object's vtable RVA (0x3839338 expected: the holder
        // constructor 0x3178860 builds holder +0xE00 with 0x3178B04, which
        // stores that vtable, and holder +0x1560 with 0x3178EB4).
        std::uint64_t ped = 0;
        std::uint64_t holder = 0;
        std::uint32_t layout = 0;
        std::uint32_t extVtable = 0;
    };

    inline bool SameField(const State& a, const State& b, std::size_t field)
    {
        return a.read[field] == b.read[field] && (!a.read[field] || a.values[field] == b.values[field]);
    }

    inline bool SameState(const State& a, const State& b)
    {
        if (a.ok != b.ok)
            return false;
        for (std::size_t field = 0; field < kFieldCount; ++field)
            if (!SameField(a, b, field))
                return false;
        return true;
    }

    inline void AppendValue(std::string& out, const State& state, std::size_t field)
    {
        char text[24];
        if (!state.read[field])
            std::snprintf(text, sizeof(text), "na");
        else
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(state.values[field]));
        out += text;
    }

    // "row_mask=0x..,own_flags40=0x..,..." (every field).
    inline std::string FormatState(const State& state)
    {
        std::string out;
        char head[96];
        std::snprintf(head, sizeof(head), "ok=%d,layout=0x%x,ext_vt=0x%x", state.ok ? 1 : 0, state.layout, state.extVtable);
        out += head;
        for (std::size_t field = 0; field < kFieldCount; ++field)
        {
            out += ',';
            out += FieldName(field);
            out += '=';
            AppendValue(out, state, field);
        }
        return out;
    }

    // "name:rec>play|..." for the fields that differ (empty when none);
    // `count` receives how many.
    inline std::string DiffStates(const State& record, const State& play, std::size_t& count)
    {
        std::string out;
        count = 0;
        for (std::size_t field = 0; field < kFieldCount; ++field)
        {
            if (SameField(record, play, field))
                continue;
            ++count;
            if (!out.empty())
                out += '|';
            out += FieldName(field);
            out += ':';
            AppendValue(out, record, field);
            out += '>';
            AppendValue(out, play, field);
        }
        return out;
    }

    // Bit per field that ever differed (kFieldCount < 64).
    static_assert(kFieldCount < 64, "field mask is one u64");
    inline std::uint64_t DiffMask(const State& record, const State& play)
    {
        std::uint64_t mask = 0;
        for (std::size_t field = 0; field < kFieldCount; ++field)
            if (!SameField(record, play, field))
                mask |= 1ull << field;
        return mask;
    }

    inline std::string MaskNames(std::uint64_t mask)
    {
        std::string out;
        for (std::size_t field = 0; field < kFieldCount; ++field)
        {
            if (!(mask & (1ull << field)))
                continue;
            if (!out.empty())
                out += '|';
            out += FieldName(field);
        }
        return out.empty() ? std::string("none") : out;
    }

    // The 0x8B69B0 replica's answer from the linked entity's +0x40 byte.
    constexpr std::uint64_t LinkAnswer(bool readable, bool hasLink, std::uint8_t flags40)
    {
        return !readable ? 3 : !hasLink ? 2 : ((flags40 & 1) == 0 && (flags40 & 2) != 0) ? 1 : 0;
    }

    // A step sound line's grouping key: the request's caller one frame up
    // from the create-and-play wrapper (the step path: 0x318DAFE main step,
    // 0x318DBED slot 0 extra, 0x318DEDE layer, others).
    struct RefKey
    {
        std::uint64_t ref = 0;
        std::uint32_t caller = 0;
        std::uint8_t p138 = 0;
        bool operator<(const RefKey& other) const
        {
            if (ref != other.ref)
                return ref < other.ref;
            if (caller != other.caller)
                return caller < other.caller;
            return p138 < other.p138;
        }
    };

    // The init
    // params a step sound was asked with, live horse against puppet (the
    // replayed footsteps run the engine's step path on the puppet, which
    // builds the params from the puppet's own state). Layout from the
    // params constructor 0x487E34 (build 1491.50): 16 variables (u32 name
    // hash, f32 value) at +0x10, their count at +0x90 (the step path
    // 0x318C4C4 and the tack loops 0x319F8A4 append there, up to 16), the
    // volume f32 at +0xA4 (0 dB by default; the census's vol=).
    constexpr std::size_t kParamsVariables = 0x10;
    constexpr std::size_t kParamsVariableCount = 0x90;
    constexpr std::size_t kParamsMaxVariables = 16;
    constexpr std::size_t kParamsVolumeOffset = 0xA4;
    constexpr std::size_t kParamsBytesNeeded = kParamsVolumeOffset + 4;

    // "vol=-1.97,nvars=2,vars=0xb3d66f04:0.50|0xa53f2e56:1.00" ("vol=na,
    // nvars=na,vars=none" without params; a count above 16 is shown but
    // only 16 are listed).
    inline std::string FormatStepParams(const std::uint8_t* params, std::size_t size)
    {
        if (!params || size < kParamsBytesNeeded)
            return "vol=na,nvars=na,vars=none";
        float volume = 0.0f;
        std::uint32_t count = 0;
        std::memcpy(&volume, params + kParamsVolumeOffset, sizeof(volume));
        std::memcpy(&count, params + kParamsVariableCount, sizeof(count));
        char head[64];
        std::snprintf(head, sizeof(head), "vol=%.2f,nvars=%u,vars=", static_cast<double>(volume), count);
        std::string out = head;
        const std::size_t listed = count < kParamsMaxVariables ? count : kParamsMaxVariables;
        for (std::size_t n = 0; n < listed; ++n)
        {
            std::uint32_t hash = 0;
            float value = 0.0f;
            std::memcpy(&hash, params + kParamsVariables + 8 * n, sizeof(hash));
            std::memcpy(&value, params + kParamsVariables + 8 * n + 4, sizeof(value));
            char item[40];
            std::snprintf(item, sizeof(item), "%s0x%08x:%.2f", n ? "|" : "", hash, static_cast<double>(value));
            out += item;
        }
        if (!listed)
            out += "none";
        return out;
    }

    // The step's filters as the
    // step sound 0x318D52C built them (params +0x104 low-pass u32, min with
    // the foot slot +0x3C0; +0x108 high-pass u32, max with the slot +0x3B0;
    // offline 0x318D776..0x318D794). "lpf=24000,hpf=0" ("lpf=na,hpf=na"
    // without params).
    constexpr std::size_t kParamsLowPassOffset = 0x104;
    constexpr std::size_t kParamsHighPassOffset = 0x108;
    inline std::string FormatStepFilters(const std::uint8_t* params, std::size_t size)
    {
        if (!params || size < kParamsHighPassOffset + 4)
            return "lpf=na,hpf=na";
        std::uint32_t lowPass = 0;
        std::uint32_t highPass = 0;
        std::memcpy(&lowPass, params + kParamsLowPassOffset, sizeof(lowPass));
        std::memcpy(&highPass, params + kParamsHighPassOffset, sizeof(highPass));
        char text[48];
        std::snprintf(text, sizeof(text), "lpf=%u,hpf=%u", lowPass, highPass);
        return text;
    }
}
