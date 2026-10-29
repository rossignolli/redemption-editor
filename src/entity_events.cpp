#include "entity_events.h"

#include <algorithm>

namespace replay::entity
{
    bool ForwardEventCursor::Advance(
        std::uint32_t timeMs, bool normalPlay, std::uint32_t& fromMs, std::uint32_t& toMs)
    {
        const std::uint32_t last = m_lastMs;
        m_lastMs = timeMs;
        if (!normalPlay || timeMs <= last || timeMs - last > kMaxStepMs)
            return false;
        fromMs = last;
        toMs = timeMs;
        return true;
    }

    const PoseSegment* PoseSegmentTrack::At(ReplayId entity, std::uint32_t timeMs) const
    {
        const PoseSegment* found = nullptr;
        for (const PoseSegment& segment : m_segments)
        {
            if (segment.entity == entity && segment.startMs <= timeMs)
                found = &segment;
        }
        return found;
    }

    ReplayId PoseSegmentTrack::OwnerOf(std::uint32_t recordId) const
    {
        for (const PoseSegment& segment : m_segments)
        {
            if (segment.recordId == recordId)
                return segment.entity;
        }
        return {};
    }
}
