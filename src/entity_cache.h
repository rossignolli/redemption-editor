#pragma once

// The per-frame entity cache.
//
// Asking the ScriptHook the same questions about the same entities every
// frame from several modules (DOES_ENTITY_EXIST, getScriptHandleBaseAddress,
// IS_ENTITY_A_PED, GET_ENTITY_COORDS, _HAS_PED_TAKEN_GORE_DAMAGE x9) costs
// ~1,700 to 2,000 calls per recording frame with 56 peds. A replay recorder
// is better off reading its entities in one pass over the engine's pools.
//
// Here: the script handle pool (fwScriptGuid, entity_pools.h) is decoded
// once per script tick (BeginFrame); a handle then resolves the way the
// engine's GetAt 0x2B09388 does, by reading memory only: the slot's flag
// byte must equal the handle's low byte, the guid's +0x08 is the entity,
// the entity's +0x30 its type and +0x70 its position (all signature-checked
// by pools::Check). Every answer is read fresh (three or four guarded
// reads, no ScriptHook call), so an entity deleted earlier in the same
// frame is already gone: the only thing kept across a frame is the decoded
// header. Values that only a native gives (frozen) are cached per frame in
// a table that the code which changes them updates.
//
// Safety: every read is a guarded copy; the first 512 resolves of a
// session, and one in 64 after that, are compared with the natives
// (DOES_ENTITY_EXIST, getScriptHandleBaseAddress, GET_ENTITY_COORDS); three
// mismatches switch the memory path off for the session (log line
// ENTITY_CACHE,status=disabled) and everything goes back to the natives.
// Off the script thread, or with EntityPoolReader=0, the natives are used.
//
// Gore: the engine's per-ped gore entry (table pointer at 0x497CB80, u16 count
// at 0x497CB88, entries of 0x20: +0x00 ped, +0x18 mask) read once per frame;
// _HAS_PED_TAKEN_GORE_DAMAGE's core 0xFD66F4 is `bt [entry+0x18], limb`, i.e.
// bit (limb & 31) of the 32-bit mask (limb 37 is bit 5). Getter 0xB4498C and
// core 0xFD66F4 signature-checked.
//
// The pure parts below (the guid resolve, the cross-check policy, the
// frame table, the gore table) are unit tested (tests/entity_cache_tests.cpp).

