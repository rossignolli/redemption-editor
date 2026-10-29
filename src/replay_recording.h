#pragma once

// The clip in memory: everything one recording produces and playback reads,
// per entity and per subsystem. Pure data; the recorder fills it, playback
// reads it, and the live registries of either side (engine handles,
// puppets) stay outside.

#include "appearance_track.h"
#include "metaped_state.h"
#include "clip_area.h"
#include "camera_track.h"
#include "time_scale.h"
#include "entity_events.h"
#include "entity_lifecycle.h"
#include "entity_transform.h"
#include "environment_track.h"
#include "sky_track.h"
#include "timecycle_track.h"
#include "moon_track.h"
#include "env_override.h"
#include "record_gate.h"
#include "replay_ropes.h"
#include "train_consist.h"
#include "vehicle_doors.h"
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "mix_probe_core.h"
#include "render_target_core.h"
#include "game_listener_core.h"
#include "wind_replay_core.h"
#include "mix_scene_core.h"
#include "category_state_core.h"
#include "entity_query_track.h"
#include "ped_audio_state.h"
#include "ped_speed_core.h"
#include "shader_vars_core.h"
#include "foot_event_core.h"
#include "foot_movement_core.h"
#include "weapon_fx_intent_core.h"
#include "weapon_observer.h"
#include "object_look_core.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <array>
#include <vector>

namespace replay
{
    // A map door near the scene: the door system's hash (stable, from the
    // ytyp) and its rotation and position when they change. Played back on the
    // door itself.
    struct DoorSample
    {
        std::uint32_t timeMs = 0;
        float rotation[3] = {};
        float position[3] = {};
    };
    struct DoorTrack
    {
        std::uint32_t hash = 0;
        std::vector<DoorSample> samples; // time ascending
    };

    // The script-controlled map state: IPLs and interior entity sets. The
    // candidate IPLs (ipl_candidates.h) are tested with IS_IPL_ACTIVE_HASH at
    // the recording start and every 2 s; a change is an event. An IPL without
    // an event before a time is off there (the start holds an "on" event at 0
    // for every active candidate). An entity set of an interior found near the
    // player or the camera holds its first recorded state before its first
    // event (the interior was found then).
    struct IplChange
    {
        std::uint32_t timeMs = 0;
        std::uint32_t hash = 0;
        bool active = false;
    };
    struct IplSphere
    {
        std::uint32_t hash = 0;
        float center[3] = {};
        float radius = 0.0f;
    };
    struct WorldInterior
    {
        std::uint32_t nameHash = 0; // GET_INTERIOR_LOCATION_AND_NAMEHASH
        float position[3] = {};
    };
    struct EntitySetChange
    {
        std::uint32_t timeMs = 0;
        std::uint16_t interior = 0; // index into WorldState::interiors
        bool active = false;
        char name[64] = {};
    };
    struct WorldState
    {
        bool recorded = false;
        std::uint8_t camp = 255;   // world::kCampUnknown: Global_40.f_4283 at the recording
        std::uint8_t chapter = 0;  // world::Chapter, 0 unknown
        std::uint8_t flags = 0;    // world::WorldFlags
        std::uint32_t candidateListHash = 0; // which candidate list was tested (world::CandidateListHash)
        std::uint32_t candidateCount = 0;
        std::vector<IplChange> ipls;             // time ascending
        std::vector<IplSphere> spheres;          // bounding spheres of the IPLs that were on, by hash ascending
        std::vector<WorldInterior> interiors;
        std::vector<EntitySetChange> entitySets; // time ascending
    };

    // The player's special abilities while recording: Dead Eye and Eagle Eye
    // spans, replayed as the game's own screen effect (and the slow motion)
    // when the viewer turns them on.
    enum class AbilityKind : std::uint8_t { DeadEye = 0, EagleEye = 1 };
    struct AbilitySpan
    {
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0; // equal to start while still open
        AbilityKind kind = AbilityKind::DeadEye;
        bool open = false;       // recording only
    };

    // A recorded vehicle's components: cargo prop set, lantern (light prop set)
    // and extras, applied to the vehicle puppet at creation. 0 = none. The look
    // is the vehicle's latest variation: it is read again while the vehicle is
    // recorded (VEHL version 2) and carries the paint (tint) and livery.
    // kLookUnset: a clip without it (the create packet's values apply).
    constexpr std::int32_t kLookUnset = INT32_MIN;
    struct VehicleLook
    {
        entity::ReplayId id;
        std::uint32_t propSet = 0;
        std::uint32_t lightPropSet = 0;
        std::uint32_t extrasExist = 0; // bit n: extra n exists
        std::uint32_t extrasOn = 0;    // bit n: extra n turned on
        std::int32_t tint = kLookUnset;   // _GET_VEHICLE_TINT
        std::int32_t livery = kLookUnset; // _GET_VEHICLE_LIVERY
        std::uint32_t lastCheckMs = 0;    // recording only
    };

    // The charred ("scorched") render bit of a recorded ped or horse, stored
    // when it changes; replayed with SET_ENTITY_RENDER_SCORCHED on the puppet in
    // both directions. Keyed by the entity's packed id.
    struct ScorchTrack
    {
        std::uint32_t id = 0;
        std::vector<std::pair<std::uint32_t, bool>> states; // time ascending
        std::uint32_t lastCheckMs = 0; // recording only
        bool checked = false;          // recording only
    };
    // The recorded state at timeMs: the last change at or before it, else
    // off (a puppet is created without the bit).
    inline bool ScorchedAt(const ScorchTrack& track, std::uint32_t timeMs)
    {
        bool on = false;
        for (const auto& [atMs, state] : track.states)
        {
            if (atMs > timeMs)
                break;
            on = state;
        }
        return on;
    }

