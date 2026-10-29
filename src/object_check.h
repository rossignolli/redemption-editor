#pragma once

// The rules of the world objects as state packets, pure and unit tested in
// ReplayCoreTests (object_check_tests.cpp), shared by the mod and the clip
// tool (ReplayClipTool objects).
//
// A replay editor usually records an object of the map only when it changes
// (moves, breaks, animates) and leaves the rest to the map; a script or
// temporary object (not of the map) is recorded as an object of its own with
// its create and delete, and the replay hides the original of every object it
// plays (deferred when the original is not there yet). Here:
//   - scenery: the objects of the scene that are not of the map (a camp's
//     props, created by its script) recorded as state: model, transform,
//     when they appeared and left. Playback builds them frozen, hides an
//     original still in the world, and the start waits for them;
//   - an animated world object (the camp's donation box and ledger) is
//     recorded when its bones move, with its pose;
//   - a weapon object held by script outside the ped's weapon slots (the
//     five finger fillet knife) carries its weapon and components, read from
//     the object itself;
//   - loot and pickups present before the take (the shelf's loot, a weapon
//     on the ground) are scenery entries too, with the take as a visibility
//     flip or a leaving time;
//   - a prop set's objects (owner 23) are scenery, a still object the
//     spawned object watch saw born next to a recorded ped is a late entry
//     when it is scenery (ini SpawnedStaticScenery=2: whatever its owner
//     unless it is the map's), and an object first listed out of the scenery
//     radius is looked at again until it comes within it;
//   - verdicts the QA ring and the clip tool report (object_wrong).

