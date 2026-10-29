// Town NPCs: the entity discovery without the pool listing
// (entity_discovery.h) and the missing puppet detector (puppet_watch.h).
#include "entity_discovery.h"
#include "puppet_watch.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
    int g_watchFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_watchFailures;
    }

    void TestNearbyWalk()
    {
        using replay::entity::NearbyWalk;
        // A street: the player (1) sees 10 and 11; 10 sees 20; 20 sees 30
        // and the player; 30 sees 40. The listing gave nothing.
        std::map<std::int32_t, std::vector<std::int32_t>> graph = {
            {1, {10, 11}}, {10, {1, 20}}, {11, {1}}, {20, {1, 30}}, {30, {40}}, {40, {}}};
        std::uint32_t queries = 0;
        const auto query = [&](std::int32_t seed, std::int32_t* out, int capacity) {
            ++queries;
            const std::vector<std::int32_t>& near = graph[seed];
            int n = 0;
            for (const std::int32_t h : near)
                if (n < capacity)
                    out[n++] = h;
            return n;
        };
        std::int32_t handles[16]{};
        int count = 0;
        replay::entity::NearbyWalkStats stats = NearbyWalk({1}, query, handles, count, 16, 64);
        Check(count == 6, "the walk reaches every ped of the chain, the seed included when a neighbour lists it");
        Check(stats.found == 6 && !stats.budgetSpent, "found counts the handles added");
        Check(stats.calls == 6, "each ped is asked once");

        // Handles the listing already had are not added twice.
        count = 2;
        handles[0] = 10;
        handles[1] = 40;
        stats = NearbyWalk({1}, query, handles, count, 16, 64);
        Check(count == 6 && stats.found == 4, "known handles are kept once");

        // The call budget stops the spread.
        count = 0;
        stats = NearbyWalk({1}, query, handles, count, 16, 2);
        Check(stats.calls == 2 && stats.budgetSpent, "the budget bounds the queries");
        Check(count == 4, "two calls reach the player's and the first neighbour's lists");

        // The capacity bounds the output, the walk goes on to the budget.
        count = 0;
        stats = NearbyWalk({1}, query, handles, count, 2, 64);
        Check(count == 2, "the output never passes its capacity");

        // Seeds are deduplicated, zero seeds skipped.
        count = 0;
        stats = NearbyWalk({0, 11, 11, 1}, query, handles, count, 16, 64);
        Check(stats.seeds == 2, "zero and repeated seeds are dropped");

        std::int32_t extra[4]{5, 0, 0, 0};
        int extraCount = 1;
        Check(replay::entity::AddMissing({5, 6, 0, 6}, extra, extraCount, 4) == 1 && extraCount == 2,
            "dependents missing from the listing are added once");

        Check(replay::entity::IsLateEntry(48800, 11.0f), "a ped entering at 48.8 s, 11 m from the camera, is late");
        Check(!replay::entity::IsLateEntry(500, 11.0f), "the opening census is not late");
        Check(!replay::entity::IsLateEntry(48800, 120.0f), "far from the camera it is the normal entry");
    }

    void TestPuppetWatch()
    {
        using namespace replay::watch;
        Config config;
        config.thresholdMs = 500;
        PuppetWatch watch(config);
        std::vector<Event> events;
        std::vector<Observation> missing = {{7, Reason::CreateWait, 20.0f}};
        watch.Update(1000, missing, events);
        watch.Update(1400, missing, events);
        Check(events.empty(), "under the threshold nothing is logged");
        missing[0].reason = Reason::Dressing;
        missing[0].distanceM = 15.0f;
        watch.Update(1600, missing, events);
        Check(events.size() == 1 && events[0].begin && events[0].id == 7, "past the threshold an episode begins");
        Check(events[0].reason == Reason::CreateWait, "the episode keeps its first reason");
        Check(events[0].missingMs == 600 && events[0].distanceM == 15.0f, "missing time and nearest distance");
        events.clear();
        watch.Update(1700, {}, events);
        Check(events.size() == 1 && !events[0].begin && events[0].missingMs == 700 && !events[0].suspended,
            "the entity showing ends the episode");
        Check(events[0].reason == Reason::Dressing, "the end names the last reason");
        Check(watch.Stats().episodes == 1 && watch.Stats().entities == 1 && watch.Stats().worstMs == 700,
            "the summary counts the episode");
        Check(watch.Stats().perReason[static_cast<int>(Reason::CreateWait)] == 1, "per reason, by the first one");

        // A short gap never becomes an episode.
        events.clear();
        watch.Update(2000, {{8, Reason::ModelLoading, 5.0f}}, events);
        watch.Update(2100, {}, events);
        Check(events.empty() && watch.Stats().episodes == 1, "a 100 ms gap is not an episode");

        // A suspend (pause, jump) ends the open ones and restarts the timers.
        watch.Update(3000, {{9, Reason::NoPuppet, 5.0f}}, events);
        watch.Update(3600, {{9, Reason::NoPuppet, 5.0f}}, events);
        Check(events.size() == 1 && events.back().begin, "second episode");
        watch.Suspend(3700, events);
        Check(events.size() == 2 && events.back().suspended && events.back().missingMs == 700, "suspend closes it");
        events.clear();
        watch.Update(3800, {{9, Reason::NoPuppet, 5.0f}}, events);
        watch.Update(4200, {{9, Reason::NoPuppet, 5.0f}}, events);
        Check(events.empty(), "after a suspend the threshold counts again from zero");
        Check(watch.Stats().entities == 2 && watch.Stats().episodes == 2, "distinct entities");
        watch.Update(4400, {{9, Reason::NoPuppet, 5.0f}}, events);
        Check(watch.Stats().entities == 2 && watch.Stats().episodes == 3, "the same entity again is one entity");
        events.clear();
        watch.Finish(5000, events);
        Check(events.size() == 1 && watch.Open() == 0, "the end of the playback closes what is open");
        Check(std::string(ReasonText(Reason::Invisible)) == "invisible", "reason names");

        Check(IsPopIn(48800, 48815, 20, 11.0f), "shown 15 ms after its entry, 11 m away: a pop in");
        Check(!IsPopIn(48800, 60000, 20, 11.0f), "shown long after its entry (a jump onto it): not a pop in");
        Check(!IsPopIn(500, 520, 20, 11.0f), "the opening census is not a pop in");
        Check(!IsPopIn(48800, 48815, 20, 80.0f), "far away it is not");
    }
}

int RunPuppetWatchTests()
{
    TestNearbyWalk();
    TestPuppetWatch();
    if (g_watchFailures == 0)
        std::puts("Puppet watch tests passed.");
    return g_watchFailures;
}
