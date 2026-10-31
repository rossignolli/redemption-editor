// Render export: fwTimer::Update detour that replaces the real frame delta by
// the export step while an export holds the lock (export_timer.h has the
// design and the evidence).

#include "export_timer.h"

#include "game_sig.h"
#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"

#include <windows.h>

#include <MinHook.h>

namespace replay::exporttimer
{
    namespace
    {
        namespace sig = replay::sig;

        // fwTimer::Update: mov rax,rsp; mov [rax+18h],rbx; push rdi; sub rsp,60h;
        // movaps [rax-18h],xmm6; xor edi,edi; cmp [flag],dil.
        const sig::Signature kUpdate{"export_timer_update",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 70 E8 33 FF 40 38 3D ?? ?? ?? ?? 0F 29 78 D8 44 0F 29 40 C8 44 "
            "0F 29 48 B8 74 0E",
            0x2B13D3C};
        // The real delta: call [QueryPerformanceCounter]; mov rax,[previous
        // counter]; xorps xmm8,xmm8; mov rcx,[rsp+70h]; sub rcx,rax; cvtsi2ss
        // xmm8,rcx; lea rcx,[rsp+78h]; mulss xmm8,[seconds per tick].
        const sig::Signature kDeltaSite{"export_timer_delta_site", kUpdate, sig::Offset{0x10A},
            "FF 15 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 45 0F 57 C0 48 8B 4C 24 70 48 2B C8 F3 4C 0F 2A C1 48 8D 4C 24 "
            "78 F3 44 0F 59 05 ?? ?? ?? ??",
            0x2B13E46};
        const sig::Signature kPreviousCounter{"export_timer_previous_counter", kDeltaSite, sig::Rel32{0x09, 0x0D}, "",
            0x59B3CA8}; // u64
        const sig::Signature kPeriod{"export_timer_period", kDeltaSite, sig::Rel32{0x28, 0x2C}, "",
            0x542819C}; // f32 seconds per tick
        // Then: call [QueryPerformanceCounter]; mov rax,[rsp+78h]; mov [previous
        // counter],rax (the new previous counter).
        const sig::Signature kStoreSite{"export_timer_store_site", kUpdate, sig::Offset{0x136},
            "FF 15 ?? ?? ?? ?? 48 8B 44 24 78 48 89 05 ?? ?? ?? ??", 0x2B13E72};
        const sig::Signature kStoredCounter{"export_timer_stored_counter", kStoreSite, sig::Rel32{0x0E, 0x12}, "",
            0x59B3CA8};
        // The system time set: xor r9d,r9d (no clamp); lea rcx,[system set];
        // movaps xmm2,xmm7 (warp 1.0); movaps xmm1,xmm8 (realDelta); call the
        // time set helper.
        const sig::Signature kSystemSetSite{"export_timer_system_set_site", kUpdate, sig::Offset{0x219},
            "45 33 C9 48 8D 0D ?? ?? ?? ?? 0F 28 D7 41 0F 28 C8 E8 ?? ?? ?? ??", 0x2B13F55};
        const sig::Signature kSystemSet{"export_timer_system_set", kSystemSetSite, sig::Rel32{0x06, 0x0A}, "",
            0x59B38F0};
        // The time set helper: movaps xmm3,xmm1; mov rdx,rcx; mulss xmm3,xmm2
        // (step = realDelta * warp); ...; movss [rdx+0Ch],xmm3 (stepSeconds).
        const sig::Signature kSetHelper{"export_timer_set_helper", kSystemSetSite, sig::Rel32{0x12, 0x16},
            "0F 28 D9 48 8B D1 F3 0F 59 DA", 0x2B13C90};
        const sig::Signature kSetHelperStore{"export_timer_set_helper_store", kSetHelper, sig::Offset{0x72},
            "F3 0F 11 5A 0C", 0x2B13D02};
        // Its clamp of a clamped set: movss xmm1,[min step]; comiss xmm4,xmm1;
        // movss xmm0,[max step].
        const sig::Signature kClampSite{"export_timer_clamp_site", kSetHelper, sig::Offset{0x23},
            "F3 0F 10 0D ?? ?? ?? ?? 0F 2F E1 F3 0F 10 05 ?? ?? ?? ??", 0x2B13CB3};
        const sig::Signature kClampMin{"export_timer_clamp_min", kClampSite, sig::Rel32{0x04, 0x08}, "",
            0x3CC8388}; // f32
        const sig::Signature kClampMax{"export_timer_clamp_max", kClampSite, sig::Rel32{0x0F, 0x13}, "",
            0x3CC838C}; // f32
        // The game time set (GET_FRAME_TIME): lea rcx,[game set]; mov r9b,1
        // (clamped); mov [..],eax; movaps xmm2,xmm6 (the warp); movaps
        // xmm1,xmm8; call the time set helper.
        const sig::Signature kGameSetSite{"export_timer_game_set_site", kUpdate, sig::Offset{0x1BB},
            "48 8D 0D ?? ?? ?? ?? 41 B1 01 89 05 ?? ?? ?? ?? 0F 28 D6 41 0F 28 C8 E8 ?? ?? ?? ??", 0x2B13EF7};
        const sig::Signature kGameSet{"export_timer_game_set", kGameSetSite, sig::Rel32{0x03, 0x07}, "", 0x59B3930};
        constexpr std::uintptr_t kSetStepOffset = 0x0C; // f32 stepSeconds (kSetHelperStore)