#include "entity_lifecycle.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace replay::objcheck
{
    // ---- engine layout (build 1491.50, offline dump) ------------------------

    // CObject's weapon: HAS_WEAPON_GOT_WEAPON_COMPONENT's core 0x10EABB4
    // reads [object+0x448] (null tested) and the weapon's components at
    // +0x390 (count +0x3E0); _GIVE_WEAPON_COMPONENT_TO_WEAPON_OBJECT's core
    // 0x10E9AF0 passes [weapon+0x68] as the weapon hash to the inventory
    // lookup 0x1166A0C and to the give call 0x77F9E4. Playback validates the
    // hash with _GET_WEAPONTYPE_MODEL(hash) == the object's model.
    constexpr std::uint32_t kObjectWeaponOffset = 0x448;
    constexpr std::uint32_t kWeaponHashOffset = 0x68;

    // The entity owner (5 bits of [entity+0xB4]): DeleteAllMissionObjects
    // 0x7FE7D8 deletes owner 5 (and 0x7C000000 == 0x14000000), and
    // DeleteAllTempObjects 0x7FE894 owner 2 or 3. The map's own objects are
    // none of these (their value is logged at the snapshot, H until a bench).
    constexpr std::uint32_t kOwnerFieldOffset = 0xB4;
    constexpr std::uint32_t kOwnerMask = 0x7C000000u;
    constexpr std::uint32_t kOwnerShift = 26;
    constexpr std::uint32_t kOwnerMission = 5;
    constexpr std::uint32_t kOwnerTempA = 2;
    constexpr std::uint32_t kOwnerTempB = 3;
    // A prop set's objects (the player camp's tent, bedroll and cooking grate).
    // The prop set spawner 0x7FD248 creates each object through the object
    // factory 0x7FC250 (the one CREATE_OBJECT's core 0x10D30A0 calls) with 0x17
    // as the owner of the create parameters (0x6A8C84, the owner at +4;
    // CREATE_OBJECT passes 5) and sets it again with SetOwnedBy 0xE3F058 (the 5
    // bits at +0xB4) right after; no other site writes 23 as a constant. The
    // instance itself (0x7FCFCC, 0x150 bytes) is no object.
    constexpr std::uint32_t kOwnerPropSet = 23;
    constexpr std::uint32_t kOwnerUnknown = 0xFF;

    inline std::uint32_t OwnerOf(std::uint32_t field)
    {
        return (field & kOwnerMask) >> kOwnerShift;
    }

    // ---- a weapon object's weapon --------------------------------------------

    // The weapon read from the object, kept only when the weapon type's model
    // is the object's own (a stale pointer or another layout never passes).
    inline std::uint32_t ResolveObjectWeapon(std::uint32_t readHash, std::uint32_t modelOfReadHash, std::uint32_t objectModel)
    {
        return readHash && modelOfReadHash && modelOfReadHash == objectModel ? readHash : 0u;
    }

    // The weapon of each model in a clip, from the creates that carry one.
    inline std::map<std::uint32_t, std::uint32_t> WeaponModels(const std::vector<entity::CreatePacket>& created)
    {
        std::map<std::uint32_t, std::uint32_t> models;
        for (const entity::CreatePacket& packet : created)
            if (packet.weaponHash && (packet.kind == entity::EntityKind::Weapon || packet.kind == entity::EntityKind::Prop))
                models.emplace(packet.modelHash, packet.weaponHash);
        return models;
    }

    // A weapon or prop built as the bare model although its model is a
    // weapon's in the same clip: playback makes it without the weapon's
    // components (a knife without its grip). Hats, vehicle pieces and gore
    // pieces are rebuilt another way.
    inline bool WeaponPlain(const entity::CreatePacket& packet, const std::map<std::uint32_t, std::uint32_t>& weaponModels)
    {
        if (packet.weaponHash || (packet.kind != entity::EntityKind::Weapon && packet.kind != entity::EntityKind::Prop))
            return false;
        if (packet.kind == entity::EntityKind::Prop &&
            (packet.propOrigin == entity::PropOrigin::DroppedHat || packet.propOrigin == entity::PropOrigin::VehiclePiece ||
                packet.propOrigin == entity::PropOrigin::GorePiece))
            return false;
        return weaponModels.count(packet.modelHash) != 0;
    }

    inline const char* OriginText(entity::PropOrigin origin)
    {
        switch (origin)
        {
        case entity::PropOrigin::DroppedHat: return "hat";
        case entity::PropOrigin::DroppedWeapon: return "weapon";
        case entity::PropOrigin::GorePiece: return "gore_piece";
        case entity::PropOrigin::Projectile: return "projectile";
        case entity::PropOrigin::WorldObject: return "world_object";
        case entity::PropOrigin::WorldPiece: return "world_piece";
        case entity::PropOrigin::VehiclePiece: return "vehicle_piece";
        default: return "?";
        }
    }

    // ---- scenery ---------------------------------------------------------------

    constexpr std::uint32_t kNeverGone = 0xFFFFFFFFu;
    // Around the player at the snapshot and for the objects that appear
    // later (the world snapshot's radius).
    constexpr float kSceneryRadiusM = 60.0f;
    // The scenery the recording keeps besides the map dressing, and the map
    // dressing's own room on top (kMaxMapDressing), so a town's map data
    // never takes the room of the scene's objects.
    constexpr std::size_t kMaxSceneryOwn = 768;
    // Stand-ins built per tick at playback (models already streamed).
    constexpr std::uint32_t kSceneryCreatesPerTick = 32;
    // The original of a played object in the world: same model this close
    // to where the object stood (the model and the position, as the world
    // stand-ins already do).
    constexpr float kOriginalRadiusM = 0.6f;
    // A deferred hide looks again this often until the original appears.
    constexpr std::uint32_t kHideRetryMs = 500;

    // ---- map data by its creator -----------------------------------------------
    // A replay editor treats an object of the map data (an IPL instance) as
    // map-object state and gives every other object a create. RDR2 keeps that
    // origin in the owner field itself (offline dump, build 1491.50):
    //   - 25: a real object made from a map data instance. 0xE2F42C walks
    //     the queued (map data slot, instance index) pairs of the map data
    //     store, builds the object, sets the owner bits to 25 (0xE2F7F9 and
    //     0xE2F815) and calls 0xE2EA6C, which stores the slot at +0xD8, the
    //     instance index at +0xDA and 1 at +0xDC and puts the object in the
    //     store entry's per-instance table (+0x280). The object removal
    //     0x7FED8C releases that table slot only for owner 25 (cmp
    //     0x64000000 at 0x7FEE69). No other site writes 25 as a constant
    //     (no SetOwnedBy 0xE3F058 caller loads it, no other store of
    //     0x64000000 at +0xB4; the lend's give-back 0x83F400 restores the
    //     owner it kept).
    //   - 14: an entity set up from a map data entity definition (0xE3E95C,
    //     the definition's flags, archetype and tint, then the owner bits
    //     to 14 at 0xE3E9F8).
    //   - 17: the entities an interior instance adds (0xE76E24 passes 0x11
    //     to SetOwnedBy for each); 0xEB04D0 walks a map data entity list
    //     and acts on owner 14 or 17 only.
    //   - 24: a map object the engine lends for a while (0x83F426 keeps the
    //     old owner at +0x798 and sets 24; 0x83F400 gives it back).
    // So the map's owners are exactly 14, 17, 24 and 25 (they also held
    // 97.8% of the objects that were no scenery in 67 snapshots); a "most
    // frequent owner" learned at the snapshot is not part of the rule (it only
    // adds noise: a camp's map owner was 25, a town's 14 or 17). CREATE_OBJECT
    // gives 5, a prop set 23, the temporary objects 2 or 3; every other value
    // (0, 1, 4 the persistence system, 7, 11, 18...) is no map data either.
    constexpr std::uint32_t kOwnerMapInstance = 25;   // a real object from a map data instance
    constexpr std::uint32_t kOwnerMapDefinition = 14; // an entity from a map data definition
    constexpr std::uint32_t kOwnerInterior = 17;      // an interior instance's entity
    constexpr std::uint32_t kOwnerLent = 24;          // a map object lent for a while
    constexpr std::uint32_t kMapDataOwnerMask =
        (1u << kOwnerMapDefinition) | (1u << kOwnerInterior) | (1u << kOwnerLent) | (1u << kOwnerMapInstance);
    constexpr std::uint32_t kWorldOwnerMask = kMapDataOwnerMask; // the older name

    inline bool IsMapDataOwner(std::uint32_t owner)
    {
        return owner < 32 && (kMapDataOwnerMask & (1u << owner)) != 0;
    }

    // An object of the scene that is not of the map: a script's object
    // (mission entity, owner 5), a temporary one (owner 2 or 3), a prop
    // set's (owner 23: a camp's tent and grate) and any other creator's (an
    // owner read and not map data: every non-map object gets a create).
    inline bool IsSceneryOwner(bool missionEntity, std::uint32_t owner)
    {
        return missionEntity || (owner < 32 && !IsMapDataOwner(owner));
    }

    // Why a snapshot object became scenery or not (the scenery_by_rule
    // counts of POSE_V2_WORLD_SNAPSHOT and the SCNY v3 `rule` of an entry).
    enum class SceneryRule : std::uint8_t
    {
        Legacy = 0,      // an entry of a clip before SCNY v3 (or a path that does not say)
        Script = 1,      // a mission entity, CREATE_OBJECT's owner 5, a prop set's 23
        Temp = 2,        // a temporary object (owner 2 or 3)
        Pickup = 3,      // a pickup or loot object, whoever owns it
        Other = 4,       // any other creator that is not the map's
        MapDressing = 5, // map data inside a clip IPL's sphere (an entry, built only when missing at playback)
        MapData = 6,     // map data left to the map (no entry)
        Unknown = 7,     // the owner was not read (no entry)
    };
    constexpr std::size_t kSceneryRuleCount = 8;

    inline SceneryRule SceneryRuleOf(bool missionEntity, std::uint32_t owner, bool pickup)
    {
        if (pickup)
            return SceneryRule::Pickup;
        if (missionEntity || owner == kOwnerMission || owner == kOwnerPropSet)
            return SceneryRule::Script;
        if (owner == kOwnerTempA || owner == kOwnerTempB)
            return SceneryRule::Temp;
        if (owner >= 32)
            return SceneryRule::Unknown;
        return IsMapDataOwner(owner) ? SceneryRule::MapData : SceneryRule::Other;
    }

    inline const char* SceneryRuleText(SceneryRule rule)
    {
        switch (rule)
        {
        case SceneryRule::Legacy: return "legacy";
        case SceneryRule::Script: return "script";
        case SceneryRule::Temp: return "temp";
        case SceneryRule::Pickup: return "pickup";
        case SceneryRule::Other: return "other";
        case SceneryRule::MapDressing: return "map_dressing";
        case SceneryRule::MapData: return "map_data";
        case SceneryRule::Unknown: return "unknown";
        default: return "?";
        }
    }

    // ---- map dressing --------------------------------------------------------
    // A mission's camp (tent and boxes) can be map data of the mission's IPL
    // (every snapshot object there owner 25, the map data instance's, none
    // scenery); a playback in a mission only counts the world state's
    // difference (RestoreWorldStateInMission=0), and the closed theater's
    // clear removes a town's map data too (a store's shelf). So the map data
    // near the player (nearest first) is kept as a map dressing entry with the
    // clip IPL whose sphere holds it (0: the always-on map), and the playback
    // builds it only when the map does not have it: its IPL off, or its
    // original still missing after kMapDressingWaitMs.
    // Nearest first within the snapshot radius: each entry is watched with
    // the scenery (four times a second). The closed theater clears the town's
    // map data, so every map data object of the snapshot is kept: the
    // snapshot lists at most 1024 objects (recorder::kPoolCapacity), so the
    // cap is that list. The old cap of 192 left Saint Denis 274 to 458 short
    // and Blackwater 69 (built_missing at playback). Measured on the QA logs
    // (rec.scenery, PROFILE_ZONE): about 1.5 us per watched entry
    // per scan (320 us for about 200 entries), so 650 map data objects cost
    // about 1 ms every 250 ms; play.object_stands stays under 35 us a frame.
    constexpr std::size_t kMaxMapDressing = 1024;
    constexpr std::size_t kMaxScenery = kMaxSceneryOwn + kMaxMapDressing;
    constexpr std::uint64_t kMapDressingWaitMs = 4000; // the original's search before a stand-in is built

    struct DressingSphere
    {
        std::uint32_t hash = 0;
        float center[3] = {};
        float radius = 0.0f;
    };

    // The IPL that holds a point: the smallest sphere of the IPLs on that
    // contains it (a camp's own IPL inside a larger region's), 0 when none.
    inline std::uint32_t DressingIplAt(const float (&point)[3], const std::vector<DressingSphere>& spheres)
    {
        std::uint32_t best = 0;
        float bestRadius = 0.0f;
        for (const DressingSphere& sphere : spheres)
        {
            if (!(sphere.radius > 0.0f))
                continue;
            float d2 = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
                d2 += (point[axis] - sphere.center[axis]) * (point[axis] - sphere.center[axis]);
            if (d2 > sphere.radius * sphere.radius)
                continue;
            if (!best || sphere.radius < bestRadius || (sphere.radius == bestRadius && sphere.hash < best))
            {
                best = sphere.hash;
                bestRadius = sphere.radius;
            }
        }
        return best;
    }

    // A map dressing object that left during the recording: gone only when
    // its IPL went off with it (the mission took the camp down); while the
    // IPL stays on (or for the always-on map) it only streamed out (the
    // player rode away), and the map brings it back. The caller passes true
    // for an entry without an IPL.
    inline bool MapDressingGone(bool iplActiveNow)
    {
        return !iplActiveNow;
    }

    enum class MapDressing : std::uint8_t
    {
        Wait = 0,  // its IPL is on: the original is looked for
        Live = 1,  // the original stands where it stood: left to the map
        Build = 2, // built as a scenery stand-in (the original hidden when it shows)
    };

    // At playback: `taken` a prop took it over (a drawer the player opened,
    // an item picked up), `changed` its visibility flipped or it was taken
    // away while recorded (a shelf's loot), `iplActive` the IPL that held it
    // is on now, `originalFound` a live object of its model stands where it
    // stood, `waitedMs` since the stand-ins began. Taken or changed, it plays
    // as any scenery: its stand-in until the prop takes over or the take,
    // the map's original hidden all along (left to the map, the original
    // stayed drawn next to the prop: the closed drawer over the open one,
    // the item on the shelf and in the hand).
    inline MapDressing MapDressingVerdict(bool taken, bool changed, bool iplActive, bool originalFound, std::uint64_t waitedMs)
    {
        if (taken || changed || !iplActive)
            return MapDressing::Build;
        if (originalFound)
            return MapDressing::Live;
        return waitedMs >= kMapDressingWaitMs ? MapDressing::Build : MapDressing::Wait;
    }

    inline const char* MapDressingText(MapDressing verdict)
    {
        switch (verdict)
        {
        case MapDressing::Wait: return "wait";
        case MapDressing::Live: return "live";
        case MapDressing::Build: return "build";
        default: return "?";
        }
    }

    // ---- late scenery ----------------------------------------------------------

    // The map's owners of a recording (the map data owners above; the learned
    // owner does not count, see kMapDataOwnerMask).
    inline std::uint32_t MapOwnerMask(std::uint32_t /*learnedMapOwner*/ = kOwnerUnknown)
    {
        return kMapDataOwnerMask;
    }

    // An object first seen during the recording next to a recorded ped (the
    // spawned object watch) that stands still becomes a late scenery entry
    // when its owner was read and is not the map's: a script's, a prop set's,
    // a temporary object or any other creator's (an object is registered at
    // its creation, with no owner rule). Never a rope's entity (the rope
    // track's).
    inline bool SpawnSceneryOwner(bool missionEntity, std::uint32_t owner, bool ropeModel, std::uint32_t mapOwnerMask)
    {
        if (ropeModel)
            return false;
        if (IsSceneryOwner(missionEntity, owner))
            return true;
        if (owner >= 32)
            return false; // not read
        return (mapOwnerMask & (1u << owner)) == 0;
    }

    // The watch makes the entry once the object stood still this long since
    // its first sight (the scenery's own settle) and keeps watching it: a
    // move or an animation later records it as a prop, which takes over
    // from the stand-in (MarkPoseV2SceneryTaken). Entries the watch adds in
    // one recording at most (a flood guard; the scenery's own cap too).
    constexpr std::uint32_t kSpawnSceneryAfterMs = 1000;
    constexpr std::uint32_t kMaxSpawnScenery = 64;

    enum class SpawnScenery : std::uint8_t
    {
        Wait = 0,    // not still long enough yet
        Add = 1,     // becomes an entry now
        Already = 2, // an entry, pending or a child already (the scenery's own path)
        Map = 3,     // the map's or a rope's: left to the map, as before
        Full = 4,    // the watch's cap
    };

    inline SpawnScenery SpawnSceneryVerdict(std::uint32_t stillMs, bool alreadyScenery, bool ownerQualifies,
        std::uint32_t addedThisRecording)
    {
        if (stillMs < kSpawnSceneryAfterMs)
            return SpawnScenery::Wait;
        if (alreadyScenery)
            return SpawnScenery::Already;
        if (!ownerQualifies)
            return SpawnScenery::Map;
        return addedThisRecording >= kMaxSpawnScenery ? SpawnScenery::Full : SpawnScenery::Add;
    }

    inline const char* SpawnSceneryText(SpawnScenery verdict)
    {
        switch (verdict)
        {
        case SpawnScenery::Wait: return "wait";
        case SpawnScenery::Add: return "scenery";
        case SpawnScenery::Already: return "scenery_already";
        case SpawnScenery::Map: return "map";
        case SpawnScenery::Full: return "scenery_full";
        default: return "?";
        }
    }

    // The listing reaches 200 m for objects with a handle, so a mission's camp
    // set up 60 to 200 m away is listed long before it is within the scenery
    // radius. An object out of the radius is looked at again every
    // kSceneryFarRecheckMs while listed (one memory read of its position);
    // decided once within it. The far table starts over past kMaxSceneryFar
    // objects (a long buffer recording).
    constexpr std::uint32_t kSceneryFarRecheckMs = 1000;
    constexpr std::size_t kMaxSceneryFar = 8192;

    // An unseen listed object is looked at now: never seen far, or its
    // recheck is due.
    inline bool SceneryLookDue(bool seenFar, std::uint32_t nowMs, std::uint32_t nextMs)
    {
        return !seenFar || nowMs >= nextMs;
    }

    // An object seen far and then within the radius appears in the clip from
    // its first sight when it stood there since (the camp was there before
    // the player came: the stand-in must not pop in at 60 m); one that moved
    // in the meantime from when it came within the radius.
    inline std::uint32_t SceneryLateAppearMs(bool seenFar, std::uint32_t firstSeenMs, bool movedSinceFirst, std::uint32_t nowMs)
    {
        return seenFar && !movedSinceFirst && firstSeenMs <= nowMs ? firstSeenMs : nowMs;
    }

    // Shown at `timeMs`: within its life, and before the prop that took it
    // over is ready (the world stand-in rule: 500 ms of grace for the prop).
    inline bool SceneryShown(std::uint32_t appearMs, std::uint32_t goneMs, bool taken, std::uint32_t takeoverMs,
        bool propReady, std::uint32_t timeMs)
    {
        if (timeMs < appearMs || timeMs >= goneMs)
            return false;
        if (!taken)
            return true;
        return timeMs < takeoverMs || (!propReady && timeMs < takeoverMs + 500u);
    }

    // Whether the stand-in may give way to the prop puppet that takes it over
    // (SceneryShown's propReady). The puppet is created, given its skeleton
    // and armed in one tick, its first pose published (and written) in that
    // tick; hiding the stand-in then would leave a frame without either. With
    // overlapTicks > 0 the stand-in stays shown until the prop has been posed
    // for more than overlapTicks consecutive ticks (1: hidden in the tick
    // after the first published pose); a prop without a pose track counts
    // from Ready, and one with a pose track that is not armed yet gets
    // kHandoffArmGraceTicks more (SceneryShown's 500 ms still caps the wait).
    // overlapTicks 0: hidden as soon as the puppet is Ready. Pure (tested).
    constexpr std::uint32_t kHandoffArmGraceTicks = 3;
    inline bool HandoffPropReady(bool propReady, bool hasPoseTrack, std::uint32_t readyTicks, std::uint32_t posedTicks,
        std::uint32_t overlapTicks)
    {
        if (!propReady)
            return false;
        if (overlapTicks == 0)
            return true;
        if (posedTicks > overlapTicks)
            return true;
        return readyTicks > overlapTicks + (hasPoseTrack ? kHandoffArmGraceTicks : 0u);
    }

    // The clip window [base, end): appear and gone moved to the window, an
    // object gone before the base or appearing at or after the end dropped.
    inline bool CutLife(std::uint32_t& appearMs, std::uint32_t& goneMs, std::uint32_t base, std::uint32_t end)
    {
        if (goneMs != kNeverGone && goneMs <= base)
            return false;
        if (appearMs >= end)
            return false;
        appearMs = appearMs > base ? appearMs - base : 0u;
        if (goneMs != kNeverGone)
            goneMs = goneMs >= end ? kNeverGone : goneMs - base;
        return true;
    }

    // ---- the scenery's visibility --------------------------------------------
    // A mission keeps objects of its own invisible (visible=0 in every
    // recording); without the visibility their stand-ins would hang in the
    // sky. The entry keeps its visibility when recorded and the times it
    // flipped.

    // Flips kept per entry (the scan reads four times a second).
    constexpr std::size_t kMaxVisibilityFlips = 64;

    // Visible at `timeMs`: the state when recorded, flipped by every flip
    // at or before it (`flips` ascending).
    inline bool VisibleAt(bool visibleWhenRecorded, const std::vector<std::uint32_t>& flips, std::uint32_t timeMs)
    {
        bool visible = visibleWhenRecorded;
        for (const std::uint32_t at : flips)
        {
            if (at > timeMs)
                break;
            visible = !visible;
        }
        return visible;
    }

    // The scan saw `visibleNow` at `nowMs`: a flip when it differs from the
    // last state kept (none past the cap). True when one was added.
    inline bool NoteVisibility(bool visibleWhenRecorded, std::vector<std::uint32_t>& flips, bool visibleNow, std::uint32_t nowMs)
    {
        const bool last = (flips.size() % 2 == 0) ? visibleWhenRecorded : !visibleWhenRecorded;
        if (last == visibleNow || flips.size() >= kMaxVisibilityFlips || (!flips.empty() && nowMs < flips.back()))
            return false;
        flips.push_back(nowMs);
        return true;
    }

    // The ring buffer drops what is before keepFrom: the flips
    // before it fold into the state (times stay absolute).
    inline void TrimVisibility(std::uint8_t& visible, std::vector<std::uint32_t>& flips, std::uint32_t keepFrom)
    {
        std::size_t folded = 0;
        bool state = visible != 0;
        while (folded < flips.size() && flips[folded] < keepFrom)
        {
            state = !state;
            ++folded;
        }
        visible = state ? 1 : 0;
        flips.erase(flips.begin(), flips.begin() + static_cast<std::ptrdiff_t>(folded));
    }

    // The clip window [base, end), as CutLife: flips at or before the base
    // fold into the state at the window's start, those at or after the end
    // are dropped, the rest move to the window.
    inline void CutVisibility(std::uint8_t& visible, std::vector<std::uint32_t>& flips, std::uint32_t base, std::uint32_t end)
    {
        bool state = visible != 0;
        std::vector<std::uint32_t> kept;
        for (const std::uint32_t at : flips)
        {
            if (at <= base)
                state = !state;
            else if (at < end)
                kept.push_back(at - base);
        }
        visible = state ? 1 : 0;
        flips.swap(kept);
    }

    inline bool Near(const float a[3], const float b[3], float radius)
    {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    // ---- loot and pickups present before the take ------------------------------
    // A shelf has its loot when the player reaches for it in the recording;
    // recorded only at the take (the hand copy), the replay would show an
    // empty shelf, since the live world is looted already. The engine hides
    // the shelf original at the take and keeps it (the items still listed
    // free 30 s later, each IS_ENTITY_VISIBLE 0). Pickups and loot become
    // scenery entries (model, transform, visibility, when they left), so the
    // stand-in, its visibility flips and the original's hide are the
    // scenery's:
    //   - a pickup object (DOES_PICKUP_OBJECT_EXIST, IS_OBJECT_A_PORTABLE_PICKUP)
    //     is scenery whoever owns it;
    //   - any other free object a recorded ped comes within reach of (a map
    //     object, the shelf's loot) is watched, and becomes an entry when it
    //     is hidden or removed with a recorded ped still within reach.

    // A recorded ped this close to an object reaches it (008: the takes at
    // 0.49 to 0.72 m from Arthur).
    constexpr float kLootReachM = 2.5f;
    // The player this close for a watch and a hidden take; a removal counts
    // only closer: a small map object streams out a few tens of meters from
    // the player (the shelf's cigar), and that is not a take.
    constexpr float kLootPlayerRadiusM = 30.0f;
    constexpr float kLootGonePlayerM = 10.0f;
    // Objects watched at once (one IS_ENTITY_VISIBLE each per scan); a
    // watched object with no recorded ped this close leaves the watch (and
    // comes back when one reaches it again).
    constexpr std::size_t kMaxLootWatch = 128;
    constexpr float kLootUnwatchM = 10.0f;

    // Scenery: a script's or a temporary object, a pickup whoever owns it,
    // any other creator's that is not map data, never a rope's entity (the
    // rope track's).
    inline bool SceneryCandidate(bool missionEntity, std::uint32_t owner, bool pickup, bool ropeModel)
    {
        if (ropeModel)
            return false;
        return pickup || IsSceneryOwner(missionEntity, owner);
    }

    // Watched for a take: not scenery (that path follows it already), free,
    // drawn, a recorded ped within reach and the player near.
    inline bool LootWatchable(bool sceneryCandidate, bool attached, bool visible, float actorDistanceM, float playerDistanceM)
    {
        return !sceneryCandidate && !attached && visible && actorDistanceM >= 0.0f && actorDistanceM <= kLootReachM &&
            playerDistanceM >= 0.0f && playerDistanceM <= kLootPlayerRadiusM;
    }

    enum class LootTake : std::uint8_t
    {
        None = 0,
        Hidden = 1, // the engine hid the original (the shelf's loot)
        Gone = 2,   // removed (a pickup collected)
        Dropped = 3, // hidden or removed with nobody within reach: not a take, no longer watched
    };

    // A watched object at a scan: a take only with a recorded ped within
    // reach of where it stood and the player near (closer for a removal);
    // negative distances were not read.
    inline LootTake LootTakeOf(bool exists, bool visibleNow, float actorDistanceM, float playerDistanceM)
    {
        if (exists && visibleNow)
            return LootTake::None;
        const bool reach = actorDistanceM >= 0.0f && actorDistanceM <= kLootReachM;
        const float playerLimit = exists ? kLootPlayerRadiusM : kLootGonePlayerM;
        if (!reach || playerDistanceM < 0.0f || playerDistanceM > playerLimit)
            return LootTake::Dropped;
        return exists ? LootTake::Hidden : LootTake::Gone;
    }

    // A map dressing object removed while its IPL stays on streamed out (the
    // player rode away; the map brings it back), unless a recorded ped is within
    // reach of its spot and the player close: then it was taken (an item picked
    // up), as a watched loot's removal (LootTakeOf). Streaming drops an object
    // tens of meters from the player, never within reach.
    inline bool DressingRemovalOf(float actorDistanceM, float playerDistanceM)
    {
        return LootTakeOf(false, false, actorDistanceM, playerDistanceM) == LootTake::Gone;
    }

    // A mission can delete a table set and make the same models again at the
    // same spot (two handles of each model, the second appearing between the
    // two removals). Taken as a take, playback would draw the stand-in until
    // it and keep the live table, its "original", hidden for the rest of the
    // clip. A map object is keyed by its model and position, not by the
    // handle: a removal followed within kReinstanceMs by an object of the same
    // model within kOriginalRadiusM is the same object made again, its watch
    // goes on on the new handle and nothing is recorded.
    constexpr std::uint32_t kReinstanceMs = 1500;

    inline bool ReinstanceOf(std::uint32_t goneModel, const float gonePosition[3], std::uint32_t model,
        const float position[3])
    {
        return goneModel && model == goneModel && Near(gonePosition, position, kOriginalRadiusM);
    }

    enum class LootGoneStep : std::uint8_t
    {
        Wait = 0,       // still looking for the same object made again
        Reinstance = 1, // made again: the watch goes on, no take
        Gone = 2,       // a take at goneMs
    };

    inline LootGoneStep LootGoneStepOf(bool reinstanceFound, std::uint32_t goneMs, std::uint32_t nowMs)
    {
        if (reinstanceFound)
            return LootGoneStep::Reinstance;
        return nowMs >= goneMs && nowMs - goneMs >= kReinstanceMs ? LootGoneStep::Gone : LootGoneStep::Wait;
    }

    // A watched object that left the spot (picked up and carried, kicked)
    // is no longer watched: its removal later is not the spot's take (a
    // pelt stowed on the horse would leave a stand-in on the ground). The
    // hidden shelf original stays in place (the scenery's settle distance).
    constexpr float kLootMovedM = 0.15f;
    inline bool LootInPlace(const float watched[3], const float now[3])
    {
        return Near(watched, now, kLootMovedM);
    }

    // A drawn watched object stays watched while a recorded ped is near.
    inline bool LootKeepWatching(float actorDistanceM)
    {
        return actorDistanceM >= 0.0f && actorDistanceM <= kLootUnwatchM;
    }

    // The entry of an object taken at `nowMs`: drawn until the take (one
    // visibility flip there) or alive until it (gone there).
    inline void LootLife(LootTake take, std::uint32_t nowMs, std::uint32_t& goneMs, std::uint8_t& visible,
        std::vector<std::uint32_t>& flips)
    {
        visible = 1;
        flips.clear();
        goneMs = kNeverGone;
        if (take == LootTake::Gone)
            goneMs = nowMs;
        else if (take == LootTake::Hidden)
            flips.push_back(nowMs);
    }

    // When the entry appears: an object of the snapshot or of the map was
    // there from the start; any other one from when it was first seen.
    inline std::uint32_t LootAppearMs(bool inSnapshot, bool mapObject, std::uint32_t firstSeenMs)
    {
        return inSnapshot || mapObject ? 0u : firstSeenMs;
    }

    // A loot entry in a clip: neither a script's nor a temporary object
    // (only the watch above records those as scenery): a map object, read.
    // A map dressing entry (SCNY v3 rule) is no loot: its caller checks.
    inline bool IsLootEntry(bool missionEntity, std::uint32_t owner)
    {
        return owner != kOwnerUnknown && !IsSceneryOwner(missionEntity, owner);
    }

    // The originals of the played objects, nearest first. A shelf holds rows
    // of the same model a few tenths of a meter apart, all within
    // kOriginalRadiusM of the taken item's spot: the first of the model in the
    // object list would hide a neighbor for the whole playback (a gap next to
    // the stand-in) and leave nothing for the item's own original. Each pair
    // is a stand and a live object of its model within the radius; the
    // closest pairs win, one object per stand.
    struct OriginalPair
    {
        std::size_t stand = 0;
        std::size_t object = 0;
        float distance2 = 0.0f;
    };

    inline std::vector<std::pair<std::size_t, std::size_t>> NearestOriginals(std::vector<OriginalPair> pairs)
    {
        std::sort(pairs.begin(), pairs.end(), [](const OriginalPair& a, const OriginalPair& b) {
            if (a.distance2 != b.distance2)
                return a.distance2 < b.distance2;
            return a.stand != b.stand ? a.stand < b.stand : a.object < b.object;
        });
        std::vector<std::pair<std::size_t, std::size_t>> chosen;
        std::vector<std::size_t> stands;
        std::vector<std::size_t> objects;
        for (const OriginalPair& pair : pairs)
        {
            if (std::find(stands.begin(), stands.end(), pair.stand) != stands.end() ||
                std::find(objects.begin(), objects.end(), pair.object) != objects.end())
                continue;
            stands.push_back(pair.stand);
            objects.push_back(pair.object);
            chosen.emplace_back(pair.stand, pair.object);
        }
        return chosen;
    }

    // The start of a playback waits for the stand-ins of the scenery due at
    // the playhead, like the map's doors (door_rebind.h MapSettle): done when
    // none is pending, or at the cap.
    inline bool SceneryReady(std::uint32_t pending, std::uint64_t elapsedMs, std::uint64_t capMs)
    {
        return pending == 0 || elapsedMs >= capMs;
    }

    // ---- animated world objects --------------------------------------------------

    // A snapshot object is watched for its bones when its skeleton has at
    // least two bones (object_pose.h kMinBones) and at most this many (the
    // spawned objects' cap).
    constexpr std::uint32_t kMaxWatchedAnimated = 64;

    // ---- the skeleton that appears, the scenery's children -----------------------
    //   - An object builds its skeleton only when something first needs one
    //     (CreateSkeleton 0xE34A18, 36 call sites in the dump: animation,
    //     PLAY_ENTITY_ANIM, the attach paths), so a chest's lid has no
    //     skeleton when the snapshot reads the bones. The world scan reads
    //     again the skeleton of a watched object near a recorded actor. On a
    //     scenery object (a script's or temporary one, recorded as an object
    //     of its own all along) a skeleton that appears is an animation:
    //     recorded at once with its pose. On a map object (recorded only once
    //     it changes) and on an object the snapshot never read (its watch
    //     cap) it is a baseline: recorded when its bones then move.
    //   - An object attached to a scenery object (a chest's lining, the loot)
    //     is kept as the scenery's child, as state (model, the engine attach
    //     frame and the transform in the parent's frame, when it was attached
    //     and left, visibility); playback attaches each to the parent's
    //     stand-in, and to the parent's prop once the prop took over.

    enum class SkeletonWatch : std::uint8_t
    {
        Unread = 0,  // not read at the snapshot (the kMaxWatchedAnimated cap)
        Absent = 1,  // read, no skeleton of two bones or more
        Present = 2, // its locals kept (the animated world object path compares them)
    };

    // Read again only near a recorded actor (the spawned objects' radius,
    // object_pose.h spawn::kNearM: the chest stood 0.8 m from Arthur), every
    // kSkeletonRereadMs per object, at most kSkeletonReadsPerScan per scan.
    constexpr float kSkeletonNearM = 3.0f;
    constexpr std::uint32_t kSkeletonRereadMs = 500;
    constexpr std::uint32_t kSkeletonReadsPerScan = 8;
    // Baselines taken by the reread (objects the snapshot cap left unread).
    // 256: the map objects that join the watch late (LateJoinWatch) take
    // theirs here too (a cabinet whose skeleton stays).
    constexpr std::uint32_t kMaxSkeletonBaselines = 256;

    inline bool ShouldRereadSkeleton(SkeletonWatch state, float nearestActorM, std::uint32_t nowMs, std::uint32_t nextMs,
        std::uint32_t readsThisScan)
    {
        return state != SkeletonWatch::Present && nearestActorM >= 0.0f && nearestActorM <= kSkeletonNearM &&
            nowMs >= nextMs && readsThisScan < kSkeletonReadsPerScan;
    }

    enum class SkeletonReread : std::uint8_t
    {
        Nothing = 0,
        Baseline = 1, // locals kept from now on, recorded when they change
        Appeared = 2, // the skeleton came: recorded now, with its pose
    };

    // ---- the rest pose -----------------------------------------------------------
    // A map object builds its skeleton only when the loot animation starts
    // (CreateSkeleton); on a map object the reread above makes that skeleton
    // a baseline, read up to kSkeletonRereadMs later, and a drawer that opens
    // fast and stays open never changes after its baseline: never recorded,
    // the stand-in closed. The skeleton is built in the model's bind pose (the
    // pose finalizer takes the model's own rows where nothing animated,
    // tpose_check.h): a skeleton read away from that pose was animated
    // already, so it is an animation on a map object too (recorded at once
    // with its pose); one at its bind pose stays a baseline. Unknown: the bind
    // pose could not be read (a baseline).
    enum class RestState : std::uint8_t
    {
        Unknown = 0,
        AtRest = 1,
        Away = 2,
    };

    // A bone turned past this or moved past this from its bind local is out
    // of its rest pose (tpose_check.h kPosedRad; a drawer slides: its
    // translation, 2 cm, far below a drawer's travel and well above the
    // stream's error).
    constexpr float kRestTurnRad = 0.0873f;
    constexpr float kRestMoveM = 0.02f;
    constexpr std::size_t kRestFloatsPerBone = 12; // BoneQts: rotation xyzw, translation xyz pad, scale

    struct RestMatch
    {
        std::uint32_t bones = 0;    // compared bones (min of both sides)
        std::uint32_t compared = 0; // with a usable rotation on both sides
        std::uint32_t turned = 0;
        std::uint32_t moved = 0;
        float maxTurnRad = 0.0f;
        float maxMoveM = 0.0f;
    };

    // `locals` and `bind`: `bones` and `bindBones` records of
    // kRestFloatsPerBone floats each (the pose buffer's locals, and the
    // skeleton data's bone records read as rotation, translation, scale).
    inline RestMatch CompareWithRest(const float* locals, std::uint32_t bones, const float* bind, std::uint32_t bindBones)
    {
        RestMatch m;
        m.bones = bones < bindBones ? bones : bindBones;
        if (!locals || !bind)
            return m;
        for (std::uint32_t bone = 0; bone < m.bones; ++bone)
        {
            const float* a = locals + static_cast<std::size_t>(bone) * kRestFloatsPerBone;
            const float* b = bind + static_cast<std::size_t>(bone) * kRestFloatsPerBone;
            const float na = a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3];
            const float nb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3];
            if (!(na > 0.25f && na < 4.0f) || !(nb > 0.25f && nb < 4.0f))
                continue;
            ++m.compared;
            float dot = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / std::sqrt(na * nb);
            dot = std::fabs(dot);
            dot = dot > 1.0f ? 1.0f : dot;
            const float turn = 2.0f * std::acos(dot);
            if (turn > kRestTurnRad)
                ++m.turned;
            if (turn > m.maxTurnRad)
                m.maxTurnRad = turn;
            const float dx = a[4] - b[4], dy = a[5] - b[5], dz = a[6] - b[6];
            const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(move))
            {
                if (move > kRestMoveM)
                    ++m.moved;
                if (move > m.maxMoveM)
                    m.maxMoveM = move;
            }
        }
        return m;
    }

    // The verdict: the bind pose read for every bone of the skeleton (the
    // counts must match: another skeleton is not this one's rest) and at
    // least two bones compared (object_pose.h kMinBones).
    inline RestState RestOf(const RestMatch& m, std::uint32_t bones, std::uint32_t bindBones)
    {
        if (bones != bindBones || m.compared < 2)
            return RestState::Unknown;
        return m.turned + m.moved > 0 ? RestState::Away : RestState::AtRest;
    }

    // The snapshot objects with bones within reach of a recorded actor (the
    // reread's radius) are read every tick, not four times a second: a drawer
    // slides out in half a second, and a quarter second unseen left the
    // replay's stand-in closed while live was already 0.23 m open.
    inline bool NearWatch(float nearestActorM)
    {
        return nearestActorM >= 0.0f && nearestActorM <= kSkeletonNearM;
    }

    // Within reach and away from its rest pose at the snapshot (a drawer or a
    // lid left open): recorded from the first read, with its pose, so the
    // replay never draws it at rest (the stand-in is a plain object of the
    // model, drawn in its bind pose). Unknown (no proven bind layout) keeps
    // the old rule.
    inline bool OpenAtStart(RestState rest)
    {
        return rest == RestState::Away;
    }

    inline const char* RestText(RestState rest)
    {
        switch (rest)
        {
        case RestState::AtRest: return "rest";
        case RestState::Away: return "away";
        default: return "unknown";
        }
    }

    // `resolved` and `bones`: the reread (a pose skeleton of `bones` bones);
    // `scenery`: the object is a scenery entry (IsSceneryOwner); `rest`, the
    // skeleton against its bind pose: one that appears (Absent before) away
    // from its rest pose is an animation on a map object too. An object the
    // snapshot left unread had its skeleton before (nothing tells when it
    // came): a baseline.
    inline SkeletonReread AfterReread(SkeletonWatch state, bool resolved, std::uint32_t bones, std::uint32_t baselines,
        bool scenery, RestState rest = RestState::Unknown)
    {
        if (!resolved || bones < 2 || state == SkeletonWatch::Present)
            return SkeletonReread::Nothing;
        if (state == SkeletonWatch::Absent && (scenery || rest == RestState::Away))
            return SkeletonReread::Appeared;
        return baselines < kMaxSkeletonBaselines ? SkeletonReread::Baseline : SkeletonReread::Nothing;
    }

    // A kept skeleton read again with another bone count (rebuilt: the
    // locals no longer compare) is a new skeleton, as one that appears
    // (on a map object too when it is away from its rest pose).
    inline SkeletonReread AfterRebuild(bool scenery, RestState rest = RestState::Unknown)
    {
        return scenery || rest == RestState::Away ? SkeletonReread::Appeared : SkeletonReread::Baseline;
    }

    // The objects are read nearest first (to the player or a recorded actor):
    // in list order the first kMaxWatchedAnimated objects with a skeleton can
    // leave the drawer next to the player unread while a shop sign 55 m away
    // is watched. True when `a` is read before `b`.
    inline bool WatchBefore(float aDistanceM, std::uint32_t aIndex, float bDistanceM, std::uint32_t bIndex)
    {
        if (aDistanceM != bDistanceM)
            return aDistanceM < bDistanceM;
        return aIndex < bIndex;
    }

    // A map object first listed after the snapshot (the player rode into town
    // after the recording started) is not in the world watch. One that comes
    // within kSkeletonNearM of a recorded actor joins it (an unread entry the
    // reread takes on), once per object, at most kMaxLateJoins a recording.
    constexpr std::uint32_t kMaxLateJoins = 512;
    inline bool LateJoinWatch(bool inWatch, bool decided, float nearestActorM, bool mapObject, std::uint32_t joined)
    {
        return !inWatch && !decided && mapObject && nearestActorM >= 0.0f && nearestActorM <= kSkeletonNearM &&
            joined < kMaxLateJoins;
    }

    // The state after a reread: an unread object that resolved nothing is
    // absent from then on (a skeleton coming later is an animation).
    inline SkeletonWatch NextSkeletonWatch(SkeletonWatch state, SkeletonReread result, bool resolved)
    {
        if (result != SkeletonReread::Nothing)
            return SkeletonWatch::Present;
        return resolved ? state : SkeletonWatch::Absent;
    }

    // ---- a recorded prop's bones, watched until they move ------------------------
    // A prop taken by the spawned object watch because its root moved, before
    // its bones did (a chair tilting): object_pose.h spawn::PoseSpawned
    // decides the pose once, so it would be recorded rigid and its tilt lost.
    // Here a prop registered without a pose (and not waiting for one) that
    // playback could pose (object_pose.h PoseableProp) stays watched: its
    // locals kept as a baseline and compared every kPropBoneIntervalMs, as
    // the animated world objects; a skeleton that appears or is rebuilt on it
    // (on an object the replay records all along it is an animation) or bones
    // that move start its pose late (the retry path, the create takes the
    // bone count). The pose places (object_pose.h ShouldPose) still decide;
    // these caps bound the reads.
    constexpr std::size_t kMaxPropBoneWatches = 32;      // props watched at once
    constexpr std::uint32_t kPropBoneIntervalMs = 250;   // per prop: 4 reads a second
    constexpr std::uint32_t kPropBoneReadsPerTick = 8;   // reads per recorder tick

    // At the register: whether the prop is watched (`poseable`:
    // object_pose.h PoseableProp for its origin and weapon).
    inline bool WatchPropBones(bool poseable, bool posed, bool poseCandidate, std::size_t watching)
    {
        return poseable && !posed && !poseCandidate && watching < kMaxPropBoneWatches;
    }

    inline bool PropBonesDue(std::uint32_t nowMs, std::uint32_t nextMs, std::uint32_t readsThisTick)
    {
        return nowMs >= nextMs && readsThisTick < kPropBoneReadsPerTick;
    }

    // The watch full. Rigid props with no skeleton (arrows, thrown objects,
    // rope ends: Projectile origin, no weapon) stay watched for a skeleton that
    // may never come, so after a fight they could hold every place and the
    // chair that comes later was never watched. A newcomer with a skeleton
    // (present, or not copied yet) takes the place of the oldest watched prop
    // without one; a newcomer without one is left out.
    enum class PropBoneRoom : std::uint8_t
    {
        Watch = 0, // a free place
        Evict = 1, // the oldest absent watch leaves for it
        Full = 2,  // left out
    };

    inline PropBoneRoom PropBoneWatchRoom(std::size_t watching, bool newcomerAbsent, bool absentWatched)
    {
        if (watching < kMaxPropBoneWatches)
            return PropBoneRoom::Watch;
        return !newcomerAbsent && absentWatched ? PropBoneRoom::Evict : PropBoneRoom::Full;
    }

    // The state at the register: `read` the locals were copied (a skeleton
    // of two bones or more), `bones` the skeleton's bone count (0 without
    // one). A skeleton whose locals could not be copied is Unread: its
    // first good read is a baseline, not an animation.
    inline SkeletonWatch PropBonesAtRegister(bool read, std::uint32_t bones)
    {
        if (read)
            return SkeletonWatch::Present;
        return bones < 2 ? SkeletonWatch::Absent : SkeletonWatch::Unread;
    }

    enum class PropBoneStep : std::uint8_t
    {
        Keep = 0,     // still watched, baseline unchanged
        Baseline = 1, // locals kept from now on
        Appeared = 2, // a skeleton came on the recorded prop: pose now
        Rebuilt = 3,  // its skeleton was rebuilt with another bone count: pose now
        Moved = 4,    // its bones moved: pose now
    };

    inline bool PropBonesPose(PropBoneStep step)
    {
        return step == PropBoneStep::Appeared || step == PropBoneStep::Rebuilt || step == PropBoneStep::Moved;
    }

    // One read. `state` before it, `read` and `bones` as above, `keptBones`
    // the baseline's bone count, `localsChanged` the comparison with the
    // baseline (meaningful only for a read of `keptBones` bones). The task
    // 50 rules with `scenery` true: the prop is recorded all along.
    inline PropBoneStep AfterPropBoneRead(SkeletonWatch state, bool read, std::uint32_t bones, std::uint32_t keptBones,
        bool localsChanged)
    {
        if (!read)
            return PropBoneStep::Keep;
        if (state == SkeletonWatch::Present)
        {
            if (bones != keptBones)
                return AfterRebuild(true) == SkeletonReread::Appeared ? PropBoneStep::Rebuilt : PropBoneStep::Baseline;
            return localsChanged ? PropBoneStep::Moved : PropBoneStep::Keep;
        }
        switch (AfterReread(state, true, bones, 0, true))
        {
        case SkeletonReread::Appeared: return PropBoneStep::Appeared;
        case SkeletonReread::Baseline: return PropBoneStep::Baseline;
        default: return PropBoneStep::Keep;
        }
    }

    // The state after a read that did not start the pose: locals kept after
    // a good read; a skeleton gone (fewer than two bones) is absent, so one
    // that comes back is an animation; a failed copy keeps the state.
    inline SkeletonWatch NextPropBoneWatch(SkeletonWatch state, bool read, std::uint32_t bones)
    {
        if (read)
            return SkeletonWatch::Present;
        return bones < 2 ? SkeletonWatch::Absent : state;
    }

    inline const char* PropBoneStepText(PropBoneStep step)
    {
        switch (step)
        {
        case PropBoneStep::Baseline: return "baseline";
        case PropBoneStep::Appeared: return "appeared";
        case PropBoneStep::Rebuilt: return "rebuilt";
        case PropBoneStep::Moved: return "moved";
        default: return "keep";
        }
    }

    // Children kept per recording.
    constexpr std::size_t kMaxSceneryChildren = 256;

    // A child seen in the first scan that sees its parent as an entry was
    // there when the parent appeared (the snapshot's scenery: at 0); later
    // ones appear when seen.
    inline std::uint32_t ChildAppearMs(bool parentFirstScan, std::uint32_t parentAppearMs, std::uint32_t nowMs)
    {
        return parentFirstScan ? parentAppearMs : nowMs;
    }

    // A child leaves its entry when it is gone, no longer attached to that
    // parent (picked up: the attached scan records it on the ped), or taken
    // by another path.
    inline bool ChildLeft(bool exists, bool attachedToParent, bool recordedElsewhere)
    {
        return !exists || !attachedToParent || recordedElsewhere;
    }

    // The engine attach frame read from the child (weapon_observer.h
    // ReadAttachFrame) is kept only when it is plausible: a bone index or -1,
    // an offset within 50 m, a unit quaternion.
    inline bool FramePlausible(int bone, const float position[3], const float rotation[4])
    {
        if (bone < -1 || bone >= 1024)
            return false;
        for (int axis = 0; axis < 3; ++axis)
            if (!(std::fabs(position[axis]) <= 50.0f))
                return false;
        const float norm = rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] +
            rotation[3] * rotation[3];
        return norm > 0.9f && norm < 1.1f;
    }

    // The child's transform in its parent's frame: positions in metres, unit
    // quaternions x y z w mapping each entity's frame to the world (as
    // ReadEntityTransform builds them).
    inline void RelativeFrame(const float parentPos[3], const float parentRot[4], const float childPos[3],
        const float childRot[4], float outPos[3], float outRot[4])
    {
        // q* (the parent's inverse rotation).
        const float ix = -parentRot[0], iy = -parentRot[1], iz = -parentRot[2], iw = parentRot[3];
        // v' = q* v q, as t = 2 (u x v), v' = v + w t + u x t with u = q*.xyz.
        const float vx = childPos[0] - parentPos[0], vy = childPos[1] - parentPos[1], vz = childPos[2] - parentPos[2];
        const float tx = 2.0f * (iy * vz - iz * vy);
        const float ty = 2.0f * (iz * vx - ix * vz);
        const float tz = 2.0f * (ix * vy - iy * vx);
        outPos[0] = vx + iw * tx + (iy * tz - iz * ty);
        outPos[1] = vy + iw * ty + (iz * tx - ix * tz);
        outPos[2] = vz + iw * tz + (ix * ty - iy * tx);
        // q* c.
        const float cx = childRot[0], cy = childRot[1], cz = childRot[2], cw = childRot[3];
        outRot[0] = iw * cx + ix * cw + iy * cz - iz * cy;
        outRot[1] = iw * cy - ix * cz + iy * cw + iz * cx;
        outRot[2] = iw * cz + ix * cy - iy * cx + iz * cw;
        outRot[3] = iw * cw - ix * cx - iy * cy - iz * cz;
    }

    enum class ChildHost : std::uint8_t
    {
        None = 0,    // the parent is not drawn: the child neither
        StandIn = 1, // the parent's frozen stand-in
        Prop = 2,    // the prop that took the parent over (moving, broken, posed)
    };

    // The prop hosts once it took over and its puppet is ready, else the
    // stand-in while it is shown (object_check.h SceneryShown keeps it 500 ms
    // for a prop not built yet).
    inline ChildHost ChooseChildHost(bool propTookOver, bool propReady, bool standInShown)
    {
        if (propTookOver && propReady)
            return ChildHost::Prop;
        return standInShown ? ChildHost::StandIn : ChildHost::None;
    }

    // The engine frame (parent bone and offsets) only on a posed prop: the
    // stand-in is a fresh CREATE_OBJECT without a skeleton, so it takes the
    // transform in the parent's frame at the entity root (bone -1), which is
    // where the child stood on the closed parent.
    inline bool AttachByFrame(ChildHost host, bool hasFrame, bool hostPosed)
    {
        return host == ChildHost::Prop && hasFrame && hostPosed;
    }

    // ---- objects on a vehicle or a train car -------------------------------------
    // A hay bale on a cart, the cabinet doors and the drawer inside a train
    // car, the cargo: the attached scan takes recorded peds and weapons as
    // parents, the world scan skips any object with a parent, and a recorded
    // train car is recorded by replay_trains outside the recorder's entity
    // list. An object attached to a recorded vehicle or train car is recorded
    // the way the objects on a ped are (the weapon registry: the parent, the
    // engine attach frame, a parent-relative track, its bones when it has a
    // skeleton), bounded so the peds' own weapons keep their places.
    constexpr float kVehicleChildNearM = 30.0f;        // a recorded ped this close (the scene, not the whole train)
    constexpr std::size_t kMaxVehicleChildren = 48;     // recorded at once
    constexpr std::size_t kVehicleChildWeaponRoom = 32; // weapon registry places left to the peds

    enum class VehicleChild : std::uint8_t
    {
        Record = 0,
        Far = 1,  // no recorded ped within kVehicleChildNearM
        Full = 2, // the cap, or the registry's room for the peds
    };

    inline VehicleChild VehicleChildVerdict(float nearestPedM, std::size_t vehicleChildren, std::size_t weapons,
        std::size_t weaponCapacity)
    {
        if (nearestPedM < 0.0f || nearestPedM > kVehicleChildNearM)
            return VehicleChild::Far;
        if (vehicleChildren >= kMaxVehicleChildren || weapons + kVehicleChildWeaponRoom >= weaponCapacity)
            return VehicleChild::Full;
        return VehicleChild::Record;
    }

    // ---- the live objects that are not the map's ---------------------------------
    // A replay hides a live object when a recorded object stands within
    // kOriginalRadiusM of it (SearchPoseV2Originals), and the world clear
    // (HidePoseV2World) hides peds, vehicles, vehicle debris and ropes but no
    // object; in a mission the closed theater refuses to clear the world
    // (WORLD_MODEL verdict=refuse_mission), so the mission's live props would
    // stay drawn next to their recorded copies. Every live object in the clip
    // area that is a script's, a temporary one, a prop set's or a pickup, and
    // not the replay's own, is hidden while the replay plays and given back at
    // its end, like the mission peds. The map's objects stay (the stand-ins
    // hide their own originals); what the player, his horse or a puppet holds
    // stays.
    struct LiveObject
    {
        bool replayOwn = false;    // a puppet, a stand-in, an original a stand hides, a helper
        bool onKept = false;       // attached to the player, what he owns, or a puppet
        bool inArea = true;        // within the clip area (InClipArea)
        bool missionEntity = false;
        std::uint32_t owner = kOwnerUnknown;
        bool pickup = false;
        bool rope = false;         // a rope's entity: the rope track's
        bool vehicleModel = false; // a vehicle's debris: ClearPoseV2VehicleDebris's
        // Another mod's object (IsOtherModObject): an ASI's thread made it
        // (_GET_ENTITY_SCRIPT is ScriptHook's thread hash), or a script made
        // it after the playback began (the game's scripts are paused then).
        bool otherScript = false;    // _GET_ENTITY_SCRIPT == ScriptHook's thread
        bool bornInPlayback = false; // not listed at the playback's first pass
    };

    // An object another mod placed: the lantern or light of a lighting mod, a
    // trainer's prop. While the replay plays, the game's script threads are
    // paused (world_theatre.h) and the other ASIs' threads keep running
    // (UnpauseForScriptHook), so a script object (a mission entity, or the
    // CREATE_OBJECT owner 5) that appears after the playback began is
    // another ASI's; an object of an ASI's thread (ScriptHookRDR2 runs every
    // ASI in a thread with the script hash joaat("audiotest")) is one whenever
    // it was made. Such an object is the mod's: never hidden, frozen or
    // deleted by the replay (QA4: "can't place anything in the world in
    // replay, like lanterns or lights").
    inline bool IsOtherModObject(bool otherScript, bool bornInPlayback, bool missionEntity, std::uint32_t owner)
    {
        return otherScript || (bornInPlayback && (missionEntity || owner == kOwnerMission));
    }

    // The recording's leftovers (the props it adopted, the pickup objects
    // born during it) are deleted at the playback start. The recording never
    // makes a world object a mission entity, so one that is a mission entity
    // is a script's: a mod's lantern the spawned object watch recorded, a
    // mission's prop. It is left alone: deleting it took it from its script
    // (SET_ENTITY_AS_MISSION_ENTITY moves the ownership), whose handle then
    // named a freed object (a tester: placing a lighting mod's props was "the
    // biggest factor in crashing my game after exiting a replay").
    inline bool LeftoverDeletable(bool missionEntity)
    {
        return !missionEntity;
    }

    enum class LiveHide : std::uint8_t
    {
        Hide = 0,
        Replay = 1,
        OnKept = 2,
        Outside = 3,
        Map = 4,
        Rope = 5,
        Debris = 6,
        OtherMod = 7, // another mod's object (IsOtherModObject)
    };
    constexpr std::size_t kLiveHideCount = 8;

    inline LiveHide HideLiveObject(const LiveObject& object)
    {
        if (object.replayOwn)
            return LiveHide::Replay;
        if (IsOtherModObject(object.otherScript, object.bornInPlayback, object.missionEntity, object.owner))
            return LiveHide::OtherMod;
        if (object.vehicleModel)
            return LiveHide::Debris;
        if (object.rope)
            return LiveHide::Rope;
        if (object.onKept)
            return LiveHide::OnKept;
        if (!object.inArea)
            return LiveHide::Outside;
        return object.pickup || IsSceneryOwner(object.missionEntity, object.owner) ? LiveHide::Hide : LiveHide::Map;
    }

    inline const char* LiveHideText(LiveHide verdict)
    {
        switch (verdict)
        {
        case LiveHide::Hide: return "hide";
        case LiveHide::Replay: return "replay";
        case LiveHide::OnKept: return "kept";
        case LiveHide::Outside: return "outside";
        case LiveHide::Map: return "map";
        case LiveHide::Rope: return "rope";
        case LiveHide::Debris: return "debris";
        case LiveHide::OtherMod: return "other_mod";
        default: return "?";
        }
    }

    // The clip area (the recording's box) with the playback start clear's
    // 30 m margin; a clip without an area takes everything.
    constexpr float kClipAreaMarginM = 30.0f;
    inline bool InClipArea(bool hasArea, float x, float y, float minX, float minY, float maxX, float maxY)
    {
        if (!hasArea)
            return true;
        return x >= minX - kClipAreaMarginM && x <= maxX + kClipAreaMarginM && y >= minY - kClipAreaMarginM &&
            y <= maxY + kClipAreaMarginM;
    }

    // ---- a gore piece is a ped's -------------------------------------------------
    // The gore scan takes any loose object that appears within 1.5 m of a ped
    // right after its gore event, so a shot-through railing's fragment could
    // be taken as a gore piece and played back as a live railing piece (or
    // the whole railing tumbling along the fragment's track). A gore piece is
    // a ped's: an object of a map object's model (a taken world object, a
    // break of the map) is that object's piece, recorded by the world paths.
    inline bool GorePieceCandidate(bool worldPieceModel, bool worldBreakModel)
    {
        return !worldPieceModel && !worldBreakModel;
    }

    // Playback adopts the piece the engine dropped from the owner puppet:
    // never a mission's object, a stand-in, or the original a stand hides.
    inline bool AdoptForGore(bool missionEntity, bool standIn, bool standOriginal)
    {
        return !missionEntity && !standIn && !standOriginal;
    }

    // A clip recorded before: a gore piece of a model the clip has as a world
    // object (the railing) is not built (the whole model would tumble); the
    // ped gore models always are.
    inline bool GorePiecePlayable(bool knownGoreModel, bool worldModel)
    {
        return knownGoreModel || !worldModel;
    }

    // ---- verdicts (QA ring: POSE_V2_OBJECT_CHECK, kind object_wrong) -----------

    enum class Verdict : std::uint8_t
    {
        Duplicate = 0,       // the original still drawn next to the replay's copy
        SceneryMissing = 1,  // a scenery object due at the playhead with no stand-in after the start wait
        WeaponPlain = 2,     // a weapon object recorded as its bare model
        FragUnplayed = 3,    // a recorded break whose object never got it
        AnimatedUnposed = 4, // an animated world object recorded without its pose
        LookMismatch = 5,    // an object without the custom texture or tint the clip wants
    };

    inline const char* VerdictText(Verdict verdict)
    {
        switch (verdict)
        {
        case Verdict::Duplicate: return "duplicate";
        case Verdict::SceneryMissing: return "scenery_missing";
        case Verdict::WeaponPlain: return "weapon_plain";
        case Verdict::FragUnplayed: return "frag_unplayed";
        case Verdict::AnimatedUnposed: return "animated_unposed";
        case Verdict::LookMismatch: return "object_look_mismatch";
        default: return "?";
        }
    }
}
