#pragma once

#include "puppet_lod.h"

// Playback core: the world clear (ambient population deleted and spawn
// suppressed, player's horses and vehicle hidden and restored), puppets
// following the lifecycle track (built from the create packet, dressed,
// armed to the pose override, deleted when not alive), pose segments and
// re-arming when the engine rebuilds a skeleton, attachments, the prepare
// hold at the start, the per-frame playback tick that drives objects, ropes
// and events, and the ground markers. Works on the shared Context and
// provides the PlaybackHooks the subsystems use.

#include "replay_context.h"

namespace replay::playback
{
    void Attach(Context& context);
    void Start();
    void Update();
    void Stop();
    // Gives the world entities hidden by the last playback back as the map
    // collision around each one loads (world_landing.h). Every tick, also
    // after Stop.
    void UpdateWorldLanding();
    // The game restarted its scripts with a playback open. Stop() would call
    // natives on the old world's handles; this empties the puppets, the hidden
    // world, the overrides and every subsystem's playback table without a
    // native on them (counts for SESSION_RESET).
    struct ForgetReport
    {
        bool active = false;
        std::uint32_t puppets = 0;
        std::uint32_t hiddenWorld = 0;
        std::uint32_t models = 0;
        std::uint32_t objects = 0; // stand-ins and the attach helper
        std::uint32_t events = 0;  // fx, decals, fires and sound slots forgotten or stopped
        std::uint32_t trains = 0;  // hidden or halted world trains and train puppets
    };
    ForgetReport ForgetAfterRestart();
    void DrawMarkers();
    // Normal play: not paused, not preparing, not scrubbing.
    bool NormalPlay();
    // Entities of the first frame still being built: the transport holds.
    bool Preparing();
    // Where the snow and mud trail map is centred (the puppet the
    // override points at); false when none (the map is on the player).
    bool TrailFocus(float& x, float& y, float& z);
    // The mount puppet a rider puppet sits on now (packed
    // ReplayIds; 0 for none), from the rider and mount pairs of the jerk
    // detector (pose_jerk.h MountPairs, updated every tick).
    std::uint32_t MountOf(std::uint32_t packedRider);

    // The seek settle's side (seek_settle.h, script.cpp).
    // Cheap scrub: while on, the vehicle rebuilds and the event state
    // (replay::events::SetStateDeferred) wait for the release.
    void SetCheapScrub(bool cheap);
    bool CheapScrub();
    // Bodies created per frame (seek_settle CreateBudget; 0: the default).
    void SetCreateBudget(std::uint32_t budget);
    // The missing puppet detector (puppet_watch.h) pauses while
    // the seek settle or the clip load wait holds the playhead.
    void SetWatchHeld(bool held);
    // PuppetLodMultiplier, PuppetAlwaysPrerender, PuppetForceAnimUpdate,
    // PuppetScriptPose (puppet_lod.h).
    void SetPuppetLod(const lod::Config& config);
    // HatFix (default 1, hat_chain.h): a puppet's hat fitted again where the
    // recording has the bind pose; 0 plays the recorded hat (A/B).
    void SetHatFix(bool on);
    // HideNeverAnimated (default 1, tpose_check.h):
    // 1): a body whose record never changed pose and whose recorded pose is
    // its model's bind pose (the T-pose) is not shown; 0 shows it as
    // recorded; 2 also hides a never animated record whose bind pose could
    // not be read. TPoseWatch (default 1): the T-pose detector on what the
    // shown puppets draw (POSE_V2_TPOSE, POSE_V2_TPOSE_SUMMARY).
    void SetTPose(int hideNeverAnimated, bool watch);
    // TPoseSettleMs (default 1500; 0 = judge at once) and TPoseOnScreenOnly
    // (default 1): the watch does not judge while the replay prepares or the
    // screen is faded, for settleMs after a pause, resume, scrub or jump, nor
    // (onScreenOnly) a body off screen.
    void SetTPoseGate(std::uint32_t settleMs, bool onScreenOnly);
    // SegmentSwitchInTick (default 1, segment_switch.h): a skeleton change
    // re-armed in the tick it is seen; 0 releases then arms (A/B).
    void SetSegmentSwitchInTick(bool on);
    // NoRecordMapNow (default 1, no_record_fallback.h): a posed body whose
    // skeleton changed to a bone count no record has is armed through the
    // skeleton map at once (0: hidden through six layer retries first);
    // NoRecordAltAppearance (default 0): behind the mapped pose, the entity's
    // other recorded appearances of the segment are tried for one that builds
    // the record.
    void SetNoRecordFallback(bool mapNow, bool alternates);
    // What the last Update left to build (bodies alive at the playhead).
    struct SettleState
    {
        std::uint32_t puppets = 0;
        std::uint32_t building = 0;      // Loading, Dressing, or not placed yet
        std::uint32_t modelsMissing = 0; // of those, model not loaded
        std::uint32_t dueWithModel = 0;  // model loaded, waiting for the create budget
    };
    SettleState GetSettleState();
    struct Counters
    {
        std::uint64_t creates = 0;
        std::uint64_t vehicleRebuilds = 0;
    };
    Counters GetCounters();
    // Bodies alive at timeMs whose model is not loaded (a far jump test).
    std::uint32_t AliveWithoutModel(std::uint32_t timeMs);
    // The model window: models alive in [playhead - windowMs, playhead +
    // windowMs] requested ahead, the ones untouched for 5 s let go (PRELOAD).
    void PreloadModels(std::uint32_t playheadMs, std::uint64_t now, std::uint32_t windowMs);
    // The entity's RECORDED root at timeMs (the engine matrix of its pose
    // record there, interpolated exactly as its puppet's is) and, when
    // headBone (an engine bone index, GET_PED_BONE_INDEX) is a bone of that
    // record, the bone's recorded world position. What the editor camera
    // follows, instead of the live puppet read by natives. False outside the
    // record's span or without a pose record (the caller falls back to the
    // live puppet).
    struct RecordedBody
    {
        float position[3] = {};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // quaternion x, y, z, w
        bool hasHead = false;
        float head[3] = {};
        std::uint32_t recordId = 0;
        // With a bone asked, its object space transform as recorded (the
        // camera's bone attach composes it with the root,
        // camera::EntityFromBone), and the record's bone count.
        float boneTranslation[3] = {};
        float boneRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // quaternion x, y, z, w
        std::uint32_t boneCount = 0;
        bool heldFirst = false; // sampled at the record's first sample (holdFirst)
    };
    // holdFirst (camera lookups): before the record's first sample,
    // sample the first one (camera_bones.h CameraRecordTime); out.heldFirst says so.
    bool RecordedBodyAt(std::uint32_t packedId, std::uint32_t timeMs, int headBone, RecordedBody& out, bool holdFirst = false);
}
