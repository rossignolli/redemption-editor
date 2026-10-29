#pragma once

// PoseTrack v2: which entities the recording holds.
//
// A replay editor usually records every entity in the world; here the
// number of skeletons captured at once is bounded, so a scan of the ped pool feeds a
// distance rule with hysteresis. Pinned entities (the player and whatever it
// is attached to) always have a place and may displace the farthest unpinned
// entity; the rest enter nearest first inside enterRadius and leave beyond
// exitRadius, so an entity at the border does not flap in and out.
// When full, a clearly nearer newcomer also takes the place of
// the farthest unpinned entity that no recorded relation holds (see
// SelectionConfig::evictMargin).
//
// Pure policy: no engine or native access, unit tested in ReplayCoreTests.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::entity
{
    struct Candidate
    {
        std::int32_t handle = 0;
        float distance = 0.0f; // metres from the player
        bool pinned = false;
        // Held by a recorded relation (a draft harness, a vehicle
        // seat, a rope): never evicted for a nearer newcomer.
        bool locked = false;
    };

    // Budget: the bound is the capture cost, not memory. Measured with 40
    // peds recorded: about 13 us per ped per 60 fps frame in the pose detour
    // plus about 6 us on the script thread, 0.79 ms per frame in all, so
    // 40 peds and 8 vehicles stay under 1 ms per frame. The radius only keeps the far edge of the streamed world out;
    // capacity picks the nearest first.
    struct SelectionConfig
    {
        float enterRadius = 150.0f;
        float exitRadius = 175.0f;
        std::uint32_t capacity = 8;
        // Priority when full (otherwise a ring full of ambient peds never
        // takes two brawlers spawned 8 m away). An unpinned newcomer evicts the
        // farthest unpinned, unlocked recorded entity when it is nearer by
        // more than evictMargin (hysteresis: two peds at about the same
        // distance do not swap back and forth), at most maxEvictions per
        // scan (each entry costs a skeleton resolve and an appearance read).
        float evictMargin = 10.0f;
        std::uint32_t maxEvictions = 4;
    };

    // What the selection did beyond enter and leave (evidence).
    struct SelectionReport
    {
        std::vector<std::int32_t> evicted;     // retired for a nearer newcomer (also in `retire`)
        std::vector<std::int32_t> skippedFull; // inside enterRadius, no place and nothing to evict
    };

    // The distance the radius rules compare. An entity falling through the
    // map (e.g. a camp horse left without collision) would leave the
    // recording when the fall alone carried the 3D distance past the exit
    // radius and come back at the same spot as a new replay entity, a
    // pop-in every few seconds. The radius only stands for the far edge of the streamed world, which is
    // horizontal; the vertical difference counts only beyond a slack, so an
    // entity that falls (or a ped on a cliff above) keeps its place and its
    // replay id while it is near on the map.
    constexpr float kSelectionVerticalSlackM = 200.0f;

    inline float SelectionDistance(float dx, float dy, float dz, float verticalSlackM = kSelectionVerticalSlackM)
    {
        const float vertical = std::fabs(dz);
        const float excess = vertical > verticalSlackM ? vertical - verticalSlackM : 0.0f;
        return std::sqrt(dx * dx + dy * dy + excess * excess);
    }

    // Evidence for the case the distance rule does not
    // cover. Handles the selection retired (out of range or evicted) are
    // kept for a while; the same handle registered again inside the window
    // is a re-entry (one engine entity recorded as two replay entities).
    // Pure, unit tested; the capture logs POSE_V2_REENTRY and a count.
    struct RecentRetirements
    {
        struct Entry
        {
            std::int32_t handle = 0;
            std::uint32_t timeMs = 0;
            float z = 0.0f;
            bool evicted = false;
        };
        static constexpr std::size_t kCapacity = 64;
        static constexpr std::uint32_t kWindowMs = 15000;
        std::vector<Entry> entries;

        void Clear() { entries.clear(); }

        void Note(std::int32_t handle, std::uint32_t timeMs, float z, bool evicted)
        {
            for (Entry& entry : entries)
            {
                if (entry.handle == handle)
                {
                    entry = {handle, timeMs, z, evicted};
                    return;
                }
            }
            if (entries.size() >= kCapacity)
                entries.erase(entries.begin()); // the oldest
            entries.push_back({handle, timeMs, z, evicted});
        }

        // The retirement of `handle` inside the window before `nowMs`, removed
        // from the list (one re-entry per retirement).
        bool Take(std::int32_t handle, std::uint32_t nowMs, Entry& out)
        {
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                const Entry& entry = entries[index];
                if (entry.handle != handle)
                    continue;
                const bool inside = nowMs >= entry.timeMs && nowMs - entry.timeMs <= kWindowMs;
                out = entry;
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
                return inside;
            }
            return false;
        }
    };

    // When `recorded` holds more than the capacity (the ped class
    // gave a place to a posed object, object_pose.h), the farthest unpinned,
    // unlocked entities are retired (and reported as evicted) until it fits.
    // `candidates`: this scan's entities (recorded ones included). `recorded`:
    // handles recorded now. Recorded handles missing from `candidates` are
    // left alone (existence is checked by the caller every frame). Outputs
    // are ordered: `retire` farthest first, `add` in priority order.
    void SelectEntities(
        const std::vector<Candidate>& candidates,
        const std::vector<std::int32_t>& recorded,
        const SelectionConfig& config,
        std::vector<std::int32_t>& retire,
        std::vector<std::int32_t>& add,
        SelectionReport* report = nullptr);
}
