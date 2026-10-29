#include "pose_segments.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::entity;

    int g_segmentFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_segmentFailures;
    }

    // Records the recorder has sampled (RecordSampled is a plain function).
    std::vector<std::uint32_t> g_sampled;
    bool Sampled(std::uint32_t recordId)
    {
        return std::find(g_sampled.begin(), g_sampled.end(), recordId) != g_sampled.end();
    }

    const ReplayId kPed{3, 1};
    const ReplayId kOther{4, 1};

    MetaPedAsset Asset(std::uint32_t drawable)
    {
        MetaPedAsset asset;
        asset.drawable = drawable;
        asset.albedo = drawable + 100;
        return asset;
    }

    CreatePacket Create(ReplayId id, std::uint32_t timeMs, std::uint32_t bones)
    {
        CreatePacket packet;
        packet.id = id;
        packet.timeMs = timeMs;
        packet.boneCount = bones;
        return packet;
    }

    PoseSegment Segment(PoseSegmentTrack& track, ReplayId id, std::uint32_t startMs,
        std::uint32_t bones, std::vector<std::uint32_t> drawables = {})
    {
        PoseSegment segment;
        segment.entity = id;
        segment.startMs = startMs;
        segment.recordId = track.NextRecordId();
        segment.boneCount = bones;
        for (std::uint32_t drawable : drawables)
            segment.appearance.assets.push_back(Asset(drawable));
        track.Add(segment);
        return segment;
    }

    void TestSegmentAtWaitsForSamples()
    {
        PoseSegmentTrack track;
        track.Begin();
        g_sampled.clear();
        Check(SegmentAt(track, kPed, 500, Sampled) == nullptr, "no segments: create applies");

        const PoseSegment first = Segment(track, kPed, 1000, 693);
        Segment(track, kOther, 1000, 693); // another entity never counts
        Check(SegmentAt(track, kPed, 999, Sampled) == nullptr, "before the segment start");
        Check(SegmentAt(track, kPed, 1000, Sampled) == nullptr,
            "segment started but its record has no sample yet: previous record holds");
        g_sampled.push_back(first.recordId);
        const PoseSegment* at = SegmentAt(track, kPed, 1000, Sampled);
        Check(at && at->recordId == first.recordId, "segment plays from its start once sampled");

        const PoseSegment second = Segment(track, kPed, 2000, 710);
        at = SegmentAt(track, kPed, 2500, Sampled);
        Check(at && at->recordId == first.recordId, "unsampled later segment falls back to the previous");
        g_sampled.push_back(second.recordId);
        at = SegmentAt(track, kPed, 2500, Sampled);
        Check(at && at->recordId == second.recordId, "later segment plays once sampled");
        Check(SegmentAt(track, kPed, 2500, nullptr) == nullptr, "no sampler: nothing has samples");
    }

    void TestRecordHasSuccessor()
    {
        PoseSegmentTrack track;
        track.Begin();
        Check(!RecordHasSuccessor(track, kPed, kPed.Packed()), "create alone has no successor");
        const PoseSegment first = Segment(track, kPed, 1000, 693);
        Check(RecordHasSuccessor(track, kPed, kPed.Packed()), "create followed by a segment");
        Check(!RecordHasSuccessor(track, kPed, first.recordId), "last segment has no successor");
        const PoseSegment second = Segment(track, kPed, 2000, 710);
        Check(RecordHasSuccessor(track, kPed, first.recordId), "middle segment has a successor");
        Check(!RecordHasSuccessor(track, kPed, second.recordId), "new last segment has none");
        Check(!RecordHasSuccessor(track, kOther, first.recordId), "another entity's record: none");
    }

    // A ped spawned just before the entity scan is registered while the
    // engine still builds it: its create record keeps a few captures with the
    // old bone count and never gets a track, and its pose starts in the first
    // segment ~150 ms later. At the create time no segment applies and the
    // create record has no sample; the successor tells playback to wait for
    // the segment instead of failing (no_track).
    void TestFreshSpawnWaitsForFirstSegment()
    {
        PoseSegmentTrack track;
        track.Begin();
        g_sampled.clear(); // the create record (66 bones) never sampled
        const CreatePacket create = Create(kPed, 197, 66);
        const PoseSegment built = Segment(track, kPed, 320, 959);
        g_sampled.push_back(built.recordId);
        Check(SegmentAt(track, kPed, create.timeMs, Sampled) == nullptr,
            "fresh spawn: at the create time the create record applies");
        Check(!Sampled(kPed.Packed()), "fresh spawn: the create record has no sample");
        Check(RecordHasSuccessor(track, kPed, kPed.Packed()),
            "fresh spawn: a later segment exists, so the create waits (no no_track)");
        const PoseSegment* at = SegmentAt(track, kPed, 320, Sampled);
        Check(at && at->recordId == built.recordId, "fresh spawn: the puppet is created from the segment");
        Check(RecordForBones(track, &create, kPed, 330, 959, Sampled) == built.recordId,
            "fresh spawn: the built skeleton arms on the segment record");
        Check(RecordForBones(track, &create, kPed, 330, 66, Sampled) == 0,
            "fresh spawn: the unsampled create record is never armed");
        // A ped whose create record has no track and no later segment still
        // has nothing to play (Failed, no_track).
        Check(!RecordHasSuccessor(track, kOther, kOther.Packed()), "no successor: nothing to wait for");
    }

    void TestRecordForBones()
    {
        PoseSegmentTrack track;
        track.Begin();
        g_sampled = {kPed.Packed()};
        const CreatePacket create = Create(kPed, 0, 710);
        const PoseSegment hatOff = Segment(track, kPed, 1000, 693);
        const PoseSegment hatBack = Segment(track, kPed, 3000, 710);
        g_sampled.push_back(hatOff.recordId);

        Check(RecordForBones(track, &create, kPed, 2900, 710, Sampled) == kPed.Packed(),
            "unsampled nearer record loses to the sampled create record");
        g_sampled.push_back(hatBack.recordId);
        Check(RecordForBones(track, &create, kPed, 2900, 710, Sampled) == hatBack.recordId,
            "nearest sampled record with the bones");
        Check(RecordForBones(track, &create, kPed, 1200, 710, Sampled) == kPed.Packed(),
            "create record when nearer");
        Check(RecordForBones(track, &create, kPed, 5000, 693, Sampled) == hatOff.recordId,
            "only record with that bone count");
        Check(RecordForBones(track, &create, kPed, 1000, 500, Sampled) == 0, "no record has the bones");
        Check(RecordForBones(track, nullptr, kPed, 100, 710, Sampled) == hatBack.recordId,
            "without a create packet only segments count");
    }

    // A ped created with 969 bones, hat knocked at 53341 (the hat's create),
    // the recorded segment with 952 bones starts at 53357.
    void TestEarlySegmentForBones()
    {
        PoseSegmentTrack track;
        track.Begin();
        const PoseSegment hatOff = Segment(track, kPed, 53357, 952);
        const PoseSegment later = Segment(track, kPed, 60000, 969);
        g_sampled = {kPed.Packed(), hatOff.recordId, later.recordId};
        const PoseSegment* early = EarlySegmentForBones(track, kPed, 53342, 952, Sampled);
        Check(early && early->recordId == hatOff.recordId, "knocked a frame early: the next segment with the new bones");
        Check(!EarlySegmentForBones(track, kPed, 53342, 969, Sampled), "the skeleton still has the old bones: nothing early");
        Check(!EarlySegmentForBones(track, kPed, 53000, 952, Sampled), "357 ms before the segment: outside the window");
        Check(!EarlySegmentForBones(track, kPed, 53357, 952, Sampled), "at the segment start it is not early (SegmentAt has it)");
        Check(!EarlySegmentForBones(track, kOther, 53342, 952, Sampled), "another entity's segment never counts");
        g_sampled = {kPed.Packed(), later.recordId};
        Check(!EarlySegmentForBones(track, kPed, 53342, 952, Sampled), "an unsampled segment is not taken");
        g_sampled.clear();
    }

    void TestNeighbourRecord()
    {
        PoseSegmentTrack track;
        track.Begin();
        const CreatePacket create = Create(kPed, 0, 710);
        const PoseSegment hatOff = Segment(track, kPed, 1000, 693);
        g_sampled = {kPed.Packed(), hatOff.recordId};
        std::uint32_t bones = 0;

        Check(NeighbourRecord(track, &create, kPed, 900, 710, Sampled, bones) == hatOff.recordId &&
                bones == 693,
            "segment ahead within the window is the neighbour of the create record");
        Check(NeighbourRecord(track, &create, kPed, 1500, 693, Sampled, bones) == kPed.Packed() &&
                bones == 710,
            "create span just behind is the neighbour of the segment");
        Check(NeighbourRecord(track, &create, kPed, 2100, 693, Sampled, bones) == 0 && bones == 0,
            "create span ended more than the window ago: none");
        Check(NeighbourRecord(track, &create, kPed, 1500, 710, Sampled, bones) == hatOff.recordId,
            "inside the neighbour's span the distance is zero");

        const PoseSegment hatBack = Segment(track, kPed, 3000, 710);
        g_sampled.push_back(hatBack.recordId);
        Check(NeighbourRecord(track, &create, kPed, 2500, 693, Sampled, bones) == hatBack.recordId &&
                bones == 710,
            "nearest of two candidates");
        g_sampled = {kPed.Packed(), hatOff.recordId};
        Check(NeighbourRecord(track, &create, kPed, 2500, 693, Sampled, bones) == 0,
            "unsampled neighbour is not offered");
    }

    void TestPropLayers()
    {
        PoseSegmentTrack track;
        track.Begin();
        CreatePacket owner = Create(kPed, 0, 710);
        owner.appearance.assets = {Asset(1), Asset(2)}; // hat, shirt
        CreatePacket hat;
        hat.id = ReplayId{9, 1};
        hat.kind = EntityKind::Prop;
        hat.owner = kPed;
        hat.timeMs = 1010;
        Appearance out;

        Check(std::string(PropLayers(track, &owner, hat, out)) == "none" && out.assets.empty(),
            "owner without segments: no layers");

        Segment(track, kPed, 1000, 693, {2});
        const char* source = PropLayers(track, &owner, hat, out);
        Check(std::string(source) == "owner_diff" && out.assets.size() == 1 &&
                out.assets[0].drawable == 1,
            "layer the owner lost at the nearest segment");
        Check(std::string(PropLayers(track, nullptr, hat, out)) == "none",
            "first change without the owner's create: nothing to diff");

        CreatePacket late = hat;
        late.timeMs = 2500;
        Check(std::string(PropLayers(track, &owner, late, out)) == "none",
            "segment outside the window is not the prop's change");

        Segment(track, kPed, 3000, 710, {2, 3});
        late.timeMs = 3020;
        Check(std::string(PropLayers(track, &owner, late, out)) == "none",
            "a segment that only gains layers loses none");

        CreatePacket dressed = hat;
        dressed.appearance.assets = {Asset(7)};
        Check(std::string(PropLayers(track, &owner, dressed, out)) == "object" &&
                out.assets.size() == 1 && out.assets[0].drawable == 7,
            "prop with its own layers keeps them");
    }
}

