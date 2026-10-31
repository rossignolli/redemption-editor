// replay_objects_recording_internal.h: the counters and steps the replay_objects_recording*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "replay_objects_internal.h"

namespace replay::objects
{

        struct HatLookStats
        {
            std::uint32_t props = 0;   // prop creates that read a look
            std::uint32_t scenery = 0; // scenery entries that read a look
            std::uint32_t unread = 0;  // a guarded read failed
            std::uint32_t logs = 0;    // POSE_V2_HAT_LOOK lines this recording
        };
        extern HatLookStats g_hatLookStats;
        const char* ReadPoseV2HatLook(Object object, std::vector<replay::entity::MetaPedAsset>& out);
        void LogPoseV2HatLook(const char* path, const std::string& what, std::uint32_t model,
            const std::vector<replay::entity::MetaPedAsset>& look, const float* position);
    struct ObjectPoseStats
    {
        std::uint32_t started = 0;
        std::uint32_t late = 0;
        std::uint32_t continued = 0;
        std::uint32_t noPlaceTries = 0;
        std::uint32_t skeletonChanged = 0;
        std::uint32_t peak = 0;
        std::uint32_t spawned = 0; // spawned free objects posed at their register
    };
    extern ObjectPoseStats g_objectPoseStats;
    // weapon_observer.h AttachFrame: a frame read with a child bone gets that
    // bone folded into its offsets (the child's skeleton object matrix of the
    // bone, a guarded read). False when the bone could not be read: the
    // caller keeps no frame and playback places the object by its relative
    // track. True with nothing to do for a frame without a child bone.
    bool FoldPoseV2AttachChildBone(Object child, replay::weapon::AttachFrame& frame, const std::string& who);
    const char* TryStartPoseV2ObjectPose(Object object, replay::entity::ReplayId id,
        replay::skeleton::ResolvedPoseSkeleton& objectSkeleton, std::string& poseError, bool& posed,
        std::size_t* posedOut);

