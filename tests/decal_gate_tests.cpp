// The producer gate of the engine's decal request ring, run with real threads.

#include "../src/decal_producer_gate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: decal gate: %s\n", message);
        ++g_failures;
    }

    void TestSingleThread()
    {
        replay::decal::ProducerGate gate;
        Check(!gate.Held(), "a new gate is free");
        Check(gate.Enter(7), "the first entry does not wait");
        Check(gate.Held() && gate.HeldBy(7), "held by the thread that entered");
        gate.Leave(7);
        Check(!gate.Held(), "free after the leave");
        const replay::decal::GateCounters counters = gate.Counters();
        Check(counters.entries == 1 && counters.waits == 0 && counters.timeouts == 0 && counters.reentries == 0,
            "one entry, no wait, no timeout");
        gate.Leave(9);
        Check(!gate.Held(), "a leave by a thread that never held it does nothing");
    }

    void TestReentry()
    {
        replay::decal::ProducerGate gate;
        Check(gate.Enter(3), "outer entry");
        Check(gate.Enter(3), "the holder re-enters without waiting");
        gate.Leave(3);
        Check(gate.HeldBy(3), "still held after the inner leave");
        gate.Leave(3);
        Check(!gate.Held(), "free after the outer leave");
        Check(gate.Counters().reentries == 1, "one re-entry counted");
    }

    void TestContention()
    {
        replay::decal::ProducerGate gate;
        std::atomic<bool> holderReady{false};
        std::atomic<bool> holderDone{false};
        std::atomic<bool> waiterSawDone{false};
        bool waiterAtOnce = true;
        std::thread holder([&]() {
            gate.Enter(1);
            holderReady.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            holderDone.store(true);
            gate.Leave(1);
        });
        while (!holderReady.load())
            std::this_thread::yield();
        std::thread waiter([&]() {
            waiterAtOnce = gate.Enter(2);
            waiterSawDone.store(holderDone.load());
            gate.Leave(2);
        });
        holder.join();
        waiter.join();
        Check(!waiterAtOnce, "the second producer waited");
        Check(waiterSawDone.load(), "the second producer entered only after the holder left");
        const replay::decal::GateCounters counters = gate.Counters();
        Check(counters.entries == 2 && counters.waits == 1, "two entries, one wait");
        Check(counters.timeouts == 0, "no timeout within the bound");
        Check(counters.waitUsMax >= 10000 && counters.waitUsMax == counters.waitUsTotal, "the wait was measured");
        Check(!gate.Held(), "free at the end");
    }

    void TestTimeout()
    {
        replay::decal::ProducerGate gate;
        Check(gate.Enter(1), "holder enters");
        const auto started = std::chrono::steady_clock::now();
        const bool atOnce = gate.Enter(2, 5000); // 5 ms bound, the holder never leaves
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        Check(!atOnce, "the bounded wait reports a wait");
        Check(gate.HeldBy(1), "the holder keeps the gate after a timeout");
        Check(waited.count() >= 4 && waited.count() < 1000, "the wait stopped near the bound");
        gate.Leave(2);
        Check(gate.HeldBy(1), "a leave by the thread that timed out does nothing");
        gate.Leave(1);
        Check(!gate.Held(), "the holder's leave frees it");
        Check(gate.Counters().timeouts == 1, "one timeout counted");
    }

    void TestExclusion()
    {
        replay::decal::ProducerGate gate;
        constexpr int kThreads = 4;
        constexpr int kRounds = 4000;
        int shared = 0; // written only under the gate: a lost update means two producers at once
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back([&gate, &shared, t]() {
                const std::uint32_t thread = static_cast<std::uint32_t>(t + 1);
                for (int round = 0; round < kRounds; ++round)
                {
                    replay::decal::GateScope scope(gate, thread);
                    const int seen = shared;
                    std::this_thread::yield();
                    shared = seen + 1;
                }
            });
        }
        for (std::thread& thread : threads)
            thread.join();
        Check(shared == kThreads * kRounds, "no update lost between the producers");
        Check(gate.Counters().entries == static_cast<std::uint64_t>(kThreads * kRounds), "every entry counted");
        Check(gate.Counters().timeouts == 0, "no producer gave up");
        Check(!gate.Held(), "free at the end");
    }
}

int RunDecalGateTests()
{
    g_failures = 0;
    TestSingleThread();
    TestReentry();
    TestContention();
    TestTimeout();
    TestExclusion();
    if (g_failures == 0)
        std::printf("decal gate tests passed\n");
    return g_failures;
}
