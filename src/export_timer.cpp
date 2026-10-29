// Render export: fwTimer::Update detour that replaces the real frame delta by
// the export step while an export holds the lock (export_timer.h has the
// design and the evidence).

#include "export_timer.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"

#include <windows.h>

#include <MinHook.h>

namespace replay::exporttimer
{
    namespace
    {
        // Build 1491.50:
        //   02B13D3C  48 8B C4              mov rax, rsp
        //   02B13D3F  48 89 58 18           mov [rax+18h], rbx
        //   02B13D43  57                    push rdi
        //   02B13D44  48 83 EC 60           sub rsp, 60h
        //   02B13D48  0F 29 70 E8           movaps [rax-18h], xmm6
        //   02B13D4C  33 FF                 xor edi, edi
        //   02B13D4E  40 38 3D 92 FB E9 02  cmp [0x59B38E7], dil
        constexpr std::uintptr_t kUpdateRva = 0x2B13D3C;
        constexpr std::uint8_t kUpdateSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x0F, 0x29, 0x70, 0xE8,
            0x33, 0xFF, 0x40, 0x38, 0x3D, 0x92, 0xFB, 0xE9, 0x02};
        //   02B13E46  FF 15 F4 E1 51 03     call [0x6032040] (QueryPerformanceCounter)
        //   02B13E4C  48 8B 05 55 FE E9 02  mov rax, [0x59B3CA8] (previous counter)
        //   02B13E53  45 0F 57 C0           xorps xmm8, xmm8
        //   02B13E57  48 8B 4C 24 70        mov rcx, [rsp+70h]
        //   02B13E5C  48 2B C8              sub rcx, rax
        //   02B13E5F  F3 4C 0F 2A C1        cvtsi2ss xmm8, rcx
        //   02B13E64  48 8D 4C 24 78        lea rcx, [rsp+78h]
        //   02B13E69  F3 44 0F 59 05 2A 43 91 02  mulss xmm8, [0x542819C] (seconds per tick)
        constexpr std::uintptr_t kDeltaSiteRva = 0x2B13E46;
        constexpr std::uint8_t kDeltaSiteSignature[] = {
            0xFF, 0x15, 0xF4, 0xE1, 0x51, 0x03, 0x48, 0x8B, 0x05, 0x55, 0xFE, 0xE9, 0x02, 0x45, 0x0F, 0x57,
            0xC0, 0x48, 0x8B, 0x4C, 0x24, 0x70, 0x48, 0x2B, 0xC8, 0xF3, 0x4C, 0x0F, 0x2A, 0xC1, 0x48, 0x8D,
            0x4C, 0x24, 0x78, 0xF3, 0x44, 0x0F, 0x59, 0x05, 0x2A, 0x43, 0x91, 0x02};
        //   02B13E72  FF 15 C8 E1 51 03     call [0x6032040]
        //   02B13E78  48 8B 44 24 78        mov rax, [rsp+78h]
        //   02B13E7D  48 89 05 24 FE E9 02  mov [0x59B3CA8], rax (the new previous counter)
        constexpr std::uintptr_t kStoreSiteRva = 0x2B13E72;
        constexpr std::uint8_t kStoreSiteSignature[] = {
            0xFF, 0x15, 0xC8, 0xE1, 0x51, 0x03, 0x48, 0x8B, 0x44, 0x24, 0x78, 0x48, 0x89, 0x05, 0x24, 0xFE, 0xE9, 0x02};
        //   02B13F55  45 33 C9              xor r9d, r9d (no clamp)
        //   02B13F58  48 8D 0D 91 F9 E9 02  lea rcx, [0x59B38F0] (system time set)
        //   02B13F5F  0F 28 D7              movaps xmm2, xmm7 (warp 1.0)
        //   02B13F62  41 0F 28 C8           movaps xmm1, xmm8 (realDelta)
        //   02B13F66  E8 25 FD FF FF        call 0x2B13C90
        constexpr std::uintptr_t kSystemSetSiteRva = 0x2B13F55;
        constexpr std::uint8_t kSystemSetSiteSignature[] = {
            0x45, 0x33, 0xC9, 0x48, 0x8D, 0x0D, 0x91, 0xF9, 0xE9, 0x02, 0x0F, 0x28, 0xD7, 0x41, 0x0F, 0x28,
            0xC8, 0xE8, 0x25, 0xFD, 0xFF, 0xFF};
        //   02B13C90  0F 28 D9              movaps xmm3, xmm1
        //   02B13C93  48 8B D1              mov rdx, rcx
        //   02B13C96  F3 0F 59 DA           mulss xmm3, xmm2 (step = realDelta * warp)
        //   ...
        //   02B13D02  F3 0F 11 5A 0C        movss [rdx+0Ch], xmm3 (stepSeconds)
        constexpr std::uintptr_t kSetHelperRva = 0x2B13C90;
        constexpr std::uint8_t kSetHelperSignature[] = {0x0F, 0x28, 0xD9, 0x48, 0x8B, 0xD1, 0xF3, 0x0F, 0x59, 0xDA};
        constexpr std::uintptr_t kSetHelperStoreRva = 0x2B13D02;
        constexpr std::uint8_t kSetHelperStoreSignature[] = {0xF3, 0x0F, 0x11, 0x5A, 0x0C};

