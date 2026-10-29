#include "fire_observer.h"
#include "guarded_memory.h"
#include "mod_clock.h"
#include "profiler.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace replay::fire
{
    namespace
    {
        constexpr std::uintptr_t kManagerRva = 0x4C3F1E0; // the fire manager object itself
        constexpr std::size_t kFiresOffset = 0x10;
        constexpr std::size_t kRemnantsOffset = 0xF820;
        constexpr std::size_t kRemnantBytes = 0xF0;
        constexpr std::size_t kRemnantListOffset = 0x87820; // u16 indices
        constexpr std::size_t kRemnantCountOffset = 0x88820;
        constexpr std::uint32_t kRemnantMax = 2048;

        struct Hook
        {
            std::uintptr_t rva;
            std::uint8_t signature[24];
            void* target = nullptr;
        };
        Hook g_start{0x12B5EA4, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48,
                                 0x8D, 0xA8, 0x88, 0xF9, 0xFF, 0xFF, 0x48, 0x81}};
        Hook g_stop{0x12B2368, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57,
                                0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}};
        Hook g_remAdd{0x126E6D8, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
                                  0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC}};
        Hook g_remRemove{0x12B31C4, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                     0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20}};
        Hook g_fireUpdate{0x12BC2D4, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                      0x41, 0x57, 0x48, 0x8D, 0xA8, 0xF8, 0xFD, 0xFF}};
        Hook g_itemUpdate{0x12C0B88, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83,
                                      0xEC, 0x30, 0x8A, 0x81, 0xD1, 0x00, 0x00, 0x00}};
        Hook g_spread{0x12B3B54, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                  0x41, 0x57, 0x48, 0x8D, 0xA8, 0x48, 0xF3, 0xFF}};

        using StartFn = void*(__fastcall*)(void*, void*);
        using StopFn = void(__fastcall*)(void*, std::uint8_t, std::uint8_t);
        using RemAddFn = void*(__fastcall*)(void*, void*);
        using RemRemoveFn = void(__fastcall*)(void*, void*, std::uint8_t);
        using SpreadFn = std::uint8_t(__fastcall*)(void*, std::uint8_t);
        using FireUpdateFn = void(__fastcall*)(void*, float);
        using ItemUpdateFn = std::uint8_t(__fastcall*)(void*, const float*, void*);

        std::atomic<StartFn> g_origStart{nullptr};
        std::atomic<StopFn> g_origStop{nullptr};
        std::atomic<RemAddFn> g_origRemAdd{nullptr};
        std::atomic<RemRemoveFn> g_origRemRemove{nullptr};
        std::atomic<SpreadFn> g_origSpread{nullptr};
        std::atomic<FireUpdateFn> g_origFireUpdate{nullptr};
        std::atomic<ItemUpdateFn> g_origItemUpdate{nullptr};
        std::atomic<bool> g_freeze{false};
        constexpr std::uint32_t kFrozenMax = 512;
        std::atomic<std::uint64_t> g_frozen[kFrozenMax];
        std::atomic<std::uint32_t> g_frozenCount{0};

        bool IsFrozen(std::uint64_t owner)
        {
            if (!owner)
                return false;
            const std::uint32_t count = g_frozenCount.load(std::memory_order_acquire);
            for (std::uint32_t n = 0; n < count && n < kFrozenMax; ++n)
                if (g_frozen[n].load(std::memory_order_relaxed) == owner)
                    return true;
            return false;
        }
        std::atomic<bool> g_observing{false};
        std::atomic<bool> g_blockSpread{false};
        std::atomic<std::int32_t> g_inFlight{0};
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};
        bool g_installed = false;

        constexpr std::uint32_t kRingSize = 2048;
        using Slot = replay::ring::Slot<Call>; // event_ring.h
        Slot g_ring[kRingSize];
        std::atomic<std::uint64_t> g_next{0};

        struct InFlight
        {
            InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
            ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
        };

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

        std::uintptr_t Manager() { return g_base + kManagerRva; }

        std::uint32_t CallerRva()
        {
            void* frames[1] = {};
            const USHORT captured = RtlCaptureStackBackTrace(2, 1, frames, nullptr);
            const std::uintptr_t address = captured ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
            return address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
        }

        int SlotOfAddress(std::uintptr_t fire)
        {
            const std::uintptr_t first = Manager() + kFiresOffset;
            if (fire < first || fire >= first + kFireCount * kFireBytes || (fire - first) % kFireBytes)
                return -1;
            return static_cast<int>((fire - first) / kFireBytes);
        }

        int RemnantOfAddress(std::uintptr_t entry)
        {
            const std::uintptr_t first = Manager() + kRemnantsOffset;
            if (entry < first || entry >= first + kRemnantMax * kRemnantBytes || (entry - first) % kRemnantBytes)
                return -1;
            return static_cast<int>((entry - first) / kRemnantBytes);
        }

        // Live tracking (all the time): per fire slot the descriptor that
        // started it; per remnant the descriptor of its fire.
        struct LiveSlot
        {
            bool alive = false;
            std::uint8_t desc[kDescBytes] = {};
        };
        LiveSlot g_liveFires[kFireCount];
        struct LiveRemnantSlot
        {
            bool alive = false;
            std::uint8_t desc[kDescBytes] = {};
        };
        LiveRemnantSlot g_liveRemnants[kRemnantMax];
        SRWLOCK g_liveLock = SRWLOCK_INIT;

        // Starts and stops seen per slot, for WriteFireState (never 0 after a start,
        // so 0 means no start).
        std::atomic<std::uint32_t> g_fireGeneration[kFireCount] = {};

        void BumpGeneration(const void* fire)
        {
            const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
            if (slot < 0)
                return;
            if (g_fireGeneration[slot].fetch_add(1, std::memory_order_acq_rel) + 1 == 0)
                g_fireGeneration[slot].fetch_add(1, std::memory_order_acq_rel);
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

        void* __fastcall StartDetour(void* manager, void* desc)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.fire_start");
            InFlight inFlight;
            const bool observe = g_observing.load(std::memory_order_relaxed);
            alignas(16) std::uint8_t before[kDescBytes] = {};
            const bool copied = desc && GuardedRead(before, desc, kDescBytes);
            const StartFn original = g_origStart.load(std::memory_order_acquire);
            void* fire = original ? original(manager, desc) : nullptr;
            if (fire)
                BumpGeneration(fire);
            if (fire && copied)
            {
                const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
                if (slot >= 0)
                {
                    AcquireSRWLockExclusive(&g_liveLock);
                    g_liveFires[slot].alive = true;
                    std::memcpy(g_liveFires[slot].desc, before, kDescBytes);
                    ReleaseSRWLockExclusive(&g_liveLock);
                }
            }
            if (!observe || !copied)
                return fire;
            replay::ring::Writer<Call> slot(g_ring, kRingSize, g_next);
            Call* call = &slot.value;
            call->kind = fire ? CallKind::Start : CallKind::Refused;
            call->timeUs = QpcUs();
            call->fire = reinterpret_cast<std::uint64_t>(fire);
            call->slot = fire ? SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire)) : -1;
            call->remnant = -1;
            call->caller = CallerRva();
            std::memcpy(call->desc, before, kDescBytes);
            if (!fire || !GuardedRead(call->state, fire, kFireBytes))
                std::memset(call->state, 0, kFireBytes);
            slot.Publish();
            return fire;
        }

        void __fastcall StopDetour(void* fire, std::uint8_t instant, std::uint8_t arg3)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.fire_stop");
            InFlight inFlight;
            if (fire)
            {
                const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
                if (slot >= 0)
                {
                    AcquireSRWLockExclusive(&g_liveLock);
                    g_liveFires[slot].alive = false; // its desc stays for the remnant add below
                    ReleaseSRWLockExclusive(&g_liveLock);
                }
            }
            if (g_observing.load(std::memory_order_relaxed) && fire)
            {
                replay::ring::Writer<Call> slot(g_ring, kRingSize, g_next);
                Call* call = &slot.value;
                call->kind = CallKind::Stop;
                call->timeUs = QpcUs();
                call->fire = reinterpret_cast<std::uint64_t>(fire);
                call->slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
                call->remnant = -1;
                call->instant = instant;
                call->arg3 = arg3;
                call->caller = CallerRva();
                if (!GuardedRead(call->state, fire, kFireBytes))
                    std::memset(call->state, 0, kFireBytes);
                slot.Publish();
            }
            if (fire)
                BumpGeneration(fire);
            const StopFn original = g_origStop.load(std::memory_order_acquire);
            if (original)
                original(fire, instant, arg3);
        }

        void* __fastcall RemAddDetour(void* manager, void* fire)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            const RemAddFn original = g_origRemAdd.load(std::memory_order_acquire);
            void* entry = original ? original(manager, fire) : nullptr;
            if (entry && fire)
            {
                const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
                const int remnant = RemnantOfAddress(reinterpret_cast<std::uintptr_t>(entry));
                if (slot >= 0 && remnant >= 0)
                {
                    AcquireSRWLockExclusive(&g_liveLock);
                    g_liveRemnants[remnant].alive = true;
                    std::memcpy(g_liveRemnants[remnant].desc, g_liveFires[slot].desc, kDescBytes);
                    ReleaseSRWLockExclusive(&g_liveLock);
                }
            }
            if (g_observing.load(std::memory_order_relaxed))
            {
                replay::ring::Writer<Call> slot(g_ring, kRingSize, g_next);
                Call* call = &slot.value;
                call->kind = CallKind::RemnantAdd;
                call->timeUs = QpcUs();
                call->fire = reinterpret_cast<std::uint64_t>(fire);
                call->slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
                call->remnant = entry ? RemnantOfAddress(reinterpret_cast<std::uintptr_t>(entry)) : -1;
                call->caller = CallerRva();
                slot.Publish();
            }
            return entry;
        }

        void __fastcall RemRemoveDetour(void* manager, void* entry, std::uint8_t killGroup)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (entry)
            {
                const int remnant = RemnantOfAddress(reinterpret_cast<std::uintptr_t>(entry));
                if (remnant >= 0)
                {
                    AcquireSRWLockExclusive(&g_liveLock);
                    g_liveRemnants[remnant].alive = false;
                    ReleaseSRWLockExclusive(&g_liveLock);
                }
            }
            if (g_observing.load(std::memory_order_relaxed) && entry)
            {
                replay::ring::Writer<Call> slot(g_ring, kRingSize, g_next);
                Call* call = &slot.value;
                call->kind = CallKind::RemnantRemove;
                call->timeUs = QpcUs();
                call->fire = 0;
                call->slot = -1;
                call->remnant = RemnantOfAddress(reinterpret_cast<std::uintptr_t>(entry));
                call->arg3 = killGroup;
                call->caller = CallerRva();
                slot.Publish();
            }
            const RemRemoveFn original = g_origRemRemove.load(std::memory_order_acquire);
            if (original)
                original(manager, entry, killGroup);
        }

        std::uint8_t __fastcall SpreadDetour(void* fire, std::uint8_t arg)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (g_blockSpread.load(std::memory_order_relaxed))
                return 0;
            const SpreadFn original = g_origSpread.load(std::memory_order_acquire);
            return original ? original(fire, arg) : 0;
        }

        void __fastcall FireUpdateDetour(void* fire, float dt)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (g_freeze.load(std::memory_order_relaxed) && IsFrozen(reinterpret_cast<std::uint64_t>(fire)))
                dt = 0.0f;
            const FireUpdateFn original = g_origFireUpdate.load(std::memory_order_acquire);
            if (original)
                original(fire, dt);
        }

        std::uint8_t __fastcall ItemUpdateDetour(void* item, const float* dt, void* out)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            alignas(16) static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (g_freeze.load(std::memory_order_relaxed) && item)
            {
                std::uint64_t owner = 0;
                if (GuardedRead(&owner, static_cast<std::uint8_t*>(item) + 0xB8, sizeof(owner)) && IsFrozen(owner))
                    dt = kZero;
            }
            const ItemUpdateFn original = g_origItemUpdate.load(std::memory_order_acquire);
            return original ? original(item, dt, out) : 0;
        }

        bool HookOne(Hook& hook, void* detour, void** trampoline, std::string& error, const char* name)
        {
            void* target = reinterpret_cast<void*>(g_base + hook.rva);
            if (!GuardedCompare(target, hook.signature, sizeof(hook.signature)))
            {
                error = std::string(name) + "_signature_mismatch";
                return false;
            }
            MH_STATUS status = MH_CreateHook(target, detour, trampoline);
            if (status != MH_OK || !*trampoline)
            {
                error = std::string(name) + "_mh_create_" + MH_StatusToString(status);
                return false;
            }
            status = replay::hooks::Enable(target);
            if (status != MH_OK)
            {
                MH_RemoveHook(target);
                error = std::string(name) + "_mh_enable_" + MH_StatusToString(status);
                return false;
            }
            hook.target = target;
            return true;
        }

        void UnhookOne(Hook& hook)
        {
            if (!hook.target)
                return;
            MH_DisableHook(hook.target);
            MH_RemoveHook(hook.target);
            hook.target = nullptr;
        }

        int RecordException(EXCEPTION_POINTERS* pointers, std::uint32_t& code);
        bool GuardedStart(StartFn fn, void* manager, std::uint8_t* desc, void*& fire, std::uint32_t& code)
        {
            __try
            {
                fire = fn(manager, desc);
                return true;
            }
            __except (RecordException(GetExceptionInformation(), code))
            {
                return false;
            }
        }

        bool GuardedStop(StopFn fn, void* fire, std::uint8_t instant, std::uint8_t arg3, std::uint32_t& code)
        {
            __try
            {
                fn(fire, instant, arg3);
                return true;
            }
            __except (RecordException(GetExceptionInformation(), code))
            {
                return false;
            }
        }

        bool GuardedRemRemove(RemRemoveFn fn, void* manager, void* entry, std::uint32_t& code)
        {
            __try
            {
                fn(manager, entry, 0);
                return true;
            }
            __except (RecordException(GetExceptionInformation(), code))
            {
                return false;
            }
        }

        // The faulting address of the last guarded call (RVA when inside the
        // game image), for the log: code alone does not say which call failed.
        std::uintptr_t g_lastExceptionAddress = 0;
        int RecordException(EXCEPTION_POINTERS* pointers, std::uint32_t& code)
        {
            code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0;
            g_lastExceptionAddress = pointers && pointers->ExceptionRecord
                ? reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress) : 0;
            return EXCEPTION_EXECUTE_HANDLER;
        }
        std::string ExceptionText(std::uint32_t code)
        {
            char text[64]{};
            const std::uintptr_t at = g_lastExceptionAddress;
            if (at >= g_base && at < g_imageEnd)
                std::snprintf(text, sizeof(text), "exception_%08X@rva_%llX", code, static_cast<unsigned long long>(at - g_base));
            else
                std::snprintf(text, sizeof(text), "exception_%08X@abs_%llX", code, static_cast<unsigned long long>(at));
            return text;
        }
    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        for (Hook* hook : {&g_start, &g_stop, &g_remAdd, &g_remRemove, &g_spread, &g_fireUpdate, &g_itemUpdate})
        {
            if (!GuardedCompare(reinterpret_cast<void*>(g_base + hook->rva), hook->signature, sizeof(hook->signature)))
            {
                char text[64]{};
                std::snprintf(text, sizeof(text), "fire_signature_mismatch_0x%X", static_cast<unsigned>(hook->rva));
                error = text;
                return false;
            }
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        if (!HookOne(g_start, reinterpret_cast<void*>(&StartDetour), &trampoline, error, "fire_start"))
            return false;
        g_origStart.store(reinterpret_cast<StartFn>(trampoline));
        trampoline = nullptr;
        if (!HookOne(g_stop, reinterpret_cast<void*>(&StopDetour), &trampoline, error, "fire_stop"))
        {
            Shutdown();
            return false;
        }
        g_origStop.store(reinterpret_cast<StopFn>(trampoline));
        trampoline = nullptr;
        if (!HookOne(g_remAdd, reinterpret_cast<void*>(&RemAddDetour), &trampoline, error, "remnant_add"))
        {
            Shutdown();
            return false;
        }
        g_origRemAdd.store(reinterpret_cast<RemAddFn>(trampoline));
        trampoline = nullptr;
        if (!HookOne(g_remRemove, reinterpret_cast<void*>(&RemRemoveDetour), &trampoline, error, "remnant_remove"))
        {
            Shutdown();
            return false;
        }
        g_origRemRemove.store(reinterpret_cast<RemRemoveFn>(trampoline));
        trampoline = nullptr;
        if (!HookOne(g_spread, reinterpret_cast<void*>(&SpreadDetour), &trampoline, error, "fire_spread"))
        {
            Shutdown();
            return false;
        }
        g_origSpread.store(reinterpret_cast<SpreadFn>(trampoline));
        trampoline = nullptr;
        if (!HookOne(g_fireUpdate, reinterpret_cast<void*>(&FireUpdateDetour), &trampoline, error, "fire_update"))
        {
            Shutdown();
            return false;
        }
        g_origFireUpdate.store(reinterpret_cast<FireUpdateFn>(trampoline));
        trampoline = nullptr;
        if (!HookOne(g_itemUpdate, reinterpret_cast<void*>(&ItemUpdateDetour), &trampoline, error, "area_item_update"))
        {
            Shutdown();
            return false;
        }
        g_origItemUpdate.store(reinterpret_cast<ItemUpdateFn>(trampoline));
        g_installed = true;
        return true;
    }

    void SetObserving(bool on) { g_observing.store(on, std::memory_order_relaxed); }

    void SnapshotLive(std::vector<LiveFire>& fires, std::vector<LiveRemnant>& remnants)
    {
        fires.clear();
        remnants.clear();
        if (!g_installed)
            return;
        AcquireSRWLockShared(&g_liveLock);
        for (int slot = 0; slot < static_cast<int>(kFireCount); ++slot)
        {
            if (!g_liveFires[slot].alive)
                continue;
            LiveFire live;
            live.slot = slot;
            live.fire = FireAddress(slot);
            std::memcpy(live.desc, g_liveFires[slot].desc, kDescBytes);
            if (GuardedRead(live.state, reinterpret_cast<void*>(live.fire), kFireBytes))
                fires.push_back(live);
        }
        for (int index = 0; index < static_cast<int>(kRemnantMax); ++index)
        {
            if (!g_liveRemnants[index].alive)
                continue;
            LiveRemnant live;
            live.index = index;
            live.entry = RemnantAddress(index);
            std::memcpy(live.desc, g_liveRemnants[index].desc, kDescBytes);
            if (GuardedRead(live.position, reinterpret_cast<void*>(live.entry + 0x30), sizeof(live.position)))
                remnants.push_back(live);
        }
        ReleaseSRWLockShared(&g_liveLock);
    }
    void SetBlockSpread(bool on) { g_blockSpread.store(on, std::memory_order_relaxed); }

    void SetFreeze(bool on) { g_freeze.store(on, std::memory_order_relaxed); }

    void SetFrozenOwners(const std::vector<std::uint64_t>& owners)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(owners.size(), static_cast<std::size_t>(kFrozenMax)));
        g_frozenCount.store(0, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_frozen[n].store(owners[n], std::memory_order_relaxed);
        g_frozenCount.store(count, std::memory_order_release);
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
    }

    std::uint64_t TotalCalls() { return g_next.load(std::memory_order_acquire); }

    void CollectSince(std::uint64_t& cursor, std::vector<Call>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_ring, kRingSize, g_next, cursor, [&](const Call& call) { out.push_back(call); });
    }

    void* StartFire(const std::uint8_t* desc, std::string& error)
    {
        const StartFn fn = g_origStart.load(std::memory_order_acquire);
        if (!fn)
        {
            error = "fire_not_installed";
            return nullptr;
        }
        alignas(16) std::uint8_t copy[kDescBytes + 0x20] = {};
        std::memcpy(copy, desc, kDescBytes);
        void* fire = nullptr;
        std::uint32_t code = 0;
        if (!GuardedStart(fn, reinterpret_cast<void*>(Manager()), copy, fire, code))
        {
            error = ExceptionText(code);
            return nullptr;
        }
        if (!fire)
            error = "engine_refused";
        else
            BumpGeneration(fire); // the trampoline skips StartDetour
        return fire;
    }

    bool StopFire(void* fire, bool instant, std::uint8_t arg3, std::string& error)
    {
        const StopFn fn = g_origStop.load(std::memory_order_acquire);
        if (!fn || !fire)
        {
            error = "fire_not_installed";
            return false;
        }
        BumpGeneration(fire); // the trampoline skips StopDetour
        // Nor the live tracking (the clip-aware clear stops world fires the detour saw
        // start).
        const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
        if (slot >= 0)
        {
            AcquireSRWLockExclusive(&g_liveLock);
            g_liveFires[slot].alive = false;
            ReleaseSRWLockExclusive(&g_liveLock);
        }
        std::uint32_t code = 0;
        if (!GuardedStop(fn, fire, instant ? 1 : 0, arg3, code))
        {
            error = ExceptionText(code);
            return false;
        }
        return true;
    }

    bool EngineLiveCounts(std::uint32_t& fires, std::uint32_t& remnants)
    {
        constexpr std::size_t kActiveCountOffset = 0xB18B8;
        fires = 0;
        remnants = 0;
        if (!g_installed)
            return false;
        const std::uintptr_t manager = Manager();
        std::int32_t active = 0;
        if (!GuardedRead(&active, reinterpret_cast<void*>(manager + kActiveCountOffset), sizeof(active)) || active < 0 ||
            active > static_cast<std::int32_t>(kFireCount))
            return false;
        std::uint32_t left = 0;
        if (!GuardedRead(&left, reinterpret_cast<void*>(manager + kRemnantCountOffset), sizeof(left)) || left > kRemnantMax)
            return false;
        fires = static_cast<std::uint32_t>(active);
        remnants = left;
        return true;
    }

    bool ReadActiveFires(std::vector<EngineFire>& out)
    {
        constexpr std::size_t kActiveCountOffset = 0xB18B8;
        constexpr std::size_t kActiveListOffset = 0xB18BC;
        out.clear();
        if (!g_installed)
            return false;
        const std::uintptr_t manager = Manager();
        std::int32_t active = 0;
        if (!GuardedRead(&active, reinterpret_cast<void*>(manager + kActiveCountOffset), sizeof(active)) || active < 0 ||
            active > static_cast<std::int32_t>(kFireCount))
            return false;
        std::int32_t slots[kFireCount] = {};
        if (active && !GuardedRead(slots, reinterpret_cast<void*>(manager + kActiveListOffset), active * sizeof(std::int32_t)))
            return false;
        bool seen[kFireCount] = {};
        out.reserve(static_cast<std::size_t>(active));
        for (std::int32_t n = 0; n < active; ++n)
        {
            const std::int32_t slot = slots[n];
            if (slot < 0 || slot >= static_cast<std::int32_t>(kFireCount) || seen[slot])
                continue;
            seen[slot] = true;
            EngineFire fire;
            fire.slot = slot;
            fire.fire = FireAddress(slot);
            if (!GuardedRead(fire.state, reinterpret_cast<void*>(fire.fire), kFireBytes))
                continue;
            out.push_back(fire);
        }
        AcquireSRWLockShared(&g_liveLock);
        for (EngineFire& fire : out)
            fire.tracked = g_liveFires[fire.slot].alive;
        ReleaseSRWLockShared(&g_liveLock);
        return true;
    }

    int ForgetLiveFires()
    {
        int forgotten = 0;
        AcquireSRWLockExclusive(&g_liveLock);
        for (LiveSlot& slot : g_liveFires)
        {
            forgotten += slot.alive ? 1 : 0;
            slot.alive = false;
        }
        ReleaseSRWLockExclusive(&g_liveLock);
        return forgotten;
    }

    int ClearRemnants(const float* center, float radius, std::string& error)
    {
        const RemRemoveFn fn = g_origRemRemove.load(std::memory_order_acquire);
        if (!fn)
        {
            error = "fire_not_installed";
            return 0;
        }
        const std::uintptr_t manager = Manager();
        std::uint32_t count = 0;
        if (!GuardedRead(&count, reinterpret_cast<void*>(manager + kRemnantCountOffset), sizeof(count)) || count > kRemnantMax)
        {
            error = "remnant_count_unreadable";
            return 0;
        }
        std::uint16_t indices[kRemnantMax] = {};
        if (count && !GuardedRead(indices, reinterpret_cast<void*>(manager + kRemnantListOffset), count * sizeof(std::uint16_t)))
        {
            error = "remnant_list_unreadable";
            return 0;
        }
        std::vector<std::uintptr_t> doomed;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            if (indices[n] >= kRemnantMax)
                continue;
            const std::uintptr_t entry = manager + kRemnantsOffset + indices[n] * kRemnantBytes;
            float pos[3] = {};
            if (!GuardedRead(pos, reinterpret_cast<void*>(entry + 0x30), sizeof(pos)))
                continue;
            const float dx = pos[0] - center[0];
            const float dy = pos[1] - center[1];
            const float dz = pos[2] - center[2];
            if (dx * dx + dy * dy + dz * dz <= radius * radius)
                doomed.push_back(entry);
        }
        int removed = 0;
        for (const std::uintptr_t entry : doomed)
        {
            std::uint32_t code = 0;
            if (GuardedRemRemove(fn, reinterpret_cast<void*>(manager), reinterpret_cast<void*>(entry), code))
                ++removed;
            else
                error = ExceptionText(code);
        }
        return removed;
    }

    namespace
    {
        constexpr std::uintptr_t kAreaRva = 0x4E2F5B0;
        constexpr std::uintptr_t kAreaClearRva = 0x12A35E4;
        constexpr std::uint8_t kAreaClearSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57,
            0x48, 0x83, 0xEC, 0x60, 0x41, 0x0F, 0x28, 0x00, 0x49, 0x8B, 0xD8, 0x0F, 0x28, 0x0A, 0x4C, 0x8D};
        using AreaClearFn = void(__fastcall*)(void*, const float*, const float*, std::uint8_t);

        bool GuardedAreaClear(AreaClearFn fn, void* area, const float* center, const float* radius, std::uint32_t& code)
        {
            __try
            {
                fn(area, center, radius, 1);
                return true;
            }
            __except (RecordException(GetExceptionInformation(), code))
            {
                return false;
            }
        }
    }

    bool ClearBurntGround(const float* center, float radius, std::string& error)
    {
        if (!g_base)
        {
            error = "fire_not_installed";
            return false;
        }
        const auto* target = reinterpret_cast<const std::uint8_t*>(g_base + kAreaClearRva);
        if (!GuardedCompare(target, kAreaClearSignature, sizeof(kAreaClearSignature)))
        {
            error = "area_clear_signature_mismatch";
            return false;
        }
        alignas(16) float c[4] = {center[0], center[1], center[2], 0.0f};
        alignas(16) float r[4] = {radius, radius, radius, radius};
        std::uint32_t code = 0;
        if (!GuardedAreaClear(reinterpret_cast<AreaClearFn>(g_base + kAreaClearRva),
                reinterpret_cast<void*>(g_base + kAreaRva), c, r, code))
        {
            error = ExceptionText(code);
            return false;
        }
        return true;
    }

    namespace
    {
        constexpr std::size_t kAreaItemsPtr = 0x58;
        constexpr std::uint32_t kAreaItems = 2048;
        constexpr std::size_t kAreaItemBytes = 0xE0;

        bool GuardedReadItems(std::uintptr_t storage, std::vector<AreaItem>& out)
        {
            __try
            {
                for (std::uint32_t n = 0; n < kAreaItems; ++n)
                {
                    const auto* item = reinterpret_cast<const std::uint8_t*>(storage + n * kAreaItemBytes);
                    const std::uint8_t flags = item[0xD0];
                    const std::uint64_t owner = *reinterpret_cast<const std::uint64_t*>(item + 0xB8);
                    if ((flags & 0x20) || !(flags & 1) || !owner)
                        continue;
                    AreaItem info;
                    info.owner = owner;
                    info.primary = *reinterpret_cast<const std::uint64_t*>(item + 0xB0) == 0;
                    std::memcpy(info.center, item + 0x30, sizeof(info.center));
                    std::memcpy(&info.outer, item + 0x3C, 4);
                    std::memcpy(&info.burnt, item + 0x64, 4);
                    std::memcpy(&info.width, item + 0x6C, 4);
                    std::memcpy(&info.ember, item + 0x80, 4);
                    std::memcpy(&info.glow, item + 0x88, 4);
                    std::memcpy(&info.glowStart, item + 0x8C, 4);
                    std::memcpy(&info.state, item + 0x60, 4);
                    info.flags = flags;
                    out.push_back(info);
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedWriteItems(std::uintptr_t storage, std::vector<AreaItemWrite>& writes)
        {
            __try
            {
                for (std::uint32_t n = 0; n < kAreaItems; ++n)
                {
                    auto* item = reinterpret_cast<std::uint8_t*>(storage + n * kAreaItemBytes);
                    const std::uint8_t flags = item[0xD0];
                    if ((flags & 0x20) || !(flags & 1) || *reinterpret_cast<const std::uint64_t*>(item + 0xB0) != 0)
                        continue;
                    const std::uint64_t owner = *reinterpret_cast<const std::uint64_t*>(item + 0xB8);
                    if (!owner)
                        continue;
                    for (AreaItemWrite& write : writes)
                    {
                        if (write.done || write.owner != owner)
                            continue;
                        std::memcpy(item + 0x3C, &write.outer, 4);
                        float bound = 0.0f;
                        std::memcpy(&bound, item + 0x4C, 4);
                        if (bound < write.outer)
                            std::memcpy(item + 0x4C, &write.outer, 4);
                        std::memcpy(item + 0x64, &write.burnt, 4);
                        if (write.ember >= 0.0f)
                            std::memcpy(item + 0x80, &write.ember, 4);
                        if (write.glow >= 0.0f)
                            std::memcpy(item + 0x88, &write.glow, 4);
                        if (write.width >= 0.0f)
                            std::memcpy(item + 0x6C, &write.width, 4);
                        write.done = true;
                        break;
                    }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::uintptr_t AreaItemStorage()
        {
            std::uintptr_t storage = 0;
            GuardedRead(&storage, reinterpret_cast<void*>(g_base + kAreaRva + kAreaItemsPtr), sizeof(storage));
            return storage;
        }
    }

    bool ReadAreaItems(std::vector<AreaItem>& out, std::string& error)
    {
        out.clear();
        if (!g_base)
        {
            error = "fire_not_installed";
            return false;
        }
        const std::uintptr_t storage = AreaItemStorage();
        if (!storage || !GuardedReadItems(storage, out))
        {
            error = "area_items_unreadable";
            return false;
        }
        return true;
    }

    bool WriteAreaItems(std::vector<AreaItemWrite>& writes, std::string& error)
    {
        if (!g_base)
        {
            error = "fire_not_installed";
            return false;
        }
        const std::uintptr_t storage = AreaItemStorage();
        if (!storage || !GuardedWriteItems(storage, writes))
        {
            error = "area_items_unwritable";
            return false;
        }
        return true;
    }

    std::uint64_t FireAddress(int slot)
    {
        return slot < 0 ? 0 : static_cast<std::uint64_t>(Manager() + kFiresOffset + static_cast<std::uintptr_t>(slot) * kFireBytes);
    }

    std::uint64_t RemnantAddress(int index)
    {
        return index < 0 ? 0 : static_cast<std::uint64_t>(Manager() + kRemnantsOffset + static_cast<std::uintptr_t>(index) * kRemnantBytes);
    }

    int SlotOf(const void* fire) { return SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire)); }

    std::uint32_t FireGeneration(const void* fire)
    {
        const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
        return slot < 0 ? 0 : g_fireGeneration[slot].load(std::memory_order_acquire);
    }

    bool WriteFireState(void* fire, std::uint32_t startGeneration, std::size_t offset, const void* data,
        std::size_t size, std::string& error)
    {
        if (!g_installed)
        {
            error = "fire_not_installed"; // the layout gate: signatures verified
            return false;
        }
        if (!data || !FireStateFieldAllowed(offset, size))
        {
            error = "fire_field_unknown";
            return false;
        }
        const int slot = SlotOfAddress(reinterpret_cast<std::uintptr_t>(fire));
        if (slot < 0)
        {
            error = "fire_outside_pool";
            return false;
        }
        if (!FireOwnedBy(g_fireGeneration[slot].load(std::memory_order_acquire), startGeneration))
        {
            error = "fire_not_owned";
            return false;
        }
        if (!GuardedWrite(static_cast<std::uint8_t*>(fire) + offset, data, size))
        {
            error = "fire_write_fault";
            return false;
        }
        return true;
    }

    void Shutdown()
    {
        g_observing.store(false, std::memory_order_relaxed);
        g_blockSpread.store(false, std::memory_order_relaxed);
        g_freeze.store(false, std::memory_order_relaxed);
        UnhookOne(g_itemUpdate);
        UnhookOne(g_fireUpdate);
        UnhookOne(g_spread);
        UnhookOne(g_remRemove);
        UnhookOne(g_remAdd);
        UnhookOne(g_stop);
        UnhookOne(g_start);
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_origStart.store(nullptr);
        g_origStop.store(nullptr);
        g_origRemAdd.store(nullptr);
        g_origRemRemove.store(nullptr);
        g_origSpread.store(nullptr);
        g_origFireUpdate.store(nullptr);
        g_origItemUpdate.store(nullptr);
        g_installed = false;
    }
}