namespace
{
    // A wagon: create record without samples, the whole pose in a segment
    // from 0 ms with the same bone count. The record the puppet plays is the
    // segment's from the first tick; the create record only while no segment
    // has sampled.
    void TestPlaybackRecordFollowsSegmentForVehicle()
    {
        const ReplayId wagon{34, 1272};
        PoseSegmentTrack track;
        track.Begin();
        const PoseSegment seg = Segment(track, wagon, 0, 149);
        g_sampled = {seg.recordId};
        Check(PlaybackRecordAt(track, wagon, 0, Sampled) == seg.recordId, "wagon plays its segment record at 0 ms");
        Check(PlaybackRecordAt(track, wagon, 21950, Sampled) == seg.recordId, "wagon plays its segment record at the mark");
        g_sampled = {};
        Check(PlaybackRecordAt(track, wagon, 21950, Sampled) == wagon.Packed(), "no sampled segment: the create record");
        const ReplayId cart{0, 1006};
        Check(PlaybackRecordAt(track, cart, 5000, Sampled) == cart.Packed(), "entity without segments: the create record");
    }
}

int RunPoseSegmentsTests()
{
    TestPlaybackRecordFollowsSegmentForVehicle();
    TestSegmentAtWaitsForSamples();
    TestRecordHasSuccessor();
    TestFreshSpawnWaitsForFirstSegment();
    TestRecordForBones();
    TestNeighbourRecord();
    TestEarlySegmentForBones();
    TestPropLayers();
    if (g_segmentFailures == 0)
        std::printf("Pose segment tests passed.\n");
    return g_segmentFailures;
}
