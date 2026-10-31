// replay_playback_internal.h: the state and helpers the replay_playback*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "replay_playback.h"
#include "camera_bones.h" // CameraRecordTime
#include "object_check.h"
#include "guarded_memory.h"
#include "tpose_check.h"

#include "burn_level.h"
#include "entity_appearance.h"
#include "hat_look.h" // the knock wait
#include "metaped_reader.h"
#include "anim_lod_hook.h" // the puppets' anim LOD (RecordPedAnimLod0)
#include "game_sig.h"
#include "shader_vars_replay.h" // the horses' muscles (MuscleReplay)
#include "entity_pools.h"
#include "log_format.h"
#include "log_policy.h"
#include "mount_placement.h"
#include "playback_cache.h"
#include "pose_capture.h"
#include "pose_jerk.h"
#include "puppet_collision.h"
#include "pose_playback.h"
#include "pose_segments.h"
#include "profiler.h"
#include "entity_cache.h"
#include "replay_events.h"
#include "fx_observer.h" // PlaceScope
#include "fx_motion_rule.h" // StepMetres
#include "editor_keys.h" // the keys a message names
#include "entity_selection.h"
#include "replay_markers.h"
#include "replay_objects.h"
#include "object_look.h"
#include "replay_trains.h"
#include "vehicle_doors_engine.h"
#include "rope_observer.h"
#include "reins_guard.h"
#include "rope_models.h"   // a rope's entity is the rope track's
#include "rope_world.h"
#include "world_landing.h"
#include "puppet_blink.h"
#include "puppet_watch.h"
#include "player_hold.h"
#include "seek_settle.h"
#include "skeleton_map.h"
#include "skeleton_signature.h"
#include "skeleton_resolve.h"
#include "engine_hud.h"
#include "frag_observer.h"
#include "vegetation_speed.h"
#include "audio_observer.h"
#include "world_theatre.h" // kScriptHookThreadHash: the other ASIs' threads

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace replay::playback
{
        Context& Ctx();
    extern std::vector<Hash> g_poseV2RequestedModels;
    extern std::vector<Hash> g_poseV2RequestedWeapons;
    extern std::vector<std::uint32_t> g_poseV2WaitSegmentLogged;
    bool PoseV2NormalPlay();
    extern std::vector<replay::entity::CreatePacket> g_poseV2Alive;
    extern bool g_poseV2CheapScrub;
    extern replay::playback::SettleState g_poseV2SettleState;
    extern replay::playback::Counters g_poseV2Counters;
    extern std::uint32_t g_poseV2UnposedHideLogs;
    extern replay::watch::UnposedTracker g_poseV2Unposed;
    extern bool g_poseV2OverrideInUse[replay::pose::capture::kMaxOverrides];
    extern std::uint32_t g_poseV2ScorchPlayLogs;
    extern std::uint32_t g_poseV2AppearancePlayLogs;
    // The metaped look at the dressing.
    struct PoseV2MetapedPlayStats
    {
        std::uint64_t dressed = 0;      // bodies that left Dressing
        std::uint64_t withState = 0;    // ... with a recorded metaped state
        std::uint64_t exact = 0;        // ... whose expressions read back as recorded
        std::uint64_t notExact = 0;
        std::uint64_t unverified = 0;   // the puppet could not be read
        std::uint64_t applies = 0;      // Apply calls (dressing, segment, change)
        std::uint64_t expressionsSet = 0;
        std::uint64_t retries = 0;
        std::uint64_t notRecorded = 0;  // appearance_not_recorded
        std::uint64_t lookahead = 0;    // dressed from a later segment
    };
    extern PoseV2MetapedPlayStats g_poseV2MetapedPlay;
    // Bodies dressed by shop items or layers (metaped_state.h DressPath), why
    // the layers, and the read back of the shop items after the dress
    // (POSE_V2_PLAYBACK_SHOP_ITEMS, _SUMMARY).
    struct PoseV2ShopPlayStats
    {
        std::uint64_t dressShop = 0;      // DressPoseV2Body calls by shop items
        std::uint64_t dressLayers = 0;    // ... by layers
        std::uint64_t whyNoState = 0;
        std::uint64_t whyNoShopItems = 0;
        std::uint64_t whyPrintDiffers = 0;
        std::uint64_t whyShopRefused = 0;
        std::uint64_t whyNoLayers = 0;
        std::uint64_t checks = 0;         // read backs (dressing and shown puppets)
        std::uint64_t checksShown = 0;    // ... of a dress on a shown puppet
        std::uint64_t layersExactAfterShop = 0;
        std::uint64_t layersNotExactAfterShop = 0;
        std::uint64_t fallbacks = 0;      // shop path refused: dressed again by layers
        std::uint64_t itemsRecorded = 0;
        std::uint64_t matched = 0;
        std::uint64_t stateDiffer = 0;
        std::uint64_t missing = 0;
        std::uint64_t extra = 0;
        std::uint64_t exactBodies = 0;    // checks whose shop items read back as recorded
    };
    extern PoseV2ShopPlayStats g_poseV2ShopPlay;
    struct PoseV2SwitchTotals
    {
        std::uint64_t switches = 0, rearmedInTick = 0, released = 0, holeTicks = 0, worstHole = 0;
        std::uint64_t kept = 0, held = 0, holdTimeouts = 0, rearmFailed = 0;
        // prefix_hold.h: holds begun, verified, given up (differ, unread,
        // timeout) and ended by a record fitting the skeleton again.
        std::uint64_t prefixHolds = 0, prefixVerified = 0, prefixReleased = 0, prefixEnded = 0;
    };
    extern PoseV2SwitchTotals g_poseV2Switch;
    void LogPoseV2SwitchSummary();
    struct NoRecordTotals
    {
        std::uint64_t mapNow = 0, mapRefused = 0, retries = 0, sameAnswer = 0;
        std::uint64_t exactAfter = 0, diffs = 0;
    };
    extern NoRecordTotals g_noRecord;
    extern std::uint32_t g_poseV2WindowPlayLogs;
    extern std::uint32_t g_poseV2LookVerifyLogs;
    extern std::uint32_t g_poseV2PartPlayLogs;
    extern std::unordered_map<std::uint32_t, std::uint16_t> g_poseV2PartKeptLogged;
    extern std::uint32_t g_poseV2VehicleFragResults[replay::frag::kVehicleGroupResults];
    extern std::uint32_t g_poseV2VehicleFragRebuilds;
    extern std::uint32_t g_poseV2VehicleFragLogs;
    extern std::uint32_t g_poseV2BurntPlayLogs;
    extern std::uint32_t g_poseV2BurnLevelPlayLogs;
    extern std::uint64_t g_poseV2BurnLevelWrites;
    extern std::uint64_t g_poseV2BurnLevelEngineChanged;
    extern std::uint32_t g_poseV2FireCharPlayLogs;
    extern std::uint64_t g_poseV2FireCharWrites;
    extern std::uint64_t g_poseV2FireCharRemoves;
    extern std::uint64_t g_poseV2FireCharCreated;
    extern std::uint64_t g_poseV2FireCharCreateFailed;
    extern std::uint64_t g_poseV2FireCharShapeDiffers;
    extern replay::lod::Config g_poseV2LodConfig;
    void PreparePoseV2HatFix(replay::playback::Puppet& puppet, std::uint32_t recordId, std::uint32_t timeMs,
        const replay::pose::PoseSample& sample);
    void FixPoseV2Hat(replay::playback::Puppet& puppet, replay::pose::PoseSample& sample);
    extern replay::lod::Summary g_poseV2LodSummary;
    extern replay::jerk::MountPairs g_poseV2Mounts;
    extern std::uint32_t g_poseV2PlayerPacked;
    bool PoseV2RecordedVisible(std::uint32_t packed, std::uint32_t timeMs);
    void LogPoseV2TPoseSummary();
    extern replay::jerk::Detector g_poseV2Jerk;
    extern std::vector<replay::jerk::Finding> g_poseV2JerkFindings;
    extern std::uint32_t g_poseV2JerkLogs;
    extern std::uint32_t g_poseV2MountLogs;
    extern std::uint64_t g_poseV2CarriedTicks;
    extern std::uint64_t g_poseV2PushedHolds;
    extern std::uint32_t g_poseV2WriteCheckLogs;
    extern std::uint64_t g_poseV2WriteCheckAt[replay::pose::capture::kMaxOverrides];
    extern bool g_poseV2WriteChanged[replay::pose::capture::kMaxOverrides];
    extern replay::pose::PoseSample g_poseV2Sample;
    extern std::uint32_t g_poseV2MappedArms;
    extern std::uint32_t g_poseV2RefusedArms;
    extern std::uint32_t g_poseV2RefusedHiddenLogs;
    // Signature checks of the playback (skeleton_signature.h), by verdict, and
    // what a mismatch led to.
    struct PoseV2SignatureCounts
    {
        std::uint32_t same = 0;
        std::uint32_t differ = 0;
        std::uint32_t unknown = 0;
        std::uint32_t mapped = 0;   // a mismatch armed through the skeleton map
        std::uint32_t refused = 0;  // a mismatch the map refused: hidden
        std::uint32_t rechecks = 0; // armed skeletons compared again
        std::uint32_t rearms = 0;   // a recheck that found a mismatch
        std::uint32_t timeouts = 0; // the puppet's table not read in time (armed unchecked)
        std::uint32_t rebuilt = 0;  // the record's own skeleton rebuilt mid-recording
    };
    extern PoseV2SignatureCounts g_poseV2Signature;
    bool SampleWorldTransform(
        const replay::pose::PoseSample& sample,
        replay::pose::WorldTransform& world);

    // The entity's recorded world transform at timeMs, from the record of
    // its segment there (declared later with the segment helpers).
    bool PoseV2RecordedWorldAt(
        replay::entity::ReplayId id, std::uint32_t timeMs, replay::pose::WorldTransform& world);
    const replay::entity::PoseSegment* PoseV2SegmentAt(replay::entity::ReplayId id, std::uint32_t timeMs);
    extern std::vector<replay::pose::playback::HiddenPedState> g_poseV2HiddenWorld;
    extern ULONGLONG g_poseV2NextWorldScanAt;
    extern std::uint64_t g_poseV2WorldDeleted;
    extern std::uint64_t g_poseV2WorldRehidden;
    extern std::uint64_t g_poseV2WorldOwnerChanges;
    extern std::uint64_t g_poseV2WorldRefrozen;
    extern std::uint64_t g_poseV2WorldRecollided;
    extern replay::landing::Queue<replay::pose::playback::HiddenPedState> g_poseV2Landing;
    extern replay::ropes::WorldRopeHider g_poseV2WorldRopes;
    extern std::uint32_t g_poseV2WorldRopeScans;
    extern std::uint32_t g_poseV2WorldRopeSnapshotFails;
    extern std::uint32_t g_poseV2WorldRopeNoHandle;
    extern std::uint32_t g_poseV2CreateRefused;
    extern std::uint32_t g_poseV2CreateRefusedLogs;
    void ResetPoseV2LiveObjects();
    void HidePoseV2World();
    void DeletePoseV2RecordingLeftovers();
    void RestorePoseV2World();
    void RestorePoseV2WorldRopes();
    void UpdatePoseV2WorldLanding();

    void ResetPoseV2Watch(std::uint32_t startMs);

    // keepSignature keeps the puppet's parent table read for its skeleton (a
    // recheck that found a mismatch arms again through the map with it); every
    // other release forgets it (a new skeleton, a segment switch, a deleted
    // puppet: compared again at the next arm).
    void ReleasePoseV2Override(replay::playback::Puppet& puppet, bool keepSignature = false);
    bool PoseV2LodBody(const replay::playback::Puppet& puppet);
    void ApplyPoseV2Lod(replay::playback::Puppet& puppet);
    void UndoPoseV2Lod(replay::playback::Puppet& puppet);
    void UpdatePoseV2AnimLod();
    void EndPoseV2AnimLod();
    void LogPoseV2AnimSkip(const replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void LogPoseV2LodSummary();
    bool PoseV2RecordSampled(std::uint32_t recordId);
    void SetPoseV2Wear(replay::playback::Puppet& puppet, const replay::entity::Appearance& appearance);
    const replay::entity::Appearance& PoseV2Wear(const replay::playback::Puppet& puppet);
    replay::WantedAppearance PoseV2AppearanceAt(const replay::playback::Puppet& puppet, std::uint32_t timeMs);
    replay::DressAppearance PoseV2DressAt(const replay::playback::Puppet& puppet, std::uint32_t timeMs);
    replay::metaped::Wanted PoseV2MetapedAt(const replay::playback::Puppet& puppet, std::uint32_t timeMs);
    bool ApplyPoseV2Metaped(replay::playback::Puppet& puppet, std::uint32_t timeMs, bool force);
    replay::metaped::DressPath DressPoseV2Body(replay::playback::Puppet& puppet, const replay::entity::Appearance& appearance,
        std::uint32_t timeMs, bool shown);
    bool CheckPoseV2ShopDress(replay::playback::Puppet& puppet, std::uint32_t timeMs, const char* phase,
        const replay::entity::appearance::Match& match);
    void LogPoseV2Dressed(const replay::playback::Puppet& puppet, const replay::entity::appearance::Match& match,
        bool timedOut, std::uint32_t timeMs);
    bool PoseV2RecordHasSuccessor(replay::entity::ReplayId id, std::uint32_t recordId);
    std::uint32_t PoseV2RecordForBones(
        replay::entity::ReplayId id, std::uint32_t timeMs, std::uint32_t bones);
    std::uint32_t PoseV2NeighbourRecord(
        replay::entity::ReplayId id, std::uint32_t timeMs, std::uint32_t bones,
        std::uint32_t& neighbourBones);
    void ReleaseVehicleForRebuild(replay::playback::Puppet& puppet);

    // Arms the override on the puppet's current skeleton and writes the
    // pose at the playhead at once, from the script thread, so the skeleton
    // never renders a frame without it (the finalizer keeps it afterwards).
    // A skeleton with another segment's bones (the engine has not rebuilt
    // it yet, or already has) takes that segment's pose meanwhile.
    bool PoseV2KnockPending(replay::entity::ReplayId owner);
    bool PublishPoseV2Mapped(replay::playback::Puppet& puppet, const replay::pose::PoseSample& sample);
    bool RecheckPoseV2Signature(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void ScheduleSignatureRecheck(replay::playback::Puppet& puppet);
    void LearnPoseV2Categories(const replay::playback::Puppet& puppet, const replay::entity::Appearance& next);
    void StepPoseV2NoRecord(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void ArmPoseV2Puppet(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void EndPoseV2PrefixHold(replay::playback::Puppet& puppet, const char* why, std::uint32_t cloneBones, std::uint32_t timeMs);
    bool HoldPoseV2Prefix(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved,
        std::uint32_t timeMs);
    bool RearmPoseV2InTick(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved,
        std::uint32_t record, std::uint32_t timeMs, const char* why);
    void LogPoseV2HatSummary();
    void RefreshPoseV2Override(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void CheckPoseV2Write(const replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void LogPoseV2Puppet(const replay::playback::Puppet& puppet, const char* status, std::uint32_t timeMs);
    void QueuePoseV2Puppet(const replay::entity::CreatePacket& packet);
    bool AdvancePoseV2Puppet(replay::playback::Puppet& puppet, std::uint32_t timeMs, bool mayCreate);
    void SwitchPoseV2Segment(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    replay::playback::Puppet* FindPoseV2Puppet(replay::entity::ReplayId id);
    void UpdatePoseV2Attachments(std::uint32_t timeMs);
    void PlayPoseV2Ropes(std::uint32_t timeMs);
    void StepPoseV2ShopCheck(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void ApplyPoseV2Appearance(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void ApplyPoseV2Scorched(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    void VerifyPoseV2VehicleLook(std::uint32_t timeMs);
    void PlayPoseV2VehicleWindows(std::uint32_t timeMs);
    void PlayPoseV2VehicleDoors(std::uint32_t timeMs);
    void PlayPoseV2VehicleParts(std::uint32_t timeMs);
    void PlayPoseV2VehicleFrags(std::uint32_t timeMs);
    void PlayPoseV2VehicleBurnt(std::uint32_t timeMs);
    void PlayPoseV2BurnLevel(std::uint32_t timeMs);
    void PlayPoseV2FireChar(std::uint32_t timeMs);
    void UpdatePoseV2SettleState(std::uint32_t timeMs);
    extern bool g_poseV2WatchHeld;
    extern std::vector<std::uint32_t> g_poseV2RecordUnsampled;
    void UpdatePoseV2PuppetWatch(std::uint32_t timeMs);
    bool BeginPoseV2BlinkTick(std::uint32_t timeMs);
    void EndPoseV2BlinkTick();
    void JudgePoseV2BlinkAbsent(const replay::playback::Puppet& puppet, bool alive, std::uint32_t timeMs);
    void JudgePoseV2BlinkReady(replay::playback::Puppet& puppet, bool alive, bool recordedVisible, bool inTrack,
        bool sampled, std::uint32_t poseRecord, bool recordChanged, bool moved, std::uint32_t timeMs);
    void LogPoseV2WatchSummary();
    void UpdatePoseV2Jerks(std::uint32_t timeMs);
    void LogPoseV2JerkSummary();
}
