// Module lifetime and the editor transport reset. Real threads and a real
// MinHook detour in this test executable: the image checks run against the
// test's own module.

#include "module_lifetime.h"
#include "hook_batch.h"
#include "editor_protocol.h"
#include "fire_observer.h"

#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
    int g_lifecycleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_lifecycleFailures;
    }

    // ------------------------------------------------------------ pure

    void TestPureDecisions()
    {
        using replay::lifetime::ThreadHasLeft;
        Check(ThreadHasLeft(true, false, false, true), "a signalled handle has left");
        Check(!ThreadHasLeft(false, true, true, true), "flag set but still in the image: not left");
        Check(ThreadHasLeft(false, true, true, false), "flag set and outside the image: left");
        Check(!ThreadHasLeft(false, false, true, false), "outside the image without the flag (a system call): not left");
        Check(!ThreadHasLeft(false, true, false, false), "flag set, context unknown: not left");
        Check(replay::lifetime::DrainComplete(0, 0), "drain: zero before and after the grace");
        Check(!replay::lifetime::DrainComplete(0, 1), "drain: a late entry during the grace is not drained");
        Check(!replay::lifetime::DrainComplete(2, 0), "drain: busy before the grace");

        std::uint8_t local = 0;
        Check(replay::lifetime::InThisModule(reinterpret_cast<std::uintptr_t>(&TestPureDecisions)),
            "this function is inside the image");
        Check(!replay::lifetime::InThisModule(reinterpret_cast<std::uintptr_t>(&local)), "a stack address is not");
        Check(!replay::lifetime::InThisModule(reinterpret_cast<std::uintptr_t>(&Sleep)) ||
                GetModuleHandleW(L"kernel32.dll") == nullptr,
            "a kernel32 function is not");

        Check(replay::fire::FireStateFieldAllowed(replay::fire::kFireAgeOffset, sizeof(float)), "fire age field");
        Check(replay::fire::FireStateFieldAllowed(replay::fire::kFireRngOffset, sizeof(std::uint64_t)), "fire seed field");
        Check(!replay::fire::FireStateFieldAllowed(replay::fire::kFireAgeOffset, 8), "age with the seed's size refused");
        Check(!replay::fire::FireStateFieldAllowed(0x100, sizeof(float)), "unknown field refused");
        Check(replay::fire::FireOwnedBy(7, 7), "same generation: owned");
        Check(!replay::fire::FireOwnedBy(8, 7), "stopped or reused since: not owned");
        Check(!replay::fire::FireOwnedBy(0, 0), "no start noted: not owned");
    }

    // ------------------------------------------------------------ threads

    std::atomic<bool> g_exitFlag{false};
    std::atomic<bool> g_release{false};

    DWORD WINAPI QuickThread(void*)
    {
        g_exitFlag.store(true, std::memory_order_release);
        return 0;
    }

    // Keeps running this image's code: flag clear.
    DWORD WINAPI BusyThread(void*)
    {
        while (!g_release.load(std::memory_order_acquire))
            _mm_pause();
        g_exitFlag.store(true, std::memory_order_release);
        return 0;
    }

    // Sets the flag, then keeps running this image's code: a sleep after the
    // flag cannot assume the thread has left the image.
    DWORD WINAPI FlagThenBusyThread(void*)
    {
        g_exitFlag.store(true, std::memory_order_release);
        while (!g_release.load(std::memory_order_acquire))
            _mm_pause();
        return 0;
    }

    void TestThreadWait()
    {
        using replay::lifetime::ThreadLeave;
        using replay::lifetime::WaitThreadLeft;
        Check(WaitThreadLeft(nullptr, g_exitFlag, 10) == ThreadLeave::NoThread, "no thread");

        g_exitFlag.store(false);
        HANDLE quick = CreateThread(nullptr, 0, &QuickThread, nullptr, 0, nullptr);
        const ThreadLeave quickLeave = WaitThreadLeft(quick, g_exitFlag, 2000);
        Check(quickLeave == ThreadLeave::Exited || quickLeave == ThreadLeave::LeftModule, "a finished thread has left");
        WaitForSingleObject(quick, INFINITE);
        CloseHandle(quick);

        g_exitFlag.store(false);
        g_release.store(false);
        HANDLE busy = CreateThread(nullptr, 0, &BusyThread, nullptr, 0, nullptr);
        Check(WaitThreadLeft(busy, g_exitFlag, 50) == ThreadLeave::Timeout, "a running thread times out");
        g_release.store(true);
        const ThreadLeave busyLeave = WaitThreadLeft(busy, g_exitFlag, 2000);
        Check(busyLeave == ThreadLeave::Exited || busyLeave == ThreadLeave::LeftModule, "released, it leaves");
        WaitForSingleObject(busy, INFINITE);
        CloseHandle(busy);

        g_exitFlag.store(false);
        g_release.store(false);
        HANDLE late = CreateThread(nullptr, 0, &FlagThenBusyThread, nullptr, 0, nullptr);
        while (!g_exitFlag.load())
            Sleep(1);
        Check(WaitThreadLeft(late, g_exitFlag, 60) == ThreadLeave::Timeout,
            "flag set but the thread still in the image: not left (the old wait would have unloaded)");
        const std::int32_t timeoutsBefore = replay::lifetime::UnloadTimeouts();
        Check(!replay::lifetime::StopWait(late, g_exitFlag, 30, "test_late"), "StopWait reports the timeout");
        Check(replay::lifetime::UnloadTimeouts() == timeoutsBefore + 1 &&
                std::strcmp(replay::lifetime::LastUnloadTimeout(), "test_late") == 0,
            "the timeout is counted and named");
        g_release.store(true);
        Check(replay::lifetime::StopWait(late, g_exitFlag, 2000, "test_late"), "released, StopWait succeeds");
        WaitForSingleObject(late, INFINITE);
        CloseHandle(late);
    }

    // ------------------------------------------------------------ detours

    HANDLE g_insideOriginal = nullptr;
    HANDLE g_leaveOriginal = nullptr;
    std::atomic<int> g_detourCalls{0};
    volatile int g_sink = 0;

    __declspec(noinline) int HookTarget(int value)
    {
        // Long enough for a 5-byte jump; blocks in the kernel when asked,
        // like an engine call a detour is waiting on.
        g_sink = g_sink + value;
        if (value == 1)
        {
            SetEvent(g_insideOriginal);
            WaitForSingleObject(g_leaveOriginal, INFINITE);
        }
        return value * 3 + g_sink * 0;
    }

    using HookTargetFn = int (*)(int);
    std::atomic<HookTargetFn> g_hookOriginal{nullptr};

    int HookDetour(int value)
    {
        replay::lifetime::DetourScope lifetime;
        g_detourCalls.fetch_add(1);
        const HookTargetFn original = g_hookOriginal.load();
        return original ? original(value) + 1 : 0;
    }

    DWORD WINAPI CallHooked(void*)
    {
        volatile HookTargetFn target = &HookTarget;
        return static_cast<DWORD>(target(1));
    }

    void TestDetourDrain()
    {
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            Check(false, "MinHook initializes");
            return;
        }
        void* trampoline = nullptr;
        if (MH_CreateHook(reinterpret_cast<void*>(&HookTarget), reinterpret_cast<void*>(&HookDetour), &trampoline) != MH_OK ||
            !trampoline)
        {
            Check(false, "test hook created");
            return;
        }
        g_hookOriginal.store(reinterpret_cast<HookTargetFn>(trampoline));
        Check(MH_EnableHook(reinterpret_cast<void*>(&HookTarget)) == MH_OK, "test hook enabled");

        g_insideOriginal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_leaveOriginal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE caller = CreateThread(nullptr, 0, &CallHooked, nullptr, 0, nullptr);
        Check(WaitForSingleObject(g_insideOriginal, 2000) == WAIT_OBJECT_0, "the call is inside the original");
        Check(replay::lifetime::DetoursActive() == 1, "the detour counts the call while the original runs");

        // The unload order: all hooks off, then the drain, then removal.
        Check(replay::hooks::DisableAllForUnload() == MH_OK, "all hooks disabled in one call");
        volatile HookTargetFn direct = &HookTarget;
        const int callsBefore = g_detourCalls.load();
        Check(direct(5) == 15 && g_detourCalls.load() == callsBefore, "after the disable a new call skips the detour");
        Check(!replay::lifetime::DrainDetours(60, 5), "the drain waits for the call inside the trampoline's original");
        SetEvent(g_leaveOriginal);
        Check(replay::lifetime::DrainDetours(2000, 5), "the drain completes once the call returned");
        Check(replay::lifetime::DetoursActive() == 0, "no detour active");
        DWORD result = 0;
        WaitForSingleObject(caller, INFINITE);
        GetExitCodeThread(caller, &result);
        Check(result == 4, "the detoured call returned through the detour");
        CloseHandle(caller);
        Check(MH_RemoveHook(reinterpret_cast<void*>(&HookTarget)) == MH_OK, "trampoline removed after the drain");
        g_hookOriginal.store(nullptr);
        CloseHandle(g_insideOriginal);
        CloseHandle(g_leaveOriginal);
    }

    // ------------------------------------------------------------ stripes

    // The striped in-flight count of the detours.
    constexpr int kScopeThreads = 6;
    std::atomic<int> g_scopesInside{0};
    std::atomic<bool> g_scopesRelease{false};

    DWORD WINAPI ScopeThread(void*)
    {
        replay::lifetime::DetourScope lifetime;
        g_scopesInside.fetch_add(1);
        while (!g_scopesRelease.load(std::memory_order_acquire))
            Sleep(1);
        return 0;
    }

    // Enters and leaves as fast as it can (each thread on its own stripe).
    DWORD WINAPI ChurnThread(void*)
    {
        for (int n = 0; n < 200000; ++n)
        {
            replay::lifetime::DetourScope lifetime;
        }
        return 0;
    }

    // Micro-benchmark (printed, not checked): threads entering and leaving
    // at once on one shared counter and on the striped one. A tight loop: far
    // more contention than the engine's job threads make (~65 finalizer calls
    // a frame), so an upper bound per call.
    constexpr int kBenchCalls = 1000000;
    std::atomic<std::int32_t> g_benchSingle{0};
    replay::lifetime::StripedCounter g_benchStriped;
    std::atomic<bool> g_benchGo{false};

    DWORD WINAPI BenchSingleThread(void*)
    {
        while (!g_benchGo.load(std::memory_order_acquire))
            _mm_pause();
        for (int n = 0; n < kBenchCalls; ++n)
        {
            g_benchSingle.fetch_add(1, std::memory_order_seq_cst);
            g_benchSingle.fetch_sub(1, std::memory_order_seq_cst);
        }
        return 0;
    }

    DWORD WINAPI BenchStripedThread(void*)
    {
        while (!g_benchGo.load(std::memory_order_acquire))
            _mm_pause();
        for (int n = 0; n < kBenchCalls; ++n)
        {
            const std::uint32_t stripe = replay::lifetime::ThreadStripe(); // per call, as DetourScope
            g_benchStriped.Enter(stripe);
            g_benchStriped.Leave(stripe);
        }
        return 0;
    }

    // Nanoseconds per enter and leave, the threads running at once.
    double BenchCounter(LPTHREAD_START_ROUTINE routine, int threadCount)
    {
        HANDLE handles[8] = {};
        g_benchGo.store(false);
        for (int index = 0; index < threadCount; ++index)
            handles[index] = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr);
        LARGE_INTEGER frequency{}, started{}, ended{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        g_benchGo.store(true, std::memory_order_release);
        WaitForMultipleObjects(static_cast<DWORD>(threadCount), handles, TRUE, INFINITE);
        QueryPerformanceCounter(&ended);
        for (int index = 0; index < threadCount; ++index)
            CloseHandle(handles[index]);
        const double ns = static_cast<double>(ended.QuadPart - started.QuadPart) * 1e9 /
            static_cast<double>(frequency.QuadPart);
        return ns / kBenchCalls;
    }

    void TestStripedCounter()
    {
        using replay::lifetime::CounterStripe;
        using replay::lifetime::kStripes;
        using replay::lifetime::StripedCounter;
        static_assert(sizeof(CounterStripe) == 64 && alignof(CounterStripe) == 64, "one cache line per stripe");
        static StripedCounter counter;
        Check(counter.Sum() == 0, "a striped counter starts at zero");
        Check(reinterpret_cast<std::uintptr_t>(&counter.stripes[1]) % 64 == 0, "each stripe starts a cache line");
        counter.Enter(0);
        counter.Enter(7);
        counter.Enter(kStripes - 1);
        counter.Enter(7);
        Check(counter.Sum() == 4, "entries on different stripes add up");
        counter.Leave(7);
        Check(counter.Sum() == 3, "a leave takes its call's stripe");
        counter.Leave(0);
        counter.Leave(kStripes - 1);
        counter.Leave(7);
        Check(counter.Sum() == 0, "balanced: zero");
        for (std::uint32_t stripe = 0; stripe < kStripes; ++stripe)
            counter.Enter(stripe);
        Check(counter.Sum() == static_cast<std::int32_t>(kStripes), "one call on every stripe");
        for (std::uint32_t stripe = kStripes; stripe-- > 0;)
            counter.Leave(stripe);
        Check(counter.Sum() == 0, "every stripe left: zero");
        Check(replay::lifetime::ThreadStripe() < kStripes, "the thread's stripe is in range");

        // Real threads, each scope on its thread's stripe: DetoursActive
        // sums them all.
        const std::int32_t base = replay::lifetime::DetoursActive();
        g_scopesInside.store(0);
        g_scopesRelease.store(false);
        HANDLE threads[kScopeThreads] = {};
        for (HANDLE& thread : threads)
            thread = CreateThread(nullptr, 0, &ScopeThread, nullptr, 0, nullptr);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (g_scopesInside.load() < kScopeThreads && GetTickCount64() < deadline)
            Sleep(1);
        Check(replay::lifetime::DetoursActive() == base + kScopeThreads, "DetoursActive counts every thread's scope");
        Check(!replay::lifetime::DrainDetours(30, 1), "the drain waits while the scopes are open");
        g_scopesRelease.store(true);
        for (HANDLE thread : threads)
        {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        Check(replay::lifetime::DetoursActive() == base, "every scope left: back to the count before");

        // Scopes opening and closing on four threads: the summed read never
        // goes below zero or above the calls that can be inside.
        HANDLE churn[4] = {};
        for (HANDLE& thread : churn)
            thread = CreateThread(nullptr, 0, &ChurnThread, nullptr, 0, nullptr);
        std::int32_t lowest = base;
        std::int32_t highest = base;
        std::uint64_t reads = 0;
        while (WaitForMultipleObjects(4, churn, TRUE, 0) == WAIT_TIMEOUT)
        {
            const std::int32_t active = replay::lifetime::DetoursActive();
            lowest = active < lowest ? active : lowest;
            highest = active > highest ? active : highest;
            ++reads;
        }
        for (HANDLE thread : churn)
            CloseHandle(thread);
        Check(lowest >= base && highest <= base + 4, "the summed count stays within the calls inside");
        Check(replay::lifetime::DetoursActive() == base, "the churn left the count where it was");
        if (base == 0)
            Check(replay::lifetime::DrainDetours(1000, 1), "drained once every thread left");
        std::printf("Striped detour count: %u stripes, %d threads counted, %llu summed reads during the churn in [%d, %d]\n",
            static_cast<unsigned>(kStripes), kScopeThreads, static_cast<unsigned long long>(reads), lowest, highest);

        const double single1 = BenchCounter(&BenchSingleThread, 1);
        const double striped1 = BenchCounter(&BenchStripedThread, 1);
        const double single4 = BenchCounter(&BenchSingleThread, 4);
        const double striped4 = BenchCounter(&BenchStripedThread, 4);
        Check(g_benchSingle.load() == 0 && g_benchStriped.Sum() == 0, "benchmark counters balanced");
        std::printf("  in-flight counter, ns per enter+leave: 1 thread one counter %.1f, striped %.1f; "
                    "4 threads at once one counter %.1f, striped %.1f\n",
            single1, striped1, single4, striped4);
    }

    // ------------------------------------------------------------ transport

    using namespace replay::editor;

    Command MakeCommand(std::uint64_t timeMs)
    {
        Command command{};
        command.structSize = sizeof(Command);
        command.version = kApiVersion;
        command.type = CommandType::SeekAbsolute;
        command.timeMs = timeMs;
        return command;
    }

    void TestTransportResetDuringSubmit()
    {
        constexpr std::uint32_t pid = 4242;
        auto transport = std::make_unique<SharedTransport>();
        InitializeSharedTransport(*transport, pid, 11);
        Command popped{};

        // Steady state.
        for (std::uint64_t n = 0; n < 3; ++n)
            Check(SubmitSharedCommand(*transport, MakeCommand(100 + n), pid), "submit");
        for (std::uint64_t n = 0; n < 3; ++n)
            Check(TryPopSharedCommand(*transport, popped, pid) && popped.timeMs == 100 + n, "pop in order");
        Check(!TryPopSharedCommand(*transport, popped, pid), "queue empty");

        // A new probe resets the mapping between the old submit's checks
        // and its publication (write was 3): the stale command is dropped
        // and the index stays the new session's.
        Check(!SubmitSharedCommandWith(*transport, MakeCommand(900), pid,
                  [&]() noexcept { InitializeSharedTransport(*transport, pid, 12); }),
            "a submit overtaken by a reset reports failure");
        Check(AtomicRead(&transport->commandWrite) == 0, "the new session's write index is untouched");
        Check(SubmitSharedCommand(*transport, MakeCommand(200), pid), "new session submit");
        Check(TryPopSharedCommand(*transport, popped, pid) && popped.timeMs == 200, "new session command arrives");

        // Same race with write == 0 (the compare-exchange cannot tell the
        // sessions apart): the stale publish leaves a hole at slot 0, which
        // the consumer skips instead of waiting on it for good.
        InitializeSharedTransport(*transport, pid, 13);
        SubmitSharedCommandWith(*transport, MakeCommand(901), pid,
            [&]() noexcept { InitializeSharedTransport(*transport, pid, 14); });
        Check(!TryPopSharedCommand(*transport, popped, pid), "the hole itself yields nothing");
        Check(SubmitSharedCommand(*transport, MakeCommand(300), pid), "submit after the hole");
        Check(TryPopSharedCommand(*transport, popped, pid) && popped.timeMs == 300, "the command after the hole arrives");

        // An older editor publishes `write + 1` unconditionally: several
        // holes, then the new commands.
        InitializeSharedTransport(*transport, pid, 15);
        InterlockedExchange(&transport->commandWrite, 7);
        Check(SubmitSharedCommand(*transport, MakeCommand(400), pid), "submit behind the stale index");
        Check(SubmitSharedCommand(*transport, MakeCommand(401), pid), "second submit behind the stale index");
        Check(TryPopSharedCommand(*transport, popped, pid) && popped.timeMs == 400, "holes skipped, first command");
        Check(TryPopSharedCommand(*transport, popped, pid) && popped.timeMs == 401, "second command");
        Check(!TryPopSharedCommand(*transport, popped, pid), "queue empty again");
        Check(!ConsumeSharedCommandOverflow(*transport), "no overflow from the holes");
    }
}

int RunLifecycleTests()
{
    TestPureDecisions();
    TestThreadWait();
    TestDetourDrain();
    TestStripedCounter();
    TestTransportResetDuringSubmit();
    if (g_lifecycleFailures == 0)
        std::printf("lifecycle tests passed\n");
    return g_lifecycleFailures;
}
