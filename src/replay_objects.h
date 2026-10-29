#pragma once

// Weapon and prop objects (the reference object packets), recording and
// playback.
//
// Recording: every object at a recorded ped's weapon attach points is an
// entity of its own with a WeaponPoint attachment, sampled in the ped's
// frame; strap state and attach frame changes are tracked; a weapon that
// leaves its ped continues as a dropped weapon prop (same object, or the
// replacement object/pickup found where it was); a dropped hat is a prop
// created when it leaves the ped, sampled by its world transform.
//
// Playback: weapon puppets are built by the weapon system once their
// parent puppet exists and ride it as an engine attachment with the
// recorded frame, so the engine draws their straps; prop puppets are
// knocked off the owner puppet (hats) or built as weapon objects (dropped
// weapons) and placed by their recorded world transform.

#include "replay_context.h"
#include "vehicle_pieces.h"

#include <cstdint>
#include <vector>

namespace replay::objects
{
    void Attach(Context& context);
    // Strap visibility detour (installed once, first capture or playback).
    void EnsureStrapVisibility();
    // Installs the strap and frag detours (inside the mod-load hook batch) so
    // the first recording enables nothing.
    void WarmUp();
    bool StrapVisibilityReady();

    // Recording side, called by the recorder core.
    void BeginRecording();
    void EndRecording();
    void Scan();                // weapon objects at the recorded peds' attach points
    void Sample();              // one relative sample per weapon at the capture rate
    void UpdateRecordedProps(); // new dropped hats, one world sample per prop
    void SearchDrops();         // replacement objects of dropped weapons
    // While the recording gate holds, the frag detour's queue is drained and
    // discarded (a break in a cutscene is not in the clip).
    void DiscardPendingEvents();
    // A severed limb object the engine created next to a recorded ped: recorded
    // as a prop owned by the ped. False when known or full.
    bool RegisterGorePiece(Object object, Entity ownerHandle);
    void RetireWeaponsOf(Entity parent, const char* reason);
    // Capture places for posed objects (ini ObjectPosePlaces, 0..8), and how many
    // of them the ped class gives up now (the posed objects and those waiting for
    // a place). See object_pose.h.
    void SetObjectPosePlaces(std::uint32_t places);
    // Ini WeaponBonePose (default 1) and WeaponBonePlaces (default 4, 0..8): a real
    // weapon in a hand gets a pose record of its own bones (the reload's lever,
    // pump, cylinder, gate, hammer) from the moment it is in the hand, in every
    // capture slot the bodies do not use (object_pose.h WeaponPlaces); and, for
    // the fx check, the weapons with a recorded pose this playback reached and armed.
    void SetWeaponBonePose(bool on);
    void WeaponBonesPlayed(std::uint32_t& seen, std::uint32_t& armed);
    // The drawn check of the armed weapon poses (WEAPON_BONES_DRAWN lines and
    // _TOTAL, written here) and the ini key WeaponPassPose (default 1: the pose
    // written again after the weapon system's per-frame pass, detour 0x117D49C).
    void WeaponBonesDrawn(std::uint32_t& overwritten, std::uint32_t& mixed, std::uint32_t& shown, std::uint32_t& none);
    void SetWeaponPassPose(bool on);
    // Weapons with a bone the record turns and the drawing does not (frozen_bones
    // in WEAPON_BONES_DRAWN), from the last WeaponBonesDrawn.
    std::uint32_t WeaponBonesFrozen();
    // The weapon looks read back this playback (weapons checked, last check
    // disagreeing on components, on wear), with the WEAPON_LOOK_TOTAL line; the fx
    // check turns them into weapon_components_mismatch and weapon_wear_mismatch.
    // See weapon_look_core.h.
    void WeaponLookPlayed(std::uint32_t& checked, std::uint32_t& componentsMismatch, std::uint32_t& wearMismatch);
    // ini WeaponWearWrite (default 1): rust (+0xD4) and the sixth wear
    // channel (+0xE4), which no native sets, written on the replay's own
    // weapon objects (guarded plain writes); 0: only the natives' four.
    void SetWeaponWearWrite(bool on);
    // Ini AmmoScanEveryTick (default 1): while a recorded ped holds a weapon, the
    // objects new in the pool since the last tick are asked for their parent every
    // tick, so the cartridge taken into the hand enters the clip at the start of its life.
    void SetAmmoScanEveryTick(bool on);
    // Ini HatLook (default 1): a hat object records the layer it draws and playback
    // rebuilds it with that look (owner knock after the layer lands, else the hat
    // dresser); a hat model without a look is not drawn. 0: the bare model.
    void SetHatLook(bool on);
    std::uint32_t ObjectPoseReservedPlaces();
    // Free objects the engine creates next to a recorded ped during the recording
    // are recorded once they move or animate (ini SpawnedObjects, 1 by default; 0:
    // watched and logged only, status=would_record). See object_pose.h.
    void SetSpawnedObjects(bool enabled);
    bool SpawnedObjects();
    // The burn level of the recorded weapon objects, props and scenery objects
    // (BurnLevelTrack: recorded, and written on their puppets and stand-ins in
    // playback; ini ObjectBurnLevel, 1 by default; 0: neither, BLVL tracks of
    // vehicles, peds and horses still play).
    void SetObjectBurnLevel(bool enabled);
    bool ObjectBurnLevel();
    // StillPoseEngine (1 by default): an attached object whose pose record never
    // changed is not pose-armed; the engine poses it by its attach as live.
    // ObjectDrawnCheck (0 by default): each tick, the pose the last frame drew on a
    // posed or still object against the expected one and the bind pose
    // (POSE_V2_OBJECT_DRAWN, POSE_V2_OBJECT_DRAWN_SUMMARY, write_checks).
    // See still_pose_core.h.
    void SetStillPose(bool engineRule, bool drawnCheck);
    bool StillPoseEngine();
    bool ObjectDrawnCheck();
    // Ini SceneryHandoffTicks (0..8, default 1): a scenery or world object
    // stand-in stays shown until the prop puppet that takes it over has been posed
    // for more than this many ticks (one frame of overlap, no frame without
    // either); 0: hidden in the tick the prop is Ready (objcheck::HandoffPropReady).
    void SetSceneryHandoffTicks(std::uint32_t ticks);
    std::uint32_t SceneryHandoffTicks();
    // Such an object that stands still for a second becomes a late scenery entry
    // (ini SpawnedStaticScenery, objcheck::SpawnSceneryOwner): 1 (default) when its
    // owner is a proven scenery owner (script, temporary, prop set: a camp's tent
    // and grate) or it is a pickup; 2 also any other owner that is not the map's;
    // 0 dropped as static. Clamped to 0..2.
    void SetSpawnedStaticScenery(int mode);
    int SpawnedStaticScenery();
    // The object at an engine address on an end of a recorded rope (the fishing
    // bobber) becomes a prop with a world track, owned by the recorded ped `owner`
    // or by the holder of the recorded weapon `owner`. Invalid id when it is no
    // listed object, already recorded, a static map object from before the
    // recording, or has no owner; `status` names the outcome.
    replay::entity::ReplayId RegisterRopeEndObject(std::uint64_t address, replay::entity::ReplayId owner, const char*& status);