#include "entity_pools.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::entities
{
    constexpr std::size_t kGuidEntity = 0x08; // fwScriptGuid +0x08: the entity

    // The engine's GetAt on the script guid pool, then the guid's target.
    // `readByte(address, out)` and `readQword(address, out)` are guarded
    // reads. 0 when the handle does not name a live guid.
    template <class ReadByte, class ReadQword>
    std::uint64_t ResolveGuid(const pools::PoolHeader& guids, std::int32_t handle, ReadByte&& readByte, ReadQword&& readQword)
    {
        if (handle < 0 || !guids.storage || !guids.flags || guids.size <= 0 ||
            guids.entrySize < static_cast<std::int32_t>(kGuidEntity + 8))
            return 0;
        const std::int32_t index = handle >> 8;
        if (index >= guids.size)
            return 0;
        std::uint8_t flag = pools::kFlagFree;
        if (!readByte(guids.flags + static_cast<std::uint64_t>(index), flag))
            return 0;
        // GetAt compares the slot's byte with the handle's low byte; a free
        // slot (bit 0x80) never matches a handle the engine gave out.
        if (flag != static_cast<std::uint8_t>(handle & 0xFF) || !pools::SlotUsed(flag))
            return 0;
        std::uint64_t entity = 0;
        const std::uint64_t entry = guids.storage +
            static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(guids.entrySize);
        if (!readQword(entry + kGuidEntity, entity))
            return 0;
        return entity;
    }

    // Which memory answers are compared with the natives: the first
    // kFirst, then one in kEvery; kMaxMismatches mismatches disable the
    // memory path.
    struct CrossCheck
    {
        static constexpr std::uint32_t kFirst = 512;
        static constexpr std::uint32_t kEvery = 64;
        static constexpr std::uint32_t kMaxMismatches = 3;
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint32_t mismatches = 0;
        bool disabled = false;

        bool Due()
        {
            ++calls;
            return !disabled && (calls <= kFirst || calls % kEvery == 0);
        }
        // Returns true when this mismatch disabled the path.
        bool Note(bool match)
        {
            ++checked;
            if (match || disabled)
                return false;
            if (++mismatches >= kMaxMismatches)
            {
                disabled = true;
                return true;
            }
            return false;
        }
        void Reset() { *this = {}; }
    };

    // Values only a native gives, cached for one frame. The frame number
    // is the caller's (a script tick counter); a new frame empties it.
    template <class Value>
    class FrameTable
    {
    public:
        void BeginFrame(std::uint64_t frame)
        {
            if (frame == m_frame)
                return;
            m_frame = frame;
            m_values.clear();
        }
        const Value* Find(std::int32_t handle) const
        {
            const auto found = m_values.find(handle);
            return found == m_values.end() ? nullptr : &found->second;
        }
        void Put(std::int32_t handle, const Value& value) { m_values[handle] = value; }
        void Forget(std::int32_t handle) { m_values.erase(handle); }
        void Clear() { m_values.clear(); }
        std::size_t Size() const { return m_values.size(); }
        std::uint64_t Frame() const { return m_frame; }

    private:
        std::uint64_t m_frame = ~0ull;
        std::unordered_map<std::int32_t, Value> m_values;
    };

    // ----- Gore table -----
    constexpr std::size_t kGoreEntryBytes = 0x20;
    constexpr std::size_t kGoreEntryPed = 0x00;
    constexpr std::size_t kGoreEntryMask = 0x18;
    constexpr std::uint32_t kGoreMaxEntries = 4096;

    struct GoreEntry
    {
        std::uint64_t ped = 0;
        std::uint32_t mask = 0; // the whole dword at +0x18
        std::uint32_t order = 0; // position in the engine's table
    };

    // The table sorted by ped, ties in table order: the engine's getter
    // returns the first entry of a ped, and so does GoreMaskOf.
    inline void SortGore(std::vector<GoreEntry>& table)
    {
        std::sort(table.begin(), table.end(), [](const GoreEntry& a, const GoreEntry& b) {
            return a.ped != b.ped ? a.ped < b.ped : a.order < b.order;
        });
    }

    // Parses `count` raw entries (count * 0x20 bytes) into a sorted table.
    inline void ParseGore(const std::uint8_t* bytes, std::uint32_t count, std::vector<GoreEntry>& table)
    {
        table.clear();
        for (std::uint32_t n = 0; n < count; ++n)
        {
            GoreEntry entry;
            std::memcpy(&entry.ped, bytes + n * kGoreEntryBytes + kGoreEntryPed, 8);
            std::memcpy(&entry.mask, bytes + n * kGoreEntryBytes + kGoreEntryMask, 4);
            entry.order = n;
            if (entry.ped)
                table.push_back(entry);
        }
        SortGore(table);
    }

    // The mask of a ped (false when the ped has no entry: the native then
    // answers false for every limb).
    inline bool GoreMaskOf(const std::vector<GoreEntry>& table, std::uint64_t ped, std::uint32_t& mask)
    {
        mask = 0;
        const auto found = std::lower_bound(table.begin(), table.end(), ped,
            [](const GoreEntry& entry, std::uint64_t value) { return entry.ped < value; });
        if (found == table.end() || found->ped != ped)
            return false;
        mask = found->mask;
        return true;
    }

    // _HAS_PED_TAKEN_GORE_DAMAGE's core 0xFD66F4: `bt eax, ebx` on the
    // dword, so the bit is limb mod 32.
    inline bool GoreHas(std::uint32_t mask, int limb)
    {
        return ((mask >> (static_cast<unsigned>(limb) & 31u)) & 1u) != 0;
    }

    // ----- Engine side (entity_cache.cpp), script thread -----

    // Once per script tick, first thing: decodes the guid pool header and
    // starts a new frame of the native-value table.
    void BeginFrame();
    // Signature checks (pools::Check plus the gore getter) and the log line.
    std::string Describe();

    bool Exists(std::int32_t handle);
    // getScriptHandleBaseAddress through memory (the natives' result).
    std::uint8_t* AddressOf(std::int32_t handle);
    bool IsPed(std::int32_t handle);
    bool IsVehicle(std::int32_t handle);
    // GET_ENTITY_COORDS(handle, TRUE, FALSE) through memory. False (and
    // zero) when the entity does not exist.
    bool Position(std::int32_t handle, float out[3]);

    // _IS_ENTITY_FROZEN, one native per entity per frame; NoteFrozen after
    // FREEZE_ENTITY_POSITION keeps the table right for the rest of the frame.
    bool Frozen(std::int32_t handle);
    void NoteFrozen(std::int32_t handle, bool frozen);

    // The script handle of the entity at an engine address, through the
    // pools (0 when none of the three pools holds it). `found` false means
    // the pool reader is not available and the caller must use its listing.
    std::int32_t HandleAtAddress(std::uint64_t address, bool& available);

    // Handles of the entities of a pool within `radius` of a point, read
    // from the pool (handles made only for those). -1 when unavailable.
    int ListNear(pools::Kind kind, const float center[3], float radius, int* handles, int capacity);
    // Every live entity of a pool with its position (no handle made; a
    // handle for one of them: pools::HandleOf). -1 when unavailable.
    int ReadPool(pools::Kind kind, std::vector<pools::Entry>& out);

    // Gore mask of a ped (engine address) from this frame's table read.
    // `available` false when the gore table is not usable (signature or
    // cross-check): the caller polls the native.
    bool GoreMask(const void* ped, std::uint32_t& mask, bool& available);
    // Cross-check one memory gore answer with the native (the caller asks).
    bool GoreCheckDue();
    void GoreNote(bool match, std::int32_t handle, int limb);

    struct Stats
    {
        std::uint64_t frames = 0;
        std::uint64_t memoryResolves = 0;
        std::uint64_t nativeResolves = 0;
        std::uint64_t positions = 0;
        std::uint64_t frozenNatives = 0;
        std::uint64_t frozenHits = 0;
        std::uint64_t goreReads = 0;
        std::uint64_t checked = 0;
        std::uint32_t mismatches = 0;
        // Differences the engine proved to be the memory path's right answer (a
        // handle the ScriptHook export still resolved after the entity was gone;
        // a mounted or seated ped's own position).
        std::uint64_t exportStale = 0;
        std::uint64_t positionReal = 0;
        bool memory = false;
        bool gore = false;
    };
    Stats Snapshot();
    // One ENTITY_CACHE_TOTAL line (recording or playback stop).
    std::string SummaryLine(const char* phase);
}
