// The world's ropes hidden while the replay plays (rope_world.h) and the
// hidden entities given back once the map collision around them is in
// (world_landing.h).

#include "rope_world.h"
#include "world_landing.h"

#include <cstdio>
#include <set>
#include <vector>

namespace
{
    int g_cleanupFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_cleanupFailures;
    }

    using namespace replay::ropes;

    WorldRopeView Rope(std::uint32_t id, bool replay, bool world, bool visible, float cameraM = 5.0f)
    {
        WorldRopeView view;
        view.id = id;
        view.replay = replay;
        view.onWorldEntity = world;
        view.visible = visible;
        view.cameraM = cameraM;
        return view;
    }

    void TestRopeHideAndRestore()
    {
        WorldRopeHider hider;
        hider.BeginScan();
        Check(hider.Observe(Rope(1, false, true, true), 0).action == WorldRopeAction::Hide,
            "the player's reins (world entity, drawn) are hidden on first sight");
        Check(hider.Observe(Rope(2, true, true, true), 0).action == WorldRopeAction::Leave,
            "a puppet's reins are never hidden");
        Check(hider.Observe(Rope(3, false, false, true), 0).action == WorldRopeAction::Leave,
            "a rope tied to no entity (map dressing) is left");
        Check(hider.Observe(Rope(4, false, true, false), 0).action == WorldRopeAction::Hide,
            "a world rope not drawn is still hidden (the flag is not trusted yet)");
        Check(hider.EndScan() == 0 && hider.Size() == 2, "two world ropes held");
        Check(hider.Bit() == VisibleBit::Unknown, "no hide checked yet");

        // Next scan: the hide cleared the flag of rope 1: the flag is proven.
        hider.BeginScan();
        const WorldRopeStep held = hider.Observe(Rope(1, false, true, false), 250);
        Check(held.action == WorldRopeAction::Leave && !held.stray, "a hide that held needs nothing");
        Check(hider.Observe(Rope(4, false, true, false), 250).action == WorldRopeAction::Leave, "still not drawn");
        hider.EndScan();
        Check(hider.Bit() == VisibleBit::Proven && hider.Stats().hideHeld == 1, "the flag is proven by the hide");

        // A script draws rope 1 again near the camera: hidden again, rope_stray.
        hider.BeginScan();
        const WorldRopeStep reshown = hider.Observe(Rope(1, false, true, true, 6.0f), 500);
        Check(reshown.action == WorldRopeAction::Hide && reshown.stray, "drawn again near the camera: rope_stray");
        hider.Observe(Rope(4, false, true, false), 500);
        hider.EndScan();
        hider.BeginScan();
        hider.Observe(Rope(1, false, true, false), 750);
        hider.Observe(Rope(4, false, true, false), 750);
        hider.EndScan();
        hider.BeginScan();
        const WorldRopeStep again = hider.Observe(Rope(1, false, true, true, 6.0f), 1000);
        Check(again.action == WorldRopeAction::Hide && !again.stray, "one rope_stray per rope every 2 s");
        hider.Observe(Rope(4, false, true, false), 1000);
        hider.EndScan();
        hider.BeginScan();
        const WorldRopeStep far = hider.Observe(Rope(1, false, true, true, 80.0f), 4000);
        Check(far.action == WorldRopeAction::Hide && !far.stray, "far from the camera: hidden, not a finding");
        hider.Observe(Rope(4, false, true, false), 4000);
        hider.EndScan();
        Check(hider.Stats().strays == 1 && hider.Stats().rehidden == 3, "counted");

        // Restore: only the rope drawn before the replay comes back.
        const std::vector<std::uint32_t> show = hider.TakeRestore();
        Check(show.size() == 1 && show[0] == 1, "the flag proven: a rope not drawn before stays as it was");
        Check(hider.Empty(), "the list empties at the restore");
    }

    void TestRopeUnprovenFlag()
    {
        WorldRopeHider hider;
        // The flag reads 0 on every rope: it may not be the drawn flag, so
        // every hidden rope is shown again at the end.
        hider.BeginScan();
        hider.Observe(Rope(7, false, true, false), 0);
        hider.EndScan();
        Check(hider.Bit() == VisibleBit::Unknown, "a rope not drawn proves nothing");
        const std::vector<std::uint32_t> show = hider.TakeRestore();
        Check(show.size() == 1 && show[0] == 7, "unproven flag: every hidden rope is shown again");

        // Hides the flag never follows: the flag says nothing.
        WorldRopeHider stuck;
        for (std::uint32_t scan = 0; scan < 6; ++scan)
        {
            stuck.BeginScan();
            for (std::uint32_t id = 10; id < 12; ++id)
                stuck.Observe(Rope(id, false, true, true, 3.0f), scan * 250ull);
            stuck.EndScan();
        }
        Check(stuck.Bit() == VisibleBit::Disproven, "disproven after hides that did not take");
        Check(stuck.Stats().strays == 0, "no rope_stray while the flag is not proven");
        Check(stuck.TakeRestore().size() == 2, "disproven: every hidden rope is shown again");
    }

    void TestRopeReleasedAndGone()
    {
        WorldRopeHider hider;
        hider.BeginScan();
        hider.Observe(Rope(20, false, true, true), 0);
        hider.Observe(Rope(21, false, true, true), 0);
        hider.EndScan();
        // Rope 20 now holds a puppet (built after the scan): shown again.
        hider.BeginScan();
        Check(hider.Observe(Rope(20, true, true, false), 250).action == WorldRopeAction::Show,
            "a hidden rope a puppet took is shown again");
        Check(hider.EndScan() == 1, "rope 21 was deleted by the game");
        Check(hider.Empty() && hider.Stats().released == 1 && hider.Stats().gone == 1, "released and gone counted");
    }

    using replay::landing::Action;
    using replay::landing::Decide;
    using replay::landing::Queue;
    using replay::landing::Verdict;

    void TestDecide()
    {
        Check(Decide(true, false, 100, 1000) == Verdict::Land, "collision in: lands");
        Check(Decide(false, false, 100, 1000) == Verdict::Wait, "collision missing: waits");
        Check(Decide(true, true, 100, 1000) == Verdict::Wait, "a rider waits for its mount");
        Check(Decide(false, false, 1000, 1000) == Verdict::TimedOut, "time limit");
    }

    struct Log
    {
        std::vector<std::pair<std::int32_t, Action>> actions;
    };

    void TestQueue()
    {
        Queue<int> queue;
        // 1: the player's horse, map not loaded for 3 ticks; 2: a camp ped
        // loaded at once; 3: its rider (on horse 1); 4: never loads.
        queue.Add(1, 0, 0, 101);
        queue.Add(2, 0, 0, 102);
        queue.Add(3, 1, 0, 103);
        queue.Add(4, 0, 0, 104, 2000);
        std::set<std::int32_t> loaded{2, 3};
        std::set<std::int32_t> alive{1, 2, 3, 4};
        Log log;
        const auto tick = [&](unsigned long long now) {
            queue.Tick(
                now, [&](std::int32_t h) { return alive.count(h) != 0; },
                [&](const Queue<int>::Entry& e) { return loaded.count(e.handle) != 0; },
                [&](const Queue<int>::Entry& e, Action a, unsigned long long) { log.actions.push_back({e.handle, a}); });
        };
        tick(0);
        Check(log.actions.size() == 1 && log.actions[0].first == 2 && log.actions[0].second == Action::CollisionOn,
            "a loaded entity gets its collision first, still frozen");
        Check(queue.Waiting(1) && queue.Waiting(3) && !queue.Waiting(2), "the rider waits for the horse");
        tick(100);
        Check(log.actions.size() == 2 && log.actions[1].first == 2 && log.actions[1].second == Action::Release,
            "released on the next tick");
        loaded.insert(1);
        tick(600);
        Check(log.actions.size() == 4 && log.actions[2].first == 1 && log.actions[3].first == 3,
            "horse and rider land once the map is in under the horse");
        tick(700);
        Check(log.actions.size() == 6, "both released");
        alive.erase(4);
        tick(800);
        Check(queue.Empty(), "a deleted entity leaves the queue");
        const replay::landing::Stats& stats = queue.Totals();
        Check(stats.queued == 4 && stats.landed == 3 && stats.gone == 1 && stats.timedOut == 0, "totals");
        Check(stats.waited == 3 && stats.maxWaitMs == 700 && stats.totalWaitMs == 1400,
            "three waited (one then deleted), the longest 700 ms (horse and rider)");
    }

    void TestQueueTimeoutAndTake()
    {
        Queue<int> queue;
        queue.Add(9, 0, 0, 9, 1000);
        Log log;
        const auto tick = [&](unsigned long long now) {
            queue.Tick(
                now, [](std::int32_t) { return true; }, [](const Queue<int>::Entry&) { return false; },
                [&](const Queue<int>::Entry& e, Action a, unsigned long long) {
                    Check(a != Action::CollisionOn || e.timedOut, "timed out flag set on the entry");
                    log.actions.push_back({e.handle, a});
                });
        };
        tick(500);
        Check(log.actions.empty(), "waits before the limit");
        tick(1000);
        tick(1016);
        Check(log.actions.size() == 2 && queue.Empty() && queue.Totals().timedOut == 1, "given back at the time limit");

        Queue<int> taken;
        taken.Add(5, 0, 0, 55);
        taken.Add(6, 0, 0, 66);
        std::uint32_t requests = 0;
        taken.ForRequests(1, [&](const Queue<int>::Entry&) { ++requests; });
        Check(requests == 1, "collision requests capped per tick");
        const std::vector<int> back = taken.TakeAll();
        Check(back.size() == 2 && back[0] == 55 && taken.Empty(), "a new playback takes the waiting ones back");
    }
}

