// The scrub and jump model (seek_settle.h): far jump rules, the settle wait,
// the model window, the creation budget and hold, the timeline drag gate and
// the scrub cost line.
#include "entity_lifecycle.h"
#include "seek_settle.h"
#include "state_budget.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay::settle;

    int g_settleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_settleFailures;
    }

    void TestFarBits()
    {
        JumpFacts facts;
        facts.windowCentreMs = 10000;
        facts.targetMs = 12000;
        Check(FarBitsOf(facts) == kFarNone, "a 2 s jump inside the window, camera unknown, is near");
        facts.targetMs = 14001;
        Check(FarBitsOf(facts) == kFarOutsideWindow, "a jump past the 4 s window is far");
        facts.targetMs = 5999;
        Check(FarBitsOf(facts) == kFarOutsideWindow, "backward past the window too");
        facts.targetMs = 10500;
        facts.cameraValid = true;
        facts.cameraMovedMeters = 150.0f;
        Check(FarBitsOf(facts) == kFarCamera, "the camera moving 150 m is far");
        facts.cameraMovedMeters = 20.0f;
        facts.collisionLoaded = false;
        Check(FarBitsOf(facts) == kFarNoCollision, "20 m onto ground without collision is far");
        facts.cameraMovedMeters = 0.5f;
        Check(FarBitsOf(facts) == kFarNone, "under a metre the collision is not asked");
        facts.cameraMovedMeters = 150.0f;
        facts.fine = true;
        Check(FarBitsOf(facts) == kFarNone, "a frame step is never far by the camera");
        facts.puppetsWithoutModel = 2;
        Check(FarBitsOf(facts) == kFarModels, "a frame step onto puppets without their model is far");
        facts.fine = false;
        facts.puppetsWithoutModel = 0;
        facts.callerWantsFreeze = true;
        Check((FarBitsOf(facts) & kFarCaller) != 0, "the caller may ask for the frozen picture");
        Check(FarReasons(kFarOutsideWindow | kFarCamera) == "outside_window+camera_far", "far reasons read in bit order");
        Check(FarReasons(0) == "near", "no reason reads near");

        Check(IsJumpMove(1000, 1033, true, false), "a step while paused is a jump");
        Check(!IsJumpMove(1000, 1016, false, false), "a normal play step is not");
        Check(IsJumpMove(1000, 1400, false, false) && IsJumpMove(1000, 900, false, false), "a long or backward move in play is");
        Check(!IsJumpMove(1000, 5000, true, true), "a scrub tick is not a jump");
        Check(!IsJumpMove(1000, 1000, true, false), "no move is no jump");
    }

    void TestSettle()
    {
        Config config;
        Settle settle;
        Readiness waiting;
        waiting.building = 3;
        Readiness ready;

        settle.Begin("scrub_release", 1000, 5000, kFarNone);
        Check(settle.Active() && !settle.Far(), "a near settle holds without the frozen picture");
        Verdict verdict = settle.Update(1016, waiting, config);
        Check(!verdict.done && settle.StableTicks() == 0, "puppets still building: not done");
        std::uint64_t now = 1032;
        for (std::uint32_t tick = 0; tick + 1 < config.stableTicks; ++tick, now += 16)
        {
            verdict = settle.Update(now, ready, config);
            Check(!verdict.done, "ready for fewer ticks than stableTicks: not done");
        }
        verdict = settle.Update(now, waiting, config);
        Check(!verdict.done && settle.StableTicks() == 0, "a tick not ready starts the count again");
        for (std::uint32_t tick = 0; tick < config.stableTicks; ++tick, now += 16)
            verdict = settle.Update(now, ready, config);
        Check(verdict.done && !verdict.timedOut && !settle.Active(), "stable ticks in a row end the settle");
        Check(settle.EndedElapsedMs() == now - 16 - 1000, "the wait measured from the first jump");

        // Deferred state and pending reconciles hold it too.
        Readiness deferred;
        deferred.stateDeferred = true;
        Check(!IsReady(deferred) && Missing(deferred) == "state_deferred", "the deferred rebuild is waited for");
        Readiness pending;
        pending.statePending = true;
        pending.sceneLoaded = false;
        Check(Missing(pending) == "state_pending+scene", "missing parts listed");

        // Promotion: a near settle that goes on past 400 ms freezes.
        settle.Begin("seek", 0, 1000, kFarNone);
        verdict = settle.Update(399, waiting, config);
        Check(!verdict.promoted && !settle.Far(), "not promoted before 400 ms");
        verdict = settle.Update(400, waiting, config);
        Check(verdict.promoted && settle.Far() && (settle.FarBits() & kFarSlow), "promoted to the frozen picture at 400 ms");

        // The stall limit.
        verdict = settle.Update(6599, waiting, config);
        Check(!verdict.done, "not given up before 6.6 s");
        verdict = settle.Update(6600, waiting, config);
        Check(verdict.done && verdict.timedOut, "given up at 6.6 s (the stall limit)");

        // A new jump retargets: the count and the limit start again; far stays.
        settle.Begin("seek", 0, 1000, kFarOutsideWindow);
        settle.Update(3000, waiting, config);
        settle.Begin("seek", 3000, 9000, kFarNone);
        Check(settle.Retargets() == 1 && settle.Far() && settle.TargetMs() == 9000, "a retarget keeps the frozen picture");
        verdict = settle.Update(9000, waiting, config);
        Check(!verdict.done, "the stall limit starts again at the retarget");
        verdict = settle.Update(9600, waiting, config);
        Check(verdict.done && verdict.timedOut && settle.EndedElapsedMs() == 9600, "retargeted wait gives up 6.6 s after the retarget");

        // A scrub cancels it.
        settle.Begin("seek", 0, 1000, kFarCamera);
        settle.Cancel(50);
        Check(!settle.Active() && !settle.Far(), "a scrub cancels the settle");
        Check(settle.Update(60, ready, config).done == false, "a cancelled settle does nothing");
    }

    void TestModelPreload()
    {
        Config config;
        config.maxPreloads = 3;
        ModelPreload preload;
        std::vector<std::uint32_t> request;
        std::vector<std::uint32_t> release;
        preload.Update(0, {11, 12, 11, 0}, {}, request, release, config);
        Check(request == std::vector<std::uint32_t>({11, 12}) && release.empty(), "the window's models requested once each");
        preload.Update(1000, {11, 12, 13, 14}, {}, request, release, config);
        Check(request.size() == 3 && preload.Count() == 3 && preload.Capped() == 1, "the preload cap holds");
        preload.Update(4000, {11}, {}, request, release, config);
        Check(release.empty(), "nothing released before 5 s untouched");
        preload.Update(6000, {11}, {}, request, release, config);
        Check(release.size() == 2 && preload.Holds(11) && !preload.Holds(12) && !preload.Holds(13),
            "models untouched for 5 s are released, a touched one stays");
        preload.Update(6100, {11, 15}, {11}, request, release, config);
        Check(!preload.Holds(11) && preload.Holds(15) && request == std::vector<std::uint32_t>({15}),
            "a model a puppet owns is forgotten, never released by the window");
        preload.Update(20000, {}, {11}, request, release, config);
        Check(release == std::vector<std::uint32_t>({15}), "the owned model is not in the release list");
        preload.Update(20001, {16}, {}, request, release, config);
        preload.Clear(release);
        Check(release == std::vector<std::uint32_t>({16}) && preload.Count() == 0, "stop releases what the window held");
    }

    // The lifecycle's models in a window: alive at its start, created in
    // it (in any block), never the weapons.
    void TestModelsBetween()
    {
        using namespace replay::entity;
        IdAllocator ids;
        const ReplayId a = ids.Allocate();
        const ReplayId b = ids.Allocate();
        const ReplayId c = ids.Allocate();
        const ReplayId gun = ids.Allocate();
        const auto packet = [](ReplayId id, std::uint32_t timeMs, std::uint32_t model) {
            CreatePacket created;
            created.id = id;
            created.timeMs = timeMs;
            created.modelHash = model;
            return created;
        };
        LifecycleTrack track;
        track.Begin(2000);
        track.Create(packet(a, 0, 100));
        CreatePacket weapon = packet(gun, 0, 900);
        weapon.weaponHash = 0x1234;
        track.Create(weapon);
        for (std::uint32_t t = 0; t <= 3000; t += 100)
            track.Advance(t);
        track.Delete(a, 3000);
        track.Create(packet(b, 3000, 200));
        for (std::uint32_t t = 3000; t <= 9000; t += 100)
            track.Advance(t);
        track.Create(packet(c, 9000, 300));
        track.Advance(12000);
        std::vector<std::uint32_t> models;
        track.ModelsBetween(0, 2000, models);
        Check(models == std::vector<std::uint32_t>({100}), "the window at the start holds the first model, no weapon");
        track.ModelsBetween(2500, 6500, models);
        Check(models == std::vector<std::uint32_t>({100, 200}), "alive at the window start plus the created ones");
        track.ModelsBetween(4000, 8000, models);
        Check(models == std::vector<std::uint32_t>({200}), "a deleted entity leaves the window");
        track.ModelsBetween(5000, 13000, models);
        Check(models == std::vector<std::uint32_t>({200, 300}), "creates in later blocks join");
    }

    void TestCreationBudget()
    {
        Check(CreateBudget(false, false) == 4 && CreateBudget(true, false) == 6 && CreateBudget(true, true) == 12,
            "4 in play, 6 settling, 12 behind the frozen picture");
        Config config;
        Check(HoldForCreation(true, 2, 0, config), "normal play holds a frame for a due puppet with its model");
        Check(!HoldForCreation(true, 0, 0, config), "nothing due: no hold");
        Check(!HoldForCreation(false, 2, 0, config), "only normal play holds this way");
        Check(!HoldForCreation(true, 2, config.maxCreationHoldFrames, config), "the hold is bounded");
    }

    void TestDragGate()
    {
        Config config;
        DragGate gate;
        Check(gate.OnSeek(0, SeekKind::Plain), "a plain seek settles");
        Check(!gate.OnSeek(10, SeekKind::Dragging) && gate.Dragging(), "a drag seek does not");
        Check(!gate.OnSeek(26, SeekKind::Dragging) && !gate.TakeIdleEnd(400, config), "still dragging");
        Check(gate.OnSeek(40, SeekKind::DragEnd) && !gate.Dragging(), "the release settles");
        gate.OnSeek(1000, SeekKind::Dragging);
        Check(!gate.TakeIdleEnd(1499, config) && gate.TakeIdleEnd(1500, config) && !gate.Dragging(),
            "a drag without its end settles after 500 ms idle");
        Check(!gate.TakeIdleEnd(3000, config), "the idle end is taken once");
    }

    void TestScrubCost()
    {
        CostCounters before;
        before.decodes = 100;
        before.bloodReconciles = 7;
        ScrubCost cost;
        cost.Begin("keys", -1, 30000, before);
        cost.Tick(12000.0, 20);
        cost.Tick(4000.0, 22);
        CostCounters after = before;
        after.decodes = 400;
        after.bloodReconciles = 9;
        after.deferredTicks = 2;
        const std::string line = cost.Finish(26000, 30000.0, after, true);
        Check(!cost.Active(), "finished");
        Check(line.find("SCRUB_COST,kind=keys,direction=-1,from_ms=30000,to_ms=26000,ticks=2,avg_ms=8.00,max_ms=12.00,release_ms=30.00,puppets=22,decodes=300") == 0,
            "the cost line sums the scrub");
        Check(line.find("blood_reconciles=2") != std::string::npos && line.find("deferred=1,deferred_ticks=2") != std::string::npos,
            "reconciles and deferred ticks are differences");
        // A counter reset in the middle (a new playback) never underflows.
        Check(Since(5, 9) == 5, "a counter that went back counts from zero");
    }

    // The blood rebuild after a jump as a queue under a time budget per tick
    // (state_budget.h).
    void TestStateBudget()
    {
        using namespace replay::budget;
        Check(StateBudgetUs(false, false) == kPlayUs && StateBudgetUs(true, false) == kSettleUs &&
                StateBudgetUs(true, true) == kFrozenUs && kPlayUs <= 4000 && kFrozenUs > kSettleUs,
            "play, settle and frozen budgets");

        // Plans: only the difference.
        Check(PlanPed(true, 5000, true, true, 5000, 6000) == PedPlan::Nothing, "holds exactly the target: nothing");
        Check(PlanPed(true, 3000, true, true, 5000, 6000) == PedPlan::Forward, "forward with the same puppet adds the rest");
        Check(PlanPed(true, 3000, false, true, 5000, 6000) == PedPlan::Rebuild, "a new puppet rebuilds");
        Check(PlanPed(true, 7000, true, true, 5000, 6000) == PedPlan::Rebuild, "a record past the target rebuilds");
        Check(PlanPed(false, 0, false, true, 5000, 6000) == PedPlan::Rebuild, "nothing laid yet: lay all");
        Check(PlanPed(false, 0, false, false, 0, 6000) == PedPlan::Nothing, "no record due: nothing");
        Check(PlanPed(true, 7000, true, false, 0, 6000) == PedPlan::Rebuild, "back before the first record clears");

        // The budget: each op costs 1 ms on a fake clock.
        std::uint64_t clock = 0;
        const auto now = [&clock]() { return clock; };
        OpQueue queue;
        queue.PushClear(7);
        for (std::size_t index = 0; index < 10; ++index)
            queue.PushApply(7, index);
        Check(queue.Holds(7) && !queue.Holds(8), "holds the ped it plans");
        std::vector<Op> ran;
        const auto run = [&](const Op& op) {
            ran.push_back(op);
            clock += 1000;
        };
        RunStats stats = queue.Run(4000, now, run);
        Check(stats.done == 4 && stats.exhausted && stats.clears == 1 && queue.Size() == 7, "4 ms of 1 ms ops: 4 ops, then stop");
        Check(ran.front().kind == Op::Kind::Clear && ran[1].kind == Op::Kind::Apply && ran[1].index == 0, "the clear goes first, then in order");
        // An op dearer than the whole budget still runs: the queue always drains.
        const auto slow = [&](const Op& op) {
            ran.push_back(op);
            clock += 50000;
        };
        stats = queue.Run(4000, now, slow);
        Check(stats.done == 1 && stats.exhausted, "at least one op per tick");
        std::uint32_t ticks = 0;
        while (!queue.Empty() && ticks < 100)
        {
            queue.Run(kFrozenUs, now, run);
            ++ticks;
        }
        Check(queue.Empty() && ran.size() == 11 && ticks == 1, "the frozen budget drains the rest in one tick");
        for (std::size_t n = 1; n < ran.size(); ++n)
            Check(ran[n].index == n - 1, "every record once, in order");

        // Budgeted over many ticks ends on the same state as all at once:
        // a ped model holding record indices, a clear empties it.
        const auto simulate = [](std::uint32_t budgetUs, std::uint32_t& tickCount) {
            std::vector<std::size_t> held = {0, 1, 2, 3, 4, 5};
            OpQueue plan;
            plan.PushClear(1);
            for (std::size_t index = 0; index < 4; ++index)
                plan.PushApply(1, index);
            std::uint64_t t = 0;
            tickCount = 0;
            while (!plan.Empty())
            {
                plan.Run(budgetUs, [&t]() { return t; }, [&](const Op& op) {
                    if (op.kind == Op::Kind::Clear)
                        held.clear();
                    else
                        held.push_back(op.index);
                    t += 3000;
                });
                ++tickCount;
            }
            return held;
        };
        std::uint32_t oneTicks = 0;
        std::uint32_t budgetTicks = 0;
        const std::vector<std::size_t> once = simulate(1000000, oneTicks);
        const std::vector<std::size_t> budgeted = simulate(kPlayUs, budgetTicks);
        Check(once == budgeted && once.size() == 4 && oneTicks == 1 && budgetTicks == 3,
            "the budgeted rebuild (3 ticks) ends on the one-tick state");

        // The settle waits while the queue has work.
        Readiness readiness;
        readiness.statePending = true;
        Check(!IsReady(readiness) && Missing(readiness) == "state_pending", "a queue with work keeps the settle waiting");
        queue.Reset();
        Check(queue.Empty(), "reset empties");
    }
}

int RunSeekSettleTests()
{
    TestFarBits();
    TestSettle();
    TestModelPreload();
    TestModelsBetween();
    TestCreationBudget();
    TestDragGate();
    TestScrubCost();
    TestStateBudget();
    if (g_settleFailures == 0)
        std::puts("Seek settle tests passed.");
    return g_settleFailures;
}
