#include "reins_guard.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"
#include "profiler.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>

namespace replay::reins
{
    namespace
    {
        // Build 1491.50: 0x702F78(ctrl). Prologue: mov [rsp+8],rbx; push rdi;
        // sub rsp,30h; mov edx,[rcx+70h]; mov rdi,rcx; mov eax,edx;
        // shr eax,0Eh; test al,1; jne (offline dump).
        constexpr std::uintptr_t kRestoreRva = 0x702F78;
        constexpr std::uint8_t kRestoreSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08,       // mov [rsp+8], rbx
            0x57,                               // push rdi
            0x48, 0x83, 0xEC, 0x30,             // sub rsp, 30h
            0x8B, 0x51, 0x70,                   // mov edx, [rcx+70h]
            0x48, 0x8B, 0xF9,                   // mov rdi, rcx
            0x8B, 0xC2,                         // mov eax, edx
            0xC1, 0xE8, 0x0E,                   // shr eax, 0Eh
            0xA8, 0x01,                         // test al, 1
            0x0F,                               // jne (rel32)
        };
        // 0x6B2C00(ctrl, holder, mode). Prologue:
        // mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi; push rdi;
        // push r14; push r15; sub rsp,30h; mov r9,[rcx+28h]; xor r15d,r15d;
        // mov ebx,r8d (offline dump).
        constexpr std::uintptr_t kHoldRva = 0x6B2C00;
        constexpr std::uint8_t kHoldSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08,       // mov [rsp+8], rbx
            0x48, 0x89, 0x6C, 0x24, 0x10,       // mov [rsp+10h], rbp
            0x48, 0x89, 0x74, 0x24, 0x18,       // mov [rsp+18h], rsi
            0x57, 0x41, 0x56, 0x41, 0x57,       // push rdi; push r14; push r15
            0x48, 0x83, 0xEC, 0x30,             // sub rsp, 30h
            0x4C, 0x8B, 0x49, 0x28,             // mov r9, [rcx+28h]
            0x45, 0x33, 0xFF,                   // xor r15d, r15d
            0x41, 0x8B, 0xD8,                   // mov ebx, r8d
        };
        constexpr std::uint64_t kCtrlOwner = 0x10;  // the horse side object
        constexpr std::uint64_t kOwnerRope = 0x7A0; // its reins rope instance
        constexpr std::size_t kMaxOwned = 128;      // ropes::kEngineRopePoolCapacity (followed ropes are engine ropes)

        using RestoreFn = void(__fastcall*)(void*);
        // r9 is not read by the function (it is scratch there); forwarded as is.
        using HoldFn = void(__fastcall*)(void*, void*, std::int32_t, std::uint64_t);

        std::atomic<RestoreFn> g_original{nullptr};
        std::atomic<std::int32_t> g_inFlight{0};
        std::atomic<std::uint64_t> g_calls{0};
        std::atomic<std::uint64_t> g_skipped{0};
        std::atomic<std::uint64_t> g_faults{0};
        void* g_target = nullptr;
        bool g_installed = false;
        std::atomic<HoldFn> g_holdOriginal{nullptr};
        std::atomic<std::uint64_t> g_holdCalls{0};
        std::atomic<std::uint64_t> g_holdSkipped{0};
        std::atomic<std::uint64_t> g_holdSkippedRest{0};
        std::atomic<std::uint64_t> g_holdFaults{0};
        void* g_holdTarget = nullptr;
        bool g_holdInstalled = false;
        bool g_holdWanted = true; // ini ReinsHoldGuard
        SRWLOCK g_lock = SRWLOCK_INIT;
        std::uint64_t g_owned[kMaxOwned] = {};
        std::size_t g_ownedCount = 0;

        bool GuardedRope(void* ctrl, std::uint64_t& rope)
        {
            __try
            {
                const std::uint64_t owner =
                    *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(ctrl) + kCtrlOwner);
                rope = owner ? *reinterpret_cast<const std::uint64_t*>(owner + kOwnerRope) : 0;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool Owned(std::uint64_t rope)
        {
            AcquireSRWLockShared(&g_lock);
            const bool skip = SkipRestore(rope, g_owned, g_ownedCount);
            ReleaseSRWLockShared(&g_lock);
            return skip;
        }

        void __fastcall RestoreDetour(void* ctrl)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.reins_restore");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const RestoreFn original = g_original.load(std::memory_order_acquire);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t rope = 0;
            if (ctrl && GuardedRope(ctrl, rope))
            {
                if (rope && Owned(rope))
                {
                    g_skipped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            else if (ctrl)
            {
                g_faults.fetch_add(1, std::memory_order_relaxed);
            }
            if (original)
                original(ctrl);
        }

        // A hold mode change on a rope the playback follows is skipped (the
        // recorded entries own its pins); every other call is forwarded.
        void __fastcall HoldDetour(void* ctrl, void* holder, std::int32_t mode, std::uint64_t r9)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.reins_hold");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const HoldFn original = g_holdOriginal.load(std::memory_order_acquire);
            g_holdCalls.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t rope = 0;
            if (ctrl && GuardedRope(ctrl, rope))
            {
                if (rope && Owned(rope))
                {
                    g_holdSkipped.fetch_add(1, std::memory_order_relaxed);
                    if (mode == 0)
                        g_holdSkippedRest.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            else if (ctrl)
            {
                g_holdFaults.fetch_add(1, std::memory_order_relaxed);
            }
            if (original)
                original(ctrl, holder, mode, r9);
        }

        bool InstallHold(std::uint8_t* base, std::string& error)
        {
            void* target = base + kHoldRva;
            if (!replay::mem::Matches(target, kHoldSignature, sizeof(kHoldSignature)))
            {
                error = "reins_hold_signature_mismatch";
                return false;
            }
            void* trampoline = nullptr;
            MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&HoldDetour), &trampoline);
            if (status != MH_OK || !trampoline)
            {
                error = std::string("mh_create_hold_") + MH_StatusToString(status);
                return false;
            }
            g_holdOriginal.store(reinterpret_cast<HoldFn>(trampoline), std::memory_order_release);
            status = replay::hooks::Enable(target);
            if (status != MH_OK)
            {
                MH_RemoveHook(target);
                g_holdOriginal.store(nullptr, std::memory_order_release);
                error = std::string("mh_enable_hold_") + MH_StatusToString(status);
                return false;
            }
            g_holdTarget = target;
            g_holdInstalled = true;
            return true;
        }
    }

    void SetHoldGuard(bool on) { g_holdWanted = on; }
    bool HoldInstalled() { return g_holdInstalled; }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* target = base + kRestoreRva;
        if (!replay::mem::Matches(target, kRestoreSignature, sizeof(kRestoreSignature)))
        {
            error = "reins_restore_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&RestoreDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<RestoreFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        g_installed = true;
        // The hold mode guard; its failure leaves the put-back
        // guard on and is reported in the error text.
        if (g_holdWanted && !g_holdInstalled)
        {
            std::string holdError;
            if (!InstallHold(base, holdError))
                error = holdError;
        }
        return true;
    }

    void Shutdown()
    {
        if (g_installed && g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_installed = false;
            g_target = nullptr;
        }
        if (g_holdInstalled && g_holdTarget)
        {
            MH_DisableHook(g_holdTarget);
            MH_RemoveHook(g_holdTarget);
            g_holdInstalled = false;
            g_holdTarget = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
        g_holdOriginal.store(nullptr, std::memory_order_release);
        AcquireSRWLockExclusive(&g_lock);
        g_ownedCount = 0;
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool Installed() { return g_installed; }

    void SetOwnedRopes(const std::vector<std::uint64_t>& ropes)
    {
        AcquireSRWLockExclusive(&g_lock);
        g_ownedCount = 0;
        for (const std::uint64_t rope : ropes)
        {
            if (g_ownedCount >= kMaxOwned)
                break;
            if (rope)
                g_owned[g_ownedCount++] = rope;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    Stats Snapshot()
    {
        Stats stats;
        stats.calls = g_calls.load(std::memory_order_relaxed);
        stats.skipped = g_skipped.load(std::memory_order_relaxed);
        stats.faults = g_faults.load(std::memory_order_relaxed);
        stats.holdCalls = g_holdCalls.load(std::memory_order_relaxed);
        stats.holdSkipped = g_holdSkipped.load(std::memory_order_relaxed);
        stats.holdSkippedRest = g_holdSkippedRest.load(std::memory_order_relaxed);
        stats.holdFaults = g_holdFaults.load(std::memory_order_relaxed);
        return stats;
    }
}
