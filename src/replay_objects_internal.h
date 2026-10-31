// replay_objects_internal.h: the state and helpers the replay_objects*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "replay_objects.h"
#include "replay_trains.h" // the train cars' pose places

#include "audio_observer.h"
#include "burn_level.h" // object burn level
#include "engine_sigs.h"
#include "frag_observer.h"
#include "frag_piece.h" // a loose piece is not the whole object
#include "game_sig.h"
#include "entity_appearance.h"
#include "entity_pools.h"
#include "log_format.h"
#include "pose_playback.h"
#include "pose_capture.h"
#include "pose_segments.h"
#include "profiler.h"
#include "entity_cache.h"
#include "game_sig.h" // the vehicle pieces' signatures
#include "guarded_memory.h"
#include "hat_look.h" // the look of a loose hat
#include "log_policy.h"
#include "object_pose.h"
#include "object_check.h"
#include "object_look.h" // custom texture and tint
#include "replay_world.h" // the map dressing decides after the clip's map state
#include "rope_models.h" // a rope's own entity is no prop
#include "skeleton_resolve.h"
#include "skeleton_walk.h"
#include "track_index.h"
#include "vehicle_pieces.h"
#include "weapon_components.h"
#include "weapon_observer.h"

#include "main.h"
#include "natives.h"
#include "weapon_bones_core.h" // weapon bones play check
#include "weapon_look_core.h" // the weapon look
#include "strap_state_core.h" // the strap state played on a puppet weapon

