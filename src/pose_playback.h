#pragma once

#include <cstdint>
#include <vector>

// PoseTrack v2: playback clones.
//
// Every actor native used by pose playback lives here so the architecture
// audit can hold it to one contract: puppets are created from the recorded
// model (appearance rebuilt by entity_appearance), made inert (no AI events, ragdoll, collision, damage or physics
// drift), placed each frame at the engine world transform recorded in the
// same snapshot as their pose, and deleted on stop. No task, weapon,
// velocity or force native is allowed. The skeleton pose itself is written
// by the finalizer detour in pose_capture.cpp.

#include "pose_math.h"
#include "world_landing.h"

#include "types.h"

namespace replay::pose::playback
{
    // Model streaming for puppets (request every frame until loaded).
    bool RequestModel(Hash model);
    void ReleaseModel(Hash model);

    // Creates an inert puppet of `model` (loaded) at `at`, hidden until its
    // appearance is rebuilt. Returns 0 on failure.
    Ped CreatePuppet(Hash model, bool horse, const WorldTransform& at);
    // Inert vehicle puppet, without the draft animals the engine would add
    // (those are recorded entities of their own).
    Vehicle CreateVehiclePuppet(Hash model, int tint, int livery, const WorldTransform& at);
    bool IsReadyToRender(Ped ped);
    void PlaceClone(Ped clone, const WorldTransform& at);
    // Moves a hidden (invisible, frozen, collision-off) entity, the real player
    // during a replay (TrailPlayerFollow).
    void MoveHiddenEntity(Entity entity, float x, float y, float z);
    void SetCloneVisible(Ped clone, bool visible);
    void DeleteClone(Ped& clone);
    void DeleteVehiclePuppet(Vehicle& vehicle);
    // Streams a recorded prop set (cargo or lanterns); true when it is loaded or
    // there is none.
    bool RequestPropSet(Hash propSet);
    // Replaces the prop sets CREATE_VEHICLE put on the puppet with the
    // recorded ones and sets the recorded extras.
    void ApplyVehicleLook(Vehicle vehicle, Hash propSet, Hash lightPropSet,
        std::uint32_t extrasExist, std::uint32_t extrasOn);
    // The extras of `extrasExist` the vehicle has turned on (bit n: extra n), e.g.
    // the ones CREATE_VEHICLE picked at random.
    std::uint32_t VehicleExtrasOn(Vehicle vehicle, std::uint32_t extrasExist);

    // Weapon objects: the weapon asset is streamed and the object built by the
    // weapon system, or a plain object of `model` when no weapon is recorded.
    // Inert, hidden, frozen.
    bool RequestWeaponAsset(Hash weapon);
    void ReleaseWeaponAsset(Hash weapon);
    Object CreateWeaponPuppet(Hash weapon, Hash model, const WorldTransform& at);
    // The recorded weapon components: their models are streamed first
    // (true when all are in), then given to the object, and the default
    // components the recording did not have are removed.
    bool RequestWeaponComponents(const std::vector<std::uint32_t>& components);
    void ApplyWeaponComponents(Object object, Hash weapon, const std::vector<std::uint32_t>& components,
        std::uint32_t& added, std::uint32_t& removed);
    void DeleteObjectPuppet(Object& object);

    // World position of a weapon object's muzzle bone; false without one.
    bool WeaponMuzzle(Object weapon, Vector3& position);

    // Recorded draft harness between puppets; the engine then draws the
    // harness and reins.
    bool AttachDraftHarness(Ped animal, Vehicle vehicle, int harness);
    void DetachDraftHarness(Vehicle vehicle, Ped animal);

    // Recorded seat: a ped in a vehicle is put into its seat; its position then
    // comes from the vehicle and only the bones from the recording.

    // Playback world: ambient population (not mission entities, not the player's)
    // is deleted, which also frees ped pool room for puppets; every other ped and
    // vehicle is hidden, frozen and made non-colliding, and restored when playback
    // stops.
    struct HiddenPedState
    {
        Ped ped = 0;
        bool visible = true;
        bool collisionDisabled = false;
        bool frozen = false;
        // Its snow and mud trail record, given up while hidden.
        bool hadTrail = false;
        // Its model when hidden: a handle whose entity its owner deleted
        // meanwhile (another mod's prop) and the engine gave to another
        // entity is not that entity (StillHiddenEntity).
        std::uint32_t model = 0;
    };

    // The hidden entity is still there and still the one hidden (the same
    // model); every restore step asks it before touching the handle.
    bool StillHiddenEntity(const HiddenPedState& state);

    bool HideWorldPed(Ped ped, HiddenPedState& state);
    bool IsMissionEntity(Entity entity);
    // Ambient entity removal; returns false when the entity survived.
    bool DeleteAmbientPed(Ped ped);
    bool DeleteAmbientVehicle(Vehicle vehicle);
    void RestoreWorldPed(const HiddenPedState& state);
    // The restore in steps (world_landing.h). Shown at once; the collision back
    // once the map collision around it is loaded (REQUEST_COLLISION_AT_COORD at its
    // position when `request`), then the freeze state and the rest (RestoreWorldPed).
    void BeginWorldPedLanding(const HiddenPedState& state);
    bool WorldPedCollisionLoaded(Entity entity, bool request);
    void LandWorldPedCollision(const HiddenPedState& state);
    // The mount of a ped riding one (0 otherwise, or not a ped).
    Entity MountOfWorldPed(Entity entity);
    // The re-hide of an entity already hidden checks the freeze and the collision
    // as well as the visibility; returns what it put back.
    constexpr std::uint32_t kRehideVisible = 1u;
    constexpr std::uint32_t kRehideFrozen = 2u;
    constexpr std::uint32_t kRehideCollision = 4u;
    std::uint32_t RehideWorldPed(Entity entity);
    // The owner's change seen by a re-hide becomes what is given back
    // (world_landing.h NoteOwnerChange).
    inline std::uint32_t NoteOwnerChange(HiddenPedState& state, std::uint32_t fixed)
    {
        return replay::landing::NoteOwnerChange(state.visible, state.frozen, state.collisionDisabled, fixed);
    }
    // No ambient or scenario population spawns this frame.
    void SuppressPopulationThisFrame();
}
