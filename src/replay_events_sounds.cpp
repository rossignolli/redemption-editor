// replay_events_sounds.cpp: entity, collision, melee and impact sounds recorded and played back, anim audio, footsteps played, the sound tallies.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    std::vector<PoseV2LiveCollisionSound> g_poseV2LiveCollisionSounds;
    std::uint64_t g_poseV2SoundStopCursor = 0;
    namespace
    {

    // Playback of the collision sounds: at most kSlots at once, each in a
    // slot this module owns (the engine nulls it when the sound ends).
    // The engine keeps the address of each sound slot below in the sound
    // (+0x58) and clears it from its audio thread when the sound ends, which
    // can be after a hot reload unmapped this image (an AV in the engine's
    // audio thread writing into the unloaded .asi). The slots live in
    // process heap memory that is never freed (a few hundred bytes per load),
    // so a late clear always lands in valid memory. Static fallback only if
    // the heap refuses.
    template <std::size_t N>
    std::uint64_t (&EngineSoundSlots())[N]
    {
        static std::uint64_t fallback[N] = {};
        void* memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(fallback));
        return *static_cast<std::uint64_t(*)[N]>(memory ? memory : static_cast<void*>(fallback));
    }

    constexpr std::size_t kPoseV2CollisionSlots = 24;
    std::uint64_t (&g_poseV2CollisionSlot)[kPoseV2CollisionSlots] = EngineSoundSlots<kPoseV2CollisionSlots>();
    std::size_t g_poseV2CollisionSlotEvent[kPoseV2CollisionSlots] = {};
    }
    std::uint32_t g_poseV2CollisionLastMs = 0;
    std::size_t g_poseV2CollisionNext = 0;
    std::uint64_t g_poseV2CollisionPlayed = 0;
    std::uint64_t g_poseV2CollisionRefused = 0;
    std::string g_poseV2CollisionLast;
    namespace
    {

    // Ground impacts (one-shots on the static collision audio) play through
    // an owned slot, like the collision loops, so their position can be set:
    // queued through create-and-play, none is heard.
    constexpr std::size_t kPoseV2ImpactSlots = 8;
    std::uint64_t (&g_poseV2ImpactSlot)[kPoseV2ImpactSlots] = EngineSoundSlots<kPoseV2ImpactSlots>();
    std::size_t g_poseV2ImpactNext = 0;

    // Entity sounds (replay_recording.h EntitySound): a persistent sound the
    // engine tied to a recorded object through its tracker (the lit
    // molotov's loop on the flying bottle). Recording follows
    // each one from its create to its stop (StopAndForget), its slot no
    // longer holding it, or its object gone. Objects only: peds have their
    // own sound tracks and a puppet vehicle may make its own loops.
    struct PoseV2LiveEntitySound
    {
        std::uint64_t sound = 0;
        std::uint64_t slot = 0;
        Object object = 0;
        std::size_t index = 0;
    };
    std::vector<PoseV2LiveEntitySound> g_poseV2LiveEntitySounds;
    std::uint64_t g_poseV2EntitySoundCursor = 0;
    std::uint64_t g_poseV2EntitySoundStopCursor = 0;
    }
    std::uint64_t g_poseV2EntitySoundsUnowned = 0;
    std::uint64_t g_poseV2EntitySoundsAudioOther = 0;
    namespace
    {
    std::uint32_t g_poseV2EntitySoundLogs = 0;

    // Ped audio sounds (RecordPoseV2PedAudioSound below).
    struct PoseV2PedAudioOwner
    {
        Ped ped = 0;
        replay::entity::ReplayId id;
        std::uint64_t holder = 0;
    };
    std::vector<PoseV2PedAudioOwner> g_poseV2PedAudioOwners;
    }
    std::uint64_t g_poseV2PedAudioRecorded = 0;
    std::uint64_t g_poseV2PedAudioUnowned = 0; // no tracker, no recorded ped
    namespace
    {
    std::uint32_t g_poseV2PedAudioLogs = 0;
    }

    void BeginPoseV2EntitySounds()
    {
        std::vector<replay::audio::EntitySoundCreate> stale;
        replay::audio::CollectEntitySoundsSince(g_poseV2EntitySoundCursor, stale);
        std::vector<replay::audio::SoundStop> staleStops;
        replay::audio::CollectSoundStopsSince(g_poseV2EntitySoundStopCursor, staleStops);
        g_poseV2LiveEntitySounds.clear();
        g_poseV2EntitySoundsUnowned = 0;
        g_poseV2EntitySoundsAudioOther = 0;
        g_poseV2EntitySoundLogs = 0;
        g_poseV2PedAudioRecorded = 0;
        g_poseV2PedAudioUnowned = 0;
        g_poseV2PedAudioLogs = 0;
    }
    namespace
    {

    // Sounds of a recorded object that the object-owned paths missed. The lit
    // molotov's match (p_matchstick01x) is taken into the hand, lit and
    // thrown away within ~600 ms; the attached-object scan adopts it up to
    // 400 ms late, so a sound it made first would have no owner, and
    // one-shots on an object (created and played, or a deferred request owned
    // by it) no track at all. A candidate is kept up to
    // kPoseV2ObjectSoundWaitMs and attributed as soon as the object is
    // recorded: a sound belongs with its entity and is dropped only when the
    // entity is never recorded.
    constexpr std::uint32_t kPoseV2ObjectSoundWaitMs = 600;
    constexpr std::uint32_t kPoseV2OneShotStartWindowMs = 500;
    struct PoseV2ObjectSoundCandidate
    {
        std::uint64_t timeUs = 0;
        std::uint64_t audio = 0;
        std::uint64_t soundRef = 0;
        bool byHash = false;
        std::uint64_t tracker = 0; // params +0xC8
        std::uint64_t owner = 0;   // deferred request owner
        bool persistent = false;
        std::uint64_t sound = 0;   // persistent: the sound and its slot
        std::uint64_t slot = 0;
        bool hasParams = false;
        std::uint8_t params[replay::audio::kSoundParamsBytes] = {};
        std::uint32_t callers[3] = {};
        const char* path = "";
    };
    std::vector<PoseV2ObjectSoundCandidate> g_poseV2ObjectSoundPending;
    }
    std::uint64_t g_poseV2ObjectSoundsLate = 0;    // attributed after a wait
    std::uint64_t g_poseV2ObjectSoundsExpired = 0; // never owned by a recorded object
    std::uint64_t g_poseV2ObjectSoundsOneShot = 0;
    namespace
    {
    std::uint32_t g_poseV2ObjectSoundLogs = 0;
    constexpr std::size_t kPoseV2ObjectSoundPendingCap = 96;

    void QueuePoseV2ObjectSound(const PoseV2ObjectSoundCandidate& candidate)
    {
        if (g_poseV2ObjectSoundPending.size() >= kPoseV2ObjectSoundPendingCap)
        {
            ++g_poseV2ObjectSoundsExpired;
            g_poseV2ObjectSoundPending.erase(g_poseV2ObjectSoundPending.begin());
        }
        g_poseV2ObjectSoundPending.push_back(candidate);
    }
    }

    void ResetPoseV2ObjectSounds()
    {
        g_poseV2ObjectSoundPending.clear();
        g_poseV2ObjectSoundsLate = 0;
        g_poseV2ObjectSoundsExpired = 0;
        g_poseV2ObjectSoundsOneShot = 0;
        g_poseV2ObjectSoundLogs = 0;
    }
    namespace
    {

    void LogPoseV2ObjectSound(const PoseV2ObjectSoundCandidate& candidate, const char* outcome, replay::entity::ReplayId id,
        std::uint32_t waitMs)
    {
        if (g_poseV2ObjectSoundLogs >= 40)
            return;
        ++g_poseV2ObjectSoundLogs;
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        const bool staticAudio = candidate.audio > module && candidate.audio < module + 0x7000000;
        Ctx().log << "POSE_V2_OBJECT_SOUND,path=" << candidate.path << ",outcome=" << outcome
            << ",id=" << (id.Valid() ? replay::entity::IdText(id) : std::string("none")) << std::hex
            << ",ref=0x" << candidate.soundRef << ",audio=" << (staticAudio ? "static:0x" : "heap:0x")
            << (staticAudio ? candidate.audio - module : candidate.audio) << ",callers=0x" << candidate.callers[0] << "/0x"
            << candidate.callers[1] << "/0x" << candidate.callers[2] << std::dec << ",wait_ms=" << waitMs << '\n';
    }

    // Persistent sounds the peds' own audio makes without tracker (the audio
    // entity inside the ped audio holder, span kPedAudioSpan): foley and
    // weapon handling, the shell ~600 ms after each shot, the horse's body
    // sounds. They come through the persistent core by ref (in the census
    // rows: the return after the call to the instance creator, the return in
    // the core after its call to that, and a zero for the audio observer's own
    // detour on the core). They are recorded with the ped and
    // replayed on the puppet's audio entity: ESND entries with pedAudio, the
    // offset taken from the holder.

    void BuildPoseV2PedAudioOwners()
    {
        g_poseV2PedAudioOwners.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !entity.handle || !replay::entities::Exists(entity.handle) ||
                !replay::entities::IsPed(entity.handle))
                continue;
            if (const std::uint64_t holder = replay::audio::PedAudioHolder(replay::entities::AddressOf(entity.handle)))
                g_poseV2PedAudioOwners.push_back({entity.handle, entity.id, holder});
        }
    }

    void RecordPoseV2PedAudioSound(const replay::audio::EntitySoundCreate& create, std::uint32_t nowMs)
    {
        const auto owner = std::find_if(g_poseV2PedAudioOwners.begin(), g_poseV2PedAudioOwners.end(),
            [&](const PoseV2PedAudioOwner& entry) {
                return create.audio >= entry.holder && create.audio - entry.holder < replay::kEntitySoundPedAudioSpan &&
                    ((create.audio - entry.holder) & 7u) == 0; });
        if (owner == g_poseV2PedAudioOwners.end())
        {
            ++g_poseV2PedAudioUnowned;
            return;
        }
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint64_t ageMs = now > create.timeUs ? (now - create.timeUs) / 1000 : 0;
        replay::EntitySound sound;
        sound.entity = owner->id;
        sound.startMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
        sound.soundRef = create.soundRef;
        sound.byHash = create.byHash;
        sound.audioOnEntity = true;
        sound.pedAudio = true;
        sound.audioOffset = static_cast<std::uint32_t>(create.audio - owner->holder);
        sound.caller = create.caller;
        if (create.hasParams)
            sound.params.assign(create.params, create.params + sizeof(create.params));
        std::uint64_t current = 0;
        const bool alive = replay::audio::ReadU64(create.slot, current) && current == create.sound;
        if (alive)
        {
            sound.endMs = sound.startMs;
            Ctx().clip.entitySounds.push_back(std::move(sound));
            g_poseV2LiveEntitySounds.push_back({create.sound, create.slot, owner->ped, Ctx().clip.entitySounds.size() - 1});
        }
        else
        {
            sound.endMs = nowMs > sound.startMs ? nowMs : sound.startMs + 1;
            sound.ended = true;
            Ctx().clip.entitySounds.push_back(std::move(sound));
        }
        ++g_poseV2PedAudioRecorded;
        if (g_poseV2PedAudioLogs < 60)
        {
            ++g_poseV2PedAudioLogs;
            const replay::EntitySound& logged = Ctx().clip.entitySounds.back();
            float position[3] = {};
            if (logged.params.size() >= sizeof(position))
                std::memcpy(position, logged.params.data(), sizeof(position));
            Ctx().log << "POSE_V2_PED_AUDIO_SOUND,phase=record,id=" << replay::entity::IdText(logged.entity) << std::hex
                << ",ref=0x" << logged.soundRef << ",by_hash=" << logged.byHash << ",holder+0x" << logged.audioOffset
                << ",caller=0x" << logged.caller << std::dec << ",has_params=" << !logged.params.empty()
                << ",positioned=" << (position[0] != 0.0f || position[1] != 0.0f || position[2] != 0.0f)
                << ",alive=" << alive << ",time_ms=" << logged.startMs << '\n';
        }
    }

    void TrackPoseV2EntitySounds(bool closeAll)
    {
        REPLAY_PROFILE_ZONE("rec.entity_sounds");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        static std::vector<replay::audio::EntitySoundCreate> creates;
        replay::audio::CollectEntitySoundsSince(g_poseV2EntitySoundCursor, creates);
        bool pedOwnersBuilt = false;
        for (const replay::audio::EntitySoundCreate& create : creates)
        {
            if (!create.tracker)
            {
                // No tracker: a ped audio sound or nothing to record.
                if (!pedOwnersBuilt)
                {
                    BuildPoseV2PedAudioOwners();
                    pedOwnersBuilt = true;
                }
                RecordPoseV2PedAudioSound(create, nowMs);
                continue;
            }
            PoseV2ObjectSoundCandidate candidate;
            candidate.timeUs = create.timeUs;
            candidate.audio = create.audio;
            candidate.soundRef = create.soundRef;
            candidate.byHash = create.byHash;
            candidate.tracker = create.tracker;
            candidate.persistent = true;
            candidate.sound = create.sound;
            candidate.slot = create.slot;
            candidate.hasParams = create.hasParams;
            std::memcpy(candidate.params, create.params, sizeof(candidate.params));
            candidate.callers[0] = create.caller;
            candidate.path = "persistent";
            QueuePoseV2ObjectSound(candidate);
        }
        if (!g_poseV2ObjectSoundPending.empty())
        {
            const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
            const std::uint64_t now = replay::audio::NowUs();
            // `audio` is a train car's own audio entity (0 for the others): its
            // whistle, bell, engine and wheels.
            struct Owner { Object object; replay::entity::ReplayId id; std::uint64_t address; std::uint64_t audio; };
            static std::vector<Owner> owners;
            owners.clear();
            for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                if (prop.object && replay::entities::Exists(prop.object))
                    owners.push_back({prop.object, prop.id, reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(prop.object)), 0});
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                if (weapon.object && replay::entities::Exists(weapon.object))
                    owners.push_back({weapon.object, weapon.id, reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), 0});
            {
                static std::vector<replay::trains::RecordedCarRef> cars;
                replay::trains::RecordedCars(cars);
                for (const replay::trains::RecordedCarRef& car : cars)
                    owners.push_back({car.handle, car.id, car.address, car.audio});
            }
            for (std::size_t pending = 0; pending < g_poseV2ObjectSoundPending.size();)
            {
                const PoseV2ObjectSoundCandidate& create = g_poseV2ObjectSoundPending[pending];
                const std::uint64_t ageMs = now > create.timeUs ? (now - create.timeUs) / 1000 : 0;
                const auto inside = [](std::uint64_t value, std::uint64_t base) {
                    return base && value >= base && value - base < replay::kEntitySoundSpan; };
                const auto insideAudio = [](std::uint64_t value, std::uint64_t base) {
                    return base && value >= base && value - base < replay::kEntitySoundVehicleAudioSpan; };
                const auto owner = std::find_if(owners.begin(), owners.end(), [&](const Owner& entry) {
                    return inside(create.tracker, entry.address) || (create.owner && create.owner == entry.address) ||
                        (!create.persistent && inside(create.audio, entry.address)) ||
                        insideAudio(create.audio, entry.audio) || insideAudio(create.tracker, entry.audio); });
                if (owner == owners.end())
                {
                    if (ageMs <= kPoseV2ObjectSoundWaitMs && !closeAll)
                    {
                        ++pending;
                        continue;
                    }
                    ++g_poseV2EntitySoundsUnowned;
                    ++g_poseV2ObjectSoundsExpired;
                    if (!create.persistent || create.tracker)
                        LogPoseV2ObjectSound(create, "no_owner", {}, static_cast<std::uint32_t>(ageMs));
                    g_poseV2ObjectSoundPending.erase(g_poseV2ObjectSoundPending.begin() + static_cast<std::ptrdiff_t>(pending));
                    continue;
                }
                const PoseV2ObjectSoundCandidate candidate = create;
                g_poseV2ObjectSoundPending.erase(g_poseV2ObjectSoundPending.begin() + static_cast<std::ptrdiff_t>(pending));
                replay::EntitySound sound;
                if (candidate.audio > module && candidate.audio < module + 0x7000000 && (candidate.audio & 7u) == 0)
                    sound.audioRva = candidate.audio - module;
                else if (inside(candidate.audio, owner->address) && ((candidate.audio - owner->address) & 7u) == 0)
                {
                    sound.audioOnEntity = true;
                    sound.audioOffset = static_cast<std::uint32_t>(candidate.audio - owner->address);
                }
                else if (insideAudio(candidate.audio, owner->audio) && ((candidate.audio - owner->audio) & 7u) == 0)
                {
                    // The train car's audio entity.
                    sound.audioOnEntity = true;
                    sound.vehicleAudio = true;
                    sound.audioOffset = static_cast<std::uint32_t>(candidate.audio - owner->audio);
                }
                else
                {
                    ++g_poseV2EntitySoundsAudioOther;
                    LogPoseV2ObjectSound(candidate, "audio_other", owner->id, static_cast<std::uint32_t>(ageMs));
                    continue;
                }
                sound.entity = owner->id;
                sound.startMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
                sound.soundRef = candidate.soundRef;
                sound.byHash = candidate.byHash;
                sound.trackerOffset = inside(candidate.tracker, owner->address)
                    ? static_cast<std::uint32_t>(candidate.tracker - owner->address) : 0;
                sound.caller = candidate.callers[0];
                if (candidate.hasParams)
                    sound.params.assign(candidate.params, candidate.params + sizeof(candidate.params));
                const bool late = ageMs > 50;
                if (late)
                    ++g_poseV2ObjectSoundsLate;
                std::uint64_t current = 0;
                const bool alive = candidate.persistent && replay::audio::ReadU64(candidate.slot, current) && current == candidate.sound;
                if (candidate.persistent && alive)
                {
                    sound.endMs = sound.startMs;
                    Ctx().clip.entitySounds.push_back(std::move(sound));
                    g_poseV2LiveEntitySounds.push_back({candidate.sound, candidate.slot, owner->object, Ctx().clip.entitySounds.size() - 1});
                }
                else if (candidate.persistent)
                {
                    sound.endMs = nowMs > sound.startMs ? nowMs : sound.startMs + 1;
                    sound.ended = true;
                    Ctx().clip.entitySounds.push_back(std::move(sound));
                }
                else
                {
                    sound.oneShot = true;
                    sound.ended = true;
                    sound.endMs = sound.startMs + kPoseV2OneShotStartWindowMs;
                    ++g_poseV2ObjectSoundsOneShot;
                    Ctx().clip.entitySounds.push_back(std::move(sound));
                }
                if (late || !candidate.persistent)
                    LogPoseV2ObjectSound(candidate, late ? "late" : "recorded", owner->id, static_cast<std::uint32_t>(ageMs));
                if (g_poseV2EntitySoundLogs < 40)
                {
                    ++g_poseV2EntitySoundLogs;
                    const replay::EntitySound& logged = Ctx().clip.entitySounds.back();
                    Ctx().log << "POSE_V2_ENTITY_SOUND,phase=record,id=" << replay::entity::IdText(logged.entity) << std::hex
                        << ",ref=0x" << logged.soundRef << ",by_hash=" << logged.byHash
                        << ",audio=" << (logged.vehicleAudio ? "train+0x" : logged.audioOnEntity ? "entity+0x" : "static:0x")
                        << (logged.audioOnEntity ? logged.audioOffset : logged.audioRva)
                        << ",tracker=+0x" << logged.trackerOffset << ",caller=0x" << logged.caller << std::dec
                        << ",one_shot=" << logged.oneShot << ",time_ms=" << logged.startMs << '\n';
                }
            }
        }
        static std::vector<replay::audio::SoundStop> stops;
        replay::audio::CollectSoundStopsSince(g_poseV2EntitySoundStopCursor, stops);
        for (std::size_t index = g_poseV2LiveEntitySounds.size(); index-- > 0;)
        {
            const PoseV2LiveEntitySound live = g_poseV2LiveEntitySounds[index];
            std::uint64_t current = 0;
            const bool stopped = std::any_of(stops.begin(), stops.end(),
                [&](const replay::audio::SoundStop& stop) { return stop.sound == live.sound; });
            const bool gone = !replay::entities::Exists(live.object) || !replay::audio::ReadU64(live.slot, current) ||
                current != live.sound;
            if (!closeAll && !stopped && !gone)
                continue;
            replay::EntitySound& sound = Ctx().clip.entitySounds[live.index];
            sound.endMs = nowMs;
            sound.ended = !closeAll || stopped || gone;
            if (g_poseV2EntitySoundLogs < 80)
            {
                ++g_poseV2EntitySoundLogs;
                Ctx().log << "POSE_V2_ENTITY_SOUND,phase=record_end,id=" << replay::entity::IdText(sound.entity)
                    << ",how=" << (stopped ? "stop" : gone ? "gone" : "record_end") << ",start_ms=" << sound.startMs
                    << ",end_ms=" << sound.endMs << '\n';
            }
            g_poseV2LiveEntitySounds.erase(g_poseV2LiveEntitySounds.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    // Playback: while normal play is inside a sound's span its puppet has
    // it (started when the span is crossed, or mid-span for a loop longer
    // than kPoseV2EntitySoundLoopMs after a scrub or pause, as the registered fx are rebuilt
    // on play), positioned on the puppet every frame; paused, scrubbed or
    // outside its span it is stopped (the scrub rule: nothing is mounted).
    // 48 slots: the ped audio sounds share them.
    constexpr std::size_t kPoseV2EntitySoundSlots = 48;
    constexpr std::uint32_t kPoseV2EntitySoundLoopMs = 1000;
    std::uint64_t (&g_poseV2EntitySoundSlot)[kPoseV2EntitySoundSlots] = EngineSoundSlots<kPoseV2EntitySoundSlots>();
    std::size_t g_poseV2EntitySoundSlotEvent[kPoseV2EntitySoundSlots] = {};
    }
    std::uint32_t g_poseV2EntitySoundLastMs = 0;
    std::uint64_t g_poseV2EntitySoundsPlayed = 0;
    std::uint64_t g_poseV2EntitySoundsRefused = 0;
    std::string g_poseV2EntitySoundLast;
    std::uint32_t g_poseV2EntitySoundPlayLogs = 0;
    // A short (one-shot) sound whose span the playhead crossed but that could
    // not start yet (puppet still dressing, no free slot) waits here, per
    // event, until it starts or its span is over; a scrub, pause or seek
    // forgets it (the scrub rule).
    std::vector<std::uint8_t> g_poseV2EntitySoundWaiting;
    std::vector<std::uint8_t> g_poseV2EntitySoundOutcome;
    std::unordered_map<std::uint32_t, std::string> g_poseV2EntitySoundLastRefusal; // by owner (packed id)
    namespace
    {
    void SetPoseV2EntitySoundOutcome(std::size_t index, std::uint8_t outcome)
    {
        if (index < g_poseV2EntitySoundOutcome.size())
            g_poseV2EntitySoundOutcome[index] = outcome;
    }
    // Per event, within one pass of normal play through its span (cleared by
    // a scrub, pause or seek, like the waiting state): started once, it is
    // not started again when the engine ends it. A ped's sound controller
    // keeps foley referenced for 1 to 5 s (stopped later by StopAndForget)
    // while the sound itself lasts a fraction of that: taken for a loop, it
    // would be restarted every frame after the engine ended it and the slots
    // would run out. A tracked sound that stopped by itself is not created
    // again ("stopped outside the replay").
    constexpr std::uint8_t kPoseV2EntitySoundWaitingState = 1;
    constexpr std::uint8_t kPoseV2EntitySoundDone = 2;
    constexpr std::uint8_t kPoseV2EntitySoundRefusedOnce = 3; // a loop refused a slot: retried, counted once
    // The slots' owner, start and distance to the camera for the slot policy
    // (sound_slot_policy.h).
    std::uint32_t g_poseV2EntitySoundSlotOwner[kPoseV2EntitySoundSlots] = {};
    std::uint32_t g_poseV2EntitySoundSlotStartMs[kPoseV2EntitySoundSlots] = {};
    bool g_poseV2EntitySoundSlotLong[kPoseV2EntitySoundSlots] = {}; // a loop or a long recorded span
    float g_poseV2EntitySoundSlotDistance[kPoseV2EntitySoundSlots] = {};
    }
    std::uint64_t g_poseV2EntitySoundsStolen = 0;
    std::uint64_t g_poseV2EntitySoundsTooFar = 0;
    std::uint64_t g_poseV2EntitySoundsEnded = 0; // ended by the engine inside their span, not restarted
    namespace
    {
    bool g_poseV2EntitySoundSlotHeld[kPoseV2EntitySoundSlots] = {}; // the slot had our sound last frame
    }
    std::uint64_t g_poseV2EntitySoundsLate = 0; // one-shots started after a wait
    // A short ped / train car audio sound stops after its recorded length
    // (puppet_audio_rule.h StopAtRecordedLength).
    std::uint64_t g_poseV2PedAudioLengthStops = 0;
    // Every row's outcome per family (owner, call site, set), once per pass,
    // for the sound-check report and POSE_V2_ENTITY_SOUND_FAMILY
    // (entity_sound_tally.h).
    replay::esndtally::Tally g_poseV2EsndTally;
    std::vector<std::uint8_t> g_poseV2EsndPassCounted; // per row, this pass
    bool g_poseV2EsndNewPass = true;
    namespace
    {
    std::uint32_t g_poseV2EsndPassFromMs = 0;
    }
    std::vector<PoseV2LineOutcome> g_poseV2LineOutcomes;
    namespace
    {
    std::uint32_t g_poseV2LinePass = 0; // moves on every tick out of normal play (a new pass follows)
    }
    std::uint64_t g_poseV2EntitySoundsBySpeech = 0; // rows left to the replayed line
    std::uint64_t g_poseV2EntitySoundsSpeechFallback = 0; // rows played because the line was refused
    // SoundGenericMode=packets: the rows (ESND), requests (SNDS), collisions
    // (CSND) left to the generic player's packets, and the animation audio
    // events not asked (the engine would make their sounds outside the
    // replay's call, which the ports refuse; the packets carry them).
    std::uint64_t g_poseV2EntitySoundsToGeneric = 0;
    std::uint64_t g_poseV2SoundsToGeneric = 0;
    std::uint64_t g_poseV2CollisionToGeneric = 0;
    std::uint64_t g_poseV2AnimToGeneric = 0;
    namespace
    {
    constexpr std::uint32_t kPoseV2SpeechRowToleranceMs = 100; // the row starts at the line's ms
    constexpr std::uint32_t kPoseV2SpeechRowWaitMs = 250;
    }
    void NotePoseV2LineOutcome(std::uint32_t owner, std::uint32_t eventMs, bool played)
    {
        for (PoseV2LineOutcome& known : g_poseV2LineOutcomes)
        {
            if (known.owner == owner && known.eventMs == eventMs)
            {
                known.played = played; // this pass's answer
                known.pass = g_poseV2LinePass;
                return;
            }
        }
        if (g_poseV2LineOutcomes.size() > 4096)
            g_poseV2LineOutcomes.erase(g_poseV2LineOutcomes.begin(), g_poseV2LineOutcomes.begin() + 1024);
        g_poseV2LineOutcomes.push_back({owner, eventMs, g_poseV2LinePass, played});
    }
    namespace
    {
    // Whether the clip has a line of the row's owner at the row's start,
    // and what this pass did with it.
    bool PoseV2LineAtRow(const replay::EntitySound& sound, replay::puppetaudio::LineState& state)
    {
        state = replay::puppetaudio::LineState::Unknown;
        static std::vector<std::size_t> lines;
        const std::uint32_t from = sound.startMs > kPoseV2SpeechRowToleranceMs ? sound.startMs - kPoseV2SpeechRowToleranceMs : 0;
        Ctx().clip.speechLines.Range(from, sound.startMs + kPoseV2SpeechRowToleranceMs, lines);
        bool found = false;
        for (const std::size_t index : lines)
        {
            const replay::entity::SpeechEvent& line = Ctx().clip.speechLines.Events()[index];
            if (line.speaker != sound.entity)
                continue;
            found = true;
            for (const PoseV2LineOutcome& known : g_poseV2LineOutcomes)
            {
                if (known.owner == sound.entity.Packed() && known.eventMs == line.timeMs && known.pass == g_poseV2LinePass)
                {
                    if (known.played)
                        state = replay::puppetaudio::LineState::Played;
                    else if (state == replay::puppetaudio::LineState::Unknown)
                        state = replay::puppetaudio::LineState::Refused;
                }
            }
        }
        return found;
    }
    replay::esndtally::FamilyKey PoseV2EsndKey(const replay::EntitySound& sound)
    {
        return replay::esndtally::KeyOf(sound.entity.Packed(), sound.caller, sound.soundRef);
    }
    void TallyPoseV2EntitySound(std::size_t index, replay::esndtally::Outcome outcome, const std::string& status = {})
    {
        const auto& sounds = Ctx().clip.entitySounds;
        if (index >= sounds.size() || index >= g_poseV2EsndPassCounted.size() || g_poseV2EsndPassCounted[index])
            return;
        g_poseV2EsndPassCounted[index] = 1;
        g_poseV2EsndTally.Note(PoseV2EsndKey(sounds[index]), outcome, sounds[index].startMs,
            Ctx().transport.Rate() < 0.99, status);
    }

    // A puppet's animation, weapon and body code make many of the ped audio
    // sounds by themselves in playback (most foley refs), others never (the
    // shell after a shot, some foley). What a puppet made itself is watched
    // through the same create detours (audio::SetObserveEntityCreates) and a
    // recorded ped audio sound is started kPoseV2PedAudioHoldMs after its
    // time only when its puppet made no create of the same ref from
    // kPoseV2PedAudioBeforeMs before it until then; each create of the puppet
    // answers for one recorded sound. A long sound restarted mid-span (after
    // a scrub or a pause) is left to the puppet when the puppet made that ref
    // at any time of this playback.
    constexpr std::uint32_t kPoseV2PedAudioHoldMs = 120;
    constexpr std::uint32_t kPoseV2PedAudioBeforeMs = 300;
    constexpr std::uint32_t kPoseV2PedAudioKeepMs = 3000;
    struct PoseV2PuppetCreate
    {
        replay::entity::ReplayId id;
        std::uint64_t soundRef = 0;
        std::uint32_t timeMs = 0;
        bool used = false;
    };
    std::vector<PoseV2PuppetCreate> g_poseV2PuppetCreates;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> g_poseV2PuppetCreatedRefs; // (packed id, ref), this playback
    std::uint64_t g_poseV2PuppetCreateCursor = 0;
    }
    std::uint64_t g_poseV2PedAudioPlayed = 0;
    std::uint64_t g_poseV2PedAudioByPuppet = 0; // left to the puppet's own create
    std::uint64_t g_poseV2PuppetCreatesSeen = 0;

    void ResetPoseV2PuppetCreates()
    {
        std::vector<replay::audio::EntitySoundCreate> stale;
        replay::audio::CollectEntitySoundsSince(g_poseV2PuppetCreateCursor, stale);
        g_poseV2PuppetCreates.clear();
        g_poseV2PuppetCreatedRefs.clear();
        g_poseV2PedAudioPlayed = 0;
        g_poseV2PedAudioByPuppet = 0;
        g_poseV2PuppetCreatesSeen = 0;
    }
    namespace
    {

    void CollectPoseV2PuppetCreates(std::uint32_t timeMs)
    {
        static std::vector<replay::audio::EntitySoundCreate> creates;
        replay::audio::CollectEntitySoundsSince(g_poseV2PuppetCreateCursor, creates);
        g_poseV2PuppetCreates.erase(std::remove_if(g_poseV2PuppetCreates.begin(), g_poseV2PuppetCreates.end(),
            [&](const PoseV2PuppetCreate& create) {
                return create.timeMs > timeMs || timeMs - create.timeMs > kPoseV2PedAudioKeepMs; }),
            g_poseV2PuppetCreates.end());
        if (creates.empty())
            return;
        struct Holder { replay::entity::ReplayId id; std::uint64_t holder; std::uint32_t span; };
        static std::vector<Holder> holders;
        holders.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            // A train car puppet's own audio entity too (its sounds are not
            // played again when the puppet makes them).
            if (puppet.packet.kind == replay::entity::EntityKind::TrainCar && puppet.clone && replay::entities::Exists(puppet.clone))
            {
                if (const std::uint64_t audio = replay::trains::AudioEntityOf(replay::entities::AddressOf(puppet.clone)))
                    holders.push_back({puppet.packet.id, audio, replay::kEntitySoundVehicleAudioSpan});
                continue;
            }
            if ((puppet.packet.kind != replay::entity::EntityKind::Ped && puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            if (const std::uint64_t holder = replay::audio::PedAudioHolder(replay::entities::AddressOf(puppet.clone)))
                holders.push_back({puppet.packet.id, holder, replay::kEntitySoundPedAudioSpan});
        }
        for (const replay::audio::EntitySoundCreate& create : creates)
        {
            const auto holder = std::find_if(holders.begin(), holders.end(), [&](const Holder& entry) {
                return create.audio >= entry.holder && create.audio - entry.holder < entry.span; });
            if (holder == holders.end())
                continue;
            ++g_poseV2PuppetCreatesSeen;
            g_poseV2PuppetCreates.push_back({holder->id, create.soundRef, timeMs, false});
            const std::pair<std::uint32_t, std::uint64_t> key{holder->id.Packed(), create.soundRef};
            if (std::find(g_poseV2PuppetCreatedRefs.begin(), g_poseV2PuppetCreatedRefs.end(), key) ==
                g_poseV2PuppetCreatedRefs.end())
                g_poseV2PuppetCreatedRefs.push_back(key);
        }
    }

    // True when the puppet made this sound itself (the create is used up).
    bool PoseV2PuppetMadeSound(const replay::EntitySound& sound, bool midSpan, std::uint32_t timeMs)
    {
        if (midSpan)
            return std::find(g_poseV2PuppetCreatedRefs.begin(), g_poseV2PuppetCreatedRefs.end(),
                       std::make_pair(sound.entity.Packed(), sound.soundRef)) != g_poseV2PuppetCreatedRefs.end();
        const std::uint32_t from = sound.startMs > kPoseV2PedAudioBeforeMs ? sound.startMs - kPoseV2PedAudioBeforeMs : 0;
        for (PoseV2PuppetCreate& create : g_poseV2PuppetCreates)
        {
            if (create.used || create.id != sound.entity || create.soundRef != sound.soundRef || create.timeMs < from ||
                create.timeMs > timeMs)
                continue;
            create.used = true;
            return true;
        }
        return false;
    }
    }

    // Recorded params with a position (placed sounds such as the shell)
    // keep it; the others follow the puppet.
    // A positioned ped audio loop follows the puppet too
    // (puppet_audio_rule.h PedAudioFollowsPuppet).
    namespace
    {
    bool PoseV2EntitySoundFollowsPuppet(const replay::EntitySound& sound)
    {
        if (!sound.pedAudio || sound.params.size() < 12)
            return true;
        float position[3] = {};
        std::memcpy(position, sound.params.data(), sizeof(position));
        const bool positioned = position[0] != 0.0f || position[1] != 0.0f || position[2] != 0.0f;
        const bool loop = !sound.ended || sound.endMs - sound.startMs >= kPoseV2EntitySoundLoopMs;
        return replay::puppetaudio::PedAudioFollowsPuppet(sound.pedAudio, positioned, loop, true);
    }

    void StopPoseV2EntitySounds()
    {
        for (std::uint64_t& slot : g_poseV2EntitySoundSlot)
            replay::audio::StopPersistentSound(&slot);
        g_poseV2EntitySoundWaiting.clear();
        for (bool& held : g_poseV2EntitySoundSlotHeld)
            held = false;
    }

    float PoseV2EntitySoundDistance(const Vector3& at, const Vector3& camera)
    {
        const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    }

    void PlayPoseV2EntitySounds(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.entity_sounds");
        const auto& sounds = Ctx().clip.entitySounds;
        const bool normalPlay = Ctx().hooks->NormalPlay();
        const std::uint32_t lastMs = g_poseV2EntitySoundLastMs;
        const bool step = normalPlay && timeMs >= lastMs && timeMs - lastMs <= replay::entity::ForwardEventCursor::kMaxStepMs;
        g_poseV2EntitySoundLastMs = timeMs;
        replay::audio::SetObserveEntityCreates(true);
        CollectPoseV2PuppetCreates(timeMs);
        if (!step)
        {
            StopPoseV2EntitySounds();
            g_poseV2EsndNewPass = true;
            ++g_poseV2LinePass;
            return;
        }
        if (g_poseV2EsndNewPass || g_poseV2EsndPassCounted.size() != sounds.size())
        {
            g_poseV2EsndPassCounted.assign(sounds.size(), std::uint8_t{0});
            g_poseV2EsndPassFromMs = lastMs;
            g_poseV2EsndNewPass = false;
            // The lines of earlier passes are not this one's: the pass number
            // moved while out of normal play, before the speech track asked
            // this tick's lines.
            g_poseV2LineOutcomes.erase(std::remove_if(g_poseV2LineOutcomes.begin(), g_poseV2LineOutcomes.end(),
                [](const PoseV2LineOutcome& known) { return known.pass != g_poseV2LinePass; }),
                g_poseV2LineOutcomes.end());
        }
        const auto puppetOf = [&](const replay::EntitySound& sound) -> const replay::playback::Puppet* {
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(sound.entity);
            return puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                    replay::entities::Exists(puppet->clone)
                ? puppet
                : nullptr;
        };
        // A ped audio sound starts kPoseV2PedAudioHoldMs late (the time to see
        // whether its puppet makes it itself); a short one (under
        // kPoseV2EntitySoundLoopMs) is left to the engine once playing, like a
        // one-shot. A train car's audio sound (vehicleAudio) follows the ped
        // audio rules: its puppet may make it itself.
        const auto ownAudio = [](const replay::EntitySound& sound) { return sound.pedAudio || sound.vehicleAudio; };
        // No hold for ped audio while the puppets' own are refused
        // (puppet_audio_rule.h PedAudioHoldMs).
        const bool ownPersistentRefused = PoseV2OwnPersistentRefused();
        const auto holdOf = [&](const replay::EntitySound& sound) -> std::uint32_t {
            return replay::puppetaudio::PedAudioHoldMs(sound.pedAudio, ownAudio(sound), ownPersistentRefused,
                kPoseV2PedAudioHoldMs); };
        const auto shortOf = [&](const replay::EntitySound& sound) {
            return sound.oneShot ||
                (ownAudio(sound) && sound.ended && sound.endMs - sound.startMs < kPoseV2EntitySoundLoopMs); };
        // Stop and drive the playing ones.
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD(); // the listener
        // A slot the engine cleared since last frame: its sound ended by
        // itself and the slot is free again.
        for (std::size_t slot = 0; slot < kPoseV2EntitySoundSlots; ++slot)
        {
            if (g_poseV2EntitySoundSlotHeld[slot] && !g_poseV2EntitySoundSlot[slot])
            {
                ++g_poseV2EntitySoundsEnded;
                if (g_poseV2EntitySoundSlotEvent[slot] < sounds.size())
                    g_poseV2EsndTally.EngineEnded(PoseV2EsndKey(sounds[g_poseV2EntitySoundSlotEvent[slot]]));
            }
            g_poseV2EntitySoundSlotHeld[slot] = false;
        }
        for (std::size_t slot = 0; slot < kPoseV2EntitySoundSlots; ++slot)
        {
            if (!g_poseV2EntitySoundSlot[slot])
                continue;
            const replay::EntitySound& sound = sounds[g_poseV2EntitySoundSlotEvent[slot]];
            const replay::playback::Puppet* puppet = puppetOf(sound);
            // A one-shot is never cut at the end of its start window.
            if (!puppet || timeMs < sound.startMs || (sound.ended && !shortOf(sound) && timeMs >= sound.endMs + holdOf(sound)))
            {
                replay::audio::StopPersistentSound(&g_poseV2EntitySoundSlot[slot]);
                continue;
            }
            // A short own-audio sound lives its recorded length.
            if (replay::puppetaudio::StopAtRecordedLength(true, ownAudio(sound), sound.oneShot,
                    sound.ended, sound.startMs, sound.endMs, kPoseV2EntitySoundLoopMs,
                    g_poseV2EntitySoundSlotStartMs[slot], timeMs))
            {
                replay::audio::StopPersistentSound(&g_poseV2EntitySoundSlot[slot]);
                ++g_poseV2PedAudioLengthStops;
                g_poseV2EsndTally.LengthStop(PoseV2EsndKey(sound));
                continue;
            }
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, TRUE);
            g_poseV2EntitySoundSlotDistance[slot] = PoseV2EntitySoundDistance(at, camera);
            if (!PoseV2EntitySoundFollowsPuppet(sound))
                continue;
            const float position[4] = {at.x, at.y, at.z, 0.0f};
            replay::audio::SetSoundPosition(g_poseV2EntitySoundSlot[slot], position);
        }
        // Start the ones due.
        if (g_poseV2EntitySoundWaiting.size() != sounds.size())
            g_poseV2EntitySoundWaiting.assign(sounds.size(), 0);
        if (g_poseV2EntitySoundOutcome.size() != sounds.size())
            g_poseV2EntitySoundOutcome.assign(sounds.size(), kEsndOutcomeNone);
        for (std::size_t index = 0; index < sounds.size(); ++index)
        {
            const replay::EntitySound& sound = sounds[index];
            // A recorded packet carries the row; the generic player plays it
            // with its adjusts (never both).
            if (replay::soundgeneric::EntitySoundHandedOff(index))
            {
                if (timeMs >= sound.startMs && g_poseV2EntitySoundWaiting[index] != kPoseV2EntitySoundDone)
                {
                    g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundDone;
                    ++g_poseV2EntitySoundsToGeneric;
                    SetPoseV2EntitySoundOutcome(index, kEsndOutcomeToGeneric);
                    if (sound.startMs > g_poseV2EsndPassFromMs)
                        TallyPoseV2EntitySound(index, replay::esndtally::Outcome::ToGeneric);
                }
                continue;
            }
            const std::uint32_t beginMs = sound.startMs + holdOf(sound);
            // A short ped audio sound keeps a one-shot's start window.
            const std::uint32_t windowEndMs = !ownAudio(sound) ? sound.endMs
                : shortOf(sound) ? (std::max)(sound.endMs + holdOf(sound), beginMs + kPoseV2OneShotStartWindowMs)
                : sound.endMs + holdOf(sound);
            if (timeMs < beginMs || (sound.ended && timeMs >= windowEndMs))
            {
                // A row whose span went by on this pass without a start (not
                // refused, not the puppet's) is "passed".
                if (timeMs >= beginMs && index < g_poseV2EntitySoundOutcome.size() &&
                    g_poseV2EntitySoundOutcome[index] == kEsndOutcomeNone)
                    g_poseV2EntitySoundOutcome[index] = kEsndOutcomePassed;
                // A row this pass reached (its start after the pass began) and
                // went by without a start or the puppet's own.
                if (timeMs >= beginMs && beginMs > g_poseV2EsndPassFromMs)
                    TallyPoseV2EntitySound(index, replay::esndtally::Outcome::Passed);
                g_poseV2EntitySoundWaiting[index] = 0; // its span is over: the chance is gone
                continue;
            }
            // A sound at 0 is crossed by the first step off 0.
            const bool crossed = lastMs < beginMs || (beginMs == 0 && lastMs == 0 && timeMs > 0);
            const bool loop = !sound.ended || sound.endMs - sound.startMs >= kPoseV2EntitySoundLoopMs;
            const std::uint8_t state = g_poseV2EntitySoundWaiting[index];
            if (state == kPoseV2EntitySoundDone)
                continue; // once per pass (started, left to the puppet or refused)
            const bool waiting = state == kPoseV2EntitySoundWaitingState;
            const bool refusedOnce = state == kPoseV2EntitySoundRefusedOnce;
            if (!crossed && !loop && !waiting)
                continue;
            if (!loop)
                g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundWaitingState; // until it starts (below) or its span ends
            bool playing = false;
            std::size_t free = kPoseV2EntitySoundSlots;
            for (std::size_t slot = 0; slot < kPoseV2EntitySoundSlots; ++slot)
            {
                if (g_poseV2EntitySoundSlot[slot] && g_poseV2EntitySoundSlotEvent[slot] == index)
                    playing = true;
                else if (!g_poseV2EntitySoundSlot[slot] && free == kPoseV2EntitySoundSlots)
                    free = slot;
            }
            if (playing)
            {
                g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundDone;
                SetPoseV2EntitySoundOutcome(index, kEsndOutcomePlayed);
                TallyPoseV2EntitySound(index, replay::esndtally::Outcome::Played);
                continue;
            }
            // A line's own sound (the ped's sound controller) is left to the
            // replayed line, or played when the line was refused.
            if (replay::puppetaudio::SpeechControllerCaller(sound.caller, replay::audio::SpeechControllerCallers()) &&
                (crossed || waiting || loop))
            {
                replay::puppetaudio::LineState lineState = replay::puppetaudio::LineState::Unknown;
                const bool lineAtRow = PoseV2LineAtRow(sound, lineState);
                const replay::puppetaudio::SpeechRow rule = replay::puppetaudio::SpeechRowRule(true, lineAtRow, lineState,
                    timeMs > sound.startMs ? timeMs - sound.startMs : 0, kPoseV2SpeechRowWaitMs);
                if (rule == replay::puppetaudio::SpeechRow::Wait)
                {
                    g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundWaitingState;
                    continue;
                }
                if (rule == replay::puppetaudio::SpeechRow::LeaveToLine)
                {
                    ++g_poseV2EntitySoundsBySpeech;
                    g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundDone;
                    SetPoseV2EntitySoundOutcome(index, kEsndOutcomeByPuppet);
                    TallyPoseV2EntitySound(index, replay::esndtally::Outcome::ByPuppet);
                    continue;
                }
                if (rule == replay::puppetaudio::SpeechRow::Play && lineAtRow)
                    ++g_poseV2EntitySoundsSpeechFallback;
            }
            // Made by the puppet itself: not played again.
            if (ownAudio(sound) && (crossed || waiting || loop) &&
                PoseV2PuppetMadeSound(sound, !crossed && !waiting && loop, timeMs))
            {
                if (crossed || waiting)
                    ++g_poseV2PedAudioByPuppet;
                g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundDone;
                SetPoseV2EntitySoundOutcome(index, kEsndOutcomeByPuppet);
                TallyPoseV2EntitySound(index, replay::esndtally::Outcome::ByPuppet);
                continue;
            }
            const replay::playback::Puppet* puppet = puppetOf(sound);
            if (!puppet)
                continue; // the puppet may come a few frames late: tried again next frame (waiting)
            std::string status = "no_slot";
            bool ok = false;
            // The slot policy (free, owner cap, nearer replaces farther or
            // older, too far not started).
            const Vector3 puppetAt = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, TRUE);
            const float distance = PoseV2EntitySoundDistance(puppetAt, camera);
            {
                replay::soundslots::SlotView views[kPoseV2EntitySoundSlots];
                for (std::size_t slot = 0; slot < kPoseV2EntitySoundSlots; ++slot)
                {
                    views[slot].used = g_poseV2EntitySoundSlot[slot] != 0;
                    views[slot].owner = g_poseV2EntitySoundSlotOwner[slot];
                    views[slot].distance = g_poseV2EntitySoundSlotDistance[slot];
                    views[slot].startedMs = g_poseV2EntitySoundSlotStartMs[slot];
                    views[slot].longSound = g_poseV2EntitySoundSlotLong[slot];
                }
                const replay::soundslots::Choice choice =
                    replay::soundslots::Choose(views, kPoseV2EntitySoundSlots, sound.entity.Packed(), distance);
                free = choice.slot < 0 ? kPoseV2EntitySoundSlots : static_cast<std::size_t>(choice.slot);
                if (choice.reason == replay::soundslots::Reason::TooFar)
                    status = "too_far";
                if (choice.Steals())
                {
                    replay::audio::StopPersistentSound(&g_poseV2EntitySoundSlot[free]);
                    ++g_poseV2EntitySoundsStolen;
                }
            }
            if (free != kPoseV2EntitySoundSlots)
            {
                const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
                // The offsets are checked again against this image before they
                // become a pointer (StartEntitySound then wants an object with
                // a vtable in the image there).
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    module + reinterpret_cast<const IMAGE_DOS_HEADER*>(module)->e_lfanew);
                // A ped audio sound's offset is from the puppet's holder; a
                // train car sound's offset is from the puppet car's audio
                // entity (none on an object puppet: refused, logged).
                const std::uint64_t owner = sound.pedAudio
                    ? replay::audio::PedAudioHolder(replay::entities::AddressOf(puppet->clone))
                    : sound.vehicleAudio ? replay::trains::AudioEntityOf(replay::entities::AddressOf(puppet->clone))
                    : sound.audioOnEntity ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone))
                    : module;
                std::string imageStatus;
                if (!replay::EntitySoundAudioValid(sound, nt->OptionalHeader.SizeOfImage))
                    status = "bad_audio_entity";
                else if (!sound.audioOnEntity && !replay::audio::TrustedImageAudio(module + sound.audioRva, imageStatus))
                    status = imageStatus; // an image audio the recorder does not write (or not that object now)
                else if (!owner && sound.pedAudio)
                    continue; // no ped audio yet (the puppet's audio component comes up later): next frame
                else if (!owner && sound.vehicleAudio)
                    status = "no_train_audio";
                else if (!owner)
                    status = "bad_audio_entity";
                else
                    ok = replay::audio::StartEntitySound(owner + (sound.audioOnEntity ? sound.audioOffset : sound.audioRva),
                        sound.soundRef, sound.byHash, sound.params.empty() ? nullptr : sound.params.data(),
                        &g_poseV2EntitySoundSlot[free], status);
                if (ok)
                {
                    if (waiting && !crossed)
                        ++g_poseV2EntitySoundsLate;
                    if (sound.pedAudio)
                        ++g_poseV2PedAudioPlayed;
                    g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundDone;
                    g_poseV2EntitySoundSlotEvent[free] = index;
                    g_poseV2EntitySoundSlotOwner[free] = sound.entity.Packed();
                    g_poseV2EntitySoundSlotStartMs[free] = timeMs;
                    g_poseV2EntitySoundSlotLong[free] = !sound.ended || sound.endMs - sound.startMs >= replay::soundslots::kLongSoundMs;
                    g_poseV2EntitySoundSlotDistance[free] = distance;
                    if (PoseV2EntitySoundFollowsPuppet(sound))
                    {
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, TRUE);
                        const float position[4] = {at.x, at.y, at.z, 0.0f};
                        replay::audio::SetSoundPosition(g_poseV2EntitySoundSlot[free], position);
                    }
                }
                else
                    g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundDone; // refused by the engine: one try
            }
            else if ((waiting && !crossed) || refusedOnce)
                continue; // still no slot: counted and logged once, when it was crossed
            else if (loop)
                g_poseV2EntitySoundWaiting[index] = kPoseV2EntitySoundRefusedOnce; // tried again, not counted again
            if (status == "too_far")
                ++g_poseV2EntitySoundsTooFar; // not heard there, not a refusal
            else
                ++(ok ? g_poseV2EntitySoundsPlayed : g_poseV2EntitySoundsRefused);
            g_poseV2EntitySoundLast = status;
            // The row's outcome for the QA mark's window.
            SetPoseV2EntitySoundOutcome(index, ok ? kEsndOutcomePlayed : status == "too_far" ? kEsndOutcomeTooFar : kEsndOutcomeRefused);
            TallyPoseV2EntitySound(index, ok ? replay::esndtally::Outcome::Played
                    : status == "too_far" ? replay::esndtally::Outcome::TooFar : replay::esndtally::Outcome::Refused,
                status);
            if (!ok && status != "too_far")
                g_poseV2EntitySoundLastRefusal[sound.entity.Packed()] = status;
            const bool imageRefused =
                status.rfind("unknown_image_audio", 0) == 0 || status.rfind("image_audio_vtable_mismatch", 0) == 0;
            if (g_poseV2EntitySoundPlayLogs < 40 || imageRefused)
            {
                ++g_poseV2EntitySoundPlayLogs;
                Ctx().log << "POSE_V2_ENTITY_SOUND,phase=play,id=" << replay::entity::IdText(sound.entity) << std::hex
                    << ",ref=0x" << sound.soundRef << std::dec << ",ped_audio=" << sound.pedAudio
                    << ",train_audio=" << sound.vehicleAudio << ",status=" << status
                    << ",crossed=" << crossed << ",event_ms=" << sound.startMs << ",playhead_ms=" << timeMs << '\n';
            }
        }
        for (std::size_t slot = 0; slot < kPoseV2EntitySoundSlots; ++slot)
            g_poseV2EntitySoundSlotHeld[slot] = g_poseV2EntitySoundSlot[slot] != 0;
    }

    void StopPoseV2CollisionSounds()
    {
        StopPoseV2EntitySounds();
        for (std::uint64_t& slot : g_poseV2CollisionSlot)
            replay::audio::StopPersistentSound(&slot);
        for (std::uint64_t& slot : g_poseV2ImpactSlot)
            replay::audio::StopPersistentSound(&slot);
    }
    namespace
    {

    bool PlayPoseV2GroundImpact(const replay::entity::SoundEvent& sound, std::string& status)
    {
        if (sound.params.size() < 12)
        {
            status = "no_params";
            return false;
        }
        // The static audio entity comes from the clip: one the recorder
        // writes, still with its vtable, or the impact is refused.
        if (sound.globalAudio && !replay::audio::TrustedImageAudio(sound.globalAudio, status))
            return false;
        std::uint64_t* slot = &g_poseV2ImpactSlot[g_poseV2ImpactNext];
        g_poseV2ImpactNext = (g_poseV2ImpactNext + 1) % kPoseV2ImpactSlots;
        if (!replay::audio::StartPersistentSound(sound.globalAudio, sound.soundRef, sound.params.data(), slot, status))
            return false;
        float position[4] = {};
        std::memcpy(position, sound.params.data(), 12);
        if (!replay::audio::SetSoundPosition(*slot, position))
            status = "playing_no_position";
        return true;
    }
    }

    replay::entity::ForwardEventCursor g_poseV2SoundCursorPlay;
    std::uint64_t g_poseV2SoundsPlayed = 0;
    std::uint64_t g_poseV2SoundsRefused = 0;
    std::uint64_t g_poseV2SoundsNeighbor = 0; // another ped's sound in an old clip, not played
    std::string g_poseV2SoundLast;
    // Playback side.
    replay::entity::ForwardEventCursor g_poseV2FootstepCursorPlay;
    replay::entity::ForwardEventCursor g_poseV2ExplosionCursorPlay;
    // Animation audio playback (PlayPoseV2AnimAudio).
    replay::entity::ForwardEventCursor g_poseV2AnimCursorPlay;
    std::uint64_t g_poseV2AnimPlayed = 0;
    std::uint64_t g_poseV2AnimRefused = 0;
    std::uint64_t g_poseV2AnimNoPuppet = 0;
    std::uint64_t g_poseV2AnimRetried = 0;
    std::uint64_t g_poseV2AnimConsumed = 0;
    std::uint64_t g_poseV2AnimNotConsumed = 0;
    std::uint64_t g_poseV2AnimPlayLines = 0;
    std::map<std::string, std::uint64_t> g_poseV2AnimRefusals;
    std::vector<std::size_t> g_poseV2AnimCarried;
    std::vector<std::uint64_t> g_poseV2AnimQueuedAudio;
    std::uint64_t g_poseV2FootstepEngineCursor = 0;
    std::uint64_t g_poseV2FootstepsPlayed = 0;
    std::uint64_t g_poseV2FootstepsRefused = 0;
    std::uint64_t g_poseV2FootstepsPuppetEngine = 0;
    // The record's foot wetness (steps paired with one, and above 0) and the
    // playback's outcomes per entity (horse_sound_report.h), for the log and
    // the sound report.
    std::uint64_t g_poseV2FootWetnessRecorded = 0;
    std::uint64_t g_poseV2FootWetRecorded = 0;
    // Steps recorded with ground layers (and with a layer above 0) and with
    // the holder's audio LOD.
    std::uint64_t g_poseV2FootLayersRecorded = 0;
    std::uint64_t g_poseV2FootLayersNonZero = 0;
    std::uint64_t g_poseV2FootLodRecorded = 0;
    replay::horsesound::Tallies g_poseV2SoundTallies;
    namespace
    {

    // The tallies of one entity (horse_sound_report.h).
    replay::horsesound::FootTally& PoseV2FootTally(const replay::entity::ReplayId& id)
    {
        auto& tally = g_poseV2SoundTallies.feet[replay::entity::IdText(id)];
        if (!tally.recorded)
        {
            const replay::entity::CreatePacket* packet = Ctx().clip.FindCreate(id.Packed());
            tally.horse = packet && packet->kind == replay::entity::EntityKind::Horse;
        }
        return tally;
    }
    }

    replay::horsesound::SpeechTally& PoseV2SpeechTally(const replay::entity::ReplayId& id, bool animal)
    {
        auto& tally = g_poseV2SoundTallies.speech[replay::entity::IdText(id)];
        const replay::entity::CreatePacket* packet = Ctx().clip.FindCreate(id.Packed());
        if (packet && packet->kind == replay::entity::EntityKind::Horse)
            tally.cls = replay::horsesound::SpeakerClass::Horse;
        else if (animal)
            tally.cls = replay::horsesound::SpeakerClass::Animal;
        return tally;
    }

    std::string g_poseV2FootstepLastError;
    // Refused footsteps per entity and reason (evidence).
    std::map<std::string, std::uint32_t> g_poseV2FootstepRefusals;
    // Evidence: the audio state of each ped puppet, logged once per puppet
    // and reason (ready for 1 s, first refused footstep, first refused
    // sound).
    std::map<std::string, std::uint32_t> g_poseV2AudioStateLogged;
    std::map<std::string, std::uint32_t> g_poseV2FootForced; // force writes per puppet and status
    // The puppets with recorded steps get the audio LOD override 0
    // (audio::SetPuppetAudioLodOverride). Sorted packed ids.
    std::vector<std::uint32_t> g_poseV2FootLodIds;
    // The override set to the LOD the recorded steps had (FOOT v6) instead of
    // 0 (foot_material.h PuppetAudioLodAt; ini PuppetAudioLodRecorded,
    // default 1); an owner without recorded LODs (an older clip) keeps LOD 0.
    // Per owner, the steps' (time, LOD).
    bool g_poseV2PuppetAudioLodRecorded = true;
    std::unordered_map<std::uint32_t, std::vector<replay::footmaterial::LodPoint>> g_poseV2FootLodPoints;
    std::uint64_t g_poseV2FootLodRecordedWrites = 0;
    std::uint64_t g_poseV2FootLodChosen[4] = {}; // ticks per chosen LOD (0..3) with a recorded LOD
    std::map<std::string, std::uint32_t> g_poseV2FootLodLogged; // per puppet and status
    std::uint64_t g_poseV2FootLodWrites = 0;

    // (packed id, reason) already logged, tested before any string is built
    // ("ready1s" is asked for every ready ped every tick).
    std::unordered_set<std::uint64_t> g_poseV2AudioStateSeen;
    namespace
    {

    void LogPuppetAudioState(const replay::playback::Puppet& puppet, const char* reason, std::uint32_t timeMs)
    {
        if (!replay::log::Verbose()) // evidence, Debug=1 only
            return;
        const std::uint64_t seenKey = (static_cast<std::uint64_t>(puppet.packet.id.Packed()) << 32) ^
            static_cast<std::uint64_t>(std::hash<std::string_view>{}(reason) & 0xFFFFFFFFu);
        if (!g_poseV2AudioStateSeen.insert(seenKey).second)
            return;
        if (g_poseV2AudioStateLogged[replay::entity::IdText(puppet.packet.id) + reason]++ != 0)
            return;
        Ctx().log << "POSE_V2_PED_AUDIO,id=" << replay::entity::IdText(puppet.packet.id)
                  << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec
                  << ",reason=" << reason << ",playhead_ms=" << timeMs << ','
                  << replay::audio::PedAudioState(replay::entities::AddressOf(puppet.clone)) << '\n';
    }
    }
    std::vector<PoseV2AudioWatch> g_poseV2AudioWatches;

    void WatchPoseV2AudioEvents()
    {
        if (!g_poseV2AudioObserverReady)
            return;
        const Ped player = PLAYER::PLAYER_PED_ID();
        const bool playerMelee = PED::IS_PED_IN_MELEE_COMBAT(player) != FALSE;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            auto watch = std::find_if(g_poseV2AudioWatches.begin(), g_poseV2AudioWatches.end(),
                [&](const PoseV2AudioWatch& candidate) { return candidate.ped == entity.handle; });
            if (watch == g_poseV2AudioWatches.end())
            {
                PoseV2AudioWatch fresh;
                fresh.ped = entity.handle;
                fresh.health = ENTITY::GET_ENTITY_HEALTH(entity.handle);
                fresh.speaking = AUDIO::IS_ANY_SPEECH_PLAYING(entity.handle) != FALSE;
                fresh.id = entity.id;
                BYTE* address = replay::entities::AddressOf(entity.handle);
                fresh.address = reinterpret_cast<std::uint64_t>(address);
                fresh.audio = replay::audio::PedAudio(address);
                {
                    // Both speech entity paths, where both exist, must agree.
                    const std::uint64_t speech = replay::audio::SpeechEntity(address);
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_SPEECH_ENTITY,ped=" << replay::entity::IdText(entity.id)
                        << ",native_path=0x" << std::hex << speech
                        << ",audio_path=0x" << (fresh.audio ? fresh.audio + 0x48C0 : 0) << std::dec
                        << ",match=" << (speech && fresh.audio && speech == fresh.audio + 0x48C0 ? 1 : 0)
                        << '\n';
                }
                g_poseV2AudioWatches.push_back(fresh);
                continue;
            }
            const int health = ENTITY::GET_ENTITY_HEALTH(entity.handle);
            if (watch->health >= 0 && health < watch->health)
            {
                char label[96]{};
                std::snprintf(label, sizeof(label), "hit:%s:-%d:player_melee=%d",
                    replay::entity::IdText(entity.id).c_str(), watch->health - health, playerMelee ? 1 : 0);
                OpenPoseV2AudioWindow(label, 150000, 150000);
            }
            watch->health = health;
            const bool speaking = AUDIO::IS_ANY_SPEECH_PLAYING(entity.handle) != FALSE;
            if (speaking && !watch->speaking)
            {
                char label[96]{};
                std::snprintf(label, sizeof(label), "speech:%s:last=0x%08X",
                    replay::entity::IdText(entity.id).c_str(),
                    static_cast<unsigned>(AUDIO::_GET_LAST_PLAYED_SPEECH_FOR_PED(entity.handle)));
                OpenPoseV2AudioWindow(label, 1500000, 100000);
                // The playing line's data: voice +0x110, context +0x114,
                // variation +0x118.
                replay::audio::SpeechProbe probe;
                std::string error;
                const bool ok = replay::audio::ProbeSpeech(replay::entities::AddressOf(entity.handle), probe, error);
                std::uint32_t voice = 0;
                std::uint32_t context = 0;
                std::int32_t variation = 0;
                if (ok && probe.lineData.size() >= 0x11C)
                {
                    std::memcpy(&voice, probe.lineData.data() + 0x110, 4);
                    std::memcpy(&context, probe.lineData.data() + 0x114, 4);
                    std::memcpy(&variation, probe.lineData.data() + 0x118, 4);
                }
                // Evidence only: lines that start without an accepted Say
                // (pain and effort vocals) are not speech events.
                std::ostream& log = Ctx().log;
                log << "POSE_V2_SPEECH_EDGE,ped=" << replay::entity::IdText(entity.id)
                    << ",status=" << (context ? "line" : (ok ? "no_line" : error.c_str()))
                    << ",voice=0x" << std::hex << voice
                    << ",context=0x" << replay::audio::FinalizeHash(context) << std::dec
                    << ",variation=" << variation
                    << ",time_ms=" << replay::pose::capture::NowMs()
                    << '\n';
                log.flush();
            }
            watch->speaking = speaking;
        }
    }

    // Every melee impact sound the engine played for a recorded ped (its
    // melee audio, holder +0x1B60, is the entry's first argument).
    void RecordPoseV2MeleeHits()
    {
        REPLAY_PROFILE_ZONE("rec.melee");
        static std::vector<replay::audio::MeleeCall> calls;
        replay::audio::CollectMeleeSince(g_poseV2MeleeCursor, calls);
        if (calls.empty())
            return;
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::MeleeCall& call : calls)
        {
            const replay::recorder::RecordedEntity* owner = nullptr;
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (!entity.vehicle && replay::entities::Exists(entity.handle) &&
                    replay::audio::MeleeAudio(replay::entities::AddressOf(entity.handle)) == call.args[0])
                {
                    owner = &entity;
                    break;
                }
            }
            if (!owner)
                continue;
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            replay::entity::MeleeEvent hit;
            hit.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            hit.entity = owner->id;
            hit.info = call.args[1];
            std::memcpy(&hit.infoVtable, call.info, sizeof(hit.infoVtable));
            std::memcpy(&hit.infoHash, call.info + 0x48, sizeof(hit.infoHash));
            Ctx().clip.meleeHits.Add(hit);
        }
    }

    // Every body impact the engine played for a recorded ped (its melee
    // audio is the first argument), with the other ped when recorded.
    void RecordPoseV2BodyImpacts()
    {
        REPLAY_PROFILE_ZONE("rec.body_impacts");
        static std::vector<replay::audio::ImpactCall> calls;
        replay::audio::CollectImpactsSince(g_poseV2BodyImpactCursor, calls);
        if (calls.empty())
            return;
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::ImpactCall& call : calls)
        {
            const replay::recorder::RecordedEntity* owner = nullptr;
            const replay::recorder::RecordedEntity* other = nullptr;
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !replay::entities::Exists(entity.handle))
                    continue;
                void* address = replay::entities::AddressOf(entity.handle);
                if (replay::audio::MeleeAudio(address) == call.args[0])
                    owner = &entity;
                if (reinterpret_cast<std::uint64_t>(address) == call.args[2])
                    other = &entity;
            }
            if (!owner)
                continue;
            // Evidence for the two optional objects (8th, 9th): which
            // recorded entity they are, or their vtable.
            const auto describe = [&](std::uint64_t value) -> std::string {
                if (!value)
                    return "0";
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                {
                    if (replay::entities::Exists(entity.handle) &&
                        reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == value)
                    {
                        return "entity:" + replay::entity::IdText(entity.id);
                    }
                }
                for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                {
                    if (replay::entities::Exists(weapon.object) &&
                        reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)) == value)
                    {
                        return "weapon:" + replay::entity::IdText(weapon.id);
                    }
                }
                std::uint64_t vtable = 0;
                const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
                char text[48]{};
                if (replay::audio::ReadU64(value, vtable) && vtable > module && vtable < module + 0x7000000)
                    std::snprintf(text, sizeof(text), "vtable:0x%llX", static_cast<unsigned long long>(vtable - module));
                else
                    std::snprintf(text, sizeof(text), "data");
                return text;
            };
            if (Ctx().clip.bodyImpacts.Events().size() < 24)
            {
                std::ostream& log = Ctx().log;
                log << "POSE_V2_BODY_IMPACT_ARGS,ped=" << replay::entity::IdText(owner->id)
                    << ",a6=" << describe(call.args[5])
                    << ",a8=" << describe(call.args[7])
                    << ",a9=" << describe(call.args[8])
                    << ",a10_byte=" << (call.args[9] & 0xFF)
                    << ",caller=0x" << std::hex << call.callers[0] << std::dec
                    << '\n';
                log.flush();
            }
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            replay::entity::BodyImpactEvent impact;
            impact.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            impact.entity = owner->id;
            if (other)
                impact.other = other->id;
            std::memcpy(impact.position, call.position, sizeof(impact.position));
            impact.arg4 = call.args[3];
            impact.arg5 = call.args[4];
            impact.definition = call.args[5];
            impact.definitionVtable = call.definitionVtable;
            impact.definitionHash = call.definitionHash;
            impact.arg7 = call.args[6];
            // Only its low byte is read (mov al,[rbp+0x97]): the
            // upper bytes are register leftovers, often a heap address.
            impact.arg10 = call.args[9] & 0xFF;
            Ctx().clip.bodyImpacts.Add(impact);
        }
    }

    // Every deferred sound request whose owner is a recorded ped.
    void RecordPoseV2Sounds()
    {
        REPLAY_PROFILE_ZONE("rec.sounds");
        static std::vector<replay::audio::SoundRequest> requests;
        replay::audio::CollectSoundRequestsSince(g_poseV2SoundCursor, requests);
        if (requests.empty())
            return;
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::SoundRequest& request : requests)
        {
            // A deferred request whose owner is a recorded weapon or prop (a thrown
            // bottle) or nobody recorded.
            if (g_poseV2ResearchWindows)
            {
                const replay::entity::ReplayId ownerId = RecordedIdAtAddress(request.owner);
                const bool ped = std::any_of(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
                    [&](const replay::recorder::RecordedEntity& entity) {
                        return reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == request.owner; });
                static replay::log::LogLimit otherOwnerLogs; // per session
                if (g_poseV2ResearchWindows && !ped && otherOwnerLogs.Take(80))
                {
                    const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
                    const bool staticAudio = request.audioEntity > module && request.audioEntity < module + 0x7000000;
                    const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                    Ctx().log << "POSE_V2_SOUND_DEFERRED_OTHER,owner=" << (ownerId.Valid() ? replay::entity::IdText(ownerId) : std::string("none"))
                        << std::hex << ",owner_ptr=0x" << request.owner << ",ref=0x" << request.soundRef
                        << ",audio=" << (staticAudio ? "static:0x" : "heap:0x") << (staticAudio ? request.audioEntity - module : request.audioEntity)
                        << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1] << std::dec
                        << ",flags=" << request.flags[0] << "/" << request.flags[1] << "/" << request.flags[2] << "/" << request.flags[3]
                        << ",params=" << request.hasParams << ",time_ms=" << replay::pose::capture::RecordMsAgo(ageMs, nowMs) << '\n';
                }
            }
            // A deferred request owned by something that is not a recorded ped (an
            // object in the hand: a molotov's match) goes to the object sounds
            // (TrackPoseV2EntitySounds), which wait for the object to be recorded.
            if (request.owner && !request.inReplayedSystem && request.hasParams &&
                std::none_of(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
                    [&](const replay::recorder::RecordedEntity& entity) {
                        return !entity.vehicle && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == request.owner; }))
            {
                PoseV2ObjectSoundCandidate candidate;
                candidate.timeUs = request.timeUs;
                candidate.audio = request.audioEntity;
                candidate.soundRef = request.soundRef;
                candidate.owner = request.owner;
                std::memcpy(&candidate.tracker, request.params + 0xC8, sizeof(candidate.tracker));
                candidate.hasParams = true;
                std::memcpy(candidate.params, request.params, sizeof(candidate.params));
                candidate.callers[0] = request.callers[0];
                candidate.callers[1] = request.callers[1];
                candidate.path = "deferred";
                QueuePoseV2ObjectSound(candidate);
                continue;
            }
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !replay::entities::Exists(entity.handle))
                    continue;
                void* address = replay::entities::AddressOf(entity.handle);
                if (reinterpret_cast<std::uint64_t>(address) != request.owner)
                    continue;
                const std::uint64_t holder = replay::audio::PedAudioHolder(address);
                if (!holder || request.audioEntity < holder || request.audioEntity - holder >= replay::audio::kPedAudioSpan)
                {
                    // Evidence: the ped owns the request but another audio
                    // entity plays it (not replayed yet).
                    static replay::log::LogLimit logged; // per session
                    if (logged.Take(30))
                    {
                        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
                        const bool staticAudio = request.audioEntity > module && request.audioEntity < module + 0x7000000;
                        std::ostream& log = Ctx().log;
                        log << "POSE_V2_SOUND_OWNER_OTHER_AUDIO,ped=" << replay::entity::IdText(entity.id)
                            << ",ref=0x" << std::hex << request.soundRef
                            << ",audio=" << (staticAudio ? "static:0x" : "heap:0x")
                            << (staticAudio ? request.audioEntity - module : request.audioEntity)
                            << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1] << std::dec
                            << ",params=" << (request.hasParams ? 1 : 0) << '\n';
                        log.flush();
                    }
                    break;
                }
                const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                replay::entity::SoundEvent sound;
                sound.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
                sound.entity = entity.id;
                sound.audioOffset = request.audioEntity - holder;
                sound.soundRef = request.soundRef;
                if (request.hasParams)
                    sound.params.assign(request.params, request.params + sizeof(request.params));
                std::memcpy(sound.flags, request.flags, sizeof(sound.flags));
                Ctx().clip.sounds.Add(sound);
                if (Ctx().clip.sounds.Events().size() <= 40)
                {
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_SOUND_EVENT,ped=" << replay::entity::IdText(entity.id)
                        << ",ref=0x" << std::hex << request.soundRef
                        << ",audio_offset=0x" << sound.audioOffset
                        << ",caller=0x" << request.callers[0] << "/0x" << request.callers[1] << std::dec
                        << ",params=" << (request.hasParams ? 1 : 0)
                        << ",ref_state=" << replay::audio::IndirectRefState(request.soundRef)
                        << ",time_ms=" << sound.timeMs
                        << '\n';
                    log.flush();
                }
                break;
            }
        }
    }
    namespace
    {

    // Direct create-and-play requests of recorded peds: on their ped audio,
    // or on a static audio entity (collisions) whose tracker is the ped.
    // Sound-making functions shared by footsteps and other systems (a body
    // fall calls the same foot audio helpers), for evidence logs only: the
    // step sound, its layer, its create and three more helpers; and the
    // collision impact code (two functions).
    namespace
    {
        const sig::Signature kImpactCodeA{"events_impact_code_a",
            "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 A8 FE FF FF 48 81 EC 30 "
            "02 00 00 0F 29 70 C8", 0x2F07D88};
        const sig::Signature kImpactCodeB{"events_impact_code_b",
            "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FD FF FF 48 81 EC 58 03 00 00 0F 29 70 A8",
            0x2F083F0};

        // The RVA a signature was found at (0: not found).
        std::uint32_t FoundRva(const sig::Signature& s)
        {
            const std::uintptr_t at = sig::Find(s);
            const std::uintptr_t base = sig::ImageBase();
            return at > base ? static_cast<std::uint32_t>(at - base) : 0;
        }

        // The start of the function an engine return address lies in, by the
        // image's unwind table (0: none).
        std::uint32_t FunctionStartRva(std::uint32_t rva)
        {
            static const std::uintptr_t base = sig::ImageBase();
            if (!rva || !base)
                return 0;
            DWORD64 imageBase = 0;
            const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(base + rva, &imageBase, nullptr);
            return function ? function->BeginAddress : 0;
        }
    }

    bool IsPoseV2FootstepHelperCaller(std::uint32_t rva)
    {
        static const std::uint32_t kHelpers[] = {FoundRva(kAudioStepSound), FoundRva(kAudioStepLayers),
            FoundRva(kAudioStepCreate), FoundRva(kAudioStepOther), FoundRva(kAudioFootHelperB),
            FoundRva(kAudioFootHelperA)};
        const std::uint32_t start = FunctionStartRva(rva);
        if (!start)
            return false;
        for (const std::uint32_t helper : kHelpers)
        {
            if (helper == start)
                return true;
        }
        return false;
    }
    }

    void RecordPoseV2PlaySounds()
    {
        REPLAY_PROFILE_ZONE("rec.play_sounds");
        static std::vector<replay::audio::SoundRequest> requests;
        replay::audio::CollectPlayRequestsSince(g_poseV2PlaySoundCursor, requests);
        if (requests.empty())
            return;
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::SoundRequest& request : requests)
        {
            // Requests made inside a footstep or melee action call are
            // replayed by those systems; nothing plays twice.
            const bool covered = request.inReplayedSystem;
            bool sharedHelper = false;
            for (const std::uint32_t caller : request.callers)
                sharedHelper = sharedHelper || IsPoseV2FootstepHelperCaller(caller);
            if (sharedHelper && !covered && g_poseV2SharedHelperLogged < 40)
            {
                ++g_poseV2SharedHelperLogged;
                std::ostream& log = Ctx().log;
                log << "POSE_V2_SOUND_SHARED_HELPER,ref=0x" << std::hex << request.soundRef
                    << ",persistent=" << request.flags[0]
                    << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1]
                    << "/0x" << request.callers[2] << "/0x" << request.callers[3] << std::dec
                    << ",time_ms=" << nowMs << '\n';
                log.flush();
            }
            std::uint64_t tracker = 0;
            if (request.hasParams)
                std::memcpy(&tracker, request.params + 0xC8, sizeof(tracker));
            bool attributed = false;
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !replay::entities::Exists(entity.handle))
                    continue;
                void* address = replay::entities::AddressOf(entity.handle);
                const std::uint64_t holder = replay::audio::PedAudioHolder(address);
                const bool onPed = holder && request.audioEntity >= holder && request.audioEntity - holder < replay::audio::kPedAudioSpan;
                const bool tracked = tracker && tracker == replay::audio::PedSoundTracker(address) &&
                    request.audioEntity > module && request.audioEntity < module + 0x7000000;
                if (!onPed && !tracked)
                    continue;
                attributed = true;
                // Persistent sounds may loop: only the tracked collision
                // path replays them (with their stop), never as one-shots.
                if (covered || request.flags[0] == 1)
                {
                    ++g_poseV2PlaySoundsCovered;
                    break;
                }
                const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                replay::entity::SoundEvent sound;
                sound.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
                sound.entity = entity.id;
                sound.kind = onPed ? 1 : 2;
                sound.audioOffset = onPed ? request.audioEntity - holder : 0;
                sound.globalAudio = onPed ? 0 : request.audioEntity;
                sound.soundRef = request.soundRef;
                if (request.hasParams)
                    sound.params.assign(request.params, request.params + sizeof(request.params));
                Ctx().clip.sounds.Add(sound);
                if (Ctx().clip.sounds.Events().size() <= 80)
                {
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_SOUND_EVENT,ped=" << replay::entity::IdText(entity.id)
                        << ",kind=" << (onPed ? "direct" : "tracked")
                        << (request.flags[0] == 1 ? "_persistent" : "")
                        << ",ref=0x" << std::hex << request.soundRef
                        << ",audio=" << (onPed ? "ped+0x" : "static:0x")
                        << (onPed ? sound.audioOffset : request.audioEntity - module)
                        << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1]
                        << "/0x" << request.callers[2] << "/0x" << request.callers[3] << std::dec
                        << ",ref_state=" << replay::audio::IndirectRefState(request.soundRef)
                        << ",time_ms=" << sound.timeMs
                        << '\n';
                    log.flush();
                }
                break;
            }
            // One-shots on a recorded weapon object's audio (the carried
            // weapons rattle when the body hits the ground) belong to that
            // weapon and replay on its puppet.
            if (!attributed && !covered && request.flags[0] == 0)
            {
                for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                {
                    if (!weapon.object || !replay::entities::Exists(weapon.object))
                        continue;
                    const std::uint64_t audio = replay::audio::WeaponAudio(replay::entities::AddressOf(weapon.object));
                    if (!audio || request.audioEntity < audio || request.audioEntity - audio >= 0x200)
                        continue;
                    const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                    replay::entity::SoundEvent sound;
                    sound.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
                    sound.entity = weapon.id;
                    sound.kind = 4;
                    sound.audioOffset = request.audioEntity - audio;
                    sound.soundRef = request.soundRef;
                    if (request.hasParams)
                        sound.params.assign(request.params, request.params + sizeof(request.params));
                    Ctx().clip.sounds.Add(sound);
                    attributed = true;
                    if (g_poseV2WeaponSoundLogged < 40)
                    {
                        ++g_poseV2WeaponSoundLogged;
                        std::ostream& log = Ctx().log;
                        log << "POSE_V2_SOUND_WEAPON,weapon=" << replay::entity::IdText(weapon.id)
                            << ",ref=0x" << std::hex << request.soundRef
                            << ",offset=0x" << sound.audioOffset
                            << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1]
                            << "/0x" << request.callers[2] << "/0x" << request.callers[3] << std::dec
                            << ",time_ms=" << sound.timeMs << '\n';
                        log.flush();
                    }
                    break;
                }
            }
            // Collision sounds (persistent, on the static collision audio, no
            // tracker) born at a recorded ped's position belong to it (body fall and
            // drag sounds are within 0.2 m of the ped). Replayed on the same audio
            // entity and position.
            const bool staticAudio = request.audioEntity > module && request.audioEntity < module + 0x7000000;
            // Collision sounds are persistent (looped scrapes): tracked from creation
            // to stop and replayed as persistent sounds, never as one-shots (a one-shot
            // replay would leave the loops playing).
            if (!attributed && !covered && request.flags[0] == 1 && request.sound && request.hasParams &&
                request.audioEntity == replay::audio::StaticCollisionAudio())
            {
                float position[3] = {};
                std::memcpy(position, request.params, sizeof(position));
                const replay::recorder::RecordedEntity* nearest = nullptr;
                float best = 0.75f;
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                {
                    if (entity.vehicle || !replay::entities::Exists(entity.handle))
                        continue;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, TRUE);
                    const float dx = at.x - position[0];
                    const float dy = at.y - position[1];
                    const float dz = at.z - position[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < best)
                    {
                        best = distance;
                        nearest = &entity;
                    }
                }
                if (nearest)
                {
                    const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                    replay::CollisionSound sound;
                    sound.entity = nearest->id;
                    sound.startMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
                    sound.endMs = sound.startMs;
                    sound.soundRef = request.soundRef;
                    sound.params.assign(request.params, request.params + sizeof(request.params));
                    Ctx().clip.collisionSounds.push_back(std::move(sound));
                    g_poseV2LiveCollisionSounds.push_back(
                        {request.sound, request.slot, Ctx().clip.collisionSounds.size() - 1});
                    attributed = true;
                }
            }
            // Evidence: every request made by the collision impact code (it
            // plays impacts through the create-and-play by hash on the
            // colliding entity's audio), attributed or not.
            {
                static const std::uint32_t impactA = FoundRva(kImpactCodeA);
                static const std::uint32_t impactB = FoundRva(kImpactCodeB);
                bool impactCode = false;
                for (const std::uint32_t caller : request.callers)
                {
                    const std::uint32_t start = FunctionStartRva(caller);
                    impactCode = impactCode || (start && (start == impactA || start == impactB));
                }
                if (impactCode && g_poseV2ImpactLogged < 60)
                {
                    ++g_poseV2ImpactLogged;
                    float position[3] = {};
                    if (request.hasParams)
                        std::memcpy(position, request.params, sizeof(position));
                    std::string nearestId = "none";
                    float nearestM = 1e9f;
                    for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                    {
                        if (entity.vehicle || !replay::entities::Exists(entity.handle))
                            continue;
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, TRUE);
                        const float d = std::sqrt((at.x - position[0]) * (at.x - position[0]) +
                            (at.y - position[1]) * (at.y - position[1]) + (at.z - position[2]) * (at.z - position[2]));
                        if (d < nearestM)
                        {
                            nearestM = d;
                            nearestId = replay::entity::IdText(entity.id);
                        }
                    }
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_COLLISION_IMPACT_SOUND"
                        << ",attributed=" << (attributed ? 1 : 0)
                        << ",covered=" << (covered ? 1 : 0)
                        << ",persistent=" << (request.flags[0] == 1 ? 1 : 0)
                        << ",ref=0x" << std::hex << request.soundRef
                        << ",audio=" << (staticAudio ? "static:0x" : "other:0x")
                        << (staticAudio ? request.audioEntity - module : request.audioEntity)
                        << ",tracker=0x" << tracker
                        << ",callers=0x" << request.callers[1] << "/0x" << request.callers[2]
                        << "/0x" << request.callers[3] << std::dec
                        << ",nearest=" << nearestId << ",nearest_m=" << nearestM
                        << ",time_ms=" << nowMs << '\n';
                    log.flush();
                }
            }
            // Collision one-shots (impacts: direct create-and-play on the
            // static collision audio, no tracker) born at a recorded ped's
            // position belong to it and replay as one-shots there.
            if (!attributed && !covered && request.flags[0] == 0 && staticAudio && request.hasParams && !tracker)
            {
                float position[3] = {};
                std::memcpy(position, request.params, sizeof(position));
                const replay::recorder::RecordedEntity* nearest = nullptr;
                float best = 3.0f;
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                {
                    if (entity.vehicle || !replay::entities::Exists(entity.handle))
                        continue;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, TRUE);
                    const float dx = at.x - position[0];
                    const float dy = at.y - position[1];
                    const float dz = at.z - position[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < best)
                    {
                        best = distance;
                        nearest = &entity;
                    }
                }
                // An impact can come from any part of the body (a fall impact can be
                // about 1 m from the ped origin).
                if (nearest && best <= 1.5f)
                {
                    const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                    replay::entity::SoundEvent sound;
                    sound.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
                    sound.entity = nearest->id;
                    sound.kind = 3;
                    sound.globalAudio = request.audioEntity;
                    sound.soundRef = request.soundRef;
                    sound.params.assign(request.params, request.params + sizeof(request.params));
                    Ctx().clip.sounds.Add(sound);
                    attributed = true;
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_SOUND_GROUND_IMPACT,ped=" << replay::entity::IdText(nearest->id)
                        << ",ref=0x" << std::hex << request.soundRef << std::dec
                        << ",distance_m=" << best << ",time_ms=" << sound.timeMs << '\n';
                    log.flush();
                }
                else if (nearest && g_poseV2PersistentUnattributedLogged < 40)
                {
                    ++g_poseV2PersistentUnattributedLogged;
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_SOUND_UNATTRIBUTED,kind=impact"
                        << ",ref=0x" << std::hex << request.soundRef
                        << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1]
                        << "/0x" << request.callers[2] << std::dec
                        << ",nearest=" << replay::entity::IdText(nearest->id) << ",nearest_m=" << best
                        << ",time_ms=" << nowMs << '\n';
                    log.flush();
                }
            }
            // A one-shot nobody recorded owns whose tracker or audio entity is inside
            // an object goes to the object sounds (it may be a recorded object's, or
            // one about to be recorded).
            if (!attributed && !covered && request.flags[0] == 0 && request.hasParams && tracker &&
                !(request.audioEntity > module && request.audioEntity < module + 0x7000000 &&
                    request.audioEntity == replay::audio::StaticCollisionAudio()))
            {
                PoseV2ObjectSoundCandidate candidate;
                candidate.timeUs = request.timeUs;
                candidate.audio = request.audioEntity;
                candidate.soundRef = request.soundRef;
                candidate.tracker = tracker;
                candidate.hasParams = true;
                std::memcpy(candidate.params, request.params, sizeof(candidate.params));
                candidate.callers[0] = request.callers[1];
                candidate.callers[1] = request.callers[2];
                candidate.callers[2] = request.callers[3];
                candidate.path = "direct";
                QueuePoseV2ObjectSound(candidate);
            }
            // Every other request nobody recorded owns, with its tracker and the
            // nearest recorded prop.
            if (g_poseV2ResearchWindows && !attributed && !covered)
            {
                static replay::log::LogLimit unattributedLogs; // per session
                if (unattributedLogs.Take(150))
                {
                    float position[3] = {};
                    if (request.hasParams)
                        std::memcpy(position, request.params, sizeof(position));
                    const replay::entity::ReplayId trackerId = RecordedIdAtAddress(tracker);
                    std::string nearProp = "none";
                    float nearPropM = 1e9f;
                    for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                    {
                        if (!prop.object || !replay::entities::Exists(prop.object))
                            continue;
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(prop.object, TRUE, TRUE);
                        const float d = std::sqrt((at.x - position[0]) * (at.x - position[0]) +
                            (at.y - position[1]) * (at.y - position[1]) + (at.z - position[2]) * (at.z - position[2]));
                        if (d < nearPropM)
                        {
                            nearPropM = d;
                            nearProp = replay::entity::IdText(prop.id);
                        }
                    }
                    const std::uint64_t ageMs = now > request.timeUs ? (now - request.timeUs) / 1000 : 0;
                    Ctx().log << "POSE_V2_SOUND_UNATTR_ALL,persistent=" << request.flags[0] << std::hex << ",ref=0x" << request.soundRef
                        << ",audio=" << (staticAudio ? "static:0x" : "heap:0x") << (staticAudio ? request.audioEntity - module : request.audioEntity)
                        << ",tracker=0x" << tracker << std::dec << ",tracker_id=" << (trackerId.Valid() ? replay::entity::IdText(trackerId) : std::string("none"))
                        << std::hex << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1] << "/0x" << request.callers[2]
                        << "/0x" << request.callers[3] << std::dec << ",params=" << request.hasParams
                        << ",pos=" << position[0] << "/" << position[1] << "/" << position[2]
                        << ",near_prop=" << nearProp << ",near_prop_m=" << nearPropM
                        << ",time_ms=" << replay::pose::capture::RecordMsAgo(ageMs, nowMs) << '\n';
                }
            }
            // Evidence: collision (persistent) sounds nobody recorded owns,
            // with where they come from and where they are.
            if (!attributed && request.flags[0] == 1 && g_poseV2PersistentUnattributedLogged < 40)
            {
                ++g_poseV2PersistentUnattributedLogged;
                float position[3] = {};
                if (request.hasParams)
                    std::memcpy(position, request.params, sizeof(position));
                std::string nearest = "none";
                float best = 1e9f;
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                {
                    if (entity.vehicle || !replay::entities::Exists(entity.handle))
                        continue;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, TRUE);
                    const float dx = at.x - position[0];
                    const float dy = at.y - position[1];
                    const float dz = at.z - position[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < best)
                    {
                        best = distance;
                        nearest = replay::entity::IdText(entity.id);
                    }
                }
                std::ostream& log = Ctx().log;
                log << "POSE_V2_SOUND_UNATTRIBUTED,kind=persistent"
                    << ",ref=0x" << std::hex << request.soundRef
                    << ",audio=" << (request.audioEntity > module && request.audioEntity < module + 0x7000000 ? "static:0x" : "heap:0x")
                    << (request.audioEntity > module && request.audioEntity < module + 0x7000000 ? request.audioEntity - module : request.audioEntity)
                    << ",tracker=0x" << tracker
                    << ",callers=0x" << request.callers[0] << "/0x" << request.callers[1]
                    << "/0x" << request.callers[2] << "/0x" << request.callers[3] << std::dec
                    << ",params=" << (request.hasParams ? 1 : 0)
                    << ",pos=" << position[0] << "/" << position[1] << "/" << position[2]
                    << ",nearest=" << nearest << ",nearest_m=" << best
                    << ",time_ms=" << nowMs
                    << '\n';
                log.flush();
            }
        }
    }

    // Follows every tracked collision sound until the engine stops it (the
    // stop detour) or it ends by itself (its slot no longer holds it),
    // sampling its intensity each frame.
    void TrackPoseV2CollisionSounds(bool closeAll)
    {
        TrackPoseV2EntitySounds(closeAll);
        REPLAY_PROFILE_ZONE("rec.collision_sounds");
        static std::vector<replay::audio::SoundStop> stops;
        replay::audio::CollectSoundStopsSince(g_poseV2SoundStopCursor, stops);
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (std::size_t index = g_poseV2LiveCollisionSounds.size(); index-- > 0;)
        {
            const PoseV2LiveCollisionSound live = g_poseV2LiveCollisionSounds[index];
            replay::CollisionSound& sound = Ctx().clip.collisionSounds[live.index];
            std::uint64_t current = 0;
            const bool stopped = std::any_of(stops.begin(), stops.end(),
                [&](const replay::audio::SoundStop& stop) { return stop.sound == live.sound; });
            const bool gone = !replay::audio::ReadU64(live.slot, current) || current != live.sound;
            if (closeAll || stopped || gone)
            {
                sound.endMs = nowMs;
                sound.ended = true;
                g_poseV2LiveCollisionSounds.erase(g_poseV2LiveCollisionSounds.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            float value = 0.0f;
            if (replay::audio::ReadSoundVariable(live.sound, kPoseV2CollisionIntensity, value) &&
                (sound.intensity.empty() || sound.intensity.back().second != value))
            {
                sound.intensity.push_back({nowMs, value});
            }
        }
    }

    void PlayPoseV2MeleeHits(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.melee");
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        const bool normalPlay = Ctx().hooks->NormalPlay();
        if (!g_poseV2MeleeCursorPlay.Advance(timeMs, normalPlay, fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.meleeHits.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::MeleeEvent& hit = Ctx().clip.meleeHits.Events()[index];
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(hit.entity);
            std::string status = "no_puppet";
            std::string sounds = "none";
            bool ok = false;
            bool ready = false;
            if (puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                replay::entities::Exists(puppet->clone))
            {
                ready = true;
                sounds = PoseV2SoundsDuring([&] {
                    ok = replay::audio::PlayMeleeImpact(replay::entities::AddressOf(puppet->clone),
                        hit.info, hit.infoVtable, hit.infoHash, status);
                });
            }
            ++(ok ? g_poseV2MeleePlayed : g_poseV2MeleeRefused);
            g_poseV2MeleeLastStatus = status;
            // A hit recorded with its sound that the replay plays mute is flagged.
            const int soundCount = replay::meleecheck::SoundCount(sounds);
            const replay::meleecheck::Outcome outcome = replay::meleecheck::Judge(ready, ok, soundCount);
            g_meleeCheck.Note(hit.entity.Packed(), hit.timeMs, hit.infoHash, outcome, soundCount, status);
            std::ostream& log = Ctx().log;
            log << "POSE_V2_PLAYBACK_MELEE,ped=" << replay::entity::IdText(hit.entity)
                << ",hash=0x" << std::hex << hit.infoHash << std::dec
                << ",status=" << status
                << ",sounds=" << sounds
                << ",outcome=" << replay::meleecheck::OutcomeToken(outcome)
                << ",event_ms=" << hit.timeMs
                << ",playhead_ms=" << timeMs
                << '\n';
            if (outcome != replay::meleecheck::Outcome::Played)
            {
                replay::meleecheck::Row row;
                row.owner = hit.entity.Packed();
                row.eventMs = hit.timeMs;
                row.hash = hit.infoHash;
                row.outcome = outcome;
                row.sounds = soundCount;
                row.status = status;
                log << replay::meleecheck::Tally::FlagLine(row) << '\n';
            }
            log.flush();
        }
    }

    void PlayPoseV2CollisionSounds(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.collision_sounds");
        const bool normalPlay = Ctx().hooks->NormalPlay();
        const bool step = normalPlay && timeMs >= g_poseV2CollisionLastMs &&
            timeMs - g_poseV2CollisionLastMs <= replay::entity::ForwardEventCursor::kMaxStepMs;
        if (!step)
        {
            // Paused, scrubbing or a jump: nothing keeps playing, and the
            // start cursor follows the playhead.
            StopPoseV2CollisionSounds();
            g_poseV2CollisionNext = 0;
            while (g_poseV2CollisionNext < Ctx().clip.collisionSounds.size() &&
                Ctx().clip.collisionSounds[g_poseV2CollisionNext].startMs <= timeMs)
            {
                ++g_poseV2CollisionNext;
            }
            g_poseV2CollisionLastMs = timeMs;
            return;
        }
        // Start the sounds whose start was crossed.
        while (g_poseV2CollisionNext < Ctx().clip.collisionSounds.size() &&
            Ctx().clip.collisionSounds[g_poseV2CollisionNext].startMs <= timeMs)
        {
            const std::size_t index = g_poseV2CollisionNext++;
            const replay::CollisionSound& sound = Ctx().clip.collisionSounds[index];
            if (sound.startMs <= g_poseV2CollisionLastMs || sound.endMs <= timeMs)
                continue;
            if (replay::soundgeneric::PacketCovers(sound.entity.Packed(), sound.soundRef, sound.startMs))
            {
                ++g_poseV2CollisionToGeneric;
                continue;
            }
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(sound.entity);
            std::size_t free = kPoseV2CollisionSlots;
            for (std::size_t slot = 0; slot < kPoseV2CollisionSlots; ++slot)
            {
                if (!g_poseV2CollisionSlot[slot])
                {
                    free = slot;
                    break;
                }
            }
            std::string status = "no_puppet";
            bool ok = false;
            if (free == kPoseV2CollisionSlots)
            {
                status = "no_slot";
            }
            else if (puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                replay::entities::Exists(puppet->clone))
            {
                ok = replay::audio::StartPersistentSound(replay::audio::StaticCollisionAudio(), sound.soundRef,
                    sound.params.data(), &g_poseV2CollisionSlot[free], status);
                if (ok)
                    g_poseV2CollisionSlotEvent[free] = index;
            }
            ++(ok ? g_poseV2CollisionPlayed : g_poseV2CollisionRefused);
            g_poseV2CollisionLast = status;
        }
        // Drive and stop the playing ones.
        for (std::size_t slot = 0; slot < kPoseV2CollisionSlots; ++slot)
        {
            if (!g_poseV2CollisionSlot[slot])
                continue;
            const replay::CollisionSound& sound = Ctx().clip.collisionSounds[g_poseV2CollisionSlotEvent[slot]];
            if (timeMs >= sound.endMs)
            {
                replay::audio::StopPersistentSound(&g_poseV2CollisionSlot[slot]);
                continue;
            }
            float value = -1.0f;
            for (const auto& sample : sound.intensity)
            {
                if (sample.first > timeMs)
                    break;
                value = sample.second;
            }
            if (value >= 0.0f)
                replay::audio::WriteSoundVariable(g_poseV2CollisionSlot[slot], kPoseV2CollisionIntensity, value);
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(sound.entity);
            if (puppet && puppet->clone && replay::entities::Exists(puppet->clone))
            {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, TRUE);
                const float position[4] = {at.x, at.y, at.z, 0.0f};
                replay::audio::SetSoundPosition(g_poseV2CollisionSlot[slot], position);
            }
        }
        g_poseV2CollisionLastMs = timeMs;
    }

    // ShotEnvGroupReplay: the holder's environment group on a carried weapon's
    // sound the live game made with one.
    bool g_shotEnvGroupReplay = true;
    std::uint32_t g_shotEnvLines = 0;
    std::uint64_t g_shotEnvGiven = 0;
    std::uint64_t g_shotEnvMissing = 0;
    namespace
    {

    // Carried weapon sounds: on the weapon puppet's audio, following the ped
    // puppet it is attached to.
    bool PlayPoseV2WeaponSound(const replay::playback::Puppet& puppet, const replay::entity::SoundEvent& sound, std::string& status)
    {
        const std::uint64_t audio = replay::audio::WeaponAudio(replay::entities::AddressOf(puppet.clone));
        if (!audio || sound.audioOffset >= 0x200)
        {
            status = "no_weapon_audio";
            return false;
        }
        void* ped = nullptr;
        static std::vector<replay::entity::Attachment> attachments;
        Ctx().clip.lifecycle.AttachmentsAt(sound.timeMs, attachments);
        for (const replay::entity::Attachment& attachment : attachments)
        {
            if (attachment.child != puppet.packet.id || attachment.kind != replay::entity::AttachmentKind::WeaponPoint)
                continue;
            const replay::playback::Puppet* parent = Ctx().puppets.Find(attachment.parent);
            if (parent && parent->state == replay::playback::PuppetState::Ready && parent->clone &&
                replay::entities::Exists(parent->clone))
            {
                ped = replay::entities::AddressOf(parent->clone);
            }
            break;
        }
        // The recorded params carry the live group's pointer at +0xE0 (non zero:
        // the game gave one, the holder's); the copy of the params
        // leaves it clear, so it is asked again here.
        std::uint64_t env = 0;
        const char* envStatus = "not_asked";
        std::uint64_t recordedEnv = 0;
        if (sound.params.size() >= 0xE8)
            std::memcpy(&recordedEnv, sound.params.data() + 0xE0, sizeof(recordedEnv));
        if (g_shotEnvGroupReplay && recordedEnv && ped)
            env = replay::audio::EnvironmentGroupOf(ped, envStatus);
        if (recordedEnv)
            ++(env ? g_shotEnvGiven : g_shotEnvMissing);
        const bool ok = replay::audio::PlayDirectSound(ped, 0, audio + sound.audioOffset, sound.soundRef,
            sound.params.empty() ? nullptr : sound.params.data(), ped != nullptr, status, env);
        if (g_shotEnvLines < 200)
        {
            ++g_shotEnvLines;
            Ctx().log << "SHOT_ENV,phase=play,path=weapon_sound,weapon=" << replay::entity::IdText(puppet.packet.id)
                      << ",holder=" << (ped ? 1 : 0) << ",recorded_env=" << (recordedEnv ? 1 : 0) << ",given=" << (env ? 1 : 0)
                      << ",status=" << envStatus << ",rule=" << (g_shotEnvGroupReplay ? 1 : 0) << ",ref=0x" << std::hex
                      << sound.soundRef << std::dec << ",result=" << status << ",event_ms=" << sound.timeMs << '\n';
        }
        return ok;
    }
    }

    void PlayPoseV2Sounds(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.sounds");
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        const bool normalPlay = Ctx().hooks->NormalPlay();
        if (!g_poseV2SoundCursorPlay.Advance(timeMs, normalPlay, fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.sounds.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::SoundEvent& sound = Ctx().clip.sounds.Events()[index];
            if (replay::soundgeneric::PacketCovers(sound.entity.Packed(), sound.soundRef, sound.timeMs))
            {
                ++g_poseV2SoundsToGeneric;
                continue;
            }
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(sound.entity);
            std::string status = "no_puppet";
            bool ok = false;
            if (puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                replay::entities::Exists(puppet->clone))
            {
                void* entity = replay::entities::AddressOf(puppet->clone);
                const std::uint8_t* params = sound.params.empty() ? nullptr : sound.params.data();
                ok = sound.kind == 0
                    ? replay::audio::PlaySoundRequest(entity, sound.audioOffset, sound.soundRef, params, sound.flags, status)
                    : sound.kind == 3
                    ? PlayPoseV2GroundImpact(sound, status)
                    : sound.kind == 4
                    ? PlayPoseV2WeaponSound(*puppet, sound, status)
                    // A static audio entity named by the clip only when it is one
                    // the recorder writes, with its vtable (not any image address).
                    : (!sound.globalAudio || replay::audio::TrustedImageAudio(sound.globalAudio, status)) &&
                      replay::audio::PlayDirectSound(entity, sound.audioOffset, sound.globalAudio, sound.soundRef,
                          params, sound.kind != 3, status);
            }
            if (status == "not_queued")
                status += std::string(":") + replay::audio::IndirectRefState(sound.soundRef);
            if (!ok && puppet && puppet->clone && puppet->packet.kind == replay::entity::EntityKind::Ped &&
                status.find("audio_unregistered") != std::string::npos)
                LogPuppetAudioState(*puppet, "sound", timeMs);
            if (!ok && status == "neighbor_ped_audio")
                ++g_poseV2SoundsNeighbor;
            else
                ++(ok ? g_poseV2SoundsPlayed : g_poseV2SoundsRefused);
            g_poseV2SoundLast = status;
            // A refused image audio is always logged (a clip naming one the
            // recorder does not write).
            if (g_poseV2SoundsPlayed + g_poseV2SoundsRefused <= 80 || sound.kind >= 3 ||
                status.rfind("unknown_image_audio", 0) == 0 || status.rfind("image_audio_vtable_mismatch", 0) == 0)
            {
                std::ostream& log = Ctx().log;
                log << "POSE_V2_PLAYBACK_SOUND,ped=" << replay::entity::IdText(sound.entity)
                    << ",kind=" << static_cast<int>(sound.kind)
                    << ",ref=0x" << std::hex << sound.soundRef << std::dec
                    << ",status=" << status
                    << ",event_ms=" << sound.timeMs
                    << ",playhead_ms=" << timeMs
                    << '\n';
                log.flush();
            }
        }
    }

    // The animation audio track in normal forward play, like the other one-shot
    // sounds: each due event is queued on the puppet's audio entity with the
    // engine's AddAnimEvent, and the next audio update runs the ped handler on
    // it as it would for the clip's Audio tag. Only Ready ped and horse puppets
    // whose clone is a ped are asked. A full queue (4) is tried once more next
    // frame. The next frame also checks that the engine consumed the queue
    // (count back to 0), evidence that ProcessAnimEvents ran.
    void PlayPoseV2AnimAudio(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.anim_audio");
        for (const std::uint64_t audio : g_poseV2AnimQueuedAudio)
        {
            const std::int32_t count = replay::audio::AnimEventCount(audio);
            ++(count == 0 ? g_poseV2AnimConsumed : g_poseV2AnimNotConsumed);
        }
        g_poseV2AnimQueuedAudio.clear();
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2AnimCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
        {
            g_poseV2AnimCarried.clear();
            return;
        }
        static std::vector<std::size_t> range;
        static std::vector<std::pair<std::size_t, bool>> due; // index, retry
        Ctx().clip.animAudio.Range(fromMs, toMs, range);
        // Packets mode: the events' sounds are in the packets.
        if (replay::soundgeneric::PacketsMode())
        {
            g_poseV2AnimToGeneric += range.size();
            g_poseV2AnimCarried.clear();
            return;
        }
        due.clear();
        for (const std::size_t index : g_poseV2AnimCarried)
            due.push_back({index, true});
        for (const std::size_t index : range)
            due.push_back({index, false});
        g_poseV2AnimCarried.clear();
        const std::vector<replay::entity::AnimAudioEvent>& events = Ctx().clip.animAudio.Events();
        for (const auto& [index, retry] : due)
        {
            if (index >= events.size())
                continue;
            const replay::entity::AnimAudioEvent& event = events[index];
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(event.entity);
            std::string status = "no_puppet";
            bool ok = false;
            std::uint64_t audio = 0;
            if (puppet && (puppet->packet.kind == replay::entity::EntityKind::Ped ||
                              puppet->packet.kind == replay::entity::EntityKind::Horse) &&
                puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                replay::entities::Exists(puppet->clone) && replay::entities::IsPed(puppet->clone))
            {
                ok = replay::audio::QueueAnimEvent(replay::entities::AddressOf(puppet->clone), event.hash, event.extra,
                    audio, status);
            }
            if (ok)
            {
                ++g_poseV2AnimPlayed;
                if (std::find(g_poseV2AnimQueuedAudio.begin(), g_poseV2AnimQueuedAudio.end(), audio) ==
                    g_poseV2AnimQueuedAudio.end())
                    g_poseV2AnimQueuedAudio.push_back(audio);
            }
            else if (status == "queue_full" && !retry)
            {
                ++g_poseV2AnimRetried;
                g_poseV2AnimCarried.push_back(index);
                continue;
            }
            else if (status == "no_puppet")
            {
                ++g_poseV2AnimNoPuppet;
            }
            else
            {
                ++g_poseV2AnimRefused;
                ++g_poseV2AnimRefusals[status];
            }
            if (++g_poseV2AnimPlayLines <= 80 || (!ok && g_poseV2AnimRefused <= 20 && status != "no_puppet"))
                Ctx().log << "POSE_V2_ANIM_AUDIO,phase=play,ped=" << replay::entity::IdText(event.entity) << ",hash=0x" << std::hex
                          << event.hash << std::dec << ",extra=" << event.extra << ",status=" << status
                          << ",retry=" << (retry ? 1 : 0) << ",event_ms=" << event.timeMs << ",playhead_ms=" << timeMs << '\n';
        }
    }

    // One line per horse and per animal (vocals and steps), one per person with
    // a silent line, and the totals; the in-game acceptance reads
    // POSE_V2_SOUND_TALLY (horse_sound_report.h).
    void LogPoseV2SoundTallies(std::ostream& log)
    {
        const auto& tallies = g_poseV2SoundTallies;
        std::set<std::string> ids;
        for (const auto& entry : tallies.speech)
            ids.insert(entry.first);
        for (const auto& entry : tallies.feet)
            ids.insert(entry.first);
        for (const std::string& id : ids)
        {
            const auto speechIt = tallies.speech.find(id);
            const auto feetIt = tallies.feet.find(id);
            const replay::horsesound::SpeechTally speech =
                speechIt != tallies.speech.end() ? speechIt->second : replay::horsesound::SpeechTally{};
            const replay::horsesound::FootTally feet =
                feetIt != tallies.feet.end() ? feetIt->second : replay::horsesound::FootTally{};
            const bool horse = feet.horse || speech.cls == replay::horsesound::SpeakerClass::Horse;
            const bool animal = speech.cls == replay::horsesound::SpeakerClass::Animal;
            if (!horse && !animal && !replay::horsesound::SpeechProblem(speech))
                continue;
            log << "POSE_V2_HORSE_SOUND,id=" << id
                << ",class=" << (horse ? "horse" : replay::horsesound::ClassToken(speech.cls))
                << ",vocals_due=" << speech.recorded << ",vocals_played=" << speech.played
                << ",vocals_refused=" << speech.refused << ",vocals_killed=" << speech.killed
                << ",vocals_no_puppet=" << speech.noPuppet
                << ",native_pinned=" << speech.pinned << ",native_context=" << speech.context
                << ",direct=" << speech.direct << ",other_phrase=" << speech.mismatch
                << ",start_delay_avg_ms=" << speech.DelayAvgMs() << ",start_delay_max_ms=" << speech.delayMaxMs
                << ",worst_hole_ms=" << speech.worstHoleMs << ",worst_hole_at_ms=" << speech.worstHoleAtMs
                << ",steps_due=" << feet.recorded << ",steps_played=" << feet.played << ",steps_refused=" << feet.refused
                << ",wet_known=" << feet.wetKnown << ",wet=" << feet.wet << ",wet_played=" << feet.wetPlayed
                << ",wet_gate_closed=" << feet.wetGateClosed
                << ",reasons=" << (speech.reasons.empty() && feet.reasons.empty() ? std::string("none")
                       : replay::horsesound::ReasonsText(speech.reasons) +
                           (speech.reasons.empty() || feet.reasons.empty() ? "" : "; ") +
                           replay::horsesound::ReasonsText(feet.reasons))
                << '\n';
        }
        const auto animals = tallies.SpeechTotals(true);
        const auto all = tallies.SpeechTotals(false);
        std::uint32_t wet = 0, wetPlayed = 0, wetKnown = 0, horseSteps = 0, horsePlayed = 0;
        for (const auto& entry : tallies.feet)
        {
            if (!entry.second.horse)
                continue;
            wet += entry.second.wet;
            wetPlayed += entry.second.wetPlayed;
            wetKnown += entry.second.wetKnown;
            horseSteps += entry.second.recorded;
            horsePlayed += entry.second.played;
        }
        char percent[32]{};
        std::snprintf(percent, sizeof(percent), "%.1f", replay::horsesound::SilentPercent(animals));
        log << "POSE_V2_SOUND_TALLY,animal_vocals_due=" << animals.recorded << ",animal_vocals_played=" << animals.played
            << ",animal_vocals_refused=" << animals.refused << ",animal_vocals_killed=" << animals.killed
            << ",animal_vocals_no_puppet=" << animals.noPuppet << ",animal_silent_pct=" << percent
            << ",speech_due=" << all.recorded << ",speech_played=" << all.played << ",speech_refused=" << all.refused
            << ",speech_killed=" << all.killed
            << ",horse_steps_due=" << horseSteps << ",horse_steps_played=" << horsePlayed
            << ",horse_wet_known=" << wetKnown << ",horse_wet=" << wet << ",horse_wet_played=" << wetPlayed << '\n';
    }

    void PlayPoseV2Footsteps(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.footsteps");
        static std::vector<replay::audio::FootstepCall> engine;
        replay::audio::CollectFootstepsSince(g_poseV2FootstepEngineCursor, engine);
        // Only Ready ped and horse puppets whose clone is a ped are asked for their
        // foot audio, once per frame. An object's +0x9C is not a ped component
        // index, so the component row read would be another one and its +0x30 slot
        // holds no pointer; each fault is caught, but its dispatch runs every
        // vectored handler in the process.
        if (!engine.empty())
        {
            static std::vector<std::uint64_t> puppetFeet;
            puppetFeet.clear();
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                        puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                    !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                    continue;
                const std::uint64_t foot = replay::audio::FootstepAudio(replay::entities::AddressOf(puppet.clone));
                if (foot)
                    puppetFeet.push_back(foot);
            }
            for (const replay::audio::FootstepCall& call : engine)
            {
                for (const std::uint64_t foot : puppetFeet)
                {
                    if (foot == call.footAudio)
                    {
                        ++g_poseV2FootstepsPuppetEngine;
                        break;
                    }
                }
            }
        }
        const ULONGLONG nowTick = GetTickCount64();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            // Horses too, and only a clone that exists and is a ped (the same check as
            // the engine footstep match above).
            if ((puppet.packet.kind == replay::entity::EntityKind::Ped || puppet.packet.kind == replay::entity::EntityKind::Horse) &&
                puppet.state == replay::playback::PuppetState::Ready && puppet.clone &&
                replay::entities::Exists(puppet.clone) && replay::entities::IsPed(puppet.clone))
            {
                std::string status;
                replay::audio::ForceFootstepAudio(replay::entities::AddressOf(puppet.clone), status);
                if (status != "already" && ++g_poseV2FootForced[replay::entity::IdText(puppet.packet.id) + ":" + status] <= 3)
                    Ctx().log << "POSE_V2_PED_AUDIO_FORCE,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",status=" << status << ",playhead_ms=" << timeMs << '\n';
                // A puppet whose steps the clip carries keeps its audio LOD low: at a
                // holder LOD above the foot's max LOD (e.g. 3 against 2 at 30 m from the
                // free camera) the engine turns the foot off and its steps,
                // with every layer, are refused.
                if (std::binary_search(g_poseV2FootLodIds.begin(), g_poseV2FootLodIds.end(), puppet.packet.id.Packed()))
                {
                    std::string lodStatus;
                    // The recorded LOD at the playhead, never above the foot's max LOD; LOD 0
                    // when the clip has none for this owner.
                    std::uint8_t wantedLod = 0;
                    bool recordedLod = false;
                    if (g_poseV2PuppetAudioLodRecorded)
                    {
                        const auto points = g_poseV2FootLodPoints.find(puppet.packet.id.Packed());
                        std::uint8_t maxLod = replay::footmaterial::kLodUnknown;
                        replay::audio::PuppetFootMaxLod(replay::entities::AddressOf(puppet.clone), maxLod);
                        recordedLod = points != g_poseV2FootLodPoints.end() &&
                            replay::footmaterial::PuppetAudioLodAt(points->second, timeMs, maxLod, wantedLod);
                        if (recordedLod && wantedLod < 4)
                            ++g_poseV2FootLodChosen[wantedLod];
                    }
                    replay::audio::SetPuppetAudioLodValue(replay::entities::AddressOf(puppet.clone), recordedLod ? wantedLod : 0,
                        lodStatus);
                    if (lodStatus != "already")
                    {
                        if (recordedLod && lodStatus.compare(0, 10, "forced_lod") == 0)
                            ++g_poseV2FootLodRecordedWrites;
                        else if (lodStatus == "forced_lod0")
                            ++g_poseV2FootLodWrites;
                        if (++g_poseV2FootLodLogged[replay::entity::IdText(puppet.packet.id) + ":" + lodStatus] <= 3)
                            Ctx().log << "POSE_V2_PED_AUDIO_LOD,id=" << replay::entity::IdText(puppet.packet.id)
                                      << ",status=" << lodStatus << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            if (puppet.packet.kind == replay::entity::EntityKind::Ped &&
                puppet.state == replay::playback::PuppetState::Ready && puppet.clone &&
                nowTick - puppet.stateSince >= 1000 && replay::entities::Exists(puppet.clone))
                LogPuppetAudioState(puppet, "ready1s", timeMs);
        }
        // The movement volume detour acts while a clip plays, knowing whether the
        // hidden live player rides (the engine's own test reads his mount)
        // (foot_material.h MovementTermCorrection).
        {
            const Ped live = PLAYER::PLAYER_PED_ID();
            replay::audio::SetMovementTermState(true, live && PED::IS_PED_ON_MOUNT(live));
        }
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        const bool normalPlay = Ctx().hooks->NormalPlay();
        if (!g_poseV2FootstepCursorPlay.Advance(timeMs, normalPlay, fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.footsteps.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::FootstepEvent& step = Ctx().clip.footsteps.Events()[index];
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(step.entity);
            // Per entity, for the log and the sound report.
            replay::horsesound::FootTally& tally = PoseV2FootTally(step.entity);
            ++tally.recorded;
            const float wetness = replay::footmaterial::DecodeWetness(step.wetness);
            if (step.wetness)
                ++tally.wetKnown;
            if (replay::footmaterial::WetStep(wetness))
                ++tally.wet;
            if (!puppet || puppet->state != replay::playback::PuppetState::Ready || !puppet->clone ||
                !replay::entities::Exists(puppet->clone))
            {
                ++g_poseV2FootstepsRefused;
                ++g_poseV2FootstepRefusals[replay::entity::IdText(step.entity) + ":no_puppet"];
                ++tally.refused;
                ++tally.reasons["no_puppet"];
                continue;
            }
            replay::audio::FootstepCall call;
            call.eventId = step.eventId;
            call.arg3 = step.arg3;
            call.foot = step.foot;
            call.arg5 = step.arg5;
            call.flag = step.flag;
            call.arg7 = step.arg7;
            call.arg8 = step.arg8;
            call.materialHash = step.materialHash;
            call.materialRef = step.materialRef;
            call.wetness = wetness;
            if (step.mixKnown)
            {
                call.mix.known = true;
                call.mix.volume = step.mixVolume;
                call.mix.highPass = step.mixHighPass;
                call.mix.lowPass = step.mixLowPass;
            }
            call.drive.baseKnown = (step.driveKnown & 1) != 0;
            call.drive.base = step.driveBase;
            call.drive.impactKnown = (step.driveKnown & 2) != 0;
            call.drive.impact = step.driveImpact;
            if (step.layersKnown)
            {
                call.layersKnown = true;
                for (std::uint8_t l = 0; l < replay::footmaterial::kLayers; ++l)
                    call.layers[l] = replay::footmaterial::DecodeLayer(step.layer[l]);
            }
            std::string error;
            if (replay::audio::PlayFootstep(replay::entities::AddressOf(puppet->clone), call, error))
            {
                ++g_poseV2FootstepsPlayed;
                ++tally.played;
                if (replay::footmaterial::WetStep(wetness) && replay::audio::FootWetnessReplay())
                {
                    // The step's own gate: the ped's +0xB0 bit 7
                    // set skips the wet layer (guarded read, rung 2).
                    std::uint64_t flags = 0;
                    if (replay::audio::ReadU64(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone)) + 0xB0,
                            flags) && (flags & 0x80))
                        ++tally.wetGateClosed;
                    else
                        ++tally.wetPlayed;
                }
            }
            else
            {
                ++g_poseV2FootstepsRefused;
                ++tally.refused;
                ++tally.reasons[error];
                g_poseV2FootstepLastError = error;
                LogPuppetAudioState(*puppet, "footstep", timeMs);
                ++g_poseV2FootstepRefusals[replay::entity::IdText(step.entity) + ":0x" +
                    [&] { char text[12]{}; std::snprintf(text, sizeof(text), "%X", puppet->packet.modelHash); return std::string(text); }() +
                    ":" + error];
            }
        }
    }
}
