#include "entity_events.h"
#include "entity_lifecycle.h"
#include "entity_selection.h"
#include "entity_transform.h"
#include "speech_identity.h"
#include "object_pose.h"
#include "skeleton_walk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay::entity;

    int g_lifecycleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_lifecycleFailures;
    }

    CreatePacket Packet(ReplayId id, std::uint32_t timeMs, std::int32_t handle)
    {
        CreatePacket packet;
        packet.id = id;
        packet.timeMs = timeMs;
        packet.sourceHandle = handle;
        packet.boneCount = 710;
        return packet;
    }

    bool Contains(const std::vector<CreatePacket>& alive, ReplayId id)
    {
        return std::any_of(alive.begin(), alive.end(),
            [&](const CreatePacket& packet) { return packet.id == id; });
    }

    void TestIdSlotReuseGetsNewInstance()
    {
        IdAllocator ids(4);
        const ReplayId a = ids.Allocate();
        const ReplayId b = ids.Allocate();
        Check(a.slot == 0 && b.slot == 1, "slots are handed out lowest first");
        ids.Release(a);
        const ReplayId c = ids.Allocate();
        Check(c.slot == 0 && c.instance != a.instance,
            "a reused slot never aliases the earlier entity");
        Check(ReplayId::FromPacked(c.Packed()) == c, "packed id round trip");

        ids.Release(a); // stale id: must not free c's slot
        Check(ids.InUse() == 2, "releasing a stale id is ignored");
        ids.Allocate();
        ids.Allocate();
        Check(!ids.Allocate().Valid(), "allocation fails when every slot is used");
    }

    void TestCreateDeleteAndFullCreatePerBlock()
    {
        IdAllocator ids;
        const ReplayId player = ids.Allocate();
        const ReplayId horse = ids.Allocate();
        LifecycleTrack track;
        track.Begin(2000);
        track.Create(Packet(player, 0, 11));
        track.Advance(900);
        track.Create(Packet(horse, 1500, 22));
        for (std::uint32_t t = 1500; t <= 5200; t += 33)
            track.Advance(t);
        track.Delete(horse, 5200);
        track.Advance(7000);

        std::vector<CreatePacket> alive;
        track.AliveAt(1000, alive);
        Check(alive.size() == 1 && Contains(alive, player), "only the player before the horse create");
        track.AliveAt(1500, alive);
        Check(alive.size() == 2 && Contains(alive, horse), "create applies at its own time");
        track.AliveAt(4100, alive);
        Check(alive.size() == 2, "a later block restores both from its full create");
        track.AliveAt(5199, alive);
        Check(Contains(alive, horse), "the horse lives until its delete");
        track.AliveAt(5200, alive);
        Check(alive.size() == 1 && !Contains(alive, horse), "delete applies at its own time");
        track.AliveAt(6500, alive);
        Check(alive.size() == 1 && alive[0].sourceHandle == 11,
            "the block after the delete no longer creates the horse");

        Check(track.Stats().blocks == 4, "one block per 2 s");
        Check(track.Stats().fullCreates == 0 + 2 + 2 + 1,
            "each block starts with a full create of the live entities");
        Check(!track.Create(Packet(player, 7000, 11)), "an id cannot be created twice while live");
        Check(!track.Delete(horse, 7000), "an id cannot be deleted twice");
    }

    void TestBlockIsSelfContained()
    {
        // Seeking into any block must not need earlier blocks: the covering
        // block alone answers which entities exist.
        IdAllocator ids;
        LifecycleTrack track;
        track.Begin(2000);
        std::vector<ReplayId> created;
        for (std::uint32_t t = 0; t < 20000; t += 700)
        {
            if (created.size() < 5)
            {
                created.push_back(ids.Allocate());
                track.Create(Packet(created.back(), t, static_cast<std::int32_t>(t)));
            }
            else
            {
                track.Delete(created.front(), t);
                ids.Release(created.front());
                created.erase(created.begin());
            }
            track.Advance(t);
        }
        std::vector<CreatePacket> alive;
        track.AliveAt(19999, alive);
        Check(alive.size() == track.Live().size(), "the last block reproduces the live set");
        track.AliveAt(0, alive);
        Check(alive.size() == 1, "the first block starts from its first create");
    }

    void TestGapOpensOnlyCoveringBlock()
    {
        LifecycleTrack track;
        track.Begin(2000);
        IdAllocator ids;
        track.Create(Packet(ids.Allocate(), 100, 1));
        track.Advance(30500); // stall: one block, not fifteen
        Check(track.BlockCount() == 2, "a stall opens only the covering block");
        std::vector<CreatePacket> alive;
        track.AliveAt(15000, alive);
        Check(alive.size() == 1, "time inside the gap resolves from the earlier block");
        track.AliveAt(30000, alive);
        Check(alive.size() == 1, "the covering block full-creates the live entity");
    }
    void TestAttachmentsFollowBlocks()
    {
        IdAllocator ids;
        const ReplayId wagon = ids.Allocate();
        const ReplayId left = ids.Allocate();
        const ReplayId right = ids.Allocate();
        LifecycleTrack track;
        track.Begin(2000);
        track.Create(Packet(wagon, 0, 1));
        track.Create(Packet(left, 0, 2));
        track.Create(Packet(right, 0, 3));
        Check(track.Attach({left, wagon, AttachmentKind::DraftHarness, 0}, 100), "attach a harnessed horse");
        Check(track.Attach({right, wagon, AttachmentKind::DraftHarness, 1}, 100), "attach the second horse");
        Check(!track.Attach({left, wagon, AttachmentKind::DraftHarness, 0}, 200),
            "re-attaching the same state emits nothing");
        track.Advance(4500);
        Check(track.Detach(right, 4600), "detach at unharness");
        track.Advance(6100);

        std::vector<Attachment> attached;
        track.AttachmentsAt(50, attached);
        Check(attached.empty(), "nothing attached before the attach event");
        track.AttachmentsAt(4100, attached);
        Check(attached.size() == 2, "a later block restores attachments from its full state");
        track.AttachmentsAt(4600, attached);
        Check(attached.size() == 1 && attached[0].child == left, "detach applies at its own time");
        track.AttachmentsAt(6200, attached);
        Check(attached.size() == 1 && attached[0].index == 0, "the next block carries the remaining one");

        track.Delete(wagon, 6300);
        track.AttachmentsAt(6300, attached);
        Check(attached.empty(), "deleting the parent detaches its children");
        Check(!track.Attach({right, left, AttachmentKind::DraftHarness, 0}, 6400) ||
                track.LiveAttachments().size() == 1,
            "attachments need live entities");
    }

    Candidate Near(std::int32_t handle, float distance, bool pinned = false)
    {
        return {handle, distance, pinned};
    }

    void TestSelectionNearestFirstWithinCapacity()
    {
        SelectionConfig config;
        config.capacity = 3;
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        SelectEntities(
            {Near(5, 25.0f), Near(1, 0.0f, true), Near(6, 10.0f), Near(7, 35.0f), Near(8, 3.0f)},
            {}, config, retire, add);
        Check(add == std::vector<std::int32_t>({1, 8, 6}),
            "pinned first, then nearest, up to capacity");
        Check(retire.empty(), "nothing to retire from an empty recording");
    }

    void TestSelectionHysteresis()
    {
        SelectionConfig config;
        config.enterRadius = 30.0f; // the test geometry predates the 60/75 m defaults
        config.exitRadius = 40.0f;
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        SelectEntities({Near(1, 0.0f, true), Near(2, 38.0f), Near(3, 35.0f)},
            {1, 2}, config, retire, add);
        Check(retire.empty(), "a recorded entity inside the exit radius stays");
        Check(add.empty(), "a new entity outside the enter radius does not enter");
        SelectEntities({Near(1, 0.0f, true), Near(2, 41.0f)}, {1, 2}, config, retire, add);
        Check(retire == std::vector<std::int32_t>({2}), "beyond the exit radius it leaves");
        SelectEntities({Near(1, 500.0f, true)}, {1}, config, retire, add);
        Check(retire.empty(), "a pinned entity never leaves by distance");
    }

    // A camp horse about 49 m from the player on the map, falling through it
    // from z 45.4 to -129: with the 3D distance it would leave at 175 m and
    // come back as a new entity every 8.8 s; with the selection distance it
    // keeps its place.
    void TestSelectionVerticalFall()
    {
        const float playerX = 672.09f, playerY = -1257.5f, playerZ = 43.98f;
        const float horseX = 693.25f, horseY = -1212.95f;
        SelectionConfig config;
        config.capacity = 56;
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        bool kept = true;
        float last3d = 0.0f;
        for (float z = 45.36f; z >= -131.0f; z -= 4.0f)
        {
            const float dx = horseX - playerX, dy = horseY - playerY, dz = z - playerZ;
            last3d = std::sqrt(dx * dx + dy * dy + dz * dz);
            SelectEntities({Near(1, 0.0f, true), Near(2, SelectionDistance(dx, dy, dz))}, {1, 2}, config, retire, add);
            kept = kept && retire.empty();
        }
        Check(last3d > config.exitRadius, "the clip's fall carries the 3D distance past the exit radius");
        Check(kept, "a horse falling through the map near the player keeps its place");
        Check(std::fabs(SelectionDistance(30.0f, 40.0f, -170.0f) - 50.0f) < 0.01f,
            "the height inside the slack does not count");
        Check(std::fabs(SelectionDistance(0.0f, 0.0f, 260.0f) - 60.0f) < 0.01f,
            "the height beyond the slack counts");
        Check(SelectionDistance(0.0f, 180.0f, 0.0f) > config.exitRadius,
            "far on the map still leaves");
    }

    void TestRecentRetirements()
    {
        RecentRetirements retired;
        RecentRetirements::Entry entry;
        retired.Note(77, 14300, -129.0f, false);
        Check(retired.Take(77, 16200, entry) && entry.timeMs == 14300 && !entry.evicted,
            "the same handle 1.9 s after its retirement is a re-entry");
        Check(!retired.Take(77, 16300, entry), "one re-entry per retirement");
        retired.Note(78, 1000, 0.0f, true);
        Check(!retired.Take(78, 1000 + RecentRetirements::kWindowMs + 1, entry), "outside the window it is a new entry");
        Check(!retired.Take(79, 2000, entry), "a handle never retired is not a re-entry");
        for (std::int32_t handle = 100; handle < 100 + static_cast<std::int32_t>(RecentRetirements::kCapacity) + 5; ++handle)
            retired.Note(handle, 3000, 0.0f, false);
        Check(retired.entries.size() == RecentRetirements::kCapacity, "the list is bounded");
        Check(!retired.Take(100, 3100, entry) && retired.Take(110, 3100, entry), "the oldest leave first");
        retired.Clear();
        Check(retired.entries.empty(), "a new recording starts empty");
    }

    // Defaults: a crowd larger than the capacity keeps the nearest
    // inside 150 m; recorded entities stay until 175 m.
    void TestSelectionBudgetDefaults()
    {
        SelectionConfig config;
        config.capacity = 40;
        Check(config.enterRadius == 150.0f && config.exitRadius == 175.0f,
            "default radius is 150 m to enter, 175 m to leave");
        std::vector<Candidate> crowd{Near(1, 0.0f, true)};
        for (std::int32_t handle = 2; handle <= 60; ++handle)
            crowd.push_back(Near(handle, static_cast<float>(handle) * 3.0f)); // 6 .. 180 m
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        SelectEntities(crowd, {}, config, retire, add);
        bool nearest = add.size() == 40 && add.front() == 1;
        for (std::size_t index = 1; index < add.size(); ++index)
            nearest = nearest && add[index] == static_cast<std::int32_t>(index + 1);
        Check(nearest, "the pinned player and the 39 nearest enter, nearest first");

        SelectEntities({Near(1, 0.0f, true), Near(2, 160.0f), Near(3, 170.0f)},
            {1, 3}, config, retire, add);
        Check(add.empty() && retire.empty(),
            "160 m does not enter, a recorded entity at 170 m stays");
        SelectEntities({Near(1, 0.0f, true), Near(3, 176.0f), Near(4, 149.0f)},
            {1, 3}, config, retire, add);
        Check(retire == std::vector<std::int32_t>({3}) && add == std::vector<std::int32_t>({4}),
            "beyond 175 m it leaves and a newcomer inside 150 m takes its place");
    }

    void TestSelectionPinnedDisplacesFarthest()
    {
        SelectionConfig config;
        config.capacity = 3;
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        SelectEntities(
            {Near(1, 0.0f, true), Near(2, 5.0f), Near(3, 20.0f), Near(4, 2.0f, true)},
            {1, 2, 3}, config, retire, add);
        Check(add == std::vector<std::int32_t>({4}) && retire == std::vector<std::int32_t>({3}),
            "a pinned newcomer displaces the farthest unpinned entity");
        SelectEntities({Near(1, 0.0f, true), Near(2, 5.0f), Near(3, 12.0f), Near(9, 3.0f)},
            {1, 2, 3}, config, retire, add);
        Check(add.empty() && retire.empty(), "an unpinned newcomer inside the eviction margin waits for a free place");
        SelectEntities({Near(1, 0.0f, true), Near(9, 1.0f)}, {1, 2, 3}, config, retire, add);
        Check(add.empty(), "recorded entities missing from the scan still hold their place");
    }

    Candidate Locked(std::int32_t handle, float distance)
    {
        Candidate candidate{handle, distance, false};
        candidate.locked = true;
        return candidate;
    }

    // A ring full of ambient peds must still let two brawlers spawned 8 m
    // away enter.
    void TestSelectionNearerEvictsFarthest()
    {
        SelectionConfig config;
        config.capacity = 4;
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        SelectionReport report;
        // Full: player, ambient peds at 60, 120 and 140 m; two brawlers at 8 m.
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Near(4, 140.0f), Near(10, 8.0f), Near(11, 9.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(add == std::vector<std::int32_t>({10, 11}), "nearer newcomers enter when full, nearest first");
        Check(retire == std::vector<std::int32_t>({4, 3}), "the farthest recorded peds are evicted, farthest first");
        Check(report.evicted == std::vector<std::int32_t>({4, 3}) && report.skippedFull.empty(), "evictions reported");

        // A locked entity (seat, harness, rope) is skipped: the next farthest goes.
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Locked(4, 140.0f), Near(10, 8.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(add == std::vector<std::int32_t>({10}) && retire == std::vector<std::int32_t>({3}),
            "a locked entity is never evicted");

        // Nothing evictable: the newcomer is reported as skipped for lack of a place.
        SelectEntities({Near(1, 0.0f, true), Locked(2, 60.0f), Locked(3, 120.0f), Locked(4, 140.0f), Near(10, 8.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(add.empty() && retire.empty() && report.skippedFull == std::vector<std::int32_t>({10}),
            "full of locked entities: skipped and reported");

        // At most maxEvictions per scan; the rest wait for the next scan.
        config.maxEvictions = 1;
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Near(4, 140.0f), Near(10, 8.0f), Near(11, 9.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(add == std::vector<std::int32_t>({10}) && retire == std::vector<std::int32_t>({4}) &&
                report.skippedFull == std::vector<std::int32_t>({11}),
            "evictions capped per scan");

        // A pinned newcomer still displaces past the cap, never a locked entity.
        config.maxEvictions = 0;
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Locked(4, 140.0f), Near(5, 1.0f, true)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(add == std::vector<std::int32_t>({5}) && retire == std::vector<std::int32_t>({3}),
            "a pinned newcomer displaces the farthest unlocked entity regardless of the cap");
    }

    void TestBeginClearsAttachments()
    {
        IdAllocator ids;
        LifecycleTrack track;
        track.Begin();
        const ReplayId parent = ids.Allocate();
        const ReplayId child = ids.Allocate();
        track.Create(Packet(parent, 0, 1));
        track.Create(Packet(child, 0, 2));
        Check(track.Attach({child, parent, AttachmentKind::WeaponPoint, 0}, 0), "attach");
        track.Begin();
        Check(track.LiveAttachments().empty(), "a new recording starts without attachments");
        std::vector<Attachment> out;
        track.Create(Packet(parent, 2500, 1));
        track.AttachmentsAt(2500, out);
        Check(out.empty(), "no attachment leaks into the new recording's blocks");
    }

    void TestEventRangeIsHalfOpen()
    {
        EventTrack track;
        ShotEvent shot;
        for (const std::uint32_t time : {100u, 200u, 200u, 300u})
        {
            shot.timeMs = time;
            track.Add(shot);
        }
        std::vector<std::size_t> out;
        track.Range(100, 200, out);
        Check(out.size() == 2 && out[0] == 1 && out[1] == 2, "range excludes its start and includes its end");
        track.Range(0, 99, out);
        Check(out.empty(), "nothing before the first event");
        track.Range(300, 300, out);
        Check(out.empty(), "an empty slice plays nothing");
    }

    void TestSpeechTrackRange()
    {
        SpeechTrack track;
        SpeechEvent line;
        line.timeMs = 500;
        line.variation = 2;
        track.Add(line);
        line.timeMs = 400; // late: clamped, order kept
        track.Add(line);
        std::vector<std::size_t> out;
        track.Range(0, 500, out);
        Check(out.size() == 2 && track.Events()[1].timeMs == 500, "speech events keep time order");
        track.Range(500, 1000, out);
        Check(out.empty(), "an event at the slice start was already played");
    }

    void TestSpeechIdentity()
    {
        using namespace replay::speech;
        Check(Finalize(PartialHash("abc")) == 0xED131F5Bu, "joaat of a known text");
        Check(PartialHash("ABC") == PartialHash("abc"), "name hash ignores case");
        Identity recorded;
        recorded.voice = 0x80E04F59;
        recorded.context = 0x1234;
        recorded.variation = 3;
        recorded.voiceObjectHash = 0x80E04F59;
        recorded.voiceObjectType = 0x22;
        Identity played = recorded;
        Check(Compare(recorded, played) == 0 && MismatchText(0) == "none", "the same line matches");
        played.variation = 4;
        Check(Compare(recorded, played) == kMismatchVariation && MismatchText(kMismatchVariation) == "variation",
            "another variation is a mismatch");
        played = recorded;
        played.voice = 0xE3CA8B39;
        played.voiceObjectHash = 0xE3CA8B39;
        Check(Compare(recorded, played) == (kMismatchVoice | kMismatchVoiceObject) &&
                MismatchText(kMismatchVoice | kMismatchVoiceObject) == "voice+voice_obj",
            "another voice is a mismatch");
        played = recorded;
        played.voiceObjectType = 0x21;
        Check(Compare(recorded, played) == kMismatchVoiceObject, "a voice object of another type is a mismatch");
        played = recorded;
        played.context = 0x5678;
        Check(Compare(recorded, played) == kMismatchContext, "another context is a mismatch");
        Identity unknown = recorded;
        unknown.voice = 0;
        unknown.voiceObjectHash = 0;
        played = recorded;
        played.voice = 0x11111111;
        Check(Compare(unknown, played) == 0, "fields the recording does not know are not compared");
        Check(Label(recorded.voice, recorded.context, 3) == Label(recorded.voice, recorded.context, 3) &&
                Label(recorded.voice, recorded.context, 3) != Label(recorded.voice, recorded.context, 4),
            "the label follows the variation");
        Check(std::string(PathName(0x2F9FB46)) == "ai" && std::string(PathName(0x2FA130E)) == "direct_ctx" &&
                std::string(PathName(1)) == "other",
            "core paths by return address");
    }

    void TestSpeechTiming()
    {
        using namespace replay::speech;
        Check(MedianLoad({}) == 0 && MedianLoad({300, 100, 200}) == 200, "median load");
        Check(StartLead(1150, 1000, true, 200) == 150, "an AI line is requested at its own request time");
        Check(StartLead(3000, 1000, true, 200) == 200, "a scripted line is requested one load before its start");
        Check(StartLead(1000, 1000, false, 200) == 0, "a line timed by its request keeps it");
        Check(StartLead(900, 1000, true, 200) == 0, "a start before the request is not trusted");
    }

    // An animal's lines killed while
    // loading (reason 4, one frame after the request) are asked again
    // through the animal path; nothing else is.
    void TestAnimalLineRetry()
    {
        using namespace replay::speech;
        Check(RetryAnimalLine(true, false, false, true, 4, 33), "an animal line stopped with reason 4 while loading is retried");
        Check(!RetryAnimalLine(false, false, false, true, 4, 33), "a human line is not");
        Check(!RetryAnimalLine(true, true, false, true, 4, 33), "a line that started is not");
        Check(!RetryAnimalLine(true, false, true, true, 4, 33), "a line is retried once");
        Check(!RetryAnimalLine(true, false, false, false, 0, 33), "a line gone without a known stop is not");
        Check(!RetryAnimalLine(true, false, false, true, 2, 33), "a line stopped by the replay (reason 2) is not");
        Check(!RetryAnimalLine(true, false, false, true, 1, 33), "a line that ended is not");
        Check(!RetryAnimalLine(true, false, false, true, 4, kAnimalRetryWindowMs + 1), "a late stop is not");
    }

    // The carried pelt (p_cs_pelt_large) gets a pose record; a
    // weapon, a rigid prop and a full capture table do not.
    void TestObjectPoseChoice()
    {
        using namespace replay::object_pose;
        Check(ShouldPose(false, 64, 20, 0, 64), "an attached object with a skeleton is posed");
        Check(!ShouldPose(true, 64, 20, 0, 64), "a weapon object is not");
        Check(!ShouldPose(false, 1, 20, 0, 64), "a rigid prop is not");
        Check(!ShouldPose(false, 64, 20, kCapacity, 64), "at most kCapacity objects");
        Check(!ShouldPose(false, 64, 64 - kBodyReserve, 0, 64), "the places kept for bodies are not taken");
        Check(ShouldPose(false, 64, 64 - kBodyReserve - 2, 1, 64), "the last free place outside the reserve is used");
    }

    // A busy scene: the recorder holds 61 bodies, 56 of them in the full ped
    // class, and the pelt (61 bones) must still get a place.
    void TestObjectPosePlaces()
    {
        using namespace replay::object_pose;
        // places 0: no place is given up, so this case never passes.
        Check(!ShouldPose(false, 61, 56, 0, 0, 56, 61, 64), "places 0: 61 bodies leave the pelt no place");
        Check(ShouldPose(false, 61, 20, 0, 0, 56, 25, 64), "places 0: a quiet scene still poses it");
        // places 4: the ped class gives a place up; while it is full the
        // object waits (it reserves the place the next scan frees).
        Check(!ShouldPose(false, 61, 56, 0, 4, 56, 61, 64), "places 4: a full ped class first gives a place up");
        Check(ShouldPose(false, 61, 55, 0, 4, 56, 60, 64), "places 4: one ped left, the pelt starts");
        Check(ShouldPose(false, 61, 52, 3, 4, 56, 60, 64), "the fourth object starts");
        Check(!ShouldPose(false, 61, 52, 4, 4, 56, 60, 64), "a fifth object is refused");
        Check(!ShouldPose(false, 61, 54, 2, 4, 56, 62, 64), "never more than the ped class can give");
        Check(!ShouldPose(false, 61, 55, 0, 4, 56, 64, 64), "never more than the 64 capture places");
        Check(!ShouldPose(true, 61, 20, 0, 4, 56, 25, 64), "a weapon object is not posed");
        Check(!ShouldPose(false, 1, 20, 0, 4, 56, 25, 64), "nor a rigid prop");
        Check(ClampPlaces(12) == kMaxPlaces && ClampPlaces(4) == 4, "places clamped to kMaxPlaces");
        Check(!ShouldPose(false, 61, 20, 8, 12, 56, 25, 64), "at most kMaxPlaces objects");

        Check(PedPlaces(56, 4) == 52 && PedPlaces(56, 0) == 56 && PedPlaces(56, 20) == 48, "ped class capacity");
        Check(PedPlaces(4, 8) == 0, "never below zero");
        Check(ReservedPlaces(4, 0, 0) == 0, "no object: the ped class keeps every place");
        Check(ReservedPlaces(4, 0, 1) == 1 && ReservedPlaces(4, 1, 0) == 1, "a waiting or posed object takes one");
        Check(ReservedPlaces(4, 3, 3) == 4, "at most the object places");
        Check(ReservedPlaces(0, 2, 2) == 0, "places 0 reserves nothing");
        // The classes never hold more than 64: ped class + vehicles + posed.
        for (std::size_t posed = 0; posed <= 4; ++posed)
            Check(PedPlaces(56, ReservedPlaces(4, posed, 0)) + 8 + posed <= 64, "the three classes fit in 64 places");
    }

    // The pelt that leaves the hand goes on as a dropped weapon
    // prop; its pose continues there, and playback plays it only on such a
    // prop.
    void TestObjectPoseContinuation()
    {
        using namespace replay::object_pose;
        Check(ContinuesPose(true, 0, true, false), "a posed object continues as a dropped prop");
        Check(ContinuesPose(true, 0, false, true), "a waiting one continues as a candidate");
        Check(!ContinuesPose(true, 0, false, false), "an object that never had a skeleton does not");
        Check(!ContinuesPose(true, 0x1234u, true, false), "a real weapon does not");
        Check(!ContinuesPose(false, 0, true, true), "nor a prop of another origin");
        Check(PoseableProp(true, 0) && !PoseableProp(true, 0x1234u) && !PoseableProp(false, 0),
            "playback plays a pose only on a dropped object with no weapon");
        Check(ShouldAskSkeletonAgain(61, false, 0, 1000, 0), "the first ask");
        Check(!ShouldAskSkeletonAgain(61, false, 1, 1500, 2000), "not before the interval");
        Check(ShouldAskSkeletonAgain(61, false, 1, 2000, 2000), "again after it, when the skeleton is gone");
        Check(!ShouldAskSkeletonAgain(61, false, kMaxSkeletonAsks, 99999, 0), "at most kMaxSkeletonAsks");
        Check(!ShouldAskSkeletonAgain(61, true, 0, 1000, 0), "not while it resolves");
        Check(!ShouldAskSkeletonAgain(1, false, 0, 1000, 0), "not for a rigid record");
    }

    // Free objects spawned
    // next to a recorded ped (object_pose.h spawn).
    void TestSpawnedObjectRule()
    {
        using namespace replay::object_pose;
        using namespace replay::object_pose::spawn;
        Check(WatchAtFirstSight(true, false, false, false, false, 0.8f), "a new free object next to a ped is watched");
        Check(!WatchAtFirstSight(false, false, false, false, false, 0.8f), "not with the rule off");
        Check(!WatchAtFirstSight(true, true, false, false, false, 0.8f), "an object of the start snapshot is not");
        Check(!WatchAtFirstSight(true, false, true, false, false, 0.8f), "nor a recorded one");
        Check(!WatchAtFirstSight(true, false, false, true, false, 0.8f), "nor one with a parent entity");
        Check(!WatchAtFirstSight(true, false, false, false, true, 0.8f), "nor a gore, vehicle or drop model");
        Check(!WatchAtFirstSight(true, false, false, false, false, kNearM + 0.01f), "nor one first seen far away");
        Check(!WatchAtFirstSight(true, false, false, false, false, -1.0f), "nor one with no ped");

        const float origin[3]{10.0f, 20.0f, 30.0f};
        const float still[3]{0.0f, 0.0f, 90.0f};
        const float nudged[3]{10.1f, 20.0f, 30.0f};
        const float carried[3]{10.0f, 20.2f, 30.0f};
        const float turned[3]{0.0f, 0.0f, 99.0f};
        const float wrapped[3]{0.0f, 0.0f, -270.0f};
        Check(!RootMoved(origin, nudged, still, still), "10 cm is no move");
        Check(RootMoved(origin, carried, still, still), "20 cm is a move");
        Check(RootMoved(origin, origin, still, turned), "9 degrees is a turn");
        Check(!RootMoved(origin, origin, still, wrapped), "90 and -270 degrees are the same heading");
        Check(AngleDelta(170.0f, -170.0f) < 20.01f && AngleDelta(170.0f, -170.0f) > 19.99f, "angle wraps");

        // Two bones: identity locals, then each kind of change.
        std::vector<float> a(2 * kFloatsPerBone, 0.0f);
        for (std::size_t bone = 0; bone < 2; ++bone)
        {
            a[bone * kFloatsPerBone + 3] = 1.0f; // w
            for (std::size_t k = 8; k < 12; ++k)
                a[bone * kFloatsPerBone + k] = 1.0f; // scale
        }
        std::vector<float> b = a;
        Check(!LocalsChanged(a.data(), b.data(), 2), "same locals");
        b[kFloatsPerBone + 4] = 0.005f;
        Check(!LocalsChanged(a.data(), b.data(), 2), "5 mm is noise");
        b[kFloatsPerBone + 4] = 0.02f;
        Check(LocalsChanged(a.data(), b.data(), 2), "2 cm on the second bone is a change");
        b = a;
        const float half = 0.5f * 3.14159265f / 180.0f; // 1 degree about z
        b[2] = std::sin(half);
        b[3] = std::cos(half);
        Check(!LocalsChanged(a.data(), b.data(), 2), "1 degree is noise");
        const float rolled = 0.5f * 10.0f * 3.14159265f / 180.0f;
        b[2] = std::sin(rolled);
        b[3] = std::cos(rolled);
        Check(LocalsChanged(a.data(), b.data(), 2), "10 degrees is a change");
        b = a;
        b[3] = -1.0f; // the same rotation, the other sign
        Check(!LocalsChanged(a.data(), b.data(), 2), "q and -q are the same rotation");

        Check(Decide(false, false, 100) == Verdict::Keep, "still watched");
        Check(Decide(true, false, 100) == Verdict::Record, "a move records");
        Check(Decide(false, true, 100) == Verdict::Record, "an animation records");
        Check(Decide(false, true, kWatchMs + 1) == Verdict::Record, "even at the end of the watch");
        Check(Decide(false, false, kWatchMs) == Verdict::Expire, "static for the whole watch: never");
        Check(PoseSpawned(true, 61) && !PoseSpawned(false, 61) && !PoseSpawned(true, 1),
            "posed only when animated with bones");
        Check(PoseableProp(false, true, 0) && PoseableProp(true, false, 0) && !PoseableProp(false, true, 0x1234u) &&
                  !PoseableProp(false, false, 0),
            "playback poses a dropped or spawned free object with no weapon");
    }

    // The ped class capacity dropped below the recorded count (a
    // posed object took a place): the farthest unpinned, unlocked entities
    // leave, at most maxEvictions per scan, and nobody enters.
    void TestSelectionShrinksToCapacity()
    {
        SelectionConfig config;
        config.capacity = 3;
        std::vector<std::int32_t> retire;
        std::vector<std::int32_t> add;
        SelectionReport report;
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Near(4, 140.0f), Near(10, 8.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(retire == std::vector<std::int32_t>({4, 3}) && report.evicted == std::vector<std::int32_t>({4, 3}),
            "the farthest recorded entity leaves for the object, a nearer newcomer still swaps");
        Check(add == std::vector<std::int32_t>({10}), "the class stays at its new capacity");
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Near(4, 140.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(retire == std::vector<std::int32_t>({4}) && add.empty(), "with no newcomer only the farthest leaves");
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Locked(4, 140.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(retire == std::vector<std::int32_t>({3}), "a locked entity stays, the next farthest leaves");
        config.capacity = 1;
        config.maxEvictions = 1;
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Near(4, 140.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(retire == std::vector<std::int32_t>({4}), "at most maxEvictions per scan");
        config.capacity = 4;
        config.maxEvictions = 4;
        SelectEntities({Near(1, 0.0f, true), Near(2, 60.0f), Near(3, 120.0f), Near(4, 140.0f)},
            {1, 2, 3, 4}, config, retire, add, &report);
        Check(retire.empty() && add.empty() && report.evicted.empty(), "at capacity nothing changes");
    }

    // The getter bodies
    // of vtable+0xB8 (build 1491.50), the generic one read for objects, the
    // walk evidence line and the late object pose policy.
    void TestObjectSkeletonWalk()
    {
        using namespace replay::skeleton::walk;
        // The 12 bytes read live for p_cs_pelt_large, then the
        // rest of the body at 0x65B6FC.
        const std::uint8_t pelt[kGetterProbeBytes] = {0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, 0xC1, 0xE8, 0x06, 0xA8,
            0x01, 0x74, 0x05, 0x48, 0x8B, 0x41, 0x38, 0xC3, 0x33, 0xC0, 0xC3, 0xCC, 0x00, 0x65};
        Check(ClassifyGetter(pelt, sizeof(pelt)) == GetterKind::Generic, "the pelt's getter is the generic body");
        Check(ClassifyGetter(pelt, 12) == GetterKind::Unknown, "12 bytes do not prove the generic body");
        std::uint8_t other[kGetterProbeBytes];
        std::memcpy(other, pelt, sizeof(other));
        other[16] = 0x40; // mov rax,[rcx+40h]: another field
        Check(ClassifyGetter(other, sizeof(other)) == GetterKind::Unknown, "a body reading another field is unknown");
        std::uint8_t vehicle[kGetterProbeBytes] = {0x48, 0x8B, 0x81, 0x38, 0x09, 0x00, 0x00, 0xC3};
        Check(ClassifyGetter(vehicle, sizeof(vehicle)) == GetterKind::Vehicle, "the vehicle body");
        std::uint8_t ped[kGetterProbeBytes] = {0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00, 0x25, 0xFF, 0xFF, 0x01, 0x00, 0x2B};
        Check(ClassifyGetter(ped, sizeof(ped)) == GetterKind::Ped, "the ped body");
        Check(ClassifyGetter(nullptr, 0) == GetterKind::Unknown, "no bytes");
        Check(GenericIsFragment(1u << 6) && !GenericIsFragment(0xFFFFFFBFu), "bit 6 of +0xB0 is the fragment flag");

        Evidence evidence;
        std::memcpy(evidence.getter, pelt, sizeof(evidence.getter));
        evidence.kind = GetterKind::Generic;
        evidence.entityType = 5;
        evidence.isFragment = 0;
        evidence.owner = 1;
        evidence.ownerSkel = 0;
        evidence.skeletonData = 1;
        const std::string text = Format(evidence);
        Check(text == "getter=8B81B0000000C1E806A80174|gk=generic|type=5|isfrag=0|frag=-|frag_owner=-|frag_skel=-"
                      "|owner=1|skel=0|skel_data=1|bones=-",
            "the walk line names every step");

        using namespace replay::object_pose;
        Check(ShouldRetryResolve(true, false, 1000, 1000), "a candidate is resolved again when due");
        Check(!ShouldRetryResolve(true, false, 999, 1000), "not before its time");
        Check(!ShouldRetryResolve(true, true, 5000, 1000), "a posed object is not");
        Check(!ShouldRetryResolve(false, false, 5000, 1000), "nor an object that is no candidate");
        Check(PlaybackBones(0, 61) == 61 && PlaybackBones(61, 0) == 61 && PlaybackBones(0, 0) == 0,
            "a late pose takes the bone count from the clip's create");
        Check(ShouldAskSkeleton(61, false, false), "a replay object without skeleton is given one");
        Check(!ShouldAskSkeleton(61, false, true), "once");
        Check(!ShouldAskSkeleton(61, true, false), "not when it has one");
        Check(!ShouldAskSkeleton(1, false, false), "not for a rigid record");
    }

    void TestFootstepTrackRange()
    {
        FootstepTrack track;
        FootstepEvent step;
        step.timeMs = 100;
        step.foot = 1;
        track.Add(step);
        step.timeMs = 130;
        step.foot = 0;
        track.Add(step);
        std::vector<std::size_t> out;
        track.Range(100, 200, out);
        Check(out.size() == 1 && track.Events()[out[0]].foot == 0, "a footstep at the slice start was already played");
    }

    void TestFxTrackRange()
    {
        FxTrack track;
        FxEvent event;
        event.timeMs = 100;
        event.effectHash = 1;
        track.Add(event);
        event.timeMs = 90; // late: clamped to the previous time
        event.effectHash = 2;
        track.Add(event);
        event.timeMs = 140;
        event.effectHash = 3;
        track.Add(event);
        std::vector<std::size_t> out;
        track.Range(100, 200, out);
        Check(out.size() == 1 && track.Events()[out[0]].effectHash == 3, "fx events at the slice start were already played");
        Check(track.Events()[1].timeMs == 100 && !track.Events()[1].entity.Valid(), "a late fx event is clamped and world-placed by default");
    }

    void TestDecalTrackRange()
    {
        DecalTrack track;
        DecalEvent event;
        event.timeMs = 100; event.engineId = 1; track.Add(event);
        event.timeMs = 140; event.engineId = 2; track.Add(event);
        std::vector<std::size_t> out;
        track.Range(100, 200, out);
        Check(out.size() == 1 && track.Events()[out[0]].engineId == 2, "decals at the slice start were already played");
    }

    void TestRegisteredFxFrameAt()
    {
        RegisteredFxTrack track;
        track.startMs = 100;
        track.endMs = 400;
        RegisteredFxFrame frame;
        frame.timeMs = 100; frame.zoom = 1.0f; track.frames.push_back(frame);
        frame.timeMs = 250; frame.zoom = 2.0f; track.frames.push_back(frame);
        Check(RegisteredFxFrameAt(track, 50) == nullptr, "no frame before the track");
        Check(RegisteredFxFrameAt(track, 100)->zoom == 1.0f, "the first frame at its time");
        Check(RegisteredFxFrameAt(track, 249)->zoom == 1.0f && RegisteredFxFrameAt(track, 300)->zoom == 2.0f, "the last frame at or before");
        Check(RegisteredFxFrameAt(track, 400)->zoom == 2.0f && RegisteredFxFrameAt(track, 401) == nullptr, "the track ends at endMs");
        // Two frames at the same time: the later one (as the old walk did).
        frame.timeMs = 250; frame.zoom = 3.0f; track.frames.push_back(frame);
        Check(RegisteredFxFrameAt(track, 250)->zoom == 3.0f && RegisteredFxFrameAt(track, 260)->zoom == 3.0f,
            "the last of equal times");

        // No frame cap: a lamp carried for 4 minutes at 60 frames a second
        // (the old cap of 4096 froze it after 68 s) keeps every frame, and
        // the lookup finds the right one anywhere along it.
        RegisteredFxTrack lamp;
        lamp.startMs = 0;
        constexpr std::uint32_t kFrames = 4u * 60u * 60u;
        for (std::uint32_t n = 0; n < kFrames; ++n)
        {
            frame.timeMs = n * 1000u / 60u;
            frame.zoom = static_cast<float>(n);
            lamp.frames.push_back(frame);
        }
        lamp.endMs = lamp.frames.back().timeMs;
        Check(lamp.frames.size() > 4096, "a long track keeps more than the old cap");
        bool exact = true;
        for (std::uint32_t n = 0; n < kFrames; n += 97)
        {
            const RegisteredFxFrame* at = RegisteredFxFrameAt(lamp, lamp.frames[n].timeMs);
            const RegisteredFxFrame* between = RegisteredFxFrameAt(lamp, lamp.frames[n].timeMs + 1u);
            exact = exact && at && at->zoom == static_cast<float>(n) && between &&
                (between->zoom == static_cast<float>(n) || between->timeMs == lamp.frames[n].timeMs + 1u);
        }
        Check(exact, "the frame in force along a long track");
        Check(RegisteredFxFrameAt(lamp, 200000)->zoom == static_cast<float>(12000),
            "a frame past the 68th second is played, not the last one before the old cap");
        Check(kMaxRegisteredFxTracks >= 65536, "the track count is only a runaway guard");
    }

    void TestComposeFxWorld()
    {
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float parent[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};
        const float offset[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0}; // w 0, as the engine leaves it
        const float zero[16] = {};
        float out[16];
        ComposeFxWorld(identity, parent, out);
        Check(out[12] == 10.0f && out[13] == 20.0f && out[1] == 1.0f, "identity offset gives the parent");
        ComposeFxWorld(offset, zero, out);
        Check(out[12] == 1.0f && out[0] == 1.0f, "an unset parent leaves the offset");
        ComposeFxWorld(offset, parent, out);
        Check(out[12] == 10.0f && out[13] == 21.0f, "the offset position is rotated by the parent");
    }

    void TestPoseSegmentsFollowTime()
    {
        PoseSegmentTrack track;
        track.Begin();
        const ReplayId npc{3, 16};
        const ReplayId other{4, 17};
        Check(track.At(npc, 5000) == nullptr, "no change: the create applies");
        PoseSegment segment;
        segment.entity = npc;
        segment.startMs = 13600;
        segment.recordId = track.NextRecordId();
        segment.boneCount = 900;
        track.Add(segment);
        segment.startMs = 20000;
        segment.recordId = track.NextRecordId();
        segment.boneCount = 880;
        track.Add(segment);
        Check((segment.recordId & PoseSegmentTrack::kRecordBit) != 0, "segment records never alias a packed id");
        Check(track.At(npc, 13599) == nullptr, "before the change the create applies");
        Check(track.At(npc, 13600) && track.At(npc, 13600)->boneCount == 900, "the change applies from its start");
        Check(track.At(npc, 25000) && track.At(npc, 25000)->boneCount == 880, "the latest change wins");
        Check(track.At(other, 25000) == nullptr, "other entities are not affected");
        Check(track.OwnerOf(segment.recordId) == npc, "a segment record maps back to its entity");
        Check(!track.OwnerOf(npc.Packed()).Valid(), "a packed id is not a segment record");
    }

    void TestForwardCursorPlaysOnlyNormalSteps()
    {
        ForwardEventCursor cursor;
        cursor.Reset(1000);
        std::uint32_t from = 0;
        std::uint32_t to = 0;
        Check(cursor.Advance(1016, true, from, to) && from == 1000 && to == 1016, "normal step plays");
        Check(!cursor.Advance(1016, true, from, to), "no movement plays nothing");
        Check(!cursor.Advance(1032, false, from, to), "paused or scrubbing plays nothing");
        Check(!cursor.Advance(5000, true, from, to), "a forward jump plays nothing");
        Check(!cursor.Advance(4000, true, from, to), "rewind plays nothing");
        Check(cursor.Advance(4016, true, from, to) && from == 4000, "play resumes from the new position");
    }

    bool Close(float a, float b)
    {
        return std::fabs(a - b) < 1e-4f;
    }

    void TestRelativeTransformRoundTrip()
    {
        replay::pose::WorldTransform parent;
        parent.position = {100.0f, -50.0f, 30.0f};
        // 90 degrees about Z.
        parent.rotation = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
        replay::pose::WorldTransform child;
        child.position = {100.0f, -49.0f, 31.0f};
        child.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        const replay::pose::WorldTransform relative = RelativeTransform(parent, child);
        // One metre along world +Y is the parent's +X after a 90 degree turn.
        Check(Close(relative.position.x, 1.0f) && Close(relative.position.y, 0.0f) &&
                Close(relative.position.z, 1.0f),
            "relative position is in parent space");
        const replay::pose::WorldTransform back = ComposeTransform(parent, relative);
        Check(Close(back.position.x, child.position.x) && Close(back.position.y, child.position.y) &&
                Close(back.position.z, child.position.z),
            "compose restores the child position");
        Check(Close(std::fabs(replay::pose::Dot(back.rotation, child.rotation)), 1.0f),
            "compose restores the child rotation");
    }

    void TestRelativeTrackRateAndSample()
    {
        RelativeTrack track(30);
        TransformSample sample;
        sample.timeMs = 0;
        sample.relative.position = {0.0f, 0.0f, 0.0f};
        Check(track.Append(sample), "first sample accepted");
        sample.timeMs = 10;
        Check(!track.Append(sample), "sample inside the interval is gated");
        sample.visible = false;
        Check(track.Append(sample), "visibility change passes the gate");
        sample.visible = true;
        sample.timeMs = 100;
        sample.relative.position = {1.0f, 0.0f, 0.0f};
        Check(track.Append(sample), "later sample accepted");
        sample.timeMs = 50;
        Check(!track.Append(sample), "going back in time is refused");

        TransformSample out;
        Check(track.Sample(55, out) && Close(out.relative.position.x, 0.5f) && !out.visible,
            "sample interpolates and keeps the earlier visibility");
        Check(track.Sample(500, out) && Close(out.relative.position.x, 1.0f) && out.visible,
            "sample clamps to the last one");
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        Check(track.Span(first, last) && first == 0 && last == 100, "span covers the samples");
    }

    void TestSeatPlacementError()
    {
        // The carriage faces +Y at (10, 0, 0); the seat holds the ped 1 m
        // behind and 0.5 m up in vehicle space.
        replay::pose::WorldTransform parent;
        parent.position = {10.0f, 0.0f, 0.0f};
        parent.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        replay::pose::WorldTransform seatRelative;
        seatRelative.position = {0.0f, -1.0f, 0.5f};
        seatRelative.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        replay::pose::WorldTransform child;
        child.position = {10.0f, -1.0f, 0.5f};
        child.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        Check(PlacementErrorM(parent, seatRelative, child) < 0.001f, "ped already where the seat holds it");
        child.position = {10.0f, -2.5f, 0.5f};
        Check(std::fabs(PlacementErrorM(parent, seatRelative, child) - 1.5f) < 0.001f,
            "ped still 1.5 m from the seat");
        // The same relation with the carriage turned 90 degrees about Z.
        const float half = std::sqrt(0.5f);
        parent.rotation = {0.0f, 0.0f, half, half};
        child.position = {11.0f, 0.0f, 0.5f};
        Check(PlacementErrorM(parent, seatRelative, child) < 0.001f, "seat relation follows the vehicle rotation");
    }
}

int RunEntityLifecycleTests()
{
    TestIdSlotReuseGetsNewInstance();
    TestCreateDeleteAndFullCreatePerBlock();
    TestBlockIsSelfContained();
    TestGapOpensOnlyCoveringBlock();
    TestAttachmentsFollowBlocks();
    TestSelectionNearestFirstWithinCapacity();
    TestSelectionHysteresis();
    TestSelectionVerticalFall();
    TestRecentRetirements();
    TestSelectionBudgetDefaults();
    TestSelectionPinnedDisplacesFarthest();
    TestSelectionNearerEvictsFarthest();
    TestBeginClearsAttachments();
    TestEventRangeIsHalfOpen();
    TestForwardCursorPlaysOnlyNormalSteps();
    TestSpeechTrackRange();
    TestSpeechIdentity();
    TestSpeechTiming();
    TestAnimalLineRetry();
    TestObjectPoseChoice();
    TestObjectPosePlaces();
    TestObjectPoseContinuation();
    TestSpawnedObjectRule();
    TestSelectionShrinksToCapacity();
    TestObjectSkeletonWalk();
    TestPoseSegmentsFollowTime();
    TestFootstepTrackRange();
    TestFxTrackRange();
    TestComposeFxWorld();
    TestRegisteredFxFrameAt();
    TestDecalTrackRange();
    TestRelativeTransformRoundTrip();
    TestSeatPlacementError();
    TestRelativeTrackRateAndSample();
    return g_lifecycleFailures;
}
