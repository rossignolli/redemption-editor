#pragma once

// The checks the replay makes before it hands a decal to the rage decal
// core 0x2C2EF08 (directly, AddDecalWithId, or through the generic wrapper
// 0x2C2F054, AddDecal). Pure logic; the guarded reads live in
// decal_observer.cpp.
//
// - Info (settings +0x88). The pending ring's worker 0x2C42C18 reads
//   [info+0x72] with no null check (a crash at 0x2C42D1A). Each of the 11
//   callers of the info lookup 0x2C3A3D8 refuses a null result and a
//   record whose qword +0x130 is 0 (e.g. 0x125BA82..0x125BA93) before it
//   adds.
// - Id (AddDecalWithId). The core ends with `lock add word [mgr + slot*2 +
//   0x2148], 1`, slot = (id - 0x10000) >> 16, with no range or liveness
//   check. The id table has 0xC00 slots (generation words at mgr+0x948,
//   free-list links / counters at mgr+0x2148, head at mgr+0x3948; allocator
//   0x2C2F734, alive test 0x2C3DD04 compares the generation word with the
//   id's low word). A dead id bumps a free-list link (the allocator then
//   hands out a wrong slot); a slot past the table adds 1 to whatever
//   manager field lies there, the request ring included (mgr+0xB5D0).
// - Ring entry (a guarded fault at 0x15E5BD writing 0x9). The core takes
//   entry (cursor [mgr+0xD75D0] + 1) mod 0x400 of stride 0x330 at
//   mgr+0xB5D0 and assigns the entity ref at entry+0x310 through 0x15EDE0,
//   which first unlinks the previous entity at entry+0x318 (0x15E544 writes
//   [old+8]). A corrupted previous value (1 was seen) faults there, so a
//   non-null previous value that is not a readable pointer is refused.

#include <cstdint>

namespace replay::decal::guard
{
    constexpr std::uint32_t kIdSlots = 0xC00;
    constexpr std::uintptr_t kIdGenerationOffset = 0x948;
    constexpr std::uintptr_t kRingOffset = 0xB5D0;
    constexpr std::uintptr_t kRingCursorOffset = 0xD75D0;
    constexpr std::uint32_t kRingEntries = 0x400;
    constexpr std::uintptr_t kRingStride = 0x330;
    constexpr std::uintptr_t kRingEntityOffset = 0x318;
    constexpr std::uintptr_t kInfoDataOffset = 0x130;
    constexpr std::uint64_t kLowestPointer = 0x10000;

    // The slot of an id, or -1 when it has none in the table.
    inline std::int32_t IdSlot(std::uint32_t id)
    {
        if (id < 0x10000u)
            return -1;
        const std::uint32_t slot = (id >> 16) - 1;
        return slot < kIdSlots ? static_cast<std::int32_t>(slot) : -1;
    }

    // Alive the way 0x2C3DD04 tests it: the slot's generation word is the
    // id's low word.
    inline bool IdAlive(std::uint32_t id, std::uint16_t generation)
    {
        return IdSlot(id) >= 0 && generation == static_cast<std::uint16_t>(id & 0xFFFF);
    }

    enum class InfoVerdict : std::uint8_t
    {
        Ok,
        Null,        // settings +0x88 is 0 (the worker would read [0+0x72])
        Unreadable,  // the record cannot be read
        NoData,      // qword +0x130 is 0: the game's callers refuse it too
    };

    inline InfoVerdict JudgeInfo(std::uint64_t info, bool dataRead, std::uint64_t data)
    {
        if (!info)
            return InfoVerdict::Null;
        if (info < kLowestPointer || !dataRead)
            return InfoVerdict::Unreadable;
        return data ? InfoVerdict::Ok : InfoVerdict::NoData;
    }

    inline const char* InfoVerdictText(InfoVerdict verdict)
    {
        switch (verdict)
        {
        case InfoVerdict::Ok: return "ok";
        case InfoVerdict::Null: return "info_null";
        case InfoVerdict::Unreadable: return "info_unreadable";
        case InfoVerdict::NoData: return "info_no_data";
        }
        return "?";
    }

    // The ring entry the next add takes (the core's own wrap).
    inline std::uint32_t NextRingIndex(std::uint32_t cursor)
    {
        const std::uint32_t next = cursor + 1;
        return next >= kRingEntries ? 0 : next;
    }

    inline std::uintptr_t RingEntityAddress(std::uintptr_t manager, std::uint32_t index)
    {
        return manager + kRingOffset + static_cast<std::uintptr_t>(index) * kRingStride + kRingEntityOffset;
    }

    // The previous entity of the entry is safe to unlink: none, or a pointer
    // whose +8 the unlink can write (readable is the caller's guarded read).
    inline bool RingRefPlausible(std::uint64_t previous, bool readable)
    {
        if (!previous)
            return true;
        return previous >= kLowestPointer && (previous & 0x7) == 0 && readable;
    }

    // The refusals, by reason (POSE_V2_DECAL_GUARD and the playback summary).
    struct Refusals
    {
        std::uint64_t infoNull = 0;
        std::uint64_t infoUnreadable = 0;
        std::uint64_t infoNoData = 0;
        std::uint64_t idOutOfRange = 0;
        std::uint64_t idDead = 0;
        std::uint64_t ringStale = 0;
        std::uint64_t Info() const { return infoNull + infoUnreadable + infoNoData; }
        std::uint64_t Total() const { return Info() + idOutOfRange + idDead + ringStale; }
    };

    inline void Count(Refusals& refusals, InfoVerdict verdict)
    {
        if (verdict == InfoVerdict::Null)
            ++refusals.infoNull;
        else if (verdict == InfoVerdict::Unreadable)
            ++refusals.infoUnreadable;
        else if (verdict == InfoVerdict::NoData)
            ++refusals.infoNoData;
    }
}