    // The broken windows of a recorded vehicle (bit n: IS_VEHICLE_WINDOW_INTACT(n)
    // false, windows 0..3), stored when the mask changes; replayed with
    // REMOVE_VEHICLE_WINDOW on the puppet, and a scrub back before a break
    // rebuilds the puppet (no native puts the glass back). Keyed by the
    // vehicle's packed id.
    constexpr int kVehicleWindows = 4;
    struct WindowTrack
    {
        std::uint32_t id = 0;
        std::vector<std::pair<std::uint32_t, std::uint8_t>> states; // time ascending
        std::uint32_t lastCheckMs = 0; // recording only
        bool checked = false;          // recording only
    };
    // The recorded mask at timeMs: the last change at or before it, else the
    // first one (the recorder stores its first read at the vehicle's entry,
    // so a window a model lacks never reads as a break to undo).
    inline std::uint8_t BrokenWindowsAt(const WindowTrack& track, std::uint32_t timeMs)
    {
        std::uint8_t mask = track.states.empty() ? 0 : track.states.front().second;
        for (const auto& [atMs, state] : track.states)
        {
            if (atMs > timeMs)
                break;
            mask = state;
        }
        return mask;
    }

    // The wheels and doors a recorded vehicle lost. Bits 0..5: the wheel of
    // that index (the index _BREAK_OFF_VEHICLE_WHEEL takes: 0 LF, 1 RF, 2 LM,
    // 3 RM, 4 LR, 5 RR) is off; bits 6..15: door 0..9 (_IS_VEHICLE_DOOR_BROKEN)
    // is off. Stored when the mask changes, the first read always (the state at
    // the vehicle's entry). Replayed with the same natives on the puppet; a
    // scrub back before a loss rebuilds the puppet (no native puts a wheel or
    // door back). Keyed by the vehicle's packed id.
    constexpr int kVehiclePartWheels = 6;
    constexpr int kVehiclePartDoors = 10;
    constexpr int kVehiclePartDoorShift = kVehiclePartWheels;
    struct VehiclePartTrack
    {
        std::uint32_t id = 0;
        std::vector<std::pair<std::uint32_t, std::uint16_t>> states; // time ascending
        std::uint32_t lastCheckMs = 0; // recording only
        bool checked = false;          // recording only
    };
    // The recorded mask at timeMs: the last change at or before it, else the
    // first one (the state the vehicle entered the recording with).
    inline std::uint16_t BrokenPartsAt(const VehiclePartTrack& track, std::uint32_t timeMs)
    {
        std::uint16_t mask = track.states.empty() ? 0 : track.states.front().second;
        for (const auto& [atMs, state] : track.states)
        {
            if (atMs > timeMs)
                break;
            mask = state;
        }
        return mask;
    }

    // The burnt look of a recorded ped, horse or vehicle: its engine fire record
    // (burn_level.h, CharState): whether it has one, the record flags and the
    // burn volumes' kept floats (extents, age, char amount, growth;
    // kFireCharFloats per volume). Sampled 4 times a second, stored on change
    // (presence, flags, volume count, or a value moving by kFireCharStep).
    // Played on the puppet in both directions: no record at the playhead takes
    // the puppet's off, a record writes the volumes into the puppet's own.
    constexpr std::size_t kFireCharFloats = 24;
    constexpr float kFireCharStep = 0.05f;
    // Where a burn volume sits on the entity: the fragment component (+0xB0, -1
    // the whole entity), the sub index (+0xE4 u16), the volume flags (+0xE6),
    // the component mask (+0xB8) and the direction (+0x60). A volume is the
    // char of that part: the wagon's canvas chars only through a volume on its
    // component (index-wise writes char the wrong parts and never the canvas).
    struct FireCharAnchor
    {
        std::int32_t component = -1;
        std::uint16_t sub = 0xFFFF;
        std::uint8_t flags = 0;
        std::uint32_t mask = 0;
        float dir[3] = {0.0f, 0.0f, 1.0f};
    };
    // Same part of the entity.
    inline bool FireCharSamePart(const FireCharAnchor& a, const FireCharAnchor& b)
    {
        return a.component == b.component && a.sub == b.sub;
    }
    struct FireCharState
    {
        bool present = false;
        std::uint8_t flags = 0;
        std::vector<std::array<float, kFireCharFloats>> volumes;
        // One per volume (FREC version 2); empty in version 1 clips (volumes then
        // match by index).
        std::vector<FireCharAnchor> anchors;
    };
    // For each recorded volume, the index of the entity's volume on the same
    // part (component and sub), each used once, in order; -1 when the entity
    // has no volume there. Pure, tested.
    inline std::vector<int> MatchFireCharVolumes(const std::vector<FireCharAnchor>& recorded, const std::vector<FireCharAnchor>& entity)
    {
        std::vector<int> match(recorded.size(), -1);
        std::vector<bool> used(entity.size(), false);
        for (std::size_t r = 0; r < recorded.size(); ++r)
            for (std::size_t e = 0; e < entity.size(); ++e)
                if (!used[e] && FireCharSamePart(recorded[r], entity[e]))
                {
                    match[r] = static_cast<int>(e);
                    used[e] = true;
                    break;
                }
        return match;
    }
    struct FireCharTrack
    {
        std::uint32_t id = 0;
        std::vector<std::pair<std::uint32_t, FireCharState>> states; // time ascending
        std::uint32_t lastCheckMs = 0; // recording only
        bool checked = false;          // recording only
    };
    // The state at timeMs: the last change at or before it, else the first
    // one (the state the entity entered the recording with); null when the
    // track is empty.
    inline const FireCharState* FireCharAt(const FireCharTrack& track, std::uint32_t timeMs)
    {
        const FireCharState* state = track.states.empty() ? nullptr : &track.states.front().second;
        for (const auto& [atMs, value] : track.states)
        {
            if (atMs > timeMs)
                break;
            state = &value;
        }
        return state;
    }
    // Whether the fire rebuild keeps this entity's engine record (a puppet with
    // a track: the track decides its record at every playhead; puppets of clips
    // without a track are cleared).
    inline bool FireCharKept(const std::vector<FireCharTrack>& tracks, std::uint32_t id)
    {
        for (const FireCharTrack& track : tracks)
            if (track.id == id)
                return !track.states.empty();
        return false;
    }
    // Whether an entity's engine fire record is kept through the fire
    // rebuild's area clear at playhead t, from the recorded fires on that
    // entity (start, stop; stop UINT32_MAX when it never stopped). Kept as soon
    // as a fire on it started by t, burning or not: dropping a burning one
    // loses the look after a scrub, since the relit fire makes a record of its
    // own parts, not the recorded ones. FREC writes the record part by part and
    // neutralizes the parts the recording does not have, so a kept record does
    // not grow with each relight. Depends only on t (idempotent scrub).
    inline bool FireRecordKeptAt(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& spans, std::uint32_t t)
    {
        for (const auto& span : spans)
            if (span.first <= t)
                return true;
        return false;
    }

