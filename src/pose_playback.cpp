#include "pose_playback.h"

#include "weapon_components.h"
#include "weapon_order_core.h" // the wraps last
#include "audio_observer.h"

#include <algorithm>

#include "enums.h"
#include "main.h"
#include "natives.h"

namespace replay::pose::playback
{
    bool RequestModel(Hash model)
    {
        if (!STREAMING::IS_MODEL_VALID(model))
            return false;
        if (STREAMING::HAS_MODEL_LOADED(model))
            return true;
        STREAMING::REQUEST_MODEL(model, FALSE);
        return false;
    }

    void ReleaseModel(Hash model)
    {
        STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
    }

    Ped CreatePuppet(Hash model, bool horse, const WorldTransform& at)
    {
        if (!STREAMING::HAS_MODEL_LOADED(model))
            return 0;
        Ped puppet = PED::CREATE_PED(
            model, at.position.x, at.position.y, at.position.z, 0.0f,
            FALSE, FALSE, FALSE, FALSE);
        if (!puppet || !ENTITY::DOES_ENTITY_EXIST(puppet))
            return 0;

        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(puppet, TRUE, TRUE);
        PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(puppet, TRUE);
        PED::SET_PED_CAN_RAGDOLL(puppet, FALSE);
        // The model's default loadout would draw its own holstered weapons;
        // the puppet carries only the recorded weapon objects.
        WEAPON::REMOVE_ALL_PED_WEAPONS(puppet, TRUE, TRUE);
        if (horse)
            PED::_SET_HORSE_SCRIPTED_FLAG(puppet, TRUE);
        ENTITY::SET_ENTITY_INVINCIBLE(puppet, TRUE);
        ENTITY::SET_ENTITY_COLLISION(puppet, FALSE, FALSE);
        // Frozen: the recorded root is the only thing that moves it.
        ENTITY::FREEZE_ENTITY_POSITION(puppet, TRUE);
        ENTITY::SET_ENTITY_VISIBLE(puppet, FALSE);
        PlaceClone(puppet, at);
        return puppet;
    }

