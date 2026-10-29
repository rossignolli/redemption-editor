#pragma once

// Pose segment selection (pure data, unit tested in ReplayCoreTests).
//
// A recorded entity whose skeleton changes continues in pose segments, each
// with its own pose record and bone count (entity_events.h). Playback asks
// here which record plays at the playhead, which record has the bones the
// puppet's skeleton currently has, which neighbouring record to keep ready
// while the engine rebuilds that skeleton, and which layers a dropped prop
// wears. Whether a record has samples yet comes from the recorder through
// `RecordSampled`, so this file never touches the capture side.

#include "entity_events.h"
#include "entity_lifecycle.h"

#include <cstdint>

namespace replay::entity
{
    // Whether a pose record has at least one sample.
    using RecordSampled = bool (*)(std::uint32_t recordId);

    // The segment that plays at timeMs: the latest one started by then whose
    // record has sampled. A segment starts when the skeleton change is seen
    // and its first sample comes up to a capture interval later; until then
    // the previous record holds its last pose, so the switch never leaves a
    // frame without a pose. Null when the create record still applies.
    const PoseSegment* SegmentAt(
        const PoseSegmentTrack& track, ReplayId id, std::uint32_t timeMs, RecordSampled sampled);

    // The record a puppet plays at the playhead: the segment's (SegmentAt) or
    // the create record. Every kind with a pose track follows it, vehicles
    // included: a wagon registered while the engine still built it has a
    // create record without samples and its whole pose in a segment from the
    // clip start.
    std::uint32_t PlaybackRecordAt(
        const PoseSegmentTrack& track, ReplayId id, std::uint32_t timeMs, RecordSampled sampled);

    // Whether another segment of the entity follows this record: past the
    // record's last sample the puppet then holds that pose instead of
    // hiding, because the entity is still alive in the next segment.
    bool RecordHasSuccessor(const PoseSegmentTrack& track, ReplayId id, std::uint32_t recordId);

    // Record of the entity whose bones match `bones`, nearest in time to the
    // playhead: the create record (when `create` is given) or a segment.
    // 0 when none has them or none has sampled.
    std::uint32_t RecordForBones(
        const PoseSegmentTrack& track, const CreatePacket* create, ReplayId id,
        std::uint32_t timeMs, std::uint32_t bones, RecordSampled sampled);

    // Record of the entity with a bone count other than `bones` whose span
    // (from its start to the next segment's) is within this window of the
    // playhead: the pose to keep ready while the engine rebuilds the
    // skeleton across that boundary. 0 when none.
    constexpr std::uint32_t kNeighbourWindowMs = 1000;
    std::uint32_t NeighbourRecord(
        const PoseSegmentTrack& track, const CreatePacket* create, ReplayId id,
        std::uint32_t timeMs, std::uint32_t bones, RecordSampled sampled,
        std::uint32_t& neighbourBones);

    // The entity's next segment when it starts at most `windowMs` after the
    // playhead and has `bones`: the engine already rebuilt the puppet's
    // skeleton for it (a knocked hat is created about 16 ms before the
    // recorded segment). Taking the create or earlier segment there would
    // apply its layers again and the next frame remove them: three skeleton
    // rebuilds in two frames, a visible pop. Null when none.
    constexpr std::uint32_t kEarlySegmentWindowMs = 250;
    const PoseSegment* EarlySegmentForBones(
        const PoseSegmentTrack& track, ReplayId id, std::uint32_t timeMs, std::uint32_t bones,
        RecordSampled sampled, std::uint32_t windowMs = kEarlySegmentWindowMs);

    // The layers a prop wears: its own recorded layers ("object"), or else
    // the ones its owner lost at the pose segment nearest to the prop's
    // create ("owner_diff"; the hat layer leaves the ped when the hat object
    // appears), or "none". `ownerCreate` is the owner's create packet (may
    // be null), the segment before the first change.
    constexpr std::uint32_t kPropLayerWindowMs = 1000;
    const char* PropLayers(
        const PoseSegmentTrack& track, const CreatePacket* ownerCreate,
        const CreatePacket& prop, Appearance& out);
}
