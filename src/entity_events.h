#pragma once

// PoseTrack v2: timed events.
//
// A shot is recorded at the moment the game fires it, with the shooter and
// weapon ReplayIds, and replayed as an event: it plays only while the
// playhead moves forward in normal play, never on a seek, a jump or while
// paused, and never re-simulates anything that changes the recording.
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include "entity_lifecycle.h"
#include "pose_math.h"
#include "speech_request.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace replay::entity
{
    struct ShotEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId shooter;
        ReplayId weapon; // weapon object entity, invalid when not recorded
        std::uint32_t weaponHash = 0;
        pose::Vec3 muzzle;    // world, at the shot
        pose::Vec3 direction; // unit, world
        pose::Vec3 impact;    // world, valid when hasImpact
        bool hasImpact = false;
        pose::Vec3 impactNormal; // surface normal, valid when hasNormal
        bool hasNormal = false;
        // Engine particle effects the weapon triggered for this shot (0 when
        // none was seen): muzzle flash on the weapon, impact at the hit.
        std::uint32_t muzzleFx = 0;
        std::uint32_t impactFx = 0;
        // Attachment flags of the gameplay muzzle instance at its start
        // (the bits the weapon code sets), valid when hasMuzzleFlags.
        std::uint32_t muzzleFlags = 0;
        bool hasMuzzleFlags = false;

        // Engine weapon fire event (audio) of this shot, raw, with the
        // entity references it holds located by role so playback can point
        // them at the puppets.
        enum class FireRef : std::uint8_t
        {
            Shooter = 1,
            ShooterAudio = 2,
            Weapon = 3,
            Unknown = 4,
        };
        struct FireRefField
        {
            std::uint16_t offset = 0;
            FireRef role = FireRef::Unknown;
            // Recorded entity the reference pointed at (any kind), when it
            // is one of the recording's entities.
            ReplayId target;
            // Otherwise: the recorded entity holding this reference in its
            // own memory at viaOffset (e.g. a weapon object's weapon).
            ReplayId via;
            std::uint16_t viaOffset = 0;
        };
        std::array<std::uint8_t, 0xA0> fireInfo{};
        bool hasFireInfo = false;
        std::vector<FireRefField> fireRefs;
        // Whether the shooter was the local player when recorded: the weapon audio
        // plays the player variant for the player only. 0 = unknown (older clip),
        // 1 = no, 2 = yes.
        std::uint8_t shooterPlayer = 0;
    };

    // A speech line as the engine chose it (context + voice + variation),
    // recorded when the speaker starts it.
    struct SpeechEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId speaker;
        std::uint32_t voiceHash = 0;
        std::uint32_t contextPartialHash = 0; // joaat before finalization
        std::int32_t variation = 0;
        // The context the Say asked for; the engine may resolve it to the line's
        // own context above.
        std::uint32_t requestedContextPartialHash = 0;
        // The voice object the line used (line +0x1E8; pointer valid in the
        // recording session only), its type byte and partial name hash, and the
        // line data type (1, 2, 3).
        std::uint64_t voiceObject = 0;
        std::uint8_t voiceObjectType = 0;
        std::uint32_t voiceObjectPartial = 0;
        std::uint32_t dataType = 0;
        // Engine path (return address of the core call).
        std::uint32_t path = 0;
        // timeMs is the audible start (state 5) when timedByStart; coreMs
        // is when the core created the line (the request).
        std::uint32_t coreMs = 0;
        bool timedByStart = false;
        // A stop before the natural end (reason != 1): its time and reason.
        std::uint32_t stopMs = 0;
        std::uint32_t stopReason = 0;
        // The natural end (stop reason 1) on the clip's clock, 0 when unknown (a
        // clip before SPCH v2, or the recording ended first; speech_resume.h). A
        // jump into the line says it again with the time already played as its
        // start offset.
        std::uint32_t endMs = 0;
        // The line was already audible this long at timeMs (a line playing when
        // the recording or the saved buffer window began, adopted at that instant).
        // The playback asks it with startedBeforeMs + (playhead - timeMs) as its
        // start offset. SPCH v3; 0 for every line heard from its start.
        std::uint32_t startedBeforeMs = 0;
        // The request fields the engine copied to the line when it created it
        // (volume type, audibility, ...; speech_request.h), section SPRQ; not read
        // for an older clip.
        replay::speech::request::Fields request;
    };

    // Timed events of one kind, in time order.
    template <typename Event>
    class TimedEvents
    {
    public:
        void Begin() { m_events.clear(); }
        // Times must not go backwards (a late one is clamped). Returns the
        // index of the event.
        std::size_t Add(const Event& event)
        {
            Event copy = event;
            if (!m_events.empty() && copy.timeMs < m_events.back().timeMs)
                copy.timeMs = m_events.back().timeMs;
            m_events.push_back(copy);
            return m_events.size() - 1;
        }
        Event& At(std::size_t index) { return m_events[index]; }
        const std::vector<Event>& Events() const { return m_events; }
        // Replay buffer trim, drops the events before timeMs. Returns how many
        // went.
        std::size_t DropBefore(std::uint32_t timeMs)
        {
            std::size_t count = 0;
            while (count < m_events.size() && m_events[count].timeMs < timeMs)
                ++count;
            if (count)
                m_events.erase(m_events.begin(), m_events.begin() + static_cast<std::ptrdiff_t>(count));
            return count;
        }
        // Indices of the events with afterMs < timeMs <= untilMs. The first index
        // by binary search (Add keeps the times in order). No state is kept, so a
        // scrub back or a jump needs no reset: each call finds its own start, and
        // events sharing a time all fall on the same side.
        void Range(std::uint32_t afterMs, std::uint32_t untilMs, std::vector<std::size_t>& out) const
        {
            out.clear();
            if (untilMs <= afterMs)
                return;
            const auto first = std::upper_bound(m_events.begin(), m_events.end(), afterMs,
                [](std::uint32_t time, const Event& event) { return time < event.timeMs; });
            for (auto it = first; it != m_events.end() && it->timeMs <= untilMs; ++it)
                out.push_back(static_cast<std::size_t>(it - m_events.begin()));
        }
        // The first index with timeMs > afterMs (events.size() when none).
        std::size_t FirstAfter(std::uint32_t afterMs) const
        {
            return static_cast<std::size_t>(std::upper_bound(m_events.begin(), m_events.end(), afterMs,
                                                [](std::uint32_t time, const Event& event) { return time < event.timeMs; }) -
                m_events.begin());
        }

    private:
        std::vector<Event> m_events;
    };

    class EventTrack : public TimedEvents<ShotEvent>
    {
    public:
        const std::vector<ShotEvent>& Shots() const { return Events(); }
    };
    using SpeechTrack = TimedEvents<SpeechEvent>;

    // One foot-down event of a ped's footstep audio: the event id and foot the
    // engine gave the footstep entry, with its remaining arguments, replayed as
    // is on the puppet.
    struct FootstepEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::int32_t eventId = -1;
        std::uint32_t arg3 = 0;
        std::uint8_t foot = 0;
        std::uint8_t arg5 = 0;
        std::uint8_t flag = 0;
        std::uint8_t arg8 = 0;
        std::uint32_t arg7 = 0;
        // The step container (the standing material of that foot, foot_material.h)
        // the step played with: its name hash and ref low dword; 0 = unknown (clip
        // before FOOT v2).
        std::uint32_t materialHash = 0;
        std::uint32_t materialRef = 0;
        // The foot's wetness the step played with (foot_material.h EncodeWetness);
        // 0 = unknown (clip before FOOT v3).
        std::uint8_t wetness = 0;
        // The per-foot volume and filters the step played with (the quadruped's
        // hit pattern; foot_material.h FootMix); mixKnown 0 = unknown (clip before
        // FOOT v4).
        std::uint8_t mixKnown = 0;
        float mixVolume = 0.0f;
        std::uint16_t mixHighPass = 0;
        std::uint16_t mixLowPass = 0;
        // FOOT v5 (foot_material.h FootDrive): bit 0 base known, bit 1 impact
        // known; the base volume (foot audio +0x510) and the foot's locomotion value
        // (the step's variable 0x01401A30).
        std::uint8_t driveKnown = 0;
        float driveBase = 0.0f;
        float driveImpact = 0.0f;
        // FOOT v6 (foot_material.h kLayerBase): the foot's five ground layer
        // weights the step played with (EncodeLayer, thousandths; layersKnown 0 =
        // unknown) and the ped audio holder's LOD at the foot-down (0xFF =
        // unknown).
        std::uint8_t layersKnown = 0;
        std::uint16_t layer[5] = {};
        std::uint8_t audioLod = 0xFF;
    };
    using FootstepTrack = TimedEvents<FootstepEvent>;
    // An explosion sound, played only in normal forward play; the tag resolves
    // the audio config again.
    struct ExplosionAudioEvent
    {
        std::uint32_t timeMs = 0;
        bool hasInstance = false;
        std::int32_t tag = -1;
        std::uint32_t overrideRef = 0;
        float position[3] = {};
        std::uint8_t underwater = 0;
        std::uint8_t b31 = 0;
        std::uint8_t b32 = 0;
        std::uint32_t interior = 0;
        std::uint32_t caller = 0;
    };
    using ExplosionAudioTrack = TimedEvents<ExplosionAudioEvent>;
    // An animation audio event of a recorded ped or horse (the clip's Audio
    // tag: AddAnimEvent 0x2C2D934 on the ped's audio entity), kept only when the
    // engine made nothing observable from it (no sound, footstep or speech line:
    // those have their own tracks). Playback queues it on the puppet's audio
    // entity through the same AddAnimEvent, in normal forward play only, and the
    // engine's handler plays it. The payload is {+0 extra, +4 hash, +8 word 0,
    // +0x10 qword 0}: no pointer.
    struct AnimAudioEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::uint32_t hash = 0;
        std::int32_t extra = -1;
    };
    using AnimAudioTrack = TimedEvents<AnimAudioEvent>;
    // Wetness and environment dirt of a recorded ped: state, one event when it
    // changes.
    struct SurfaceEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        float wet[4] = {};
        std::uint8_t wetFlags = 0;
        std::int32_t dirt[3] = {};
    };
    using SurfaceTrack = TimedEvents<SurfaceEvent>;

    // One particle effect start (blood, foot dust, splashes, impacts, explosion
    // bursts): the effect the engine started, where (the instance matrix: the
    // offset on the entity's bone when attached, the world matrix otherwise) and
    // how (zoom, flags). Attached effects on an unrecorded entity are not
    // stored; the shot record keeps the muzzle flash and the bullet impact.
    // FxSettings: the instance fields the engine's sites write before the start
    // (build 1491.50): +0x350 (0x2C4F508, from the entity), +0x380..+0x38B and
    // +0x38C (0x2C4E008, 0x2C4DE90), +0x39C (0x145292E, muzzle), +0x3A0
    // (0x144D633, impact), +0x3A8 (the relocate distance: 10.0 at the instance
    // init 0x29D6179, 1.0 at the hoof site 0x146C1F9), +0x3BC (u16, 0x29D61BB
    // init, site 0x145441C); +0x5CC is evidence only. Read at the start (and per
    // tick for a registered effect), written back on our instance before its
    // start. The names "tint", "alpha", "lod" and "cull" are guesses; offsets
    // and writers come from the binary.
    struct FxSettings
    {
        static constexpr std::uint32_t kVel350 = 0x01;
        static constexpr std::uint32_t kTint380 = 0x02;
        static constexpr std::uint32_t kAlpha38C = 0x04;
        static constexpr std::uint32_t kAux39C = 0x08;
        static constexpr std::uint32_t kLod3A0 = 0x10;
        static constexpr std::uint32_t kTele3A8 = 0x20;
        static constexpr std::uint32_t kCull3BC = 0x40;
        static constexpr std::uint32_t kEv5CC = 0x80;
        static constexpr std::uint32_t kAll = 0xFF;
        // The fields written back (+0x5CC stays evidence).
        static constexpr std::uint32_t kApplied = kAll & ~kEv5CC;
        std::uint32_t mask = 0;
        float vel350[4] = {};
        float tint380[3] = {};
        float alpha38C = 0.0f;
        float aux39C = 0.0f;
        float lod3A0 = 0.0f;
        float tele3A8 = 0.0f;
        std::uint16_t cull3BC = 0;
        std::uint32_t ev5CC = 0;
    };
    // Field by field (the bits of the floats: a NaN equals itself), only the
    // fields in `mask`; returns the bits of the fields that differ.
    inline std::uint32_t FxSettingsDiff(const FxSettings& a, const FxSettings& b, std::uint32_t mask)
    {
        const auto same = [](const void* x, const void* y, std::size_t size) {
            const auto* p = static_cast<const std::uint8_t*>(x);
            const auto* q = static_cast<const std::uint8_t*>(y);
            for (std::size_t n = 0; n < size; ++n)
                if (p[n] != q[n])
                    return false;
            return true;
        };
        std::uint32_t diff = 0;
        if ((mask & FxSettings::kVel350) && !same(a.vel350, b.vel350, sizeof(a.vel350)))
            diff |= FxSettings::kVel350;
        if ((mask & FxSettings::kTint380) && !same(a.tint380, b.tint380, sizeof(a.tint380)))
            diff |= FxSettings::kTint380;
        if ((mask & FxSettings::kAlpha38C) && !same(&a.alpha38C, &b.alpha38C, 4))
            diff |= FxSettings::kAlpha38C;
        if ((mask & FxSettings::kAux39C) && !same(&a.aux39C, &b.aux39C, 4))
            diff |= FxSettings::kAux39C;
        if ((mask & FxSettings::kLod3A0) && !same(&a.lod3A0, &b.lod3A0, 4))
            diff |= FxSettings::kLod3A0;
        if ((mask & FxSettings::kTele3A8) && !same(&a.tele3A8, &b.tele3A8, 4))
            diff |= FxSettings::kTele3A8;
        // The byte +0x3BC only; +0x3BD (the high byte of the u16 kept in the clip)
        // is the instance update's own (0x29ED5D9) and is neither written back nor
        // compared.
        if ((mask & FxSettings::kCull3BC) && (a.cull3BC & 0xFFu) != (b.cull3BC & 0xFFu))
            diff |= FxSettings::kCull3BC;
        if ((mask & FxSettings::kEv5CC) && a.ev5CC != b.ev5CC)
            diff |= FxSettings::kEv5CC;
        return diff;
    }

    // Drops from `mask` a float field that is not a finite number (a damaged
    // clip, a live value caught mid write): such a value is never written into
    // an instance.
    inline void FxSettingsKeepFinite(FxSettings& s)
    {
        const auto finite = [](const float* v, int n) {
            for (int i = 0; i < n; ++i)
            {
                // The exponent bits all set: an infinity or a NaN (any fp model).
                std::uint32_t bits = 0;
                std::memcpy(&bits, &v[i], 4);
                if ((bits & 0x7F800000u) == 0x7F800000u)
                    return false;
            }
            return true;
        };
        if (!finite(s.vel350, 4))
            s.mask &= ~FxSettings::kVel350;
        if (!finite(s.tint380, 3))
            s.mask &= ~FxSettings::kTint380;
        if (!finite(&s.alpha38C, 1))
            s.mask &= ~FxSettings::kAlpha38C;
        if (!finite(&s.aux39C, 1))
            s.mask &= ~FxSettings::kAux39C;
        if (!finite(&s.lod3A0, 1))
            s.mask &= ~FxSettings::kLod3A0;
        if (!finite(&s.tele3A8, 1))
            s.mask &= ~FxSettings::kTele3A8;
    }

    struct FxEvent
    {
        std::uint32_t timeMs = 0;
        std::uint32_t effectHash = 0;
        std::uint32_t assetHash = 0;
        std::uint32_t callerSite = 0; // engine caller RVA (evidence)
        ReplayId entity;              // invalid: placed in the world
        std::int32_t bone = -1;
        float zoom = 1.0f;
        std::uint32_t flags = 0;
        // Offset on the bone when attached; the world matrix otherwise.
        float matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        std::uint8_t evolutions[16] = {}; // the instance's evolution values at start
        // Section FXSS, absent in older clips.
        bool hasSettings = false;
        FxSettings settings;
    };
    using FxTrack = TimedEvents<FxEvent>;

    // One decal the engine added (bullet holes, blood splats, scorch marks,
    // footprints): the settings struct the rage add received (0x14C bytes, world
    // position and orientation at +0x00, dimensions at +0x30, attach flag at
    // +0x13A bit 0) plus the entity it was attached to (a recorded entity by id,
    // or a world object by model and position). Replayed by adding the same
    // settings on the puppet.
    struct DecalEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        bool attached = false;
        std::uint32_t attachModel = 0;
        float attachPos[3] = {};
        std::uint32_t callerReturn = 0; // evidence: the core's caller
        std::uint32_t engineId = 0;     // the id the engine gave it (the result)
        std::uint32_t requestId = 0;    // the id the caller asked for (a pool re-adds with its own)
        std::uint8_t flag = 0;
        // The info record as (hash, index); settings +0x88 is zeroed in the copy (a
        // raw engine pointer never goes into a recording).
        bool infoKnown = false;
        std::uint32_t infoHash = 0;
        std::int32_t infoIndex = 0;
        std::uint8_t settings[0x150] = {};
        // The add named an entity (entity, or attachModel and attachPos) even
        // without the attach flag: a bullet mark on a wooden deck. The engine keeps
        // that entity with the decal and paints the projected mirror on it itself.
        // Always true when attached with an entity found; false in DECL version 1
        // clips.
        bool hasEntity = false;
    };
    using DecalTrack = TimedEvents<DecalEvent>;
    // A projected decal (scorch, char) the engine painted on an entity; the
    // entity is a recorded one (id) or a world object (model + position).
    struct ProjDecalEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::uint32_t attachModel = 0;
        float attachPos[3] = {};
        std::uint32_t typeHash = 0;
        std::int32_t arg4 = 0;
        float position[4] = {};
        float direction[4] = {};
        float side[4] = {};
        float size = 0.0f;
        std::uint16_t arg9 = 0;
        std::uint8_t arg10 = 0;
        std::int32_t arg11 = 0;
        std::int32_t arg12 = 0;
        // The info as (hash, index) in the decal info table, never the raw pointer
        // (see decal_observer.h kSettingsInfo). The 13th argument is an owner, not
        // an info (a blood record or null); infoKnown=false is played with a null
        // owner.
        bool infoKnown = false;
        std::uint32_t infoHash = 0;
        std::int32_t infoIndex = 0;
    };
    using ProjDecalTrack = TimedEvents<ProjDecalEvent>;

    // A limb of a recorded ped the engine dismembered (gore bit set).
    struct GoreEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::int32_t limb = 0;
    };
    using GoreTrack = TimedEvents<GoreEvent>;

    // The gameplay gore event (handler 0xB4EB3C): the whole 0x50-byte event
    // with the ped and inflictor as replay ids (the pointers are replaced at
    // playback).
    struct GoreHitEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        ReplayId inflictor; // invalid: none or not recorded
        std::uint8_t context[0x50] = {};
        std::uint8_t result = 0;
        std::uint32_t caller = 0;
    };
    using GoreHitTrack = TimedEvents<GoreHitEvent>;

    // A ped damage record (blood, wound, damage pack) finalized on a
    // recorded entity: the whole 0x160-byte record and where it came from.
    struct BloodEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::uint8_t slot = 0;
        std::uint32_t caller = 0;
        std::int32_t index = -1;
        float age = 0.0f;
        // Record +0x78, an entity pointer the engine copies into the new
        // record: kept as the recorded entity it named, zeroed in `record`.
        // Null when it named nothing recorded (null is a value the engine
        // itself passes, bullet path 0x125E11C).
        ReplayId ref78Entity;
        std::uint8_t record[0x160] = {};
    };
    using BloodTrack = TimedEvents<BloodEvent>;

    // An engine fire (seed or spread child): its start descriptor as the
    // engine received it, the state right after init, the entities it names
    // as replay ids, and its end.
    struct FireStartEvent
    {
        std::uint32_t timeMs = 0;
        std::int32_t slot = -1;
        std::uint32_t type = 0;
        std::uint32_t group = 0;
        float position[3] = {};
        std::uint64_t rng = 0; // fire +0x1C8
        ReplayId entityA;      // desc +0x40
        ReplayId entityB;      // desc +0x48
        // An entity the recording does not have (a map or stage object: the deck a
        // molotov burns on) as its model and position, so playback can hand the
        // same world object to the engine (the registered fx and decals find theirs
        // the same way). 0 when none.
        std::uint32_t entityAModel = 0;
        std::uint32_t entityBModel = 0;
        float entityAPos[3] = {};
        float entityBPos[3] = {};
        std::uint32_t caller = 0;
        // A fire alive (or a remnant left) when the recording started: its
        // age at record start (seconds) and whether it was only a remnant.
        bool initial = false;
        bool initialRemnant = false;
        float initialAge = 0.0f;
        std::uint8_t desc[0x120] = {};
        // An initial fire the start detour never saw (lit before the observer was
        // installed), its descriptor rebuilt from the engine's fire (fire_initial.h
        // DescFromFire).
        bool rebuilt = false;
    };
    using FireStartTrack = TimedEvents<FireStartEvent>;

    struct FireStopEvent
    {
        std::uint32_t timeMs = 0;
        std::uint32_t start = 0; // index into the start track
        std::uint8_t instant = 0;
        std::uint8_t arg3 = 0;
    };
    using FireStopTrack = TimedEvents<FireStopEvent>;

    // The burning / burnt disc a fire left on the ground (its area item),
    // sampled at 4 Hz while the fire or its remnant owns it.
    struct FireItemSample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t start = 0;
        float outer = 0.0f;
        float burnt = 0.0f;
        float ember = 0.0f;
        float glow = 0.0f;
        float width = 0.0f;
    };
    using FireItemTrack = TimedEvents<FireItemSample>;

    // A decal the engine removed by id (a growing blood pool is re-added
    // and its previous decal removed every frame).
    struct DecalRemoveEvent
    {
        std::uint32_t timeMs = 0;
        std::uint32_t engineId = 0;
        // 1 when the recorder found the decal dead (the engine's own expiry,
        // IsDecalAlive) instead of seeing a removal call; 0 in DCRM version 1
        // clips.
        std::uint8_t expired = 0;
    };
    using DecalRemoveTrack = TimedEvents<DecalRemoveEvent>;

    // A registered (continuous) particle effect, keyed by owner and id: the
    // engine gets or creates it by key every frame it is alive and updates its
    // instance; the recording keeps the instance state per frame (stored only
    // when it changes) from its creation to its last update.
    struct RegisteredFxFrame
    {
        std::uint32_t timeMs = 0;
        float matrix[16] = {};
        float parent[16] = {};
        std::uint8_t evolutions[16] = {};
        float zoom = 1.0f;
        float aux350[4] = {};
        std::uint32_t flags = 0;        // +0x580 as recorded
        std::uint32_t planeCount = 0;   // collision planes (a ground fire's surface)
        float planes[16 * 12] = {};
        float start310[4] = {};         // tracer start point (instance +0x310)
        float farClip = -1.0f;          // far clip override (instance +0x3C4)
    };
    // An attach the instance took after its track started (the dynamite's fuse
    // trail once the thrown stick is a recorded prop, a fire arrow's flame on
    // the arrow that stuck): from timeMs on, the instance rides entity `id` at
    // `bone`.
    struct RegisteredFxAttach
    {
        std::uint32_t timeMs = 0;
        ReplayId id;
        std::int32_t bone = -1;
    };
    // Section RGSS: the settings of a registered instance from timeMs on (the
    // first entry is the one at its creation).
    struct RegisteredFxSettingsChange
    {
        std::uint32_t timeMs = 0;
        FxSettings settings;
    };
    // Section RGSS: a stretch the engine's site did not call the key (no call
    // strictly inside (fromMs, toMs)); the registry stops the instance softly
    // (0x2C52635) and restarts it when the call comes back (0x2C5268A), one
    // burst per call run (the hoof dust).
    struct RegisteredFxGap
    {
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
    };
    struct RegisteredFxTrack
    {
        std::uint32_t effectHash = 0;
        std::uint8_t flag = 0; // the engine call's r9b and last two bytes, replayed as is
        std::uint8_t arg8 = 0;
        std::uint8_t arg9 = 0;
        std::uint32_t callerSite = 0; // evidence
        // The entity the engine attached the instance to (a burning object,
        // a wading ped): the recorded entity when it is one, otherwise the
        // entity's model and position so playback can find the same world
        // object (map and stage objects persist across the replay).
        bool attached = false;
        ReplayId attachId;
        std::int32_t attachBone = -1;
        std::uint32_t attachModel = 0;
        float attachPos[3] = {};
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;      // last update seen
        std::vector<RegisteredFxFrame> frames; // time ascending
        // Attach changes after the start (time ascending); before the first one
        // the track's own attach holds.
        std::vector<RegisteredFxAttach> attachTimeline;
        // Section RGSS (optional): settings changes and the stretches without a
        // call, both time ascending.
        std::vector<RegisteredFxSettingsChange> settings;
        std::vector<RegisteredFxGap> callGaps;
    };
    // The settings in force at timeMs (the first entry before it); null when
    // the track has none.
    inline const FxSettings* RegisteredFxSettingsAt(const RegisteredFxTrack& track, std::uint32_t timeMs)
    {
        if (track.settings.empty())
            return nullptr;
        const FxSettings* found = &track.settings.front().settings;
        for (const RegisteredFxSettingsChange& entry : track.settings)
        {
            if (entry.timeMs > timeMs)
                break;
            found = &entry.settings;
        }
        return found;
    }
    // The attach change in force at timeMs (the last one at or before it);
    // null before the first one (the track's own attach holds).
    inline const RegisteredFxAttach* RegisteredFxAttachAt(const RegisteredFxTrack& track, std::uint32_t timeMs)
    {
        const RegisteredFxAttach* found = nullptr;
        for (const RegisteredFxAttach& entry : track.attachTimeline)
        {
            if (entry.timeMs > timeMs)
                break;
            found = &entry;
        }
        return found;
    }
    // The frame in force at timeMs (the last one at or before it); null
    // before the first frame or after endMs. A binary search: a track has
    // no frame cap (a lamp carried for minutes stores every changed frame).
    inline const RegisteredFxFrame* RegisteredFxFrameAt(const RegisteredFxTrack& track, std::uint32_t timeMs)
    {
        if (track.frames.empty() || timeMs < track.frames.front().timeMs || timeMs > track.endMs)
            return nullptr;
        const auto after = std::upper_bound(track.frames.begin(), track.frames.end(), timeMs,
            [](std::uint32_t t, const RegisteredFxFrame& frame) { return t < frame.timeMs; });
        return &*std::prev(after);
    }

    // The registered fx tracks a recording keeps. A track keeps every frame
    // (no per-track cap: the recording's byte budget, ReplayBufferMB and
    // ManualMaxMB, bounds them, replay_buffer.h ApproxBytes counts them). The
    // track count is only a guard against a runaway engine site: Saint Denis
    // makes about 15 tracks a second, so this is over an hour of it, and the
    // buffer gives back the tracks its trim emptied (DropEmptiedRegisteredFx).
    constexpr std::size_t kMaxRegisteredFxTracks = 65536;
    constexpr std::size_t kDroppedRegisteredFxTrack = static_cast<std::size_t>(-1);

    // The buffer's trim (replay_buffer.h TrimBefore) empties the tracks that
    // ended before its base; this removes them unless a live key still holds
    // the track (held[index] != 0: the recorder adds to it). remap[old index]
    // is the track's new index, kDroppedRegisteredFxTrack for a removed one.
    // A track with frames is never removed. Returns the tracks removed.
    inline std::size_t DropEmptiedRegisteredFx(std::vector<RegisteredFxTrack>& tracks, std::uint32_t keepFrom,
        const std::vector<std::uint8_t>& held, std::vector<std::size_t>& remap)
    {
        remap.assign(tracks.size(), kDroppedRegisteredFxTrack);
        std::size_t write = 0;
        for (std::size_t index = 0; index < tracks.size(); ++index)
        {
            const bool isHeld = index < held.size() && held[index] != 0;
            if (tracks[index].frames.empty() && tracks[index].endMs < keepFrom && !isHeld)
                continue;
            if (write != index)
                tracks[write] = std::move(tracks[index]);
            remap[index] = write++;
        }
        const std::size_t dropped = tracks.size() - write;
        tracks.resize(write);
        return dropped;
    }

    // World matrix of an effect instance from its offset and parent matrices
    // (4 x float4, rows = X, Y, Z axes and position; row vectors, so the
    // offset is applied first): the engine places a hoof-dust puff with the
    // world matrix in the parent and an identity offset, and a script world
    // effect with an identity parent and the world matrix in the offset. A
    // parent that was never set (all zero) counts as identity. Affine: the
    // w of the rows is not used (the engine leaves 0 or NaN there), the
    // position row is always a point.
    inline void ComposeFxWorld(const float offset[16], const float parent[16], float out[16])
    {
        bool zero = true;
        for (int n = 0; n < 16 && zero; ++n)
            zero = parent[n] == 0.0f;
        if (zero)
        {
            for (int n = 0; n < 16; ++n)
                out[n] = offset[n];
            return;
        }
        float result[16] = {};
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k)
                    sum += offset[row * 4 + k] * parent[k * 4 + col];
                if (row == 3)
                    sum += parent[12 + col];
                result[row * 4 + col] = sum;
            }
            result[row * 4 + 3] = parent[row * 4 + 3];
        }
        for (int n = 0; n < 16; ++n)
            out[n] = result[n];
    }

    // One melee impact sound: the ped plus the melee definition hash. The
    // engine's definition object (vtable and key hash +0x48) is kept so playback
    // can reuse it in the same session; the definition is an action result and
    // infoHash is its name hash, the stable key playback resolves it again by.
    struct MeleeEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::uint64_t info = 0;
        std::uint64_t infoVtable = 0;
        std::uint32_t infoHash = 0;
    };
    using MeleeTrack = TimedEvents<MeleeEvent>;

    // One body impact: the ped whose impact sound plays, the other ped of the
    // hit, the position and the values the engine gave the impact function.
    struct BodyImpactEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        ReplayId other;
        float position[4] = {};
        std::uint64_t arg4 = 0;
        std::uint64_t arg5 = 0;
        std::uint64_t definition = 0;
        std::uint64_t definitionVtable = 0;
        std::uint32_t definitionHash = 0;
        std::uint64_t arg7 = 0;
        std::uint64_t arg10 = 0;
    };
    using BodyImpactTrack = TimedEvents<BodyImpactEvent>;

    // One ped sound request: the sound ref, the audio entity relative to the
    // ped audio, the init params value fields (empty when the request had none)
    // and its flags.
    struct SoundEvent
    {
        std::uint32_t timeMs = 0;
        ReplayId entity;
        std::uint64_t audioOffset = 0;
        std::uint64_t soundRef = 0;
        std::vector<std::uint8_t> params;
        std::uint64_t flags[4] = {};
        // 0: deferred request (0x310E680); 1: direct create-and-play on the
        // ped audio; 2: direct create-and-play on a static audio entity
        // (collision and similar) tracking the ped; 3: collision sound on a
        // static audio entity at the ped's position, without tracker; 4:
        // direct create-and-play on a weapon object's audio (the weapons a
        // ped carries rattle when it falls), entity = the weapon, offset
        // relative to that audio.
        std::uint8_t kind = 0;
        std::uint64_t globalAudio = 0;
    };
    using SoundTrack = TimedEvents<SoundEvent>;

    // A recorded entity whose skeleton changes (a metaped losing its hat changes
    // its bone count) keeps its ReplayId and continues in a new pose segment:
    // its own pose record, bone count and appearance from startMs on. The create
    // packet is the segment before the first change.
    struct PoseSegment
    {
        ReplayId entity;
        std::uint32_t startMs = 0;
        std::uint32_t recordId = 0;
        std::uint32_t boneCount = 0;
        Appearance appearance;
    };

    class PoseSegmentTrack
    {
    public:
        // Pose records of segments are numbered apart from packed ReplayIds
        // (whose top bit is always clear).
        static constexpr std::uint32_t kRecordBit = 0x80000000u;

        void Begin()
        {
            m_segments.clear();
            m_next = 1;
        }
        std::uint32_t NextRecordId() { return kRecordBit | m_next++; }
        // Times per entity must not go backwards.
        void Add(const PoseSegment& segment) { m_segments.push_back(segment); }
        // The entity's latest segment started at or before timeMs, or null
        // when the create packet still applies.
        const PoseSegment* At(ReplayId entity, std::uint32_t timeMs) const;
        // Owner of a segment record, invalid for anything else.
        ReplayId OwnerOf(std::uint32_t recordId) const;
        const std::vector<PoseSegment>& Segments() const { return m_segments; }

    private:
        std::vector<PoseSegment> m_segments;
        std::uint32_t m_next = 1;
    };

    // Decides which slice of the timeline plays its events this frame.
    class ForwardEventCursor
    {
    public:
        // Steps larger than this are a jump, not normal play.
        static constexpr std::uint32_t kMaxStepMs = 250;

        void Reset(std::uint32_t timeMs) { m_lastMs = timeMs; }
        // True with (fromMs, toMs] to play when the playhead moved forward
        // by a normal step; otherwise just follows the playhead.
        bool Advance(std::uint32_t timeMs, bool normalPlay, std::uint32_t& fromMs, std::uint32_t& toMs);

    private:
        std::uint32_t m_lastMs = 0;
    };
}
