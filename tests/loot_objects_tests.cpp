// Objects. A: a loot drawer's skeleton read away from its bind pose is an
// animation on a map object too, the snapshot watches the nearest objects first
// and a map object listed late joins the watch near an actor. B: objects on a
// recorded vehicle or train car are recorded with their parent. C: a map object
// removed and made again at its spot is no take. D: the live objects that are
// not the map's are hidden while the replay plays. E: a gore piece is never a
// piece of the map.
#include "object_check.h"
#include "tpose_check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace replay::objcheck;

    int g_lootObjectsFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_lootObjectsFailures;
    }

    // One local as the engine keeps it: rotation xyzw, translation xyz pad, scale.
    void PutLocal(std::vector<float>& out, std::uint32_t bone, float angleZ, float tx, float ty, float tz)
    {
        if (out.size() < (bone + 1u) * kRestFloatsPerBone)
            out.resize((bone + 1u) * kRestFloatsPerBone, 0.0f);
        float* p = out.data() + static_cast<std::size_t>(bone) * kRestFloatsPerBone;
        p[0] = 0.0f;
        p[1] = 0.0f;
        p[2] = std::sin(angleZ * 0.5f);
        p[3] = std::cos(angleZ * 0.5f);
        p[4] = tx;
        p[5] = ty;
        p[6] = tz;
        p[7] = 0.0f;
        p[8] = p[9] = p[10] = 1.0f;
        p[11] = 0.0f;
    }

    void TestRestPose()
    {
        // A drawer: a root and the drawer bone, the drawer 0.3 m out.
        std::vector<float> bind;
        PutLocal(bind, 0, 0.0f, 0.0f, 0.0f, 0.0f);
        PutLocal(bind, 1, 0.0f, 0.0f, 0.1f, 0.5f);
        std::vector<float> closed = bind;
        RestMatch m = CompareWithRest(closed.data(), 2, bind.data(), 2);
        Check(RestOf(m, 2, 2) == RestState::AtRest && m.compared == 2 && m.turned == 0 && m.moved == 0,
            "rest: a skeleton just built is at its bind pose");
        std::vector<float> open = bind;
        PutLocal(open, 1, 0.0f, 0.0f, 0.4f, 0.5f);
        m = CompareWithRest(open.data(), 2, bind.data(), 2);
        Check(RestOf(m, 2, 2) == RestState::Away && m.moved == 1 && m.turned == 0 && std::fabs(m.maxMoveM - 0.3f) < 1e-4f,
            "rest: a drawer slid out is away from its rest (a translation)");
        std::vector<float> door = bind;
        PutLocal(door, 1, 0.6f, 0.0f, 0.1f, 0.5f);
        m = CompareWithRest(door.data(), 2, bind.data(), 2);
        Check(RestOf(m, 2, 2) == RestState::Away && m.turned == 1, "rest: a cabinet door turned is away from its rest");
        std::vector<float> nudge = bind;
        PutLocal(nudge, 1, 0.01f, 0.0f, 0.105f, 0.5f);
        m = CompareWithRest(nudge.data(), 2, bind.data(), 2);
        Check(RestOf(m, 2, 2) == RestState::AtRest, "rest: a fraction of a degree and 5 mm stay the rest pose");
        Check(RestOf(CompareWithRest(open.data(), 2, bind.data(), 3), 2, 3) == RestState::Unknown,
            "rest: a bind pose of another bone count is not this skeleton's");
        std::vector<float> zeros(2 * kRestFloatsPerBone, 0.0f);
        Check(RestOf(CompareWithRest(zeros.data(), 2, bind.data(), 2), 2, 2) == RestState::Unknown,
            "rest: locals not written yet (zero quaternions) tell nothing");
        Check(RestOf(CompareWithRest(nullptr, 2, bind.data(), 2), 2, 2) == RestState::Unknown, "rest: no locals");
        Check(std::string(RestText(RestState::Away)) == "away" && std::string(RestText(RestState::AtRest)) == "rest" &&
                  std::string(RestText(RestState::Unknown)) == "unknown",
            "rest: names");

        // The reread rules.
        Check(AfterReread(SkeletonWatch::Absent, true, 2, 0, false, RestState::Away) == SkeletonReread::Appeared,
            "reread: a map object's skeleton appearing open is an animation (recorded at once)");
        Check(AfterReread(SkeletonWatch::Absent, true, 2, 0, false, RestState::AtRest) == SkeletonReread::Baseline,
            "reread: a map object's skeleton appearing closed is the baseline (recorded when it moves)");
        Check(AfterReread(SkeletonWatch::Absent, true, 2, 0, false, RestState::Unknown) == SkeletonReread::Baseline,
            "reread: without the bind pose the old rule holds");
        Check(AfterReread(SkeletonWatch::Absent, true, 2, kMaxSkeletonBaselines, false, RestState::Away) ==
                  SkeletonReread::Appeared,
            "reread: an animation is recorded even with the baselines full");
        Check(AfterReread(SkeletonWatch::Unread, true, 2, 0, false, RestState::Away) == SkeletonReread::Baseline,
            "reread: a skeleton the snapshot left unread was there before: a baseline");
        Check(AfterReread(SkeletonWatch::Absent, true, 2, 0, true) == SkeletonReread::Appeared,
            "reread: scenery as before");
        Check(AfterRebuild(false, RestState::Away) == SkeletonReread::Appeared &&
                  AfterRebuild(false, RestState::AtRest) == SkeletonReread::Baseline &&
                  AfterRebuild(true, RestState::AtRest) == SkeletonReread::Appeared,
            "rebuild: a map object's skeleton rebuilt away from its rest is an animation");
        Check(kMaxSkeletonBaselines >= 256, "reread: room for the late joins' baselines");

        // The snapshot's order and the late join.
        Check(WatchBefore(2.0f, 9, 40.0f, 1) && !WatchBefore(40.0f, 1, 2.0f, 9) && WatchBefore(5.0f, 1, 5.0f, 2),
            "watch: nearest first, list order on a tie");
        Check(LateJoinWatch(false, false, 1.0f, true, 0), "late join: a map object within reach of an actor joins");
        Check(!LateJoinWatch(true, false, 1.0f, true, 0) && !LateJoinWatch(false, true, 1.0f, true, 0) &&
                  !LateJoinWatch(false, false, kSkeletonNearM + 0.5f, true, 0) && !LateJoinWatch(false, false, 1.0f, false, 0) &&
                  !LateJoinWatch(false, false, 1.0f, true, kMaxLateJoins) && !LateJoinWatch(false, false, -1.0f, true, 0),
            "late join: in the watch already, decided, far, not the map's, the cap, no actor");
    }

    struct Arena
    {
        static constexpr std::uintptr_t kBase = 0x10000000ull;
        std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x2000, 0);
        template <typename T>
        void Put(std::uintptr_t address, const T& value)
        {
            std::memcpy(bytes.data() + (address - kBase), &value, sizeof(T));
        }
        bool Read(std::uintptr_t address, void* out, std::size_t size) const
        {
            if (address < kBase || address + size > kBase + bytes.size())
                return false;
            std::memcpy(out, bytes.data() + (address - kBase), size);
            return true;
        }
    };

    void TestBindLocalsRead()
    {
        using namespace replay::tpose;
        Arena arena;
        const std::uintptr_t skeleton = Arena::kBase + 0x100;
        const std::uintptr_t data = Arena::kBase + 0x400;
        const std::uintptr_t records = Arena::kBase + 0x800;
        arena.Put<std::uint32_t>(skeleton + kSkeletonGroupCountOffset, 1);
        arena.Put<std::uint32_t>(skeleton + kSkeletonBoneCountOffset, 2);
        arena.Put<std::uintptr_t>(skeleton + kSkeletonGroupDataOffset, data);
        arena.Put<std::uint16_t>(data + kDataBoneCountOffset, 2);
        arena.Put<std::uintptr_t>(data + kDataBonesOffset, records);
        for (std::uint32_t bone = 0; bone < 2; ++bone)
        {
            replay::pose::BoneQts local;
            local.translation[1] = 0.1f * static_cast<float>(bone + 1);
            arena.Put(records + bone * kBoneRecordBytes, local);
            arena.Put<float>(records + bone * kBoneRecordBytes + 0x30, 77.0f); // the bone's own fields: not taken
        }
        const auto read = [&](std::uintptr_t address, void* out, std::size_t size) { return arena.Read(address, out, size); };
        std::vector<replay::pose::BoneQts> bind;
        Check(std::string_view(ReadBindLocals(read, skeleton, bind)).empty() && bind.size() == 2 &&
                  std::fabs(bind[1].translation[1] - 0.2f) < 1e-6f && bind[1].rotation.w == 1.0f && bind[0].scale[0] == 1.0f,
            "bind locals: rotation, translation and scale of each 0x50 byte record");
        arena.Put<std::uint32_t>(skeleton + kSkeletonBoneCountOffset, 3);
        Check(std::string_view(ReadBindLocals(read, skeleton, bind)) == "count_mismatch" && bind.empty(),
            "bind locals: groups that do not add up are refused");
        Check(std::string_view(ReadBindLocals(read, 0, bind)) == "no_skeleton", "bind locals: no skeleton");
        static_assert(sizeof(replay::pose::BoneQts) == kRestFloatsPerBone * sizeof(float), "a local is 12 floats");
    }

    void TestVehicleChildren()
    {
        Check(VehicleChildVerdict(2.0f, 0, 10, 96) == VehicleChild::Record, "vehicle child: near a ped, room: recorded");
        Check(VehicleChildVerdict(kVehicleChildNearM + 1.0f, 0, 10, 96) == VehicleChild::Far &&
                  VehicleChildVerdict(-1.0f, 0, 10, 96) == VehicleChild::Far,
            "vehicle child: no recorded ped near (the rest of the train): left out");
        Check(VehicleChildVerdict(2.0f, kMaxVehicleChildren, 10, 96) == VehicleChild::Full,
            "vehicle child: the cap");
        Check(VehicleChildVerdict(2.0f, 0, 96 - kVehicleChildWeaponRoom, 96) == VehicleChild::Full &&
                  VehicleChildVerdict(2.0f, 0, 96 - kVehicleChildWeaponRoom - 1, 96) == VehicleChild::Record,
            "vehicle child: the peds' weapons keep their room in the registry");
    }

    void TestReinstance()
    {
        const float spot[3] = {665.53f, -1243.90f, 43.14f};
        const float same[3] = {665.60f, -1243.85f, 43.14f};
        const float away[3] = {666.61f, -1244.89f, 43.14f};
        Check(ReinstanceOf(0x915b769bu, spot, 0x915b769bu, same), "reinstance: the same model at the spot");
        Check(!ReinstanceOf(0x915b769bu, spot, 0x5f5f56e4u, same) && !ReinstanceOf(0x915b769bu, spot, 0x915b769bu, away) &&
                  !ReinstanceOf(0u, spot, 0u, same),
            "reinstance: another model, or a neighbor of the model a meter away, is not it");
        // A recorded case: gone at 105601, made again before 106003.
        Check(LootGoneStepOf(false, 105601, 105851) == LootGoneStep::Wait, "reinstance: waits after the removal");
        Check(LootGoneStepOf(true, 105601, 105851) == LootGoneStep::Reinstance, "reinstance: made again, no take");
        Check(LootGoneStepOf(false, 105601, 105601 + kReinstanceMs) == LootGoneStep::Gone,
            "reinstance: nothing made again in time: the take, at the removal");
        Check(LootGoneStepOf(false, 105601, 100000) == LootGoneStep::Wait, "reinstance: a clock behind waits");
    }

    void TestLiveObjectHide()
    {
        LiveObject object;
        object.missionEntity = true;
        object.owner = kOwnerMission;
        Check(HideLiveObject(object) == LiveHide::Hide, "live: a mission's object in the area is hidden (the spool of wire)");
        object.replayOwn = true;
        Check(HideLiveObject(object) == LiveHide::Replay, "live: the replay's own objects stay");
        object.replayOwn = false;
        object.onKept = true;
        Check(HideLiveObject(object) == LiveHide::OnKept, "live: what the player or a puppet holds stays");
        object.onKept = false;
        object.inArea = false;
        Check(HideLiveObject(object) == LiveHide::Outside, "live: outside the clip area");
        object.inArea = true;
        object.missionEntity = false;
        object.owner = 14;
        Check(HideLiveObject(object) == LiveHide::Map, "live: the map's objects are the map's (their originals hide by stand)");
        object.owner = kOwnerPropSet;
        Check(HideLiveObject(object) == LiveHide::Hide, "live: a prop set's object is hidden");
        object.owner = kOwnerTempA;
        Check(HideLiveObject(object) == LiveHide::Hide, "live: a temporary object is hidden");
        object.owner = 14;
        object.pickup = true;
        Check(HideLiveObject(object) == LiveHide::Hide, "live: a pickup is hidden");
        object.rope = true;
        Check(HideLiveObject(object) == LiveHide::Rope, "live: a rope's entity is the rope track's");
        object.rope = false;
        object.vehicleModel = true;
        Check(HideLiveObject(object) == LiveHide::Debris, "live: a vehicle's debris has its own path");
        Check(std::string(LiveHideText(LiveHide::Hide)) == "hide" && std::string(LiveHideText(LiveHide::Map)) == "map",
            "live: names");
        Check(InClipArea(true, 0.0f, 0.0f, -10.0f, -10.0f, 10.0f, 10.0f) && !InClipArea(true, 50.0f, 0.0f, -10.0f, -10.0f, 10.0f, 10.0f) &&
                  InClipArea(true, 35.0f, 0.0f, -10.0f, -10.0f, 10.0f, 10.0f) && InClipArea(false, 1e6f, 0.0f, 0, 0, 0, 0),
            "live: the clip area with its 30 m margin (no area: everywhere)");
    }

    // QA4 (two testers): a lighting mod's lantern placed during the replay
    // was hidden at once and every tick after, and placing props was "the
    // biggest factor" in the crash after leaving the replay.
    void TestOtherModObjects()
    {
        // A mod's prop placed during the replay: CREATE_OBJECT from an ASI.
        LiveObject lantern;
        lantern.missionEntity = true;
        lantern.owner = kOwnerMission;
        lantern.bornInPlayback = true;
        Check(HideLiveObject(lantern) == LiveHide::OtherMod, "mod: a script object born during the playback is the mod's");
        lantern.inArea = true;
        lantern.onKept = false;
        Check(HideLiveObject(lantern) != LiveHide::Hide, "mod: never hidden, in the clip area too");
        lantern.missionEntity = false; // released by its script, the owner still CREATE_OBJECT's
        Check(HideLiveObject(lantern) == LiveHide::OtherMod, "mod: the CREATE_OBJECT owner alone is a script's");
        // Placed before the replay, by an ASI's thread (joaat("audiotest")).
        LiveObject before;
        before.missionEntity = true;
        before.owner = kOwnerMission;
        before.otherScript = true;
        Check(HideLiveObject(before) == LiveHide::OtherMod, "mod: an ASI thread's object made before the replay is the mod's");
        before.owner = 14;
        before.missionEntity = false;
        Check(HideLiveObject(before) == LiveHide::OtherMod, "mod: an ASI thread's object whatever its owner");
        // What stays as it was.
        LiveObject mission;
        mission.missionEntity = true;
        mission.owner = kOwnerMission;
        Check(HideLiveObject(mission) == LiveHide::Hide, "mod: a game mission's prop there at the start is still hidden");
        LiveObject temp;
        temp.owner = kOwnerTempA;
        temp.bornInPlayback = true;
        Check(HideLiveObject(temp) == LiveHide::Hide, "mod: a temporary object (no script's) born meanwhile is still hidden");
        LiveObject propSet;
        propSet.owner = kOwnerPropSet;
        propSet.bornInPlayback = true;
        Check(HideLiveObject(propSet) == LiveHide::Hide, "mod: a prop set's object born meanwhile is still hidden");
        LiveObject map;
        map.owner = 25;
        map.bornInPlayback = true;
        Check(HideLiveObject(map) == LiveHide::Map, "mod: a map object streamed in meanwhile is the map's");
        LiveObject own = lantern;
        own.replayOwn = true;
        Check(HideLiveObject(own) == LiveHide::Replay, "mod: the replay's own objects are the replay's first");
        Check(IsOtherModObject(false, true, true, 0) && IsOtherModObject(true, false, false, 14) &&
                  !IsOtherModObject(false, false, true, kOwnerMission) && !IsOtherModObject(false, true, false, kOwnerTempB),
            "mod: the rule");
        Check(std::string(LiveHideText(LiveHide::OtherMod)) == "other_mod", "mod: name");
        Check(static_cast<std::size_t>(LiveHide::OtherMod) + 1 == kLiveHideCount, "mod: the verdict count");
        // The leftovers at the playback start: a script's object is never deleted.
        Check(LeftoverDeletable(false), "leftover: the recording's adopted world object goes");
        Check(!LeftoverDeletable(true), "leftover: a mission entity (a mod's prop the recording watched) stays its script's");
    }

    void TestGorePieces()
    {
        Check(GorePieceCandidate(false, false), "gore: a loose object by a gore event, of no world model, is a piece");
        Check(!GorePieceCandidate(true, false), "gore: a piece of a map object (the railing) is a world piece");
        Check(!GorePieceCandidate(false, true), "gore: the model of a break of the map is never a gore model");
        Check(!AdoptForGore(true, false, false) && !AdoptForGore(false, true, false) && !AdoptForGore(false, false, true) &&
                  AdoptForGore(false, false, false),
            "gore: never adopt a mission object, a stand-in or the original a stand hides");
        Check(GorePiecePlayable(true, false) && GorePiecePlayable(false, false) && !GorePiecePlayable(false, true),
            "gore: an old clip's piece of a world model is not built whole");
    }
}

int RunLootObjectsTests()
{
    g_lootObjectsFailures = 0;
    TestRestPose();
    TestBindLocalsRead();
    TestVehicleChildren();
    TestReinstance();
    TestLiveObjectHide();
    TestOtherModObjects();
    TestGorePieces();
    if (g_lootObjectsFailures == 0)
        std::printf("loot objects tests passed\n");
    return g_lootObjectsFailures;
}