    // Same state within kFireCharStep.
    inline bool FireCharSame(const FireCharState& a, const FireCharState& b)
    {
        if (a.present != b.present || a.flags != b.flags || a.volumes.size() != b.volumes.size() ||
            a.anchors.size() != b.anchors.size())
            return false;
        for (std::size_t n = 0; n < a.anchors.size(); ++n)
            if (!FireCharSamePart(a.anchors[n], b.anchors[n]) || (a.anchors[n].flags & 0x0F) != (b.anchors[n].flags & 0x0F) ||
                a.anchors[n].mask != b.anchors[n].mask)
                return false;
        for (std::size_t n = 0; n < a.volumes.size(); ++n)
            for (std::size_t k = 0; k < kFireCharFloats; ++k)
            {
                const float d = a.volumes[n][k] - b.volumes[n][k];
                if (d > kFireCharStep || d < -kFireCharStep)
                    return false;
            }
        return true;
    }

    // The burn level (+0x1C of the shader effect's instance block, burn_level.h)
    // of the recorded weapon objects and props (the meat a camp's script cooks
    // with _SET_OBJECT_BURN_LEVEL, 0 raw to 1 cooked, and the campfire's logs)
    // and, keyed by the scenery index instead (Recording::sceneryBurnLevels),
    // of the scenery objects. Stored when it changes by kBurnLevelStep or more,
    // the first read always. Written on the puppet at the playhead in both
    // directions. Vehicles, peds and horses are not recorded (the level stays 1
    // on them; their burnt look is the fire record, FireCharTrack); old clips
    // may still carry them. Keyed by the entity's packed id.
    constexpr float kBurnLevelStep = 0.02f;
    struct BurnLevelTrack
    {
        std::uint32_t id = 0;
        std::vector<std::pair<std::uint32_t, float>> states; // time ascending
        std::uint32_t lastCheckMs = 0; // recording only
        bool checked = false;          // recording only
    };
    // A level read at timeMs goes into the track when it is the track's first
    // read, when it moved by kBurnLevelStep or more from the last one stored, or
    // when it reached an end (0 or 1, the values the scripts set at the start
    // and the end of a cook) the last one stored is not at. Never a value that
    // is not a number, never back in time. true when stored. Pure (tested).
    inline bool NoteBurnLevel(BurnLevelTrack& track, std::uint32_t timeMs, float level)
    {
        track.lastCheckMs = timeMs;
        track.checked = true;
        if (!std::isfinite(level))
            return false;
        if (!track.states.empty())
        {
            const float last = track.states.back().second;
            const float delta = std::fabs(level - last);
            const bool end = (level == 0.0f || level == 1.0f) && level != last;
            if ((delta < kBurnLevelStep && !end) || timeMs < track.states.back().first)
                return false;
        }
        track.states.push_back({timeMs, level});
        return true;
    }
    // The puppet kinds the recorded burn level is written on: vehicles, peds
    // and horses (old clips) and the weapon objects and props. Pure (tested).
    inline bool BurnLevelPlayable(entity::EntityKind kind)
    {
        return kind == entity::EntityKind::Vehicle || kind == entity::EntityKind::Ped ||
            kind == entity::EntityKind::Horse || kind == entity::EntityKind::Weapon || kind == entity::EntityKind::Prop;
    }
    // The recorded level at timeMs: the last change at or before it, else
    // the first one (the level the entity entered the recording with).
    inline float BurnLevelAt(const BurnLevelTrack& track, std::uint32_t timeMs)
    {
        float level = track.states.empty() ? 0.0f : track.states.front().second;
        for (const auto& [atMs, state] : track.states)
        {
            if (atMs > timeMs)
                break;
            level = state;
        }
        return level;
    }

