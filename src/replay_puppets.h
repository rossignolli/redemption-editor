#pragma once

// Playback puppet registry (pure data): the inert entities that play the
// recording, one per alive replay id, with the state playback keeps per
// puppet (build state, arming, attachment, evidence counters).

#include "entity_lifecycle.h"
#include "hat_chain.h"
#include "metaped_state.h"
#include "no_record_fallback.h"
#include "playback_cache.h"
#include "pose_math.h"
#include "puppet_blink.h"
#include "puppet_lod.h"
#include "segment_switch.h"
#include "skeleton_map.h"
#include "still_pose_core.h"
#include "weapon_observer.h"

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::playback
{
    // Puppets are built from the create packet: stream the model, create the entity, rebuild the recorded appearance (outfit
    // first, recorded layers when the outfit does not reproduce them), and
    // only then show it and write the pose.
    constexpr std::uint64_t kModelTimeoutMs = 5000;
    constexpr std::uint64_t kDressTimeoutMs = 2000;
    // The engine can refuse a ped for a moment (population budget), so
    // creation is retried on later frames.
    constexpr std::uint64_t kCreateRetryMs = 200;
    // Puppets built per frame, at most.
    constexpr std::uint32_t kMaxCreatesPerFrame = 4;
    // ClipStartStagger (clip_start_gate.h): while the clip start creates in batches, every kind of puppet (bodies,
    // weapons, scenery stand-ins) shares one budget per tick. 0: no cap (the
    // per-kind budgets above only). The script sets the cap and clears the
    // use once per tick (replay::playback::BeginStartBatchTick).
    inline std::uint32_t g_startBatchCap = 0;
    inline std::uint32_t g_startBatchUsed = 0;
    inline std::uint64_t g_startBatchCreates = 0; // created under the cap, for STREAM_PRESSURE
    inline void BeginStartBatchTick(std::uint32_t cap)
    {
        g_startBatchCap = cap;
        g_startBatchUsed = 0;
    }
    inline bool StartBatchLeft() { return g_startBatchCap == 0 || g_startBatchUsed < g_startBatchCap; }
    inline void StartBatchUse()
    {
        if (g_startBatchCap == 0)
            return;
        ++g_startBatchUsed;
        ++g_startBatchCreates;
    }

    enum class PuppetState
    {
        Loading,
        Dressing,
        Ready,
        Failed,
    };

    struct Puppet
    {
        entity::CreatePacket packet;
        Ped clone = 0;
        PuppetState state = PuppetState::Loading;
        std::uint64_t stateSince = 0;
        bool layersApplied = false;
        bool outfitApplied = false;
        // What the puppet must wear at the playhead (appearance_track.h
        // DressAt: the playhead's, or a later segment's for a ped recorded
        // while the engine still built it), the metaped state worn
        // (metaped_state.h StateAt key, 0 none) and its one retry. `wear` is
        // a copy, not a pointer: the list of puppets is a vector, and a
        // puppet queued or erased while another dressed would move the
        // appearance a pointer referred to.
        entity::Appearance wear;
        bool hasWear = false;
        const char* dressFrom = "create";
        std::uint32_t dressLookaheadMs = 0;
        std::uint32_t metapedKey = 0;
        bool metapedRetried = false;
        std::uint32_t metapedSet = 0; // expressions set on the clone (evidence)
        // metaped_state.h DressPath: how the body was
        // last dressed and why; the shop path refused for good once its
        // layers were not the recorded ones (back to the layers); the shop
        // items applied; when a dress of a shown puppet is checked (0 none).
        metaped::DressPath dressedBy = metaped::DressPath::Layers;
        const char* dressWhy = "none";
        bool shopRefused = false;
        std::uint32_t shopItemsApplied = 0;
        std::uint64_t shopCheckAt = 0;
        std::uint64_t shopCheckSince = 0;
        // segment_switch.h: the ticks without a
        // pose around skeleton changes, and since when the arm holds the
        // previous record while the engine has not rebuilt the skeleton.
        segswitch::Gap switchGap;
        std::uint64_t holdSince = 0;
        // prefix_hold.h: the armed record's body group (set at the exact arm,
        // given to the override slot), and the hold of a skeleton that shrank
        // to a count no record has (a gore rebuild in progress).
        std::uint32_t bodyBones = 0;
        prefixhold::Hold prefixHold;
        std::uint32_t createAttempts = 0;
        std::uint64_t nextCreateAt = 0;
        std::uint32_t extraRerolls = 0; // vehicle re-creations for a recorded extra, one per create
        // Current attachment applied to this puppet (child side).
        bool attached = false;
        entity::Attachment attachment;
        Entity attachedParentClone = 0; // the parent entity it was attached to
        std::uint32_t attachFailures = 0; // failed harness attaches on this parent entity
        std::uint64_t attachRetryAt = 0;
        int overrideSlot = -1;
        bool armFailureLogged = false;
        std::uint64_t armDeadline = 0;
        // A layer application that left the skeleton with a bone count no
        // record of the entity has (a T-pose otherwise) is applied again a
        // few times.
        std::uint32_t layerRetries = 0;
        std::uint64_t nextLayerRetryAt = 0;
        // no_record_fallback.h: the arm was
        // lost to a skeleton change (the body had been posed: it is armed
        // through the map at once, not hidden), the map refused for that
        // change (the retries above, hidden), and the steps behind the
        // mapped pose (retries, other recorded appearances).
        bool lostToChange = false;
        bool noRecordMapRefused = false;
        norecord::Fallback noRecord;
        // The layer retries spent and still
        // no record with the skeleton's bone count: armed through a per-group
        // skeleton map (skeleton_map.h) once the detour read the puppet's
        // parent table and locals (mapRequest, for mapSkeleton/mapBones until
        // mapDeadline). `mapped`: the armed pose goes through `map`, the
        // puppet-only bones in mapRest. armRefused: no record and no map, the
        // puppet stays hidden rather than a visible T-pose (armError says why), a
        // line every refusedLogAt; a skeleton other than
        // failedSkeleton/failedBones (checked at rearmProbeAt) is armed again.
        int mapRequest = -1;
        std::uint8_t* mapSkeleton = nullptr;
        std::uint32_t mapBones = 0;
        std::uint64_t mapDeadline = 0;
        bool mapped = false;
        std::uint32_t mapSegmentRecord = 0; // the segment's record when the map was armed
        std::string armError;
        pose::SkeletonMap map;
        std::vector<pose::BoneQts> mapRest;
        std::uint64_t refusedLogAt = 0;
        int playerHoldState = 0; // the recorded player's last POSE_V2_PLAYER_HOLD state (player_hold.h)
        std::uint8_t* failedSkeleton = nullptr;
        std::uint32_t failedBones = 0;
        std::uint64_t rearmProbeAt = 0;
        // skeleton_signature.h: the puppet's engine
        // parent table for sigSkeleton/sigBones (sigHave), asked of the
        // detour by sigRequest until sigDeadline (sigFailed: it could not
        // be read for that skeleton, not asked again), with its locals and
        // group count for a skeleton map. sigVerifiedKey: the record layout
        // the armed skeleton was last compared with; sigRecheckAt: when the
        // armed skeleton is compared again (layers applied on it, which may
        // rebuild it in place with the same bone count), sigRechecks left.
        // sigLogged*: the last POSE_V2_SKELETON_SIGNATURE line's key.
        // sigRefusedKey: the layout a signature refusal was for (another
        // layout at the playhead arms again).
        int sigRequest = -1;
        std::uint8_t* sigSkeleton = nullptr;
        std::uint32_t sigBones = 0;
        std::uint64_t sigDeadline = 0;
        bool sigHave = false;
        bool sigFailed = false;
        std::vector<std::uint16_t> sigParents;
        std::vector<pose::BoneQts> sigLocals;
        std::uint32_t sigGroups = 0;
        std::uintptr_t sigVerifiedKey = 0;
        std::uint64_t sigRecheckAt = 0;
        std::uint32_t sigRechecks = 0;
        std::uintptr_t sigLoggedKey = 0;
        std::uint8_t* sigLoggedSkeleton = nullptr;
        std::uintptr_t sigRefusedKey = 0;
        bool sigMapped = false; // armed through a map the signature check chose (rechecked on a layout change)
        // Last recorded world transform placed.
        pose::WorldTransform lastWorld{};
        bool hasWorld = false;
        // The natives that place the clone and set its
        // visibility run only when the value changes (a standing ped's
        // transform is bit-identical frame after frame); the placement is
        // re-asserted every kPlaceRefreshFrames anyway.
        bool placed = false;
        pose::WorldTransform placedWorld{};
        std::uint32_t placeHeldFrames = 0;
        int visibleState = -1; // -1 unknown, 0 hidden, 1 visible
        // Weapons: no parent at the playhead, so nothing to build yet.
        bool waitingForParent = false;
        // Frames of seat evidence left to log after taking a seat.
        std::uint32_t seatFollowLeft = 0;
        // Pose segment at the playhead: its record and bone count.
        std::uint32_t recordId = 0;
        std::uint32_t boneCount = 0;
        // Override target: the skeleton armed (the engine rebuilds it on any
        // layer change, from the script thread or in its own update) and the
        // record whose bones match it: the segment at the playhead, or the
        // nearest segment with those bones while the skeleton is still the
        // other one, so no frame renders without the recorded pose.
        std::uint8_t* armedSkeleton = nullptr;
        std::uint32_t armedBones = 0;
        std::uint32_t armedRecordId = 0;
        // hat_chain.h: the hat fitted again where the
        // recording has the bind pose (the ped off screen while recorded),
        // built for the armed record's layout at the playhead (hatLayout).
        hatchain::PuppetFix hatFix;
        std::uintptr_t hatLayout = 0;
        // tpose_check.h: the never animated hide
        // judged for neverRecord on neverSkeleton (0: not yet) and whether it
        // hides the puppet; the bind rotations of bindSkeleton (bindError: why
        // they could not be read, "" read); the T-pose watch's next check
        // (GetTickCount64).
        std::uint32_t neverRecord = 0;
        std::uint8_t* neverSkeleton = nullptr;
        bool neverHidden = false;
        std::uint8_t* bindSkeleton = nullptr;
        std::vector<pose::Quat> bindRotations;
        const char* bindError = "";
        std::uint64_t tposeCheckAt = 0;
        // still_pose_core.h: the still rule decided for
        // stillRecord (0: not yet) and whether it leaves the object to the
        // engine (stillSkip); the record's constant locals the drawn check
        // compares an unarmed object with; the drawn check's totals.
        std::uint32_t stillRecord = 0;
        bool stillSkip = false;
        std::vector<pose::BoneQts> stillLocals;
        stillpose::Totals drawn;
        std::uint64_t hatCheckAt = 0; // the layout looked at again from then (GetTickCount64)
        // The last pose sample, kept while the playhead does not move (playback_cache.h).
        SampleReuse sampleReuse;
        // Ambient voice given to the puppet for its replayed lines, and a
        // voice the engine did not find (not retried).
        std::uint32_t speechVoice = 0;
        std::uint32_t failedVoice = 0;
        // Props: the owner puppet was asked to drop its hat, the hat it had
        // dropped before, and how long the engine gets to make the new one.
        bool knockRequested = false;
        Object hatBefore = 0;
        std::uint64_t knockDeadline = 0;
        // hatlook::WhenToKnock: when the owner
        // puppet was dressed with the hat for the knock (0: it wore it).
        std::uint64_t hatWoreAt = 0;
        // Weapons: attached to the parent puppet; the world transform the
        // last attach asked for, checked on the next frame.
        bool attachedToParent = false;
        bool attachCheckPending = false;
        pose::WorldTransform attachWanted{};
        std::uint32_t attachCheckLoggedMs = 0; // periodic evidence
        int conditionApplied = -1; // index of the weapon condition state set on the clone
        // strap_state_core.h: the strap state (index in its track) last put
        // on the clone and the clone it was put on, for one log line per change.
        int strapApplied = -1;
        Object strapClone = 0;
        std::uint32_t strapTestRegive = 0; // developer bench: the regive generation done
        // weapon_look_core.h: the read back of the look
        // (wall clock ms of the next one, 0 none), the wait for the models of
        // a later state's components, and the wear set again after the
        // engine moved it.
        std::uint64_t weaponLookCheckAt = 0;
        std::uint64_t weaponLookWaitUntil = 0;
        std::uint32_t weaponLookReapplied = 0;
        // The charred state written on the clone (-1 none yet)
        // and when the engine's own value is next compared with it.
        int scorchedApplied = -1;
        std::uint64_t scorchCheckAt = 0;
        // The appearance change worn (AppearanceAt's key: 0 = the
        // create packet or pose segment, n = change n - 1 of its track).
        std::uint32_t appearanceKey = 0;
        // The broken windows removed on windowsClone (a new clone starts
        // with its glass, mask 0). The paint and livery set on lookClone,
        // checked again
        // at lookCheckAt (0: done) after the vehicle rendered.
        Entity lookClone = 0;
        std::int32_t lookTint = -1;
        std::int32_t lookLivery = -1;
        std::uint32_t lookChecks = 0;
        std::uint64_t lookCheckAt = 0;
        Entity windowsClone = 0;
        std::uint8_t windowsApplied = 0;
        // The lost wheels and doors applied on partsClone
        // (VehiclePartTrack bits; a new clone starts whole, mask 0).
        Entity partsClone = 0;
        std::uint16_t partsApplied = 0;
        // How many of the vehicle's recorded fragment group
        // events were applied on fragClone (a new clone starts whole, 0).
        Entity fragClone = 0;
        std::uint32_t fragApplied = 0;
        // burntClone was seen on fire or charred from the
        // playhead burntSeenMs (UINT32_MAX: not yet); checked at burntCheckAt.
        Entity burntClone = 0;
        std::uint32_t burntSeenMs = 0xFFFFFFFFu;
        std::uint64_t burntCheckAt = 0;
        // The burn level last written on burnLevelClone (a new
        // clone starts with the model's own level; burnLevelHas false:
        // nothing written on it yet).
        Entity burnLevelClone = 0;
        float burnLevelWritten = 0.0f;
        bool burnLevelHas = false;
        // The fire record (burnt look) on fireCharClone, checked
        // at fireCharCheckAt; fireCharIgnitedAt: our one-tick fire made to
        // get a record (0: none), put out on the next check.
        Entity fireCharClone = 0;
        std::uint64_t fireCharCheckAt = 0;
        std::uint64_t fireCharIgnitedAt = 0;
        std::uint32_t fireCharIgnites = 0;
        std::uint32_t fireCharWrites = 0;
        bool fireCharHad = false;
        // Attach frame applied (bone attach): re-attached only
        // when the recorded frame or the parent changes.
        bool attachFrameApplied = false;
        Ped attachFrameParent = 0;
        weapon::AttachFrame attachFrame;
        // puppet_lod.h: LOD multiplier and
        // pre-render applied to lod.clone, the pose writes seen per tick.
        lod::PuppetState lod;
        // puppet_blink.h: the override slot's write counters at the last
        // tick, for the frame in between (the bind check).
        blink::WriteBaseline blinkWrites;
    };

    // What the puppet must wear, a copy kept in the puppet; until it is dressed,
    // its create packet.
    inline void SetWear(Puppet& puppet, const entity::Appearance& appearance)
    {
        if (&appearance != &puppet.wear)
            puppet.wear = appearance;
        puppet.hasWear = true;
    }
    inline const entity::Appearance& Wear(const Puppet& puppet)
    {
        return puppet.hasWear ? puppet.wear : puppet.packet.appearance;
    }

    constexpr std::uint32_t kPlaceRefreshFrames = 15;

    struct Puppets
    {
        std::vector<Puppet> list;

        Puppet* Find(entity::ReplayId id)
        {
            for (Puppet& puppet : list)
            {
                if (puppet.packet.id == id)
                    return &puppet;
            }
            return nullptr;
        }
        const Puppet* Find(entity::ReplayId id) const
        {
            return const_cast<Puppets*>(this)->Find(id);
        }
        std::size_t Count() const { return list.size(); }
    };
}
