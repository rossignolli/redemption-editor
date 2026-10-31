#pragma once

// The player state the ped audio of a puppet reads.
//
// (a) Foley variants. The engine draws two variant indices per ped audio
// holder once, at activation: 0 and 0 for the local player, random for every
// other ped, horse and animal; every sound the holder creates picks its
// variant by them. A puppet is never the local player, so left alone the
// player's puppet would draw random variants and every other puppet new ones.
// So each recorded owner's two indices are recorded once (clip section FVAR)
// and written into its puppet's component, the player's 0 and 0 included. A
// clip without the section gives the recorded player's puppet the live
// player's indices. A puppet whose activation already ran without the
// resource of an index 2 or 3 is left as it is (the resource is requested
// only there).
//
// (b) Ped variables (evidence only). The block of each recorded owner
// (record) and puppet (play), and which names the sounds resolved in those
// blocks (a read-only detour on the variable lookup, ped_audio_state.cpp).
//
// The engine code is found by signature (game_sig_core.h): the signatures of
// (a) and (b) in ped_audio_state.cpp, those of (c) and (d) below.
// Evidence: offline dump of build 1491.50.
// Research: docs/research/ped-audio-state.md

#include "game_sig_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace replay::pedaudiostate
{
    // ---- (a) foley variants ----
    constexpr std::size_t kComponentOffset = 0x210; // in the holder
    constexpr std::size_t kVariantAOffset = 0xEC;
    constexpr std::size_t kVariantBOffset = 0xF0;
    constexpr std::size_t kResourceOffset = 0x168;
    constexpr std::int32_t kUndrawn = -1;
    constexpr std::int32_t kMaxVariant = 64; // tunable counts are small; anything past this is not a variant
    constexpr std::uint32_t kVariantVariableA = 0x99789761u;
    constexpr std::uint32_t kVariantVariableB = 0xDF33D3DEu;

    struct Variants
    {
        std::int32_t a = kUndrawn;
        std::int32_t b = kUndrawn;
        bool Drawn() const { return a != kUndrawn && b != kUndrawn; }
        bool Plausible() const { return a >= kUndrawn && a <= kMaxVariant && b >= kUndrawn && b <= kMaxVariant; }
        bool operator==(const Variants& o) const { return a == o.a && b == o.b; }
        bool operator!=(const Variants& o) const { return !(*this == o); }
    };

    // The activation requests the +0x168 resource for an index 2 or 3.
    inline bool NeedsResource(std::int32_t index) { return index == 2 || index == 3; }

    enum class Decision : std::uint8_t
    {
        Same = 0,        // the puppet already has the target
        Write = 1,       // write the target
        NoTarget = 2,    // nothing recorded for this owner (and not the player)
        Implausible = 3, // a value read is out of range: the layout is not the expected one
        NoResource = 4,  // the target needs the +0x168 resource the puppet's activation did not request
    };

    inline const char* DecisionName(Decision d)
    {
        switch (d)
        {
        case Decision::Same: return "same";
        case Decision::Write: return "write";
        case Decision::NoTarget: return "no_target";
        case Decision::Implausible: return "implausible";
        case Decision::NoResource: return "no_resource";
        }
        return "?";
    }

    // `current`: the puppet's component now; `resourceHeld`: its +0x168 is
    // set. An undrawn puppet (activation not run yet) takes any target: its
    // activation then skips the draw and requests the resource itself.
    inline Decision Decide(const Variants& current, const Variants& target, bool resourceHeld)
    {
        if (!target.Drawn())
            return Decision::NoTarget;
        if (!current.Plausible() || !target.Plausible() || target.a < 0 || target.b < 0)
            return Decision::Implausible;
        if (current == target)
            return Decision::Same;
        if (current.Drawn() && !resourceHeld && (NeedsResource(target.a) || NeedsResource(target.b)))
            return Decision::NoResource;
        return Decision::Write;
    }

    // One recorded owner's indices (clip section FVAR). Drawn once per ped
    // by the engine, so one row per owner.
    struct VariantRow
    {
        std::uint32_t owner = 0; // packed replay id
        std::int32_t a = kUndrawn;
        std::int32_t b = kUndrawn;
        std::uint8_t player = 0; // the owner was the local player when recorded
    };
    constexpr std::size_t kVariantRowBytes = 4 + 4 + 4 + 1;
    constexpr std::size_t kMaxVariantRows = 4096;

    struct VariantTrack
    {
        std::vector<VariantRow> rows; // sorted by owner
        std::uint32_t changed = 0;    // evidence: an owner seen again with other indices (not saved)

        void Clear()
        {
            rows.clear();
            changed = 0;
        }

        const VariantRow* Find(std::uint32_t owner) const
        {
            const auto it = std::lower_bound(rows.begin(), rows.end(), owner,
                [](const VariantRow& row, std::uint32_t id) { return row.owner < id; });
            return it != rows.end() && it->owner == owner ? &*it : nullptr;
        }

        // True when the owner is new (kept). A drawn owner seen again with
        // other indices keeps the first (the engine draws once; a reused
        // slot is another owner id) and counts.
        bool Note(std::uint32_t owner, const Variants& v, bool player)
        {
            if (!v.Drawn() || !v.Plausible() || v.a < 0 || v.b < 0)
                return false;
            const auto it = std::lower_bound(rows.begin(), rows.end(), owner,
                [](const VariantRow& row, std::uint32_t id) { return row.owner < id; });
            if (it != rows.end() && it->owner == owner)
            {
                changed += (it->a != v.a || it->b != v.b) ? 1u : 0u;
                return false;
            }
            if (rows.size() >= kMaxVariantRows)
                return false;
            VariantRow row;
            row.owner = owner;
            row.a = v.a;
            row.b = v.b;
            row.player = player ? 1 : 0;
            rows.insert(it, row);
            return true;
        }

        // The rows are sorted, unique and in range (a loaded section).
        bool Valid() const
        {
            if (rows.size() > kMaxVariantRows)
                return false;
            for (std::size_t n = 0; n < rows.size(); ++n)
            {
                const VariantRow& row = rows[n];
                if (row.a < 0 || row.a > kMaxVariant || row.b < 0 || row.b > kMaxVariant || row.player > 1)
                    return false;
                if (n && rows[n - 1].owner >= row.owner)
                    return false;
            }
            return true;
        }
    };

    // The cut window keeps the rows of the owners it keeps (`kept(owner)`).
    template <typename Kept>
    void CutInto(const VariantTrack& in, VariantTrack& out, Kept kept)
    {
        out.Clear();
        for (const VariantRow& row : in.rows)
            if (kept(row.owner))
                out.rows.push_back(row);
    }

    enum class TargetSource : std::uint8_t
    {
        None = 0,
        Recorded = 1, // the clip's row for this owner
        Player = 2,   // no row: the recorded player's puppet takes the live player's indices
    };

    inline const char* TargetSourceName(TargetSource s)
    {
        return s == TargetSource::Recorded ? "recorded" : s == TargetSource::Player ? "player" : "none";
    }

    // What a puppet should carry. `livePlayer` is the live player's
    // component (0 and 0 by the draw's rule; undrawn when unreadable, then
    // the rule's own 0 and 0).
    inline TargetSource TargetFor(const VariantTrack& track, std::uint32_t owner, bool recordedPlayer,
        const Variants& livePlayer, Variants& out)
    {
        if (const VariantRow* row = track.Find(owner))
        {
            out.a = row->a;
            out.b = row->b;
            return TargetSource::Recorded;
        }
        if (recordedPlayer)
        {
            out = livePlayer.Drawn() && livePlayer.Plausible() ? livePlayer : Variants{0, 0};
            return TargetSource::Player;
        }
        out = Variants{};
        return TargetSource::None;
    }

    // ---- (b) ped variables ----
    constexpr std::size_t kHolderBlockIndexOffset = 0x12; // int16, -1 none
    constexpr std::size_t kBlockEntryBytes = 16;        // in the block table
    constexpr std::size_t kBlockValuesOffset = 8;      // in the entry
    constexpr std::size_t kDescriptorCountOffset = 8;  // u32
    constexpr std::size_t kDescriptorNamesOffset = 0xC; // u32 name every 8 bytes
    constexpr std::size_t kDescriptorNameStride = 8;
    constexpr std::int32_t kMaxBlockIndex = 0x1FF;     // a sound's request counts blocks up to 0x1FF
    constexpr std::uint32_t kMaxBlockVariables = 32;   // PED_VARIABLES has 20
    constexpr std::uint32_t kPedVariablesName = 0x549DD7FBu; // joaat("PED_VARIABLES")
    constexpr std::uint32_t kIsPlayer = 0x836BD219u;         // joaat("is_player")

    // What the ped variable update (and the block's creation) write, from the
    // code: the reader of the log gets a label for the names it knows.
    struct KnownVariable
    {
        std::uint32_t name;
        const char* label;
    };
    constexpr KnownVariable kKnownVariables[] = {
        {0x836BD219u, "is_player"},           // row +0xB8 player test
        {0xA1691FFDu, "player_test_b"},       // the same value
        {0xDB583619u, "holder_4662_bit0"},    // player or focus flag
        {0xA40EABB8u, "row88_61c"},           // component row +0x88, +0x61C
        {0x1E6DE9ACu, "row88_620"},           // same component, +0x620
        {0x5712CB55u, "fn_317b548"},          // a function of the ped (the label is its address)
        {0xBDE9B2A5u, "ped_b0_bit7"},         // ped +0xB0 bit 7
        {0x67B5B64Au, "fn_31893fc"},          // a function of the ped (the label is its address)
        {0x16755137u, "rowf0_dead"},          // row +0xF0 health component at or below 0
        {0x66778478u, "rowd0_398_bit3"},      // a function of holder +0x1A0, or row +0xD0 +0x398 bit 3
        {0x8BAE9C31u, "speed"},               // row +0x88 +0x5DC, else the length of the ped's velocity
        {0x758A1FDAu, "fn_317c120"},          // a function of the ped and holder +0x4608 (the label: its address)
        {0x39F3F833u, "row50_22_bit3"},       // with a global flag: row +0x50 +0x22 bit 3
        {0x4A276B23u, "player_settings"},     // player only: the bits of joaat("player")
        {0x69AB332Bu, "holder_465c"},         // at the block's creation (the block alloc)
    };

    inline const char* KnownLabel(std::uint32_t name)
    {
        for (const KnownVariable& known : kKnownVariables)
            if (known.name == name)
                return known.label;
        return nullptr;
    }

    struct Block
    {
        std::int32_t index = -1;
        std::uintptr_t values = 0; // the values array (the probe's range)
        std::uint32_t count = 0;
        std::uint32_t names[kMaxBlockVariables] = {};
        float data[kMaxBlockVariables] = {};
    };

    // The holder's block in the block table at `table` (where the block
    // setter has it, ped_audio_state.cpp; 0: not found). read(address, out,
    // size) -> bool: a guarded read.
    template <typename Read>
    bool ReadBlock(std::uintptr_t table, std::uintptr_t holder, Read read, Block& out)
    {
        out = Block{};
        if (!table || !holder)
            return false;
        std::int16_t index = -1;
        if (!read(holder + kHolderBlockIndexOffset, &index, sizeof(index)) || index < 0 || index > kMaxBlockIndex)
            return false;
        const std::uintptr_t entry = table + static_cast<std::uintptr_t>(index) * kBlockEntryBytes;
        std::uintptr_t descriptor = 0;
        std::uintptr_t values = 0;
        std::uint32_t count = 0;
        if (!read(entry, &descriptor, sizeof(descriptor)) || !descriptor ||
            !read(entry + kBlockValuesOffset, &values, sizeof(values)) || !values ||
            !read(descriptor + kDescriptorCountOffset, &count, sizeof(count)) || count == 0 || count > kMaxBlockVariables)
            return false;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            if (!read(descriptor + kDescriptorNamesOffset + n * kDescriptorNameStride, &out.names[n], sizeof(std::uint32_t)))
                return false;
        }
        if (!read(values, out.data, count * sizeof(float)))
            return false;
        out.index = index;
        out.values = values;
        out.count = count;
        return true;
    }

    // The value of `name` in a block (false: not in it).
    inline bool ValueOf(const Block& block, std::uint32_t name, float& out)
    {
        for (std::uint32_t n = 0; n < block.count && n < kMaxBlockVariables; ++n)
            if (block.names[n] == name)
            {
                out = block.data[n];
                return true;
            }
        return false;
    }

    inline bool SameValue(float a, float b)
    {
        if (std::isnan(a) || std::isnan(b))
            return std::isnan(a) && std::isnan(b);
        return std::fabs(a - b) <= 1e-4f * (1.0f + std::fabs(a));
    }

    // The block as "name=value;..." (known names labelled).
    inline std::string BlockText(const Block& block)
    {
        std::string text;
        char part[64]{};
        for (std::uint32_t n = 0; n < block.count && n < kMaxBlockVariables; ++n)
        {
            const char* label = KnownLabel(block.names[n]);
            if (label)
                std::snprintf(part, sizeof(part), "%s%s=%g", text.empty() ? "" : ";", label, block.data[n]);
            else
                std::snprintf(part, sizeof(part), "%s0x%08x=%g", text.empty() ? "" : ";", block.names[n], block.data[n]);
            text += part;
        }
        return text.empty() ? "none" : text;
    }

    // Names whose value differs between two blocks ("name:rec>play;...");
    // a name missing on one side counts with "-".
    inline std::string DiffText(const Block& recorded, const Block& replayed, std::uint32_t& differing)
    {
        differing = 0;
        std::string text;
        char part[96]{};
        const auto name = [](std::uint32_t hash, char* out, std::size_t size) {
            if (const char* label = KnownLabel(hash))
                std::snprintf(out, size, "%s", label);
            else
                std::snprintf(out, size, "0x%08x", hash);
        };
        char label[24]{};
        for (std::uint32_t n = 0; n < recorded.count && n < kMaxBlockVariables; ++n)
        {
            float other = 0.0f;
            const bool there = ValueOf(replayed, recorded.names[n], other);
            if (there && SameValue(recorded.data[n], other))
                continue;
            ++differing;
            name(recorded.names[n], label, sizeof(label));
            if (there)
                std::snprintf(part, sizeof(part), "%s%s:%g>%g", text.empty() ? "" : ";", label, recorded.data[n], other);
            else
                std::snprintf(part, sizeof(part), "%s%s:%g>-", text.empty() ? "" : ";", label, recorded.data[n]);
            text += part;
        }
        for (std::uint32_t n = 0; n < replayed.count && n < kMaxBlockVariables; ++n)
        {
            float unused = 0.0f;
            if (ValueOf(recorded, replayed.names[n], unused))
                continue;
            ++differing;
            name(replayed.names[n], label, sizeof(label));
            std::snprintf(part, sizeof(part), "%s%s:->%g", text.empty() ? "" : ";", label, replayed.data[n]);
            text += part;
        }
        return text.empty() ? "none" : text;
    }

    // Whether two snapshots of a block differ (a log line on change only).
    inline bool BlockChanged(const Block& a, const Block& b)
    {
        if (a.count != b.count || a.index != b.index)
            return true;
        for (std::uint32_t n = 0; n < a.count && n < kMaxBlockVariables; ++n)
            if (a.names[n] != b.names[n] || !SameValue(a.data[n], b.data[n]))
                return true;
        return false;
    }

    // In-session reference for the play phase (log only, never saved): the
    // recorded owners' blocks at each change, with the owner's model, so the
    // same clip replayed in the same session can be compared at the playhead.
    struct BlockSample
    {
        std::uint32_t timeMs = 0;
        Block block;
    };
    struct BlockHistory
    {
        std::uint32_t owner = 0;
        std::uint32_t model = 0;
        std::vector<BlockSample> samples; // time order
        static constexpr std::size_t kMaxSamples = 512;

        // The sample in force at `timeMs` (the first one before the first).
        const Block* At(std::uint32_t timeMs) const
        {
            if (samples.empty())
                return nullptr;
            const auto it = std::upper_bound(samples.begin(), samples.end(), timeMs,
                [](std::uint32_t t, const BlockSample& s) { return t < s.timeMs; });
            return it == samples.begin() ? &samples.front().block : &std::prev(it)->block;
        }

        bool Note(std::uint32_t timeMs, const Block& block)
        {
            if (!samples.empty() && !BlockChanged(samples.back().block, block))
                return false;
            if (samples.size() >= kMaxSamples)
                return false;
            if (!samples.empty() && timeMs < samples.back().timeMs)
                timeMs = samples.back().timeMs;
            samples.push_back({timeMs, block});
            return true;
        }
    };

    // A published block: the values range of one owner's PED_VARIABLES.
    struct Range
    {
        std::uintptr_t values = 0;
        std::uint32_t count = 0;
        std::uint32_t owner = 0; // packed replay id
        // The block's names, for the volume probe's reading of a sound's
        // variable pointer (wvol=ped:...).
        std::uint32_t names[kMaxBlockVariables] = {};
    };
    // Index of the range holding `address` (a float of the block), -1 none.
    inline int RangeOf(const Range* ranges, std::uint32_t n, std::uintptr_t address)
    {
        for (std::uint32_t i = 0; i < n; ++i)
        {
            const Range& r = ranges[i];
            if (r.values && address >= r.values && address < r.values + r.count * sizeof(float) &&
                ((address - r.values) % sizeof(float)) == 0)
                return static_cast<int>(i);
        }
        return -1;
    }

    // Engine side (ped_audio_state.cpp).
    // The signatures of (a) and (b); false leaves both off (`error` names the first mismatch).
    bool Init(std::string& error);
    bool VariantsReady();
    bool BlocksReady();
    // Script thread: the holder's component indices and whether +0x168 is held.
    bool ReadVariants(std::uintptr_t holder, Variants& out, bool& resourceHeld);
    // Script thread: the holder's PED_VARIABLES block.
    bool ReadHolderBlock(std::uintptr_t holder, Block& out);

    // Script thread: the blocks of the last
    // PED_VARS scan with their names; the owner and name of a float of one
    // of them (the volume probe names the variable a sound's wrapper reads).
    void SetNamedRanges(const std::vector<Range>& ranges);
    bool DescribeAddress(std::uintptr_t address, std::uint32_t& owner, std::uint32_t& name);

    // (c) the puppets' speed (ped_speed_core.h, PedSpeedReplay).
    // A detour on the ped variable update (its prologue, the speed source and
    // the speed store checked) that forwards every call and, for a holder the
    // script thread published (a puppet's), writes the recorded speed into
    // the holder's block right after the engine wrote its own (the frozen
    // puppet's 0). Nothing else is touched; no native is called.
    //
    // The update up to the block's creation (the detour's jump fits in it):
    // mov [rsp+18h],rbx; push rsi; push rdi; push r15; sub rsp,50h; cmp word
    // [rcx+12h],-1 (the holder's block index); mov rbx,rcx; movaps
    // [rsp+40h],xmm6; movaps [rsp+30h],xmm7; jne +5; call the block alloc.
    inline const sig::Signature kUpdatePedVariables{"pedaudio_update_ped_variables",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 83 79 12 FF 48 8B D9 0F 29 74 24 40 0F 29 7C 24 30 75 05 E8 ?? ?? ?? "
        "??",
        0x31A3ABC};
    // movss xmm2,[rcx+5DCh] (the movement component's speed).
    inline const sig::Signature kSpeedSource{"pedaudio_speed_source", kUpdatePedVariables, sig::Offset{0x438},
        "F3 0F 10 91 DC 05 00 00", 0x31A3EF4};
    // mov edx,8BAE9C31h; mov rcx,rbx; call the block setter (the speed into
    // the holder's block).
    inline const sig::Signature kSpeedStore{"pedaudio_speed_store", kUpdatePedVariables, sig::Offset{0x4BE},
        "BA 31 9C AE 8B 48 8B CB E8 ?? ?? ?? ??", 0x31A3F7A};
    constexpr std::uint32_t kMaxSpeedTargets = 256;
    struct SpeedTarget
    {
        std::uintptr_t holder = 0;
        float speed = 0.0f;
    };
    bool InstallSpeedOverride(std::string& error);
    void ShutdownSpeedOverride();
    bool SpeedOverrideInstalled();
    // Script thread: the puppets' holders and speeds (a holder keeps its
    // slot while it stays published; an empty list stops every write).
    void SetSpeedTargets(const std::vector<SpeedTarget>& targets);
    // Script thread: the engine's own speed the detour last saw for a
    // published holder (before writing over it).
    bool EngineSpeedOf(std::uintptr_t holder, float& out);
    // Script thread: the speed in the holder's block now (guarded).
    bool ReadHolderSpeed(std::uintptr_t holder, float& out);
    struct SpeedCounts
    {
        std::uint64_t calls = 0;
        std::uint64_t hits = 0;
        std::uint64_t writes = 0;
        std::uint64_t failed = 0;
    };
    SpeedCounts SpeedSnapshot();

    // (d) the movement component's speed (MotionSpeedReplay).
    // The block speed of (c) is a copy: the step volume curve, three other ped
    // audio reads and the gait classifier read the movement component's own
    // speed C +0x5DC, which a frozen puppet holds at 0. A detour on the raw
    // speed (its prologue, and the writer, the sound and the gait read
    // checked) forwards every call and, in normal play, answers the published
    // puppets' recorded speed (the target of (c): PSPD, else the recorded
    // motion). The smoother's rates do not smooth at ped speeds (ped_speed_core.h).
    // Research: docs/research/ped-audio-state.md
    constexpr std::uintptr_t kMotionSpeedOffset = 0x5DC;
    // The raw speed, float(C): mov [rsp+10h],rbx; push rdi; sub rsp,40h; mov
    // rbx,[rcx+8]; mov rdi,rcx; movaps [rsp+30h],xmm6; xorps xmm6,xmm6; and
    // rbx,-10h (unique in the code: a call that lands on these bytes calls it).
    inline const sig::Signature kMotionSpeedSource{"pedaudio_motion_speed_source",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B F9 0F 29 74 24 30 0F 57 F6 48 83 E3 F0",
        0x8AABCC};
    // The movement speed update and its writer: call the raw speed; movaps
    // xmm1,xmm0; lea rcx,[rbx+4C8h]; movaps xmm2,xmm6; call the smoother; movss
    // [rbx+5DCh],xmm0. Its first call lands on the raw speed, the second on the
    // smoother.
    inline const sig::Signature kMotionSpeedUpdate{"pedaudio_motion_speed_update",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B 79 08", 0x8EC40C};
    inline const sig::Signature kMotionSpeedWriter{"pedaudio_motion_speed_writer", kMotionSpeedUpdate,
        sig::Offset{0x1AE},
        "E8 ?? ?? ?? ?? 0F 28 C8 48 8D 8B C8 04 00 00 0F 28 D6 E8 ?? ?? ?? ?? F3 0F 11 83 DC 05 00 00", 0x8EC5BA};
    inline const sig::Signature kMotionWriterSource{"pedaudio_motion_writer_source", kMotionSpeedWriter,
        sig::Rel32{1, 5},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B F9 0F 29 74 24 30 0F 57 F6 48 83 E3 F0",
        0x8AABCC};
    inline const sig::Signature kMotionSmoother{"pedaudio_motion_smoother", kMotionSpeedWriter, sig::Rel32{0x13, 0x17},
        "8A 41 18 A8 01 75 04 0F 57 C0 C3 0F 28 C1", 0x2521410};
    // The sound that calls the raw speed, and its call.
    inline const sig::Signature kMotionSound{"pedaudio_motion_sound",
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D A8 08 FF FF FF 48 81 EC F0 01 00 00 0F 29 70 E8",
        0x31A15DC};
    inline const sig::Signature kMotionSpeedSound{"pedaudio_motion_speed_sound", kMotionSound, sig::Offset{0xF7},
        "E8 ?? ?? ?? ??", 0x31A16D3};
    inline const sig::Signature kMotionSoundSource{"pedaudio_motion_sound_source", kMotionSpeedSound, sig::Rel32{1, 5},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B F9 0F 29 74 24 30 0F 57 F6 48 83 E3 F0",
        0x8AABCC};
    // The gait classifier and its read: movss xmm0,[rdi+5DCh].
    inline const sig::Signature kGaitClassifier{"pedaudio_gait_classifier",
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 4C 8B 71 08", 0x8F14A4};
    inline const sig::Signature kMotionGaitRead{"pedaudio_motion_gait_read", kGaitClassifier, sig::Offset{0x186},
        "F3 0F 10 87 DC 05 00 00", 0x8F162A};
    constexpr std::uint32_t kMaxMotionTargets = 256;
    struct MotionTarget
    {
        std::uintptr_t ped = 0; // the puppet's ped (the component's [C +8] & ~0xF)
        float speed = 0.0f;
    };
    bool InstallMotionSpeed(std::string& error);
    void ShutdownMotionSpeed();
    bool MotionSpeedInstalled();
    // Script thread: the puppets and their speeds (empty: the engine's own everywhere).
    void SetMotionTargets(const std::vector<MotionTarget>& targets);
    // Script thread: the engine's own raw value the detour last replaced for a published ped.
    bool EngineMotionOf(std::uintptr_t ped, float& out);
    struct MotionCounts
    {
        std::uint64_t calls = 0;   // the raw speed's calls
        std::uint64_t hits = 0;    // ... for a published puppet (answered with its target)
        std::uint64_t failed = 0;  // the component's ped unreadable while targets were published
    };
    MotionCounts MotionSnapshot();
}
