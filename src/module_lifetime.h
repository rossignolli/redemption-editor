#pragma once

// Unload side of this module's own code running on other threads.
//
// ScriptHook's Ctrl+R calls FreeLibrary: DllMain(DLL_PROCESS_DETACH) is the
// only unload notice the mod gets, it runs under the loader lock and it
// cannot refuse the unload. So everything that must be finished before the
// image goes away is waited for there, with a check that proves the code
// really left the module instead of a flag followed by a Sleep:
//
// - Detours: every detour body opens a DetourScope first (before the
//   profiler zone and before the call to the original), so the count covers
//   the whole call, the trampoline included, and every early return. The
//   unload disables all hooks in one freeze, waits for the count to reach
//   zero (twice, around a grace for a call that jumped in just before the
//   jump went away), and only then are trampolines removed.
// - Worker threads made with CreateThread (they must not pin the module,
//   see pose_capture.cpp): the thread's exit flag is its last
//   statement, and the unload waits until the thread handle is signalled
//   or the flag is set and the thread's instruction pointer is outside this
//   image (it returned into kernel32 and waits for the loader lock there).
//   A join would deadlock under the loader lock.
// - std::thread workers (the clip writer, the clip list reader) need none of
//   this: the CRT's _beginthreadex takes a module reference and releases it
//   with FreeLibraryAndExitThread, so DllMain cannot run while they live.
//
// A wait that times out is counted (UnloadTimeouts) and reported by
// OnModuleDetach in the log and in a marker file the next load reports;
// the unload itself cannot be stopped from DllMain.

#include <Windows.h>