namespace
{
    // A mission enemy hidden while the
    // mission still had it invisible and shown by the mission during the
    // replay goes back visible; nothing changed, nothing to note.
    void TestOwnerChange()
    {
        using replay::landing::NoteOwnerChange;
        bool visible = false, frozen = true, noCollision = true;
        Check(NoteOwnerChange(visible, frozen, noCollision, 1u) == 1u && visible, "shown by its owner: given back visible");
        Check(frozen && noCollision, "only what the owner changed");
        Check(NoteOwnerChange(visible, frozen, noCollision, 1u) == 0, "already noted: no change counted");
        Check(NoteOwnerChange(visible, frozen, noCollision, 6u) == 6u && !frozen && !noCollision,
            "unfrozen and given collision by its owner: given back so (through the landing queue)");
        bool v2 = true, f2 = false, c2 = false;
        Check(NoteOwnerChange(v2, f2, c2, 0) == 0 && v2 && !f2 && !c2, "no re-hide fix: unchanged");
    }
}

int RunWorldCleanupTests()
{
    TestOwnerChange();
    TestRopeHideAndRestore();
    TestRopeUnprovenFlag();
    TestRopeReleasedAndGone();
    TestDecide();
    TestQueue();
    TestQueueTimeoutAndTake();
    if (g_cleanupFailures == 0)
        std::printf("World cleanup tests passed.\n");
    return g_cleanupFailures;
}
