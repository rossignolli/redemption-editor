#include "ped_blood.h"
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
        constexpr std::uintptr_t kFinalizeRva = 0x1285DA4;
        constexpr std::uint8_t kFinalizeSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xC0};
        constexpr std::uintptr_t kPoolBaseRva = 0x4C3CE70;   // record pool base pointer
        constexpr std::uintptr_t kPoolStrideRva = 0x4C3CE88; // record size
        constexpr std::uintptr_t kEngineTimeRva = 0x5429034; // float seconds the records age against
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
        LARGE_INTEGER g_frequency{};

        std::uint64_t QpcUs()
        {
            return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
        }

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size); // guarded_memory.h
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
            if (GuardedRead(&poolBase, reinterpret_cast<void*>(g_base + kPoolBaseRva), sizeof(poolBase)) &&
                GuardedRead(&stride, reinterpret_cast<void*>(g_base + kPoolStrideRva), sizeof(stride)) && stride &&
                reinterpret_cast<std::uintptr_t>(record) >= poolBase)
            {
                call.index = static_cast<std::int32_t>((reinterpret_cast<std::uintptr_t>(record) - poolBase) / stride);
            }
            if (call.copied && GuardedRead(&engineTime, reinterpret_cast<void*>(g_base + kEngineTimeRva), sizeof(engineTime)))
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
        void* target = base + kFinalizeRva;
        if (!GuardedCompare(target, kFinalizeSignature, sizeof(kFinalizeSignature)))
        {
            error = "blood_finalize_signature_mismatch";
            return false;
        }
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
        constexpr std::uintptr_t kManagerRva = 0x4C38A18;   // pointer to the ped damage manager
        constexpr std::uintptr_t kRenderGateRva = 0x5A9D690; // object whose +0xB0/+0xC0 are null when idle
        constexpr std::uintptr_t kCompCreateRva = 0xCC5A30;
        constexpr std::uint8_t kCompCreateSignature[] = {
            0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0x64, 0x24, 0x30, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x4C,
            0x8B, 0xC1, 0x48, 0x81, 0xC1, 0x9C, 0x00, 0x00};
        constexpr std::uintptr_t kCompReadRva = 0xCC8848;
        constexpr std::uint8_t kCompReadSignature[] = {
            0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00, 0x25, 0xFF, 0xFF, 0x01, 0x00, 0x2B};
        constexpr std::uintptr_t kSlotAllocRva = 0x126AB48;
        constexpr std::uint8_t kSlotAllocSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80,
            0x7A, 0x10, 0xFF, 0x48, 0x8B, 0xFA, 0x48, 0x8B};
        constexpr std::uintptr_t kAddRva = 0x125D470;
        constexpr std::uint8_t kAddSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x45,
            0x8A, 0xD1, 0x4D, 0x8B, 0xE8, 0x44, 0x8A, 0xF2};
        constexpr std::uintptr_t kClearRva = 0xE1F5A0;
        constexpr std::uint8_t kClearSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8A, 0x51, 0x10, 0x48, 0x8B, 0xD9, 0x80, 0xFA, 0xFF, 0x74};
        constexpr std::size_t kCompSlot = 0x10;
        constexpr std::size_t kRecordOwnerRef = 0x78;
        using CompFn = void*(__fastcall*)(void*);
        using AllocFn = void(__fastcall*)(void*, void*);
        using AddFn = void*(__fastcall*)(void*, std::uint8_t, const void*, std::uint8_t);
        using ClearFn = void(__fastcall*)(void*);

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

        void* ReadComp(std::uintptr_t base, std::uintptr_t rva, const void* ped)
        {
            if (!Readable(static_cast<const std::uint8_t*>(ped) + 0x9C, sizeof(std::uint32_t)))
                return nullptr;
            void* comp = reinterpret_cast<CompFn>(base + rva)(const_cast<void*>(ped));
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
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!g_base)
                g_base = reinterpret_cast<std::uintptr_t>(base);
            g_playerReady = GuardedCompare(base + kCompCreateRva, kCompCreateSignature, sizeof(kCompCreateSignature)) &&
                GuardedCompare(base + kCompReadRva, kCompReadSignature, sizeof(kCompReadSignature)) &&
                GuardedCompare(base + kSlotAllocRva, kSlotAllocSignature, sizeof(kSlotAllocSignature)) &&
                GuardedCompare(base + kAddRva, kAddSignature, sizeof(kAddSignature)) &&
                GuardedCompare(base + kClearRva, kClearSignature, sizeof(kClearSignature));
            if (!g_playerReady)
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
                const std::uintptr_t base = g_base;
                void* manager = *reinterpret_cast<void**>(base + kManagerRva);
                if (!manager)
                    return false;
                void* comp = ReadComp(base, kCompCreateRva, ped);
                if (!comp)
                    return false;
                std::uint8_t slot = *(reinterpret_cast<std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF)
                {
                    const std::uint64_t allocStart = QpcTicks();
                    reinterpret_cast<AllocFn>(base + kSlotAllocRva)(manager, comp);
                    g_lastAllocTicks = QpcTicks() - allocStart;
                    g_lastAllocated = true;
                    slot = *(reinterpret_cast<std::uint8_t*>(comp) + kCompSlot);
                }
                slotOut = slot;
                if (slot == 0xFF)
                    return false;
                const std::uint64_t addStart = QpcTicks();
                added = reinterpret_cast<AddFn>(base + kAddRva)(manager, slot, buffer, pack ? 1 : 0);
                g_lastAddTicks = QpcTicks() - addStart;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The engine's own merge test (0x125D86C, offline): a record of the
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
                const std::uintptr_t base = g_base;
                void* comp = ReadComp(base, kCompReadRva, ped);
                if (!comp)
                    return true;
                const std::uint8_t slot = *(static_cast<const std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF || slot >= 128)
                    return true;
                const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(base + kManagerRva);
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
                const std::uintptr_t pool = *reinterpret_cast<const std::uintptr_t*>(base + kPoolBaseRva);
                const std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(base + kPoolStrideRva);
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
                void* comp = ReadComp(g_base, kCompReadRva, ped);
                had = comp != nullptr;
                if (comp)
                    reinterpret_cast<ClearFn>(g_base + kClearRva)(comp);
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
        constexpr std::uintptr_t kComponentIndexBaseRva = 0x39ECE40; // u32
        constexpr std::uintptr_t kComponentTableRva = 0x39ECE48;     // u64 pointer
        constexpr std::size_t kComponentStride = 0x148;
        constexpr std::size_t kSurfaceSlot = 0x98;
        constexpr std::uintptr_t kDirtSetterRva[3] = {0x8E1624, 0x8E15E4, 0x8E15A4};
        constexpr std::uint8_t kDirtSetterSignature[3][16] = {
            {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x89, 0x91, 0xA8, 0x03, 0x00, 0x00, 0x48},
            {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x89, 0x91, 0xAC, 0x03, 0x00, 0x00, 0x48},
            {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x89, 0x91, 0xB0, 0x03, 0x00, 0x00, 0x48},
        };
        using DirtSetterFn = void(__fastcall*)(void* component, std::int32_t value);
        int g_dirtSettersChecked = 0; // 1 ok, -1 mismatch

        std::uintptr_t SurfaceComponent(void* ped)
        {
            if (!ped)
                return 0;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::uint32_t handle = 0;
            std::uint32_t indexBase = 0;
            std::uintptr_t table = 0;
            if (!GuardedRead(&handle, static_cast<std::uint8_t*>(ped) + 0x9C, 4) ||
                !GuardedRead(&indexBase, reinterpret_cast<void*>(base + kComponentIndexBaseRva), 4) ||
                !GuardedRead(&table, reinterpret_cast<void*>(base + kComponentTableRva), 8) || !table)
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

        bool GuardedDirt(DirtSetterFn fn, void* component, std::int32_t value)
        {
            __try
            {
                fn(component, value);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        if (!GuardedWrite(reinterpret_cast<void*>(comp + 0x30), surface.wet, sizeof(surface.wet)) ||
            !GuardedWrite(reinterpret_cast<void*>(comp + 0x40), &surface.wetFlags, 1))
        {
            error = "wet_write_fault";
            return false;
        }
        if (!dirt)
            return true;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_dirtSettersChecked)
        {
            g_dirtSettersChecked = 1;
            for (int n = 0; n < 3; ++n)
                if (!GuardedCompare(reinterpret_cast<void*>(base + kDirtSetterRva[n]), kDirtSetterSignature[n], 16))
                    g_dirtSettersChecked = -1;
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
            if (!GuardedDirt(reinterpret_cast<DirtSetterFn>(base + kDirtSetterRva[n]), reinterpret_cast<void*>(comp), surface.dirt[n]))
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
        const auto* gateObject = reinterpret_cast<const std::uint8_t*>(g_base + kRenderGateRva);
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
        if (GuardedRead(&engineTime, reinterpret_cast<void*>(g_base + kEngineTimeRva), sizeof(engineTime)))
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
        int GuardedEvolve(std::uintptr_t base, void* ped, const std::uint8_t* record, FinalizeFn finalize, std::uint32_t& code)
        {
            __try
            {
                void* comp = ReadComp(base, kCompReadRva, ped);
                if (!comp)
                    return 3;
                const std::uint8_t slot = *(static_cast<const std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF || slot >= 128)
                    return 3;
                const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(base + kManagerRva);
                if (!manager)
                    return 3;
                const std::uintptr_t set = *reinterpret_cast<const std::uintptr_t*>(manager + 0xD8 + slot * 8);
                if (!set)
                    return 3;
                const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(set + 0x690);
                const std::uintptr_t pool = *reinterpret_cast<const std::uintptr_t*>(base + kPoolBaseRva);
                const std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(base + kPoolStrideRva);
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
                finalize(base8, owner, 0xFF);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        FinalizeFn finalize = g_original.load(std::memory_order_acquire);
        if (!finalize)
        {
            if (!GuardedCompare(base + kFinalizeRva, kFinalizeSignature, sizeof(kFinalizeSignature)))
            {
                error = "blood_finalize_signature_mismatch";
                return false;
            }
            finalize = reinterpret_cast<FinalizeFn>(base + kFinalizeRva);
        }
        std::uint64_t gate[2] = {};
        const auto* gateObject = reinterpret_cast<const std::uint8_t*>(g_base + kRenderGateRva);
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
        const int result = GuardedEvolve(g_base, ped, record, finalize, code);
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
        bool GuardedReadSet(std::uintptr_t base, const void* ped, std::vector<HeldRecord>& out)
        {
            __try
            {
                void* comp = ReadComp(base, kCompReadRva, ped);
                if (!comp)
                    return true;
                const std::uint8_t slot = *(static_cast<const std::uint8_t*>(comp) + kCompSlot);
                if (slot == 0xFF || slot >= 128)
                    return true;
                const std::uintptr_t manager = *reinterpret_cast<const std::uintptr_t*>(base + kManagerRva);
                if (!manager)
                    return true;
                const std::uintptr_t set = *reinterpret_cast<const std::uintptr_t*>(manager + 0xD8 + slot * 8);
                if (!set)
                    return true;
                const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(set + 0x690);
                if (count > 64)
                    return true;
                const std::uintptr_t pool = *reinterpret_cast<const std::uintptr_t*>(base + kPoolBaseRva);
                const std::uint32_t stride = *reinterpret_cast<const std::uint32_t*>(base + kPoolStrideRva);
                const float now = *reinterpret_cast<const float*>(base + kEngineTimeRva);
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
        if (!GuardedReadSet(g_base, ped, out))
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