    // The retry (object_pose.h ShouldRetryResolve). A late start
    // writes the bone count into the create the clip keeps (the last one
    // with this id), which FindCreate and the pose segments read on
    // playback; the lifecycle blocks already written keep 0 (PlaybackBones).
    // Shared by the weapons and the props (RecordedWeapon and
    // RecordedProp have the same pose fields); the create patched is the
    // weapon's or the prop's.
    template <typename Recorded>
    void RetryPoseV2ObjectPose(Recorded& recorded, std::uint32_t timeMs)
    {
        recorded.nextPoseTryMs = timeMs + replay::object_pose::kRetryIntervalMs;
        ++recorded.poseTries;
        replay::skeleton::ResolvedPoseSkeleton objectSkeleton{};
        std::string poseError;
        bool posed = false;
        std::size_t posedBefore = 0;
        const char* status =
            TryStartPoseV2ObjectPose(recorded.object, recorded.id, objectSkeleton, poseError, posed, &posedBefore);
        if (PoseV2ObjectPoseRetryable(status))
        {
            recorded.poseNoPlace = std::strcmp(status, "no_place") == 0; // ReservedPlaces
            // Evidence at about 1 s and 10 s of retries, not every try.
            if ((recorded.poseTries == 4 || recorded.poseTries == 40) && g_poseV2ObjectPoseLogs < 48)
            {
                ++g_poseV2ObjectPoseLogs;
                Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(recorded.id)
                          << ",status=retry_" << status << ",tries=" << recorded.poseTries
                          << ",bones=" << objectSkeleton.boneCount << ",places=" << posedBefore << "/"
                          << ObjectPlacesLimit() << ",peds=" << CountPedBodies()
                          << ",walk=" << replay::skeleton::DescribePoseSkeletonWalk(recorded.object)
                          << ",time_ms=" << timeMs << '\n';
            }
            return;
        }
        recorded.poseCandidate = false;
        recorded.poseNoPlace = false;
        bool createPatched = false;
        if (posed)
        {
            ++g_objectPoseStats.late;
            recorded.posed = true;
            recorded.poseSkeleton = objectSkeleton.skeleton;
            recorded.poseBones = objectSkeleton.boneCount;
            for (auto packet = Ctx().clip.created.rbegin(); packet != Ctx().clip.created.rend(); ++packet)
            {
                if (packet->id == recorded.id && (packet->kind == replay::entity::EntityKind::Weapon ||
                                                     packet->kind == replay::entity::EntityKind::Prop))
                {
                    packet->boneCount = objectSkeleton.boneCount;
                    createPatched = true;
                    break;
                }
            }
        }
        if (g_poseV2ObjectPoseLogs < 48)
        {
            ++g_poseV2ObjectPoseLogs;
            Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(recorded.id)
                      << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(recorded.object))
                      << std::dec << ",status=" << status << ",late=1,tries=" << recorded.poseTries
                      << ",bones=" << objectSkeleton.boneCount
                      << ",fragment_path=" << (objectSkeleton.usedFragmentPath ? 1 : 0)
                      << ",getter=" << replay::skeleton::walk::GetterKindName(
                             static_cast<replay::skeleton::walk::GetterKind>(objectSkeleton.getterKind))
                      << ",places=" << (posedBefore + (posed ? 1 : 0)) << "/" << ObjectPlacesLimit()
                      << ",peds=" << CountPedBodies()
                      << ",create_patched=" << (createPatched ? 1 : 0)
                      << ",error=" << (poseError.empty() ? "none" : poseError.c_str()) << ",time_ms=" << timeMs
                      << '\n';
        }
    }

    // Per sample of a weapon or a prop with a pose (or waiting for
    // one): the retry, then the skeleton check of a posed object. Moved out
    // of SamplePoseV2Weapons so a prop that continues a posed
    // object is sampled the same way.
    template <typename Recorded>
    void SamplePoseV2ObjectPose(Recorded& recorded, std::uint32_t timeMs)
    {
        if (replay::object_pose::ShouldRetryResolve(recorded.poseCandidate, recorded.posed, timeMs, recorded.nextPoseTryMs))
            RetryPoseV2ObjectPose(recorded, timeMs);
        if (!recorded.posed)
            return;
        // object_pose.h: the object's pose, sampled here when the
        // pose finalizer did not visit its skeleton (as a vehicle's). A
        // skeleton the engine rebuilt ends the record: the armed pointer
        // would no longer be the object's.
        replay::skeleton::ResolvedPoseSkeleton current{};
        if (replay::skeleton::FastResolvePedPoseSkeleton(recorded.object, current) &&
            current.skeleton == recorded.poseSkeleton && current.boneCount == recorded.poseBones)
        {
            replay::pose::capture::PollIfStalled(recorded.id.Packed());
            return;
        }
        replay::pose::capture::Stop(recorded.id.Packed());
        recorded.posed = false;
        recorded.poseCandidate = false; // the record id is not started twice
        ++g_objectPoseStats.skeletonChanged;
        Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(recorded.id)
                  << ",status=skeleton_changed,bones=" << current.boneCount
                  << ",armed_bones=" << recorded.poseBones << ",time_ms=" << timeMs << '\n';
    }
    bool RegisterPoseV2Weapon(Object object, const replay::recorder::RecordedEntity& owner, int attachPoint,
        Hash weaponHint);
    void SamplePoseV2PropBones(replay::recorder::RecordedProp& prop, std::uint32_t timeMs, std::uint32_t& readsThisTick);

    void RegisterPoseV2Prop(Object object, replay::entity::ReplayId ownerId, Entity ownerHandle,
        replay::entity::PropOrigin origin, Hash weaponHash, const char* how,
        const std::vector<std::uint32_t>& weaponComponents = {}, std::uint32_t pieceGroup = 0,
        const PoseContinuation* continuation = nullptr, bool spawnedPose = false);
    // objcheck::AfterReread: the skeleton rereads near recorded
    // actors, the skeletons that appeared (recorded as animated), the
    // baselines taken for objects the snapshot cap left unread, the late
    // scenery joined to the watch.
    struct WorldSkeletonStats
    {
        std::uint32_t rereads = 0;
        std::uint32_t appeared = 0;
        std::uint32_t baselines = 0;
        std::uint32_t lateScenery = 0;
        std::uint32_t logs = 0;
        // Rereads and rebuilds judged against the bind pose (away: recorded as
        // an animation, map objects included; rest: a baseline; unknown: the
        // bind pose not read), the map objects that joined the watch late
        // (objcheck::LateJoinWatch) and the snapshot's watch order.
        std::uint32_t restAway = 0;
        std::uint32_t restAtRest = 0;
        std::uint32_t restUnknown = 0;
        std::uint32_t appearedMap = 0; // appeared on a map object (the new rule)
        std::uint32_t lateJoins = 0;
        float watchedFarthestM = -1.0f; // the farthest object the snapshot read the bones of
        std::uint32_t unreadNearM10 = 0; // snapshot objects within 10 m left unread (the cap)
    };
    extern WorldSkeletonStats g_worldSkeletonStats;
    struct SceneryStats
    {
        std::uint32_t snapshot = 0;
        std::uint32_t late = 0;
        std::uint32_t gone = 0;
        std::uint32_t taken = 0;
        std::uint32_t moving = 0;
        std::uint32_t full = 0;
        std::uint32_t invisible = 0;       // hidden when recorded
        std::uint32_t visibilityFlips = 0;
        std::uint32_t pickups = 0;         // pickup objects among the entries
        std::uint32_t lootWatched = 0;     // objects a recorded ped came within reach of
        std::uint32_t lootHidden = 0;      // taken: hidden with a ped within reach
        std::uint32_t lootGone = 0;        // taken: removed with a ped within reach
        std::uint32_t lootDropped = 0;     // hidden or removed with nobody within reach
        std::uint32_t lootMoved = 0;       // left the spot while watched (carried, pushed)
        std::uint32_t lootWatchFull = 0;
        std::uint32_t spawnWatch = 0;      // late entries the spawned object watch made
        std::uint32_t farLate = 0;         // late entries first seen out of the radius
        std::uint32_t farRechecks = 0;     // looks again at an object out of the radius
        std::uint32_t farResets = 0;       // the far table started over (kMaxSceneryFar)
    };
    extern SceneryStats g_sceneryStats;
    struct LootReinstanceStats
    {
        std::uint32_t pending = 0;
        std::uint32_t reinstanced = 0;
        std::uint32_t gone = 0;
    };
    extern LootReinstanceStats g_lootReinstanceStats;
    void ResetPoseV2Loot();
    struct MapDressingStats
    {
        std::uint32_t candidates = 0; // map data of the snapshot looked at
        std::uint32_t entries = 0;    // kept with an IPL
        std::uint32_t noIpl = 0;      // of the entries, in no sphere of an IPL on (the always-on map's)
        std::uint32_t gone = 0;       // gone before the spheres were there
        std::uint32_t full = 0;       // past kMaxMapDressing or the scenery cap
        bool listFull = false;        // the snapshot's object list was full: farther map data never seen
        std::uint32_t spheres = 0;    // the spheres of IPLs on at the resolve
        std::uint32_t streamedOut = 0; // left while their IPL stayed on (no gone time)
        std::uint32_t leftWithIpl = 0; // left with their IPL off (a gone time)
        std::uint32_t takenAway = 0;   // removed within reach of a recorded ped (a gone time: the take)
        std::uint32_t reinstanced = 0; // removed and made again at its spot (the watch goes on)
        std::set<std::uint32_t> ipls; // the IPLs that hold an entry
        bool resolved = false;
    };
    extern MapDressingStats g_mapDressingStats;
    struct SceneryChildStats
    {
        std::uint32_t added = 0;
        std::uint32_t atStart = 0; // there when the parent appeared
        std::uint32_t left = 0;
        std::uint32_t frames = 0;  // with the engine attach frame
        std::uint32_t full = 0;
        std::uint32_t visibilityFlips = 0;
        std::uint32_t fromScenery = 0; // a scenery entry of its own before it was attached
        std::uint32_t logs = 0;
    };
    extern SceneryChildStats g_sceneryChildStats;
    struct WheelPieceStats
    {
        std::uint32_t off = 0;       // wheel bits that came on in the part track
        std::uint32_t recorded = 0;  // wheel objects recorded as pieces
        std::uint32_t known = 0;     // the wheel object was recorded already (another path)
        std::uint32_t lost = 0;      // not found within 1 s
        std::uint32_t noObject = 0;  // the slot held no object (reads)
        std::uint32_t readFailed = 0;
        std::uint32_t unlisted = 0;  // read, not in the listing (reads)
        std::uint32_t urgent = 0;    // samples made at once after a break
    };
    extern WheelPieceStats g_wheelPieceStats;
    void SnapshotPoseV2World();
    void ScanPoseV2WorldObjects();
    struct PoseV2SpawnStats
    {
        std::uint32_t watched = 0;
        std::uint32_t recorded = 0;
        std::uint32_t moved = 0;
        std::uint32_t animated = 0;
        std::uint32_t expired = 0;
        std::uint32_t gone = 0;
        std::uint32_t taken = 0;    // recorded by another path (attached, drop)
        std::uint32_t attached = 0; // attached to an entity while watched
        std::uint32_t elsewhere = 0; // first seen attached to an entity that is not recorded
        std::uint32_t excluded = 0;  // gore, vehicle or pending drop model
        std::uint32_t full = 0;
        std::uint32_t refused = 0;
        // Still objects made late scenery entries, left as
        // the map's, found an entry already, past the watch's cap; static
        // verdicts of objects with an entry (joined the world watch).
        std::uint32_t scenery = 0;
        std::uint32_t sceneryMap = 0;
        std::uint32_t sceneryAlready = 0;
        std::uint32_t sceneryFull = 0;
        std::uint32_t sceneryWould = 0; // mode 1: mode 2 would have made an entry
        std::uint32_t staticScenery = 0;
        // First sights out of reach, rechecks made, watches started from the
        // recheck and recorded from them, expired objects re-armed, still
        // watches left (nobody near) and taken over by a nearer candidate,
        // recheck table resets.
        std::uint32_t firstFar = 0;
        std::uint32_t rechecks = 0;
        std::uint32_t rewatched = 0;
        std::uint32_t recordedRewatched = 0;
        std::uint32_t rearmed = 0;
        std::uint32_t left = 0;
        std::uint32_t evicted = 0;
        std::uint32_t recheckResets = 0;
    };
    extern PoseV2SpawnStats g_spawnStats;
    void ResetPoseV2Spawned();
    void LogPoseV2SpawnedSummary();
    void ScanPoseV2SpawnedObjects();
    void AddPoseV2Loot(Object object, const LootWatch& watch, replay::objcheck::LootTake take, std::uint32_t nowMs,
        bool exists);
    void ScanPoseV2Scenery();
    struct VehicleChildStats
    {
        std::uint32_t recorded = 0;
        std::uint32_t onTrainCars = 0;
        std::uint32_t posed = 0;  // with a pose record (or waiting for a skeleton)
        std::uint32_t refusedFar = 0;
        std::uint32_t refusedFull = 0;
        std::uint32_t logs = 0;
    };
    extern VehicleChildStats g_vehicleChildStats;
    void ScanPoseV2AttachedObjects();
}