    struct WeaponTrack
    {
        std::uint32_t recordId = 0;
        entity::RelativeTrack track;
    };

    // Strap state changes of a recorded weapon with straps (mask bits,
    // strap flag, blend weights), replayed on the attached puppet weapon.
    struct StrapTrack
    {
        std::uint32_t recordId = 0;
        std::vector<std::pair<std::uint32_t, weapon::StrapState>> states;
    };

    // Look of a recorded weapon object: dirt, soot, visual damage (rust and
    // wear) and degradation, 0..1 as the natives give them, stored when one
    // moves; replayed on the weapon puppet. WCND version 2 (weapon_look_core.h)
    // also carries the rust (permanent degradation), the sixth wear channel and
    // the components the engine lists on it (flags say what the state carries;
    // a version 1 state has none).
    struct WeaponCondition
    {
        float dirt = 0.0f;
        float soot = 0.0f;
        float damage = 0.0f;
        float degradation = 0.0f;
        float permanent = 0.0f;
        float wearE4 = 0.0f;
        std::uint8_t flags = 0;
        std::vector<std::uint32_t> components;
    };
    struct WeaponConditionTrack
    {
        std::uint32_t recordId = 0;
        std::vector<std::pair<std::uint32_t, WeaponCondition>> states;
        std::uint32_t lastCheckMs = 0; // recording only
    };

    // Attach frame changes of a recorded weapon (parent bone and offsets from
    // the engine attachment extension), replayed as the puppet weapon's attach.
    struct AttachFrameTrack
    {
        std::uint32_t recordId = 0;
        std::vector<std::pair<std::uint32_t, weapon::AttachFrame>> frames;
    };

    // A persistent collision sound of a recorded entity: created at startMs,
    // driven by the recorded intensity, stopped at endMs.
    struct CollisionSound
    {
        entity::ReplayId entity;
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;
        bool ended = false;
        std::uint64_t soundRef = 0;
        std::vector<std::uint8_t> params;
        std::vector<std::pair<std::uint32_t, float>> intensity;
    };

    // A sound the engine tied to a recorded object or vehicle through its
    // tracker (the lit molotov's loop on the flying bottle): a persistent sound
    // on the entity. Replayed on the puppet while the playhead is inside
    // [startMs, endMs) in normal play.
    struct EntitySound
    {
        entity::ReplayId entity;
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;
        bool ended = false;
        std::uint64_t soundRef = 0;  // ref, or hash when byHash
        bool byHash = false;
        // The audio entity: a static one of the image (audioRva) or one
        // inside the entity (audioOffset from its address).
        bool audioOnEntity = false;
        std::uint64_t audioRva = 0;
        std::uint32_t audioOffset = 0;
        std::uint32_t trackerOffset = 0; // params +0xC8 minus the entity address
        std::uint32_t caller = 0;        // evidence
        std::vector<std::uint8_t> params;
        // A one-shot (created and played, the engine ends it): the span
        // [startMs, endMs) is only the window it may start in (its puppet can come
        // a few frames late); once playing it is never cut at endMs. Absent before
        // ESND version 2.
        bool oneShot = false;
        // The audio entity is inside the owner ped's audio holder (audioOnEntity
        // set, audioOffset from the holder, below kEntitySoundPedAudioSpan): a
        // persistent sound the ped's own audio made without tracker (foley, weapon
        // handling, the shell after a shot). Played on the puppet's holder unless
        // the puppet makes the same sound itself. Absent before ESND version 3.
        bool pedAudio = false;
        // The audio entity is the owner train car's own (the pointer at car+0x11E0,
        // replay_trains.h AudioEntityOf) plus audioOffset (audioOnEntity set, below
        // kEntitySoundVehicleAudioSpan): whistle, bell, engine and wheels of a
        // recorded train car. Played on the puppet car's audio entity unless the
        // puppet makes the same sound itself. Absent before ESND version 4.
        bool vehicleAudio = false;
    };

    // What an entity sound may name as its audio entity, checked when a clip is
    // read and again before playback calls the engine with it. The recorder only
    // keeps an audio entity inside the image or within kEntitySoundSpan bytes of
    // the owner (and the tracker likewise); an audEntity is polymorphic, so it
    // sits 8-byte aligned. A file that names anything else (a hand-edited or
    // damaged clip with a coherent hash) is refused instead of turning its
    // numbers into a pointer. imageSize 0 skips the image bound (tests).
    constexpr std::uint32_t kEntitySoundSpan = 0x1000;
    constexpr std::uint32_t kEntitySoundPedAudioSpan = 0x4820; // audio_observer.h kPedAudioSpan
    constexpr std::uint32_t kEntitySoundVehicleAudioSpan = 0x1000; // from the train car's audio entity
    inline bool EntitySoundAudioValid(const EntitySound& sound, std::uint64_t imageSize)
    {
        if (sound.trackerOffset >= kEntitySoundSpan)
            return false;
        if (sound.byHash && sound.soundRef > 0xFFFFFFFFull)
            return false;
        if (sound.pedAudio && sound.vehicleAudio)
            return false;
        if (sound.vehicleAudio)
            return sound.audioOnEntity && sound.audioRva == 0 && sound.audioOffset < kEntitySoundVehicleAudioSpan &&
                (sound.audioOffset & 7u) == 0;
        if (sound.pedAudio)
            return sound.audioOnEntity && sound.audioRva == 0 && sound.audioOffset < kEntitySoundPedAudioSpan &&
                (sound.audioOffset & 7u) == 0 && sound.trackerOffset == 0;
        if (sound.audioOnEntity)
            return sound.audioRva == 0 && sound.audioOffset < kEntitySoundSpan && (sound.audioOffset & 7u) == 0;
        return sound.audioOffset == 0 && sound.audioRva != 0 && (sound.audioRva & 7u) == 0 &&
            (imageSize == 0 || sound.audioRva < imageSize);
    }

