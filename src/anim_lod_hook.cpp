#include "anim_lod_hook.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"
#include "profiler.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <vector>

namespace replay::animlod
{
    namespace
    {
        // Build 1491.50: 0x2BD410C(director, lod, visible). The prologue calls
        // 0x98AEB8 (GetComponent<Creature>), then tail-jumps to 0x2BD413C
        // (.pdata 0x2BD410C..0x2BD4139).
        constexpr std::uintptr_t kSetLodRva = 0x2BD410C;
        constexpr std::uint8_t kSetLodSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8], rbx
            0x57,                         // push rdi
            0x48, 0x83, 0xEC, 0x20,       // sub rsp, 20h
            0x41, 0x8A, 0xD8,             // mov bl, r8b
            0x40, 0x8A, 0xFA,             // mov dil, dl
            0xE8, 0x97, 0x6D, 0xDB, 0xFD, // call 0x98AEB8 (rel32, module relative)
        };

        using SetLodFn = void(__fastcall*)(void*, std::uint8_t, std::uint8_t);

        std::atomic<SetLodFn> g_original{nullptr};
        std::atomic<std::int32_t> g_inFlight{0};
        std::atomic<std::uint64_t> g_calls{0};
        std::atomic<std::uint64_t> g_forcedCalls{0};
        std::atomic<std::uint64_t> g_changed{0};
        std::atomic<std::uint64_t> g_flips{0};
        void* g_target = nullptr;
        bool g_installed = false;
        // The forced directors: written by the script thread, read by the
        // detour. A detour racing an update may miss or keep a director for
        // one call: one frame of LOD, nothing else.
        std::atomic<std::uintptr_t> g_forced[kMaxForced] = {};
        std::atomic<std::uint32_t> g_forcedEnd{0};
        // The engine's last (lod << 8 | visible | 0x10000) per slot (0: none
        // yet), for the flips count. Reset when the slot gets another director.
        std::atomic<std::uint32_t> g_lastRaw[kMaxForced] = {};
        // The lists of the two owners (script thread).
        std::vector<std::uintptr_t> g_ownerLists[2];

        void __fastcall SetLodDetour(void* director, std::uint8_t lod, std::uint8_t visible)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const SetLodFn original = g_original.load(std::memory_order_acquire);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            const std::uint32_t end = g_forcedEnd.load(std::memory_order_acquire);
            if (end != 0 && director)
            {
                const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(director);
                for (std::uint32_t index = 0; index < end && index < kMaxForced; ++index)
                {
                    if (g_forced[index].load(std::memory_order_relaxed) != key)
                        continue;
                    g_forcedCalls.fetch_add(1, std::memory_order_relaxed);
                    if (lod != 0 || visible == 0)
                        g_changed.fetch_add(1, std::memory_order_relaxed);
                    const std::uint32_t raw = 0x10000u | (static_cast<std::uint32_t>(lod) << 8) | (visible ? 1u : 0u);
                    const std::uint32_t last = g_lastRaw[index].exchange(raw, std::memory_order_relaxed);
                    if (last != 0 && last != raw)
                        g_flips.fetch_add(1, std::memory_order_relaxed);
                    lod = 0;
                    visible = 1;
                    break;
                }
            }
            if (original)
                original(director, lod, visible);
        }
    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* target = base + kSetLodRva;
        if (!replay::mem::Matches(target, kSetLodSignature, sizeof(kSetLodSignature)))
        {
            error = "anim_set_lod_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&SetLodDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<SetLodFn>(trampoline), std::memory_order_release);
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
        return true;
    }

    void Shutdown()
    {
        g_forcedEnd.store(0, std::memory_order_release);
        if (g_installed && g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_installed = false;
            g_target = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
    }

    bool Installed() { return g_installed; }

    void SetForced(const std::vector<std::uintptr_t>& directors)
    {
        const std::uint32_t oldEnd = g_forcedEnd.load(std::memory_order_relaxed);
        std::uint32_t count = 0;
        for (const std::uintptr_t director : directors)
        {
            if (count >= kMaxForced)
                break;
            if (!director)
                continue;
            if (g_forced[count].load(std::memory_order_relaxed) != director)
                g_lastRaw[count].store(0, std::memory_order_relaxed);
            g_forced[count++].store(director, std::memory_order_relaxed);
        }
        for (std::uint32_t index = count; index < oldEnd && index < kMaxForced; ++index)
            g_forced[index].store(0, std::memory_order_relaxed);
        g_forcedEnd.store(count, std::memory_order_release);
    }

    void SetForcedFor(Owner owner, const std::vector<std::uintptr_t>& directors)
    {
        g_ownerLists[static_cast<std::size_t>(owner) & 1u] = directors;
        if (g_ownerLists[0].empty() || g_ownerLists[1].empty())
        {
            SetForced(g_ownerLists[0].empty() ? g_ownerLists[1] : g_ownerLists[0]);
            return;
        }
        static std::vector<std::uintptr_t> merged;
        merged = g_ownerLists[0];
        merged.insert(merged.end(), g_ownerLists[1].begin(), g_ownerLists[1].end());
        SetForced(merged);
    }

    Stats Snapshot()
    {
        Stats stats;
        stats.calls = g_calls.load(std::memory_order_relaxed);
        stats.forced = g_forcedCalls.load(std::memory_order_relaxed);
        stats.changed = g_changed.load(std::memory_order_relaxed);
        stats.flips = g_flips.load(std::memory_order_relaxed);
        return stats;
    }

    void ResetStats()
    {
        g_calls.store(0, std::memory_order_relaxed);
        g_forcedCalls.store(0, std::memory_order_relaxed);
        g_changed.store(0, std::memory_order_relaxed);
        g_flips.store(0, std::memory_order_relaxed);
    }
}