        using UpdateFn = void(__fastcall*)(void*, void*, void*, void*);

        void* g_target = nullptr;
        // Found at install: the counter the update subtracts, the system time
        // set and the game time set (0 when not found: not read).
        std::uintptr_t g_previousCounter = 0;
        std::uintptr_t g_systemSet = 0;
        std::uintptr_t g_gameSet = 0;
        std::atomic<UpdateFn> g_original{nullptr};
        std::atomic<bool> g_locked{false};
        std::atomic<std::int64_t> g_stepNs{0};
        std::atomic<std::uint64_t> g_updates{0};
        std::atomic<std::uint64_t> g_lockedUpdates{0};
        std::atomic<std::uint64_t> g_writeFailures{0};
        std::atomic<std::int64_t> g_lastStepNs{0};
        std::atomic<float> g_lastSystemStep{0.0f};
        std::atomic<float> g_lastGameStep{0.0f};
        std::atomic<std::int64_t> g_errorTicks{0};
        float g_period = 0.0f;
        float g_clampMin = 0.0f;
        float g_clampMax = 0.0f;
        // Main thread only (the detour), reset by Lock on the script fiber
        // while unlocked (the detour does not touch it then).
        StepPlanner g_planner;

        void __fastcall UpdateDetour(void* a, void* b, void* c, void* d)
        {
            replay::lifetime::DetourScope lifetime;
            g_updates.fetch_add(1, std::memory_order_relaxed);
            const UpdateFn original = g_original.load(std::memory_order_acquire);
            const bool locked = g_locked.load(std::memory_order_acquire);
            const std::int64_t stepNs = g_stepNs.load(std::memory_order_acquire);
            bool wrote = false;
            if (locked && stepNs > 0)
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const std::int64_t ticks = g_planner.Issue(stepNs);
                const std::uint64_t previous = static_cast<std::uint64_t>(now.QuadPart - ticks);
                wrote = g_previousCounter != 0 &&
                    replay::mem::Put(g_previousCounter, &previous, sizeof(previous));
                if (!wrote)
                    g_writeFailures.fetch_add(1, std::memory_order_relaxed);
            }
            if (original)
                original(a, b, c, d);
            if (!wrote)
                return;
            float systemStep = 0.0f;
            float gameStep = 0.0f;
            replay::mem::Read(g_systemSet + kSetStepOffset, systemStep);
            if (g_gameSet)
                replay::mem::Read(g_gameSet + kSetStepOffset, gameStep);
            const std::int64_t applied = g_period > 0.0f
                ? static_cast<std::int64_t>(std::llround(static_cast<double>(systemStep) / static_cast<double>(g_period)))
                : g_planner.LastIssued();
            g_planner.Applied(applied);
            g_lockedUpdates.fetch_add(1, std::memory_order_relaxed);
            g_lastStepNs.store(stepNs, std::memory_order_relaxed);
            g_lastSystemStep.store(systemStep, std::memory_order_relaxed);
            g_lastGameStep.store(gameStep, std::memory_order_relaxed);
            g_errorTicks.store(g_planner.ErrorTicks(), std::memory_order_relaxed);
        }
    }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        const std::uintptr_t update = replay::sig::Find(kUpdate);
        if (!update)
        {
            error = "timer_update_signature_mismatch";
            return false;
        }
        // The counter the delta subtracts is the one the store writes again at
        // the end of the call (nothing written by the detour survives it).
        const std::uintptr_t previousCounter = replay::sig::Find(kPreviousCounter);
        if (!previousCounter || replay::sig::Find(kStoredCounter) != previousCounter)
        {
            error = "timer_counter_site_signature_mismatch";
            return false;
        }
        // The helper's store is reached from the helper the system set site calls.
        const std::uintptr_t systemSet = replay::sig::Find(kSystemSet);
        if (!systemSet || !replay::sig::Find(kSetHelperStore))
        {
            error = "timer_time_set_signature_mismatch";
            return false;
        }
        const std::uintptr_t period = replay::sig::Find(kPeriod);
        if (!period || !replay::mem::Read(period, g_period) || !(g_period > 0.0f && g_period < 1e-3f))
        {
            error = "timer_period_unreadable";
            return false;
        }
        if (const std::uintptr_t clampMin = replay::sig::Find(kClampMin))
            replay::mem::Read(clampMin, g_clampMin);
        if (const std::uintptr_t clampMax = replay::sig::Find(kClampMax))
            replay::mem::Read(clampMax, g_clampMax);
        g_previousCounter = previousCounter;
        g_systemSet = systemSet;
        g_gameSet = replay::sig::Find(kGameSet);
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* target = reinterpret_cast<void*>(update);
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&UpdateDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<UpdateFn>(trampoline), std::memory_order_release);
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

    bool Installed() { return g_target != nullptr; }

    bool Lock(std::string& error)
    {
        if (!g_target && !Install(error))
            return false;
        if (g_locked.load())
            return true;
        // While unlocked the detour does not touch the planner.
        g_planner.Begin(1.0 / static_cast<double>(g_period));
        g_stepNs.store(0, std::memory_order_release);
        g_locked.store(true, std::memory_order_release);
        return true;
    }

    void SetStepNs(std::int64_t stepNs) { g_stepNs.store(stepNs, std::memory_order_release); }

    void Release()
    {
        g_locked.store(false, std::memory_order_release);
        g_stepNs.store(0, std::memory_order_release);
    }

    bool Locked() { return g_locked.load(std::memory_order_acquire); }

    Stats Snapshot()
    {
        Stats stats;
        stats.installed = g_target != nullptr;
        stats.locked = g_locked.load();
        stats.updates = g_updates.load();
        stats.lockedUpdates = g_lockedUpdates.load();
        stats.lastStepNs = g_lastStepNs.load();
        stats.lastSystemStep = g_lastSystemStep.load();
        stats.lastGameStep = g_lastGameStep.load();
        stats.errorTicks = g_errorTicks.load();
        stats.writeFailures = g_writeFailures.load();
        stats.clampMin = g_clampMin;
        stats.clampMax = g_clampMax;
        stats.period = g_period;
        return stats;
    }

    void Shutdown()
    {
        Release();
        if (g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        g_original.store(nullptr, std::memory_order_release);
    }
}
