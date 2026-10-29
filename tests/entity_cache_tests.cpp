// The pure logic of the per-frame entity cache (src/entity_cache.h): a handle
// resolved through a synthetic script guid pool the way the engine's GetAt
// does, the cross-check policy, the frame table, the gore table; the event
// range lookup (entity_events.h) and the lifecycle stamp that lets playback
// skip AliveAt (entity_lifecycle.h).
#include "entity_cache.h"
#include "entity_events.h"
#include "entity_lifecycle.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace
{
    int g_cacheFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_cacheFailures;
    }

    using namespace replay::entities;
    namespace pools = replay::pools;

    // A fake address space: guid pool storage and flags, entities.
    struct Memory
    {
        std::map<std::uint64_t, std::uint8_t> bytes;
        void Byte(std::uint64_t at, std::uint8_t value) { bytes[at] = value; }
        void Qword(std::uint64_t at, std::uint64_t value)
        {
            for (int n = 0; n < 8; ++n)
                bytes[at + n] = static_cast<std::uint8_t>(value >> (8 * n));
        }
        bool ReadByte(std::uint64_t at, std::uint8_t& out) const
        {
            const auto found = bytes.find(at);
            if (found == bytes.end())
                return false;
            out = found->second;
            return true;
        }
        bool ReadQword(std::uint64_t at, std::uint64_t& out) const
        {
            out = 0;
            for (int n = 0; n < 8; ++n)
            {
                std::uint8_t b = 0;
                if (!ReadByte(at + n, b))
                    return false;
                out |= static_cast<std::uint64_t>(b) << (8 * n);
            }
            return true;
        }
    };

    constexpr std::uint64_t kStorage = 0x100000;
    constexpr std::uint64_t kFlags = 0x200000;
    constexpr std::int32_t kSlots = 16;

    pools::PoolHeader GuidHeader()
    {
        pools::PoolHeader header;
        header.storage = kStorage;
        header.flags = kFlags;
        header.size = kSlots;
        header.entrySize = 0x10;
        header.used = 0;
        return header;
    }

    // Slot `index` holds a guid of generation `generation` for `entity`.
    void Guid(Memory& memory, int index, std::uint8_t generation, std::uint64_t entity)
    {
        memory.Byte(kFlags + index, generation);
        memory.Qword(kStorage + static_cast<std::uint64_t>(index) * 0x10, 0x1111); // vtable-ish word
        memory.Qword(kStorage + static_cast<std::uint64_t>(index) * 0x10 + kGuidEntity, entity);
    }

    std::uint64_t Resolve(const Memory& memory, const pools::PoolHeader& header, std::int32_t handle)
    {
        return ResolveGuid(header, handle,
            [&](std::uint64_t at, std::uint8_t& out) { return memory.ReadByte(at, out); },
            [&](std::uint64_t at, std::uint64_t& out) { return memory.ReadQword(at, out); });
    }

    void TestResolveGuid()
    {
        Memory memory;
        for (int n = 0; n < kSlots; ++n)
            memory.Byte(kFlags + n, pools::kFlagFree | 3); // free, generation 3
        Guid(memory, 2, 5, 0xAAA000);
        Guid(memory, 7, 0x41, 0xBBB000);
        const pools::PoolHeader header = GuidHeader();

        Check(Resolve(memory, header, (2 << 8) | 5) == 0xAAA000, "a live guid resolves to its entity");
        Check(Resolve(memory, header, (7 << 8) | 0x41) == 0xBBB000, "the generation is the handle's low byte");
        Check(Resolve(memory, header, (2 << 8) | 4) == 0, "a stale generation (the slot was reused) does not resolve");
        Check(Resolve(memory, header, (3 << 8) | 3) == 0, "a free slot never resolves, even with its old generation");
        Check(Resolve(memory, header, (3 << 8) | (pools::kFlagFree | 3)) == 0,
            "a handle whose low byte has the free bit does not match a free slot");
        Check(Resolve(memory, header, (kSlots << 8) | 5) == 0, "a slot past the pool size does not resolve");
        Check(Resolve(memory, header, -1) == 0 && Resolve(memory, header, -256) == 0,
            "negative handles never index before the flags");
        Check(Resolve(memory, header, 0) == 0, "handle 0 (slot 0 free) does not resolve");

        // Deleting the entity frees its guid: the same handle stops at once.
        memory.Byte(kFlags + 2, pools::kFlagFree | 5);
        Check(Resolve(memory, header, (2 << 8) | 5) == 0, "a guid freed in the frame is gone for the next query");
        // A new guid in the same slot has a new generation: the old handle
        // stays dead, the new one resolves.
        Guid(memory, 2, 6, 0xCCC000);
        Check(Resolve(memory, header, (2 << 8) | 5) == 0 && Resolve(memory, header, (2 << 8) | 6) == 0xCCC000,
            "a reused slot answers only its new handle");

        // Unreadable memory: the flags or the target.
        Memory holes = memory;
        holes.bytes.erase(kFlags + 7);
        Check(Resolve(holes, header, (7 << 8) | 0x41) == 0, "an unreadable flag byte gives no entity");
        holes = memory;
        holes.bytes.erase(kStorage + 7 * 0x10 + kGuidEntity + 3);
        Check(Resolve(holes, header, (7 << 8) | 0x41) == 0, "an unreadable target gives no entity");

        pools::PoolHeader bad = header;
        bad.storage = 0;
        Check(Resolve(memory, bad, (7 << 8) | 0x41) == 0, "no storage, no answer");
        bad = header;
        bad.entrySize = 8;
        Check(Resolve(memory, bad, (7 << 8) | 0x41) == 0, "an entry too small for +0x08 is refused");
    }

    void TestCrossCheck()
    {
        CrossCheck check;
        std::uint32_t due = 0;
        for (int n = 0; n < 1024; ++n)
            due += check.Due() ? 1u : 0u;
        Check(due == CrossCheck::kFirst + (1024 - CrossCheck::kFirst) / CrossCheck::kEvery,
            "the first 512 calls, then one in 64, are cross-checked");
        Check(!check.Note(true) && !check.disabled, "a match changes nothing");
        Check(!check.Note(false) && !check.Note(false) && !check.disabled, "two mismatches are tolerated");
        Check(check.Note(false) && check.disabled, "the third mismatch disables the memory path");
        Check(!check.Due(), "a disabled path is never cross-checked again");
        Check(!check.Note(false), "notes after the switch-off do not report again");
        check.Reset();
        Check(!check.disabled && check.Due(), "reset starts over");
    }

    void TestFrameTable()
    {
        FrameTable<bool> table;
        table.BeginFrame(1);
        Check(table.Find(10) == nullptr, "empty at the start of a frame");
        table.Put(10, true);
        table.Put(11, false);
        Check(table.Find(10) && *table.Find(10) && table.Find(11) && !*table.Find(11), "values kept in the frame");
        table.Put(10, false); // FREEZE_ENTITY_POSITION in the frame: NoteFrozen
        Check(table.Find(10) && !*table.Find(10), "a value changed in the frame is the one read after");
        table.BeginFrame(1);
        Check(table.Size() == 2, "the same frame again keeps the values");
        table.Forget(11);
        Check(table.Find(11) == nullptr, "forget drops one handle");
        table.BeginFrame(2);
        Check(table.Size() == 0 && table.Find(10) == nullptr, "a new frame starts empty");
    }

    void TestGoreTable()
    {
        std::vector<std::uint8_t> raw(5 * kGoreEntryBytes, 0);
        const auto put = [&](int n, std::uint64_t ped, std::uint32_t mask) {
            std::memcpy(raw.data() + n * kGoreEntryBytes + kGoreEntryPed, &ped, 8);
            std::memcpy(raw.data() + n * kGoreEntryBytes + kGoreEntryMask, &mask, 4);
        };
        put(0, 0x9000, 1u << 3);
        put(1, 0x5000, (1u << 4) | (1u << 5));
        put(2, 0, 0xFFFFFFFFu);           // empty entry: skipped
        put(3, 0x9000, 0xFFFFFFFFu);      // a second entry of the same ped: the getter stops at the first
        put(4, 0x7000, 0x80000000u);
        std::vector<GoreEntry> table;
        ParseGore(raw.data(), 5, table);
        Check(table.size() == 4, "entries without a ped are dropped");
        std::uint32_t mask = 0;
        Check(GoreMaskOf(table, 0x9000, mask) && mask == (1u << 3), "the first entry of a ped wins, like the getter");
        Check(GoreMaskOf(table, 0x5000, mask) && mask == 0x30u, "the whole dword is kept");
        Check(!GoreMaskOf(table, 0x6000, mask) && mask == 0, "a ped without an entry has no gore");

        // The native's bt: limb n is bit n mod 32; limb 37 (head) is bit 5.
        Check(GoreHas(0x30u, 4) && GoreHas(0x30u, 5) && !GoreHas(0x30u, 3), "limb bits");
        Check(GoreHas(0x30u, 37), "limb 37 reads bit 5, as bt eax, ebx does");
        Check(GoreHas(0x80000000u, 31) && !GoreHas(0x80000000u, 30), "bit 31");
        // Every documented limb against a loop over the bits.
        const int limbs[] = {3, 4, 6, 7, 9, 10, 12, 13, 37};
        for (std::uint32_t pattern : {0u, 0x2B58u, 0xFFFFFFFFu, 0x20u, 0x1234ABCDu})
            for (const int limb : limbs)
                Check(GoreHas(pattern, limb) == (((pattern >> (limb % 32)) & 1u) != 0), "GoreHas matches bt for every limb");
    }

    struct TimedThing
    {
        std::uint32_t timeMs = 0;
        int tag = 0;
    };

    std::vector<std::size_t> NaiveRange(const std::vector<TimedThing>& events, std::uint32_t afterMs, std::uint32_t untilMs)
    {
        std::vector<std::size_t> out;
        if (untilMs <= afterMs)
            return out;
        for (std::size_t n = 0; n < events.size(); ++n)
            if (events[n].timeMs > afterMs && events[n].timeMs <= untilMs)
                out.push_back(n);
        return out;
    }

    void TestEventRange()
    {
        replay::entity::TimedEvents<TimedThing> events;
        events.Begin();
        // Several at the same time, a late one clamped by Add.
        const std::uint32_t times[] = {0, 10, 10, 10, 25, 40, 30, 40, 100, 100, 250};
        int tag = 0;
        for (const std::uint32_t t : times)
            events.Add({t, tag++});
        const std::vector<TimedThing>& all = events.Events();
        Check(all[6].timeMs == 40, "a late event is clamped to the one before (Add keeps the order)");

        std::vector<std::size_t> got;
        events.Range(0, 10, got);
        Check(got.size() == 3 && got[0] == 1 && got[2] == 3, "events sharing a time come together");
        events.Range(10, 40, got);
        Check(got.size() == 4 && got[0] == 4 && got.back() == 7, "the exclusive start skips the three at 10");
        events.Range(40, 40, got);
        Check(got.empty(), "an empty window gives nothing");
        events.Range(50, 40, got);
        Check(got.empty(), "a backwards window gives nothing");
        // Forward play in 16 ms ticks, then a scrub back and forward again:
        // every window equals the full walk, with no state to reset.
        std::vector<std::uint32_t> playheads;
        for (std::uint32_t t = 0; t <= 300; t += 16)
            playheads.push_back(t);
        playheads.push_back(12); // scrub back
        playheads.push_back(12);
        playheads.push_back(99);
        playheads.push_back(100);
        playheads.push_back(5000); // jump past the end
        std::uint32_t previous = 0;
        std::size_t seen = 0;
        for (const std::uint32_t now : playheads)
        {
            events.Range(previous, now, got);
            Check(got == NaiveRange(all, previous, now), "Range equals the walk from the start");
            seen += got.size();
            previous = now;
        }
        Check(events.FirstAfter(10) == 4 && events.FirstAfter(0) == 1 && events.FirstAfter(250) == all.size(),
            "FirstAfter");
        Check(events.FirstAfter(0xFFFFFFFFu) == all.size(), "FirstAfter at the end of time");
        // Random windows.
        std::uint32_t state = 12345;
        for (int n = 0; n < 2000; ++n)
        {
            state = state * 1103515245u + 12345u;
            const std::uint32_t a = (state >> 8) % 320;
            state = state * 1103515245u + 12345u;
            const std::uint32_t b = (state >> 8) % 320;
            events.Range(a, b, got);
            if (got != NaiveRange(all, a, b))
            {
                Check(false, "random windows match the walk");
                break;
            }
        }
        // DropBefore keeps the order and the lookup.
        events.DropBefore(25);
        events.Range(0, 40, got);
        Check(got.size() == 4 && got[0] == 0, "after a trim the indices start at the new front");
        (void)seen;
    }

    replay::entity::CreatePacket Packet(replay::entity::ReplayId id, std::uint32_t timeMs)
    {
        replay::entity::CreatePacket packet;
        packet.id = id;
        packet.timeMs = timeMs;
        packet.boneCount = 1;
        return packet;
    }

    void TestAliveStamp()
    {
        using namespace replay::entity;
        IdAllocator ids;
        const ReplayId a = ids.Allocate();
        const ReplayId b = ids.Allocate();
        const ReplayId c = ids.Allocate();
        LifecycleTrack track;
        track.Begin(2000);
        track.Create(Packet(a, 0));
        track.Advance(500);
        track.Create(Packet(b, 500));
        track.Create(Packet(c, 500)); // two events at the same time
        for (std::uint32_t t = 500; t <= 4500; t += 50)
            track.Advance(t);
        track.Delete(b, 4500);
        track.Advance(7000);

        Check(LifecycleTrack::kNoAliveStamp != track.AliveStamp(0), "a stamp from the first block");
        // Walk every ms: the stamp moves exactly when AliveAt changes or a
        // block begins; with the same stamp the packets are the same.
        std::vector<CreatePacket> alive;
        std::vector<CreatePacket> previous;
        std::uint64_t previousStamp = LifecycleTrack::kNoAliveStamp - 1;
        std::uint32_t moves = 0;
        bool sameStampSameAlive = true;
        for (std::uint32_t t = 0; t <= 7000; ++t)
        {
            const std::uint64_t stamp = track.AliveStamp(t);
            track.AliveAt(t, alive);
            if (stamp == previousStamp)
            {
                if (alive.size() != previous.size())
                    sameStampSameAlive = false;
                for (std::size_t n = 0; n < alive.size() && sameStampSameAlive; ++n)
                    sameStampSameAlive = alive[n].id == previous[n].id;
            }
            else
            {
                ++moves;
            }
            previous = alive;
            previousStamp = stamp;
        }
        Check(sameStampSameAlive, "the same stamp always means the same alive list");
        // t=0 (block 0), 500 (two creates), 2000, 4000 (blocks), 4500 (delete), 6000 (block).
        Check(moves == 6, "the stamp moves only at events and block starts");
        // Scrub back: the stamp of an earlier time is its own.
        Check(track.AliveStamp(100) == track.AliveStamp(499) && track.AliveStamp(499) != track.AliveStamp(500),
            "a scrub back finds the earlier stamp");
        LifecycleTrack empty;
        empty.Begin(2000);
        Check(empty.AliveStamp(100) == LifecycleTrack::kNoAliveStamp, "no block, no stamp");
    }
}

int RunEntityCacheTests()
{
    TestResolveGuid();
    TestCrossCheck();
    TestFrameTable();
    TestGoreTable();
    TestEventRange();
    TestAliveStamp();
    if (g_cacheFailures == 0)
        std::printf("Entity cache tests passed\n");
    return g_cacheFailures;
}
