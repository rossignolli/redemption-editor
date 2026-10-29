#pragma once

// Finding the entities the recording may hold without the ScriptHook pool
// listing. worldGetAllPeds and worldGetAllVehicles return 0 while the script
// handle pool has 256 free slots or fewer (see replay_objects.cpp), which is the
// normal state in a town, so NPCs would enter the recording only in rare scans.
//
// The engine still answers the per-ped nearby lists (GET_PED_NEARBY_PEDS and
// GET_PED_NEARBY_VEHICLES). NearbyWalk spreads through them from seeds (the
// player, its mount, the recorded peds and their dependents): each ped found is
// itself asked for its neighbours, within a budget of calls per scan, so a
// street full of people is reached through the people between.
// Pure: the engine query is a callback.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace replay::entity
{
    struct NearbyWalkStats
    {
        std::uint32_t calls = 0;  // nearby list queries made
        std::uint32_t found = 0;  // handles added (not in the list before)
        std::uint32_t seeds = 0;  // seeds offered (deduplicated)
        bool budgetSpent = false; // stopped with seeds left
    };

    // `query(seed, out, capacity)` fills `out` with the seed's neighbours
    // and returns how many. `handles` holds `count` handles already known
    // (the listing, may be 0) and receives the new ones, up to `capacity`.
    // Seeds are asked in order, then every handle found, breadth first.
    template <class Query>
    NearbyWalkStats NearbyWalk(const std::vector<std::int32_t>& seeds, Query&& query, std::int32_t* handles,
        int& count, int capacity, std::uint32_t maxCalls)
    {
        NearbyWalkStats stats;
        std::vector<std::int32_t> queue;
        queue.reserve(seeds.size() + 64);
        const auto known = [&](std::int32_t handle) {
            return std::find(handles, handles + count, handle) != handles + count;
        };
        for (const std::int32_t seed : seeds)
        {
            if (seed && std::find(queue.begin(), queue.end(), seed) == queue.end())
                queue.push_back(seed);
        }
        stats.seeds = static_cast<std::uint32_t>(queue.size());
        constexpr int kSlots = 64;
        std::int32_t list[kSlots]{};
        for (std::size_t next = 0; next < queue.size(); ++next)
        {
            if (stats.calls >= maxCalls)
            {
                stats.budgetSpent = true;
                break;
            }
            ++stats.calls;
            const int got = (std::min)(query(queue[next], list, kSlots), kSlots);
            for (int n = 0; n < got; ++n)
            {
                const std::int32_t handle = list[n];
                if (!handle)
                    continue;
                if (!known(handle) && count < capacity)
                {
                    handles[count++] = handle;
                    ++stats.found;
                }
                if (std::find(queue.begin(), queue.end(), handle) == queue.end())
                    queue.push_back(handle);
            }
        }
        return stats;
    }

    // Adds `extra` handles (dependents, recorded entities) missing from
    // `handles`; returns how many were added.
    inline std::uint32_t AddMissing(const std::vector<std::int32_t>& extra, std::int32_t* handles, int& count, int capacity)
    {
        std::uint32_t added = 0;
        for (const std::int32_t handle : extra)
        {
            if (!handle || count >= capacity || std::find(handles, handles + count, handle) != handles + count)
                continue;
            handles[count++] = handle;
            ++added;
        }
        return added;
    }

    // A recorded entity that entered the recording late and close to the
    // camera pops into the replay (the discovery missed it earlier). Evidence
    // only: a ped the game spawns there on purpose looks the same.

    struct LateEntryRule
    {
        std::uint32_t graceMs = 1000;   // the opening census
        float nearMeters = 40.0f;
    };

    inline bool IsLateEntry(std::uint32_t entryMs, float cameraMeters, const LateEntryRule& rule = {})
    {
        return entryMs > rule.graceMs && cameraMeters >= 0.0f && cameraMeters < rule.nearMeters;
    }
}
