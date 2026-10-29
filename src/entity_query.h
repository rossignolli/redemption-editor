#pragma once

// The sounds that depend on their owner's state.
//
// A dynamic entity sound (metadata type 0x10) picks its child when it plays, by
// asking the owner's audio entity a chain of names (0x2583044 calls the entity's
// vtable slot +0x68 with the chain, its length and the top-level sound, at
// 0x258311A). For every ped, horse and animal that slot is 0x31947D0 (vtables
// 0x3839538, 0x3839630, 0x3839728). It switches on the head of the chain and
// reads the owner's live relations: "mount" (0xA07A1994) asks the engine mount
// link (0xCC857C: component row +0x128, flags +0x10 bit 4, mount +0x178) and
// hands the rest of the chain to the mount's own audio entity (0x3194E3E); the
// boat and vehicle heads, the weapon, the clothing caches, the map material and
// two task heads read their own state. A replay puppet has none of that state,
// so every such chain answers the null sound, the sound has no child, and the
// engine ends it 2 to 3 frames later (saddle and stirrup foley cut short).
//
// The relation is given to the resolver only: when a puppet's chain starts with
// "mount", its engine mount link is empty and the replay seats it on a horse
// puppet (the rider and mount pairs of the pose playback), the rest of the chain
// goes to that horse puppet's audio entity, exactly the engine's own call at
// 0x3194E3E. No task, link, physics or pose changes.
//
// The detour also logs the outermost queries (rider, chain, answer, thread).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::entityquery
{
    // Game build 1491.50.

    constexpr std::uintptr_t kResolverRva = 0x31947D0;
    constexpr std::uintptr_t kHolderVtableRvas[] = {0x3839538, 0x3839630, 0x3839728};
    constexpr std::uint32_t kMountHead = 0xA07A1994; // joaat("mount")
    constexpr std::size_t kHolderPedOffset = 0x4600;
    constexpr std::size_t kTopEntityOffset = 0x50; // the top-level sound's entity
    constexpr std::uintptr_t kComponentTableRva = 0x39ECE48;
    constexpr std::uintptr_t kComponentIndexBaseRva = 0x39ECE40;
    constexpr std::size_t kComponentRowBytes = 0x148;
    constexpr std::size_t kComponentHandleOffset = 0x9C;
    constexpr std::uint32_t kComponentSlotMask = 0x1FFFF;
    constexpr std::uint32_t kMaxSlot = 0x10000;
    constexpr std::size_t kRowRideSlot = 0x128;
    constexpr std::size_t kRideFlagsOffset = 0x10;
    constexpr std::uint32_t kRideMountedBit = 0x10;
    constexpr std::size_t kRideMountOffset = 0x178;
    // The null answer of the resolvers: {u32 [0x3BE6C58], 0xFFFFF}; 0x2583044
    // also takes a low half of 0 or 0xFFFFFFFF as no answer.
    constexpr std::uintptr_t kNullNameRva = 0x3BE6C58;
    constexpr std::uint32_t kMaxChain = 8;
    constexpr std::uint32_t kMaxPairs = 64;

    // The ped's row in the component table (0 when the chain does not hold).
    // read(address, out, size) -> bool: a guarded read.
    template <typename Read>
    std::uintptr_t ComponentRow(std::uintptr_t base, std::uintptr_t ped, Read read)
    {
        if (!base || !ped)
            return 0;
        std::uint32_t handle = 0;
        std::uint32_t indexBase = 0;
        std::uintptr_t table = 0;
        if (!read(ped + kComponentHandleOffset, &handle, sizeof(handle)) ||
            !read(base + kComponentIndexBaseRva, &indexBase, sizeof(indexBase)) ||
            !read(base + kComponentTableRva, &table, sizeof(table)) || !table)
        {
            return 0;
        }
        const std::uint32_t slot = (handle & kComponentSlotMask) - indexBase;
        if (slot >= kMaxSlot)
            return 0;
        return table + static_cast<std::uintptr_t>(slot) * kComponentRowBytes;
    }

    // The engine mount of `ped`, as 0xCC857C reads it (IS_PED_ON_MOUNT's
    // handler 0xFB89A8 is exactly this != 0). 0: not mounted.
    template <typename Read>
    std::uintptr_t EngineMountOf(std::uintptr_t base, std::uintptr_t ped, Read read)
    {
        const std::uintptr_t row = ComponentRow(base, ped, read);
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
    // EntityQueryLog: the ENTITY_QUERY lines of the clip's owners (default on).
    void SetLogWanted(bool on);
    bool LogWanted();

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
        std::uint64_t calls = 0;     // every call of 0x31947D0
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
