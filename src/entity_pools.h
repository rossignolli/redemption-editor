#pragma once

// The engine's entity pools read directly. The recorder walks the pools
// themselves, the way a replay recorder usually does, and never asks for
// a list of handles.
//
// Why: ScriptHookRDR2's worldGetAllPeds/Vehicles/Objects give every listed
// entity a script handle (an fwScriptGuid) and stop listing while that pool
// has 256 free slots or fewer (`cmp free, 0x100` in the export). In a town
// they return 0 most of the time and the recorder goes blind.
//
// Layout (build 1491.50, static analysis, signature-checked by Check):
// - rage::fwBasePool (ctor 0x2B052B4, GetAt 0x2B09388, Delete 0x2B079E0):
//   +0x08 storage, +0x10 flag bytes (one per slot: bit 0x80 free, the low
//   7 bits the slot's generation), +0x18 slot count (int), +0x1C entry size
//   (int), +0x20/+0x24 free list ends, +0x28 used count (low 30 bits). The
//   entity is the entry itself: storage + index * entry size. A pool
//   reference is (index << 8) | flag byte.
// - The pool pointers are stored encrypted: a flag byte at G-8 (0: no
//   pool), the value at G, a 64-bit key at G+8. Every reader decodes
//   pool = ~rol(rol(value ^ k, (k & 31) + b), 32), k = rol(ror(key, a), 32).
//   Peds ("Peds", entry 0x380, CPed allocations of 0x380 at 0xBFE2FA):
//   G = 0x4A11F70, a = 2, b = 3. Vehicles ("CVehicle", entry = the largest
//   vehicle class, allocations 0x10E0 to 0x1B80): G = 0x500D850, a = 1,
//   b = 4. Objects ("Object", entry 0xCD0, allocations 0x7A0 to 0xCD0):
//   G = 0x4643280, a = 2, b = 2. Script handles ("fwScriptGuid", entry
//   0x10: +0x08 the entity): G = 0x59B3FE8, a = 2, b = 3. The engine's own
//   pool switch 0x1D55D8 (type 1 vehicle, 2 ped, 3 object) holds the three
//   entity decoders; that function is the signature.
// - Entity +0x30: type byte (3 vehicle, 4 ped, 5 object: IS_ENTITY_A_*
//   0xF9A458..). Entity +0x70: position, x y z floats plus two int16 at
//   +0x7C/+0x7E that add 32 m steps to x and y (GET_ENTITY_COORDS through
//   0xFA4D2C, constant 0x330F370 = {32, 32, 0, 0}).
// - Entity to script handle: 0xEE5C3C (`jmp 0x2B07598`), the path the
//   natives take: returns the entity's handle, allocating one only when it
//   has none. Handle = (script guid slot << 8) | its flag byte.
//
// The walk reads memory only (guarded). A handle is made only for the
// entities a caller keeps (the nearest ones, the ones recorded), so the
// listing does not fill the script handle pool.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace replay::pools
{
    enum class Kind : std::uint8_t { Ped = 0, Vehicle = 1, Object = 2, Count = 3 };
    constexpr std::size_t kKinds = 3;

    // fwBasePool header.
    constexpr std::size_t kHeaderBytes = 0x30;
    constexpr std::size_t kHeaderStorage = 0x08;
    constexpr std::size_t kHeaderFlags = 0x10;
    constexpr std::size_t kHeaderSize = 0x18;
    constexpr std::size_t kHeaderEntrySize = 0x1C;
    constexpr std::size_t kHeaderUsed = 0x28;
    constexpr std::uint32_t kUsedMask = 0x3FFFFFFFu;
    constexpr std::uint8_t kFlagFree = 0x80;
    // Entity fields read.
    constexpr std::size_t kEntityType = 0x30;
    constexpr std::size_t kEntityPosition = 0x70; // 16 bytes
    constexpr float kPositionStep = 32.0f;
    constexpr std::uint8_t kTypeVehicle = 3;
    constexpr std::uint8_t kTypePed = 4;
    constexpr std::uint8_t kTypeObject = 5;
    // Sanity bounds for a decoded pool (a wrong decode gives garbage).
    constexpr std::int32_t kMaxSlots = 0x10000;

    struct PoolHeader
    {
        std::uint64_t storage = 0;
        std::uint64_t flags = 0;
        std::int32_t size = 0;
        std::int32_t entrySize = 0;
        std::uint32_t used = 0; // low 30 bits of +0x28
    };

    inline PoolHeader ParseHeader(const std::uint8_t* bytes)
    {
        PoolHeader header;
        std::memcpy(&header.storage, bytes + kHeaderStorage, 8);
        std::memcpy(&header.flags, bytes + kHeaderFlags, 8);
        std::memcpy(&header.size, bytes + kHeaderSize, 4);
        std::memcpy(&header.entrySize, bytes + kHeaderEntrySize, 4);
        std::uint32_t used = 0;
        std::memcpy(&used, bytes + kHeaderUsed, 4);
        header.used = used & kUsedMask;
        return header;
    }

    enum class HeaderStatus : std::uint8_t { Ok, NoStorage, NoFlags, BadSize, BadEntrySize, UsedOverSize };

    inline const char* HeaderStatusText(HeaderStatus status)
    {
        switch (status)
        {
        case HeaderStatus::Ok: return "ok";
        case HeaderStatus::NoStorage: return "no_storage";
        case HeaderStatus::NoFlags: return "no_flags";
        case HeaderStatus::BadSize: return "bad_size";
        case HeaderStatus::BadEntrySize: return "bad_entry_size";
        case HeaderStatus::UsedOverSize: return "used_over_size";
        }
        return "?";
    }

    // A decoded pool must look like one: storage and flags set, a slot count
    // in range, the entry size of its class, no more used slots than slots.
    inline HeaderStatus ValidateHeader(const PoolHeader& header, std::int32_t minEntry, std::int32_t maxEntry)
    {
        if (!header.storage)
            return HeaderStatus::NoStorage;
        if (!header.flags)
            return HeaderStatus::NoFlags;
        if (header.size <= 0 || header.size > kMaxSlots)
            return HeaderStatus::BadSize;
        if (header.entrySize < minEntry || header.entrySize > maxEntry)
            return HeaderStatus::BadEntrySize;
        if (header.used > static_cast<std::uint32_t>(header.size))
            return HeaderStatus::UsedOverSize;
        return HeaderStatus::Ok;
    }

    inline std::uint64_t Rol(std::uint64_t value, unsigned bits)
    {
        bits &= 63u;
        return bits ? (value << bits) | (value >> (64u - bits)) : value;
    }
    inline std::uint64_t Ror(std::uint64_t value, unsigned bits)
    {
        bits &= 63u;
        return bits ? (value >> bits) | (value << (64u - bits)) : value;
    }

    // One pool pointer's cipher (the rotation `a` of the key and the base
    // `b` of the value's rotation; see the header).
    struct Cipher
    {
        unsigned keyRotate = 0;
        unsigned shiftBase = 0;
    };

    // The engine's reader (0x1D5653.. for objects, the others alike).
    inline std::uint64_t DecodePointer(std::uint64_t value, std::uint64_t key, Cipher cipher)
    {
        const std::uint64_t k = Rol(Ror(key, cipher.keyRotate), 32);
        const unsigned shift = static_cast<unsigned>(k & 31u) + cipher.shiftBase;
        return ~Rol(Rol(value ^ k, shift), 32);
    }

    // The engine's writer (the pool creators 0xC0992C, 0x15A354F,
    // 0x81BC50): the inverse, for the tests.
    inline std::uint64_t EncodePointer(std::uint64_t pool, std::uint64_t key, Cipher cipher)
    {
        const std::uint64_t k = Rol(Ror(key, cipher.keyRotate), 32);
        const unsigned shift = static_cast<unsigned>(k & 31u) + cipher.shiftBase;
        return Ror(Rol(~pool, 32), shift) ^ k;
    }

    inline bool SlotUsed(std::uint8_t flag) { return (flag & kFlagFree) == 0; }
    inline std::int32_t PoolRef(std::int32_t index, std::uint8_t flag) { return (index << 8) | flag; }

    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Entity +0x70..+0x7F (the reader at 0xFA4D2C).
    inline Position DecodePosition(const std::uint8_t* bytes)
    {
        float xyz[3]{};
        std::int16_t step[2]{};
        std::memcpy(xyz, bytes, 12);
        std::memcpy(step, bytes + 12, 4);
        return {xyz[0] + static_cast<float>(step[0]) * kPositionStep,
            xyz[1] + static_cast<float>(step[1]) * kPositionStep, xyz[2]};
    }

    // A live entity found in a pool.
    struct Entry
    {
        std::uint64_t address = 0;
        std::int32_t ref = 0; // (slot << 8) | flag
        std::uint8_t type = 0;
        Position position;
    };

    struct WalkStats
    {
        std::uint32_t slots = 0;     // slots looked at
        std::uint32_t used = 0;      // slots flagged used
        std::uint32_t wrongType = 0; // used slots whose type byte is not the pool's
        std::uint32_t unreadable = 0;
        std::uint32_t truncated = 0; // live entries past the output capacity
    };

    // Walks the slots of a pool whose header and flag bytes were read.
    // `read(address, type, position16)` reads one entity's type byte and its
    // 16 position bytes; false when unreadable (skipped). `expectedType` 0
    // accepts any type.
    template <class Read>
    int WalkPool(const PoolHeader& header, const std::uint8_t* flags, std::int32_t flagCount, Read&& read,
        std::uint8_t expectedType, Entry* out, int capacity, WalkStats* stats = nullptr)
    {
        WalkStats local;
        WalkStats& s = stats ? *stats : local;
        s = {};
        if (!flags || header.size <= 0 || header.entrySize <= 0)
            return 0;
        const std::int32_t slots = (std::min)((std::min)(header.size, flagCount), kMaxSlots);
        int count = 0;
        for (std::int32_t index = 0; index < slots; ++index)
        {
            ++s.slots;
            const std::uint8_t flag = flags[index];
            if (!SlotUsed(flag))
                continue;
            ++s.used;
            const std::uint64_t address =
                header.storage + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(header.entrySize);
            std::uint8_t type = 0;
            std::uint8_t position[16]{};
            if (!read(address, type, position))
            {
                ++s.unreadable;
                continue;
            }
            if (expectedType && type != expectedType)
            {
                ++s.wrongType;
                continue;
            }
            if (count >= capacity)
            {
                ++s.truncated;
                continue;
            }
            Entry& entry = out[count++];
            entry.address = address;
            entry.ref = PoolRef(index, flag);
            entry.type = type;
            entry.position = DecodePosition(position);
        }
        return count;
    }

    // The script handle pool walked read-only: every entity that already
    // has a handle. `readTarget(entryAddress, target)` reads entry +0x08.
    struct GuidEntry
    {
        std::uint64_t entity = 0;
        std::int32_t handle = 0;
    };

    template <class ReadTarget>
    int WalkGuids(const PoolHeader& header, const std::uint8_t* flags, std::int32_t flagCount, ReadTarget&& readTarget,
        GuidEntry* out, int capacity)
    {
        if (!flags || header.size <= 0 || header.entrySize <= 0)
            return 0;
        const std::int32_t slots = (std::min)((std::min)(header.size, flagCount), kMaxSlots);
        int count = 0;
        for (std::int32_t index = 0; index < slots && count < capacity; ++index)
        {
            const std::uint8_t flag = flags[index];
            if (!SlotUsed(flag))
                continue;
            std::uint64_t target = 0;
            const std::uint64_t address =
                header.storage + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(header.entrySize);
            if (!readTarget(address, target) || !target)
                continue;
            out[count++] = {target, PoolRef(index, flag)};
        }
        std::sort(out, out + count, [](const GuidEntry& a, const GuidEntry& b) { return a.entity < b.entity; });
        return count;
    }

    // The handle an entity already has (WalkGuids output, sorted), 0 if none.
    inline std::int32_t FindGuid(const GuidEntry* guids, int count, std::uint64_t entity)
    {
        const GuidEntry* end = guids + count;
        const GuidEntry* found = std::lower_bound(guids, end, entity,
            [](const GuidEntry& entry, std::uint64_t value) { return entry.entity < value; });
        return found != end && found->entity == entity ? found->handle : 0;
    }

    // The order the entries are handed out in: nearest to any centre first.
    inline float NearestDistance2(const Position& at, const Position* centers, int centerCount)
    {
        float best = 3.4e38f;
        for (int n = 0; n < centerCount; ++n)
        {
            const float dx = at.x - centers[n].x, dy = at.y - centers[n].y, dz = at.z - centers[n].z;
            best = (std::min)(best, dx * dx + dy * dy + dz * dz);
        }
        return best;
    }

    // Which entries get a handle (pure part of ListHandles): those that have
    // one (`hasHandle`) within `existingRadius`, and those that do not only
    // within `radius`, nearest first, at most `createBudget` of them. All in
    // order of distance, so a capacity cut drops the farthest. Returns the
    // indices into `entries`, in the order to hand out.
    template <class HasHandle>
    void PlanHandles(const Entry* entries, int count, const Position* centers, int centerCount, float radius,
        float existingRadius, int createBudget, HasHandle&& hasHandle, std::vector<int>& order, std::vector<float>& distance2,
        std::uint32_t* skippedFar = nullptr, std::uint32_t* skippedBudget = nullptr)
    {
        order.clear();
        distance2.assign(static_cast<std::size_t>(count), 0.0f);
        for (int n = 0; n < count; ++n)
        {
            distance2[static_cast<std::size_t>(n)] = NearestDistance2(entries[n].position, centers, centerCount);
            order.push_back(n);
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return distance2[static_cast<std::size_t>(a)] < distance2[static_cast<std::size_t>(b)];
        });
        const float radius2 = radius * radius;
        const float existing2 = existingRadius * existingRadius;
        int created = 0;
        std::size_t kept = 0;
        for (const int index : order)
        {
            const float d2 = distance2[static_cast<std::size_t>(index)];
            bool keep = false;
            if (hasHandle(entries[index]))
            {
                keep = d2 <= existing2;
                if (!keep && skippedFar)
                    ++*skippedFar;
            }
            else
            {
                if (d2 > radius2)
                {
                    if (skippedFar)
                        ++*skippedFar;
                }
                else if (created >= createBudget)
                {
                    if (skippedBudget)
                        ++*skippedBudget;
                }
                else
                {
                    ++created;
                    keep = true;
                }
            }
            if (keep)
                order[kept++] = index;
        }
        order.resize(kept);
    }

    // ----- Engine side (entity_pools.cpp) -----

    // Checks the build once (the pool switch with the three decoders, the
    // handle path, the position reader, the type predicates, GetAt and
    // Delete) and decodes the pools. Later calls return the first result.
    bool Check(std::string& error);
    // Check passed and the ini switch (EntityPoolReader, default 1) is on.
    bool Available();
    void SetEnabled(bool enabled); // the ini switch
    bool Enabled();

    struct PoolState
    {
        bool ok = false;
        std::int32_t size = 0;
        std::int32_t entrySize = 0;
        std::uint32_t used = 0;
        HeaderStatus status = HeaderStatus::NoStorage;
    };
    // The live header of a pool (decoded now, every call; the engine may
    // move nothing, but a check costs a few reads).
    PoolState State(Kind kind);
    // The script handle pool (fwScriptGuid): size and used slots.
    PoolState GuidState();

    // Reads every live entity of a pool. Returns how many (0 when
    // unavailable or unreadable).
    int Read(Kind kind, Entry* out, int capacity, WalkStats* stats = nullptr);

    // The entity's script handle; makes one (the natives' path) when it has
    // none and `create`, unless the handle pool has `GuidReserve` free slots
    // or fewer. `ref` is re-checked first (the slot still holds the same
    // entity), so an entity deleted since the walk is never touched.
    // The reserve left to the game is a quarter of the pool (362 of 1450):
    // recording near a town and teleporting into it can take the pool from
    // 844 free to 54 within a second (new handles for the town's objects, 96
    // a frame), and the game dies with ERROR:FFFFFFFF when the pool runs out.
    constexpr std::uint32_t kGuidReserveMin = 64;
    inline std::uint32_t GuidReserve(std::int32_t poolSize)
    {
        const std::uint32_t quarter = poolSize > 0 ? static_cast<std::uint32_t>(poolSize) / 4u : 0u;
        return quarter > kGuidReserveMin ? quarter : kGuidReserveMin;
    }
    // Free script handle slots beyond the reserve (0 when the pool is
    // unreadable): how many handles the mod may still make.
    std::uint32_t GuidRoom();
    std::int32_t HandleOf(Kind kind, const Entry& entry, bool create);

    struct ListStats
    {
        std::uint32_t live = 0;        // live entries of the pool
        std::uint32_t listed = 0;      // handles returned
        std::uint32_t existing = 0;    // of those, already with a handle
        std::uint32_t created = 0;     // handles made now
        std::uint32_t skippedFar = 0;  // without a handle, beyond the radius
        std::uint32_t skippedBudget = 0;
        std::uint32_t failed = 0;      // HandleOf returned 0
        std::uint32_t guidFree = 0;    // script handle pool free slots after
        std::uint32_t guidSize = 0;
        double us = 0.0;
    };

    // Handles of a pool's entities, nearest to a centre first: those with a
    // handle already within `existingRadius`, the others within `radius` (at
    // most `createBudget` new handles). -1 when unavailable (the caller
    // falls back to the listing).
    int ListHandles(Kind kind, const Position* centers, int centerCount, float radius, float existingRadius,
        int createBudget, int* handles, int capacity, ListStats* stats = nullptr);

    // Evidence for the logs: one line at warm up.
    std::string Describe();

    // The script handle pool's header decoded now (the per-frame entity
    // cache resolves handles against it; see entity_cache.h). False when
    // the reader is unavailable or the header does not validate.
    bool GuidHeader(PoolHeader& header);
    // The live entity at an engine address: its type byte picks the pool,
    // and the address must be a used slot of that pool (storage + index *
    // entry size, flag byte used). No engine call. False otherwise.
    bool EntryAt(std::uint64_t address, Kind& kind, Entry& entry);
}
