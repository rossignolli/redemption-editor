#pragma once

// Recorder registry (pure data): the engine entities being recorded and
// what the recorder remembers about them between frames. Not clip data:
// the clip (replay_recording.h) keeps only what playback needs.

#include "capture_capacity.h"
#include "entity_lifecycle.h"
#include "pose_math.h"

#include "types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::recorder
{
    // Engine pool enumeration buffer (peds, vehicles).
    constexpr int kPoolCapacity = 1024;
    // Capture rate of the transform tracks.
    constexpr std::uint32_t kSampleHz = 30;
    // Capture places per class, so parked wagons never crowd out people.
    // The ped class is the ini key PedCapacity (default 56,
    // capture_capacity.h); kPedCapacity reads the session's value.
    inline constexpr replay::capacity::PedCapacityValue kPedCapacity{};
    constexpr int kWeaponAttachPoints = 32;
    // Attached objects (hand props, saddle holster weapons) share the weapon
    // registry, so it holds more than the weapon points: 96 at the default
    // ped capacity, in proportion above it.
    inline constexpr replay::capacity::WeaponCapacityValue kWeaponCapacity{};
    // The attach point of an object found attached to a recorded entity by
    // GET_ENTITY_ATTACHED_TO rather than at one of the ped's weapon points.
    constexpr int kAttachedObjectPoint = -1;
    // Loose objects alive at once (hats, dropped weapons, gore pieces,
    // projectiles). A big fight fills a small registry with hats and
    // weapons and projectiles get dropped.
    constexpr std::size_t kPropCapacity = 128;
    // A dropped weapon's replacement object is looked for this long, this
    // close to where the weapon was.
    constexpr std::uint32_t kDropSearchMs = 1500;
    constexpr float kDropSearchRadiusM = 3.0f;

    // A recorded ped, horse or vehicle: its engine handle, replay id, the
    // skeleton it was registered with and its current pose segment.
    struct RecordedEntity
    {
        Entity handle = 0;
        entity::ReplayId id;
        std::uint8_t* skeleton = nullptr;
        bool vehicle = false;
        // Pose record and bone count of the current pose segment.
        std::uint32_t recordId = 0;
        std::uint32_t boneCount = 0;
        // Last dropped hat already known, so a hat is recorded once.
        Object lastHat = 0;
        // Weapon in hand at the last scan (evidence).
        Hash lastHeld = 0;
    };

    // A weapon object at a recorded ped's attach point.
    struct RecordedWeapon
    {
        Object object = 0;
        Ped parent = 0;
        entity::ReplayId id;
        int attachPoint = 0;
        bool seen = false;
        std::uint32_t lastSampleMs = 0;
        bool sampled = false;
        // World transform at the last sample: where a dropped weapon's
        // replacement object is looked for.
        pose::WorldTransform lastWorld{};
        // An attached object with a skeleton (object_pose.h; the
        // carried pelt) also has a pose record, id.Packed(), armed on this
        // skeleton with this bone count.
        bool posed = false;
        void* poseSkeleton = nullptr;
        std::uint32_t poseBones = 0;
        // Not a weapon and not posed yet (its skeleton may not exist at the
        // attach): resolved again from this time on (object_pose.h
        // ShouldRetryResolve). Cleared once posed or ended.
        bool poseCandidate = false;
        std::uint32_t nextPoseTryMs = 0;
        std::uint32_t poseTries = 0;
        // The last try resolved a skeleton with bones and found no capture
        // place; the ped class gives one up for it at the next scan
        // (object_pose.h ReservedPlaces).
        bool poseNoPlace = false;
        // The last burn level read (BurnLevelTrack).
        std::uint32_t burnCheckMs = 0;
        bool burnChecked = false;
        // The last custom texture and tint read (ObjectLookTrack,
        // object_look_core.h).
        std::uint32_t lookCheckMs = 0;
        bool lookChecked = false;
        // A real weapon in a hand (object_pose.h WatchWeaponBones), its own
        // bones watched until they move,
        // then posed in the weapon places (bonePose; posed is set too, so the
        // poll and the skeleton check of SamplePoseV2ObjectPose apply).
        std::uint32_t weaponHash = 0;
        bool bonePose = false;
        bool boneWatch = false;
        std::uint8_t boneWatchState = 0;
        std::uint32_t boneWatchBones = 0;
        std::uint32_t nextBoneWatchMs = 0;
        std::vector<float> boneWatchLocals;
    };

    // An object that came off a recorded ped (a knocked off hat, a dropped
    // weapon), sampled by its world transform.
    struct RecordedProp
    {
        Object object = 0;
        entity::ReplayId id;
        std::uint32_t lastSampleMs = 0;
        bool sampled = false;
        // The posed object that left the hand (the rolled pelt) keeps its
        // pose under the prop's id (object_pose.h ContinuesPose), with the
        // same fields as RecordedWeapon.
        bool posed = false;
        void* poseSkeleton = nullptr;
        std::uint32_t poseBones = 0;
        bool poseCandidate = false;
        std::uint32_t nextPoseTryMs = 0;
        std::uint32_t poseTries = 0;
        bool poseNoPlace = false;
        // The last burn level read (BurnLevelTrack).
        std::uint32_t burnCheckMs = 0;
        bool burnChecked = false;
        // The last custom texture and tint read (ObjectLookTrack,
        // object_look_core.h).
        std::uint32_t lookCheckMs = 0;
        bool lookChecked = false;
        // A prop registered without a pose (object_check.h
        // AfterPropBoneRead), its bones watched until they move
        // (objcheck::SkeletonWatch as a byte, the baseline's bone count and
        // locals, when the next read is due).
        bool boneWatch = false;
        std::uint8_t boneWatchState = 0;
        std::uint32_t boneWatchBones = 0;
        std::uint32_t nextBoneWatchMs = 0;
        std::vector<float> boneWatchLocals;
    };

    // A recorded train car (train_consist.h): a rigid world track
    // at the capture rate, no pose record. `address` is the engine entity
    // the consist walk found; `head` the consist's front car.
    struct RecordedTrainCar
    {
        Vehicle handle = 0;
        entity::ReplayId id;
        std::uint64_t address = 0;
        std::uint64_t head = 0;
        std::uint32_t lastSampleMs = 0;
        bool sampled = false;
        // The car's look read again
        // once a second (VehicleLook), and its pose record (the wheels and
        // rods: replay_trains.h, PoseCars).
        std::uint32_t lookCheckMs = 0;
        bool posed = false;
        void* poseSkeleton = nullptr;
        std::uint32_t poseBones = 0;
    };

    // A weapon that left its ped while the engine replaces the object: the
    // replacement is looked for around where the weapon was.
    struct WeaponDrop
    {
        entity::ReplayId owner;
        Entity ownerHandle = 0;
        Object previous = 0;
        Hash model = 0;
        Hash weaponHash = 0;
        std::vector<std::uint32_t> weaponComponents;
        pose::WorldTransform at{};
        std::uint32_t deadlineMs = 0;
        // The object was attached to this recorded entity (a knife
        // stuck in a ped): an attached object of the same model on the same
        // entity continues it as an attached object.
        Entity attachedTo = 0;
        // The weapon was posed or waited for a pose; its
        // replacement continues it (object_pose.h ContinuesPose).
        bool poseCandidate = false;
        entity::ReplayId poseFrom; // the weapon it continues (evidence)
    };

    // Puppets just deleted by playback can still be listed by the pools for
    // a moment; a recording started right after must not pick them up.
    struct IgnoredHandle
    {
        Entity handle = 0;
        std::uint64_t until = 0;
    };

    struct Recorder
    {
        std::vector<RecordedEntity> entities;
        std::vector<RecordedWeapon> weapons;
        std::vector<RecordedProp> props;
        std::vector<RecordedTrainCar> trainCars;
        std::vector<WeaponDrop> drops;
        // Handles that could not be registered; logged once, not retried.
        std::vector<Ped> rejected;
        std::vector<IgnoredHandle> ignored;
        bool recording = false;

        RecordedEntity* FindByHandle(Entity handle)
        {
            for (RecordedEntity& entity : entities)
            {
                if (entity.handle == handle)
                    return &entity;
            }
            return nullptr;
        }

        bool IsIgnored(Entity handle, std::uint64_t now) const
        {
            return std::any_of(ignored.begin(), ignored.end(),
                [&](const IgnoredHandle& entry) { return entry.handle == handle && now < entry.until; });
        }

        // Whether an object is already a recorded weapon or prop.
        bool KnowsObject(Entity object) const
        {
            return std::any_of(weapons.begin(), weapons.end(),
                       [&](const RecordedWeapon& weapon) { return weapon.object == object; }) ||
                std::any_of(props.begin(), props.end(),
                    [&](const RecordedProp& prop) { return prop.object == object; });
        }
    };
}
