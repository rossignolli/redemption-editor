#include "ped_blood.h"
#include "clip_sites.h" // kBloodMerge, kBloodAdd and the finalize calls a clip names
#include "engine_sigs.h" // the graphics component pool (the surface)
#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"
#include "mod_clock.h"
#include "profiler.h"
#include "readable_page.h"
#include "trusted_regions.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace replay::blood
{
    namespace
    {
        namespace sig = replay::sig;

        // The record finalize (record, owner entity, set slot), and its call of
        // the projected decal add.
        const sig::Signature kBloodFinalize{"blood_finalize",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC C0 00 00 00 "
            "0F 29 70 C8 45 8A E0 4C 8B FA 48 8B F1",
            0x1285DA4};
        const sig::Signature kBloodFinalizeProjCall{"blood_finalize_proj_call", kBloodFinalize, sig::Offset{0x1A7},
            "E8 ?? ?? ?? ?? 4C 8D 9C 24 C0 00 00 00", 0x1285F4B};
        // The record merge the add path calls, the bullet blood and the add
        // path, with their calls of the finalize, are in clip_sites.h (a
        // clip names the calls): kBloodMerge, kBloodEvolveCall, kBloodBullet,
        // kBloodBulletCall, kBloodAdd, kBloodPackCall.
        // The merge's address of a set entry's record: index * the record size (u32) +
        // the pool base (u64), then the record's info (+0x108).
        const sig::Signature kBloodPoolRead{"blood_pool_read", kBloodMerge, sig::Offset{0xC7},
            "0F AF 1D ?? ?? ?? ?? 48 03 1D ?? ?? ?? ?? 8B 83 08 01 00 00", 0x125D933};
        const sig::Signature kBloodPoolStride{"blood_pool_stride", kBloodPoolRead, sig::Rel32{0x03, 0x07}, "", 0x4C3CE88};
        const sig::Signature kBloodPoolBase{"blood_pool_base", kBloodPoolRead, sig::Rel32{0x0A, 0x0E}, "", 0x4C3CE70};
        // Its age test: the engine time (float seconds) minus the record's time
        // stamp (+0x130).
        const sig::Signature kBloodRecordAge{"blood_record_age", kBloodMerge, sig::Offset{0x265},
            "F3 0F 10 0D ?? ?? ?? ?? 48 3B EE 75 41 F7 85 DC 01 00 00 00 01 00 00 74 35 F6 83 50 01 00 00 FD 75 2C 0F "
            "28 C1 F3 0F 5C 83 30 01 00 00",
            0x125DAD1};
        const sig::Signature kBloodEngineTime{"blood_engine_time", kBloodRecordAge, sig::Rel32{0x04, 0x08}, "", 0x5429034};
        // Its test of a held record against the new one's info: the held
        // record's next info (+0x10C).
        const sig::Signature kBloodMergeNextInfo{"blood_merge_next_info", kBloodMerge, sig::Offset{0x10D},
            "8B 83 0C 01 00 00 85 C0 78 0C 8B C8 49 8B 45 78 48 8B 14 C8 EB 03", 0x125D979};
        // Its evolve of a record to the next info: +0x104, then the new info at
        // +0x108, then the finalize (record, owner, 0xFF), then the count
        // (+0x110) cleared.
        const sig::Signature kBloodEvolveWrite{"blood_evolve_write", kBloodMerge, sig::Offset{0x4CB},
            "F3 0F 11 8B 04 01 00 00 48 8D 94 24 A0 00 00 00 40 88 AB 81 00 00 00 44 88 BB 82 00 00 00 8B 06 48 8B 0D "
            "?? ?? ?? ?? 89 84 24 A0 00 00 00 E8 ?? ?? ?? ?? 0F 10 9B 20 01 00 00 66 0F 70 C3 AA 66 0F 70 D3 55 0F 14 "
            "D8 89 83 08 01 00 00",
            0x125DD37};
        // The add path's read of the record's info (+0x108) and its load of the ped damage
        // manager (a pointer).
        const sig::Signature kBloodAddManager{"blood_add_manager", kBloodAdd, sig::Offset{0x6A},
            "41 8B 90 08 01 00 00 4C 8B 0D ?? ?? ?? ?? 85 D2 78 0A 49 8B 41 78 4C 8B 04 D0 EB 03", 0x125D4DA};
        const sig::Signature kBloodManager{"blood_manager", kBloodAddManager, sig::Rel32{0x0A, 0x0E}, "", 0x4C38A18};
        // Its read of the record's owner ref (+0x78), handed to the merge.
        const sig::Signature kBloodAddOwnerRef{"blood_add_owner_ref", kBloodAdd, sig::Offset{0xB2},
            "49 8B 45 78 4C 8D 44 24 30 48 89 44 24 28 0F 28 D9 49 8B CF 44 88 54 24 20 E8 ?? ?? ?? ??", 0x125D522};
        constexpr std::size_t kRecordTimeOffset = 0x130;
        using FinalizeFn = void(__fastcall*)(void*, void*, std::uint8_t);

        constexpr std::uint32_t kRingSize = 1024;
        using Slot = replay::ring::Slot<Call>; // event_ring.h
        Slot g_ring[kRingSize];
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<FinalizeFn> g_original{nullptr};
        std::atomic<bool> g_observing{false};
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_target = nullptr;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        // Where the detour reads the record's pool index and age (0: not in this
        // game, index -1 and age 0).
        std::uintptr_t g_poolBaseAt = 0;
        std::uintptr_t g_poolStrideAt = 0;
        std::uintptr_t g_engineTimeAt = 0;
        LARGE_INTEGER g_frequency{};

        std::uint64_t QpcUs()
        {
            return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
        }

        bool GuardedRead(void* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        void __fastcall FinalizeDetour(void* record, void* entity, std::uint8_t slotIndex)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.blood_finalize");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const FinalizeFn original = g_original.load(std::memory_order_acquire);
            if (original)
                original(record, entity, slotIndex);
            if (!g_observing.load(std::memory_order_relaxed) || !record)
                return;
            void* frames[1] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, 1, frames, nullptr);
            replay::ring::Writer<Call> slot(g_ring, kRingSize, g_next);
            Call& call = slot.value;
            call.timeUs = QpcUs();
            call.entity = reinterpret_cast<std::uint64_t>(entity);
            call.slot = slotIndex;
            const std::uintptr_t address = captured ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
            call.caller = address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
            call.copied = GuardedRead(call.record, record, kRecordBytes);
            call.index = -1;
            call.age = 0.0f;
            std::uintptr_t poolBase = 0;
            std::uint32_t stride = 0;
            float engineTime = 0.0f;
            if (g_poolBaseAt && g_poolStrideAt &&
                GuardedRead(&poolBase, reinterpret_cast<void*>(g_poolBaseAt), sizeof(poolBase)) &&
                GuardedRead(&stride, reinterpret_cast<void*>(g_poolStrideAt), sizeof(stride)) && stride &&
                reinterpret_cast<std::uintptr_t>(record) >= poolBase)
            {
                call.index = static_cast<std::int32_t>((reinterpret_cast<std::uintptr_t>(record) - poolBase) / stride);
            }
            if (call.copied && g_engineTimeAt &&
                GuardedRead(&engineTime, reinterpret_cast<void*>(g_engineTimeAt), sizeof(engineTime)))
            {
                float stamp = 0.0f;
                std::memcpy(&stamp, call.record + kRecordTimeOffset, sizeof(stamp));
                call.age = engineTime - stamp;
            }
            slot.Publish();
        }
    }

    bool InstallObserver(std::string& error)
    {
        if (g_target)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        void* target = reinterpret_cast<void*>(sig::Find(kBloodFinalize));
        if (!target)
        {
            error = "blood_finalize_signature_mismatch";
            return false;
        }
        g_poolBaseAt = sig::Find(kBloodPoolBase);
        g_poolStrideAt = sig::Find(kBloodPoolStride);
        g_engineTimeAt = sig::Find(kBloodEngineTime);
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&FinalizeDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<FinalizeFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        return true;
    }

    void SetObserving(bool on)
    {
        g_observing.store(on, std::memory_order_relaxed);
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
    }

    std::uint64_t TotalCalls()
    {
        return g_next.load(std::memory_order_acquire);
    }

    void CollectSince(std::uint64_t& cursor, std::vector<Call>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_ring, kRingSize, g_next, cursor, [&](const Call& call) { out.push_back(call); });
    }

    namespace
    {
        // The RVA the return of the call at `call` (5 bytes) has, or `missing`
        // (above every RVA) when the game lacks the call.
        std::uint32_t ReturnRva(const sig::Signature& call, std::uint32_t missing)
        {
            const std::uintptr_t at = sig::Find(call);
            const std::uintptr_t base = sig::ImageBase();
            return at && base ? static_cast<std::uint32_t>(at + 5 - base) : missing;
        }

        struct CallerRvas
        {
            std::uint32_t pack;
            std::uint32_t blood;
            std::uint32_t evolve;
            std::uint32_t finalizeProj;
        };

        // Once: a value of its own for each site the game lacks, so the sites
        // stay apart (a held record's pack flag rides on them).
        const CallerRvas& Callers()
        {
            static const CallerRvas callers{ReturnRva(kBloodPackCall, 0xFFFFFFFFu),
                ReturnRva(kBloodBulletCall, 0xFFFFFFFEu), ReturnRva(kBloodEvolveCall, 0xFFFFFFFDu),
                ReturnRva(kBloodFinalizeProjCall, 0xFFFFFFFCu)};
            return callers;
        }
    }

    std::uint32_t CallerPack() { return Callers().pack; }
    std::uint32_t CallerBlood() { return Callers().blood; }
    std::uint32_t CallerEvolve() { return Callers().evolve; }
    std::uint32_t FinalizeProjReturn() { return Callers().finalizeProj; }

    namespace
    {
        // The pack apply: the set slot of the ped's damage component (+0x10, 0xFF
        // none) after the slot allocation, then the render gate object handed to
        // the gate test (busy while its +0xB0 or +0xC0 is set).
        const sig::Signature kBloodPackGate{"blood_pack_gate",
            "48 8B D3 48 8B CE E8 ?? ?? ?? ?? 80 7B 10 FF 0F 84 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 "
            "25",
            0x125ED00};
        const sig::Signature kBloodRenderGate{"blood_render_gate", kBloodPackGate, sig::Rel32{0x18, 0x1C}, "", 0x5A9D690};
        const sig::Signature kBloodGateTest{"blood_gate_test", kBloodPackGate, sig::Rel32{0x1D, 0x21},
            "48 83 B9 B0 00 00 00 00 75 0D 48 83 B9 C0 00 00 00 00 75 03 B0 01 C3", 0x2BA8E14};
        // The ped's damage component, created when missing (ped) -> component:
        // reached from a caller that reads its set slot right after.
        const sig::Signature kBloodCompCaller{"blood_comp_caller",
            "48 8B CD E8 ?? ?? ?? ?? 48 85 C0 74 14 80 78 10 FF 74 0E 0F B6 40 10", 0x12B8512};
        const sig::Signature kBloodCompCreate{"blood_comp_create", kBloodCompCaller, sig::Rel32{0x04, 0x08},
            "48 83 EC 28 48 83 64 24 30 00 48 8D 54 24 30 4C 8B C1 48 81 C1 9C 00 00 00 E8 ?? ?? ?? ?? 48 8B 44 24 30 48 "
            "83 C4 28 C3",
            0xCC5A30};
        // The same component when the ped has one (ped) -> component or null.
        const sig::Signature kBloodCompRead{"blood_comp_read",
            "8B 81 9C 00 00 00 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 94 01 E8 "
            "00 00 00 48 8B CA",
            0xCC8848};
        // The set slot allocation (manager, component) and the clear (component).
        const sig::Signature kBloodSlotAlloc{"blood_slot_alloc",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 7A 10 FF 48 8B FA 48 8B F1", 0x126AB48};
        const sig::Signature kBloodClear{"blood_clear", "40 53 48 83 EC 20 8A 51 10 48 8B D9 80 FA FF 74 1B", 0xE1F5A0};
        constexpr std::size_t kCompSlot = 0x10;
        constexpr std::size_t kRecordOwnerRef = 0x78;

        // What the player calls and reads, found once (EnsurePlayer).
        struct Player
        {
            std::uintptr_t compCreate = 0;
            std::uintptr_t compRead = 0;
            std::uintptr_t slotAlloc = 0;
            std::uintptr_t add = 0;
            std::uintptr_t clear = 0;
            std::uintptr_t manager = 0;    // pointer to the ped damage manager
            std::uintptr_t renderGate = 0; // object whose +0xB0/+0xC0 are null when idle
            std::uintptr_t poolBase = 0;   // record pool base pointer
            std::uintptr_t poolStride = 0; // record size (u32)
            std::uintptr_t engineTime = 0; // float seconds the records age against
        };
        Player g_player;

        // The damage component through the engine, checked first. The engine reads it
        // from an index in the ped (+0x9C); a ped being torn down (e.g. a scrub past a
        // burned carriage) can give an address on a stack guard page, and a first read
        // of it (comp+0x10) would raise a stack overflow there: caught by the guard,
        // but the .NET runtime of another mod in the process ends the game on that
        // code before our handler runs. So the ped's index and the component are
        // looked at before anything reads them.
        // VirtualQuery walks the page tables of the whole region to report its
        // extent; on the ped and damage component pools (big committed heaps) one
        // query takes 4 to 9 ms. A region seen committed, readable and without guard
        // pages is trusted for kRegionTtlMs, and a hit keeps it trusted for another
        // kRegionTtlMs (trusted_regions.h; the pools are allocated once). A guard page
        // is a region of its own (other protection), so a stack page is never cached;
        // the cache is dropped at every playback start and end.
        constexpr std::size_t kRegionSlots = 8;
        constexpr std::uint64_t kRegionTtlMs = 5000;
        replay::mem::TrustedRegions<kRegionSlots> g_regions;
        std::uint64_t g_queryTicks = 0;
        std::uint64_t g_queries = 0;
        std::uint64_t g_queryHits = 0;

        std::uint64_t QpcNow()
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<std::uint64_t>(now.QuadPart);
        }

        std::uint64_t QpcPerMs()
        {
            static std::uint64_t perMs = 0;
            if (!perMs)
            {
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                perMs = frequency.QuadPart > 1000 ? static_cast<std::uint64_t>(frequency.QuadPart) / 1000 : 1;
            }
            return perMs;
        }

        // The ranges the page's working set entry answered, and their time.
        std::uint64_t g_pageAnswers = 0;
        std::uint64_t g_pageTicks = 0;

        bool Readable(const void* address, std::size_t bytes)
        {
            if (!address)
                return false;
            const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(address);
            const std::uintptr_t last = first + bytes;
            const std::uint64_t now = QpcNow();
            if (g_regions.Contains(first, last, now, kRegionTtlMs * QpcPerMs()))
            {
                ++g_queryHits;
                return true;
            }
            // A range inside one resident page gets VirtualQuery's answer from that page's
            // working set entry (readable_page.h), without the walk of the whole pool
            // region (5.5 to 20 ms a query). No region was read, so none goes into the cache.
            bool answered = false;
            const bool pageReadable = replay::mem::ReadableByWorkingSet(address, bytes, answered);
            if (answered)
            {
                ++g_pageAnswers;
                g_pageTicks += QpcNow() - now;
                return pageReadable;
            }
            // VirtualQuery (readable_page.h, the same test).
            std::uintptr_t regionBase = 0;
            std::uintptr_t regionEnd = 0;
            bool queried = false;
            const bool readable = replay::mem::ReadableByQuery(address, bytes, regionBase, regionEnd, queried);
            g_queryTicks += QpcNow() - now;
            ++g_queries;
            if (regionEnd)
                g_regions.Add(regionBase, regionEnd, now, kRegionTtlMs * QpcPerMs());
            return readable;
        }

        // The ped's blood component by `f` (create or read); `faulted` when the
        // call faulted (its code in engine::LastFault()).
        void* ReadComp(const engine::Function<void*(void*)>& f, std::uintptr_t function, const void* ped, bool& faulted)
        {
            faulted = false;
            if (!Readable(static_cast<const std::uint8_t*>(ped) + 0x9C, sizeof(std::uint32_t)))
                return nullptr;
            void* comp = nullptr;
            if (!engine::Call(f, function, comp, const_cast<void*>(ped)))
            {
                faulted = true;
                return nullptr;
            }
            return comp && Readable(comp, 0x20) ? comp : nullptr;
        }

        bool g_playerChecked = false;
        bool g_playerReady = false;

        bool EnsurePlayer(std::string& error)
        {
            if (g_playerChecked)
            {
                if (!g_playerReady)
                    error = "blood_player_signature_mismatch";
                return g_playerReady;
            }
            g_playerChecked = true;
            Player player;
            player.compCreate = sig::Find(kBloodCompCreate);
            player.compRead = sig::Find(kBloodCompRead);
            player.slotAlloc = sig::Find(kBloodSlotAlloc);
            player.add = sig::Find(kBloodAdd);
            player.clear = sig::Find(kBloodClear);
            player.manager = sig::Find(kBloodManager);
            player.renderGate = sig::Find(kBloodRenderGate);
            player.poolBase = sig::Find(kBloodPoolBase);
            player.poolStride = sig::Find(kBloodPoolStride);
            player.engineTime = sig::Find(kBloodEngineTime);
            // The fields the copy of a record gets (the owner ref the add reads,
            // the time stamp the merge ages) and the gate's two fields are in the
            // bytes of these.
            g_playerReady = player.compCreate && player.compRead && player.slotAlloc && player.add && player.clear &&
                player.manager && player.renderGate && player.poolBase && player.poolStride && player.engineTime &&
                sig::Find(kBloodAddOwnerRef) && sig::Find(kBloodRecordAge) && sig::Find(kBloodGateTest);
            if (g_playerReady)
                g_player = player;
            else
                error = "blood_player_signature_mismatch";
            return g_playerReady;
        }

        // Evidence for play.blood stalls: the slot allocation and the add, timed apart
        // (QPC ticks) for the last GuardedApply.
        std::uint64_t g_lastAllocTicks = 0;
        std::uint64_t g_lastAddTicks = 0;
        bool g_lastAllocated = false;

        std::uint64_t QpcTicks()
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<std::uint64_t>(now.QuadPart);
        }

        bool GuardedApply(void* ped, std::uint8_t* buffer, bool pack, void*& added, std::uint8_t& slotOut, std::uint32_t& code)
        {
            g_lastAllocTicks = 0;
            g_lastAddTicks = 0;
            g_lastAllocated = false;
            __try
            {
                void* manager = *reinterpret_cast<void**>(g_player.manager);
                if (!manager)
                    return false;
                bool faulted = false;
                void* comp = ReadComp(engine::kBloodCompCreate, g_player.compCreate, ped, faulted);
                if (faulted)
                    return engine::Failed(code);
                if (!comp)
                    return false;
                std::uint8_t slot = *(reinterpret_cast<std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF)
                {
                    const std::uint64_t allocStart = QpcTicks();
                    if (!engine::Call(engine::kBloodSlotAlloc, g_player.slotAlloc, manager, comp))
                        return engine::Failed(code);
                    g_lastAllocTicks = QpcTicks() - allocStart;
                    g_lastAllocated = true;
                    slot = *(reinterpret_cast<std::uint8_t*>(comp) + kCompSlot);
                }
                slotOut = slot;
                if (slot == 0xFF)
                    return false;
                const std::uint64_t addStart = QpcTicks();
                if (!engine::Call(engine::kBloodAdd, g_player.add, added, manager, slot, buffer, pack ? 1 : 0))
                    return engine::Failed(code);
                g_lastAddTicks = QpcTicks() - addStart;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The engine's own merge test (kBloodMerge): a record of the
        // ped's set whose +0x108 or +0x10C info equals the new one's and whose
        // +0x00 xyz lies within info+0x198 * new w absorbs the new record and
        // the add returns null. Replaying a record the engine already rebuilt
        // (a replayed gore hit re-applies its pack) lands there: that is a
        // record already present, not a refusal. Guarded reads only.
        bool GuardedFindEquivalent(const void* ped, const std::uint8_t* record, bool& found)
        {
            found = false;
            __try
            {
                bool faulted = false;
                void* comp = ReadComp(engine::kBloodCompRead, g_player.compRead, ped, faulted);
                if (faulted)
                    return false;
                if (!comp)
                    return true;
                const std::uint8_t slot = *(static_cast<const std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF || slot >= 128)
                    return true;
                const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(g_player.manager);
                if (!manager)
                    return true;
                const std::uintptr_t set = *reinterpret_cast<const std::uintptr_t*>(manager + 0xD8 + slot * 8);
                if (!set)
                    return true;
                const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(set + 0x690);
                std::int32_t info = 0;
                std::memcpy(&info, record + 0x108, sizeof(info));
                if (count == 0 || count > 64 || info < 0)
                    return true;
                const std::uintptr_t infos = *reinterpret_cast<const std::uintptr_t*>(manager + 0x78);
                const std::uintptr_t infoObject = infos ? *reinterpret_cast<const std::uintptr_t*>(infos + static_cast<std::uintptr_t>(info) * 8) : 0;
                if (!infoObject)
                    return true;
                float position[4] = {};
                std::memcpy(position, record, sizeof(position));
                const float radius = *reinterpret_cast<const float*>(infoObject + 0x198) * position[3];
                if (!(radius > 0.0f))
                    return true;
                const std::uintptr_t pool = *reinterpret_cast<const std::uintptr_t*>(g_player.poolBase);
                const std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(g_player.poolStride);
                if (!pool || !stride)
                    return true;
                for (std::uint32_t n = 0; n < count && !found; ++n)
                {
                    const std::uint16_t index = *reinterpret_cast<const std::uint16_t*>(set + 0x610 + n * 2);
                    const auto* held = reinterpret_cast<const std::uint8_t*>(pool + static_cast<std::uintptr_t>(index) * stride);
                    const std::int32_t heldInfo = *reinterpret_cast<const std::int32_t*>(held + 0x108);
                    const std::int32_t heldNext = *reinterpret_cast<const std::int32_t*>(held + 0x10C);
                    if (heldInfo != info && heldNext != info)
                        continue;
                    const auto* heldPosition = reinterpret_cast<const float*>(held);
                    const float dx = heldPosition[0] - position[0];
                    const float dy = heldPosition[1] - position[1];
                    const float dz = heldPosition[2] - position[2];
                    found = dx * dx + dy * dy + dz * dz <= radius * radius;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                found = false;
                return false;
            }
        }

        bool GuardedClear(void* ped, bool& had, std::uint32_t& code)
        {
            __try
            {
                bool faulted = false;
                void* comp = ReadComp(engine::kBloodCompRead, g_player.compRead, ped, faulted);
                if (faulted)
                    return engine::Failed(code);
                had = comp != nullptr;
                if (comp && !engine::Call(engine::kBloodClear, g_player.clear, comp))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        // The ped's graphics component (engine_sigs.h kComponentRead: rows of
        // 0x148, the component at +0x98).
        constexpr std::size_t kComponentStride = 0x148;
        constexpr std::size_t kSurfaceSlot = 0x98;
        // The engine's wetness clear (component): the heights and amounts at
        // +0x30..+0x3F and the flags at +0x40, the fields a puppet's wetness is
        // written to.
        const sig::Signature kBloodWetClear{"blood_wet_clear",
            "33 D2 B8 00 00 00 C0 88 51 40 89 41 30 89 41 34 48 89 51 38 C3", 0x89AD10};
        // The bank recompute request (ped): the ped's graphics component as above,
        // then +0xC0 = 1.
        const sig::Signature kBloodSurfaceRecompute{"blood_surface_recompute",
            "48 83 EC 28 48 85 C9 74 43 8B 81 9C 00 00 00 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 "
            "?? ?? ?? ?? 48 8B 94 01 98 00 00 00 48 8B C2 48 83 E0 FE 48 F7 DA 48 1B C9 48 23 C8 C6 81 C0 00 00 00 01 E8 "
            "?? ?? ?? ?? 48 83 C4 28 C3",
            0xFEEC90};
        // The three dirt setters (component, value): +0x3A8, +0x3AC and +0x3B0,
        // each mirrored to the MetaPed.
        const sig::Signature kBloodDirtSet3A8{"blood_dirt_set_3a8",
            "40 53 48 83 EC 20 48 8B D9 89 91 A8 03 00 00 48 8B 49 08", 0x8E1624};
        const sig::Signature kBloodDirtSet3AC{"blood_dirt_set_3ac",
            "40 53 48 83 EC 20 48 8B D9 89 91 AC 03 00 00 48 8B 49 08", 0x8E15E4};
        const sig::Signature kBloodDirtSet3B0{"blood_dirt_set_3b0",
            "40 53 48 83 EC 20 48 8B D9 89 91 B0 03 00 00 48 8B 49 08", 0x8E15A4};
        int g_dirtSettersChecked = 0; // 1 ok, -1 mismatch
        std::uintptr_t g_dirtSetters[3] = {};
        const engine::Function<void(void*, std::int32_t)>* const kDirtSetterCalls[3] = {&engine::kBloodDirtSet3A8,
            &engine::kBloodDirtSet3AC, &engine::kBloodDirtSet3B0};

        std::uintptr_t SurfaceComponent(void* ped)
        {
            if (!ped)
                return 0;
            static const replay::enginesig::Pool pool{sig::Find(replay::enginesig::kComponentBaseIndex),
                sig::Find(replay::enginesig::kComponentTable)};
            if (!pool.Found())
                return 0;
            std::uint32_t handle = 0;
            std::uint32_t indexBase = 0;
            std::uintptr_t table = 0;
            if (!GuardedRead(&handle, static_cast<std::uint8_t*>(ped) + 0x9C, 4) ||
                !GuardedRead(&indexBase, reinterpret_cast<void*>(pool.baseIndex), 4) ||
                !GuardedRead(&table, reinterpret_cast<void*>(pool.table), 8) || !table)
                return 0;
            const std::uint32_t index = (handle & 0x1FFFF) - indexBase;
            if (index > 0x20000)
                return 0;
            std::uintptr_t raw = 0;
            if (!GuardedRead(&raw, reinterpret_cast<void*>(table + static_cast<std::uintptr_t>(index) * kComponentStride + kSurfaceSlot), 8))
                return 0;
            return raw ? (raw & ~static_cast<std::uintptr_t>(1)) : 0;
        }

        bool GuardedWrite(void* target, const void* source, std::size_t size)
        {
            __try
            {
                std::memcpy(target, source, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedDirt(int n, void* component, std::int32_t value)
        {
            return engine::Call(*kDirtSetterCalls[n], g_dirtSetters[n], component, value);
        }
    }

    bool ReadSurface(void* ped, Surface& out)
    {
        const std::uintptr_t comp = SurfaceComponent(ped);
        if (!comp)
            return false;
        return GuardedRead(out.wet, reinterpret_cast<void*>(comp + 0x30), sizeof(out.wet)) &&
            GuardedRead(&out.wetFlags, reinterpret_cast<void*>(comp + 0x40), 1) &&
            GuardedRead(out.dirt, reinterpret_cast<void*>(comp + 0x3A8), sizeof(out.dirt));
    }

    bool ApplySurface(void* ped, const Surface& surface, bool dirt, std::string& error)
    {
        const std::uintptr_t comp = SurfaceComponent(ped);
        if (!comp)
        {
            error = "no_component";
            return false;
        }
        if (!sig::Find(kBloodWetClear))
        {
            error = "wet_signature_mismatch";
            return false;
        }
        if (!GuardedWrite(reinterpret_cast<void*>(comp + 0x30), surface.wet, sizeof(surface.wet)) ||
            !GuardedWrite(reinterpret_cast<void*>(comp + 0x40), &surface.wetFlags, 1))
        {
            error = "wet_write_fault";
            return false;
        }
        if (!dirt)
            return true;
        if (!g_dirtSettersChecked)
        {
            // The recompute request is part of it: +0xC0 is written after the
            // setters.
            const sig::Signature* const setters[3] = {&kBloodDirtSet3A8, &kBloodDirtSet3AC, &kBloodDirtSet3B0};
            g_dirtSettersChecked = sig::Find(kBloodSurfaceRecompute) ? 1 : -1;
            for (int n = 0; n < 3; ++n)
            {
                g_dirtSetters[n] = sig::Find(*setters[n]);
                if (!g_dirtSetters[n])
                    g_dirtSettersChecked = -1;
            }
        }
        if (g_dirtSettersChecked < 0)
        {
            error = "dirt_setter_signature_mismatch";
            return false;
        }
        std::int32_t current[3] = {};
        if (GuardedRead(current, reinterpret_cast<void*>(comp + 0x3A8), sizeof(current)) &&
            std::memcmp(current, surface.dirt, sizeof(current)) == 0)
            return true;
        for (int n = 0; n < 3; ++n)
            if (!GuardedDirt(n, reinterpret_cast<void*>(comp), surface.dirt[n]))
            {
                error = "dirt_setter_fault";
                return false;
            }
        const std::uint8_t recompute = 1;
        GuardedWrite(reinterpret_cast<void*>(comp + 0xC0), &recompute, 1);
        return true;
    }

    void ForgetReadableRegions()
    {
        g_regions.Clear();
    }

    void QueryStats(std::uint64_t& queries, std::uint64_t& hits, double& queryMs)
    {
        queries = g_queries;
        hits = g_queryHits;
        queryMs = static_cast<double>(g_queryTicks) / static_cast<double>(QpcPerMs());
    }

    void PageAnswerStats(std::uint64_t& answers, double& ms)
    {
        answers = g_pageAnswers;
        ms = static_cast<double>(g_pageTicks) / static_cast<double>(QpcPerMs());
    }

    void LastApplyTiming(double& allocMs, double& addMs, bool& allocated)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double perMs = frequency.QuadPart ? static_cast<double>(frequency.QuadPart) / 1000.0 : 1.0;
        allocMs = static_cast<double>(g_lastAllocTicks) / perMs;
        addMs = static_cast<double>(g_lastAddTicks) / perMs;
        allocated = g_lastAllocated;
    }

    bool Apply(void* ped, const std::uint8_t* record, float age, bool pack, std::uint64_t ref78,
        bool& busy, std::string& error)
    {
        busy = false;
        if (!ped || !record)
        {
            error = "no_ped";
            return false;
        }
        if (!EnsurePlayer(error))
            return false;
        std::uint64_t gate[2] = {};
        const auto* gateObject = reinterpret_cast<const std::uint8_t*>(g_player.renderGate);
        if (!GuardedRead(&gate[0], gateObject + 0xB0, 8) || !GuardedRead(&gate[1], gateObject + 0xC0, 8))
        {
            error = "gate_unreadable";
            return false;
        }
        if (gate[0] || gate[1])
        {
            busy = true;
            error = "render_busy";
            return false;
        }
        bool present = false;
        if (GuardedFindEquivalent(ped, record, present) && present)
        {
            error = "already_present";
            return true;
        }
        alignas(16) std::uint8_t buffer[kRecordBytes] = {};
        std::memcpy(buffer, record, kRecordBytes);
        float engineTime = 0.0f;
        if (GuardedRead(&engineTime, reinterpret_cast<void*>(g_player.engineTime), sizeof(engineTime)))
        {
            const float stamp = engineTime - (age > 0.0f ? age : 0.0f);
            std::memcpy(buffer + kRecordTimeOffset, &stamp, sizeof(stamp));
        }
        // The entity the record names, resolved by the caller (a puppet or
        // null): a recorded engine address never reaches the engine.
        std::memcpy(buffer + kRecordOwnerRef, &ref78, sizeof(ref78));
        void* added = nullptr;
        std::uint8_t slot = 0xFF;
        std::uint32_t code = 0;
        if (!GuardedApply(ped, buffer, pack, added, slot, code))
        {
            char text[48]{};
            if (code)
                std::snprintf(text, sizeof(text), "exception_%08X", code);
            else
                std::snprintf(text, sizeof(text), slot == 0xFF ? "no_slot" : "no_manager");
            error = text;
            return false;
        }
        if (!added && pack)
        {
            // A pack refuses to land on a neighbouring blood record (+0x88
            // == 0); in the game its records were accepted, so the second
            // try merges like the blood path does.
            std::memcpy(buffer + kRecordOwnerRef, &ref78, sizeof(ref78));
            code = 0;
            if (!GuardedApply(ped, buffer, false, added, slot, code))
                added = nullptr;
            if (added)
                error = "pack_merged";
        }
        if (!added)
        {
            error = "engine_refused";
            return false;
        }
        return true;
    }

    namespace
    {
        // 0 found and evolved, 1 already evolved, 2 no base record, 3 no set. The owner
        // passed to the finalize is the set's own (set+0x608 -> comp, comp+0x08 &
        // ~0xF), the ped when unreadable.
        int GuardedEvolve(void* ped, const std::uint8_t* record, std::uintptr_t finalize, std::uint32_t& code)
        {
            __try
            {
                bool faulted = false;
                void* comp = ReadComp(engine::kBloodCompRead, g_player.compRead, ped, faulted);
                if (faulted)
                {
                    engine::Failed(code);
                    return -1;
                }
                if (!comp)
                    return 3;
                const std::uint8_t slot = *(static_cast<const std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF || slot >= 128)
                    return 3;
                const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(g_player.manager);
                if (!manager)
                    return 3;
                const std::uintptr_t set = *reinterpret_cast<const std::uintptr_t*>(manager + 0xD8 + slot * 8);
                if (!set)
                    return 3;
                const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(set + 0x690);
                const std::uintptr_t pool = *reinterpret_cast<const std::uintptr_t*>(g_player.poolBase);
                const std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(g_player.poolStride);
                if (count > 64 || !pool || !stride)
                    return 3;
                std::int32_t newInfo = 0;
                std::memcpy(&newInfo, record + 0x108, sizeof(newInfo));
                float position[3] = {};
                std::memcpy(position, record, sizeof(position));
                std::uint8_t* base8 = nullptr;
                bool already = false;
                for (std::uint32_t n = 0; n < count; ++n)
                {
                    const std::uint16_t index = *reinterpret_cast<const std::uint16_t*>(set + 0x610 + n * 2);
                    auto* held = reinterpret_cast<std::uint8_t*>(pool + static_cast<std::uintptr_t>(index) * stride);
                    const auto* heldPosition = reinterpret_cast<const float*>(held);
                    const float dx = heldPosition[0] - position[0];
                    const float dy = heldPosition[1] - position[1];
                    const float dz = heldPosition[2] - position[2];
                    if (dx * dx + dy * dy + dz * dz > 1.0e-6f)
                        continue;
                    const std::int32_t heldInfo = *reinterpret_cast<const std::int32_t*>(held + 0x108);
                    const std::int32_t heldNext = *reinterpret_cast<const std::int32_t*>(held + 0x10C);
                    if (heldNext == newInfo && heldInfo != newInfo)
                    {
                        base8 = held;
                        break;
                    }
                    if (heldInfo == newInfo)
                        already = true;
                }
                if (!base8)
                    return already ? 1 : 2;
                std::memcpy(base8 + 0x108, record + 0x108, 4);
                std::memcpy(base8 + 0x10C, record + 0x10C, 4);
                std::memcpy(base8 + 0x110, record + 0x110, 4);
                std::memcpy(base8 + 0x104, record + 0x104, 4);
                void* owner = ped;
                const std::uintptr_t ownerComp = *reinterpret_cast<const std::uintptr_t*>(set + 0x608);
                if (ownerComp)
                {
                    const std::uintptr_t entity = *reinterpret_cast<const std::uintptr_t*>(ownerComp + 0x08) & ~static_cast<std::uintptr_t>(0xF);
                    if (entity)
                        owner = reinterpret_cast<void*>(entity);
                }
                if (!engine::Call(engine::kBloodFinalize, finalize, static_cast<void*>(base8), owner, 0xFF))
                {
                    engine::Failed(code);
                    return -1;
                }
                return 0;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }
    }

    bool ApplyEvolve(void* ped, const std::uint8_t* record, bool& busy, std::string& error)
    {
        busy = false;
        if (!ped || !record)
        {
            error = "no_ped";
            return false;
        }
        if (!EnsurePlayer(error))
            return false;
        // The fields written into the held record (+0x104, +0x108, +0x10C,
        // +0x110) as the merge's own evolve and test use them.
        if (!sig::Find(kBloodEvolveWrite) || !sig::Find(kBloodEvolveCall) || !sig::Find(kBloodMergeNextInfo))
        {
            error = "blood_evolve_signature_mismatch";
            return false;
        }
        auto finalize = reinterpret_cast<std::uintptr_t>(g_original.load(std::memory_order_acquire));
        if (!finalize)
        {
            finalize = sig::Find(kBloodFinalize);
            if (!finalize)
            {
                error = "blood_finalize_signature_mismatch";
                return false;
            }
        }
        std::uint64_t gate[2] = {};
        const auto* gateObject = reinterpret_cast<const std::uint8_t*>(g_player.renderGate);
        if (!GuardedRead(&gate[0], gateObject + 0xB0, 8) || !GuardedRead(&gate[1], gateObject + 0xC0, 8))
        {
            error = "gate_unreadable";
            return false;
        }
        if (gate[0] || gate[1])
        {
            busy = true;
            error = "render_busy";
            return false;
        }
        std::uint32_t code = 0;
        const int result = GuardedEvolve(ped, record, finalize, code);
        switch (result)
        {
        case 0:
            return true;
        case 1:
            error = "already_evolved";
            return true;
        case 2:
            error = "no_base_record";
            return false;
        case 3:
            error = "no_set";
            return false;
        default:
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        }
    }

    namespace
    {
        bool GuardedReadSet(const void* ped, std::vector<HeldRecord>& out)
        {
            __try
            {
                bool faulted = false;
                void* comp = ReadComp(engine::kBloodCompRead, g_player.compRead, ped, faulted);
                if (faulted)
                    return false;
                if (!comp)
                    return true;
                const std::uint8_t slot = *(static_cast<const std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF || slot >= 128)
                    return true;
                const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(g_player.manager);
                if (!manager)
                    return true;
                const std::uintptr_t set = *reinterpret_cast<const std::uintptr_t*>(manager + 0xD8 + slot * 8);
                if (!set)
                    return true;
                const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(set + 0x690);
                if (count > 64)
                    return true;
                const std::uintptr_t pool = *reinterpret_cast<const std::uintptr_t*>(g_player.poolBase);
                const std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(g_player.poolStride);
                const float now = *reinterpret_cast<const float*>(g_player.engineTime);
                if (!pool || !stride)
                    return true;
                for (std::uint32_t n = 0; n < count; ++n)
                {
                    const std::uint16_t index = *reinterpret_cast<const std::uint16_t*>(set + 0x610 + n * 2);
                    const auto* record = reinterpret_cast<const std::uint8_t*>(pool + static_cast<std::uintptr_t>(index) * stride);
                    HeldRecord held;
                    std::memcpy(held.record, record, kRecordBytes);
                    float stamp = 0.0f;
                    std::memcpy(&stamp, record + kRecordTimeOffset, sizeof(stamp));
                    held.age = now - stamp;
                    held.pack = record[0x83] != 0;
                    out.push_back(held);
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool ReadRecords(const void* ped, std::vector<HeldRecord>& out, std::string& error)
    {
        out.clear();
        if (!ped)
        {
            error = "no_ped";
            return false;
        }
        if (!EnsurePlayer(error))
            return false;
        if (!GuardedReadSet(ped, out))
        {
            error = "set_unreadable";
            return false;
        }
        return true;
    }

    bool Clear(void* ped, std::string& error)
    {
        if (!ped)
        {
            error = "no_ped";
            return false;
        }
        if (!EnsurePlayer(error))
            return false;
        bool had = false;
        std::uint32_t code = 0;
        if (!GuardedClear(ped, had, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    void Shutdown()
    {
        g_observing.store(false, std::memory_order_relaxed);
        if (g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
    }
}
