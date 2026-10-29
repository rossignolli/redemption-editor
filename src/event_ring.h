#pragma once

// The protocol of the observer rings: detours on any engine thread write
// events, the script thread reads them. No lock and no wait on the writers.
//
// A ring is an array of slots and a ticket counter (`next`). A writer takes a
// ticket with one fetch_add; the ticket names its slot (ticket % size) and
// the event's place in the order. Each slot carries:
//
// - sequence: 0 never written; ticket + 1 when the slot holds that ticket's
//   event (published); (ticket + 1) | kWriting while that ticket's writer
//   fills the payload. Only the writer that set kWriting changes the slot
//   until it publishes or abandons it, so two writers never fill one
//   payload at once.
// - dropped: ticket + 1 of the last ticket whose writer could not take the
//   slot (another writer, a full lap behind, still filling it) or abandoned
//   it. The reader passes such a ticket instead of waiting for it.
//
// A writer takes its slot only from a state older than its ticket and not
// being written (Claim); otherwise its event is dropped and counted. The
// reader of a ticket copies the payload between two loads of the sequence
// (a seqlock): a sequence that changed during the copy means a newer ticket
// took the slot, and the copy is thrown away and counted (torn).
//
// Readers that keep a cursor (ForEachSince) stop at the first ticket that is
// taken but not yet published and read it again on the next call, so an
// event whose writer was preempted between its fetch_add and its publish is
// read late, never skipped. A ticket overwritten by a later lap (the ring
// overflowed) or dropped by its writer is passed and counted. Window
// readers (ForEachPublished) read what is published now and look again
// next time.
//
// The payload copy is a plain copy of a trivially copyable value, checked
// by the sequence afterwards; the fences order it against the sequence
// loads and stores.

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace replay::ring
{
    inline constexpr std::uint64_t kWriting = 1ull << 63;

    // A slot of a ring of T.
    template <typename T>
    struct Slot
    {
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> dropped{0};
        T value;
    };

    // What the protocol saw, process wide (logged at unload).
    struct Counters
    {
        std::atomic<std::uint64_t> writerDropped{0}; // writers that found the slot taken
        std::atomic<std::uint64_t> torn{0};          // copies thrown away: the slot changed during the copy
        std::atomic<std::uint64_t> lost{0};          // tickets a reader passed: overwritten, dropped or overflowed
        std::atomic<std::uint64_t> waited{0};        // reads that stopped at a ticket not yet published
    };
    inline Counters g_counters;

    // Writer: takes the slot for `ticket`. False: the slot is being written
    // by another ticket or already holds a newer one (the event is dropped).
    inline bool Claim(std::atomic<std::uint64_t>& sequence, std::uint64_t ticket) noexcept
    {
        const std::uint64_t mine = ticket + 1;
        std::uint64_t seen = sequence.load(std::memory_order_acquire);
        for (;;)
        {
            if ((seen & kWriting) != 0 || seen >= mine)
                return false;
            // acq_rel: the payload writes after this cannot move above it.
            if (sequence.compare_exchange_weak(seen, mine | kWriting, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    // Writer, after the payload: the event is readable.
    inline void Publish(std::atomic<std::uint64_t>& sequence, std::uint64_t ticket) noexcept
    {
        sequence.store(ticket + 1, std::memory_order_release);
    }

    // Writer that took a ticket and writes no event for it (could not claim
    // the slot, or gave up after claiming it).
    inline void Drop(std::atomic<std::uint64_t>& dropped, std::uint64_t ticket) noexcept
    {
        dropped.store(ticket + 1, std::memory_order_release);
        g_counters.writerDropped.fetch_add(1, std::memory_order_relaxed);
    }

    // The writer side of one event: takes a ticket, claims its slot and
    // exposes `value` to fill (the slot's payload, or a per-thread scratch
    // when the slot could not be claimed, so the writer's code is the same).
    // Publish() makes it readable; a writer destroyed without Publish()
    // gives the ticket up.
    template <typename T>
    class Writer
    {
    public:
        Writer(Slot<T>* ring, std::uint64_t size, std::atomic<std::uint64_t>& next) noexcept
            : Writer(ring, size, next.fetch_add(1, std::memory_order_relaxed))
        {
        }

        Writer(Slot<T>* ring, std::uint64_t size, std::uint64_t ticket) noexcept
            : m_slot(ring[ticket % size]), m_ticket(ticket), m_owned(Claim(m_slot.sequence, ticket)),
              value(m_owned ? m_slot.value : Scratch())
        {
            if (!m_owned)
                Drop(m_slot.dropped, ticket);
        }

        ~Writer()
        {
            if (m_published || !m_owned)
                return;
            // Claimed and given up: the slot is free again, the ticket passed.
            m_slot.sequence.store(0, std::memory_order_release);
            Drop(m_slot.dropped, m_ticket);
        }

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        std::uint64_t Ticket() const noexcept { return m_ticket; }
        bool Owned() const noexcept { return m_owned; }

        void Publish() noexcept
        {
            if (m_published)
                return;
            m_published = true;
            if (m_owned)
                replay::ring::Publish(m_slot.sequence, m_ticket);
        }

    private:
        static T& Scratch() noexcept
        {
            thread_local T scratch{};
            return scratch;
        }

        Slot<T>& m_slot;
        std::uint64_t m_ticket;
        bool m_owned;
        bool m_published = false;

    public:
        T& value; // what the writer fills
    };

    enum class Read
    {
        Ready,   // `out` holds the ticket's event
        Pending, // taken, not published yet: read it again later
        Lost,    // overwritten by a newer ticket or dropped by its writer
    };

    // Reader: the event of `ticket` from its slot.
    template <typename T>
    Read TryRead(const Slot<T>& slot, std::uint64_t ticket, T& out) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "a ring payload is copied while it may be rewritten");
        const std::uint64_t mine = ticket + 1;
        const std::uint64_t before = slot.sequence.load(std::memory_order_acquire);
        if (before == mine)
        {
            out = slot.value;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.sequence.load(std::memory_order_relaxed) == mine)
                return Read::Ready;
            // A newer ticket took the slot during the copy.
            g_counters.torn.fetch_add(1, std::memory_order_relaxed);
            return Read::Lost;
        }
        if ((before & ~kWriting) > mine)
            return Read::Lost; // a newer lap took the slot
        if (slot.dropped.load(std::memory_order_acquire) == mine)
            return Read::Lost; // its writer gave it up
        return Read::Pending;
    }

    // What one ForEachSince call passed.
    struct Drained
    {
        std::uint64_t read = 0;
        std::uint64_t overflowed = 0; // older than one ring when the call began
        std::uint64_t lost = 0;       // overwritten, torn or dropped by the writer
        bool waiting = false;         // stopped at a ticket not yet published
    };

    // Reader with a cursor: every event from `cursor` on, in ticket order,
    // until the first ticket not yet published (the cursor stays on it).
    // Tickets older than one ring are overwritten: passed and counted.
    template <typename T, typename Fn>
    Drained ForEachSince(const Slot<T>* ring, std::uint64_t size, const std::atomic<std::uint64_t>& next, std::uint64_t& cursor,
        Fn&& each)
    {
        Drained drained;
        const std::uint64_t end = next.load(std::memory_order_acquire);
        if (cursor > end)
            cursor = end;
        if (end - cursor > size)
        {
            drained.overflowed = end - size - cursor;
            g_counters.lost.fetch_add(drained.overflowed, std::memory_order_relaxed);
            cursor = end - size;
        }
        for (; cursor < end; ++cursor)
        {
            T value;
            const Read read = TryRead(ring[cursor % size], cursor, value);
            if (read == Read::Pending)
            {
                g_counters.waited.fetch_add(1, std::memory_order_relaxed);
                drained.waiting = true;
                break;
            }
            if (read == Read::Lost)
            {
                g_counters.lost.fetch_add(1, std::memory_order_relaxed);
                ++drained.lost;
                continue;
            }
            each(value);
            ++drained.read;
        }
        return drained;
    }

    // Reader without a cursor: what the last `size` tickets hold that is
    // published now (a window read looks again next time).
    template <typename T, typename Fn>
    void ForEachPublished(const Slot<T>* ring, std::uint64_t size, const std::atomic<std::uint64_t>& next, Fn&& each)
    {
        const std::uint64_t end = next.load(std::memory_order_acquire);
        const std::uint64_t first = end > size ? end - size : 0;
        for (std::uint64_t ticket = first; ticket < end; ++ticket)
        {
            T value;
            if (TryRead(ring[ticket % size], ticket, value) == Read::Ready)
                each(value);
        }
    }
}