    // A world object that moved or broke during the recording: how it stood
    // when the recording started. Playback shows a stand-in there until the
    // object's prop takes over, and hides the original.
    struct WorldObjectOrigin
    {
        entity::ReplayId id; // the prop that takes over
        std::uint32_t model = 0;
        float position[3] = {};
        float rotation[3] = {}; // degrees, GET_ENTITY_ROTATION order 2
    };

    // An object of the scene that is not of the map (a camp's props, made by its
    // script; a temporary object; object_check.h), recorded as state: model,
    // transform, when it appeared and left. Playback builds it frozen and hides
    // an original still in the world; a prop takes over when it moves, breaks
    // or animates (`prop`, the world stand-in rule).
    struct SceneryObject
    {
        std::uint32_t model = 0;
        float position[3] = {};
        float rotation[3] = {}; // degrees, GET_ENTITY_ROTATION order 2
        std::uint32_t appearMs = 0;
        std::uint32_t goneMs = 0xFFFFFFFFu; // objcheck::kNeverGone
        entity::ReplayId prop; // the prop that took it over (invalid: none)
        std::uint8_t owner = 0xFF; // the engine owner (objcheck::OwnerOf), 0xFF unread
        std::uint8_t mission = 0; // a mission entity when recorded
        // Visible when recorded, and the times it flipped since (objcheck::VisibleAt;
        // SCNY v2, older clips: visible).
        std::uint8_t visible = 1;
        std::vector<std::uint32_t> visibleFlipsMs;
        // The metaped layer a hat object draws (a hat that fell before the
        // recording started, hat_look.h), empty for every other object and in clips
        // without section SCHT.
        std::vector<entity::MetaPedAsset> hatLook;
        // SCNY v3: why the entry was made (objcheck::SceneryRule, 0 legacy) and,
        // for a map dressing entry, the clip IPL whose sphere held it (0 none).
        std::uint8_t rule = 0;
        std::uint32_t ipl = 0;
    };

    // An object attached to a scenery object (the hay lining and the loot of a
    // mission chest; object_check.h), kept as state like its parent: model, the
    // scenery entry it hangs on, the engine attach frame (parent bone and
    // offsets, when read and plausible) and its transform in the parent's frame,
    // where it stood in the world (the original's search), when it was attached
    // and left, its visibility (section SCCH).
    struct SceneryChild
    {
        std::uint32_t model = 0;
        std::uint32_t parent = 0; // index into Recording::scenery
        std::uint8_t hasFrame = 0;
        std::int16_t frameBone = -1;
        float framePosition[3] = {};
        float frameRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x y z w
        float relativePosition[3] = {};                    // in the parent's frame
        float relativeRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float position[3] = {};
        float rotation[3] = {}; // degrees, GET_ENTITY_ROTATION order 2
        std::uint32_t appearMs = 0;
        std::uint32_t goneMs = 0xFFFFFFFFu; // objcheck::kNeverGone
        std::uint8_t visible = 1;
        std::vector<std::uint32_t> visibleFlipsMs;
    };

    // Whether a recorded ped or horse was visible: the state at its first sample
    // and the times it flipped since (objcheck::VisibleAt / NoteVisibility;
    // section PVIS).
    struct PedVisibility
    {
        entity::ReplayId id;
        std::uint8_t visible = 1;
        std::vector<std::uint32_t> flipsMs;
    };

    // A fragment group of a recorded world object that came off (PartsBrokeOff
    // 0x6EAE68 seen by the frag observer).
    struct FragBreak
    {
        std::uint32_t timeMs = 0;
        entity::ReplayId id; // the world object's prop
        std::uint16_t group = 0;
    };

    // The fragment groups a loose piece of a fragment object carries (the board
    // that came off the crate). Playback breaks the piece off a whole object of
    // the model and takes off it the groups it did not carry (frag_piece.h,
    // section FPCE).
    struct FragPiece
    {
        entity::ReplayId id;     // the piece's prop
        std::uint8_t source = 0; // fragpiece::Source
        std::vector<std::uint16_t> groups; // ascending; the lowest is its root
    };