        constexpr std::uintptr_t kPreviousCounterRva = 0x59B3CA8; // u64
        constexpr std::uintptr_t kPeriodRva = 0x542819C;          // f32 seconds per tick
        constexpr std::uintptr_t kSystemSetRva = 0x59B38F0;       // fwTimeSet, warp 1.0, no clamp
        constexpr std::uintptr_t kGameSetRva = 0x59B3930;         // fwTimeSet "game" (GET_FRAME_TIME)
        constexpr std::uintptr_t kSetStepOffset = 0x0C;           // f32 stepSeconds
        constexpr std::uintptr_t kClampMinRva = 0x3CC8388;        // f32
        constexpr std::uintptr_t kClampMaxRva = 0x3CC838C;        // f32

        using UpdateFn = void(__fastcall*)(void*, void*, void*, void*);

        std::uintptr_t g_base = 0;
        void* g_target = nullptr;
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

        bool Matches(std::uintptr_t rva, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(reinterpret_cast<const void*>(g_base + rva), signature, size);
        }

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
                wrote = replay::mem::Put(g_base + kPreviousCounterRva, &previous, sizeof(previous));
                if (!wrote)
                    g_writeFailures.fetch_add(1, std::memory_order_relaxed);
            }
            if (original)
                original(a, b, c, d);
            if (!wrote)
                return;
            float systemStep = 0.0f;
            float gameStep = 0.0f;
            replay::mem::Read(g_base + kSystemSetRva + kSetStepOffset, systemStep);
            replay::mem::Read(g_base + kGameSetRva + kSetStepOffset, gameStep);
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
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!Matches(kUpdateRva, kUpdateSignature, sizeof(kUpdateSignature)))
        {
            error = "timer_update_signature_mismatch";
            return false;
        }
        if (!Matches(kDeltaSiteRva, kDeltaSiteSignature, sizeof(kDeltaSiteSignature)) ||
            !Matches(kStoreSiteRva, kStoreSiteSignature, sizeof(kStoreSiteSignature)))
        {
            error = "timer_counter_site_signature_mismatch";
            return false;
        }
        if (!Matches(kSystemSetSiteRva, kSystemSetSiteSignature, sizeof(kSystemSetSiteSignature)) ||
            !Matches(kSetHelperRva, kSetHelperSignature, sizeof(kSetHelperSignature)) ||
            !Matches(kSetHelperStoreRva, kSetHelperStoreSignature, sizeof(kSetHelperStoreSignature)))
        {
            error = "timer_time_set_signature_mismatch";
            return false;
        }
        if (!replay::mem::Read(g_base + kPeriodRva, g_period) || !(g_period > 0.0f && g_period < 1e-3f))
        {
            error = "timer_period_unreadable";
            return false;
        }
        replay::mem::Read(g_base + kClampMinRva, g_clampMin);
        replay::mem::Read(g_base + kClampMaxRva, g_clampMax);
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* target = reinterpret_cast<void*>(g_base + kUpdateRva);
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
