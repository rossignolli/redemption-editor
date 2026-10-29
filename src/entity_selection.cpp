#include "entity_selection.h"

#include <algorithm>

namespace replay::entity
{
    namespace
    {
        bool Contains(const std::vector<std::int32_t>& handles, std::int32_t handle)
        {
            return std::find(handles.begin(), handles.end(), handle) != handles.end();
        }

        // Pinned first, then nearest.
        bool Before(const Candidate& a, const Candidate& b)
        {
            if (a.pinned != b.pinned)
                return a.pinned;
            return a.distance < b.distance;
        }
    }

    void SelectEntities(
        const std::vector<Candidate>& candidates,
        const std::vector<std::int32_t>& recorded,
        const SelectionConfig& config,
        std::vector<std::int32_t>& retire,
        std::vector<std::int32_t>& add,
        SelectionReport* report)
    {
        retire.clear();
        add.clear();
        if (report)
        {
            report->evicted.clear();
            report->skippedFull.clear();
        }

        // Recorded entities that stay, and those out of range.
        std::vector<Candidate> kept;
        std::vector<Candidate> leaving;
        std::uint32_t untracked = 0; // recorded but not in this scan
        for (const std::int32_t handle : recorded)
        {
            const auto it = std::find_if(candidates.begin(), candidates.end(),
                [&](const Candidate& candidate) { return candidate.handle == handle; });
            if (it == candidates.end())
            {
                ++untracked;
                continue;
            }
            if (!it->pinned && it->distance > config.exitRadius)
                leaving.push_back(*it);
            else
                kept.push_back(*it);
        }

        std::vector<Candidate> entering;
        for (const Candidate& candidate : candidates)
        {
            if (Contains(recorded, candidate.handle))
                continue;
            if (candidate.pinned || candidate.distance <= config.enterRadius)
                entering.push_back(candidate);
        }
        std::sort(entering.begin(), entering.end(), Before);

        // When full, a pinned newcomer displaces the farthest unpinned
        // entity; an unpinned one does too when it is nearer by more than
        // the margin (the victim is never pinned or locked).
        std::sort(kept.begin(), kept.end(), Before);
        std::uint32_t used = static_cast<std::uint32_t>(kept.size()) + untracked;
        std::uint32_t evictions = 0;
        // The capacity can drop below the recorded count (a posed object took
        // a ped place, object_pose.h ReservedPlaces); the farthest unpinned,
        // unlocked entities leave until it fits, at most maxEvictions per scan.
        while (used > config.capacity && evictions < config.maxEvictions)
        {
            std::size_t victim = kept.size();
            for (std::size_t index = kept.size(); index-- > 0;)
            {
                if (kept[index].pinned)
                    break;
                if (!kept[index].locked)
                {
                    victim = index;
                    break;
                }
            }
            if (victim >= kept.size())
                break;
            ++evictions;
            --used;
            leaving.push_back(kept[victim]);
            if (report)
                report->evicted.push_back(kept[victim].handle);
            kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(victim));
        }
        for (const Candidate& candidate : entering)
        {
            if (used < config.capacity)
            {
                add.push_back(candidate.handle);
                ++used;
                continue;
            }
            // The farthest evictable entity (kept is pinned first, then nearest).
            std::size_t victim = kept.size();
            for (std::size_t index = kept.size(); index-- > 0;)
            {
                if (kept[index].pinned)
                    break;
                if (!kept[index].locked)
                {
                    victim = index;
                    break;
                }
            }
            const bool evict = victim < kept.size() &&
                (candidate.pinned ||
                    (evictions < config.maxEvictions && candidate.distance + config.evictMargin < kept[victim].distance));
            if (!evict)
            {
                if (report && !candidate.pinned)
                    report->skippedFull.push_back(candidate.handle);
                continue;
            }
            if (!candidate.pinned)
                ++evictions;
            leaving.push_back(kept[victim]);
            if (report)
                report->evicted.push_back(kept[victim].handle);
            kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(victim));
            add.push_back(candidate.handle);
        }

        std::sort(leaving.begin(), leaving.end(),
            [](const Candidate& a, const Candidate& b) { return a.distance > b.distance; });
        for (const Candidate& candidate : leaving)
            retire.push_back(candidate.handle);
    }
}
