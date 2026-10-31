#include "fire_observer.h"
#include "engine_calls.h"
#include "game_sig.h"
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
        namespace sig = replay::sig;

        // StartFireCore(mgr, desc).
        const sig::Signature kFireStart{"fire_start",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 56 48 8D A8 88 F9 FF FF 48 81 EC 60 07 00 00 48 8B F1 0F 29 70 "
            "D8 48 8B CA",
            0x12B5EA4};
        // The fire shutdown (fire, instant, u8). For a map fire it passes the
        // fire manager object itself to RemnantAdd.
        const sig::Signature kFireStop{"fire_stop",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 57 41 54 41 55 41 56 41 57 48 81 EC 70 01 00 00 45 33 ED 0F "
            "29 70 C8 83 B9 9C 00 00 00 0C 45 8A E0 0F 29 78 B8 44 8A FA",
            0x12B2368};
        const sig::Signature kFireStopRemnant{"fire_stop_remnant", kFireStop, sig::Offset{0x42},
            "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", 0x12B23AA};
        const sig::Signature kFireManager{"fire_manager", kFireStopRemnant, sig::Rel32{3, 7}, "", 0x4C3F1E0};
        // RemnantAdd(mgr, fire) and RemnantRemove(mgr, entry, u8).
        const sig::Signature kFireRemnantAdd{"fire_remnant_add",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 20 48 8D A9 28 88 08 00",
            0x126E6D8};
        const sig::Signature kFireRemnantRemove{"fire_remnant_remove",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 41 56 41 57 48 83 EC 20 48 8D B1 28 88 08 00 48 8B D9 48 "
            "8B CE 41 8A E8 48 8B FA",
            0x12B31C4};
        // TrySpread(fire, u8), called only by the fire update.
        const sig::Signature kFireSpread{"fire_spread",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 56 41 57 48 8D A8 48 F3 FF FF",
            0x12B3B54};
        // The fire update (fire, dt): it adds the step to the fire's age
        // (+0x188) and steps its effect seed (+0x1C8), the two fields
        // WriteFireState writes.
        const sig::Signature kFireUpdate{"fire_update",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 56 41 57 48 8D A8 F8 FD FF FF 48 81 EC D0 02 00 00 0F 29 70 "
            "B8 48 8B F9 0F 29 78 A8 44 0F 29 40 98",
            0x12BC2D4};
        const sig::Signature kFireAgeStep{"fire_age_step", kFireUpdate, sig::Offset{0x6CB},
            "F3 0F 58 87 88 01 00 00 F3 0F 11 87 88 01 00 00", 0x12BC99F};
        const sig::Signature kFireSeedStep{"fire_seed_step", kFireUpdate, sig::Offset{0x104A},
            "48 8B 8F C8 01 00 00 8B C1 48 69 D0 A7 FA DC 5C 48 C1 E9 20 48 03 D1 48 89 97 C8 01 00 00", 0x12BD31E};
        // The area item update (item, &dt, out): the burnt radius (+0x64) set to
        // the outer one (lane 3 of +0x30, +0x3C), the owner (+0xB8), the flags
        // (+0xD0), the ember and glow (+0x80, +0x88).
        const sig::Signature kFireItemUpdate{"fire_area_item_update",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 83 EC 30 8A 81 D1 00 00 00",
            0x12C0B88};
        const sig::Signature kFireItemBurnt{"fire_item_burnt", kFireItemUpdate, sig::Offset{0x136},
            "66 0F 70 43 30 FF F3 0F 11 43 64 48 39 BB B8 00 00 00 75 13 F6 83 D0 00 00 00 02", 0x12C0CBE};
        const sig::Signature kFireItemEmberGlow{"fire_item_ember_glow", kFireItemUpdate, sig::Offset{0x199},
            "0F 2F 8B 80 00 00 00 72 09 0F 2F 8B 88 00 00 00", 0x12C0D21};
        constexpr std::size_t kFiresOffset = 0x10;
        constexpr std::size_t kRemnantsOffset = 0xF820;
        constexpr std::size_t kRemnantBytes = 0xF0;
        constexpr std::size_t kRemnantListOffset = 0x87820; // u16 indices
        constexpr std::size_t kRemnantCountOffset = 0x88820;
        constexpr std::uint32_t kRemnantMax = 2048;

        struct Hook
        {
            const sig::Signature* signature;
            void* target = nullptr;
        };
        Hook g_start{&kFireStart};
        Hook g_stop{&kFireStop};
        Hook g_remAdd{&kFireRemnantAdd};
        Hook g_remRemove{&kFireRemnantRemove};
        Hook g_fireUpdate{&kFireUpdate};
        Hook g_itemUpdate{&kFireItemUpdate};
        Hook g_spread{&kFireSpread};

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
        std::uintptr_t g_manager = 0; // the fire manager, found at install
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

        bool GuardedRead(void* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        std::uintptr_t Manager() { return g_manager; }

        std::uint32_t CallerRva()
        {
            void* frames[1] = {};
            const USHORT captured = RtlCaptureStackBackTrace(2, 1, frames, nullptr);
            const std::uintptr_t address = captured ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
            return address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
        }

        int SlotOfAddress(std::uintptr_t fire)
        {
            if (!Manager())
                return -1;
            const std::uintptr_t first = Manager() + kFiresOffset;
            if (fire < first || fire >= first + kFireCount * kFireBytes || (fire - first) % kFireBytes)
                return -1;
            return static_cast<int>((fire - first) / kFireBytes);
        }

        int RemnantOfAddress(std::uintptr_t entry)
        {
            if (!Manager())
                return -1;
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
            void* target = reinterpret_cast<void*>(sig::Find(*hook.signature));
            if (!target)
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

        // The faulting address of the last guarded call (RVA when inside the
        // game image), for the log: code alone does not say which call failed.
        std::uintptr_t g_lastExceptionAddress = 0;
        bool Fault(std::uint32_t& code)
        {
            g_lastExceptionAddress = engine::LastFaultAddress();
            return engine::Failed(code);
        }

        bool GuardedStart(StartFn fn, void* manager, std::uint8_t* desc, void*& fire, std::uint32_t& code)
        {
            return engine::Call(engine::kFireStart, reinterpret_cast<std::uintptr_t>(fn), fire, manager, desc) || Fault(code);
        }

        bool GuardedStop(StopFn fn, void* fire, std::uint8_t instant, std::uint8_t arg3, std::uint32_t& code)
        {
            return engine::Call(engine::kFireStop, reinterpret_cast<std::uintptr_t>(fn), fire, instant, arg3) || Fault(code);
        }

        bool GuardedRemRemove(RemRemoveFn fn, void* manager, void* entry, std::uint32_t& code)
        {
            return engine::Call(engine::kFireRemnantRemove, reinterpret_cast<std::uintptr_t>(fn), manager, entry, 0) ||
                Fault(code);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)); // the image range, for the log
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        // The hooked functions, then the manager the detours place fires in.
        for (const sig::Signature* signature : {g_start.signature, g_stop.signature, g_remAdd.signature,
                 g_remRemove.signature, g_spread.signature, g_fireUpdate.signature, g_itemUpdate.signature, &kFireManager})
        {
            if (!sig::Find(*signature))
            {
                char text[64]{};
                std::snprintf(text, sizeof(text), "fire_signature_mismatch_0x%X", static_cast<unsigned>(signature->rva));
                error = text;
                return false;
            }
        }
        g_manager = sig::Find(kFireManager);
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
        // The area fire system. A walk over its items loads the area object and
        // its item storage (the pointer at area +0x58: 2048 items of 0xE0),
        // divides by the item size and tests an item's bound (lane 3 of +0x40,
        // +0x4C).
        const sig::Signature kFireAreaWalk{"fire_area_walk",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 59 20 48 8B F2 48 85 DB 0F 84 ?? ?? ?? ?? "
            "48 8D 2D ?? ?? ?? ?? 48 8B 3B 48 B8 25 49 92 24 49 92 24 49 48 8B CF 48 2B 0D ?? ?? ?? ?? 48 F7 E9 48 C1 FA "
            "06 48 8B C2 48 C1 E8 3F 48 03 D0 0F B7 C2 8B D0 44 8B C0 48 C1 EA 05 41 83 E0 1F 8B 8C 95 D0 20 00 00 8B C1 "
            "44 0F AB C0 44 0F A3 C1 89 84 95 D0 20 00 00 72 29 66 0F 70 47 40 FF",
            0x129E354};
        const sig::Signature kFireArea{"fire_area", kFireAreaWalk, sig::Rel32{0x27, 0x2B}, "", 0x4E2F5B0};
        const sig::Signature kFireAreaItems{"fire_area_items", kFireAreaWalk, sig::Rel32{0x3E, 0x42}, "", 0x4E2F608};
        // The region clear (area, &centre, &radius splat, full).
        const sig::Signature kFireAreaClear{"fire_area_clear",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 60 41 0F 28 00 49 8B D8 0F 28 0A 4C 8D 40 B8 48 "
            "8B FA 41 8A F1 48 8D 50 C8 48 8B E9",
            0x12A35E4};
        // An area item's flame ring: its width (+0x6C) against the burnt
        // radius (+0x64).
        const sig::Signature kFireItemRing{"fire_item_ring",
            "48 8B C4 48 89 58 08 57 48 83 EC 60 0F 29 70 E8 48 8B D9 8A 89 D0 00 00 00 49 8B F8 0F 29 78 D8 44 0F 29 40 "
            "C8 44 0F 29 48 B8",
            0x1280D1C};
        const sig::Signature kFireItemWidth{"fire_item_width", kFireItemRing, sig::Offset{0x2E3},
            "F3 0F 5C 7B 64 0F 57 C0 F3 0F 5E 7B 6C", 0x1280FFF};
        bool GuardedAreaClear(std::uintptr_t fn, void* area, const float* center, const float* radius, std::uint32_t& code)
        {
            return engine::Call(engine::kFireAreaClear, fn, area, center, radius, 1) || Fault(code);
        }
    }

    bool ClearBurntGround(const float* center, float radius, std::string& error)
    {
        if (!g_base)
        {
            error = "fire_not_installed";
            return false;
        }
        const std::uintptr_t clear = sig::Find(kFireAreaClear);
        const std::uintptr_t area = sig::Find(kFireArea);
        if (!clear || !area)
        {
            error = "area_clear_signature_mismatch";
            return false;
        }
        alignas(16) float c[4] = {center[0], center[1], center[2], 0.0f};
        alignas(16) float r[4] = {radius, radius, radius, radius};
        std::uint32_t code = 0;
        if (!GuardedAreaClear(clear, reinterpret_cast<void*>(area), c, r, code))
        {
            error = ExceptionText(code);
            return false;
        }
        return true;
    }

    namespace
    {
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
            const std::uintptr_t pointer = sig::Find(kFireAreaItems);
            if (pointer)
                GuardedRead(&storage, reinterpret_cast<void*>(pointer), sizeof(storage));
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
        // Only the fields the engine's item code is seen using.
        static const bool proven = sig::Find(kFireAreaWalk) && sig::Find(kFireItemBurnt) &&
            sig::Find(kFireItemEmberGlow) && sig::Find(kFireItemWidth);
        const std::uintptr_t storage = proven ? AreaItemStorage() : 0;
        if (!storage || !GuardedWriteItems(storage, writes))
        {
            error = "area_items_unwritable";
            return false;
        }
        return true;
    }

    std::uint64_t FireAddress(int slot)
    {
        return slot < 0 || !Manager()
            ? 0
            : static_cast<std::uint64_t>(Manager() + kFiresOffset + static_cast<std::uintptr_t>(slot) * kFireBytes);
    }

    std::uint64_t RemnantAddress(int index)
    {
        return index < 0 || !Manager()
            ? 0
            : static_cast<std::uint64_t>(Manager() + kRemnantsOffset + static_cast<std::uintptr_t>(index) * kRemnantBytes);
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
        // A field the fire update itself uses (the age, the seed).
        if (!data || !FireStateFieldAllowed(offset, size) ||
            !sig::Find(offset == kFireAgeOffset ? kFireAgeStep : kFireSeedStep))
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