    // The world state of the clip: the game clock at the start and at the stop
    // (seconds since midnight) and the weather at the start
    // (GET_CURR_WEATHER_STATE). Playback in product mode applies it; debug mode
    // leaves the world as it is.
    struct Environment
    {
        bool recorded = false;
        std::uint32_t clockStartS = 0;
        std::uint32_t clockEndS = 0;
        std::uint32_t weatherFrom = 0;
        std::uint32_t weatherTo = 0;
        float weatherMix = 0.0f;
        // The state over time (environment_track.h); empty in an older clip (the
        // start state above is played then).
        std::vector<EnvironmentSample> track;
        // The cloud accumulators over time and their periods (sky_track.h); empty
        // in an older clip (the clouds are then left to the session).
        std::vector<SkySample> sky;
        SkyPeriods skyPeriods;
        // The script timecycle modifier and the weather variations over time
        // (timecycle_track.h, section TCMD); empty in an older clip (the session's
        // are left alone).
        std::vector<TimecycleSample> timecycle;
        // The moon's two session angles over time, with the drawn sun and moon and
        // the date as evidence (moon_track.h, section MOON); empty in an older clip
        // (the session's moon is kept, and given back when the clip ends).
        std::vector<MoonSample> moon;
        // The editor's settings over the recorded state (env_override.h); all
        // Recorded by default. Saved in section ENVO; a cut copies them with the
        // rest.
        EnvironmentOverride edit;
        // Not saved: the load found the flag off on a clip with a track and set it
        // (clip_file.cpp, EnvironmentFromTrack below), for the log.
        bool repairedOnLoad = false;
    };

    // Where and who: the map zones at the player when the recording started
    // (town, district, state: hash and the game's text), the player's character
    // and the weather by name. For the gallery.
    struct Place
    {
        bool recorded = false;
        std::uint32_t zoneTown = 0;
        std::uint32_t zoneDistrict = 0;
        std::uint32_t zoneState = 0;
        std::uint32_t playerModel = 0;
        char town[48] = {};
        char district[48] = {};
        char state[48] = {};
        char character[16] = {};
        char weather[24] = {};
    };

    // The clock at a playhead: from the start to the stop time, in step with
    // the timeline (across midnight too). Seconds since midnight.
    inline std::uint32_t ClockAt(const Environment& environment, std::uint64_t playheadMs, std::uint64_t durationMs)
    {
        const std::uint32_t day = 24u * 60u * 60u;
        const std::uint32_t span = (environment.clockEndS + day - environment.clockStartS) % day;
        const std::uint64_t at = durationMs ? (playheadMs < durationMs ? playheadMs : durationMs) : 0;
        const std::uint32_t advanced = durationMs ? static_cast<std::uint32_t>(span * at / durationMs) : 0;
        return (environment.clockStartS + advanced) % day;
    }

    // A clip whose environment track is there but whose recorded flag is not: a
    // window of the buffer cut while the recording was still running (the flag and
    // the stop clock are set when a recording stops). The recorder ran for the
    // whole recording, so the clip did record its environment: the flag is set and
    // the start and stop clock come from the track at fromMs and toMs (the window's
    // base and end in the recording's track, or 0 and the duration in a clip's), as
    // a cut of a stopped recording gives them. False (nothing changed) with no track
    // or a clock out of the day: a clip from before the environment stays without
    // it.
    inline bool EnvironmentFromTrack(Environment& environment, const std::vector<EnvironmentSample>& track,
        std::uint64_t fromMs, std::uint64_t toMs)
    {
        const auto ms = [](std::uint64_t t) { return static_cast<std::uint32_t>(t < 0xFFFFFFFFull ? t : 0xFFFFFFFFull); };
        EnvironmentSample start;
        EnvironmentSample end;
        if (!envtrack::At(track, ms(fromMs), start) || !envtrack::At(track, ms(toMs), end) ||
            start.clockS >= envtrack::kDaySeconds || end.clockS >= envtrack::kDaySeconds)
            return false;
        environment.recorded = true;
        environment.clockStartS = start.clockS;
        environment.clockEndS = end.clockS;
        return true;
    }

