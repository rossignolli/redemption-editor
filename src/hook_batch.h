#pragma once

// One freeze for many detours. Every MH_EnableHook suspends all the game's
// threads (render included) for 60 to 160 ms. While a batch is open, Enable
// only queues, and EndBatch applies the queue with a single MH_ApplyQueued.
// Outside a batch, Enable is MH_EnableHook, timed.

#include <Windows.h>
#include <MinHook.h>

namespace replay::hooks
{
    struct Stats
    {
        int enables = 0;          // single enables outside a batch
        double enableMsTotal = 0; // their total freeze time
        double enableMsMax = 0;
        int batches = 0;
        int queued = 0;           // hooks applied through batches
        double batchMsTotal = 0;
        MH_STATUS lastBatch = MH_OK;
    };

    namespace detail
    {
        inline bool g_batchOpen = false;
        inline int g_batchQueued = 0;
        inline Stats g_stats;

        inline double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
        {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        }
    }

    inline void BeginBatch()
    {
        detail::g_batchOpen = true;
        detail::g_batchQueued = 0;
    }

    inline bool BatchOpen() { return detail::g_batchOpen; }

    inline MH_STATUS Enable(void* target)
    {
        if (detail::g_batchOpen)
        {
            const MH_STATUS status = MH_QueueEnableHook(target);
            if (status == MH_OK)
                ++detail::g_batchQueued;
            return status;
        }
        LARGE_INTEGER a{}, b{};
        QueryPerformanceCounter(&a);
        const MH_STATUS status = MH_EnableHook(target);
        QueryPerformanceCounter(&b);
        const double ms = detail::Ms(a, b);
        ++detail::g_stats.enables;
        detail::g_stats.enableMsTotal += ms;
        if (ms > detail::g_stats.enableMsMax)
            detail::g_stats.enableMsMax = ms;
        return status;
    }

    // Applies the queue with one freeze; returns its time in ms.
    inline double EndBatch(int& applied, MH_STATUS& status)
    {
        detail::g_batchOpen = false;
        applied = detail::g_batchQueued;
        detail::g_batchQueued = 0;
        status = MH_OK;
        if (applied == 0)
            return 0.0;
        LARGE_INTEGER a{}, b{};
        QueryPerformanceCounter(&a);
        status = MH_ApplyQueued();
        QueryPerformanceCounter(&b);
        const double ms = detail::Ms(a, b);
        ++detail::g_stats.batches;
        detail::g_stats.queued += applied;
        detail::g_stats.batchMsTotal += ms;
        detail::g_stats.lastBatch = status;
        return ms;
    }

    inline Stats Snapshot() { return detail::g_stats; }

    // Module unload, first step. Every hook of this module off in one freeze
    // (MinHook moves a thread standing in a trampoline back to the target), so no
    // new call enters a detour; the trampolines stay until the detours drained
    // (module_lifetime.h).
    inline MH_STATUS DisableAllForUnload()
    {
        detail::g_batchOpen = false;
        detail::g_batchQueued = 0;
        return MH_DisableHook(MH_ALL_HOOKS);
    }
}
