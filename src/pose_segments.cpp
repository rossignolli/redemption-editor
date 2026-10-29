#include "pose_segments.h"

#include <algorithm>
#include <vector>

namespace replay::entity
{
    namespace
    {
        struct Span
        {
            std::uint32_t recordId;
            std::uint32_t startMs;
            std::uint32_t bones;
        };

        // The entity's records in start order: the create record, then its
        // segments.
        std::vector<Span> SpansOf(
            const PoseSegmentTrack& track, const CreatePacket* create, ReplayId id)
        {
            std::vector<Span> spans;
            if (create)
                spans.push_back({id.Packed(), create->timeMs, create->boneCount});
            for (const PoseSegment& segment : track.Segments())
            {
                if (segment.entity == id)
                    spans.push_back({segment.recordId, segment.startMs, segment.boneCount});
            }
            std::stable_sort(spans.begin(), spans.end(),
                [](const Span& a, const Span& b) { return a.startMs < b.startMs; });
            return spans;
        }

        bool SameAsset(const MetaPedAsset& a, const MetaPedAsset& b)
        {
            return a.drawable == b.drawable && a.albedo == b.albedo && a.normal == b.normal &&
                a.material == b.material && a.palette == b.palette && a.tint0 == b.tint0 &&
                a.tint1 == b.tint1 && a.tint2 == b.tint2;
        }
    }

    const PoseSegment* SegmentAt(
        const PoseSegmentTrack& track, ReplayId id, std::uint32_t timeMs, RecordSampled sampled)
    {
        const PoseSegment* segment = track.At(id, timeMs);
        while (segment)
        {
            if (sampled && sampled(segment->recordId))
                return segment;
            if (segment->startMs == 0)
                return nullptr;
            segment = track.At(id, segment->startMs - 1);
        }
        return nullptr;
    }

    std::uint32_t PlaybackRecordAt(
        const PoseSegmentTrack& track, ReplayId id, std::uint32_t timeMs, RecordSampled sampled)
    {
        const PoseSegment* segment = SegmentAt(track, id, timeMs, sampled);
        return segment ? segment->recordId : id.Packed();
    }

    bool RecordHasSuccessor(const PoseSegmentTrack& track, ReplayId id, std::uint32_t recordId)
    {
        bool past = recordId == id.Packed();
        for (const PoseSegment& segment : track.Segments())
        {
            if (segment.entity != id)
                continue;
            if (past)
                return true;
            if (segment.recordId == recordId)
                past = true;
        }
        return false;
    }

    std::uint32_t RecordForBones(
        const PoseSegmentTrack& track, const CreatePacket* create, ReplayId id,
        std::uint32_t timeMs, std::uint32_t bones, RecordSampled sampled)
    {
        std::uint32_t best = 0;
        std::uint32_t bestDistance = 0xFFFFFFFFu;
        for (const Span& span : SpansOf(track, create, id))
        {
            if (span.bones != bones || !sampled || !sampled(span.recordId))
                continue;
            const std::uint32_t distance =
                span.startMs > timeMs ? span.startMs - timeMs : timeMs - span.startMs;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = span.recordId;
            }
        }
        return best;
    }

    std::uint32_t NeighbourRecord(
        const PoseSegmentTrack& track, const CreatePacket* create, ReplayId id,
        std::uint32_t timeMs, std::uint32_t bones, RecordSampled sampled,
        std::uint32_t& neighbourBones)
    {
        const std::vector<Span> spans = SpansOf(track, create, id);
        std::uint32_t best = 0;
        std::uint32_t bestDistance = kNeighbourWindowMs + 1;
        neighbourBones = 0;
        for (std::size_t index = 0; index < spans.size(); ++index)
        {
            const Span& span = spans[index];
            if (span.bones == bones || !sampled || !sampled(span.recordId))
                continue;
            const std::uint32_t endMs =
                index + 1 < spans.size() ? spans[index + 1].startMs : 0xFFFFFFFFu;
            const std::uint32_t distance = timeMs < span.startMs ? span.startMs - timeMs
                : timeMs >= endMs ? timeMs - endMs + 1
                : 0;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = span.recordId;
                neighbourBones = span.bones;
            }
        }
        return best;
    }

    const PoseSegment* EarlySegmentForBones(
        const PoseSegmentTrack& track, ReplayId id, std::uint32_t timeMs, std::uint32_t bones,
        RecordSampled sampled, std::uint32_t windowMs)
    {
        const PoseSegment* next = nullptr;
        for (const PoseSegment& segment : track.Segments())
        {
            if (segment.entity != id || segment.startMs <= timeMs)
                continue;
            if (!next || segment.startMs < next->startMs)
                next = &segment;
        }
        if (!next || next->startMs - timeMs > windowMs || next->boneCount != bones || !sampled ||
            !sampled(next->recordId))
            return nullptr;
        return next;
    }

    const char* PropLayers(
        const PoseSegmentTrack& track, const CreatePacket* ownerCreate,
        const CreatePacket& prop, Appearance& out)
    {
        out = {};
        if (!prop.appearance.assets.empty())
        {
            out = prop.appearance;
            return "object";
        }
        const PoseSegment* change = nullptr;
        std::uint32_t best = kPropLayerWindowMs + 1;
        for (const PoseSegment& segment : track.Segments())
        {
            if (segment.entity != prop.owner)
                continue;
            const std::uint32_t distance = segment.startMs > prop.timeMs
                ? segment.startMs - prop.timeMs
                : prop.timeMs - segment.startMs;
            if (distance < best)
            {
                best = distance;
                change = &segment;
            }
        }
        if (!change)
            return "none";
        const Appearance* before = nullptr;
        for (const PoseSegment& segment : track.Segments())
        {
            if (segment.entity == prop.owner && segment.startMs < change->startMs)
                before = &segment.appearance;
        }
        if (!before)
        {
            if (!ownerCreate)
                return "none";
            before = &ownerCreate->appearance;
        }
        for (const MetaPedAsset& layer : before->assets)
        {
            const bool kept = std::any_of(change->appearance.assets.begin(),
                change->appearance.assets.end(),
                [&](const MetaPedAsset& other) { return SameAsset(layer, other); });
            if (!kept)
                out.assets.push_back(layer);
        }
        return out.assets.empty() ? "none" : "owner_diff";
    }
}
