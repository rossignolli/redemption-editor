// replay_events_fx_registered.cpp: registered (looped) particle effects: recorded, attached, played back and ended; the fx placement per puppet.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {

    bool IsFireFxSite(std::uint32_t site)
    {
        static const std::uint32_t kFireFxSites[] = {CallReturnRva(kFireAreaFxCall), CallReturnRva(kFireSmoulderFxCall),
            CallReturnRva(kFireEntityFlamesFxCall), CallReturnRva(kFireEntitySmokeFxCall),
            CallReturnRva(kFireLimbFlamesFxCall)};
        return site && std::find(std::begin(kFireFxSites), std::end(kFireFxSites), site) != std::end(kFireFxSites);
    }
    // Engine registered fx owned by a puppet (the bleeding 0x45FAF2C0 and
    // any other) are refused while the replay plays.
    constexpr std::uint32_t kBleedFxHash = 0x45FAF2C0;
    std::uint64_t g_poseV2EngineFxStopCursor = 0;
    std::uint64_t g_poseV2EngineBleedFxStopped = 0;
    std::uint64_t g_poseV2EngineOwnerFxStopped = 0;
    std::uint64_t g_poseV2EngineFxSuppressedAtBegin = 0;
    std::uint32_t g_poseV2EngineFxStopLogs = 0;
    ULONGLONG g_poseV2EngineFxOwnersAt = 0;
    std::uint64_t g_fxEndTick = 0;
    std::uint64_t g_fxNextVeladdWallMs = 0;
    std::uint64_t g_fxNextRecVeladdWallMs = 0;
    void NoteRegReason(std::size_t index, RegNotCreated reason)
    {
        if (index >= g_fxRegReason.size())
            g_fxRegReason.resize(index + 1, RegNotCreated::None);
        if (g_fxRegReason[index] != RegNotCreated::Created)
            g_fxRegReason[index] = reason;
    }
    }
    std::map<std::uint32_t, RegisteredStat> g_poseV2FxRegistered;
    std::uint32_t g_poseV2FxRegisteredSamples = 0;
    namespace
    {
    // Evidence: state of the instances created during the phase, sampled
    // every 250 ms (first 6 keys).
    struct RegisteredWatch
    {
        std::uint64_t key = 0;
        std::uint32_t hash = 0;
        std::uint64_t inst = 0;
        std::uint32_t nextMs = 0;
        std::uint32_t lastSeenMs = 0;
        std::uint32_t createdMs = 0;
        bool lateLogged = false;
    };
    }
    std::uint32_t g_poseV2FxWatchHash = 0; // the fx_watch hash, for late snapshots
    namespace
    {
    std::vector<RegisteredWatch> g_poseV2FxRegisteredWatch;
    }
    std::vector<LiveRegistered> g_poseV2RegisteredLive;
    std::uint64_t g_poseV2FxOwnerLostEnded = 0;
    // The owner check. Recording: an attach
    // of a new tenant of a key's instance address is not the key's.
    // Playback: an attached track whose recorded parent is far from the
    // puppet is not created on it.
    bool g_poseV2RegisteredOwnerCheck = true;
    std::uint64_t g_poseV2FxTenantAttaches = 0;
    std::uint32_t g_poseV2FxTenantLogs = 0;
    // The last creation seen at each instance address in this recording (any
    // key, any effect). A call that finds its instance (created=0) takes any
    // attach record of the address, also one of a previous tenant: without
    // this a ped effect of Arthur rode his hand bone for tracks over 30 s on
    // one record made 26 s before (smoke out of Arthur's hand).
    std::unordered_map<std::uint64_t, std::uint64_t> g_poseV2FxInstCreatedUs;
    namespace
    {
    std::uint64_t PoseV2FxInstCreatedUs(std::uint64_t inst)
    {
        const auto found = g_poseV2FxInstCreatedUs.find(inst);
        return found == g_poseV2FxInstCreatedUs.end() ? 0 : found->second;
    }
    }
    // Evidence caps and counters of the late attach resolution and of the
    // attach timeline (recording and playback).
    std::uint64_t g_poseV2FxAttachTotalSeen = 0;
    std::uint32_t g_poseV2FxLateOwnerLogs = 0;
    std::uint32_t g_poseV2FxLateLostLogs = 0;
    std::uint64_t g_poseV2FxLateResolved = 0;
    std::uint64_t g_poseV2FxLateLost = 0;
    std::uint64_t g_poseV2FxAttachChanges = 0;
    std::uint32_t g_poseV2FxAttachPlayLogs = 0;
    std::uint64_t g_poseV2FxAttachPlayApplied = 0;
    std::uint64_t g_poseV2FxAttachPlayWaiting = 0;
    namespace
    {
    constexpr std::uint32_t kFxLateOwnerMs = 1500;
    constexpr std::uint32_t kFxAttachLogCap = 64;
    }
    std::vector<PendingOneShotOwner> g_poseV2FxPendingOneShot;
    std::uint64_t g_poseV2RegisteredFramesStored = 0;
    std::uint64_t g_poseV2RegisteredSkippedOld = 0; // calls on keys created before the recording
    // Camera ambient calls not recorded, and their tracks of older clips not
    // played (replay::fx::CameraAmbientFxSite).
    std::uint64_t g_poseV2CameraAmbientSkipped = 0;
    std::uint64_t g_poseV2CameraAmbientPlaySkipped = 0;
    // Every creation of a key is its own track and hoof
    // dust makes several a second: Saint Denis made 1412 tracks in 94 s, so
    // the old cap of 4096 refused every new effect after about 4.5 minutes.
    // The cap is now entity::kMaxRegisteredFxTracks (a runaway guard), the
    // byte budget bounds the rest, and the buffer drops the tracks it emptied.
    std::uint64_t g_poseV2RegisteredSplits = 0;
    std::uint64_t g_poseV2RegisteredDropped = 0;      // tracks refused by the cap
    std::uint64_t g_poseV2RegisteredTrimmed = 0;      // emptied tracks the buffer's trim removed
    std::size_t g_poseV2RegisteredMaxFrames = 0;      // the longest track's frames (evidence)
    bool g_poseV2RegisteredCapLogged = false;
    std::uint32_t g_poseV2RegisteredCapLogMs = 0;
    std::uint64_t g_poseV2RegisteredAttachStale = 0;  // attach records older than the instance, ignored
    std::uint64_t g_poseV2RegisteredHeld = 0; // keys alive before record start, taken near the player
    ScriptFxHashlessStats g_scriptFxHashless;
    namespace
    {
    std::deque<std::array<std::uint8_t, 16>> g_poseV2RegisteredKeys;
    }
    std::vector<std::unique_ptr<PlayRegistered>> g_poseV2RegisteredPlay;
    // A track in force whose entity is not ready yet keeps only this (the
    // world object found by model, the reason it waits); its entry of
    // g_poseV2RegisteredPlay is made once the entity passed the ready check,
    // so a pause can never find an entry that was never got.
    std::unordered_map<std::size_t, std::unique_ptr<PlayRegistered>> g_fxPendingEntries;
    namespace
    {
    std::uint64_t g_fxRegTick = 0;           // ticks of PlayPoseV2RegisteredFx
    }
    std::uint64_t g_fxEntryHeldNotReady = 0; // ticks a new entry waited for its entity
    std::uint64_t g_fxHoldNotReady = 0;      // paused ticks an entity was not ready (no call)
    std::uint64_t g_fxHoldSkips = 0;         // paused ticks an instance was not ours and playing (no call)
    std::uint32_t g_fxRecreateLogsByKind[3] = {};               // FX_REG_RECREATE per kind (in span, -, owner not ready)
    std::map<std::uint64_t, std::uint32_t> g_fxRecreateLogsByHash; // and per kind and hash
    namespace
    {
    constexpr std::uint32_t kFxRecreateLogsPerKind = 16;
    constexpr std::uint32_t kFxRecreateLogsPerHash = 4;
    }
    std::unordered_map<std::size_t, FxEarlyRefuse> g_fxEarlyRefuse;
    namespace
    {
    constexpr std::uint64_t kFxWorldRefuseLeaseMs = 1000;
    }
    std::uint64_t g_fxWorldEarlyTicks = 0;    // ticks an object was refused before our instance
    std::uint64_t g_fxWorldRefuseExpired = 0; // leases that ran out
    std::uint32_t g_fxWorldRefuseLogs = 0;

    // The script handle of the entity at an engine address (peds, vehicles,
    // objects), 0 when none exists.
    Entity HandleAtAddress(std::uint64_t address)
    {
        if (!address)
            return 0;
        // The pools say which entity lives at the address and give it a
        // handle (one engine call); the three worldGetAll* listings, a handle
        // for every entity of the world and an address lookup per handle,
        // only without the pool reader.
        bool poolReader = false;
        const Entity pooled = replay::entities::HandleAtAddress(address, poolReader);
        if (poolReader)
            return pooled;
        static int handles[replay::recorder::kPoolCapacity]{};
        const auto scan = [&](int count) -> Entity {
            for (int index = 0; index < count; ++index)
                if (handles[index] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(handles[index])) == address)
                    return handles[index];
            return 0;
        };
        if (const Entity found = scan(worldGetAllObjects(handles, replay::recorder::kPoolCapacity)))
            return found;
        if (const Entity found = scan(worldGetAllPeds(handles, replay::recorder::kPoolCapacity)))
            return found;
        return scan(worldGetAllVehicles(handles, replay::recorder::kPoolCapacity));
    }

    // The nearest live entity of a model within radius of a point.
    Entity FindEntityByModelNear(std::uint32_t model, const float pos[3], float radius)
    {
        static int handles[replay::recorder::kPoolCapacity]{};
        Entity best = 0;
        float bestDistance = radius * radius;
        const auto scan = [&](int count) {
            for (int index = 0; index < count; ++index)
            {
                const Entity entity = handles[index];
                const bool clone = std::any_of(Ctx().puppets.list.begin(), Ctx().puppets.list.end(),
                    [&](const replay::playback::Puppet& puppet) { return puppet.clone == entity; });
                if (!entity || clone || static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(entity)) != model)
                    continue;
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
                const float dx = at.x - pos[0];
                const float dy = at.y - pos[1];
                const float dz = at.z - pos[2];
                const float distance = dx * dx + dy * dy + dz * dz;
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = entity;
                }
            }
        };
        scan(worldGetAllObjects(handles, replay::recorder::kPoolCapacity));
        scan(worldGetAllVehicles(handles, replay::recorder::kPoolCapacity));
        scan(worldGetAllPeds(handles, replay::recorder::kPoolCapacity));
        return best;
    }
    std::uint64_t g_poseV2RegisteredCreated = 0;
    std::uint64_t g_poseV2RegisteredUpdates = 0;
    std::uint64_t g_poseV2RegisteredFailed = 0;
    std::string g_poseV2RegisteredLastError;
    std::uint64_t g_poseV2RegisteredPlaneFailures = 0;
    std::string g_poseV2RegisteredPlaneError;
    std::uint64_t g_poseV2RegisteredAttachMisses = 0;
    std::string g_poseV2RegisteredAttachError;
    namespace
    {

    // The entity id a registered fx track rides at the end of its attach
    // changes (its own attach before any).
    replay::entity::ReplayId PoseV2RegisteredAttachNow(const replay::entity::RegisteredFxTrack& track, std::int32_t& bone)
    {
        if (!track.attachTimeline.empty())
        {
            bone = track.attachTimeline.back().bone;
            return track.attachTimeline.back().id;
        }
        bone = track.attachBone;
        return track.attachId;
    }

    // Every recording tick, for the live keys: (a) an attach entity not
    // recorded yet is looked up again among the recorded peds, weapons and
    // props for kFxLateOwnerMs (the dynamite stick is a prop only after the
    // next object scan); (b) a new attach the engine made for the instance
    // (the attach observer's last record for it) becomes an attach change of
    // the track, so playback moves the effect to the new puppet at that
    // time. An attach record made after the key stopped calling belongs to a
    // new tenant of the address (registered_fx_rule.h AttachBelongsToKey):
    // refused, counted once per record, a few logged.
    bool PoseV2FxTenantAttach(LiveRegistered& live, const replay::entity::RegisteredFxTrack& track, std::uint64_t address,
        std::int32_t bone, std::uint64_t attachUs, std::uint32_t nowMs)
    {
        if (!g_poseV2RegisteredOwnerCheck || replay::regfx::AttachBelongsToKey(live.touched, attachUs, live.lastCallUs))
            return false;
        if (live.tenantAttachUs != attachUs)
        {
            live.tenantAttachUs = attachUs;
            ++g_poseV2FxTenantAttaches;
            if (g_poseV2FxTenantLogs < 12)
            {
                ++g_poseV2FxTenantLogs;
                Ctx().log << "POSE_V2_FX_ATTACH_TENANT,hash=0x" << std::hex << track.effectHash << ",entity=0x" << address
                    << std::dec << ",bone=" << bone << ",after_last_call_us=" << static_cast<long long>(attachUs - live.lastCallUs)
                    << ",track_start_ms=" << track.startMs << ",track_end_ms=" << track.endMs << ",time_ms=" << nowMs << '\n';
            }
        }
        return true;
    }

    void ResolvePoseV2RegisteredAttaches(std::uint32_t nowMs)
    {
        const std::uint64_t attaches = replay::fx::TotalAttaches();
        const bool newAttaches = attaches != g_poseV2FxAttachTotalSeen;
        g_poseV2FxAttachTotalSeen = attaches;
        // The recorded addresses, read once per tick on the first lookup
        // (a lookup per pending track would walk every recorded entity).
        static std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> addresses;
        bool addressesBuilt = false;
        const auto idAt = [&](std::uint64_t address) -> replay::entity::ReplayId {
            if (!addressesBuilt)
            {
                addressesBuilt = true;
                addresses.clear();
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                    addresses.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)), entity.id});
                for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                    if (weapon.object)
                        addresses.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), weapon.id});
                for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                    if (prop.object)
                        addresses.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(prop.object)), prop.id});
                // Train cars (the locomotive's smoke and steam).
                static std::vector<replay::trains::RecordedCarRef> cars;
                replay::trains::RecordedCars(cars);
                for (const replay::trains::RecordedCarRef& car : cars)
                    addresses.push_back({car.address, car.id});
            }
            for (const auto& [known, id] : addresses)
                if (address && known == address)
                    return id;
            return {};
        };
        for (LiveRegistered& live : g_poseV2RegisteredLive)
        {
            replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[live.track];
            // An address created again after this key's instance (another key
            // took it) is a new tenant: its attaches are not ours.
            if (newAttaches && PoseV2FxInstCreatedUs(live.inst) <= live.createdUs)
            {
                std::uint64_t address = 0;
                std::int32_t bone = -1;
                std::uint64_t attachUs = 0;
                if (replay::fx::LastAttachOf(live.inst, address, bone, live.createdUs, &attachUs) && address &&
                    (address != live.attachAddress || bone != live.attachBone) &&
                    !PoseV2FxTenantAttach(live, track, address, bone, attachUs, nowMs))
                {
                    live.attachAddress = address;
                    live.attachBone = bone;
                    live.attachResolved = false;
                    live.attachWaitSinceMs = nowMs;
                }
            }
            if (live.attachResolved || !live.attachAddress)
                continue;
            const replay::entity::ReplayId id = idAt(live.attachAddress);
            if (!id.Valid())
            {
                if (nowMs - live.attachWaitSinceMs <= kFxLateOwnerMs)
                    continue;
                live.attachResolved = true;
                ++g_poseV2FxLateLost;
                // An effect that moved onto an actor the recording does not
                // have ends there, with its owner: played on, it would stay
                // where the recording last saw it while the scene moves on.
                // On a map object it stays (the object stays).
                bool actor = false;
                if (const Entity handle = HandleAtAddress(live.attachAddress))
                    actor = replay::entities::IsPed(handle) || ENTITY::IS_ENTITY_A_VEHICLE(handle);
                if (replay::regfx::EndOnLostOwner(actor))
                {
                    live.ownerLostEnded = true;
                    ++g_poseV2FxOwnerLostEnded;
                    const std::uint32_t at = live.attachWaitSinceMs;
                    track.frames.erase(std::remove_if(track.frames.begin(), track.frames.end(),
                        [&](const replay::entity::RegisteredFxFrame& frame) { return frame.timeMs >= at; }), track.frames.end());
                    track.endMs = track.frames.empty() ? track.startMs : (std::max)(track.startMs, at);
                }
                if (g_poseV2FxLateLostLogs < kFxAttachLogCap / 2)
                {
                    ++g_poseV2FxLateLostLogs;
                    Ctx().log << "POSE_V2_FX_LATE_OWNER,kind=registered,hash=0x" << std::hex << track.effectHash << ",entity=0x"
                        << live.attachAddress << std::dec << ",bone=" << live.attachBone << ",status=lost,actor=" << actor
                        << ",ended=" << live.ownerLostEnded << ",wait_ms=" << (nowMs - live.attachWaitSinceMs)
                        << ",time_ms=" << nowMs << '\n';
                }
                continue;
            }
            live.attachResolved = true;
            std::int32_t currentBone = -1;
            const replay::entity::ReplayId current = PoseV2RegisteredAttachNow(track, currentBone);
            if (current == id && currentBone == live.attachBone)
                continue;
            const bool late = !track.attachId.Valid() && track.attachTimeline.empty();
            // A track that started on an entity not recorded then keeps the
            // id, so playback never looks the model up among the real
            // entities; the change below moves it onto the puppet once
            // that puppet exists.
            if (late)
                track.attachId = id;
            track.attachTimeline.push_back({nowMs, id, live.attachBone});
            ++g_poseV2FxAttachChanges;
            if (late)
                ++g_poseV2FxLateResolved;
            if (g_poseV2FxLateOwnerLogs < kFxAttachLogCap)
            {
                ++g_poseV2FxLateOwnerLogs;
                Ctx().log << (late ? "POSE_V2_FX_LATE_OWNER,kind=registered" : "POSE_V2_FX_ATTACH_CHANGE,phase=record")
                    << ",hash=0x" << std::hex << track.effectHash << ",entity=0x" << live.attachAddress << std::dec
                    << ",id=" << replay::entity::IdText(id) << ",bone=" << live.attachBone
                    << ",from=" << (current.Valid() ? replay::entity::IdText(current) : std::string("none"))
                    << ",status=resolved,wait_ms=" << (nowMs - live.attachWaitSinceMs)
                    << ",changes=" << track.attachTimeline.size() << ",time_ms=" << nowMs << '\n';
            }
        }
        // One-shot evidence: did the skipped entity become recorded in time?
        for (std::size_t index = g_poseV2FxPendingOneShot.size(); index-- > 0;)
        {
            const PendingOneShotOwner pending = g_poseV2FxPendingOneShot[index];
            const replay::entity::ReplayId id = idAt(pending.address);
            const bool expired = nowMs - pending.sinceMs > kFxLateOwnerMs;
            if (!id.Valid() && !expired)
                continue;
            if (g_poseV2FxLateOwnerLogs < kFxAttachLogCap)
            {
                ++g_poseV2FxLateOwnerLogs;
                Ctx().log << "POSE_V2_FX_LATE_OWNER,kind=one_shot,hash=0x" << std::hex << pending.effectHash << ",entity=0x"
                    << pending.address << std::dec << ",id=" << (id.Valid() ? replay::entity::IdText(id) : std::string("none"))
                    << ",status=" << (id.Valid() ? "recorded_later" : "lost") << ",wait_ms=" << (nowMs - pending.sinceMs)
                    << ",time_ms=" << nowMs << '\n';
            }
            g_poseV2FxPendingOneShot.erase(g_poseV2FxPendingOneShot.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    // Recording: every get-or-create call this tick keeps its key alive;
    // a created key opens a track; every live key touched this tick has
    // its instance state read and stored when it changed.
    void RecordPoseV2RegisteredFx(const std::vector<replay::fx::RegisteredCall>& calls)
    {
        REPLAY_PROFILE_ZONE("rec.fx_registered");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (LiveRegistered& live : g_poseV2RegisteredLive)
            live.touched = false;
        for (const replay::fx::RegisteredCall& rawCall : calls)
        {
            // The per-frame get of a script looped effect passes no hash; it
            // keeps its key's effect playing (a dynamite fuse burns for
            // seconds on these calls alone). It counts for the live key it
            // gets, with that key's effect, or with the effect the instance
            // holds when the key is not live yet.
            replay::fx::RegisteredCall resolvedCall;
            const replay::fx::RegisteredCall* use = &rawCall;
            if (!rawCall.effectHash && !rawCall.byReplay)
            {
                ++g_scriptFxHashless.calls;
                if (rawCall.caller == replay::fx::ScriptLoopKeepAliveSite())
                    ++g_scriptFxHashless.keepAliveSite;
                const std::uint64_t rawKey = rawCall.owner + static_cast<std::uint64_t>(rawCall.id);
                const auto liveOfKey = std::find_if(g_poseV2RegisteredLive.begin(), g_poseV2RegisteredLive.end(),
                    [&](const LiveRegistered& entry) { return entry.key == rawKey; });
                const bool liveFound = liveOfKey != g_poseV2RegisteredLive.end() &&
                    liveOfKey->track < Ctx().clip.registeredFx.size();
                // The instance's own effect: read to check a live key, and
                // taken alone only for the script owner key (a script effect
                // started before the recording); other owners' hashless calls
                // stay out unless their key is live.
                replay::fx::InstanceIdentity held;
                const bool identityRead = rawCall.inst && replay::fx::ReadInstanceIdentity(rawCall.inst, held) &&
                    (liveFound || rawCall.owner == replay::scriptfx::kScriptOwner);
                const replay::scriptfx::HashlessResult resolved = replay::scriptfx::ResolveHashless(0, rawCall.inst,
                    liveFound, liveFound ? Ctx().clip.registeredFx[liveOfKey->track].effectHash : 0u, identityRead, held.hash);
                switch (resolved.how)
                {
                case replay::scriptfx::Hashless::LiveKey: ++g_scriptFxHashless.liveKey; break;
                case replay::scriptfx::Hashless::Identity: ++g_scriptFxHashless.identity; break;
                case replay::scriptfx::Hashless::Mismatch: ++g_scriptFxHashless.mismatch; break;
                default: ++g_scriptFxHashless.unresolved; break;
                }
                if (!resolved.Keep())
                    continue;
                resolvedCall = rawCall;
                resolvedCall.effectHash = resolved.hash;
                use = &resolvedCall;
            }
            const replay::fx::RegisteredCall& call = *use;
            if (!call.effectHash || !call.inst)
                continue;
            // A camera ambient effect is placed ahead of whatever camera
            // renders; the engine makes it again ahead of the replay camera,
            // so the gameplay camera's is not part of the clip.
            if (call.caller == replay::fx::CameraAmbientFxSite())
            {
                ++g_poseV2CameraAmbientSkipped;
                continue;
            }
            const std::uint64_t key = call.owner + static_cast<std::uint64_t>(call.id);
            if (call.created)
                g_poseV2FxInstCreatedUs[call.inst] = call.timeUs;
            auto live = std::find_if(g_poseV2RegisteredLive.begin(), g_poseV2RegisteredLive.end(),
                [&](const LiveRegistered& entry) { return entry.key == key; });
            // A new creation on a live key is a new effect: the engine stopped
            // the key's instance (hoof dust 0x4c932de) and made another;
            // folded into one track the playback would keep a single instance
            // alive where the game made a burst each time. The old track ends
            // at its last call, the creation starts its own track.
            if (live != g_poseV2RegisteredLive.end() && call.created && call.inst &&
                !Ctx().clip.registeredFx[live->track].frames.empty())
            {
                ++g_poseV2RegisteredSplits;
                g_poseV2RegisteredLive.erase(live);
                live = g_poseV2RegisteredLive.end();
            }
            if (live == g_poseV2RegisteredLive.end())
            {
                if (!call.created)
                {
                    // A continuous effect already running when the recording
                    // started (a campfire, smoke still in the air): it is part
                    // of the state the recording starts with when it is near
                    // the player (60 m); far ambient effects stay out.
                    replay::fx::InstanceState held;
                    bool nearPlayer = false;
                    if (replay::fx::ReadInstanceState(call.inst, held))
                    {
                        float world[16] = {};
                        replay::entity::ComposeFxWorld(held.matrix, held.parent, world);
                        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                        const float dx = world[12] - player.x;
                        const float dy = world[13] - player.y;
                        const float dz = world[14] - player.z;
                        nearPlayer = dx * dx + dy * dy + dz * dz <= 60.0f * 60.0f;
                    }
                    if (!nearPlayer)
                    {
                        ++g_poseV2RegisteredSkippedOld;
                        continue;
                    }
                    ++g_poseV2RegisteredHeld;
                }
                if (Ctx().clip.registeredFx.size() >= replay::entity::kMaxRegisteredFxTracks)
                {
                    // With Debug=0 too (_SUMMARY): the first refusal and every 10 s while it lasts.
                    ++g_poseV2RegisteredDropped;
                    if (!g_poseV2RegisteredCapLogged || nowMs >= g_poseV2RegisteredCapLogMs + 10000)
                    {
                        g_poseV2RegisteredCapLogged = true;
                        g_poseV2RegisteredCapLogMs = nowMs;
                        Ctx().log << "POSE_V2_REGISTERED_FX_CAP_SUMMARY,event=full,tracks=" << Ctx().clip.registeredFx.size()
                                  << ",cap=" << replay::entity::kMaxRegisteredFxTracks << ",refused_tracks=" << g_poseV2RegisteredDropped
                                  << ",trimmed_tracks=" << g_poseV2RegisteredTrimmed << ",hash=0x" << std::hex << call.effectHash
                                  << std::dec << ",time_ms=" << nowMs << '\n';
                    }
                    continue;
                }
                replay::entity::RegisteredFxTrack track;
                track.effectHash = call.effectHash;
                track.flag = call.flag;
                track.arg8 = call.arg8;
                track.arg9 = call.arg9;
                track.callerSite = call.caller;
                track.startMs = nowMs;
                track.endMs = nowMs;
                std::uint64_t attachedTo = 0; // kept on the live key
                std::int32_t attachedBone = -1;
                {
                    std::uint64_t entityAddress = 0;
                    std::int32_t bone = -1;
                    std::uint64_t attachUs = 0;
                    // An attach counts only when made at or after this
                    // instance's creation. The engine pools instances: a record
                    // for a previous tenant of the address would attach bullet
                    // tracers to the player. A key alive before the recording
                    // keeps the latest record. A found instance (created=0)
                    // created earlier in the recording counts from that
                    // creation too.
                    const std::uint64_t sinceUs = call.created ? call.timeUs : PoseV2FxInstCreatedUs(call.inst);
                    std::uint64_t anyEntity = 0;
                    std::int32_t anyBone = -1;
                    std::uint64_t anyUs = 0;
                    const bool anyAttach = replay::fx::LastAttachOf(call.inst, anyEntity, anyBone, 0, &anyUs);
                    if (replay::fx::LastAttachOf(call.inst, entityAddress, bone, sinceUs, &attachUs) && entityAddress)
                    {
                        track.attached = true;
                        track.attachBone = bone;
                        attachedTo = entityAddress;
                        attachedBone = bone;
                        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                            if (reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == entityAddress)
                                track.attachId = entity.id;
                        // Weapons and props are recorded entities too. The
                        // barrel smoke (0xafac4b7b) is attached to the weapon
                        // object (bone 23); without its id playback would look
                        // the model up near the spot, skip the puppets and find
                        // the real player's gun. With the id it rides the
                        // weapon puppet.
                        if (!track.attachId.Valid())
                            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                                if (weapon.object && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)) == entityAddress)
                                    track.attachId = weapon.id;
                        if (!track.attachId.Valid())
                            for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                                if (prop.object && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(prop.object)) == entityAddress)
                                    track.attachId = prop.id;
                        if (!track.attachId.Valid()) // a train car (chimney smoke, steam)
                            track.attachId = replay::trains::RecordedIdAt(entityAddress);
                        if (const Entity handle = HandleAtAddress(entityAddress))
                        {
                            track.attachModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(handle));
                            const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
                            track.attachPos[0] = at.x;
                            track.attachPos[1] = at.y;
                            track.attachPos[2] = at.z;
                        }
                        Ctx().log << "POSE_V2_REGISTERED_FX_ATTACH,hash=0x" << std::hex << call.effectHash << ",entity=0x" << entityAddress
                            << ",model=0x" << track.attachModel << std::dec << ",bone=" << bone << ",id=" << replay::entity::IdText(track.attachId)
                            << ",pos=" << track.attachPos[0] << ";" << track.attachPos[1] << ";" << track.attachPos[2]
                            << ",created=" << int(call.created) << ",dt_us=" << static_cast<long long>(attachUs - call.timeUs)
                            << ",time_ms=" << nowMs << '\n';
                    }
                    else if (anyAttach && anyEntity)
                    {
                        // Evidence: the record this track would take without the creation-time rule.
                        ++g_poseV2RegisteredAttachStale;
                        if (g_poseV2RegisteredAttachStale <= 64)
                            Ctx().log << "POSE_V2_REGISTERED_FX_ATTACH_STALE,hash=0x" << std::hex << call.effectHash << ",entity=0x" << anyEntity
                                << std::dec << ",bone=" << anyBone << ",age_us=" << static_cast<long long>(call.timeUs - anyUs)
                                << ",caller=0x" << std::hex << call.caller << std::dec << ",time_ms=" << nowMs << '\n';
                    }
                }
                if (!call.created) // a key found alive is an effect the recording saw too
                    g_fxCheck.NoteRecord(call.effectHash, replay::fxcheck::Stored::Registered, true, call.caller);
                Ctx().clip.registeredFx.push_back(track);
                g_poseV2RegisteredLive.push_back({key, call.inst, Ctx().clip.registeredFx.size() - 1, nowMs, true});
                {
                    // The attach this track starts with; an entity not recorded
                    // yet is looked up again every tick for a while
                    // (ResolvePoseV2RegisteredAttaches).
                    LiveRegistered& added = g_poseV2RegisteredLive.back();
                    added.createdUs = call.created ? call.timeUs : PoseV2FxInstCreatedUs(call.inst);
                    added.lastCallUs = call.timeUs;
                    added.attachAddress = attachedTo;
                    added.attachBone = attachedBone;
                    added.attachResolved = !attachedTo || track.attachId.Valid();
                    added.attachWaitSinceMs = nowMs;
                }
                continue;
            }
            // The key was not called for at least one recording tick: the
            // registry stopped its instance softly and restarts it now
            // (its soft stop, then its restart); the stretch is kept (RGSS) so the
            // playback leaves the same hole.
            if (!live->touched && live->lastCallMs < g_poseV2RegisteredPrevTickMs && nowMs > live->lastCallMs)
            {
                replay::entity::RegisteredFxTrack& gapped = Ctx().clip.registeredFx[live->track];
                g_fxCheck.NoteGap(gapped.effectHash, true);
                if (gapped.callGaps.size() < 65536 &&
                    (gapped.callGaps.empty() || live->lastCallMs >= gapped.callGaps.back().toMs))
                    gapped.callGaps.push_back({live->lastCallMs, nowMs});
            }
            // The key's own new instance: its attaches count from now.
            if (call.created || call.inst != live->inst)
                live->createdUs = PoseV2FxInstCreatedUs(call.inst);
            live->inst = call.inst;
            live->lastCallMs = nowMs;
            live->lastCallUs = (std::max)(live->lastCallUs, call.timeUs);
            live->touched = true;
        }
        ResolvePoseV2RegisteredAttaches(nowMs);
        // The inherited velocity every 100 ms.
        const std::uint64_t wallMs = GetTickCount64();
        const bool sampleVeladd = wallMs >= g_fxNextRecVeladdWallMs;
        if (sampleVeladd)
            g_fxNextRecVeladdWallMs = wallMs + 100;
        for (LiveRegistered& live : g_poseV2RegisteredLive)
        {
            g_fxCheck.NoteCall(Ctx().clip.registeredFx[live.track].effectHash, true, live.touched);
            if (!live.touched || live.ownerLostEnded)
                continue;
            replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[live.track];
            replay::fx::InstanceState state;
            if (!replay::fx::ReadInstanceState(live.inst, state))
                continue;
            track.endMs = nowMs;
            // The settings the site wrote, kept when they change (RGSS); the
            // motion bits and the inherited velocity of the game's instance
            // on a moving owner.
            if (track.settings.size() < 256 && state.settings.mask &&
                (track.settings.empty() ||
                    replay::entity::FxSettingsDiff(track.settings.back().settings, state.settings,
                        replay::entity::FxSettings::kApplied & ~replay::entity::FxSettings::kVel350))) // +0x350 is in every frame (aux350)
                track.settings.push_back({nowMs, state.settings});
            if (track.attached)
            {
                g_fxCheck.NoteMotion(track.effectHash, true, state.flags2);
                if (sampleVeladd)
                {
                    const float* v = state.veladd360;
                    g_fxCheck.NoteVeladd(track.effectHash, true, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 0.0f);
                }
            }
            const bool same = !track.frames.empty() &&
                std::memcmp(track.frames.back().matrix, state.matrix, sizeof(state.matrix)) == 0 &&
                std::memcmp(track.frames.back().parent, state.parent, sizeof(state.parent)) == 0 &&
                std::memcmp(track.frames.back().evolutions, state.evolutions, sizeof(state.evolutions)) == 0 &&
                track.frames.back().zoom == state.zoom &&
                track.frames.back().flags == state.flags &&
                track.frames.back().planeCount == state.planeCount &&
                std::memcmp(track.frames.back().planes, state.planes, state.planeCount * 12 * sizeof(float)) == 0 &&
                std::memcmp(track.frames.back().aux350, state.aux350, sizeof(state.aux350)) == 0;
            if (same)
                continue;
            replay::entity::RegisteredFxFrame frame;
            frame.timeMs = nowMs;
            std::memcpy(frame.matrix, state.matrix, sizeof(frame.matrix));
            std::memcpy(frame.parent, state.parent, sizeof(frame.parent));
            std::memcpy(frame.evolutions, state.evolutions, sizeof(frame.evolutions));
            frame.zoom = state.zoom;
            frame.flags = state.flags;
            frame.planeCount = state.planeCount;
            std::memcpy(frame.planes, state.planes, sizeof(frame.planes));
            std::memcpy(frame.aux350, state.aux350, sizeof(frame.aux350));
            std::memcpy(frame.start310, state.start310, sizeof(frame.start310));
            frame.farClip = state.farClip3C4;
            track.frames.push_back(frame);
            ++g_poseV2RegisteredFramesStored;
            g_poseV2RegisteredMaxFrames = (std::max)(g_poseV2RegisteredMaxFrames, track.frames.size());
        }
        // Keys not called for a while are over.
        g_poseV2RegisteredLive.erase(std::remove_if(g_poseV2RegisteredLive.begin(), g_poseV2RegisteredLive.end(),
            [&](const LiveRegistered& entry) { return nowMs > entry.lastCallMs + 250; }), g_poseV2RegisteredLive.end());
        g_poseV2RegisteredPrevTickMs = nowMs;
    }
    }

    // Playback, every playback tick (paused too, so continuous effects stay
    // alive): every track in force at the playhead gets or creates its
    // instance by our own key and receives the frame state; a track past its
    // end is simply not called and the engine drops it.
    std::uint32_t g_poseV2RegisteredLastPlayhead = 0;
    std::uint64_t g_poseV2RegisteredCrossed = 0;
    std::vector<EndedRegistered> g_poseV2RegisteredEnded;
    std::uint32_t g_poseV2RegisteredAfterEndLogged = 0;
    namespace
    {
    constexpr std::uint32_t kRegisteredAfterEndLogs = 12;
    constexpr std::size_t kRegisteredEndedWatch = 256;
    }
    RegisteredEndStats g_poseV2RegisteredEnd;
    namespace
    {

    replay::regfx::Identity PoseV2RegisteredIdentity(const replay::fx::InstanceIdentity& read)
    {
        replay::regfx::Identity identity;
        identity.rule = read.rule;
        identity.hash = read.hash;
        identity.state = read.state;
        for (int n = 0; n < 3; ++n)
        {
            identity.position[n] = read.position[n];
            identity.offset[n] = read.offset[n];
        }
        return identity;
    }

    bool PoseV2InstHeldByLiveTrack(std::uint64_t inst)
    {
        for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
            if (entry->inst == inst)
                return true;
        return false;
    }
    }

    // A track's instance leaves the replay (its track ended, a scrub started,
    // the entity it rides is gone): stopped when it is still ours and
    // playing (immediate: killed, as on a cursor jump), then watched for one
    // second when its track simply ended.
    void EndPoseV2RegisteredEntry(const PlayRegistered& entry, const replay::entity::RegisteredFxTrack& track,
        std::uint32_t timeMs, bool immediate, bool watch)
    {
        if (!entry.inst)
            return;
        ++g_poseV2RegisteredEnd.ended;
        replay::fx::InstanceIdentity read;
        const bool readable = replay::fx::ReadInstanceIdentity(entry.inst, read);
        // Inside a stretch without a call the registry already stopped the
        // entry softly (its soft stop) and may have freed it; the pointer can be
        // a new tenant of the same effect (the hoof dust of another horse),
        // which StopAllowed (rule, hash, playing) accepts. Not calling the
        // key ends it the engine's way.
        if (entry.inGap)
        {
            ++g_fxGapEndSkipped;
            ++g_poseV2RegisteredEnd.stopNotOurs;
        }
        else if (entry.identityKnown && replay::regfx::StopAllowed(readable, entry.identity, PoseV2RegisteredIdentity(read)))
        {
            std::string error;
            if (replay::fx::StopInstance(entry.inst, immediate, error))
                ++g_poseV2RegisteredEnd.stopped;
            else
            {
                ++g_poseV2RegisteredEnd.stopFailed;
                g_poseV2RegisteredEnd.lastStopError = error;
            }
        }
        else
            ++g_poseV2RegisteredEnd.stopNotOurs;
        if (!watch || g_poseV2RegisteredEnded.size() >= kRegisteredEndedWatch)
            return;
        EndedRegistered ended;
        ended.inst = entry.inst;
        ended.hash = track.effectHash;
        ended.endMs = timeMs;
        ended.attached = track.attached;
        replay::fx::InstanceIdentity after;
        if (entry.identityKnown && replay::fx::ReadInstanceIdentity(entry.inst, after))
        {
            ended.identity = PoseV2RegisteredIdentity(after);
            ended.identityKnown = true;
        }
        g_poseV2RegisteredEnded.push_back(ended);
    }
    namespace
    {

    // Whether the entity a registered track rides at timeMs is in the
    // replay: its puppet (by id), or for a track of no recorded id the world
    // object it was on (looked up only when `search` is set: the model
    // search is not done every tick). A track that is not attached has none.
    bool PoseV2RegisteredOwnerPresent(const replay::entity::RegisteredFxTrack& track, std::uint32_t timeMs, bool search)
    {
        if (!track.attached && track.attachTimeline.empty())
            return true;
        const replay::entity::RegisteredFxAttach* change =
            track.attachTimeline.empty() ? nullptr : replay::entity::RegisteredFxAttachAt(track, timeMs);
        const replay::entity::ReplayId id = change ? change->id : track.attachId;
        if (id.Valid())
        {
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(id);
            return puppet && puppet->clone && replay::entities::Exists(puppet->clone);
        }
        if (change || !track.attached)
            return true; // a change to no recorded entity: nothing to follow
        if (!search)
            return true;
        return track.attachModel && FindEntityByModelNear(track.attachModel, track.attachPos, 3.0f) != 0;
    }
    }
    // The puppet a track would ride at timeMs is where the recorded frame's
    // parent (the bone it rode) was (registered_fx_rule.h ParentAtOwner). A
    // world track before any change and an owner with no puppet are not
    // decided here.
    std::uint32_t g_poseV2RegisteredMismatchLogs = 0;
    namespace
    {
    bool PoseV2RegisteredParentAtOwner(const replay::entity::RegisteredFxTrack& track,
        const replay::entity::RegisteredFxFrame& frame, std::uint32_t timeMs)
    {
        const replay::entity::RegisteredFxAttach* change =
            track.attachTimeline.empty() ? nullptr : replay::entity::RegisteredFxAttachAt(track, timeMs);
        if (!change && !track.attached)
            return true;
        const replay::entity::ReplayId id = change ? change->id : track.attachId;
        if (!id.Valid())
            return true;
        const replay::playback::Puppet* puppet = Ctx().puppets.Find(id);
        if (!puppet || !puppet->clone || !replay::entities::Exists(puppet->clone))
            return true;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
        const float owner[3] = {at.x, at.y, at.z};
        if (replay::regfx::ParentAtOwner(frame.parent, owner))
            return true;
        if (g_poseV2RegisteredMismatchLogs < 12)
        {
            ++g_poseV2RegisteredMismatchLogs;
            const float dx = frame.parent[12] - owner[0];
            const float dy = frame.parent[13] - owner[1];
            const float dz = frame.parent[14] - owner[2];
            Ctx().log << "POSE_V2_FX_REGISTERED_OWNER_MISMATCH,hash=0x" << std::hex << track.effectHash << std::dec
                << ",id=" << replay::entity::IdText(id) << ",bone=" << (change ? change->bone : track.attachBone)
                << ",distance_m=" << std::sqrt(dx * dx + dy * dy + dz * dz)
                << ",parent_pos=" << frame.parent[12] << ";" << frame.parent[13] << ";" << frame.parent[14]
                << ",owner_pos=" << owner[0] << ";" << owner[1] << ";" << owner[2]
                << ",start_ms=" << track.startMs << ",end_ms=" << track.endMs << ",playhead_ms=" << timeMs << '\n';
        }
        return false;
    }
    }
    // A track whose world object was not found is searched again at most
    // twice a second (the model search walks the object pool).
    std::unordered_map<std::size_t, std::uint32_t> g_poseV2RegisteredOwnerRetryMs;
    namespace
    {

    void LogPoseV2RegisteredAfterEnd(std::uint32_t timeMs)
    {
        for (std::size_t index = 0; index < g_poseV2RegisteredEnded.size();)
        {
            const EndedRegistered& ended = g_poseV2RegisteredEnded[index];
            if (timeMs < ended.endMs) // the playhead went back: the sample is meaningless
            {
                g_poseV2RegisteredEnded.erase(g_poseV2RegisteredEnded.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (timeMs < ended.endMs + 1000)
            {
                ++index;
                continue;
            }
            // Classified with an identity check (a reused address is not
            // counted as ours).
            const char* verdict = "unknown";
            if (ended.identityKnown)
            {
                replay::fx::InstanceIdentity read;
                const bool readable = replay::fx::ReadInstanceIdentity(ended.inst, read);
                switch (replay::regfx::ClassifyAfterEnd(readable, ended.identity, PoseV2RegisteredIdentity(read),
                    PoseV2InstHeldByLiveTrack(ended.inst), ended.attached)) // attached by its offset
                {
                case replay::regfx::AfterEnd::AliveOurs: ++g_poseV2RegisteredEnd.afterAlive; verdict = "alive_ours"; break;
                case replay::regfx::AfterEnd::Finishing: ++g_poseV2RegisteredEnd.afterFinishing; verdict = "finishing"; break;
                case replay::regfx::AfterEnd::Reused: ++g_poseV2RegisteredEnd.afterReused; verdict = "reused"; break;
                case replay::regfx::AfterEnd::Unreadable: ++g_poseV2RegisteredEnd.afterUnreadable; verdict = "unreadable"; break;
                }
            }
            if (g_poseV2RegisteredAfterEndLogged < kRegisteredAfterEndLogs)
            {
                ++g_poseV2RegisteredAfterEndLogged;
                replay::fx::InstanceState state;
                const bool readable = replay::fx::ReadInstanceState(ended.inst, state);
                Ctx().log << "POSE_V2_FX_REGISTERED_AFTER_END,hash=0x" << std::hex << ended.hash << std::dec
                    << ",verdict=" << verdict
                    << ",attached=" << ended.attached << ",end_ms=" << ended.endMs << ",playhead_ms=" << timeMs
                    << ",readable=" << readable << ",state=" << state.state << std::hex << ",flags=0x" << state.flags
                    << ",flags2=0x" << state.flags2 << std::dec << ",loops=" << state.loops << ",loop_t=" << state.loopTime
                    << ",pos=" << state.matrix[12] << ";" << state.matrix[13] << ";" << state.matrix[14]
                    << ",parent_pos=" << state.parent[12] << ";" << state.parent[13] << ";" << state.parent[14] << '\n';
            }
            g_poseV2RegisteredEnded.erase(g_poseV2RegisteredEnded.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    }

    // While the replay plays, the engine's own registered effects owned by a
    // puppet (its bleeding 0x45FAF2C0, and any other: the replay plays the
    // recorded ones with its own keys) are refused in the core observer
    // (fx_observer.h SetSuppressedOwners), as the engine's own decals on the
    // world are removed during playback. The owner list is the puppet
    // clones, refreshed every 250 ms; evidence counters and capped
    // POSE_V2_ENGINE_FX_STOPPED lines from the observer ring.
    void BeginPoseV2EngineFxStop()
    {
        g_poseV2EngineFxStopCursor = replay::fx::TotalRegistered();
        g_poseV2EngineBleedFxStopped = 0;
        g_poseV2EngineOwnerFxStopped = 0;
        g_poseV2EngineFxSuppressedAtBegin = replay::fx::TotalSuppressed();
        g_poseV2EngineFxStopLogs = 0;
        g_poseV2EngineFxOwnersAt = 0;
        g_poseV2EngineFxOwnerCount = 0;
        replay::fx::SetSuppressedOwners({});
    }

    void StopPoseV2EngineFx(std::uint32_t timeMs)
    {
        if (!g_poseV2FxReplayEnabled)
        {
            if (g_poseV2EngineFxOwnerCount)
            {
                replay::fx::SetSuppressedOwners({});
                g_poseV2EngineFxOwnerCount = 0;
            }
            return;
        }
        const ULONGLONG now = GetTickCount64();
        // A puppet made in this tick enters the list
        // now, not up to 250 ms later (its own effects would be made and
        // refused only after: a visible blink).
        static std::vector<std::uint64_t> lastWorldOwners;
        std::size_t clones = 0;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            clones += puppet.clone ? 1 : 0;
        const bool immediate = clones != g_fxLastPuppetClones || g_fxWorldOwnersLive != lastWorldOwners;
        g_fxLastPuppetClones = clones;
        if (immediate && now - g_poseV2EngineFxOwnersAt < 250)
            ++g_fxOwnerListImmediate;
        if (immediate || now - g_poseV2EngineFxOwnersAt >= 250)
        {
            g_poseV2EngineFxOwnersAt = now;
            std::vector<std::uint64_t> owners;
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                if (puppet.clone && replay::entities::Exists(puppet.clone))
                    if (const auto* address = replay::entities::AddressOf(puppet.clone))
                        owners.push_back(reinterpret_cast<std::uint64_t>(address));
            // The hidden player too (moved to the focus puppet, in the horse
            // when the main character rides): his own effects are not the
            // replay's.
            PoseV2HiddenPlayerEntities(owners);
            // A map object one of our live tracks rides (the chimney smoke): the
            // engine's own effect on it is the same smoke a second time; refused
            // while ours lives (only then: ours gone, the engine's comes back).
            for (const std::uint64_t address : g_fxWorldOwnersLive)
                owners.push_back(address);
            lastWorldOwners = g_fxWorldOwnersLive;
            replay::fx::SetSuppressedOwners(owners);
            g_poseV2EngineFxOwnerCount = owners.size();
        }
        static std::vector<replay::fx::RegisteredCall> calls;
        replay::fx::CollectRegisteredSince(g_poseV2EngineFxStopCursor, calls);
        for (const replay::fx::RegisteredCall& call : calls)
        {
            if (!call.suppressed)
                continue;
            ++g_poseV2EngineOwnerFxStopped;
            if (call.effectHash == kBleedFxHash)
                ++g_poseV2EngineBleedFxStopped;
            if (std::find(g_fxWorldOwnersLive.begin(), g_fxWorldOwnersLive.end(), call.owner) != g_fxWorldOwnersLive.end())
                ++g_fxWorldOwnerRefusals;
            if (g_poseV2EngineFxStopLogs < 16)
            {
                ++g_poseV2EngineFxStopLogs;
                Ctx().log << "POSE_V2_ENGINE_FX_STOPPED,playhead_ms=" << timeMs << std::hex << ",hash=0x" << call.effectHash
                    << ",owner=0x" << call.owner << ",caller=0x" << call.caller << std::dec << ",id=" << call.id
                    << ",flag=" << int(call.flag) << ",bleed=" << (call.effectHash == kBleedFxHash) << '\n';
            }
        }
    }

    void EndPoseV2EngineFxStop(std::ostream& log)
    {
        replay::fx::SetSuppressedOwners({});
        log << "POSE_V2_PLAYBACK_ENGINE_FX_STOPPED,engine_bleed_fx_stopped=" << g_poseV2EngineBleedFxStopped
            << ",engine_owner_fx_stopped=" << g_poseV2EngineOwnerFxStopped
            << ",suppressed_calls=" << (replay::fx::TotalSuppressed() - g_poseV2EngineFxSuppressedAtBegin)
            << ",owners=" << g_poseV2EngineFxOwnerCount << '\n';
        g_poseV2EngineFxOwnerCount = 0;
    }
    namespace
    {

    // The entity a registered track rides at timeMs (its puppet, or the world
    // object found by model and kept for 500 ms on the entry), 0 when none;
    // kind: puppet, world or none.
    Entity PoseV2RegisteredOwnerEntity(const replay::entity::RegisteredFxTrack& track, std::uint32_t timeMs,
        PlayRegistered& entry, const char*& kind)
    {
        kind = "none";
        if (!track.attached && track.attachTimeline.empty())
            return 0;
        const replay::entity::RegisteredFxAttach* change =
            track.attachTimeline.empty() ? nullptr : replay::entity::RegisteredFxAttachAt(track, timeMs);
        const replay::entity::ReplayId id = change ? change->id : track.attachId;
        if (id.Valid())
        {
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(id);
            kind = "puppet";
            return puppet && puppet->clone ? puppet->clone : 0;
        }
        if (change || !track.attachModel)
            return 0;
        kind = "world";
        const std::uint64_t wall = GetTickCount64();
        if (!entry.worldTarget || wall >= entry.worldTargetUntilWallMs || !replay::entities::Exists(entry.worldTarget))
        {
            entry.worldTarget = FindEntityByModelNear(track.attachModel, track.attachPos, 3.0f);
            entry.worldTargetAddress = entry.worldTarget ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entry.worldTarget)) : 0;
            entry.worldTargetUntilWallMs = wall + 500;
        }
        return entry.worldTarget;
    }

    // What the attach update tests before it frees an entry: the
    // entity there, its draw data [[e+0x28]+8] (vfunc +0xA8), a
    // matrix with an axis (the engine tests the bone matrix's X axis; the
    // entity's forward vector here [hypothesis: the same zero before the
    // entity is set up]).
    struct PoseV2OwnerReadiness
    {
        bool exists = false;
        bool draw = false;
        bool matZero = true;
        bool ready = false;
    };
    PoseV2OwnerReadiness PoseV2FxOwnerReadiness(Entity entity)
    {
        PoseV2OwnerReadiness out;
        out.exists = entity && replay::entities::Exists(entity);
        if (!out.exists)
            return out;
        bool present = false;
        replay::fx::ReadEntityDrawData(replay::entities::AddressOf(entity), present);
        out.draw = present;
        const Vector3 forward = ENTITY::GET_ENTITY_FORWARD_VECTOR(entity);
        const float axis[3] = {forward.x, forward.y, forward.z};
        out.matZero = axis[0] == 0.0f && axis[1] == 0.0f && axis[2] == 0.0f;
        out.ready = replay::fxmotion::OwnerReady(out.exists, out.draw, axis);
        return out;
    }

    // The hoof site writes +0x3A8 = 1.0 on every call; a clip
    // without RGSS gets that constant for its tracks.
    const replay::entity::FxSettings& LegacyHoofSettings()
    {
        static const replay::entity::FxSettings settings = [] {
            replay::entity::FxSettings s;
            s.mask = replay::entity::FxSettings::kTele3A8;
            s.tele3A8 = replay::fxmotion::kHoofSiteTeleportM;
            return s;
        }();
        return settings;
    }
    }

    void PlayPoseV2RegisteredFx(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.fx_registered");
        if (!g_poseV2FxReplayEnabled)
            return;
        struct LastPlayhead
        {
            std::uint32_t now;
            ~LastPlayhead() { g_poseV2RegisteredLastPlayhead = now; }
        } lastPlayhead{timeMs};
        LogPoseV2RegisteredAfterEnd(timeMs);
        // A scrub kills the live instances and creates none (a fast scrub
        // would draw the hoof and body dust as trails); a pause keeps them.
        const replay::regfx::Mode mode =
            replay::regfx::PlayMode(Ctx().hooks->NormalPlay(), Ctx().transport.Scrubbing());
        // A new stretch of play on a clear, a jump back, a step past the
        // forward cursor's limit, or a step forward while paused, as the fx
        // check's spans split there (TallyPoseV2FxCheckPlay extends a span
        // only in normal play), so a frame step does not leave a play
        // uncounted.
        if (mode == replay::regfx::Mode::Clear || timeMs < g_poseV2RegisteredLastPlayhead ||
            timeMs - g_poseV2RegisteredLastPlayhead > replay::entity::ForwardEventCursor::kMaxStepMs ||
            (mode == replay::regfx::Mode::Hold && timeMs != g_poseV2RegisteredLastPlayhead))
            ++g_fxSpanId;
        // Paused while the replay owns the world, the vfx update gets a step
        // of 0 (FxPauseFreeze).
        replay::fx::SetPauseFreeze(g_fxCfg.pauseFreeze && g_fxPauseFreezeReady && mode == replay::regfx::Mode::Hold &&
            replay::theatre::Owning());
        if (g_fxTrackCreatedSpan.size() < Ctx().clip.registeredFx.size())
            g_fxTrackCreatedSpan.resize(Ctx().clip.registeredFx.size(), 0);
        if (g_fxTrackPlaySpan.size() < Ctx().clip.registeredFx.size())
            g_fxTrackPlaySpan.resize(Ctx().clip.registeredFx.size(), 0);
        const std::uint64_t wallMs = GetTickCount64();
        ++g_fxRegTick;
        std::unordered_map<Entity, PoseV2OwnerReadiness> readiness; // one read per owner per tick
        if (mode == replay::regfx::Mode::Clear)
        {
            for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
            {
                if (entry->track < Ctx().clip.registeredFx.size())
                    EndPoseV2RegisteredEntry(*entry, Ctx().clip.registeredFx[entry->track], timeMs, true, false);
                ++g_poseV2RegisteredEnd.scrubKilled;
            }
            g_poseV2RegisteredPlay.clear();
            g_fxPendingEntries.clear();
            return;
        }
        if (g_scriptLoopPlay.size() < Ctx().clip.registeredFx.size())
            g_scriptLoopPlay.resize(Ctx().clip.registeredFx.size());
        // The tracks with an entry, so the walk over every track (no longer
        // capped at 4096) looks an entry up only where there is one. An entry
        // is added or removed only for the track being walked.
        static std::vector<std::uint8_t> withEntry;
        withEntry.assign(Ctx().clip.registeredFx.size(), 0);
        for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
            if (entry->track < withEntry.size())
                withEntry[entry->track] = 1;
        for (std::size_t index = 0; index < Ctx().clip.registeredFx.size(); ++index)
        {
            const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[index];
            // Normal play past a script looped track's end after it was
            // entered.
            if (mode == replay::regfx::Mode::Normal && ScriptLoopSite(track.callerSite) && g_scriptLoopPlay[index].entered &&
                timeMs > track.endMs)
                g_scriptLoopPlay[index].passedEnd = true;
            if (g_poseV2FirePlaying && IsFireFxSite(track.callerSite))
            {
                if (track.startMs <= timeMs && timeMs <= track.endMs)
                    NoteRegReason(index, RegNotCreated::FireSite);
                continue;
            }
            // An older clip keeps the gameplay camera's ambient effect
            // (CameraAmbientFxSite): played, it would follow the recorded
            // camera path, a few metres ahead of where that camera was.
            if (track.callerSite == replay::fx::CameraAmbientFxSite())
            {
                if (track.startMs <= timeMs && timeMs <= track.endMs && g_poseV2RegisteredLastPlayhead != timeMs)
                    ++g_poseV2CameraAmbientPlaySkipped;
                NoteRegReason(index, RegNotCreated::CameraAmbient);
                continue;
            }
            // A shell track a WFXI request plays (the engine's own registered
            // get on the puppet's CWeapon).
            if (g_wfxPlayOn && g_wfxDropTrack.size() == Ctx().clip.registeredFx.size() && g_wfxDropTrack[index])
            {
                if (track.startMs <= timeMs && timeMs <= track.endMs && g_poseV2RegisteredLastPlayhead != timeMs)
                    ++g_wfxDroppedTracksPlayed;
                NoteRegReason(index, RegNotCreated::WeaponIntent);
                continue;
            }
            const replay::entity::RegisteredFxFrame* frame = replay::entity::RegisteredFxFrameAt(track, timeMs);
            // A track shorter than a frame (a bullet tracer: one call, most of
            // them) is due when the playhead crosses its start in normal
            // forward play; the exact-ms lookup never lands on it.
            if (!frame && !track.frames.empty() && Ctx().hooks->NormalPlay() &&
                g_poseV2RegisteredLastPlayhead < track.frames.front().timeMs && track.frames.front().timeMs <= timeMs &&
                timeMs - g_poseV2RegisteredLastPlayhead <= replay::entity::ForwardEventCursor::kMaxStepMs)
            {
                frame = &track.frames.front();
                ++g_poseV2RegisteredCrossed;
            }
            auto play = !withEntry[index] ? g_poseV2RegisteredPlay.end()
                : std::find_if(g_poseV2RegisteredPlay.begin(), g_poseV2RegisteredPlay.end(),
                    [&](const std::unique_ptr<PlayRegistered>& entry) { return entry->track == index; });
            // The stretch without a call the playhead is in (or stepped over),
            // and what this tick does there.
            static const std::vector<replay::entity::RegisteredFxGap> kNoGapsHere;
            const std::vector<replay::entity::RegisteredFxGap>& gapsHere = !track.callGaps.empty() ? track.callGaps
                : index < g_fxDerivedGaps.size() ? g_fxDerivedGaps[index] : kNoGapsHere;
            const int gapHere = frame && !gapsHere.empty()
                ? replay::fxmotion::GapAt(gapsHere, timeMs, g_poseV2RegisteredLastPlayhead) : -1;
            const auto noteGap = [&](replay::fxmotion::GapSeen seen) {
                if (gapHere < 0)
                    return;
                if (g_fxGapSeen.size() <= index)
                    g_fxGapSeen.resize(Ctx().clip.registeredFx.size());
                std::vector<std::uint8_t>& seenOf = g_fxGapSeen[index];
                if (seenOf.size() < gapsHere.size())
                    seenOf.resize(gapsHere.size(), 0);
                seenOf[static_cast<std::size_t>(gapHere)] = static_cast<std::uint8_t>(replay::fxmotion::MergeGapSeen(
                    static_cast<replay::fxmotion::GapSeen>(seenOf[static_cast<std::size_t>(gapHere)]), seen));
            };
            if (!frame)
            {
                g_fxPendingEntries.erase(index);
                if (play != g_poseV2RegisteredPlay.end())
                {
                    // Stopped by the replay, not only left to the engine.
                    EndPoseV2RegisteredEntry(**play, track, timeMs, false, true);
                    g_poseV2RegisteredPlay.erase(play);
                }
                continue;
            }
            // An instance whose entity left the replay ends with it.
            if (play != g_poseV2RegisteredPlay.end() && (*play)->inst && !PoseV2RegisteredOwnerPresent(track, timeMs, false))
            {
                noteGap(replay::fxmotion::GapSeen::OwnerAbsent);
                EndPoseV2RegisteredEntry(**play, track, timeMs, false, false);
                ++g_poseV2RegisteredEnd.ownerGone;
                g_poseV2RegisteredPlay.erase(play);
                continue;
            }
            if (play == g_poseV2RegisteredPlay.end())
            {
                noteGap(mode != replay::regfx::Mode::Normal ? replay::fxmotion::GapSeen::Hold
                                                            : replay::fxmotion::GapSeen::NoEntry);
                // Created in normal play only, and with its entity there.
                if (mode != replay::regfx::Mode::Normal)
                {
                    ++g_poseV2RegisteredEnd.notCreatedPaused;
                    NoteRegReason(index, RegNotCreated::Paused);
                    continue;
                }
                {
                    const auto retry = g_poseV2RegisteredOwnerRetryMs.find(index);
                    if (retry != g_poseV2RegisteredOwnerRetryMs.end() && timeMs >= retry->second && timeMs - retry->second < 500)
                    {
                        ++g_poseV2RegisteredEnd.ownerMissing;
                        continue;
                    }
                }
                if (!replay::regfx::MayCreate(mode, track.attached, PoseV2RegisteredOwnerPresent(track, timeMs, true)))
                {
                    ++g_poseV2RegisteredEnd.ownerMissing;
                    g_poseV2RegisteredOwnerRetryMs[index] = timeMs;
                    NoteRegReason(index, RegNotCreated::OwnerMissing);
                    continue;
                }
                // Recorded on another entity (an older clip: a map effect on
                // the horse).
                if (g_poseV2RegisteredOwnerCheck && !PoseV2RegisteredParentAtOwner(track, *frame, timeMs))
                {
                    ++g_poseV2RegisteredEnd.ownerMismatch;
                    g_poseV2RegisteredOwnerRetryMs[index] = timeMs;
                    NoteRegReason(index, RegNotCreated::OwnerMismatch);
                    continue;
                }
                g_poseV2RegisteredOwnerRetryMs.erase(index);
                // The entry is made only when its entity is ready
                // (FxRegisteredOwnerReadyHold): made first, a pause on the next
                // tick would get or create it every tick with the entity not
                // drawn yet.
                std::unique_ptr<PlayRegistered>& pending = g_fxPendingEntries[index];
                if (!pending)
                {
                    pending = std::make_unique<PlayRegistered>();
                    pending->track = index;
                }
                pending->pendingTick = g_fxRegTick;
                if (g_fxCfg.ownerReadyHold)
                {
                    const char* pendingKind = "none";
                    const Entity pendingOwner = PoseV2RegisteredOwnerEntity(track, timeMs, *pending, pendingKind);
                    if (pendingOwner)
                    {
                        const auto cached = readiness.find(pendingOwner);
                        const PoseV2OwnerReadiness state = cached != readiness.end() ? cached->second
                            : (readiness[pendingOwner] = PoseV2FxOwnerReadiness(pendingOwner));
                        if (!state.ready)
                        {
                            ++g_fxOwnerNotReadyTicks;
                            ++g_fxEntryHeldNotReady;
                            pending->waitReason = "owner_not_ready";
                            NoteRegReason(index, RegNotCreated::OwnerNotReady);
                            g_fxCheck.NoteCall(track.effectHash, false, false);
                            continue;
                        }
                    }
                }
                g_poseV2RegisteredPlay.push_back(std::move(pending));
                g_fxPendingEntries.erase(index);
                g_poseV2RegisteredPlay.back()->track = index;
                g_poseV2RegisteredKeys.emplace_back();
                g_poseV2RegisteredPlay.back()->ownerKey = g_poseV2RegisteredKeys.back().data();
                play = std::prev(g_poseV2RegisteredPlay.end());
            }
            PlayRegistered& entry = **play;
            const bool normalNow = mode == replay::regfx::Mode::Normal;
            if (!normalNow)
                noteGap(replay::fxmotion::GapSeen::Hold);
            // Our settings read back one tick after the start.
            if (entry.settingsCheck && entry.inst)
            {
                entry.settingsCheck = false;
                replay::entity::FxSettings now;
                if (replay::fx::ReadInstanceSettings(entry.inst, now))
                {
                    const std::uint32_t mask = entry.checkSettings.mask & replay::entity::FxSettings::kApplied &
                        ~replay::entity::FxSettings::kVel350;
                    const std::uint32_t diff = replay::entity::FxSettingsDiff(entry.checkSettings, now, mask);
                    g_fxCheck.NoteSettings(track.effectHash, diff != 0);
                    if (diff && g_fxSettingsDiffLogs < 8)
                    {
                        ++g_fxSettingsDiffLogs;
                        Ctx().log << "FX_SETTINGS_DIFF,kind=registered,hash=0x" << std::hex << track.effectHash << ",fields=0x" << diff
                            << std::dec << ",tele=" << now.tele3A8 << "/" << entry.checkSettings.tele3A8 << ",lod=" << now.lod3A0 << "/"
                            << entry.checkSettings.lod3A0 << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            // Inside a stretch the game did not call the key, the replay does
            // not either: the registry stops the instance softly and restarts
            // it when the calls come back, one burst per call run as in the
            // game.
            if (normalNow)
            {
                static const std::vector<replay::entity::RegisteredFxGap> kNoGaps;
                const std::vector<replay::entity::RegisteredFxGap>& gaps = !track.callGaps.empty() ? track.callGaps
                    : index < g_fxDerivedGaps.size() ? g_fxDerivedGaps[index] : kNoGaps;
                if (!gaps.empty() && replay::fxmotion::InGap(gaps, timeMs, g_poseV2RegisteredLastPlayhead))
                {
                    if (!entry.inGap)
                    {
                        ++g_fxGapsEntered;
                        g_fxCheck.NoteGap(track.effectHash, false);
                    }
                    noteGap(replay::fxmotion::GapSeen::Entered);
                    entry.inGap = true;
                    ++g_fxGapTicks;
                    g_fxCheck.NoteCall(track.effectHash, false, false);
                    continue;
                }
                if (entry.inGap)
                    entry.gapJustEnded = true;
                entry.inGap = false;
            }
            // An attached track gets or creates only when its entity is ready
            // as the attach update tests it (else the engine frees the entry
            // the next frame and the key makes it again, every tick).
            const char* ownerKind = "none";
            const Entity ownerEntity = PoseV2RegisteredOwnerEntity(track, timeMs, entry, ownerKind);
            PoseV2OwnerReadiness ownerState;
            ownerState.ready = true;
            if (ownerEntity)
            {
                const auto cached = readiness.find(ownerEntity);
                ownerState = cached != readiness.end() ? cached->second : (readiness[ownerEntity] = PoseV2FxOwnerReadiness(ownerEntity));
            }
            // FxRegisteredOwnerReadyHold: paused too.
            const bool holdNow = mode == replay::regfx::Mode::Hold && g_fxCfg.ownerReadyHold;
            if ((normalNow || holdNow) && ownerEntity && !ownerState.ready)
            {
                ++g_fxOwnerNotReadyTicks;
                if (holdNow)
                    ++g_fxHoldNotReady;
                if (!entry.inst)
                    NoteRegReason(index, RegNotCreated::OwnerNotReady);
                if (normalNow)
                    g_fxCheck.NoteCall(track.effectHash, false, false);
                entry.waitReason = "owner_not_ready";
                continue;
            }
            // Paused, the key is got only for an instance that is still ours
            // and playing (it keeps it alive, the registry stops one not got);
            // any other (none yet, given to another effect, stopping, killed
            // or freed) would be created or restarted by the call, a new burst
            // at a playhead that stands still. FxPauseFreeze holds the live
            // ones (step 0).
            if (holdNow)
            {
                replay::fx::InstanceIdentity live;
                const bool ours = entry.inst && entry.identityKnown && replay::fx::ReadInstanceIdentity(entry.inst, live) &&
                    live.rule == entry.identity.rule && live.state == replay::regfx::kStatePlaying;
                if (!ours)
                {
                    ++g_fxHoldSkips;
                    continue;
                }
            }
            if (normalNow && replay::fxmotion::RecreateHeld(wallMs, entry.lastRecreateWallMs, g_fxCfg.recreateBackoffMs))
            {
                ++g_fxBackoffTicks;
                g_fxCheck.NoteCall(track.effectHash, false, false);
                continue;
            }
            // Our instance as it is before this tick's call: a blink is one the
            // engine killed or gave to another effect within two ticks of its
            // start.
            std::uint32_t priorState = 0;
            if (entry.inst)
            {
                replay::fx::InstanceIdentity prior;
                if (replay::fx::ReadInstanceIdentity(entry.inst, prior))
                {
                    priorState = prior.state;
                    if (normalNow && entry.identityKnown && entry.ticksSinceFresh < 2 &&
                        (prior.rule != entry.identity.rule || prior.state >= replay::regfx::kStateKilled))
                    {
                        ++g_fxBlinks;
                        g_fxCheck.NoteBlink(track.effectHash);
                        // Measured only (blinks of several horses at once,
                        // state 5/6, same rule [hypothesis: the rule's instance
                        // limit or a cut by distance or LOD]): the distance to
                        // the rendered camera, +0x3A0, +0x3A8, +0x3BC and how
                        // many instances of ours of this effect are playing. Up
                        // to 4 lines for each of the first 4 hashes, 24 in all.
                        std::uint32_t* hashLogs = nullptr;
                        for (int slot = 0; slot < 4 && !hashLogs; ++slot)
                            if (g_fxBlinkHashes[slot] == track.effectHash || !g_fxBlinkHashes[slot])
                            {
                                g_fxBlinkHashes[slot] = track.effectHash;
                                hashLogs = &g_fxBlinkLogsByHash[slot];
                            }
                        if (g_fxBlinkLogs < 24 && hashLogs && *hashLogs < 4)
                        {
                            ++g_fxBlinkLogs;
                            ++*hashLogs;
                            const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
                            const float dx = prior.position[0] - cam.x;
                            const float dy = prior.position[1] - cam.y;
                            const float dz = prior.position[2] - cam.z;
                            replay::entity::FxSettings settings;
                            const bool settingsRead = replay::fx::ReadInstanceSettings(entry.inst, settings);
                            std::uint32_t live = 0;
                            for (const std::unique_ptr<PlayRegistered>& other : g_poseV2RegisteredPlay)
                            {
                                replay::fx::InstanceIdentity otherId;
                                if (other->inst && other->track < Ctx().clip.registeredFx.size() &&
                                    Ctx().clip.registeredFx[other->track].effectHash == track.effectHash &&
                                    replay::fx::ReadInstanceIdentity(other->inst, otherId) && otherId.hash == track.effectHash &&
                                    otherId.state == replay::regfx::kStatePlaying)
                                    ++live;
                            }
                            Ctx().log << "FX_BLINK,hash=0x" << std::hex << track.effectHash << std::dec << ",track=" << index
                                << ",state=" << prior.state << ",same_rule=" << (prior.rule == entry.identity.rule ? 1 : 0)
                                << ",ticks=" << entry.ticksSinceFresh << ",owner_kind=" << ownerKind
                                << ",cam_dist_m=" << std::sqrt(dx * dx + dy * dy + dz * dz)
                                << ",lod3a0=" << (settingsRead ? settings.lod3A0 : -1.0f)
                                << ",tele3a8=" << (settingsRead ? settings.tele3A8 : -1.0f)
                                << ",cull3bc=0x" << std::hex << (settingsRead ? settings.cull3BC : 0xFFFFu) << std::dec
                                << ",live_ours=" << live << ",playhead_ms=" << timeMs << '\n';
                        }
                    }
                }
            }
            bool created = false;
            std::string error;
            // Flag 0 always: with the flag set the core registers the owner with
            // the fx manager (vtable +0x98) and later calls back on it as an
            // entity; our owner is a plain key, and the wade effects (flag 1)
            // crash the game at the moment they start.
            const std::uint64_t inst = replay::fx::RegisteredGetOrCreate(entry.ownerKey, 0, 0, track.effectHash,
                track.arg8, track.arg9, created, error);
            if (!inst)
            {
                ++g_poseV2RegisteredFailed;
                g_poseV2RegisteredLastError = error;
                entry.waitReason = "create_failed";
                continue;
            }
            entry.waitReason = "none";
            if (normalNow && ScriptLoopSite(track.callerSite))
            {
                replay::scriptfx::LoopTrackPlay& loop = g_scriptLoopPlay[index];
                if (!loop.entered)
                {
                    loop.entered = true;
                    loop.firstMs = timeMs;
                }
                loop.lastMs = (std::max)(loop.lastMs, timeMs);
            }
            const bool fresh = created || inst != entry.inst;
            if (normalNow)
                g_fxCheck.NoteCall(track.effectHash, false, true);
            // The fx check: once per stretch of play, as the clip basis counts
            // it (an older clip folds the key's re-creations into one track; a
            // creation again inside a stretch is the recreation counters'
            // business, not a second play).
            if (normalNow && g_fxTrackPlaySpan[index] != g_fxSpanId)
            {
                g_fxTrackPlaySpan[index] = g_fxSpanId;
                g_fxCheck.NotePlay(track.effectHash, replay::fxcheck::Maker::Ours, true, track.callerSite);
            }
            if (created)
            {
                if (index < g_fxRegReason.size() && g_fxRegReason[index] == RegNotCreated::Created)
                    ++g_fxRegRecreated;
                NoteRegReason(index, RegNotCreated::Created);
                // After a long stretch without a call the registry had freed the
                // entry and the call that comes back creates (no restart from
                // the registry): that is the game's restart too.
                if (entry.gapJustEnded && normalNow)
                {
                    ++g_fxGapCreates;
                    ++g_fxPlayRestarts;
                    g_fxCheck.NoteRestart(track.effectHash, false);
                }
                // A creation of a track already made in this stretch of play:
                // the engine freed the entry.
                const bool before = g_fxTrackCreatedSpan[index] != 0;
                // After a stretch without a call the registry may have freed
                // the entry (its instance reached state 5 unasked): a new one
                // there is the restart of the game, not a recreation loop.
                const bool sameSpan = before && g_fxTrackCreatedSpan[index] == g_fxSpanId && !entry.gapJustEnded;
                const replay::fxmotion::Recreate kind = replay::fxmotion::ClassifyRecreate(before, sameSpan, ownerState.ready);
                g_fxTrackCreatedSpan[index] = g_fxSpanId;
                if (kind != replay::fxmotion::Recreate::First)
                {
                    const int noteKind = kind == replay::fxmotion::Recreate::Reentered ? 1
                        : kind == replay::fxmotion::Recreate::OwnerNotReady ? 2 : 0;
                    g_fxCheck.NoteRecreate(track.effectHash, noteKind, index, timeMs, wallMs);
                    ++(noteKind == 1 ? g_fxRecreateReentered : noteKind == 2 ? g_fxRecreateNotReady : g_fxRecreateInSpan);
                    if (noteKind != 1)
                        entry.lastRecreateWallMs = wallMs;
                    // Capped per kind and per hash, so a pause cannot use up
                    // the lines that name the branch of the recreations in
                    // play.
                    std::uint32_t& kindLogs = g_fxRecreateLogsByKind[noteKind];
                    std::uint32_t& hashLogs = g_fxRecreateLogsByHash[(std::uint64_t(noteKind) << 32) | track.effectHash];
                    if (noteKind != 1 && kindLogs < kFxRecreateLogsPerKind &&
                        hashLogs < kFxRecreateLogsPerHash)
                    {
                        ++kindLogs;
                        ++hashLogs;
                        ++g_fxRecreateLogs;
                        Ctx().log << "FX_REG_RECREATE,hash=0x" << std::hex << track.effectHash << std::dec << ",track=" << index
                            << ",kind=" << replay::fxmotion::RecreateName(kind) << ",owner_kind=" << ownerKind
                            << ",entity_alive=" << (ownerEntity ? (ownerState.exists ? 1 : 0) : -1)
                            << ",draw=" << (ownerEntity ? (ownerState.draw ? 1 : 0) : -1)
                            << ",mat_x0=" << (ownerEntity ? (ownerState.matZero ? 1 : 0) : -1) << ",prior_state=" << priorState
                            << ",ms_since_create=" << (timeMs >= entry.createdMs ? timeMs - entry.createdMs : 0)
                            << ",span=" << g_fxSpanId << ",site=0x" << std::hex << track.callerSite << std::dec
                            << ",mode=" << (normalNow ? "normal" : "hold") << ",gap_ended=" << (entry.gapJustEnded ? 1 : 0)
                            << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            entry.ticksSinceFresh = fresh ? 0 : entry.ticksSinceFresh + 1;
            entry.gapJustEnded = false;
            entry.inst = inst;
            if (fresh) // what the end's stop checks against
            {
                replay::fx::InstanceIdentity read;
                entry.identityKnown = replay::fx::ReadInstanceIdentity(inst, read);
                entry.identity = PoseV2RegisteredIdentity(read);
            }
            if (fresh && track.attached)
            {
                // The entity the recording had it on: its puppet, or the same
                // world object found by model and position.
                Entity target = 0;
                if (track.attachId.Valid())
                {
                    const replay::playback::Puppet* puppet = Ctx().puppets.Find(track.attachId);
                    if (puppet && puppet->clone && replay::entities::Exists(puppet->clone))
                        target = puppet->clone;
                }
                // A recorded owner without a live puppet: no model search (it
                // finds a real entity, such as the player's own gun).
                if (!target && track.attachModel && !track.attachId.Valid())
                    target = ownerEntity && std::strcmp(ownerKind, "world") == 0 && replay::entities::Exists(ownerEntity)
                        ? ownerEntity // the object found this tick, kept 500 ms
                        : FindEntityByModelNear(track.attachModel, track.attachPos, 3.0f);
                std::string attachError;
                const bool attachedNow = target &&
                    replay::fx::AttachInstance(inst, replay::entities::AddressOf(target), track.attachBone, attachError);
                // What it rides now.
                entry.attachedOk = attachedNow;
                entry.ownerAddress = attachedNow && track.attachId.Valid()
                    ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(target)) : 0;
                if (attachedNow && !track.attachId.Valid())
                {
                    entry.worldTarget = target;
                    entry.worldTargetAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(target));
                }
                if (!attachedNow)
                {
                    ++g_poseV2RegisteredAttachMisses;
                    g_poseV2RegisteredAttachError = target ? attachError : "no_entity";
                    // Evidence: which track missed its entity and why.
                    static replay::log::LogLimit missLogs; // per session
                    if (g_poseV2ResearchWindows && missLogs.Take(24))
                    {
                        const replay::playback::Puppet* puppet = track.attachId.Valid() ? Ctx().puppets.Find(track.attachId) : nullptr;
                        Ctx().log << "POSE_V2_FX_REGISTERED_ATTACH_MISS,hash=0x" << std::hex << track.effectHash << std::dec
                            << ",id=" << (track.attachId.Valid() ? replay::entity::IdText(track.attachId) : std::string("none"))
                            << ",puppet=" << (puppet ? (puppet->clone ? "clone" : "no_clone") : "none")
                            << ",state=" << (puppet ? static_cast<int>(puppet->state) : -1)
                            << ",error=" << g_poseV2RegisteredAttachError << ",start_ms=" << track.startMs
                            << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            // The attach change in force at the playhead: our instance moves to
            // that entity's puppet (retried every tick until the puppet
            // exists); before the first change, or after a rewind past it, it
            // goes back to the track's own attach.
            if (!track.attachTimeline.empty())
            {
                const replay::entity::RegisteredFxAttach* change = replay::entity::RegisteredFxAttachAt(track, timeMs);
                const std::size_t wanted = change ? static_cast<std::size_t>(change - track.attachTimeline.data()) : kNoAttachChange;
                if (wanted != entry.attachApplied || (fresh && change))
                {
                    const replay::entity::ReplayId id = change ? change->id : track.attachId;
                    const std::int32_t bone = change ? change->bone : track.attachBone;
                    const replay::playback::Puppet* puppet = id.Valid() ? Ctx().puppets.Find(id) : nullptr;
                    const bool ready = puppet && puppet->clone && replay::entities::Exists(puppet->clone);
                    std::string attachError;
                    const bool attachedNow = ready &&
                        replay::fx::AttachInstance(inst, replay::entities::AddressOf(puppet->clone), bone, attachError);
                    if (attachedNow)
                    {
                        entry.attachedOk = true;
                        entry.ownerAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone));
                    }
                    // Back to a track that had no recorded entity: nothing to attach to.
                    if (attachedNow || (!change && !id.Valid()))
                    {
                        entry.attachApplied = wanted;
                        ++g_poseV2FxAttachPlayApplied;
                    }
                    else
                        ++g_poseV2FxAttachPlayWaiting;
                    const bool logMiss = !attachedNow && entry.attachApplied != wanted && entry.attachMissLogged != wanted;
                    if ((attachedNow || logMiss) && g_poseV2FxAttachPlayLogs < kFxAttachLogCap)
                    {
                        ++g_poseV2FxAttachPlayLogs;
                        if (logMiss)
                            entry.attachMissLogged = wanted;
                        Ctx().log << "POSE_V2_FX_ATTACH_CHANGE,phase=play,hash=0x" << std::hex << track.effectHash << std::dec
                            << ",id=" << (id.Valid() ? replay::entity::IdText(id) : std::string("none")) << ",bone=" << bone
                            << ",change=" << (change ? static_cast<long long>(wanted) : -1LL)
                            << ",status=" << (attachedNow ? "attached" : ready ? attachError.c_str() : "waiting_puppet")
                            << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            // As the engine's own site does: planes, flags, evolutions and zoom
            // every frame; matrices and start only when the instance was just
            // created (the site gates them on the core's created flag; a start
            // every frame restarts the effect and nothing accumulates).
            {
                // The site's settings in force, on a fresh instance before its
                // start and when they change.
                const replay::entity::FxSettings* settings =
                    g_fxCfg.settingsReplay ? replay::entity::RegisteredFxSettingsAt(track, timeMs) : nullptr;
                if (!settings && g_fxCfg.settingsReplay && track.callerSite == replay::fx::HoofRegisteredSite())
                {
                    settings = &LegacyHoofSettings();
                    if (fresh)
                        ++g_fxSettingsLegacyHoof;
                }
                if (settings && (fresh || settings != entry.appliedSettings))
                {
                    std::string settingsError;
                    if (replay::fx::ApplyInstanceSettings(inst, *settings, settingsError))
                    {
                        ++g_fxSettingsApplied;
                        if (fresh)
                        {
                            entry.settingsCheck = true;
                            entry.checkSettings = *settings;
                        }
                    }
                    else
                    {
                        ++g_fxSettingsFailed;
                        g_fxSettingsError = settingsError;
                    }
                    entry.appliedSettings = settings;
                }
            }
            {
                std::string ignored;
                if (!replay::fx::ApplyInstancePlanes(inst, frame->planes, frame->planeCount, frame->aux350, ignored))
                {
                    ++g_poseV2RegisteredPlaneFailures;
                    g_poseV2RegisteredPlaneError = ignored;
                }
                // The flags are the engine's: attach and the core set the bits
                // they own, the update sets its own (0x261 at start, 0x2e7
                // later in gameplay); writing the recorded value is not replayed.
                if (fresh && replay::fx::ApplyInstanceState(inst, frame->matrix, frame->parent, frame->evolutions, frame->zoom, false, ignored))
                    replay::fx::SnapshotReplayInstance(inst); // research: the watched effect before its start
            }
            if (fresh)
            {
                std::string ignored;
                replay::fx::ApplyInstanceExtras(inst, frame->start310, frame->farClip, ignored);
            }
            // An attached instance gets its parent from
            // the engine's attach update every frame; the recorded parent
            // written over it every tick would
            // move it between the recorded and the live bone. Written at the
            // creation only, as the hoof site does.
            const bool writeParent = fresh || !entry.attachedOk;
            if (!writeParent)
                ++g_fxParentOnceSkipped;
            if (replay::fx::ApplyInstanceState(inst, frame->matrix, frame->parent, frame->evolutions, frame->zoom, fresh, error,
                    writeParent))
            {
                entry.last = frame;
                if (fresh)
                {
                    ++g_poseV2RegisteredCreated;
                    entry.createdMs = timeMs;
                }
                else
                    ++g_poseV2RegisteredUpdates;
                if (!entry.lateLogged && track.effectHash == g_poseV2FxWatchHash && timeMs >= entry.createdMs + 1000)
                {
                    entry.lateLogged = true;
                    std::vector<std::uint8_t> snapshot;
                    if (replay::fx::SnapshotInstance(inst, snapshot))
                        LogPoseV2FxSnapshot("replay_late", snapshot);
                }
                // Evidence: our instance's state every 250 ms for the first
                // tracks (read for the line only, Debug=1 only).
                if (index < 4 && replay::log::Verbose() && timeMs >= entry.nextSampleMs)
                {
                    entry.nextSampleMs = timeMs + 250;
                    replay::fx::InstanceState state;
                    if (replay::fx::ReadInstanceState(inst, state))
                        Ctx().log << "POSE_V2_FX_REGISTERED_STATE,phase=play,hash=0x" << std::hex << track.effectHash
                            << ",flags=0x" << state.flags << std::dec << ",time_ms=" << timeMs << ",state=" << state.state
                            << ",mode=" << int(state.mode) << ",zoom=" << state.zoom << ",planes=" << state.planeCount
                            << ",quality=" << int(state.quality) << ",slot_counts=" << state.slotCounts[0] << "/" << state.slotCounts[1]
                            << "/" << state.slotCounts[2] << ",frame_planes=" << frame->planeCount
                            << ",timer=" << state.timer3f0 << ",loops=" << state.loops << ",loop_t=" << state.loopTime << std::hex
                            << ",flags2=0x" << state.flags2 << std::dec << ",b341=" << int(state.byte341)
                            << ",evo0=" << int(state.evolutions[0]) << "/" << int(frame->evolutions[0])
                            << ",p0=" << state.planes[0] << ";" << state.planes[1] << ";" << state.planes[2] << ";" << state.planes[3]
                            << ",fp0=" << frame->planes[0] << ";" << frame->planes[1] << ";" << frame->planes[2] << ";" << frame->planes[3]
                            << ",pos=" << state.matrix[12] << ";" << state.matrix[13] << ";" << state.matrix[14]
                            << ",parent_pos=" << state.parent[12] << ";" << state.parent[13] << ";" << state.parent[14]
                            << ",created=" << created << '\n';
                }
            }
            else
            {
                ++g_poseV2RegisteredFailed;
                g_poseV2RegisteredLastError = error;
            }
        }
        // The world objects our live tracks ride.
        g_fxWorldOwnersLive.clear();
        for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
            if (entry->inst && entry->worldTargetAddress && !entry->inGap &&
                std::find(g_fxWorldOwnersLive.begin(), g_fxWorldOwnersLive.end(), entry->worldTargetAddress) == g_fxWorldOwnersLive.end())
                g_fxWorldOwnersLive.push_back(entry->worldTargetAddress);
        // FxWorldOwnerEarly: and those a track in force waits to ride, under
        // the lease.
        if (g_fxCfg.worldOwnerEarly)
        {
            for (auto& lease : g_fxEarlyRefuse)
                lease.second.seen = false;
            const auto candidate = [&](const PlayRegistered& waiting) {
                if (!waiting.worldTargetAddress || waiting.track >= Ctx().clip.registeredFx.size())
                    return;
                FxEarlyRefuse& lease = g_fxEarlyRefuse[waiting.track];
                if (lease.address != waiting.worldTargetAddress)
                    lease = FxEarlyRefuse{waiting.worldTargetAddress, wallMs, false, false};
                lease.seen = true;
                if (lease.expired)
                    return;
                if (wallMs - lease.sinceWallMs < kFxWorldRefuseLeaseMs)
                {
                    ++g_fxWorldEarlyTicks;
                    if (std::find(g_fxWorldOwnersLive.begin(), g_fxWorldOwnersLive.end(), lease.address) == g_fxWorldOwnersLive.end())
                        g_fxWorldOwnersLive.push_back(lease.address);
                    return;
                }
                lease.expired = true;
                ++g_fxWorldRefuseExpired;
                if (g_fxWorldRefuseLogs < 16)
                {
                    ++g_fxWorldRefuseLogs;
                    const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[waiting.track];
                    Ctx().log << "FX_WORLD_REFUSE_EXPIRED,hash=0x" << std::hex << track.effectHash << ",owner=0x" << lease.address
                        << std::dec << ",track=" << waiting.track << ",reason=" << waiting.waitReason
                        << ",lease_ms=" << kFxWorldRefuseLeaseMs << ",playhead_ms=" << timeMs << '\n';
                }
            };
            for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
                if (!entry->inst && !entry->inGap)
                    candidate(*entry);
            for (const auto& pending : g_fxPendingEntries)
                if (pending.second && pending.second->pendingTick == g_fxRegTick)
                    candidate(*pending.second);
            for (auto lease = g_fxEarlyRefuse.begin(); lease != g_fxEarlyRefuse.end();)
                lease = lease->second.seen ? std::next(lease) : g_fxEarlyRefuse.erase(lease);
        }
    }

    void DrainPoseV2FxRegistered(const char* phase)
    {
        static std::vector<replay::fx::RegisteredCall> calls;
        replay::fx::CollectRegisteredSince(g_poseV2FxRegisteredCursor, calls);
        const bool recordPhase = std::strcmp(phase, "record") == 0;
        if (recordPhase)
            RecordPoseV2RegisteredFx(calls);
        for (const replay::fx::RegisteredCall& call : calls)
        {
            if (recordPhase && call.created && !call.byReplay && call.effectHash)
                g_fxCheck.NoteRecord(call.effectHash, replay::fxcheck::Stored::Registered, true, call.caller);
            RegisteredStat& stat = g_poseV2FxRegistered[call.effectHash];
            ++stat.calls;
            if (call.created)
                ++stat.created;
            const std::uint64_t key = call.owner + static_cast<std::uint64_t>(call.id);
            stat.keys.insert(key);
            stat.caller = call.caller;
            {
                auto watch = std::find_if(g_poseV2FxRegisteredWatch.begin(), g_poseV2FxRegisteredWatch.end(),
                    [&](const RegisteredWatch& w) { return w.key == key; });
                if (watch != g_poseV2FxRegisteredWatch.end())
                {
                    watch->inst = call.inst;
                    watch->lastSeenMs = replay::pose::capture::NowMs();
                }
                else if (call.created && g_poseV2FxRegisteredWatch.size() < 6)
                    g_poseV2FxRegisteredWatch.push_back({key, call.effectHash, call.inst, 0, replay::pose::capture::NowMs(),
                        replay::pose::capture::NowMs(), false});
            }
            if (call.created && g_poseV2FxRegisteredSamples++ < 32)
            {
                Ctx().log << "POSE_V2_FX_REGISTERED_SAMPLE,phase=" << phase << std::hex << ",hash=0x" << call.effectHash << ",asset=0x" << call.assetHash
                    << ",owner=0x" << call.owner << ",arg3=0x" << call.arg3 << ",inst=0x" << call.inst
                    << ",caller=0x" << call.caller << std::dec << ",id=" << call.id << ",created=" << int(call.created)
                    << ",flag=" << int(call.flag) << ",a8=" << int(call.arg8) << ",a9=" << int(call.arg9)
                    << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            }
        }
    }

    void SamplePoseV2FxRegisteredState(const char* phase)
    {
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (RegisteredWatch& watch : g_poseV2FxRegisteredWatch)
        {
            if (nowMs < watch.nextMs || nowMs > watch.lastSeenMs + 100)
                continue;
            watch.nextMs = nowMs + 250;
            if (!watch.lateLogged && watch.hash == g_poseV2FxWatchHash && nowMs >= watch.createdMs + 1000)
            {
                watch.lateLogged = true;
                std::vector<std::uint8_t> snapshot;
                if (replay::fx::SnapshotInstance(watch.inst, snapshot))
                    LogPoseV2FxSnapshot(std::strcmp(phase, "record") == 0 ? "gameplay_late" : "engine_late", snapshot);
            }
            replay::fx::InstanceState state;
            if (!replay::fx::ReadInstanceState(watch.inst, state))
                continue;
            std::ostream& log = Ctx().log;
            log << "POSE_V2_FX_REGISTERED_STATE,phase=" << phase << std::hex << ",hash=0x" << watch.hash << ",key=0x" << watch.key
                << ",flags=0x" << state.flags << std::dec << ",time_ms=" << nowMs << ",state=" << state.state << ",mode=" << int(state.mode)
                << ",zoom=" << state.zoom << ",quality=" << int(state.quality) << ",slot_counts=" << state.slotCounts[0] << "/"
                << state.slotCounts[1] << "/" << state.slotCounts[2] << ",planes=" << state.planeCount << ",timer=" << state.timer3f0
                << ",loops=" << state.loops << ",loop_t=" << state.loopTime << std::hex << ",flags2=0x" << state.flags2 << std::dec
                << ",b341=" << int(state.byte341)
                << ",p0=" << state.planes[0] << ";" << state.planes[1] << ";" << state.planes[2] << ";" << state.planes[3]
                << ",p0b=" << state.planes[4] << ";" << state.planes[5] << ";" << state.planes[6] << ";" << state.planes[7]
                << ",p0c=" << state.planes[8] << ";" << state.planes[9] << ";" << state.planes[10] << ";" << state.planes[11]
                << ",aux=" << state.aux350[0] << ";" << state.aux350[1] << ";" << state.aux350[2] << ";" << state.aux350[3]
                << ",pos=" << state.matrix[12] << ";" << state.matrix[13] << ";" << state.matrix[14]
                << ",parent_pos=" << state.parent[12] << ";" << state.parent[13] << ";" << state.parent[14]
                << ",x_axis=" << state.matrix[0] << ";" << state.matrix[1] << ";" << state.matrix[2]
                << ",parent_x=" << state.parent[0] << ";" << state.parent[1] << ";" << state.parent[2] << ",evo=";
            for (std::uint8_t byte : state.evolutions)
                log << std::hex << std::setw(2) << std::setfill('0') << int(byte);
            log << std::dec << std::setfill(' ') << '\n';
        }
    }

    void LogPoseV2FxRegistered(const char* phase)
    {
        DrainPoseV2FxRegistered(phase);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_FX_REGISTERED,phase=" << phase << ",effects=" << g_poseV2FxRegistered.size() << ",list=";
        bool first = true;
        for (const auto& entry : g_poseV2FxRegistered)
        {
            log << (first ? "" : ";") << "0x" << std::hex << entry.first << ":" << std::dec << entry.second.calls << "/"
                << entry.second.created << "/" << entry.second.keys.size() << "@0x" << std::hex << entry.second.caller << std::dec;
            first = false;
        }
        log << '\n';
        log.flush();
        g_poseV2FxRegistered.clear();
        g_poseV2FxRegisteredSamples = 0;
        g_poseV2FxRegisteredWatch.clear();
    }
    std::uint64_t g_poseV2FxAttachesAtStart = 0;

    void LogPoseV2FxStartSample(const char* kind, const replay::fx::FxStart& start)
    {
        if (g_poseV2FxSamples++ >= 24)
            return;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_FX_START_SAMPLE,kind=" << kind << std::hex << ",hash=0x" << start.effectHash
            << ",asset=0x" << start.assetHash << ",site=0x" << start.callerSite << ",callers=0x" << start.callers[0]
            << "/0x" << start.callers[1] << "/0x" << start.callers[2] << ",entity=0x" << start.entity
            << ",flags=0x" << start.flags << std::dec << ",bone=" << start.bone << ",mode=" << int(start.mode)
            << ",zoom=" << start.zoom << ",pos=" << start.matrix[12] << ";" << start.matrix[13] << ";" << start.matrix[14]
            << ",parent_pos=" << start.parent[12] << ";" << start.parent[13] << ";" << start.parent[14]
            << ",x_axis=" << start.matrix[0] << ";" << start.matrix[1] << ";" << start.matrix[2]
            << ",parent_x=" << start.parent[0] << ";" << start.parent[1] << ";" << start.parent[2] << ",evo=";
        for (std::uint8_t byte : start.evolutions)
            log << std::hex << std::setw(2) << std::setfill('0') << int(byte);
        log << std::dec << std::setfill(' ') << '\n';
    }

    // Fx starts with no world position (a mode 1 parent of zeros, or a
    // composed position near the map origin): counted, a few logged.
    std::uint64_t g_poseV2FxUnresolvedParent = 0;
    std::uint64_t g_poseV2FxInvalidWorld = 0;
    std::uint32_t g_poseV2FxUnresolvedLogs = 0;
    void NotePoseV2FxUnresolved(const char* kind, const replay::fx::FxStart& start)
    {
        ++(std::strcmp(kind, "unresolved_parent") == 0 ? g_poseV2FxUnresolvedParent : g_poseV2FxInvalidWorld);
        if (g_poseV2FxUnresolvedLogs >= 12)
            return;
        ++g_poseV2FxUnresolvedLogs;
        Ctx().log << "POSE_V2_FX_NO_WORLD_POSITION,kind=" << kind << std::hex << ",hash=0x" << start.effectHash
            << ",site=0x" << start.callerSite << ",entity=0x" << start.entity << ",flags=0x" << start.flags << std::dec
            << ",mode=" << int(start.mode) << ",pos=" << start.matrix[12] << ";" << start.matrix[13] << ";" << start.matrix[14]
            << ",parent_pos=" << start.parent[12] << ";" << start.parent[13] << ";" << start.parent[14] << '\n';
    }

    void LogPoseV2FxCounts(const char* tag, const std::map<std::uint32_t, std::uint32_t>& counts)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::ostream& log = Ctx().log;
        log << tag << ",keys=" << counts.size() << ",top=";
        std::size_t shown = 0;
        for (const auto& entry : sorted)
        {
            if (shown++ == 16)
                break;
            log << (shown > 1 ? ";" : "") << "0x" << std::hex << entry.first << std::dec << ":" << entry.second;
        }
        log << '\n';
    }
    // Melee impact sounds of the recording.
    std::uint64_t g_poseV2MeleeCursor = 0;
    replay::entity::ForwardEventCursor g_poseV2MeleeCursorPlay;
    std::uint64_t g_poseV2MeleePlayed = 0;
    std::uint64_t g_poseV2MeleeRefused = 0;
    std::string g_poseV2MeleeLastStatus;
    replay::meleecheck::Tally g_meleeCheck; // what each hit came to (melee_sound_check.h)
    // Body impacts of the recording.
    std::uint64_t g_poseV2BodyImpactCursor = 0;
    std::uint64_t g_poseV2BodyImpactsPlayed = 0;
    std::uint64_t g_poseV2BodyImpactsRefused = 0;
    std::string g_poseV2BodyImpactLast;
    // Ped sound requests of the recording.
    std::uint64_t g_poseV2SoundCursor = 0;
    std::uint64_t g_poseV2PlaySoundCursor = 0;
    std::uint64_t g_poseV2PlaySoundsCovered = 0;
    std::uint32_t g_poseV2PersistentUnattributedLogged = 0;
    std::uint32_t g_poseV2SharedHelperLogged = 0;
    std::uint32_t g_poseV2ImpactLogged = 0;
    std::uint32_t g_poseV2WeaponSoundLogged = 0;

    void SetFxParticlesConfig(const FxParticlesConfig& config)
    {
        g_fxCfg = config;
    }

    void FxBeforePlace(std::uint64_t puppetAddress)
    {
        if (g_fxRelocateProbeLogs >= 32 || g_fxOwned.empty())
            return;
        for (const FxOwned& owned : g_fxOwned)
        {
            if (owned.owner != puppetAddress)
                continue;
            replay::fx::MotionFields motion;
            if (!replay::fx::ReadMotionFields(owned.inst, motion))
                continue;
            g_fxProbeBefore.push_back({owned.inst, motion.flags2, owned.lastFlags2, owned.lastKnown});
        }
    }

    void FxAfterPlace(std::uint64_t puppetAddress, float stepM)
    {
        if (g_fxOwned.empty())
            return;
        g_fxPlacedThisTick.push_back({puppetAddress, stepM});
        if (g_fxProbeBefore.empty())
            return;
        for (const FxProbeBefore& before : g_fxProbeBefore)
        {
            if (g_fxRelocateProbeLogs >= 32)
                break;
            const auto owned = std::find_if(g_fxOwned.begin(), g_fxOwned.end(),
                [&](const FxOwned& entry) { return entry.inst == before.inst && entry.owner == puppetAddress; });
            if (owned == g_fxOwned.end())
                continue;
            replay::fx::MotionFields motion;
            if (!replay::fx::ReadMotionFields(before.inst, motion))
                continue;
            ++g_fxRelocateProbeLogs;
            // The placement itself marks the effect when bit_before_place=0 and
            // bit_after_place=1 with step_m under tele_m.
            Ctx().log << "FX_RELOCATE_PROBE,hash=0x" << std::hex << owned->hash << ",owner=0x" << puppetAddress << ",inst=0x"
                << before.inst << std::dec << ",bit_prev_tick=" << (before.prevKnown ? int((before.prevTick & 2u) != 0) : -1)
                << ",bit_before_place=" << int((before.before & 2u) != 0) << ",bit_after_place=" << int((motion.flags2 & 2u) != 0)
                << ",moved_bit=" << int((motion.flags2 & 0x10u) != 0) << ",tele_m=" << motion.tele3A8 << ",step_m=" << stepM
                << ",state=" << motion.state << ",playhead_ms=" << Ctx().transport.PlayheadMs() << '\n';
        }
        g_fxProbeBefore.erase(std::remove_if(g_fxProbeBefore.begin(), g_fxProbeBefore.end(),
                                  [&](const FxProbeBefore& before) {
                                      return std::any_of(g_fxOwned.begin(), g_fxOwned.end(), [&](const FxOwned& entry) {
                                          return entry.inst == before.inst && entry.owner == puppetAddress;
                                      });
                                  }),
            g_fxProbeBefore.end());
    }

    void FxEndOfPlaybackTick(bool normalPlay)
    {
        g_fxProbeBefore.clear();
        if (!g_poseV2FxReplayEnabled || !g_poseV2FxObserverReady || g_fxOwned.empty())
        {
            g_fxPlacedThisTick.clear();
            return;
        }
        static std::vector<std::pair<std::uint64_t, std::uint32_t>> toClear;
        toClear.clear();
        const std::uint64_t wallMs = GetTickCount64();
        // Each owner's step since the end of the last tick, read once (an owner the
        // playback did not place this tick: a still prop is placed every
        // kPlaceRefreshFrames, a map object never).
        ++g_fxEndTick;
        static std::unordered_map<std::uint64_t, float> ownerStep;
        ownerStep.clear();
        if (g_fxCfg.relocateClearStill)
        {
            for (const FxOwned& owned : g_fxOwned)
            {
                if (!owned.handle || ownerStep.count(owned.owner))
                    continue;
                float at[3]{};
                if (!replay::entities::Exists(owned.handle) || !replay::entities::Position(owned.handle, at))
                {
                    ownerStep[owned.owner] = -1.0f;
                    continue;
                }
                FxOwnerAt& last = g_fxOwnerLastAt[owned.owner];
                ownerStep[owned.owner] = last.tick + 1 == g_fxEndTick ? replay::fxmotion::StepMetres(last.at, at) : -1.0f;
                last.at[0] = at[0];
                last.at[1] = at[1];
                last.at[2] = at[2];
                last.tick = g_fxEndTick;
            }
            if (g_fxOwnerLastAt.size() > 1024)
                g_fxOwnerLastAt.clear();
        }
        const bool sampleVeladd = normalPlay && wallMs >= g_fxNextVeladdWallMs;
        if (sampleVeladd)
            g_fxNextVeladdWallMs = wallMs + 100;
        for (FxOwned& owned : g_fxOwned)
        {
            replay::fx::MotionFields motion;
            if (!replay::fx::ReadMotionFields(owned.inst, motion) || motion.state < replay::regfx::kStatePlaying ||
                motion.state > replay::regfx::kStateStopping)
                continue;
            // The engine gave the instance to another effect (a one-shot of ours stays
            // 5 s in the list): not ours to clear nor to measure.
            if (motion.hash != owned.hash)
            {
                ++g_fxRelocateStale;
                continue;
            }
            bool placed = false;
            float step = -1.0f;
            for (const auto& entry : g_fxPlacedThisTick)
                if (entry.first == owned.owner)
                {
                    placed = true;
                    step = entry.second;
                }
            bool cleared = false;
            if (placed && (motion.flags2 & replay::fxmotion::kRelocatedBit))
            {
                // The mark the placement itself set, cleared for a step below
                // the engine's own limit in normal play.
                if (g_fxCfg.puppetNoWarp && replay::fxmotion::ShouldClearRelocate(normalPlay, step, motion.tele3A8))
                {
                    toClear.push_back({owned.inst, owned.hash});
                    cleared = true;
                }
                else
                    ++g_fxRelocateKept;
            }
            else if (!placed && (motion.flags2 & replay::fxmotion::kRelocatedBit) && g_fxCfg.relocateClearStill)
            {
                // FxRelocateClearStill: an owner not placed this tick keeps the placement's
                // mark for the frames between two placements; it is cleared when the owner
                // itself moved less than the engine's limit for the instance and 1 m since
                // the last tick.
                const auto found = ownerStep.find(owned.owner);
                const float still = found != ownerStep.end() ? found->second : -1.0f;
                if (g_fxCfg.puppetNoWarp && replay::fxmotion::ShouldClearRelocate(normalPlay, still, motion.tele3A8))
                {
                    toClear.push_back({owned.inst, owned.hash});
                    cleared = true;
                    ++g_fxRelocateClearedStill;
                }
                else
                    ++g_fxRelocateKeptUnplaced;
                // Who set it: set again since the last clear with a step under the limit
                // is not the attach update (that one needs a parent step over
                // +0x3A8); set before and never cleared is the placement's mark kept.
                std::uint32_t& hashLogs = g_fxUnplacedLogsByHash[owned.hash];
                if (g_fxUnplacedLogs < 24 && hashLogs < 3)
                {
                    ++g_fxUnplacedLogs;
                    ++hashLogs;
                    const bool prevKnown = owned.lastKnown;
                    const bool prevBit = prevKnown && (owned.lastFlags2 & replay::fxmotion::kRelocatedBit);
                    const char* writer = !prevKnown ? "unknown"
                        : prevBit                   ? "kept_from_before"
                        : (still >= 0.0f && still > motion.tele3A8) ? "attach_update_0x2C4F1CC"
                                                    : "other_writer";
                    Ctx().log << "FX_RELOCATE_UNPLACED,hash=0x" << std::hex << owned.hash << ",owner=0x" << owned.owner
                        << ",inst=0x" << owned.inst << std::dec << ",bit_prev_tick=" << (prevKnown ? int(prevBit) : -1)
                        << ",step_m=" << still << ",tele_m=" << motion.tele3A8 << ",state=" << motion.state
                        << ",cleared=" << (cleared ? 1 : 0) << ",writer=" << writer
                        << ",playhead_ms=" << Ctx().transport.PlayheadMs() << '\n';
                }
            }
            const std::uint32_t after = cleared ? (motion.flags2 & ~replay::fxmotion::kRelocatedBit) : motion.flags2;
            owned.lastFlags2 = after;
            owned.lastKnown = true;
            if (normalPlay) // with the owner moving or not (motion_lost without a recording)
                g_fxCheck.NoteMotion(owned.hash, false, after,
                    replay::vegetation::PublishedSpeedSquared(owned.owner) >
                        replay::fxcheck::kMovingOwnerMps * replay::fxcheck::kMovingOwnerMps);
            if (sampleVeladd)
            {
                const float* v = motion.veladd;
                const float veladd = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const float ownerSpeed = std::sqrt(replay::vegetation::PublishedSpeedSquared(owned.owner));
                g_fxCheck.NoteVeladd(owned.hash, false, veladd, ownerSpeed);
                if (g_fxVeladdProbeLogs < 16 && ownerSpeed > 0.5f)
                {
                    ++g_fxVeladdProbeLogs;
                    float published[3]{};
                    replay::vegetation::PublishedVelocity(owned.owner, published);
                    Ctx().log << "FX_VELADD_PROBE,hash=0x" << std::hex << owned.hash << std::dec << ",owner_speed=" << ownerSpeed
                        << ",published=" << published[0] << ";" << published[1] << ";" << published[2] << ",veladd=" << veladd
                        << ",veladd_xyz=" << v[0] << ";" << v[1] << ";" << v[2] << ",velocity_on=" << (g_fxVelocityReady && g_fxCfg.puppetVelocity ? 1 : 0)
                        << ",playhead_ms=" << Ctx().transport.PlayheadMs() << '\n';
                }
            }
        }
        g_fxPlacedThisTick.clear();
        if (toClear.empty())
            return;
        std::uint32_t cleared = 0;
        std::uint32_t stale = 0;
        std::string error;
        ++g_fxRelocateCalls;
        if (replay::fx::ClearRelocateMarks(toClear, cleared, stale, error))
        {
            g_fxRelocateCleared += cleared;
            g_fxRelocateStale += stale;
        }
        else
        {
            ++g_fxRelocateFailed;
            if (g_fxRelocateError.empty())
                Ctx().log << "FX_MOTION_RULE,part=puppet_no_warp,status=" << error << '\n';
            g_fxRelocateError = error;
            g_fxCfg.puppetNoWarp = false; // a failed check stops the part for this session
            g_fxCfg.relocateClearStill = false; // the same clear
        }
    }
}
