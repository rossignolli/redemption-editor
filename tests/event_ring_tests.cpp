// The observer rings' protocol (src/event_ring.h): writers on any thread,
// one reader with a cursor. A ticket taken and not yet published stops the
// reader (read on the next call, never skipped); a copy that a newer writer
// tore is thrown away and counted; a writer that cannot claim its slot drops
// its event and the reader passes it.
#include "event_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
    int g_ringFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_ringFailures;
    }

    struct Event
    {
        std::uint64_t ticket = 0;
        std::uint64_t words[15] = {}; // every word = ticket * 3 + 1: a torn copy mixes two tickets
    };

    void Fill(Event& event, std::uint64_t ticket)
    {
        event.ticket = ticket;
        for (std::uint64_t& word : event.words)
            word = ticket * 3 + 1;
    }

    bool Whole(const Event& event)
    {
        for (const std::uint64_t word : event.words)
            if (word != event.ticket * 3 + 1)
                return false;
        return true;
    }

    void TestReservedNotPublished()
    {
        using namespace replay::ring;
        static Slot<Event> ring[8];
        std::atomic<std::uint64_t> next{0};
        std::uint64_t cursor = 0;
        std::vector<std::uint64_t> seen;
        const auto read = [&](const Event& event) { seen.push_back(event.ticket); };

        {
            Writer<Event> a(ring, 8, next);
            Fill(a.value, a.Ticket());
            a.Publish();
        }
        // Ticket 1 taken and still being filled while ticket 2 is published:
        // the writer of 1 was preempted between its fetch_add and its publish.
        Writer<Event>* slow = new Writer<Event>(ring, 8, next);
        Fill(slow->value, slow->Ticket());
        {
            Writer<Event> c(ring, 8, next);
            Fill(c.value, c.Ticket());
            c.Publish();
        }
        Drained drained = ForEachSince(ring, 8, next, cursor, read);
        Check(seen.size() == 1 && seen[0] == 0 && cursor == 1 && drained.waiting,
            "the reader stops at the ticket being written and keeps its place");
        drained = ForEachSince(ring, 8, next, cursor, read);
        Check(seen.size() == 1 && cursor == 1, "still being written: still waiting");
        slow->Publish();
        delete slow;
        drained = ForEachSince(ring, 8, next, cursor, read);
        Check(seen.size() == 3 && seen[1] == 1 && seen[2] == 2 && cursor == 3 && !drained.waiting && drained.lost == 0,
            "once published the late event is read, in ticket order, nothing lost");
    }

    void TestWriterDropsAndAbandons()
    {
        using namespace replay::ring;
        static Slot<Event> ring[4];
        std::atomic<std::uint64_t> next{0};
        std::uint64_t cursor = 0;
        std::vector<std::uint64_t> seen;
        const auto read = [&](const Event& event) { seen.push_back(event.ticket); };

        // Ticket 0 is still being written a full lap later: ticket 4 (same
        // slot) cannot claim it and drops its event instead of writing into
        // the payload ticket 0's writer is filling.
        Writer<Event>* lagging = new Writer<Event>(ring, 4, next);
        Fill(lagging->value, 0);
        for (int n = 1; n < 4; ++n)
        {
            Writer<Event> w(ring, 4, next);
            Fill(w.value, w.Ticket());
            w.Publish();
        }
        const std::uint64_t droppedBefore = g_counters.writerDropped.load();
        {
            Writer<Event> late(ring, 4, next);
            Check(!late.Owned() && late.Ticket() == 4, "a slot being written is not taken by the next lap");
            Fill(late.value, 4); // the scratch, not the slot
            late.Publish();
        }
        Check(g_counters.writerDropped.load() == droppedBefore + 1, "the drop is counted");
        Check(ring[0].value.ticket == 0 && Whole(ring[0].value), "the lagging writer's payload was not touched");
        lagging->Publish();
        delete lagging;
        cursor = 1; // ticket 0 is a lap old: what matters is 1..4
        const Drained drained = ForEachSince(ring, 4, next, cursor, read);
        Check(seen.size() == 3 && seen[0] == 1 && seen[2] == 3 && cursor == 5 && drained.lost == 1 && !drained.waiting,
            "the dropped ticket is passed, not waited for");

        // A writer that claims and gives up (an early return) passes its ticket.
        {
            Writer<Event> abandoned(ring, 4, next);
            Check(abandoned.Owned(), "a free slot is claimed");
        }
        {
            Writer<Event> w(ring, 4, next);
            Fill(w.value, w.Ticket());
            w.Publish();
        }
        seen.clear();
        const Drained after = ForEachSince(ring, 4, next, cursor, read);
        Check(seen.size() == 1 && seen[0] == 6 && after.lost == 1 && cursor == 7, "an abandoned ticket is passed");
    }

    void TestOverflowAndTornCopy()
    {
        using namespace replay::ring;
        static Slot<Event> ring[4];
        std::atomic<std::uint64_t> next{0};
        std::uint64_t cursor = 0;
        std::vector<std::uint64_t> seen;
        for (int n = 0; n < 10; ++n)
        {
            Writer<Event> w(ring, 4, next);
            Fill(w.value, w.Ticket());
            w.Publish();
        }
        const Drained drained = ForEachSince(ring, 4, next, cursor, [&](const Event& event) { seen.push_back(event.ticket); });
        Check(drained.overflowed == 6 && seen.size() == 4 && seen[0] == 6 && cursor == 10, "a reader a lap behind skips what was overwritten");

        // The slot of ticket 9 taken by ticket 13 after the reader saw 9:
        // TryRead of 9 must not return 13's (or a mix of) data.
        Event out;
        {
            Writer<Event> w(ring, 4, std::uint64_t{13});
            Check(w.Owned(), "a newer lap claims a published slot");
            Check(TryRead(ring[13 % 4], 9, out) == Read::Lost, "a ticket whose slot a newer writer holds is lost");
            Fill(w.value, 13);
            w.Publish();
        }
        Check(TryRead(ring[13 % 4], 13, out) == Read::Ready && out.ticket == 13 && Whole(out), "the newer ticket reads whole");
        Check(TryRead(ring[0], 12, out) == Read::Pending, "a ticket not taken yet is pending");
    }

    // Writers on several threads against a reader on another, small ring so
    // laps and collisions happen: everything the reader accepts is whole and
    // in ticket order, and every ticket is either read or counted.
    void TestThreads()
    {
        using namespace replay::ring;
        constexpr std::uint64_t kSize = 64;
        static Slot<Event> ring[kSize];
        std::atomic<std::uint64_t> next{0};
        std::atomic<bool> go{false};
        std::atomic<int> writersDone{0};
        constexpr int kWriters = 4;
        constexpr int kPerWriter = 50000;
        std::vector<std::thread> writers;
        for (int w = 0; w < kWriters; ++w)
            writers.emplace_back([&]() {
                while (!go.load())
                    std::this_thread::yield();
                for (int n = 0; n < kPerWriter; ++n)
                {
                    Writer<Event> slot(ring, kSize, next);
                    Fill(slot.value, slot.Ticket());
                    if ((n & 255) == 0)
                        std::this_thread::yield(); // a writer preempted between its ticket and its publish
                    slot.Publish();
                }
                writersDone.fetch_add(1);
            });
        std::uint64_t cursor = 0;
        std::uint64_t read = 0, lost = 0, torn = 0, outOfOrder = 0, lastTicket = 0;
        bool first = true;
        const std::uint64_t tornBefore = g_counters.torn.load();
        const auto each = [&](const Event& event) {
            ++read;
            if (!Whole(event))
                ++torn;
            if (!first && event.ticket <= lastTicket)
                ++outOfOrder;
            first = false;
            lastTicket = event.ticket;
        };
        go.store(true);
        while (writersDone.load() < kWriters)
        {
            const Drained drained = ForEachSince(ring, kSize, next, cursor, each);
            lost += drained.overflowed + drained.lost;
        }
        for (std::thread& t : writers)
            t.join();
        const Drained last = ForEachSince(ring, kSize, next, cursor, each);
        lost += last.overflowed + last.lost;
        const std::uint64_t total = static_cast<std::uint64_t>(kWriters) * kPerWriter;
        Check(torn == 0, "threads: no torn event is ever accepted");
        Check(outOfOrder == 0, "threads: events come in ticket order");
        Check(cursor == total && !last.waiting, "threads: the reader reaches the end once the writers stopped");
        Check(read + lost == total, "threads: every ticket is read or counted");
        std::printf("    ring threads: %llu tickets, %llu read, %llu passed, %llu torn copies discarded\n",
            static_cast<unsigned long long>(total), static_cast<unsigned long long>(read), static_cast<unsigned long long>(lost),
            static_cast<unsigned long long>(g_counters.torn.load() - tornBefore));

        // A ring larger than the burst loses nothing, even with preempted writers.
        constexpr std::uint64_t kBig = 1 << 16;
        static Slot<Event> big[kBig];
        std::atomic<std::uint64_t> bigNext{0};
        std::uint64_t bigCursor = 0, bigRead = 0, bigLost = 0;
        std::atomic<int> bigDone{0};
        std::vector<std::thread> bigWriters;
        for (int w = 0; w < kWriters; ++w)
            bigWriters.emplace_back([&]() {
                for (int n = 0; n < 10000; ++n)
                {
                    Writer<Event> slot(big, kBig, bigNext);
                    Fill(slot.value, slot.Ticket());
                    if ((n & 63) == 0)
                        std::this_thread::yield();
                    slot.Publish();
                }
                bigDone.fetch_add(1);
            });
        while (bigDone.load() < kWriters)
        {
            const Drained drained = ForEachSince(big, kBig, bigNext, bigCursor, [&](const Event& event) { bigRead += Whole(event) ? 1 : 0; });
            bigLost += drained.overflowed + drained.lost;
        }
        for (std::thread& t : bigWriters)
            t.join();
        const Drained bigLast = ForEachSince(big, kBig, bigNext, bigCursor, [&](const Event& event) { bigRead += Whole(event) ? 1 : 0; });
        bigLost += bigLast.overflowed + bigLast.lost;
        Check(bigRead == static_cast<std::uint64_t>(kWriters) * 10000 && bigLost == 0,
            "threads: with room in the ring every event is read, none skipped while its writer was preempted");
    }
}

int RunEventRingTests()
{
    TestReservedNotPublished();
    TestWriterDropsAndAbandons();
    TestOverflowAndTornCopy();
    TestThreads();
    if (g_ringFailures == 0)
        std::printf("event ring tests passed\n");
    return g_ringFailures;
}