    struct Recording
    {
        entity::IdAllocator ids;
        entity::LifecycleTrack lifecycle;
        entity::PoseSegmentTrack segments;
        // Every create of the recording (peds, horses, vehicles, weapons,
        // props), for lookups and logs.
        std::vector<entity::CreatePacket> created;
        std::vector<WeaponTrack> weaponTracks;
        std::vector<StrapTrack> strapTracks;
        std::vector<WeaponConditionTrack> weaponConditions;
        std::vector<AttachFrameTrack> attachFrames;
        ropes::Ropes ropes;
        entity::EventTrack shots;
        entity::SpeechTrack speechLines;
        entity::FootstepTrack footsteps;
        entity::ExplosionAudioTrack explosions;
        entity::AnimAudioTrack animAudio; // animation audio events (AANI)
        entity::SurfaceTrack surfaces;
        entity::FxTrack fx;
        std::vector<entity::RegisteredFxTrack> registeredFx;
        entity::DecalTrack decals;
        entity::DecalRemoveTrack decalRemoves;
        entity::ProjDecalTrack projDecals;
        entity::GoreTrack gore;
        entity::GoreHitTrack goreHits;
        timescale::Track timeScale;
        entity::BloodTrack blood;
        entity::FireStartTrack fireStarts;
        entity::FireStopTrack fireStops;
        entity::FireItemTrack fireItems;
        // Object handles (objects and pickup objects) alive when the recording
        // started: anything else found at playback start (a knocked hat, a
        // dropped weapon, a pickup) is a leftover of the recording.
        std::vector<int> objectsBefore;
        std::vector<int> pickupsBefore; // pickup handles
        // Engine objects the recording adopted as props (dropped weapons,
        // knocked hats, severed pieces): deleted before playback by handle,
        // since the pool listing can come back empty (see leftovers).
        std::vector<int> adoptedObjects;
        // The area the recording happened in (recorded entities, effects,
        // decals): playback clears fires and particles there before it plays.
        bool hasArea = false;
        float areaMin[3] = {};
        float areaMax[3] = {};
        // Positions refused by cliparea::PositionValid (a local offset taken for a
        // world position, near the map origin); not saved.
        std::uint32_t areaRejected = 0;
        void ExtendArea(float x, float y, float z)
        {
            if (!cliparea::PositionValid(x, y, z))
            {
                ++areaRejected;
                return;
            }
            if (!hasArea)
            {
                areaMin[0] = areaMax[0] = x;
                areaMin[1] = areaMax[1] = y;
                areaMin[2] = areaMax[2] = z;
                hasArea = true;
                return;
            }
            const float p[3] = {x, y, z};
            for (int n = 0; n < 3; ++n)
            {
                areaMin[n] = p[n] < areaMin[n] ? p[n] : areaMin[n];
                areaMax[n] = p[n] > areaMax[n] ? p[n] : areaMax[n];
            }
        }
        entity::MeleeTrack meleeHits;
        entity::BodyImpactTrack bodyImpacts;
        entity::SoundTrack sounds;
        std::vector<CollisionSound> collisionSounds;
        std::vector<EntitySound> entitySounds;
        std::vector<WorldObjectOrigin> worldObjects;
        std::vector<FragBreak> fragBreaks;
        std::vector<FragPiece> fragPieces;
        std::vector<SceneryObject> scenery;
        std::vector<SceneryChild> sceneryChildren;
        std::vector<PedVisibility> pedVisibility;
        std::vector<camera::GameSample> gameCamera; // the rendered camera per captured frame
        Environment environment;
        Place place;
        std::vector<DoorTrack> doors;
        WorldState world;
        std::vector<AbilitySpan> abilities;
        std::vector<VehicleLook> vehicleLooks;
        // The recording gate (record_gate.h): the instants the clock went on after
        // a cutscene, fade or loading screen (playback does not interpolate across
        // them), the restricted-camera spans and the charred state per ped and
        // horse.
        std::vector<std::uint32_t> cuts;
        std::vector<gate::Span> spans;
        std::vector<ScorchTrack> scorched;
        std::vector<WindowTrack> vehicleWindows;
        std::vector<VehiclePartTrack> vehicleParts;
        // The door ratios of every recorded vehicle and train car (vehicle_doors.h,
        // section VDOR).
        std::vector<vehdoors::DoorTrack> vehicleDoors;
        // Burnt state per vehicle (on fire or charred at a sample, latched;
        // SamplePoseV2VehicleBurnt), ScorchTrack layout.
        std::vector<ScorchTrack> vehicleBurnt;
        // Burn level per weapon object and prop (BurnLevelTrack); old clips may
        // also hold vehicles, peds and horses.
        std::vector<BurnLevelTrack> burnLevels;
        // Burn level per scenery object, the track's id being the entry's index in
        // `scenery` (section SBLV).
        std::vector<BurnLevelTrack> sceneryBurnLevels;
        // The custom texture (document text) and tint per weapon object and prop
        // (key: packed id) and per scenery object (key: its index in `scenery`),
        // section OLOK (object_look_core.h).
        std::vector<ObjectLookTrack> objectLooks;
        std::vector<ObjectLookTrack> sceneryObjectLooks;
        // Fire record (burnt look) per ped, horse and vehicle.
        std::vector<FireCharTrack> fireChars;
        // Metaped layer changes per ped and horse (appearance_track.h).
        std::vector<AppearanceTrack> appearances;
        // The whole metaped look per ped and horse (expressions, scale,
        // cleanliness; metaped_state.h), section MPED.
        std::vector<metaped::Track> metapeds;
        // An old clip's WGPU section (GPU water) is skipped unread (clip_file.cpp).
        // The recorded train cars (consist links, cargo, lanterns) and the head
        // cars' speed (train_consist.h, section TRNC). The cars' world tracks are in
        // weaponTracks, keyed by their id.
        trains::TrainData trains;
        // The recording's sound census (sound_check_core.h; section SCEN, compared
        // with each playback's) and the sounds of recorded vehicles, train cars and
        // objects no other track covers (section GSND).
        census::Summary soundCensus;
        std::vector<census::GeneralSound> generalSounds;
        // The generic sound effect packets (create, adjust, end; section SGEN),
        // recorded with SoundGeneric=1.
        soundgeneric::Track soundGeneric;
        // What each owned sound's distances, LOD and player flags were while
        // recording (section MIXP, written only when MixProbe=1 recorded something).
        mixprobe::Track mixProbe;
        // Named render targets (register, link to a model, release) and the UI apps
        // that draw into them (open with entry, close), as state events
        // (render_target_core.h, section RTGT).
        rendertarget::Track renderTargets;
        // The live engine volume listener position (game_listener_core.h, section
        // LSTN, ListenerRecord=1), put back with ReplayGameListener=1 on the
        // recorded game camera.
        gamelistener::Track gameListener;
        // The audio wind strength, the wind rows' weather and the simple path flag
        // of the weather audio, 4 times a second (wind_replay_core.h, section WNDA;
        // the playback logs the recorded value against the engine's).
        windreplay::Track windAudio;
        // The mixer scenes started and stopped while recording (hash, who started
        // it, a conversation scene's participants) and each recorded ped's audio mix
        // group (mix_scene_core.h, section MIXS, written only when MixSceneRecord=1
        // saw something).
        mixscene::Track mixScenes;
        // Every static sound category's controller values (volume, the two
        // multipliers, pitch, filters) on first sight and on change
        // (category_state_core.h, section CATS), put back into the categories at
        // playback (CategoryStateReplay).
        catstate::Track categoryState;
        // The engine's answers to the owner queries of the dynamic entity sounds of
        // the recorded owners (entity_query_track.h, section EQRY).
        entityquery::Track entityQueries;
        // The foley variant indices the engine drew once for each recorded owner
        // (the player's 0 and 0 included; ped_audio_state.h, section FVAR).
        pedaudiostate::VariantTrack foleyVariants;
        // Each recorded ped's and horse's PED_VARIABLES speed, 4 times a second
        // (ped_speed_core.h, section PSPD), written into its puppet's block at
        // playback.
        pedspeed::Track pedSpeed;
        // Each recorded horse's animated shader variables (the muscles under its
        // skin), about 30 times a second (shader_vars_core.h, section SHVR),
        // written into its puppet's shader vars component at playback.
        shadervars::Track shaderVars;
        // Each recorded ped's and horse's secondary steps (the entries of the foot
        // audio's second queue: type, foot, the slot's container, the foot's rise;
        // foot_event_core.h), queued again on the puppet at playback (section FSEV).
        footevent::Track footEvents;
        // Each recorded ped's and horse's movement volume result per step
        // (foot_movement_core.h, section FMOV), given back to the puppet's step
        // (FootMovementReplay).
        footmovement::Track footMovement;
        // The weapon system's requests for each shot's muzzle flash and shell
        // (weapon_fx_intent_core.h, section WFXI), before the engine's range and
        // camera tests.
        weaponfx::Track weaponFxIntents;
        // Loaded from a file: the session lists (objects before, adopted objects)
        // do not exist, so the leftover cleanup is skipped.
        bool fromDisk = false;