    // Playback side, called by the playback core.
    void BeginPlayback();
    // Stand-ins deleted, hidden world objects given back.
    void EndPlayback();
    // A script restart with a playback open: its stand-ins and attach helper
    // forgotten without a native (the count).
    std::uint32_t ForgetPlaybackAfterRestart();
    void UpdateWeaponPuppets(std::uint32_t timeMs, std::uint32_t& creates);
    void PublishStrapOwned();
    void UpdatePropPuppets(std::uint32_t timeMs, std::uint32_t& creates);
    // The pose record of another kind of puppet (a train car) armed and written
    // like a posed object's: its record id is its own id, its bones the create's;
    // nothing when the clone's skeleton has another bone count. Script thread.
    void PlayRecordedPose(replay::playback::Puppet& puppet, std::uint32_t timeMs);
    // World and scenery stand-ins due at the playhead and not built yet; the
    // playback start waits for them (object_check.h).
    std::uint32_t PendingObjectStands(std::uint32_t timeMs);
    // A gore event of `owner` recorded at hitMs is about to be replayed
    // again (after a rewind): the pieces it dropped (created within a
    // second after it) give up their current object, which is deleted, and
    // adopt the piece the engine drops now.
    void ReleaseGorePieces(replay::entity::ReplayId owner, std::uint32_t hitMs);
    // The vehicle pieces of `vehicle` (PropOrigin::VehiclePiece) give up their
    // object when the vehicle puppet is built again whole: each one breaks off the
    // new puppet again (vehicle_pieces.h).
    void ReleaseVehiclePieces(replay::entity::ReplayId vehicle);
    // A recorded piece's group broken off the vehicle puppet at the group's own
    // break (`entity`: the object BreakOffVehicleGroup made), kept hidden for the
    // piece's puppet; true when kept.
    bool KeepVehicleFragPiece(replay::entity::ReplayId vehicle, std::uint16_t group, std::uint64_t entity, std::uint32_t timeMs);
    // The state of every vehicle piece puppet, for the vehicle's own group
    // deletion and the wheel and door natives (vehicle_pieces.h).
    void VehiclePieceStates(std::vector<replay::vehicle_pieces::PieceState>& out);
    // Recording (vehicle_pieces.h WheelPieceGroup): the part track sampler asks
    // whether a vehicle must be sampled at once (a break without a fragment piece,
    // taken once), and tells a new wheel bit (index, engine slot): its wheel object
    // becomes a vehicle piece with its own track.
    bool TakeVehiclePartsUrgent(replay::entity::ReplayId vehicle);
    void NoteVehicleWheelOff(replay::entity::ReplayId vehicle, Entity vehicleHandle, int wheel, int slot);
    // Playback: the wheel _BREAK_OFF_VEHICLE_WHEEL returned from a vehicle
    // puppet is kept hidden when the clip has a use for it (true; false: the
    // caller deletes it); the clip's wheel pieces at the playhead; an object
    // puppet's object given back at its span's end (true: kept for the next
    // puppet of its model, the caller must not delete it).
    bool KeepVehicleWheel(replay::entity::ReplayId vehicle, int wheel, Object piece, std::uint32_t timeMs);
    bool ClipHasWheelPieces();
    // The wheel models of a vehicle puppet read once (a plain object puppet
    // of one waits for a wheel object instead of a fresh model).
    void NoteVehiclePuppetWheels(Entity vehicleClone);
    bool ReturnWheelObject(Object object);
    // vehicle_pieces.h wheel children: the wheels the vehicle puppet keeps at the
    // playhead because a recorded child that is one of them is attached to the
    // vehicle (bit n: wheel n); 0 for any other vehicle.
    std::uint16_t KeptVehicleWheels(const replay::playback::Puppet& vehicle, std::uint32_t timeMs);
    // The wheels whose recorded child left the vehicle (its object went on as a
    // loose prop of the vehicle) at or before the playhead.
    std::uint16_t LeftVehicleWheels(const replay::playback::Puppet& vehicle, std::uint32_t timeMs);
    // A fragment piece was broken off one of this vehicle's wheel objects (its own
    // or one a loose wheel holds) at a playhead after timeMs: the puppet must be
    // built whole again (the wheel lost those groups).
    bool VehicleWheelPiecesAhead(replay::entity::ReplayId vehicle, std::uint32_t timeMs);
    // A vehicle puppet with wheels that has made none of its wheel objects yet
    // (the engine makes them a frame or two after the vehicle): the start of a
    // playback holds for them, at most a second per puppet.
    bool VehicleWheelsPending(const replay::playback::Puppet& vehicle);
#if defined(REDEMPTION_DEV)
    // Bench (object_census): one line per recorded object, attached object and
    // vehicle within `radius` of `center` (recording) or per puppet of them
    // (playback), with its place, visibility and screen point, and per vehicle
    // its wheel slot objects and lost wheels: live and replay compared by id.
    void BenchObjectCensus(const std::string& tag, bool playback, std::uint32_t timeMs, const float center[3], float radius);
    // Bench: a stand-in that moved since the last call (it is frozen where it
    // stood), logged with the tick phase `where`.
    void BenchWatchStands(const char* where, std::uint32_t timeMs);
    // Bench (loot_watch): every object within `radius` of `center`, one
    // BENCH_LOOT_OBJ line per change (seen, gone, visibility, attach, move,
    // skeleton, bone pose), sampled every 100 ms.
    void BenchLootWatchReset();
    void BenchLootWatch(const float center[3], float radius);
#endif
    // Severed pieces are found without the ScriptHook pool listing (it returns 0
    // when the script handle pool has <= 256 free slots, which a large fight
    // reaches): the engine drops them with a few generic models, learned from every
    // piece registered and seeded from the logs.
    bool IsGorePieceModel(std::uint32_t model);
    // The nearest loose object of `model` within radius of `at` that no
    // puppet owns (pool listing when it answers, closest-of-type otherwise).
    Object FindLooseObject(std::uint32_t model, const Vector3& at, float radius, bool excludePuppets);
    // A gore clear on a puppet (rewind before its cut): delete the loose
    // pieces of the known gore models around it that no puppet owns. They
    // are what the engine dropped when the cut was replayed; nothing else
    // removes them. Returns how many were deleted.
    int DeleteStrayGorePieces(Entity around, float radius);
    // The ScriptHook pool listings, through one switch: the bench turns them
    // off (`pool_listing off`) to exercise the paths a large fight or a town
    // takes when the listing returns 0.
    int ListObjects(int* handles, int capacity);
    // Every object of the world (worldGetAllObjects, a handle for each) only while
    // the script handle pool has room for all of them beyond the reserve left to
    // the game (pools::GuidRoom); otherwise the objects that already have a handle,
    // none made. `mode` says which: "all", "existing_only" or "listing" (no pool reader).
    int ListAllObjectsGuarded(int* handles, int capacity, const char*& mode);
    int ListPickups(int* handles, int capacity);
    // The recorder's ped and vehicle candidate listings (the same switch;
    // entity_discovery.h covers a listing of 0).
    int ListPeds(int* handles, int capacity);
    int ListVehicles(int* handles, int capacity);
    void SetPoolListingDisabled(bool disabled);
    // The objects the replay made or hides itself (stand-ins, the originals the
    // stands hide, the attach helper), sorted, for the world clear's live object
    // hide (object_check.h HideLiveObject); and the engine owner of an entity
    // (5 bits of [entity+0xB4], objcheck::kOwnerUnknown when unread).
    void ReplayOwnObjects(std::vector<Object>& out);
    std::uint32_t EntityOwnerOf(Entity entity);
    // Replay objects are props, not gameplay pickups: no pickup light.
    void SuppressPickupLook(Object object);
    void DeleteAttachHelper();
}
