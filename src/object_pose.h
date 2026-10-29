#pragma once

// Skeleton pose of an attached object (pure, unit tested in ReplayCoreTests).
//
// The pelt Arthur carries after skinning a large animal is an object
// (p_cs_pelt_large) attached to his hand, with its own skeleton: the skinning
// and carry animations roll it up by its bones. Recorded as a plain attached
// object (a transform relative to its holder and no pose) it replays in its
// bind pose, a flat sheet. An attached non-weapon object whose skeleton
// resolves with more than one bone gets a pose record keyed by its own replay
// id, captured and written like a vehicle's (script thread polls, override
// written from the script thread and after the finalizer).

#include <cstddef>
#include <cstdint>

namespace replay::object_pose
{
    // Attached objects posed at once, and capture places always left to
    // peds, horses and vehicles (a body refused for lack of a place is
    // rejected for good).
    constexpr std::size_t kCapacity = 4;
    constexpr std::size_t kBodyReserve = 8;
    // A rigid prop has a single (root) bone: nothing to pose.
    constexpr std::uint32_t kMinBones = 2;

    // Whether an attached object gets a pose record. `weaponObject` is a real
    // weapon (its model is the weapon's). `bodies` are the recorded peds, horses
    // and vehicles, `posed` the objects already posed, `maxTargets` the capture
    // places (replay::pose::capture::kMaxLiveTargets).
    inline bool ShouldPose(bool weaponObject, std::uint32_t bones, std::size_t bodies, std::size_t posed,
        std::size_t maxTargets)
    {
        if (weaponObject || bones < kMinBones || posed >= kCapacity)
            return false;
        return bodies + posed + kBodyReserve < maxTargets;
    }

    // Capture places of their own for posed objects. The rule above can never
    // pass in a crowded scene: the ped class (56) and the vehicle class (8)
    // alone fill the 64 places. The object places are taken from the ped class
    // only while an object is posed or waits for a place (ReservedPlaces), so a
    // scene without one keeps all 56 ped places; the farthest unpinned ped
    // leaves for the object (entity_selection.h, capacity below the recorded
    // count). `places` is the ini key ObjectPosePlaces (dev builds, for A/B
    // only; 0, the default: the shared places below).
    constexpr std::uint32_t kDefaultPlaces = 4;
    constexpr std::uint32_t kMaxPlaces = 8;

    // The shared places. The capture slots are sized to both
    // pools (capture_capacity.h kLiveTargetSlots); the ped and vehicle classes
    // hold theirs, the train cars a few, and every slot left is shared by the
    // weapons in a hand and the posed objects (the pelt, a carried bag, a
    // drawer or a lid the player opens): an object starts while one is free,
    // and no body ever leaves for it. The four places taken from the ped class
    // (ObjectPosePlaces) were a cap with no ground in the data: the bags of a
    // town's NPCs held them from the start of 1211-Blackwater (places=4/4,
    // no_place_tries=854) and the drawer the player opened had none.
    // `sharedPlaces` is WeaponPlaces less the train cars' places.
    inline bool ShouldPoseShared(bool weaponObject, std::uint32_t bones, std::size_t posedObjects, std::size_t posedWeapons,
        std::uint32_t sharedPlaces)
    {
        return !weaponObject && bones >= kMinBones && posedObjects + posedWeapons < sharedPlaces;
    }

    inline std::uint32_t ClampPlaces(std::uint32_t places)
    {
        return places > kMaxPlaces ? kMaxPlaces : places;
    }

    // Object places held now: the posed objects and those refused for a
    // place, at most `places`.
    inline std::uint32_t ReservedPlaces(std::uint32_t places, std::size_t posed, std::size_t waiting)
    {
        const std::size_t wanted = posed + waiting;
        const std::uint32_t limit = ClampPlaces(places);
        return wanted < limit ? static_cast<std::uint32_t>(wanted) : limit;
    }

    // The ped class capacity with `reserved` object places taken from it.
    inline std::uint32_t PedPlaces(std::uint32_t pedCapacity, std::uint32_t reserved)
    {
        const std::uint32_t taken = ClampPlaces(reserved);
        return taken < pedCapacity ? pedCapacity - taken : 0;
    }

