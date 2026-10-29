#pragma once

// Pure helpers of the playback tick (replay_playback.cpp), here so the
// tests run the same code (tests/playback_perf_tests.cpp).
//
// SampleReuse: a puppet's pose sample is used again while the playhead
// does not move (pause, the start hold, a playhead held at the clip end,
// slow motion under 1 ms a tick); sampling and publishing again would put
// the same bits in the override slot. PoseTrack::Sample at the time of its
// previous call gives that call's bits again and leaves the track as that
// call left it (proven over recorded clips by the tests), and the slot
// keeps the pose published from it. The key is the record sampled, the
// playhead and the slot; arming or releasing the puppet's override drops
// the reuse (Invalidate), and a tick that published more than the pose (a
// vehicle's late and script-thread writes, an alternate pose kept for a
// skeleton rebuild) or whose publish was refused never leaves one behind.
//
// HandleSet: the world clear (HidePoseV2World, every 250 ms) asks, for
// every listed ped, vehicle and object, whether it is a puppet and
// whether it was hidden already: a sorted list answers the membership
// without a linear search.

#include "pose_math.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace replay::playback
{
    struct SampleReuse
    {
        bool valid = false;
        std::uint32_t record = 0;
        std::uint32_t timeMs = 0;
        int slot = -1;
        // What the tick took from the sample: whether it sampled, the world
        // transform placed, and the root bone's object translation (the jerk
        // detector's observation).
        bool sampled = false;
        pose::WorldTransform world{};
        float root[3] = {};
        bool hasRoot = false;

        // The last sample stands for a new one of the same record at the
        // same playhead, published to the same slot.
        bool Matches(std::uint32_t wantRecord, std::uint32_t wantTimeMs, int wantSlot) const
        {
            return valid && record == wantRecord && timeMs == wantTimeMs && slot == wantSlot;
        }

        // A new sample: the key and what the tick takes from it; valid only
        // once the tick has done with it what the reuse stands for (Keep).
        void Begin(std::uint32_t sampledRecord, std::uint32_t sampledTimeMs, int sampledSlot, bool ok,
            const pose::WorldTransform& placed, const float* rootTranslation)
        {
            valid = false;
            record = sampledRecord;
            timeMs = sampledTimeMs;
            slot = sampledSlot;
            sampled = ok;
            world = placed;
            hasRoot = rootTranslation != nullptr;
            for (int axis = 0; axis < 3; ++axis)
                root[axis] = hasRoot ? rootTranslation[axis] : 0.0f;
        }

        void Keep() { valid = true; }
        void Invalidate() { valid = false; }
    };

    // Sorted entity handles.
    class HandleSet
    {
    public:
        void Clear() { m_handles.clear(); }
        void Add(std::int32_t handle)
        {
            const auto at = std::lower_bound(m_handles.begin(), m_handles.end(), handle);
            if (at == m_handles.end() || *at != handle)
                m_handles.insert(at, handle);
        }
        // Many at once (one sort); duplicates are fine.
        template <typename Range, typename HandleOf>
        void Assign(const Range& range, HandleOf&& handleOf)
        {
            m_handles.clear();
            for (const auto& item : range)
                m_handles.push_back(static_cast<std::int32_t>(handleOf(item)));
            std::sort(m_handles.begin(), m_handles.end());
            m_handles.erase(std::unique(m_handles.begin(), m_handles.end()), m_handles.end());
        }
        bool Contains(std::int32_t handle) const
        {
            return std::binary_search(m_handles.begin(), m_handles.end(), handle);
        }
        std::size_t Size() const { return m_handles.size(); }

    private:
        std::vector<std::int32_t> m_handles;
    };
}
