// The entity pool reader's pure logic on synthetic memory: the pool header,
// the pointer cipher against the engine's instruction sequence, free slots,
// bounds, positions, the script handle walk and the plan of which entities
// get a handle.
#include "entity_pools.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    int g_poolFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_poolFailures;
    }

    using namespace replay::pools;

    // The decoders exactly as the engine's instructions run them (pool
    // switch 0x1D55D8): the compact DecodePointer must match each.
    std::uint64_t EngineDecodePed(std::uint64_t value, std::uint64_t key)
    {
        // rol rcx,0x20; ror rcx,2; xor rdx,rcx; eax=(ecx&31)+3; rol rdx,cl; rol rdx,0x20; not
        std::uint64_t rcx = Rol(key, 32);
        rcx = Ror(rcx, 2);
        std::uint64_t rdx = value ^ rcx;
        const unsigned cl = static_cast<unsigned>(rcx & 31u) + 3u;
        rdx = Rol(rdx, cl);
        rdx = Rol(rdx, 32);
        return ~rdx;
    }
    std::uint64_t EngineDecodeVehicle(std::uint64_t value, std::uint64_t key)
    {
        // ror rcx,1; rol rcx,0x20; eax=(ecx&31)+4; xor rdx,rcx; rol rdx,0x20; rol rdx,cl; not
        std::uint64_t rcx = Ror(key, 1);
        rcx = Rol(rcx, 32);
        const unsigned cl = static_cast<unsigned>(rcx & 31u) + 4u;
        std::uint64_t rdx = value ^ rcx;
        rdx = Rol(rdx, 32);
        rdx = Rol(rdx, cl);
        return ~rdx;
    }
    std::uint64_t EngineDecodeObject(std::uint64_t value, std::uint64_t key)
    {
        std::uint64_t rcx = Ror(key, 2);
        rcx = Rol(rcx, 32);
        const unsigned cl = static_cast<unsigned>(rcx & 31u) + 2u;
        std::uint64_t rdx = value ^ rcx;
        rdx = Rol(rdx, 32);
        rdx = Rol(rdx, cl);
        return ~rdx;
    }
    // The vehicle pool's writer (0x15A354F): not rdx; rol rdx,0x20;
    // r8 = rol(ror(key,1),0x20); ror rdx,(r8&31)+4; xor rdx,r8.
    std::uint64_t EngineEncodeVehicle(std::uint64_t pool, std::uint64_t key)
    {
        std::uint64_t rdx = ~pool;
        rdx = Rol(rdx, 32);
        std::uint64_t r8 = Ror(key, 1);
        r8 = Rol(r8, 32);
        rdx = Ror(rdx, static_cast<unsigned>(r8 & 31u) + 4u);
        return rdx ^ r8;
    }

    void TestCipher()
    {
        const Cipher ped{2, 3}, vehicle{1, 4}, object{2, 2};
        std::uint64_t seed = 0x9E3779B97F4A7C15ull;
        for (int n = 0; n < 2000; ++n)
        {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            const std::uint64_t key = seed ^ (seed >> 29);
            const std::uint64_t pool = 0x00000200'00000000ull + ((seed >> 7) & 0xFFFFFFFF0ull);
            const std::uint64_t encPed = EncodePointer(pool, key, ped);
            const std::uint64_t encVehicle = EncodePointer(pool, key, vehicle);
            const std::uint64_t encObject = EncodePointer(pool, key, object);
            if (DecodePointer(encPed, key, ped) != pool || EngineDecodePed(encPed, key) != pool)
            {
                Check(false, "ped cipher round trip and engine decoder");
                break;
            }
            if (DecodePointer(encVehicle, key, vehicle) != pool || EngineDecodeVehicle(encVehicle, key) != pool ||
                EngineEncodeVehicle(pool, key) != encVehicle)
            {
                Check(false, "vehicle cipher matches the engine writer and reader");
                break;
            }
            if (DecodePointer(encObject, key, object) != pool || EngineDecodeObject(encObject, key) != pool)
            {
                Check(false, "object cipher round trip and engine decoder");
                break;
            }
            // A wrong cipher does not give the pool back (it is what Check's
            // header validation catches in the game).
            if (DecodePointer(encObject, key, vehicle) == pool && DecodePointer(encObject, key, ped) == pool)
            {
                Check(false, "different ciphers differ");
                break;
            }
        }
        Check(Rol(0x8000000000000001ull, 1) == 3ull && Ror(3ull, 1) == 0x8000000000000001ull, "rotations");
        Check(Rol(0x1234ull, 0) == 0x1234ull && Ror(0x1234ull, 64) == 0x1234ull, "rotation by 0 and 64");
    }

    // A synthetic pool: entries in `storage`, one flag per slot.
    struct FakePool
    {
        std::int32_t size = 0;
        std::int32_t entrySize = 0;
        std::vector<std::uint8_t> storage;
        std::vector<std::uint8_t> flags;

        FakePool(std::int32_t slots, std::int32_t entry) : size(slots), entrySize(entry)
        {
            storage.assign(static_cast<std::size_t>(slots) * static_cast<std::size_t>(entry), 0);
            flags.assign(static_cast<std::size_t>(slots), kFlagFree);
        }
        std::uint64_t Base() const { return reinterpret_cast<std::uint64_t>(storage.data()); }
        void Put(std::int32_t index, std::uint8_t generation, std::uint8_t type, float x, float y, float z,
            std::int16_t stepX = 0, std::int16_t stepY = 0)
        {
            flags[static_cast<std::size_t>(index)] = generation & 0x7F;
            std::uint8_t* entity = storage.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(entrySize);
            entity[kEntityType] = type;
            const float xyz[3] = {x, y, z};
            std::memcpy(entity + kEntityPosition, xyz, 12);
            std::memcpy(entity + kEntityPosition + 12, &stepX, 2);
            std::memcpy(entity + kEntityPosition + 14, &stepY, 2);
        }
        void Free(std::int32_t index)
        {
            flags[static_cast<std::size_t>(index)] |= kFlagFree;
            // The engine's Delete writes -1 over the entry's first dword.
            std::memset(storage.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(entrySize), 0xFF, 4);
        }
        PoolHeader Header() const
        {
            std::uint8_t bytes[kHeaderBytes]{};
            const std::uint64_t storageAddress = Base();
            const std::uint64_t flagsAddress = reinterpret_cast<std::uint64_t>(flags.data());
            std::uint32_t used = 0;
            for (const std::uint8_t flag : flags)
                used += SlotUsed(flag) ? 1u : 0u;
            used |= 0x40000000u; // the ctor's bit 30 is not part of the count
            std::memcpy(bytes + kHeaderStorage, &storageAddress, 8);
            std::memcpy(bytes + kHeaderFlags, &flagsAddress, 8);
            std::memcpy(bytes + kHeaderSize, &size, 4);
            std::memcpy(bytes + kHeaderEntrySize, &entrySize, 4);
            std::memcpy(bytes + kHeaderUsed, &used, 4);
            return ParseHeader(bytes);
        }
        // Reads inside the buffer only; anything else is "unreadable".
        bool Read(std::uint64_t address, std::uint8_t& type, std::uint8_t* position) const
        {
            const std::uint64_t base = Base();
            if (address < base || address + kEntityPosition + 16 > base + storage.size())
                return false;
            const std::uint8_t* entity = storage.data() + (address - base);
            type = entity[kEntityType];
            std::memcpy(position, entity + kEntityPosition, 16);
            return true;
        }
    };

    void TestHeader()
    {
        FakePool pool(8, 0x380);
        pool.Put(0, 1, kTypePed, 0, 0, 0);
        pool.Put(3, 2, kTypePed, 0, 0, 0);
        const PoolHeader header = pool.Header();
        Check(header.storage == pool.Base() && header.size == 8 && header.entrySize == 0x380, "header fields");
        Check(header.used == 2, "used count without the ctor's bit 30");
        Check(ValidateHeader(header, 0x380, 0x380) == HeaderStatus::Ok, "a ped pool validates");
        Check(ValidateHeader(header, 0xCD0, 0xCD0) == HeaderStatus::BadEntrySize, "another class's entry size is refused");
        PoolHeader bad = header;
        bad.storage = 0;
        Check(ValidateHeader(bad, 0x380, 0x380) == HeaderStatus::NoStorage, "no storage");
        bad = header;
        bad.flags = 0;
        Check(ValidateHeader(bad, 0x380, 0x380) == HeaderStatus::NoFlags, "no flags");
        bad = header;
        bad.size = 0;
        Check(ValidateHeader(bad, 0x380, 0x380) == HeaderStatus::BadSize, "zero slots");
        bad.size = kMaxSlots + 1;
        Check(ValidateHeader(bad, 0x380, 0x380) == HeaderStatus::BadSize, "a garbage slot count");
        bad = header;
        bad.used = 9;
        Check(ValidateHeader(bad, 0x380, 0x380) == HeaderStatus::UsedOverSize, "more used than slots");
        Check(std::string(HeaderStatusText(HeaderStatus::BadEntrySize)) == "bad_entry_size", "status names");
    }

    void TestPosition()
    {
        std::uint8_t bytes[16]{};
        const float xyz[3] = {10.5f, -3.25f, 42.0f};
        const std::int16_t step[2] = {-7, 26};
        std::memcpy(bytes, xyz, 12);
        std::memcpy(bytes + 12, step, 4);
        const Position p = DecodePosition(bytes);
        Check(p.x == 10.5f - 7 * 32.0f && p.y == -3.25f + 26 * 32.0f && p.z == 42.0f,
            "position: x and y plus 32 m steps from the signed words at +0x7C and +0x7E");
    }

    void TestWalk()
    {
        FakePool pool(10, 0x380);
        pool.Put(0, 5, kTypePed, 1, 0, 0);
        pool.Put(2, 0x7F, kTypePed, 2, 0, 0);
        pool.Put(4, 1, kTypeObject, 3, 0, 0); // a slot of another type
        pool.Put(6, 3, kTypePed, 4, 0, 0);
        pool.Put(9, 9, kTypePed, 5, 0, 0);    // the last slot
        pool.Put(7, 2, kTypePed, 6, 0, 0);
        pool.Free(7);                          // freed: garbage left in it
        const PoolHeader header = pool.Header();
        const auto read = [&](std::uint64_t a, std::uint8_t& t, std::uint8_t* p) { return pool.Read(a, t, p); };

        Entry out[16]{};
        WalkStats stats;
        int count = WalkPool(header, pool.flags.data(), static_cast<std::int32_t>(pool.flags.size()), read, kTypePed, out,
            16, &stats);
        Check(count == 4, "live peds only: free slots and the other type are skipped");
        Check(stats.slots == 10 && stats.used == 5 && stats.wrongType == 1 && stats.unreadable == 0, "walk stats");
        Check(out[0].address == pool.Base() && out[0].ref == PoolRef(0, 5), "slot 0: address and reference");
        Check(out[1].address == pool.Base() + 2 * 0x380ull && out[1].ref == ((2 << 8) | 0x7F), "slot 2: generation kept");
        Check(out[3].address == pool.Base() + 9 * 0x380ull && out[3].position.x == 5.0f, "the last slot is walked");

        // Type 0 accepts every live slot.
        count = WalkPool(header, pool.flags.data(), 10, read, 0, out, 16, &stats);
        Check(count == 5 && stats.wrongType == 0, "no expected type: every used slot");

        // Capacity: the rest counted as truncated, nothing written past it.
        Entry small[2]{};
        count = WalkPool(header, pool.flags.data(), 10, read, kTypePed, small, 2, &stats);
        Check(count == 2 && stats.truncated == 2, "capacity bounds the output");

        // Fewer flag bytes than slots (a short copy): only those are walked.
        count = WalkPool(header, pool.flags.data(), 3, read, kTypePed, out, 16, &stats);
        Check(count == 2 && stats.slots == 3, "the flag count bounds the walk");

        // A header claiming more slots than the storage: unreadable entries
        // are skipped, never read out of bounds.
        PoolHeader wide = header;
        wide.size = 12;
        std::vector<std::uint8_t> flags = pool.flags;
        flags.push_back(1);
        flags.push_back(1);
        count = WalkPool(wide, flags.data(), 12, read, kTypePed, out, 16, &stats);
        Check(count == 4 && stats.unreadable == 2, "slots past the storage are unreadable, not read");

        // No flags or a broken header: nothing.
        Check(WalkPool(header, nullptr, 10, read, kTypePed, out, 16) == 0, "no flags: nothing");
        PoolHeader broken = header;
        broken.entrySize = 0;
        Check(WalkPool(broken, pool.flags.data(), 10, read, kTypePed, out, 16) == 0, "entry size 0: nothing");
    }

    void TestGuids()
    {
        // The script handle pool: entry 0x10, +0x08 the entity.
        FakePool guids(6, 0x10);
        const std::uint64_t entities[6] = {0x5000, 0, 0x1000, 0x3000, 0x2000, 0x4000};
        for (int n = 0; n < 6; ++n)
        {
            guids.flags[static_cast<std::size_t>(n)] = static_cast<std::uint8_t>(n + 1);
            std::memcpy(guids.storage.data() + n * 0x10 + 8, &entities[n], 8);
        }
        guids.flags[3] |= kFlagFree; // 0x3000 had a handle, released
        const PoolHeader header = guids.Header();
        const auto readTarget = [&](std::uint64_t entry, std::uint64_t& target) {
            if (entry < guids.Base() || entry + 16 > guids.Base() + guids.storage.size())
                return false;
            std::memcpy(&target, guids.storage.data() + (entry - guids.Base()) + 8, 8);
            return true;
        };
        GuidEntry out[8]{};
        const int count = WalkGuids(header, guids.flags.data(), 6, readTarget, out, 8);
        Check(count == 4, "live guids with an entity (an empty target and a free slot skipped)");
        Check(out[0].entity == 0x1000 && out[3].entity == 0x5000, "sorted by entity");
        Check(FindGuid(out, count, 0x2000) == ((4 << 8) | 5), "handle = (slot << 8) | flag");
        Check(FindGuid(out, count, 0x5000) == ((0 << 8) | 1), "slot 0");
        Check(FindGuid(out, count, 0x3000) == 0, "a released handle is not found");
        Check(FindGuid(out, count, 0x6000) == 0 && FindGuid(out, 0, 0x1000) == 0, "missing and empty");
    }

    void TestPlan()
    {
        // Entries at x = 0..9 m (address = x), centre at 0; 3 and 8 already
        // have a handle; radius 6 m; 2 new handles at most.
        Entry entries[10]{};
        for (int n = 0; n < 10; ++n)
        {
            entries[n].address = static_cast<std::uint64_t>(9 - n); // reverse order in the pool
            entries[n].position = {static_cast<float>(9 - n), 0.0f, 0.0f};
        }
        const Position center{0, 0, 0};
        const auto has = [](const Entry& e) { return e.address == 3 || e.address == 8; };
        std::vector<int> order;
        std::vector<float> d2;
        std::uint32_t far = 0, budget = 0;
        PlanHandles(entries, 10, &center, 1, 6.0f, 100.0f, 2, has, order, d2, &far, &budget);
        std::vector<std::uint64_t> got;
        for (const int index : order)
            got.push_back(entries[index].address);
        Check(got == std::vector<std::uint64_t>({0, 1, 3, 8}),
            "nearest two new ones, plus every entity that has a handle (8 beyond the radius too), by distance");
        Check(far == 2 && budget == 4, "7 and 9 beyond the radius; 2, 4, 5, 6 over the budget");

        // Entities with a handle beyond `existingRadius` are left out too.
        far = budget = 0;
        PlanHandles(entries, 10, &center, 1, 6.0f, 5.0f, 2, has, order, d2, &far, &budget);
        got.clear();
        for (const int index : order)
            got.push_back(entries[index].address);
        Check(got == std::vector<std::uint64_t>({0, 1, 3}), "a handled entity beyond the existing radius is dropped");
        Check(far == 3, "7, 8 and 9 counted beyond");

        // Two centres: the nearer counts (the player or the camera).
        const Position centers[2] = {{100, 0, 0}, {9, 0, 0}};
        far = budget = 0;
        PlanHandles(entries, 10, centers, 2, 1.5f, 1.5f, 10, [](const Entry&) { return false; }, order, d2, &far, &budget);
        got.clear();
        for (const int index : order)
            got.push_back(entries[index].address);
        Check(got == std::vector<std::uint64_t>({9, 8}), "the camera's neighbours within 1.5 m, nearest first");
        Check(far == 8 && budget == 0, "the rest beyond the radius");

        // Nothing to plan.
        PlanHandles(entries, 0, &center, 1, 6.0f, 6.0f, 2, has, order, d2);
        Check(order.empty(), "no entries, empty plan");
    }
}

namespace
{
    // The handle pool reserve left to the game.
    void TestGuidReserve()
    {
        Check(replay::pools::GuidReserve(1450) == 362u, "guid reserve: a quarter of the game's pool (362 of 1450)");
        Check(replay::pools::GuidReserve(100) == replay::pools::kGuidReserveMin, "guid reserve: never under the minimum");
        Check(replay::pools::GuidReserve(0) == replay::pools::kGuidReserveMin && replay::pools::GuidReserve(-5) == replay::pools::kGuidReserveMin,
            "guid reserve: an unreadable size keeps the minimum");
    }
}

int RunEntityPoolsTests()
{
    TestGuidReserve();
    TestCipher();
    TestHeader();
    TestPosition();
    TestWalk();
    TestGuids();
    TestPlan();
    if (g_poolFailures == 0)
        std::puts("Entity pool tests passed.");
    return g_poolFailures;
}