    // `pedBodies` are the recorded peds and horses (the ped class),
    // `pedCapacity` the class's full capacity (replay::recorder::kPedCapacity).
    // An object starts only while the ped class leaves it a place, so the
    // classes never hold more than the 64 capture places and a body's Start
    // never fails for an object.
    inline bool ShouldPose(bool weaponObject, std::uint32_t bones, std::size_t pedBodies, std::size_t posed,
        std::uint32_t places, std::uint32_t pedCapacity, std::size_t bodies, std::size_t maxTargets)
    {
        if (places == 0)
            return ShouldPose(weaponObject, bones, bodies, posed, maxTargets);
        if (weaponObject || bones < kMinBones || posed >= ClampPlaces(places))
            return false;
        return pedBodies + posed < pedCapacity && bodies + posed < maxTargets;
    }

    // The object that leaves the hand (the pelt dropped or rolled onto the
    // saddle) continues as a dropped weapon prop with a new id. Its pose
    // continues under that id when the weapon was posed or waited for its
    // skeleton or a place, and only for an object with no weapon (a real
    // weapon's moving parts follow the weapon system).
    inline bool ContinuesPose(bool droppedWeapon, std::uint32_t weaponHash, bool posed, bool candidate)
    {
        return droppedWeapon && weaponHash == 0 && (posed || candidate);
    }

    // A real weapon's own bones (lever, pump, cylinder, loading gate, hammer,
    // bolt). The weapon system does not move them on the replay object, so they
    // are recorded like any animated object's: with WeaponBonePose every real
    // weapon in a hand of a recorded ped (weapon points 0 and 1: point 0 is the
    // held weapon) records its own bones from the moment it is in the hand, as
    // long as it is there, in the capture slots the bodies and vehicles do not
    // use (WeaponPlaces: kLiveTargetSlots less the live targets; pose_capture
    // SetExtraPlaces), never taken from the bodies or from the ObjectPosePlaces
    // of the pelt and the props. A body never loses a place to a weapon (the
    // weapons stop at that count, the bodies at theirs). The pose track's
    // encoder compresses it as a body's.
    constexpr std::uint32_t kDefaultWeaponPlaces = 4; // used by the tests only

    inline std::uint32_t WeaponPlaces(std::uint32_t slots, std::uint32_t liveTargets)
    {
        return slots > liveTargets ? slots - liveTargets : 0u;
    }

    inline bool InHandPoint(int attachPoint) { return attachPoint == 0 || attachPoint == 1; }

    // Watched until its skeleton reads (a weapon object built a few frames
    // after it reaches the hand); no cap but the weapon registry.
    inline bool WatchWeaponBones(bool enabled, std::uint32_t weaponHash, int attachPoint, bool posed, bool watching)
    {
        return enabled && weaponHash != 0 && InHandPoint(attachPoint) && !posed && !watching;
    }

    inline bool ShouldPoseWeapon(bool enabled, std::uint32_t bones, std::size_t posedWeapons, std::uint32_t places)
    {
        return enabled && bones >= kMinBones && posedWeapons < places;
    }

    // Playback: the props that may carry a pose record (hats, gore pieces,
    // vehicle pieces, world objects and real weapons never do).
    inline bool PoseableProp(bool droppedWeapon, std::uint32_t weaponHash)
    {
        return droppedWeapon && weaponHash == 0;
    }

    // An object builds its skeleton only when something first needs one
    // (CreateSkeleton, 0xE34A18, called by PLAY_ENTITY_ANIM, the attach natives
    // and the animation system), so the pelt can enter the recording before it
    // has one. A candidate (attached, not a weapon, not posed, never ended by a
    // skeleton change) is resolved again at this interval while it stays
    // recorded.
    constexpr std::uint32_t kRetryIntervalMs = 250;

    inline bool ShouldRetryResolve(bool candidate, bool posed, std::uint32_t nowMs, std::uint32_t nextTryMs)
    {
        return candidate && !posed && nowMs >= nextTryMs;
    }

    // A pose that started after the create: the create packet kept in the
    // clip (Clip::created, what FindCreate and the pose segments read) takes
    // the bone count, the lifecycle blocks already written keep 0. Playback
    // uses the larger of the two.
    inline std::uint32_t PlaybackBones(std::uint32_t lifecycleBones, std::uint32_t createdBones)
    {
        return createdBones > lifecycleBones ? createdBones : lifecycleBones;
    }