    Vehicle CreateVehiclePuppet(Hash model, int tint, int livery, const WorldTransform& at)
    {
        if (!STREAMING::HAS_MODEL_LOADED(model))
            return 0;
        Vehicle vehicle = VEHICLE::CREATE_VEHICLE(
            model, at.position.x, at.position.y, at.position.z, 0.0f,
            FALSE, FALSE, TRUE, FALSE);
        if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle))
            return 0;
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(vehicle, TRUE, TRUE);
        ENTITY::SET_ENTITY_INVINCIBLE(vehicle, TRUE);
        ENTITY::SET_ENTITY_COLLISION(vehicle, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(vehicle, TRUE);
        ENTITY::SET_ENTITY_VISIBLE(vehicle, FALSE);
        if (tint >= 0)
            VEHICLE::_SET_VEHICLE_TINT(vehicle, tint);
        if (livery >= 0)
            VEHICLE::_SET_VEHICLE_LIVERY(vehicle, livery);
        PlaceClone(vehicle, at);
        return vehicle;
    }

    bool RequestPropSet(Hash propSet)
    {
        if (!propSet)
            return true;
        if (PROPSET::_HAS_PROP_SET_LOADED(propSet))
            return true;
        PROPSET::_REQUEST_PROP_SET(propSet);
        return false;
    }

    std::uint32_t VehicleExtrasOn(Vehicle vehicle, std::uint32_t extrasExist)
    {
        std::uint32_t on = 0;
        if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle))
            return on;
        for (int extra = 0; extra < 32; ++extra)
        {
            if (((extrasExist >> extra) & 1u) && VEHICLE::IS_VEHICLE_EXTRA_TURNED_ON(vehicle, extra))
                on |= 1u << extra;
        }
        return on;
    }

    void ApplyVehicleLook(Vehicle vehicle, Hash propSet, Hash lightPropSet,
        std::uint32_t extrasExist, std::uint32_t extrasOn)
    {
        if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle))
            return;
        // SET_VEHICLE_EXTRA's third argument disables the extra. Only the extras
        // whose state differs: switching an extra that is already right runs the
        // engine's extra toggle (0x15DF7FC) for nothing.
        for (int extra = 0; extra < 32; ++extra)
        {
            if (!((extrasExist >> extra) & 1u))
                continue;
            const bool on = ((extrasOn >> extra) & 1u) != 0;
            if ((VEHICLE::IS_VEHICLE_EXTRA_TURNED_ON(vehicle, extra) != FALSE) != on)
                VEHICLE::SET_VEHICLE_EXTRA(vehicle, extra, on ? FALSE : TRUE);
        }
        // The model may come with its own default cargo or lanterns.
        PROPSET::_REMOVE_VEHICLE_PROP_SETS(vehicle);
        PROPSET::_REMOVE_VEHICLE_LIGHT_PROP_SETS(vehicle);
        if (propSet)
            PROPSET::_ADD_PROP_SET_FOR_VEHICLE(vehicle, propSet);
        if (lightPropSet)
            PROPSET::_ADD_LIGHT_PROP_SET_TO_VEHICLE(vehicle, lightPropSet);
    }

    bool RequestWeaponAsset(Hash weapon)
    {
        if (!WEAPON::IS_WEAPON_VALID(weapon))
            return false;
        if (WEAPON::_HAS_WEAPON_ASSET_LOADED(weapon))
            return true;
        WEAPON::_REQUEST_WEAPON_ASSET(weapon, 31, FALSE);
        return false;
    }

    void ReleaseWeaponAsset(Hash weapon)
    {
        WEAPON::_REMOVE_WEAPON_ASSET(weapon);
    }

    Object CreateWeaponPuppet(Hash weapon, Hash model, const WorldTransform& at)
    {
        Object object = weapon
            ? WEAPON::_CREATE_WEAPON_OBJECT(
                weapon, 0, at.position.x, at.position.y, at.position.z, TRUE, 1.0f)
            : OBJECT::CREATE_OBJECT(
                model, at.position.x, at.position.y, at.position.z, FALSE, FALSE, FALSE, FALSE, FALSE);
        if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
            return 0;
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
        ENTITY::SET_ENTITY_INVINCIBLE(object, TRUE);
        ENTITY::SET_ENTITY_COLLISION(object, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(object, TRUE);
        ENTITY::SET_ENTITY_VISIBLE(object, FALSE);
        PlaceClone(object, at);
        return object;
    }

    bool RequestWeaponComponents(const std::vector<std::uint32_t>& components)
    {
        bool all = true;
        for (const std::uint32_t component : components)
        {
            const Hash model = WEAPON::_GET_WEAPON_COMPONENT_TYPE_MODEL(component);
            if (model && STREAMING::IS_MODEL_VALID(model) && !RequestModel(model))
                all = false;
        }
        return all;
    }

    void ApplyWeaponComponents(Object object, Hash weapon, const std::vector<std::uint32_t>& components,
        std::uint32_t& added, std::uint32_t& removed)
    {
        added = 0;
        removed = 0;
        if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
            return;
        // weapon_order_core.h: every component before the wraps, so a wrap is
        // built over the final set (the bolt-action wrap over its scope).
        const auto has = [&](std::uint32_t hash) { return WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(object, hash) != FALSE; };
        const auto isWrap = [](std::uint32_t hash) {
            for (const replay::weapon::Component& candidate : replay::weapon::kComponents)
            {
                if (candidate.hash == hash)
                    return replay::weaponorder::IsWrapName(candidate.name);
            }
            return false;
        };
        for (const replay::weaponorder::GiveStep& step : replay::weaponorder::PlanGive(components, has, isWrap))
        {
            if (step.removeFirst)
                WEAPON::REMOVE_WEAPON_COMPONENT_FROM_WEAPON_OBJECT(object, step.hash);
            WEAPON::_GIVE_WEAPON_COMPONENT_TO_ENTITY(object, step.hash, weapon, TRUE);
            ++added;
        }
        for (const replay::weapon::Component& candidate : replay::weapon::kComponents)
        {
            if (std::find(components.begin(), components.end(), candidate.hash) != components.end() ||
                !WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(object, candidate.hash))
            {
                continue;
            }
            WEAPON::REMOVE_WEAPON_COMPONENT_FROM_WEAPON_OBJECT(object, candidate.hash);
            ++removed;
        }
    }

    void DeleteObjectPuppet(Object& object)
    {
        if (object && ENTITY::DOES_ENTITY_EXIST(object))
        {
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            OBJECT::DELETE_OBJECT(&object);
        }
        object = 0;
    }

    bool WeaponMuzzle(Object weapon, Vector3& position)
    {
        if (!weapon || !ENTITY::DOES_ENTITY_EXIST(weapon))
            return false;
        const int bone = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(weapon, "Gun_Muzzle");
        if (bone < 0)
            return false;
        position = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(weapon, bone);
        return true;
    }

    bool IsReadyToRender(Ped ped)
    {
        return ped && ENTITY::DOES_ENTITY_EXIST(ped) && PED::IS_PED_READY_TO_RENDER(ped);
    }

    void PlaceClone(Ped clone, const WorldTransform& at)
    {
        if (!clone || !ENTITY::DOES_ENTITY_EXIST(clone))
            return;
        ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
            clone, at.position.x, at.position.y, at.position.z, FALSE, FALSE, FALSE);
        ENTITY::SET_ENTITY_QUATERNION(
            clone, at.rotation.x, at.rotation.y, at.rotation.z, at.rotation.w);
    }

    void MoveHiddenEntity(Entity entity, float x, float y, float z)
    {
        if (entity && ENTITY::DOES_ENTITY_EXIST(entity))
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, z, FALSE, FALSE, FALSE);
    }

    void SetCloneVisible(Ped clone, bool visible)
    {
        if (clone && ENTITY::DOES_ENTITY_EXIST(clone))
            ENTITY::SET_ENTITY_VISIBLE(clone, visible ? TRUE : FALSE);
    }

    void DeleteClone(Ped& clone)
    {
        if (clone && ENTITY::DOES_ENTITY_EXIST(clone))
        {
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(clone, TRUE, TRUE);
            PED::DELETE_PED(&clone);
        }
        clone = 0;
    }

    void DeleteVehiclePuppet(Vehicle& vehicle)
    {
        if (vehicle && ENTITY::DOES_ENTITY_EXIST(vehicle))
        {
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(vehicle, TRUE, TRUE);
            VEHICLE::DELETE_VEHICLE(&vehicle);
        }
        vehicle = 0;
    }

    bool AttachDraftHarness(Ped animal, Vehicle vehicle, int harness)
    {
        if (!animal || !vehicle || !ENTITY::DOES_ENTITY_EXIST(animal) ||
            !ENTITY::DOES_ENTITY_EXIST(vehicle))
        {
            return false;
        }
        return VEHICLE::_ATTACH_DRAFT_VEHICLE_HARNESS_PED(animal, vehicle, harness) != FALSE;
    }

    void DetachDraftHarness(Vehicle vehicle, Ped animal)
    {
        if (vehicle && animal && ENTITY::DOES_ENTITY_EXIST(vehicle) && ENTITY::DOES_ENTITY_EXIST(animal))
            VEHICLE::_DETACH_DRAFT_VEHICLE_HARNESS_PED(vehicle, animal);
    }

    bool IsMissionEntity(Entity entity)
    {
        return ENTITY::IS_ENTITY_A_MISSION_ENTITY(entity) != FALSE;
    }

    // Deletion needs ownership. The delete natives clear the handle even
    // when the engine keeps the entity, so the result is checked on the
    // original handle; an entity that survives is handed back to the
    // population (no longer needed) instead of staying a mission entity.
    template <typename DeleteFn>
    bool DeleteAmbientEntity(Entity entity, DeleteFn deleteFn)
    {
        if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity) || IsMissionEntity(entity))
            return false;
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(entity, TRUE, TRUE);
        if (!IsMissionEntity(entity))
            return false;
        Entity handle = entity;
        deleteFn(handle);
        if (!ENTITY::DOES_ENTITY_EXIST(entity))
            return true;
        Entity release = entity;
        ENTITY::SET_ENTITY_AS_NO_LONGER_NEEDED(&release);
        return false;
    }

    bool DeleteAmbientPed(Ped ped)
    {
        return DeleteAmbientEntity(ped, [](Entity& handle) { PED::DELETE_PED(&handle); });
    }

    bool DeleteAmbientVehicle(Vehicle vehicle)
    {
        return DeleteAmbientEntity(vehicle, [](Entity& handle) { VEHICLE::DELETE_VEHICLE(&handle); });
    }

    bool HideWorldPed(Ped ped, HiddenPedState& state)
    {
        if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
            return false;
        state.ped = ped;
        state.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(ped));
        state.visible = ENTITY::IS_ENTITY_VISIBLE(ped) != FALSE;
        state.collisionDisabled = ENTITY::GET_ENTITY_COLLISION_DISABLED(ped) != FALSE;
        state.frozen = ENTITY::_IS_ENTITY_FROZEN(ped) != FALSE;
        ENTITY::SET_ENTITY_VISIBLE(ped, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(ped, TRUE);
        ENTITY::SET_ENTITY_COLLISION(ped, FALSE, FALSE);
        // The trail manager holds 128 records world-wide and a full one refuses
        // the puppets' (0xC26100, cmp [mgr+0xC], 0x80; no eviction). A hidden
        // entity keeps its record (only a deleted one is freed), and a busy scene
        // can hide hundreds of world peds and objects. Its record is given up
        // (entity+0xB4 bit 22: has a record) and back on restore.
        std::uint32_t trailFlags = 0;
        const auto address = reinterpret_cast<std::uint64_t>(getScriptHandleBaseAddress(ped));
        state.hadTrail = address && replay::audio::ReadBytes(address + 0xB4, &trailFlags, sizeof(trailFlags)) &&
            ((trailFlags >> 22) & 1u) != 0;
        if (state.hadTrail)
            ENTITY::_0x5826EFD6D73C4DE5(ped);
        return true;
    }

    bool StillHiddenEntity(const HiddenPedState& state)
    {
        if (!state.ped || !ENTITY::DOES_ENTITY_EXIST(state.ped))
            return false;
        return !state.model || static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(state.ped)) == state.model;
    }

    void RestoreWorldPed(const HiddenPedState& state)
    {
        if (!StillHiddenEntity(state))
            return;
        ENTITY::SET_ENTITY_COLLISION(state.ped, state.collisionDisabled ? FALSE : TRUE, TRUE);
        ENTITY::FREEZE_ENTITY_POSITION(state.ped, state.frozen ? TRUE : FALSE);
        ENTITY::SET_ENTITY_VISIBLE(state.ped, state.visible ? TRUE : FALSE);
        if (state.hadTrail)
            ENTITY::_ADD_ENTITY_TRACKING_TRAILS(state.ped);
    }

    // The restore in two steps (world_landing.h). The entity is
    // shown at once, keeps its freeze and has no collision until the map
    // collision around it is in.
    void BeginWorldPedLanding(const HiddenPedState& state)
    {
        if (!StillHiddenEntity(state))
            return;
        ENTITY::SET_ENTITY_VISIBLE(state.ped, state.visible ? TRUE : FALSE);
    }

    bool WorldPedCollisionLoaded(Entity entity, bool request)
    {
        if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity))
            return false;
        if (request)
        {
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
            STREAMING::REQUEST_COLLISION_AT_COORD(at.x, at.y, at.z);
        }
        return ENTITY::HAS_COLLISION_LOADED_AROUND_ENTITY(entity) != FALSE;
    }

    void LandWorldPedCollision(const HiddenPedState& state)
    {
        if (StillHiddenEntity(state))
            ENTITY::SET_ENTITY_COLLISION(state.ped, state.collisionDisabled ? FALSE : TRUE, TRUE);
    }

    Entity MountOfWorldPed(Entity entity)
    {
        if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity) || !ENTITY::IS_ENTITY_A_PED(entity) || !PED::IS_PED_ON_MOUNT(entity))
            return 0;
        return PED::GET_MOUNT(entity);
    }

    // A game script may show, unfreeze or give collision back to a
    // hidden entity (the camp, a mission); each one is put back.
    std::uint32_t RehideWorldPed(Entity entity)
    {
        if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity))
            return 0;
        std::uint32_t fixed = 0;
        if (ENTITY::IS_ENTITY_VISIBLE(entity))
        {
            ENTITY::SET_ENTITY_VISIBLE(entity, FALSE);
            fixed |= kRehideVisible;
        }
        if (!ENTITY::_IS_ENTITY_FROZEN(entity))
        {
            ENTITY::FREEZE_ENTITY_POSITION(entity, TRUE);
            fixed |= kRehideFrozen;
        }
        if (!ENTITY::GET_ENTITY_COLLISION_DISABLED(entity))
        {
            ENTITY::SET_ENTITY_COLLISION(entity, FALSE, FALSE);
            fixed |= kRehideCollision;
        }
        return fixed;
    }

    void SuppressPopulationThisFrame()
    {
        PED::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        PED::_SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        PED::_SET_SCENARIO_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        PED::_SET_SCENARIO_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        PED::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        PED::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        // Vehicles too (wagons, coaches): the world clear keeps population out.
        VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        VEHICLE::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
        VEHICLE::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    }
}
