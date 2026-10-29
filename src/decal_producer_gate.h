#pragma once

// One producer at a time in the engine's decal request ring.
//
// The decal core 0x2C2EF08 takes the next entry of a ring of 0x400 entries
// (manager +0xB5D0, 0x330 bytes each) with a plain load, increment and store
// of the write index (+0xD75D0) and of the pending count (+0xD75D8): no
// atomic, no lock. The engine produces into it from the deferred decal job
// (0xC1F450 -> drain 0x1431DD4 -> 0x14357D0 -> 0x125B97C -> 0x125F9EC ->
// wrapper 0x2C2F054 -> core), on a worker of the task scheduler. The replay
// produces into the same ring from the script thread (AddDecal,
// AddDecalWithId). Two producers at once can take the same entry or step the
// index past the wrap; the entry recycled 0x400 adds later then carries a
// registered-reference holder (+0x310 index, +0x318 entity pointer) that
// nobody cleared, and the unlink 0x15E544 writes through it and crashes.
//
// This gate serialises the two producers: the drain detour holds it for the
// whole engine drain, the replay's adds hold it around their call of the
// core. Pure (no Windows headers) so the tests run it with std::thread. A
// wait is bounded: a producer that waited `maxWaitUs` goes on anyway and
// counts a timeout, so a dead holder never hangs the game; the counters say
// in the log whether the two producers ever met (gate_waits) and how long
// the longest wait was.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace replay::decal
{
    struct GateCounters
    {
        std::uint64_t entries = 0;     // every Enter
        std::uint64_t waits = 0;       // entries that found the gate held by another thread
        std::uint64_t waitUsTotal = 0; // time spent waiting, all entries
        std::uint64_t waitUsMax = 0;   // the longest single wait
        std::uint64_t timeouts = 0;    // entries that gave up waiting and went on
        std::uint64_t reentries = 0;   // Enter by the thread already holding it
    };

    class ProducerGate
    {
    public:
        static constexpr std::uint64_t kDefaultMaxWaitUs = 100000; // 100 ms: longer than any drain, shorter than a hang

        // Enters as `thread` (a nonzero id). Returns true when it entered
        // without waiting or re-entered, false when it waited (whether it
        // got the gate or timed out). Timed out: the caller proceeds and the
        // gate is not taken (Leave is still balanced: it does nothing).
        bool Enter(std::uint32_t thread, std::uint64_t maxWaitUs = kDefaultMaxWaitUs) noexcept
        {
            m_entries.fetch_add(1, std::memory_order_relaxed);
            std::uint32_t expected = 0;
            if (m_owner.compare_exchange_strong(expected, thread, std::memory_order_acquire, std::memory_order_relaxed))
            {
                m_depth = 1;
                return true;
            }
            if (expected == thread)
            {
                ++m_depth;
                m_reentries.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            m_waits.fetch_add(1, std::memory_order_relaxed);
            const auto started = std::chrono::steady_clock::now();
            for (;;)
            {
                std::this_thread::yield();
                expected = 0;
                if (m_owner.compare_exchange_weak(expected, thread, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    m_depth = 1;
                    NoteWait(started);
                    return false;
                }
                const std::uint64_t waited = ElapsedUs(started);
                if (waited >= maxWaitUs)
                {
                    m_timeouts.fetch_add(1, std::memory_order_relaxed);
                    NoteWait(started);
                    return false;
                }
            }
        }

        // Leaves as `thread`: only the holder releases; a thread that timed
        // out (and so never held it) leaves nothing.
        void Leave(std::uint32_t thread) noexcept
        {
            if (m_owner.load(std::memory_order_relaxed) != thread)
                return;
            if (--m_depth > 0)
                return;
            m_owner.store(0, std::memory_order_release);
        }

        bool Held() const noexcept { return m_owner.load(std::memory_order_acquire) != 0; }
        bool HeldBy(std::uint32_t thread) const noexcept { return m_owner.load(std::memory_order_acquire) == thread; }

        GateCounters Counters() const noexcept
        {
            GateCounters out;
            out.entries = m_entries.load(std::memory_order_relaxed);
            out.waits = m_waits.load(std::memory_order_relaxed);
            out.waitUsTotal = m_waitUsTotal.load(std::memory_order_relaxed);
            out.waitUsMax = m_waitUsMax.load(std::memory_order_relaxed);
            out.timeouts = m_timeouts.load(std::memory_order_relaxed);
            out.reentries = m_reentries.load(std::memory_order_relaxed);
            return out;
        }

    private:
        static std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point since) noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
        }

        void NoteWait(std::chrono::steady_clock::time_point since) noexcept
        {
            const std::uint64_t waited = ElapsedUs(since);
            m_waitUsTotal.fetch_add(waited, std::memory_order_relaxed);
            std::uint64_t seen = m_waitUsMax.load(std::memory_order_relaxed);
            while (waited > seen && !m_waitUsMax.compare_exchange_weak(seen, waited, std::memory_order_relaxed))
            {
            }
        }

        std::atomic<std::uint32_t> m_owner{0};
        std::uint32_t m_depth = 0; // touched only by the owner
        std::atomic<std::uint64_t> m_entries{0};
        std::atomic<std::uint64_t> m_waits{0};
        std::atomic<std::uint64_t> m_waitUsTotal{0};
        std::atomic<std::uint64_t> m_waitUsMax{0};
        std::atomic<std::uint64_t> m_timeouts{0};
        std::atomic<std::uint64_t> m_reentries{0};
    };

    // RAII around one producer call.
    class GateScope
    {
    public:
        GateScope(ProducerGate& gate, std::uint32_t thread, std::uint64_t maxWaitUs = ProducerGate::kDefaultMaxWaitUs) noexcept
            : m_gate(gate), m_thread(thread), m_atOnce(gate.Enter(thread, maxWaitUs))
        {
        }
        ~GateScope() { m_gate.Leave(m_thread); }
        GateScope(const GateScope&) = delete;
        GateScope& operator=(const GateScope&) = delete;
        bool EnteredAtOnce() const noexcept { return m_atOnce; }

    private:
        ProducerGate& m_gate;
        std::uint32_t m_thread;
        bool m_atOnce;
    };
}