    // Playback: the replay object is a fresh CREATE_OBJECT, which has no
    // skeleton until something asks for one. The object skeleton native
    // (0x491439AEF410A2FC, handler 0x10C0A54 -> 0x10CE198 -> 0xE52368 ->
    // CreateSkeleton) is asked once per replay object whose recorded pose
    // has bones and whose skeleton did not resolve.
    inline bool ShouldAskSkeleton(std::uint32_t recordedBones, bool resolved, bool asked)
    {
        return recordedBones >= kMinBones && !resolved && !asked;
    }

    // Asked again when the replay object's skeleton is gone later (a fragment
    // cache entry released or rebuilt in a crowded scene drops the arm), at most
    // kMaxSkeletonAsks times per replay object, kSkeletonAskIntervalMs apart.
    // `asks` is how many were made for this object.
    constexpr std::uint32_t kMaxSkeletonAsks = 4;
    constexpr std::uint32_t kSkeletonAskIntervalMs = 1000;

    inline bool ShouldAskSkeletonAgain(std::uint32_t recordedBones, bool resolved, std::uint32_t asks,
        std::uint64_t nowMs, std::uint64_t nextAskMs)
    {
        return recordedBones >= kMinBones && !resolved && asks < kMaxSkeletonAsks && nowMs >= nextAskMs;
    }

    // Playback of a free object spawned next to a recorded ped (a
    // Projectile-origin prop with no weapon, below) plays its pose record too,
    // like the dropped object. Props without a pose record (arrows, rope ends,
    // older clips) have no bones in their create and are left alone by
    // PlayPoseV2ObjectPose.
    inline bool PoseableProp(bool droppedWeapon, bool freeObject, std::uint32_t weaponHash)
    {
        return (droppedWeapon || freeObject) && weaponHash == 0;
    }

    // Objects the engine creates next to a recorded ped during the recording,
    // free in the world (not attached to an entity). An object an animation
    // moves in world space (a synchronized scene places its props without
    // attaching them: the pelt rolled up on the ground before it goes to the
    // hand) is otherwise not recorded until it is attached.
    //
    // Rule: an object first seen during the recording (not in the start
    // snapshot, not recorded, no parent entity) within kNearM of a recorded ped
    // is watched for kWatchMs; it is recorded (a prop with its world track,
    // owned by that ped) as soon as it moves or its skeleton's local pose
    // changes, and posed when it is animated. A map object streamed in never
    // moves nor animates, so it is never recorded; one first seen far from
    // every recorded ped is never watched.
    namespace spawn
    {
        constexpr float kNearM = 3.0f;
        constexpr std::uint32_t kWatchMs = 20000;
        constexpr std::size_t kMaxWatched = 32;
        // Bones whose locals are kept per watched object (the pelt has 61).
        constexpr std::uint32_t kMaxBones = 256;
        // One local as the engine stores it (BoneQts: quaternion, translation
        // and scale, four floats each).
        constexpr std::size_t kFloatsPerBone = 12;
        // Root movement, as the world object rule (ScanPoseV2WorldObjects).
        constexpr float kMoveM = 0.15f;
        constexpr float kTurnDeg = 8.0f;
        // A local pose change: a bone translation of 1 cm, or a rotation of
        // about 2 degrees (|dot| of the unit quaternions below cos(1 deg)).
        constexpr float kBoneMoveM = 0.01f;
        constexpr float kBoneDotMin = 0.99985f;

        // At the first sight of a listed object.
        inline bool WatchAtFirstSight(bool enabled, bool inSnapshot, bool known, bool hasParent, bool excludedModel,
            float nearestPedM)
        {
            return enabled && !inSnapshot && !known && !hasParent && !excludedModel && nearestPedM >= 0.0f &&
                nearestPedM <= kNearM;
        }

        inline float AngleDelta(float a, float b)
        {
            float d = a > b ? a - b : b - a;
            while (d > 180.0f)
            {
                d -= 360.0f;
                if (d < 0.0f)
                    d = -d;
            }
            return d;
        }

        // The root moved since the first sight (positions in metres,
        // rotations in degrees).
        inline bool RootMoved(const float from[3], const float to[3], const float fromRot[3], const float toRot[3])
        {
            const float dx = to[0] - from[0];
            const float dy = to[1] - from[1];
            const float dz = to[2] - from[2];
            if (dx * dx + dy * dy + dz * dz > kMoveM * kMoveM)
                return true;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (AngleDelta(fromRot[axis], toRot[axis]) > kTurnDeg)
                    return true;
            }
            return false;
        }