#include <atomic>
#include <cstdint>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace replay::lifetime
{
    // [base, base + SizeOfImage) of the image this header is compiled into.
    inline bool InThisModule(std::uintptr_t address) noexcept
    {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
        return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
    }

    // ------------------------------------------------------------ detours

    // The in-flight counts of the detours are striped by thread: one counter
    // for every detour of the mod takes two locked writes per call on a line
    // every engine job thread writes (the pose finalizer alone: ~65 calls a
    // frame). A call enters and leaves on its own thread's stripe.
    inline constexpr std::uint32_t kStripes = 16;

    // The calling thread's stripe (thread ids are multiples of 4).
    inline std::uint32_t ThreadStripe() noexcept
    {
        return (GetCurrentThreadId() >> 2) & (kStripes - 1);
    }

    struct alignas(64) CounterStripe
    {
        std::atomic<std::int32_t> value{0};
        std::uint8_t padding[60] = {}; // fills the line: the alignment adds no padding
    };
    static_assert(sizeof(CounterStripe) == 64, "one cache line per stripe");

    struct StripedCounter
    {
        CounterStripe stripes[kStripes];

        // Enter and Leave of one call take the same stripe, so a stripe
        // never goes below zero and holds the calls inside on it.
        void Enter(std::uint32_t stripe) noexcept { stripes[stripe].value.fetch_add(1, std::memory_order_seq_cst); }
        void Leave(std::uint32_t stripe) noexcept { stripes[stripe].value.fetch_sub(1, std::memory_order_seq_cst); }

        // Not one atomic read, and it does not need to be: the waits that
        // read it (DrainDetours, RemoveFinalizerHook) run with the hooks
        // already disabled, so no call enters any more except one that
        // jumped in just before, which is what the grace and the second
        // wait are for, as with the single counter. A call inside for the
        // whole sum is counted (its stripe holds it when read); the sum can
        // only miss a call that left during it (gone) or entered during it
        // (that late entry). So zero means what one counter at zero meant.
        std::int32_t Sum() const noexcept
        {
            std::int32_t sum = 0;
            for (const CounterStripe& stripe : stripes)
                sum += stripe.value.load(std::memory_order_seq_cst);
            return sum;
        }
    };

    inline StripedCounter g_detoursActive;

    // First statement of every detour body.
    struct DetourScope
    {
        DetourScope() noexcept : m_stripe(ThreadStripe()) { g_detoursActive.Enter(m_stripe); }
        ~DetourScope() noexcept { g_detoursActive.Leave(m_stripe); }
        DetourScope(const DetourScope&) = delete;
        DetourScope& operator=(const DetourScope&) = delete;

    private:
        std::uint32_t m_stripe; // the same stripe for the leave
    };

    inline std::int32_t DetoursActive() noexcept { return g_detoursActive.Sum(); }

    // Pure part of the drain (tests): the drain is over when the count was
    // seen at zero both before and after the grace.
    inline bool DrainComplete(std::int32_t beforeGrace, std::int32_t afterGrace) noexcept
    {
        return beforeGrace == 0 && afterGrace == 0;
    }

    // Call with every hook already disabled (no new entries): waits until no
    // detour of this module runs. False on timeout.
    inline bool DrainDetours(DWORD timeoutMs, DWORD graceMs = 20) noexcept
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        const auto waitZero = [deadline]() noexcept {
            while (DetoursActive() != 0)
            {
                if (GetTickCount64() >= deadline)
                    return DetoursActive();
                Sleep(1);
            }
            return 0;
        };
        const std::int32_t before = waitZero();
        if (before != 0)
            return false;
        Sleep(graceMs);
        return DrainComplete(before, waitZero());
    }

    // ------------------------------------------------------------ threads

    enum class ThreadLeave
    {
        NoThread,
        Exited,     // the handle is signalled
        LeftModule, // exit flag set, instruction pointer outside the image
        Timeout,    // still running this module's code
    };

    // Pure decision (tests). The flag alone is not enough (the epilogue
    // after it is still this image's code), the pointer alone neither (a
    // thread blocked in a system call returns into the image).
    inline bool ThreadHasLeft(bool handleSignalled, bool exitFlag, bool ipKnown, bool ipInModule) noexcept
    {
        return handleSignalled || (exitFlag && ipKnown && !ipInModule);
    }

    // Contract for the thread: nothing after the exit flag but the return
    // (no call, no destructor), so an instruction pointer outside the image
    // after the flag means it returned.
    inline ThreadLeave WaitThreadLeft(HANDLE thread, const std::atomic<bool>& exitFlag, DWORD timeoutMs) noexcept
    {
        if (!thread)
            return ThreadLeave::NoThread;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0)
                return ThreadLeave::Exited;
            const bool flag = exitFlag.load(std::memory_order_acquire);
            bool ipKnown = false;
            bool ipInModule = true;
            if (flag && SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                ipKnown = GetThreadContext(thread, &context) != FALSE;
                ipInModule = ipKnown && InThisModule(static_cast<std::uintptr_t>(context.Rip));
                ResumeThread(thread);
            }
            if (ThreadHasLeft(false, flag, ipKnown, ipInModule))
                return ThreadLeave::LeftModule;
            if (GetTickCount64() >= deadline)
                return ThreadLeave::Timeout;
            Sleep(1);
        }
    }

    // ------------------------------------------------------------ report

    inline std::atomic<std::int32_t> g_unloadTimeouts{0};
    inline std::atomic<const char*> g_lastUnloadTimeout{nullptr};

    inline void NoteUnloadTimeout(const char* what) noexcept
    {
        g_unloadTimeouts.fetch_add(1, std::memory_order_seq_cst);
        g_lastUnloadTimeout.store(what, std::memory_order_seq_cst);
    }

    inline std::int32_t UnloadTimeouts() noexcept { return g_unloadTimeouts.load(std::memory_order_seq_cst); }
    inline const char* LastUnloadTimeout() noexcept
    {
        const char* what = g_lastUnloadTimeout.load(std::memory_order_seq_cst);
        return what ? what : "";
    }

    // Worker wait with the report: true when the thread is out of the image.
    inline bool StopWait(HANDLE thread, const std::atomic<bool>& exitFlag, DWORD timeoutMs, const char* what) noexcept
    {
        const ThreadLeave leave = WaitThreadLeft(thread, exitFlag, timeoutMs);
        if (leave != ThreadLeave::Timeout)
            return true;
        NoteUnloadTimeout(what);
        return false;
    }
}