        // Empties the clip for a new recording.
        void Begin()
        {
            fromDisk = false;
            ids.Reset();
            lifecycle.Begin();
            segments.Begin();
            created.clear();
            doors.clear();
            world = WorldState{};
            abilities.clear();
            vehicleLooks.clear();
            cuts.clear();
            spans.clear();
            scorched.clear();
            vehicleWindows.clear();
            vehicleParts.clear();
            vehicleDoors.clear();
            vehicleBurnt.clear();
            burnLevels.clear();
            sceneryBurnLevels.clear();
            objectLooks.clear();
            sceneryObjectLooks.clear();
            fireChars.clear();
            appearances.clear();
            metapeds.clear();
            trains.Clear();
            soundCensus.Clear();
            generalSounds.clear();
            soundGeneric.Clear();
            mixProbe.Clear();
            renderTargets.Clear();
            gameListener.Clear();
            windAudio.Clear();
            mixScenes.Clear();
            categoryState.Clear();
            entityQueries.Clear();
            foleyVariants.Clear();
            pedSpeed.Clear();
            shaderVars.Clear();
            footEvents.Clear();
            footMovement.Clear();
            weaponFxIntents.Clear();
            weaponTracks.clear();
            strapTracks.clear();
            weaponConditions.clear();
            attachFrames.clear();
            ropes.BeginRecording();
            shots.Begin();
            speechLines.Begin();
            footsteps.Begin();
            explosions.Begin();
            animAudio.Begin();
            surfaces.Begin();
            fx.Begin();
            registeredFx.clear();
            decals.Begin();
            decalRemoves.Begin();
            projDecals.Begin();
            gore.Begin();
            goreHits.Begin();
            timeScale.Begin();
            blood.Begin();
            fireStarts.Begin();
            fireStops.Begin();
            fireItems.Begin();
            hasArea = false;
            areaRejected = 0;
            objectsBefore.clear();
            pickupsBefore.clear();
            adoptedObjects.clear();
            meleeHits.Begin();
            bodyImpacts.Begin();
            sounds.Begin();
            collisionSounds.clear();
            entitySounds.clear();
            worldObjects.clear();
            fragBreaks.clear();
            fragPieces.clear();
            scenery.clear();
            sceneryChildren.clear();
            pedVisibility.clear();
            gameCamera.clear();
            environment = Environment{};
            place = Place{};
        }

        // The create packet of a record: the entity's own, or the owner's
        // for a pose segment record.
        const entity::CreatePacket* FindCreate(std::uint32_t recordId) const
        {
            const entity::ReplayId owner = segments.OwnerOf(recordId);
            if (owner.Valid())
                recordId = owner.Packed();
            for (const entity::CreatePacket& packet : created)
            {
                if (packet.id.Packed() == recordId)
                    return &packet;
            }
            return nullptr;
        }

        entity::RelativeTrack* WeaponTrackOf(std::uint32_t recordId)
        {
            for (WeaponTrack& entry : weaponTracks)
            {
                if (entry.recordId == recordId)
                    return &entry.track;
            }
            return nullptr;
        }
    };
}