#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace replay::objects
{

    using enginesig::kFragInstVtable;
        Context& Ctx();

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;
        std::string ObjectClassText(const void* entity);
        std::uint64_t ElapsedUs(std::uint64_t start, std::uint64_t end);
        bool GuardedCaptureLayers(Entity entity, replay::entity::Appearance* out);
        Hash ReadObjectWeapon(Object object, Hash model, bool& hasWeapon);
        std::uint32_t ReadEntityOwner(Entity entity);
        extern int g_hatLookCore;
        extern int g_hatLookWalk;
        constexpr std::uint32_t kHatLookLogCap = 32;
        const char* ReadPoseV2HatLook(Object object, std::vector<replay::entity::MetaPedAsset>& out);
        extern std::uint32_t g_objectWeaponFromObject;
        extern std::uint32_t g_objectWeaponUnresolved;
        extern std::uint32_t g_objectCheckLogs;
        void LogObjectCheck(replay::objcheck::Verdict verdict, const std::string& id, std::uint32_t model,
            std::uint32_t playheadMs, const char* extra);
        void ReadObjectComponents(Object object, std::vector<std::uint32_t>& out);
        extern bool g_weaponComponentsEngine;
        extern bool g_weaponComponentsChecked;
        extern std::uint32_t g_weaponComponentsCrossChecks;
        extern bool g_weaponWearRaw;
        extern bool g_weaponWearChecked;
        extern bool g_weaponWearWrite;
        extern std::uint32_t g_weaponLookRecLogs;
        extern std::uint32_t g_weaponLookPlayLogs;
        extern replay::weaponlook::Tally g_weaponLookTally;
        constexpr std::uint32_t kWeaponLookLogCap = 400;
        bool PoseV2ComponentsAgree(Object object, const std::vector<std::uint32_t>& engine,
            const std::vector<std::uint32_t>& named, std::string& why);
        void ReadPoseV2WeaponLook(Object object, const std::vector<std::uint32_t>* fallback, replay::WeaponCondition& state);
        replay::weaponlook::Wear WearOf(const replay::WeaponCondition& state);
        const char* NotePoseV2WeaponLook(std::uint32_t recordId, const replay::WeaponCondition& state, std::uint32_t timeMs,
            bool force);
        void LogPoseV2WeaponLookRecord(const replay::entity::ReplayId& id, const replay::WeaponCondition& state,
            const char* changed, std::uint32_t timeMs);
    extern bool g_weaponPassReady;
    extern std::string g_weaponPassError;

    // Weapon objects: an object entity whose create carries the weapon,
    // attached to its ped. Every recorded ped's weapon attach points are read
    // at the scan; each object found gets its own ReplayId and a WeaponPoint
    // attachment, and its transform is sampled every frame in the ped's space
    // (both matrices read in the same frame).

    // A weapon that leaves its attach point continues as a prop (below).
    void ContinuePoseV2WeaponAsProp(const replay::recorder::RecordedWeapon& weapon, const char* reason);
    void RetirePoseV2PropOf(Object object, const char* reason);
    extern std::uint32_t g_poseV2AttachLogs;
    extern std::uint32_t g_strapApplyLogs;
    extern std::uint32_t g_poseV2WeaponEvidenceLines;
    bool SameAttachFrame(const replay::weapon::AttachFrame& a, const replay::weapon::AttachFrame& b);
    bool SameStrapState(const replay::weapon::StrapState& a, const replay::weapon::StrapState& b);
    replay::pose::WorldTransform ReadEntityTransform(Entity entity);
    std::string PoseV2WeaponName(Hash weapon);
    extern std::uint32_t g_poseV2ObjectPoseLogs;
    extern bool g_weaponBonePose;
    extern std::uint32_t g_weaponBonePlaces;
    extern std::set<std::uint32_t> g_weaponBonesSeen;
    extern std::set<std::uint32_t> g_weaponBonesArmed;
    extern std::uint32_t g_weaponBonesPlayLogs;
    // weapon_bones_core.h the drawn check of every armed weapon pose,
    // independent of ObjectDrawnCheck: at the start of the tick (before this
    // tick's publish) the buffer against the pose written last tick, and
    // whether that pose moved since the tick before (there was motion to show).
    struct WeaponDrawn
    {
        std::vector<replay::pose::BoneQts> previous; // the written locals of the last check
        // Per bone, ticks the record turned it and ticks the
        // drawn buffer (locals and objects) turned it.
        std::vector<replay::pose::BoneQts> previousDrawn;
        std::vector<std::uint32_t> recTicks;
        std::vector<std::uint32_t> drawnTicks;
        replay::weaponbones::PlayCheck check;
        std::uint32_t model = 0;
        std::uint32_t weapon = 0;
        bool interimLogged = false;
    };
    extern std::map<std::uint32_t, WeaponDrawn> g_weaponDrawn;
    extern std::map<std::uint32_t, std::uint32_t> g_passRecWeapons;
    extern std::vector<replay::weapon::WeaponRecordEntry> g_passRecPublished;
    extern std::uint32_t g_weaponDrawnLines;
    extern bool g_ammoScanEveryTick;
    extern std::vector<int> g_ammoPrevHandles;
    struct AmmoPending
    {
        Object object = 0;
        std::uint32_t firstMs = 0;
    };
    extern std::vector<AmmoPending> g_ammoPending;
    constexpr std::uint32_t kAmmoAskMs = 250;
    constexpr std::uint32_t kAmmoFreshPerTick = 32;
    constexpr std::size_t kAmmoPendingMax = 128;
    std::size_t CountPosedObjects();
    std::size_t CountPedBodies();

    // object_pose.h: resolves an attached object's skeleton and,
    // when it has bones and a capture place is free, starts its pose record
    // under the object's own id. Shared by the register and by the retry of
    // an object whose skeleton did not exist yet at the attach.
    // The place rule counts the posed weapons and props (object_pose.h
    // ShouldPoseShared); `posedOut` is the posed count before it.
    std::size_t CountWeaponBones(bool posed);
    std::uint32_t ObjectPlacesLimit();
    bool PoseV2ObjectPoseRetryable(const char* status);

    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out);
    bool IsPlayerWeapon(const replay::recorder::RecordedWeapon& weapon);

    // object_pose.h WeaponPlaces: a real weapon in a hand records its own
    // bones from now on, every tick, like every animated object (no wait for
    // the locals to move, no places but the free capture slots). A weapon
    // whose skeleton does not read yet is watched and started at the first read
    // that has one.
    void EndPoseV2WeaponBoneWatch(replay::recorder::RecordedWeapon& weapon);
    void RetirePoseV2Weapon(std::size_t index, const char* reason);
    extern std::uint32_t g_poseV2OwnerChangeLogs;
    void ChangePoseV2WeaponOwner(const replay::recorder::RecordedWeapon& weapon,
        const replay::recorder::RecordedEntity& newOwner, int attachPoint, const char* how);
    extern bool g_objectBurnLevel;
    // The ticks a scenery stand-in stays shown after the prop that takes it over
    // published its first pose (one frame of overlap, no frame without either).
    constexpr std::uint32_t kSceneryHandoffTicks = 1;
    constexpr std::uint32_t kObjectBurnCheckMs = 250;
    extern std::uint32_t g_objectBurnEntryLogs;
    extern std::uint32_t g_objectBurnLogs;
    void LogPoseV2ObjectBurn(const char* phase, std::uint32_t key, bool scenery, const replay::burn::State& state,
        Object object, std::uint32_t timeMs);
    void NotePoseV2ObjectBurn(std::vector<replay::BurnLevelTrack>& tracks, replay::TrackIndex<replay::BurnLevelTrack>& index,
        std::uint32_t key, Object object, bool scenery, std::uint32_t timeMs);

    template <class Recorded>
    void SamplePoseV2ObjectBurnLevel(Recorded& recorded, std::uint32_t timeMs)
    {
        if (!g_objectBurnLevel || !replay::burn::Enabled())
            return;
        if (recorded.burnChecked && timeMs - recorded.burnCheckMs < kObjectBurnCheckMs)
            return;
        recorded.burnChecked = true;
        recorded.burnCheckMs = timeMs;
        static replay::TrackIndex<replay::BurnLevelTrack> index;
        NotePoseV2ObjectBurn(Ctx().clip.burnLevels, index, recorded.id.Packed(), recorded.object, false, timeMs);
    }
    void SamplePoseV2SceneryBurnLevel(std::size_t entry, Object object, std::uint32_t timeMs);
    void SamplePoseV2WeaponCondition(const replay::recorder::RecordedWeapon& weapon, std::uint32_t timeMs);
    void SamplePoseV2Weapons();
    extern std::uint32_t g_poseV2PropsDropped;
    extern std::uint32_t g_poseV2RopeModelsSkipped;
    extern std::uint32_t g_poseV2RopeModelLogs;
    // A prop of a throwable weapon (knife, tomahawk, hatchet,
    // from any origin) goes under the projectile successor watch; its fate
    // is logged when the watch ends (below).
    void WatchPoseV2Throwable(Object object, replay::entity::ReplayId id, replay::entity::ReplayId ownerId,
        Entity ownerHandle, Hash weaponHash);

    // object_pose.h ContinuesPose: the weapon a dropped weapon
    // prop continues, and whether it was posed or waited for a pose.
    struct PoseContinuation
    {
        replay::entity::ReplayId from;
        bool posed = false;
        bool candidate = false;
    };

    // The groups of a loose fragment piece (below, with the breaks).
    void NotePoseV2FragPiece(Object object, replay::entity::ReplayId id, replay::entity::PropOrigin origin, Hash weapon);

    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out);
    constexpr std::uint32_t kPropBoneLogCap = 48;
    std::size_t CountPropBoneWatches();
    void EndPoseV2PropBoneWatch(replay::recorder::RecordedProp& prop);
    void RetirePoseV2Prop(std::size_t index, const char* reason);
    bool IsPoseV2KnownObject(Entity object);
    void SearchPoseV2WeaponDrops();
    extern ULONGLONG g_poseV2ProjectileScanUntil;
    extern std::uint32_t g_poseV2Projectiles;
    extern std::uint32_t g_poseV2ProjectileSuccessors;
    // A recorded projectile until it is gone: where it was last. When it
    // sticks the engine replaces it with a new plain object in the same spot
    // (always against anything that is not a ped): that object continues it.
    struct ProjectileWatch
    {
        Object object = 0;
        replay::entity::ReplayId owner;
        Entity ownerHandle = 0;
        Vector3 last{};
        ULONGLONG goneAt = 0;
        std::uint32_t model = 0;
        // The watched prop's id (fate evidence), the throwable
        // weapon it came from (0 for a projectile seen after a shot), and
        // since when it has been at rest (a throwable left where it stuck
        // ends its watch there).
        replay::entity::ReplayId id;
        Hash weaponHash = 0;
        ULONGLONG restSince = 0;
    };
    extern std::vector<ProjectileWatch> g_poseV2ProjectileWatch;
    extern std::uint32_t g_poseV2ProjectileFateLogs;
    extern std::uint32_t g_poseV2ProjectileWatchLogs;
    void LogPoseV2ProjectileFate(const ProjectileWatch& watch, const char* fate, Object successor, const char* how);
    const char* PoseV2StuckFate(Object object);
    extern std::vector<int> g_poseV2ProjectilePrevObjects;
    void SearchPoseV2Projectiles();

    // World objects. As a replay editor turns a map object into a replay
    // object once it moves, an object near the recorded actors that moves,
    // turns or breaks becomes a prop with its world track, and the clip keeps
    // how it stood when the recording started; a new object of its model
    // next to it (a broken-off piece, same model as its parent) becomes a
    // prop too. Which fragment groups broke is not known without a detour:
    // the stand-in shows the object whole until its prop takes over.
    struct WorldSnap
    {
        std::uint32_t model = 0;
        Vector3 position{};
        Vector3 rotation{};
        bool broken = false;
        bool taken = false;
        // object_check.h: the bones of an object with a
        // skeleton (the camp's donation box and ledger), watched so the
        // object is recorded, with its pose, when it animates in place.
        std::uint32_t bones = 0;
        std::vector<float> locals;
        std::uint32_t nextLocalsMs = 0;
        // objcheck::SkeletonWatch: whether the bones were read, and when a
        // reread near a recorded actor is next due (a skeleton that appears is
        // an animation: the mission chest's lid).
        replay::objcheck::SkeletonWatch skeleton = replay::objcheck::SkeletonWatch::Unread;
        std::uint32_t nextSkeletonMs = 0;
        // objcheck::NearWatch: within reach of a recorded actor at the last
        // scan (its bones then read every tick, not four times a second), and
        // away from its rest pose at the snapshot (a drawer left open).
        bool watchedNear = false;
        bool openAtStart = false;
    };
    extern std::unordered_map<Object, WorldSnap> g_worldSnapshot;
    // The snapshot objects with bones within reach of a recorded actor, read
    // every tick (SamplePoseV2NearWorldObjects), with that actor.
    struct NearWorldObject
    {
        Object object = 0;
        replay::entity::ReplayId actor{};
        Entity actorHandle = 0;
    };
    extern std::vector<NearWorldObject> g_nearWorldObjects;
    extern std::uint32_t g_nearWorldTaken;
    extern std::uint32_t g_openAtStartTaken;
    struct WorldTaken
    {
        Object object = 0;
        std::uint32_t model = 0;
    };
    extern std::vector<WorldTaken> g_worldTaken;
    extern std::uint32_t g_worldAnimatedRecorded;
    extern std::uint32_t g_worldAnimatedWatched;
    extern std::uint32_t g_worldLateMapObjects;
    extern std::unordered_set<Object> g_worldLateJoinDecided;

    // object_check.h: the scenery of the recording, the
    // objects of the scene that are not of the map. Live entries are
    // watched for leaving; late ones settle 1 s (an object still moving is
    // left to the paths that follow moving objects).
    struct SceneryLive
    {
        Object object = 0;
        std::size_t index = 0;
    };
    extern std::vector<SceneryLive> g_sceneryLive;
    extern std::unordered_map<Object, std::size_t> g_sceneryOf;
    struct SceneryPendingObject
    {
        std::uint32_t firstMs = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t owner = replay::objcheck::kOwnerUnknown;
        bool mission = false;
        bool pickup = false;
        std::uint32_t appearMs = 0; // the first sight when it was seen far and stood there since
    };
    extern std::unordered_map<Object, SceneryPendingObject> g_sceneryPending;
    extern std::unordered_set<Object> g_scenerySeen;
    // objcheck::kSceneryFarRecheckMs: the listed objects out of the scenery
    // radius, looked at again every second until they come within it (marked
    // seen only then): their first sight and where they stood.
    struct SceneryFar
    {
        std::uint32_t firstMs = 0;
        std::uint32_t nextMs = 0;
        float position[3]{};
    };
    extern std::unordered_map<Object, SceneryFar> g_sceneryFar;
    extern std::uint32_t g_lateSceneryLogs;
    extern std::uint32_t g_sceneryNextScanMs;
    // objcheck::LootWatchable: the free objects that are not scenery and that a
    // recorded ped came within reach of, watched four times a second for a take
    // (hidden or removed). Where it stood when the watch began (it does not
    // move: a moved object is the world objects' prop), its owner then and when
    // it entered the world.
    struct LootWatch
    {
        std::uint32_t model = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t owner = replay::objcheck::kOwnerUnknown;
        bool mission = false;
        std::uint32_t appearMs = 0;
    };
    extern std::unordered_map<Object, LootWatch> g_lootWatch;
    // Removals waiting kReinstanceMs for the same object made again before they
    // are a take.
    struct LootGonePending
    {
        Object object = 0;
        LootWatch watch;
        std::uint32_t goneMs = 0;
    };
    extern std::vector<LootGonePending> g_lootGonePending;
    extern std::unordered_map<Object, std::uint32_t> g_lootFirstSeenMs;
    extern std::unordered_set<Object> g_lootDecided;
    constexpr std::size_t kLootSeenCap = 16384;                  // a long buffer recording starts these over
    extern std::uint32_t g_lootLogs;
    constexpr std::uint32_t kLootLogCap = 64;
    extern std::uint32_t g_mapOwner;

    // The map data objects of the snapshot, nearest first, waiting for the
    // clip's IPL spheres (the world state's recording starts after the
    // snapshot): ResolvePoseV2MapDressing keeps the ones inside a sphere of an
    // IPL on.
    struct MapDressingPending
    {
        Object object = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t owner = replay::objcheck::kOwnerUnknown;
    };
    extern std::vector<MapDressingPending> g_mapDressingPending;
    extern std::uint32_t g_snapshotRules[replay::objcheck::kSceneryRuleCount];
    // objcheck::DressingRemovalOf: map dressing removed within reach of a
    // recorded ped, waiting kReinstanceMs for the same object made again
    // before its removal is the take.
    struct DressingGonePending
    {
        std::size_t index = 0; // into Ctx().clip.scenery
        std::uint32_t goneMs = 0;
    };
    extern std::vector<DressingGonePending> g_dressingGonePending;

    // object_check.h: the children attached to a live scenery
    // entry, watched until they leave it (gone, attached elsewhere, taken by
    // another path).
    struct SceneryChildLive
    {
        Object object = 0;
        Object parent = 0;
        std::size_t index = 0; // into Ctx().clip.sceneryChildren
    };
    extern std::vector<SceneryChildLive> g_sceneryChildLive;
    extern std::unordered_set<Object> g_sceneryChildObjects;
    extern std::unordered_set<std::size_t> g_sceneryChildScanned;
    bool PoseV2MapObject(Object object);
    void LogPoseV2PickupEntry(const char* why);
    replay::entity::ReplayId KnownObjectId(Object object);
    bool IsPedGoreModel(std::uint32_t model);
    bool PoseV2ClipWorldModel(std::uint32_t model);
    extern std::uint32_t g_goreWorldPiecesRefused;
    extern std::uint32_t g_goreWorldModelRefused;
    extern std::uint32_t g_goreRefrozen;

    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out);
    replay::objcheck::RestState PoseV2RestOf(Object object, const std::vector<float>& locals, std::uint32_t bones,
        replay::objcheck::RestMatch& match);
    void NotePoseV2Rest(replay::objcheck::RestState rest);
    const replay::recorder::RecordedEntity* PoseV2VehicleParent(Entity handle);
    extern std::uint32_t g_worldObjectsRecorded;
    extern std::uint32_t g_worldPiecesRecorded;
    extern bool g_fragReady;
    void WarmUpFrag();
    extern std::uint64_t g_fragCursor;
    extern std::uint32_t g_fragBreaks;
    extern std::uint32_t g_fragBreaksMatched;
    extern std::uint32_t g_fragLogged;
    extern std::vector<Object> g_fragBrokenObjects;
    extern std::vector<std::pair<Object, replay::frag::BreakCall>> g_fragPending;
    // Pieces broken off a recorded vehicle, waiting for their
    // object (FlushPoseV2VehiclePieces).
    struct PendingVehiclePiece
    {
        std::uint64_t entity = 0;
        std::uint64_t inst = 0;
        replay::entity::ReplayId vehicle;
        Entity vehicleHandle = 0;
        std::uint16_t group = 0;
        ULONGLONG deadline = 0;
    };
    extern std::vector<PendingVehiclePiece> g_vehiclePiecesPending;
    extern std::uint32_t g_vehiclePiecesRecorded;
    extern std::uint32_t g_vehiclePiecesLost;
    extern std::unordered_set<std::uint32_t> g_vehiclePartsUrgent;
    struct PendingWheelPiece
    {
        replay::entity::ReplayId vehicle;
        Entity vehicleHandle = 0;
        int wheel = 0;
        int slot = 0;
        std::uint32_t offMs = 0;
        ULONGLONG deadline = 0;
    };
    extern std::vector<PendingWheelPiece> g_wheelPiecesPending;
    extern std::uint32_t g_wheelPieceLogs;
    extern int g_wheelLayout;
    // frag_piece.h: the pieces the object breaks made (PartsBrokeOff with a new
    // inst, vehicles aside), kept until a prop is registered for the piece's
    // object; the last kFragPieceCalls only.
    struct FragPieceCall
    {
        std::uint64_t entity = 0;
        std::uint64_t inst = 0;
        std::vector<std::uint16_t> groups;
        // The object it broke off (its entity), recorded or on a recorded
        // entity (a loose wheel, a vehicle's wheel): the piece is recorded at
        // any distance from the peds (PoseV2PieceOfRecorded).
        std::uint64_t parentEntity = 0;
        bool parentRecorded = false;
    };
    extern std::vector<FragPieceCall> g_fragPieceCalls;
    extern std::uint32_t g_fragPiecesRecorded;
    extern std::uint32_t g_fragPiecesFromCache;
    extern std::uint32_t g_fragPieceLogs;
    bool PoseV2PieceOfRecorded(Object object);
    void FlushPoseV2FragBreaks();

    // The pieces of the breaks above become props (VehiclePiece)
    // once their object is in the listing; a piece not found within 1 s is
    // counted as lost.
    void FlushPoseV2WheelPieces();
    const char* ReadPoseV2WheelObject(Entity vehicle, int slot, replay::vehicle_pieces::WheelRead& out);
    void NotePoseV2WheelOff(replay::entity::ReplayId vehicle, Entity vehicleHandle, int wheel, int slot);
    bool PoseV2MapOwned(Object object);
    bool JoinPoseV2LateMapObject(Object object, const Vector3& at);
    void CollectPoseV2FragBreaks();
    float Distance2(const Vector3& a, const Vector3& b);
    float AngleDelta(float a, float b);

    // The scan walks the object list once. The weapon points, the attached
    // objects and the world objects each walked it with DOES_ENTITY_EXIST and
    // GET_ENTITY_ATTACHED_TO or IS_ENTITY_ATTACHED per object (0.63 and 0.60 ms
    // per scan in town); now one GET_ENTITY_ATTACHED_TO per object, existence
    // from memory.
    struct PoseV2ScanObject
    {
        Object object = 0;
        Entity parent = 0; // GET_ENTITY_ATTACHED_TO
    };
    extern std::vector<PoseV2ScanObject> g_poseV2ScanObjects;
    extern int g_poseV2ScanListed;
    // Weapon points: the 32 GET_CURRENT_PED_WEAPON_ENTITY_INDEX of a
    // ped only when something may have moved: the weapon in hand changed, an
    // object attached to it is not one of its known weapons, one of its point
    // weapons is no longer attached to it, or a second went by (staggered).
    struct PoseV2PointScan
    {
        Hash held = 0;
        std::uint32_t nextFullMs = 0;
        bool scanned = false;
    };
    extern std::unordered_map<Entity, PoseV2PointScan> g_poseV2PointScans;
    extern std::uint64_t g_poseV2PointScansFull;
    extern std::uint64_t g_poseV2PointScansSkipped;
    constexpr std::uint32_t kPoseV2PointRefreshMs = 1000;
    void CollectPoseV2ScanObjects();
    void TakePoseV2WorldObject(Object object, WorldSnap& was, replay::entity::ReplayId actor, Entity actorHandle,
        const char* how, bool animated, std::uint32_t nowMs);
    void SamplePoseV2NearWorldObjects();
    struct PoseV2SpawnWatch
    {
        std::uint32_t firstMs = 0;
        float position[3]{};
        float rotation[3]{};
        Entity ownerHandle = 0;
        std::uint32_t model = 0;
        std::uint32_t bones = 0;
        bool attachedNoParent = false;
        std::vector<float> locals; // the first sight's locals (bones >= 2)
        // The late scenery decided (once), and whether this
        // watch made the entry.
        bool sceneryDecided = false;
        bool scenery = false;
        // Watch scans that read no skeleton; a skeleton appearing after two of
        // them is an animation (the engine builds it when the object animates,
        // object_check.h).
        std::uint32_t absentReads = 0;
        // The nearest recorded ped at the last scan (-1: none), and whether the
        // watch came from the recheck.
        float nearestM = -1.0f;
        bool rewatched = false;
    };
    extern std::unordered_map<Object, PoseV2SpawnWatch> g_spawnWatch;
    extern std::unordered_set<Object> g_spawnSeen;
    // The objects refused for distance or a full watch, those whose watch ended
    // still or left it, and those attached to an entity that is not recorded,
    // looked at again every kRecheckMs while listed (seen, not decided). Where
    // it stood when it went in, and for an expired one the ped of that watch
    // (it is watched again only once re-armed).
    struct PoseV2SpawnRecheck
    {
        std::uint32_t nextMs = 0;
        float position[3]{};
        Entity expiredBy = 0;
        bool armed = true;
        bool elsewhere = false; // counted once as attached elsewhere
    };
    extern std::unordered_map<Object, PoseV2SpawnRecheck> g_spawnRecheck;
    extern std::uint32_t g_spawnLogs;
    constexpr std::uint32_t kSpawnLogCap = 64;
    extern std::uint32_t g_spawnLateLogs;
    constexpr std::uint32_t kSpawnLateLogCap = 48;
    void LogPoseV2LateScenery(const char* why, Object object, std::uint32_t owner, bool mission, std::uint32_t nowMs);
    float PoseV2NearestActorM(const float at[3]);
    float PoseV2PlayerM(const float at[3]);
    void UpdatePoseV2Props();
    extern std::uint32_t g_poseV2AttachedScans;
    extern std::vector<replay::recorder::RecordedEntity> g_vehicleParents;
    extern std::unordered_set<Object> g_vehicleChildRefused;
    extern std::uint32_t g_vehicleChildRefusedResetMs;
    void CollectPoseV2VehicleParents();
    bool PoseV2PointsNeeded(const replay::recorder::RecordedEntity& owner, Hash held, std::uint32_t nowMs,
        const std::vector<std::pair<Entity, Object>>& attachedByParent);
    void ScanPoseV2Weapons();
    void ApplyPoseV2WeaponComponents(const replay::playback::Puppet& puppet);

    // Weapon puppets ride on their parent puppet as an entity attachment,
    // like the recorded weapon (attached to its ped): the engine updates a
    // weapon's straps only for an object attached to a ped (the strap
    // update reads the object's attach parent and requires a ped), so a
    // free object never draws them. The recorded parent-relative transform
    // is the attach offset, set every frame. The attach native takes Euler
    // degrees; the engine itself converts the rotation (a hidden helper
    // object gets the quaternion and returns its rotation in the same
    // order), so no Euler convention is assumed.
    constexpr int kPoseV2AttachRotationOrder = 2;
    extern Object g_poseV2AttachHelper;
    bool PoseV2EngineEuler(const replay::pose::Quat& rotation, Hash model, Vector3& euler);
    void DeletePoseV2AttachHelper();

    // Straps: the recorded state at the playhead, and the update request
    // the owner ped's weapon manager would make. Every frame, for every
    // attach path.
    // The recorded weapon look at the playhead, set when the
    // state changes (both directions).
    // weapon_look_core.h: the whole look at the playhead, every frame for every
    // weapon puppet (attached, loose or waiting for its parent), set when the
    // object disagrees:
    // the state's components (missing given, extra removed: the engine list of
    // the puppet when the state carries the engine's), the rust and sixth wear
    // channel (rung 3, ini WeaponWearWrite), then degradation, damage, dirt and
    // soot (natives, the order of the game's own cleaning script). The puppet
    // is read back kCheckDelayMs later and then every kRecheckMs:
    // POSE_V2_WEAPON_LOOK_CHECK, and the wear set again when the engine moved
    // it (it adds soot when the replay fires the puppet weapon).
    struct WeaponLookVerdict
    {
        bool checked = false;
        bool components = false;
        bool wear = false;
    };
    extern std::map<std::uint32_t, WeaponLookVerdict> g_weaponLookVerdicts;
    extern bool g_weaponLookEnginePlay;
    extern bool g_weaponLookEnginePlayChecked;
    void ApplyPoseV2WeaponLook(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    extern std::unordered_set<std::uint32_t> g_vehicleChildRecords;
    extern std::unordered_map<std::uint32_t, ULONGLONG> g_engineCopyNextCheck;
    extern std::unordered_set<Object> g_engineCopiesHidden;
    extern std::uint32_t g_engineCopyChecks;
    void HidePoseV2EngineCopy(const replay::playback::Puppet& puppet, Entity parentClone, std::uint32_t& checks, ULONGLONG now);
    extern int g_modelInfoLayout;
    struct VehicleWheelModelsRead
    {
        bool read = false;
        replay::vehicle_pieces::WheelModels models;
    };
    extern std::unordered_map<std::uint32_t, VehicleWheelModelsRead> g_vehicleWheelModels;
    extern std::unordered_map<std::uint32_t, std::uint16_t> g_wheelChildBits;
    extern std::unordered_set<std::uint32_t> g_wheelChildLogged;
    // How long a puppet has waited for something (its first try), restarted
    // when it was not asked for a while: a puppet deleted while it waited (a
    // seek) starts again with its full wait when it comes back.
    struct WaitClock
    {
        ULONGLONG since = 0;
        ULONGLONG last = 0;
    };
    ULONGLONG WaitedMs(std::unordered_map<std::uint32_t, WaitClock>& clocks, std::uint32_t key, ULONGLONG now);

    struct LeftWheel
    {
        std::uint32_t leftMs = 0;
        std::uint16_t bits = 0;
        std::uint32_t prop = 0; // the loose prop the wheel's object went on as (packed id)
        std::uint32_t child = 0; // the wheel child on the vehicle it was before (packed id)
    };
    extern std::unordered_map<std::uint32_t, std::vector<LeftWheel>> g_leftWheels;
    extern std::map<std::pair<std::uint32_t, std::uint32_t>, Entity> g_leftWheelsDetached;
    extern std::set<std::pair<Entity, std::uint32_t>> g_leftWheelMissLogged;
    extern std::unordered_map<Entity, ULONGLONG> g_wheelsPendingSince;
    struct WheelCountSeen
    {
        std::int32_t count = -1;
        ULONGLONG since = 0;
    };
    extern std::unordered_map<Entity, WheelCountSeen> g_wheelCountSeen;
    extern std::unordered_map<std::uint32_t, std::int32_t> g_modelWheelCount;
    constexpr ULONGLONG kWheelCountSteadyMs = 100;
    extern std::unordered_set<Entity> g_wheelsMade;
    constexpr ULONGLONG kWheelsPendingMaxMs = 1000;
    // Fragment pieces broken off a wheel object of a vehicle (frag piece packed id
    // -> the vehicle and the piece's create time).
    struct FragPieceParent
    {
        replay::entity::ReplayId vehicle;
        std::uint32_t createMs = 0;
    };
    extern std::unordered_map<std::uint32_t, FragPieceParent> g_fragPieceParents;
    extern std::unordered_map<std::uint32_t, std::vector<std::uint16_t>> g_fragPieceGroupsOf;
    const replay::vehicle_pieces::WheelModels* PoseV2VehicleWheelModels(const replay::playback::Puppet& vehicle);
    std::uint16_t PoseV2WheelChildBits(std::uint32_t child, std::uint32_t childModel,
        const replay::vehicle_pieces::WheelModels& models);
    void AttachPoseV2Weapon(replay::playback::Puppet& puppet, const replay::playback::Puppet& parentPuppet,
        const replay::pose::WorldTransform& relative, const replay::pose::WorldTransform& world,
        std::uint32_t timeMs);
    void PublishPoseV2StrapOwned();
    extern replay::pose::PoseSample g_poseV2ObjectSample;
    extern std::uint32_t g_poseV2ObjectPlayLogs;
    extern std::set<std::uint32_t> g_poseV2ObjectPlayNoted;
    // Replay objects whose skeleton was asked for (clone << 32 |
    // record id), once each (object_pose.h ShouldAskSkeleton).
    // Asked again later when it is gone (ShouldAskSkeletonAgain):
    // how many asks, and the tick of the next one allowed.
    struct PoseV2SkeletonAsk
    {
        std::uint32_t asks = 0;
        std::uint64_t nextMs = 0;
    };
    extern std::map<std::uint64_t, PoseV2SkeletonAsk> g_poseV2ObjectSkeletonAsked;
    extern bool g_objectDrawnCheck;
    extern std::unordered_map<std::uint32_t, replay::tpose::TrackSignature> g_stillSignatures;
    extern std::uint32_t g_stillLines;
    extern std::uint32_t g_drawnLines;
    extern std::vector<replay::pose::BoneQts> g_drawnExpected;
    extern std::vector<replay::pose::BoneQts> g_drawnLocals;
    const replay::tpose::TrackSignature& StillSignature(std::uint32_t recordId);
    void NoteDrawn(replay::playback::Puppet& puppet, const char* mode, const replay::pose::BoneQts* expected,
        const replay::pose::BoneQts* drawn, std::uint32_t bones, bool withObjects, std::uint8_t* skeleton, int slot,
        std::uint32_t timeMs);
    void CheckStillDrawn(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved,
        std::uint32_t recordId, std::uint32_t timeMs);

    void PlayPoseV2ObjectPoseBody(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void PlayPoseV2ObjectPose(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void LogWeaponDrawn(std::uint32_t recordId, const WeaponDrawn& drawn, const char* when);

    // Defined with the vehicle pieces.
    replay::vehicle_pieces::WheelCopy PoseV2WheelCopy(const replay::playback::Puppet& puppet,
        const replay::pose::WorldTransform& at, Object& out, ULONGLONG now, std::uint32_t timeMs, bool noWait = false);
    void UpdatePoseV2Weapons(std::uint32_t timeMs, std::uint32_t& creates);
    extern std::uint32_t g_vehiclePieceLogs;
    extern std::map<std::pair<std::uint32_t, std::uint16_t>, Object> g_vehicleFragPieces;

    // The wheel objects the vehicle puppets gave (_BREAK_OFF_VEHICLE_WHEEL),
    // kept while the playback has a use for them: free (hidden) or the object
    // of a puppet (`holder`, its packed id). A wheel piece takes the wheel of
    // its own vehicle and index; a plain object puppet of a wheel model (the
    // carried wheel of 134: a held object, then a dropped prop) takes a free
    // one of its model instead of a fresh CREATE_OBJECT, which draws without
    // its vehicle (yellow); a puppet's wheel object is free again at the end of
    // its span (ReturnWheelObject). Free ones go when their vehicle puppet is
    // built again and at the playback's end.
    struct PoseV2WheelObject
    {
        Object object = 0;
        replay::entity::ReplayId vehicle;
        int wheel = -1;
        std::uint32_t model = 0;
        std::uint32_t holder = 0; // 0: free (hidden)
        bool pieced = false;      // fragment pieces broke off it (PoseV2FragPieceFromParent)
        std::uint32_t forProp = 0; // the loose wheel it came off for (LeftVehicleWheels), 0: any
    };
    extern std::vector<PoseV2WheelObject> g_wheelObjects;
    extern std::uint32_t g_wheelPlayheadMs;
    bool CreatePoseV2VehiclePiece(replay::playback::Puppet& puppet, bool sampled,
        const replay::entity::TransformSample& sample, std::uint32_t timeMs, std::uint32_t& creates);
    extern std::unordered_map<std::uint32_t, Object> g_fragPieceShaped;
    extern std::unordered_map<std::uint32_t, ULONGLONG> g_fragPieceWaitUntil;
    const replay::FragPiece* FragPieceOf(replay::entity::ReplayId id);
    constexpr std::uint32_t kMaxPieceCreatesPerFrame = 24;
    extern std::unordered_set<std::uint32_t> g_fragOrderWaited;
    extern bool g_propMadeThisPass;
    extern const std::unordered_set<std::uint32_t>* g_propRetryOnly;
    int PoseV2FragPieceFromParent(replay::playback::Puppet& puppet, const replay::pose::WorldTransform& at, ULONGLONG now,
        std::uint32_t timeMs, Object& out);
    bool ShapePoseV2FragPiece(replay::playback::Puppet& puppet, bool sampled,
        const replay::entity::TransformSample& sample, std::uint32_t timeMs);
    extern std::vector<std::uint32_t> g_hatModels;
    void EndPoseV2HatDresser(bool forget);
    void UpdatePoseV2Props(std::uint32_t timeMs, std::uint32_t& creates);
    void ScanPoseV2HandAmmo();
        extern std::set<std::uint32_t> g_goreModels;
    extern std::map<std::uint32_t, std::size_t> g_fragApplied;
    extern std::map<std::uint32_t, Object> g_fragPuppet;
    extern std::uint32_t g_fragBreaksPlayed;
    extern std::uint32_t g_fragFixes;
    extern std::uint32_t g_fragUnmapped;
    extern int g_fragLastError;
    extern std::uint32_t g_fragMaxPlayheadMs;

    // World objects in playback: a stand-in of the model where
    // the object stood at the start, shown while its prop is not alive (before
    // it moved, and after a scrub back); the original, when this session still
    // has it, is hidden for the playback and given back at the end.
    // object_check.h: the same stand-in for the scenery
    // (the objects of the scene that are not of the map, alive from appearMs
    // to goneMs), and the original's hide is deferred: the closed theater's
    // clear turns the map objects into dummies that stream back after the
    // playback started, and a camp's own props are its script's mission
    // entities; both would be left drawn next to the replay's copy. The search
    // excludes only the replay's own objects (stand-ins and puppets), every
    // 500 ms for the first 10 s of the playback and every 3 s after it, by one
    // walk of the object list.
    struct ObjectStand
    {
        std::uint32_t model = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t appearMs = 0;
        std::uint32_t goneMs = replay::objcheck::kNeverGone;
        replay::entity::ReplayId prop; // the prop that takes over (invalid: none)
        bool scenery = false;
        // objcheck::VisibleAt: the recorded visibility
        // (a mission's hidden object is not drawn).
        bool visibleWhenRecorded = true;
        std::vector<std::uint32_t> visibleFlipsMs;
        bool loot = false; // objcheck::IsLootEntry: logged when it shows and hides
        // hat_look.h: a hat's recorded layer (its prop's create for a world
        // object, SCHT for scenery), how it is built, and a hat model without a
        // look that is not drawn.
        std::vector<replay::entity::MetaPedAsset> hatLook;
        replay::hatlook::Rebuild hatRebuild = replay::hatlook::Rebuild::Model;
        bool hatHidden = false;
        Object standIn = 0;
        Object original = 0;
        bool originalWasVisible = true;
        bool shown = false;
        bool failed = false;
        bool missingLogged = false;
        const char* failWhy = "model_timeout"; // why it failed (scenery_missing): or model_invalid, rope_model
        std::uint64_t loadingSince = 0;
        // objcheck::ChooseChildHost: a scenery child (parentStand >= 0) is
        // attached to the stand at that index: its stand-in, or the prop that
        // took it over (the frame, parent bone and offsets, only on a posed
        // prop; else the transform in the parent's frame at the root). `host`
        // is what it is attached to now.
        int parentStand = -1;
        bool hasFrame = false;
        std::int16_t frameBone = -1;
        float framePosition[3]{};
        float frameRotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float relativePosition[3]{};
        float relativeRotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        Object host = 0;
        bool hostByFrame = false;
        // Set every update for the children: the ready puppet of `prop`, and
        // whether it took over and plays a pose.
        Object propClone = 0;
        bool propTookOver = false;
        bool propPosed = false;
        // The scenery entry's burn level track (index into
        // Recording::sceneryBurnLevels, -1 none).
        int burnTrack = -1;
        // The scenery entry's look track (index
        // into Recording::sceneryObjectLooks, -1 none).
        int lookTrack = -1;
        // objcheck::HandoffPropReady: the consecutive ticks the prop puppet has
        // been Ready and posed (the stand-in is hidden a tick after the prop's
        // first published pose), whether the prop has a pose track (-1 not
        // looked up yet), and whether this handoff was logged.
        std::uint32_t propReadyTicks = 0;
        std::uint32_t propPosedTicks = 0;
        std::int8_t propHasPose = -1;
        bool handoffLogged = false;
        // objcheck::MapDressingVerdict a map dressing entry, the IPL that held
        // it, the verdict (built only when the map does not have it) and the
        // live original left to the map.
        bool mapDressing = false;
        std::uint32_t mapIpl = 0;
        replay::objcheck::MapDressing mapVerdict = replay::objcheck::MapDressing::Wait;
        Object mapOriginal = 0;
    };
    extern std::vector<ObjectStand> g_objectStands;
    void EndPoseV2WorldStandIns();
    void BeginPoseV2WorldStandIns();
    Object PoseV2ChildHost(const ObjectStand& parent, replay::objcheck::ChildHost& kind);
    void UpdatePoseV2WorldStandIns(std::uint32_t timeMs);
    void WarnPoseV2WeaponPlain();
}