        // The local pose changed: `a` and `b` hold `bones` locals of
        // kFloatsPerBone floats (x y z w, translation x y z pad, scale).
        inline bool LocalsChanged(const float* a, const float* b, std::uint32_t bones)
        {
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                const float* p = a + static_cast<std::size_t>(bone) * kFloatsPerBone;
                const float* q = b + static_cast<std::size_t>(bone) * kFloatsPerBone;
                float dot = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
                if (dot < 0.0f)
                    dot = -dot;
                if (dot < kBoneDotMin)
                    return true;
                const float dx = p[4] - q[4];
                const float dy = p[5] - q[5];
                const float dz = p[6] - q[6];
                if (dx * dx + dy * dy + dz * dz > kBoneMoveM * kBoneMoveM)
                    return true;
            }
            return false;
        }

        enum class Verdict : std::uint8_t
        {
            Keep = 0,   // still watched
            Record = 1, // moved or animated: a prop from now on
            Expire = 2, // static for kWatchMs: never recorded
        };

        inline Verdict Decide(bool moved, bool animated, std::uint32_t ageMs)
        {
            if (moved || animated)
                return Verdict::Record;
            return ageMs >= kWatchMs ? Verdict::Expire : Verdict::Keep;
        }

        // A recorded spawned object takes a pose place only when its bones were
        // seen moving (a fragment prop that only flies is rigid here). One recorded
        // without a pose keeps its bones watched and is posed when they move
        // (object_check.h AfterPropBoneRead).
        inline bool PoseSpawned(bool animated, std::uint32_t bones)
        {
            return animated && bones >= kMinBones;
        }

        // An object refused for distance or a full watch (a saw listed before its
        // carpenter was recorded, more than kNearM from every recorded ped) is
        // looked at again every kRecheckMs while listed, and watched when a recorded
        // ped is within reach, like the scenery's far recheck
        // (objcheck::kSceneryFarRecheckMs). A still object whose watch ended
        // (Expire) is looked at again too, but watched again only after it was
        // re-armed: it moved, nobody recorded stood within reach, or another ped is
        // the nearest (a ped standing by a bench does not rewatch it every 20 s).
        constexpr std::uint32_t kRecheckMs = 500;
        constexpr std::size_t kMaxRecheck = 8192;
        // A watched still object with no recorded ped within kLeaveM after
        // kLeaveAfterMs goes back to the recheck (armed): the watch keeps its
        // places for the objects a ped can touch.
        constexpr float kLeaveM = 6.0f;
        constexpr std::uint32_t kLeaveAfterMs = 1500;
        // A full watch gives a place to a candidate nearer than its farthest
        // still object by kEvictMarginM (the farthest goes back armed).
        constexpr float kEvictMarginM = 0.5f;

        inline bool RecheckDue(std::uint32_t nowMs, std::uint32_t nextMs)
        {
            return static_cast<std::int32_t>(nowMs - nextMs) >= 0;
        }

        inline bool WithinReach(float nearestPedM)
        {
            return nearestPedM >= 0.0f && nearestPedM <= kNearM;
        }

        // An expired object is armed again when it moved since, when no
        // recorded ped is within reach, or when the nearest one within reach
        // is another ped than the one it expired by.
        inline bool Rearm(bool armed, bool movedSince, float nearestPedM, bool sameNearestPed)
        {
            return armed || movedSince || !WithinReach(nearestPedM) || !sameNearestPed;
        }

        // A still watched object no recorded ped is near any more.
        inline bool LeaveWatch(std::uint32_t ageMs, float nearestPedM)
        {
            return ageMs >= kLeaveAfterMs && (nearestPedM < 0.0f || nearestPedM > kLeaveM);
        }

        // The watched entry a candidate at candidateM takes the place of:
        // the farthest of `count` still entries (distances in `watchedM`,
        // -1 for no recorded ped), when it is farther than the candidate by
        // the margin. -1: none.
        inline int EvictFor(float candidateM, const float* watchedM, std::size_t count)
        {
            int best = -1;
            float bestM = candidateM + kEvictMarginM;
            for (std::size_t n = 0; n < count; ++n)
            {
                const float m = watchedM[n] < 0.0f ? 1.0e9f : watchedM[n];
                if (m > bestM)
                {
                    bestM = m;
                    best = static_cast<int>(n);
                }
            }
            return best;
        }
    }
}
