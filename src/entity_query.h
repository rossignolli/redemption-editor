#pragma once

// The sounds that depend on their owner's state.
//
// A dynamic entity sound picks its child when it plays by asking the owner's
// audio entity a chain of names (the resolver); for "mount" the resolver
// follows the engine mount link and hands the rest of the chain to the
// mount's audio entity. A replay puppet has no mount link, so the chain
// answers the null sound and the engine ends the sound 2 to 3 frames later
// (saddle and stirrup foley cut short).
//
// The relation is given to the resolver only: when a puppet's chain starts with
// "mount", its engine mount link is empty and the replay seats it on a horse
// puppet (the rider and mount pairs of the pose playback), the rest of the chain
// goes to that horse puppet's audio entity, exactly the resolver's own call in
// its mount branch. No task, link, physics or pose changes.
//
// The detour also logs the outermost queries (rider, chain, answer, thread).
// Research: docs/research/entities-sound-query.md

#include "engine_sigs.h" // enginesig::Pool

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::entityquery
{
    using enginesig::kAudioHolderVtable;
    using enginesig::kAudioHolderVtableA;
    using enginesig::kAudioHolderVtableB;

    // The resolver: the ped audio entity's sound query, its prologue up to the
    // first head of its switch (cmp dword [r8], 0EB69052Eh).
    inline const sig::Signature kResolver{"query_resolver",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 57 48 8B EC 48 83 EC 40 41 81 38 2E 05 69 EB",
        0x31947D0};

    // The vtables of the three ped audio holders (slot +0x68 the resolver in
    // each), as their constructors write them: the base constructor's, and the
    // two the ped factories build at +0x250 of their peds (0x4A70 and 0x4CB0
    // bytes), each constructor calling the base first.
    inline const sig::Signature* const kHolderVtables[] = {&kAudioHolderVtable, &kAudioHolderVtableA,
        &kAudioHolderVtableB};

    // The engine mount link (IS_PED_ON_MOUNT's handler is exactly this != 0):
    // slot u32 [ped+0x9C] & 0x1FFFF minus the base index, a row of 0x148 bytes
    // in the table, the ride component +0x128 & ~1, its flags +0x10 bit 4, the
    // mount +0x178. ComponentRow and EngineMountOf repeat these reads.
    inline const sig::Signature kMountLink{"query_mount_link",
        "8B 81 9C 00 00 00 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 94 01 28 "
        "01 00 00 48 8B C2 48 83 E0 FE 48 F7 DA 48 1B C9 48 23 C8 74 12 8B 41 10 C1 E8 04 A8 01 74 08 48 8B 81 78 01 "
        "00 00 C3 33 C0 C3",
        0xCC857C};
    inline const sig::Signature kMountLinkBaseIndex{"query_component_base_index", kMountLink, sig::Rel32{0x0D, 0x11},
        "", 0x39ECE40};
    inline const sig::Signature kMountLinkTable{"query_component_table", kMountLink, sig::Rel32{0x1B, 0x1F}, "",
        0x39ECE48};

    // The dynamic sound's call of slot +0x68 (holder, out = rbp+0x30, the chain,
    // its length, the top-level sound) and its reading of the answer from out:
    // a low half of 0 or 0xFFFFFFFF is no answer, and so is the null answer of
    // the resolvers, {u32 [null name], 0xFFFFF}.
    inline const sig::Signature kAnswerUse{"query_answer_use",
        "48 8B 01 48 8D 55 30 4C 89 4C 24 20 4C 8B C6 44 8B CB FF 50 68 8B 45 30 BE FF FF 0F 00 8B 0D ?? ?? ?? ?? FF "
        "C8 89 4D 40 89 75 44 83 F8 FD 77 32",
        0x2583108};
    inline const sig::Signature kNullName{"query_null_name", kAnswerUse, sig::Rel32{0x1F, 0x23}, "", 0x3BE6C58};

    constexpr std::uint32_t kMountHead = 0xA07A1994; // joaat("mount")
    constexpr std::size_t kHolderPedOffset = 0x4600;
    constexpr std::size_t kTopEntityOffset = 0x50; // the top-level sound's entity
    constexpr std::size_t kComponentRowBytes = 0x148;
    constexpr std::size_t kComponentHandleOffset = 0x9C;
    constexpr std::uint32_t kComponentSlotMask = 0x1FFFF;
    constexpr std::uint32_t kMaxSlot = 0x10000;
    constexpr std::size_t kRowRideSlot = 0x128;
    constexpr std::size_t kRideFlagsOffset = 0x10;
    constexpr std::uint32_t kRideMountedBit = 0x10;
    constexpr std::size_t kRideMountOffset = 0x178;
    constexpr std::uint32_t kMaxChain = 8;
    constexpr std::uint32_t kMaxPairs = 64;

    // The ped's row in the component table (0 when the chain does not hold).
    // `pool`: the component table's globals (kMountLinkBaseIndex,
    // kMountLinkTable). read(address, out, size) -> bool: a guarded read.
    template <typename Read>
    std::uintptr_t ComponentRow(const enginesig::Pool& pool, std::uintptr_t ped, Read read)
    {
        if (!pool.Found() || !ped)
            return 0;
        std::uint32_t handle = 0;
        std::uint32_t indexBase = 0;
        std::uintptr_t table = 0;
        if (!read(ped + kComponentHandleOffset, &handle, sizeof(handle)) ||
            !read(pool.baseIndex, &indexBase, sizeof(indexBase)) || !read(pool.table, &table, sizeof(table)) ||
            !table)
        {
            return 0;
        }
        const std::uint32_t slot = (handle & kComponentSlotMask) - indexBase;
        if (slot >= kMaxSlot)
            return 0;
        return table + static_cast<std::uintptr_t>(slot) * kComponentRowBytes;
    }

    // The engine mount of `ped`, as the mount link reads it (kMountLink). 0:
    // not mounted.
    template <typename Read>
    std::uintptr_t EngineMountOf(const enginesig::Pool& pool, std::uintptr_t ped, Read read)
    {
        const std::uintptr_t row = ComponentRow(pool, ped, read);
        std::uintptr_t ride = 0;
        if (!row || !read(row + kRowRideSlot, &ride, sizeof(ride)) || !ride)
            return 0;
        ride &= ~std::uintptr_t{1};
        std::uint32_t flags = 0;
        if (!read(ride + kRideFlagsOffset, &flags, sizeof(flags)) || !(flags & kRideMountedBit))
            return 0;
        std::uintptr_t mount = 0;
        if (!read(ride + kRideMountOffset, &mount, sizeof(mount)))
            return 0;
        return mount;
    }

    inline bool NullAnswer(std::uint64_t answer, std::uint32_t nullName)
    {
        const std::uint32_t low = static_cast<std::uint32_t>(answer);
        return low == 0 || low == 0xFFFFFFFFu || low == nullName;
    }

    // Whether a published pair forwards this query: the chain starts with
    // "mount" and has a rest, the rider's engine link is empty and the
    // replay seats it on a horse puppet whose audio entity is a ped one.
    inline bool ForwardsToMount(std::uint32_t head, std::uint32_t n, std::uintptr_t engineMount,
        std::uintptr_t mountHolder, bool mountHolderIsPedAudio)
    {
        return head == kMountHead && n >= 2 && !engineMount && mountHolder && mountHolderIsPedAudio;
    }

    // One query, as the detour saw it (drained on the script thread).
    enum class Source : std::uint8_t
    {
        Engine = 0,   // the engine answered
        Relation = 1, // forwarded to the replay's mount (the engine's mount branch)
        Recorded = 2, // the recording's answer for that owner and chain (entity_query_track.h)
    };

    struct Query
    {
        std::uint64_t timeUs = 0;
        std::uint32_t thread = 0;
        std::uintptr_t holder = 0;
        std::uintptr_t top = 0;
        std::uint32_t n = 0;
        std::uint32_t chain[kMaxChain] = {};
        std::uint64_t answer = 0;
        std::uintptr_t engineMount = 0; // mount head only
        std::uintptr_t mountHolder = 0; // the published mount, mount head only
        Source source = Source::Engine;
        bool outermost = false;
        bool nullAnswer = false;
    };

    inline std::string ChainText(const Query& q)
    {
        std::string text;
        char part[16]{};
        const std::uint32_t shown = q.n < kMaxChain ? q.n : kMaxChain;
        for (std::uint32_t i = 0; i < shown; ++i)
        {
            std::snprintf(part, sizeof(part), "%s0x%08x", i ? ">" : "", q.chain[i]);
            text += part;
        }
        if (q.n > kMaxChain)
            text += ">...";
        return text.empty() ? "none" : text;
    }

    // Engine side (entity_query_hook.cpp).
    bool Install(std::string& error);
    void Shutdown();
    bool Installed();
    // The ring of queries (the log lines and the recording's answers).
    void SetLogging(bool on);
    void SetRelation(bool on);
    bool RelationOn();

    struct Pair
    {
        std::uintptr_t riderHolder = 0;
        std::uintptr_t mountHolder = 0;
    };
    // The riders the replay seats on a horse puppet (replaces the list).
    void SetPairs(const std::vector<Pair>& pairs);

    // The recorded answers in force for the puppets (replaces the table): an
    // outermost query of `holder` whose chain hash and length match gets
    // `answer` without the engine (EntityQueryReplay).
    struct RecordedAnswer
    {
        std::uintptr_t holder = 0;
        std::uint32_t hash = 0;
        std::uint32_t n = 0;
        std::uint64_t answer = 0;
    };
    constexpr std::uint32_t kMaxRecordedAnswers = 1024;
    void SetRecordedAnswers(const std::vector<RecordedAnswer>& answers);
    void SetReplayAnswers(bool on);
    bool ReplayAnswersOn();
    // EntityQueryRecord: the answers of the recorded owners into the clip.
    void SetRecordAnswers(bool on);
    bool RecordAnswersOn();
    // The queries since the last drain (oldest first); `dropped` counts the
    // ones the ring overwrote before this drain.
    void Drain(std::vector<Query>& out, std::uint64_t& dropped);
    std::uint32_t NullName();
    std::uint32_t ScriptThread();
    void SetScriptThread(std::uint32_t thread);

    struct Stats
    {
        std::uint64_t calls = 0;     // every call of the resolver
        std::uint64_t outermost = 0; // calls for the top-level sound's own entity
        std::uint64_t mountHead = 0; // outermost calls whose chain starts with "mount"
        std::uint64_t forwarded = 0; // of those, answered through the replay's mount
        std::uint64_t forwardedNull = 0; // forwarded and still null
        std::uint64_t engineNull = 0;    // outermost answers the engine left null
        std::uint64_t recorded = 0;      // answered from the recording
        std::uint64_t recordedNull = 0;  // of those, a recorded null answer
    };
    Stats Snapshot();
    void ResetStats();
}
