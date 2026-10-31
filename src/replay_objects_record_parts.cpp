// replay_objects_record_parts.cpp: the recording's other steps: weapon retire and owner change, prop retire, the projectile watch, frag pieces and breaks, wheel layout, late map objects, spawn locals, vehicle parents.
// Split from replay_objects.cpp (moved code); shared internals in replay_objects_internal.h.

#include "replay_objects_internal.h"

namespace replay::objects
{

        std::uint64_t ElapsedUs(std::uint64_t start, std::uint64_t end)
        {
            return (end - start) * 1000000ull / replay::prof::TicksPerSecond();
        }

        // Layer capture is a metaped native; a prop object is an entity
        // those natives take any handle for, so the call is guarded like
        // every call whose support for that entity type is unproven.
        bool GuardedCaptureLayers(Entity entity, replay::entity::Appearance* out)
        {
            __try
            {
                replay::entity::appearance::Capture(entity, *out);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // object_check.h: the weapon of a weapon object read from
        // the object itself ([object+0x448], then [weapon+0x68]; rung 2,
        // guarded reads of an offline-proven layout), kept only when the
        // weapon type's model is the object's. A script can hold a weapon
        // object in a ped's hand that is in none of the ped's weapon slots
        // (the five finger fillet knife): the slot test alone recorded it as
        // its bare model, which has no grip (the grip is a component).
        Hash ReadObjectWeapon(Object object, Hash model, bool& hasWeapon)
        {
            hasWeapon = false;
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(object));
            std::uint64_t weapon = 0;
            if (!address || !replay::mem::Read(address + replay::objcheck::kObjectWeaponOffset, weapon) || !weapon)
                return 0;
            hasWeapon = true;
            std::uint32_t hash = 0;
            if (!replay::mem::Read(static_cast<std::uintptr_t>(weapon) + replay::objcheck::kWeaponHashOffset, hash))
                return 0;
            const Hash typeModel = hash ? WEAPON::_GET_WEAPONTYPE_MODEL(hash) : 0;
            return replay::objcheck::ResolveObjectWeapon(hash, typeModel, model);
        }

        // The components a weapon object carries (no native lists them).
        void ReadObjectComponents(Object object, std::vector<std::uint32_t>& out)
        {
            out.clear();
            for (const replay::weapon::Component& component : replay::weapon::kComponents)
                if (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(object, component.hash))
                    out.push_back(component.hash);
        }

        // The engine list checked once against the names: every named
        // component the natives see must be in it, and every hash in it must
        // answer the native (a stale layout fails one of the two).
        bool PoseV2ComponentsAgree(Object object, const std::vector<std::uint32_t>& engine,
            const std::vector<std::uint32_t>& named, std::string& why)
        {
            std::vector<std::uint32_t> sortedNamed = named;
            replay::weaponlook::Normalize(sortedNamed);
            std::vector<std::uint32_t> notInEngine, unnamed;
            replay::weaponlook::Diff(sortedNamed, engine, notInEngine, unnamed);
            if (!notInEngine.empty())
            {
                why = "named_not_in_engine:" + replay::weaponlook::HashList(notInEngine);
                return false;
            }
            for (const std::uint32_t hash : engine)
                if (!WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(object, hash))
                {
                    why = "engine_hash_refused_by_native:" + replay::weaponlook::HashList({hash});
                    return false;
                }
            why = "unnamed=" + std::to_string(unnamed.size());
            return true;
        }

        // The look of a weapon object now (recording). fallback: the
        // create's components (names), used when the engine list is off.
        void ReadPoseV2WeaponLook(Object object, const std::vector<std::uint32_t>* fallback, replay::WeaponCondition& state)
        {
            state = {};
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object));
            replay::weapon::WearFields raw;
            const bool rawOk = g_weaponWearRaw && replay::weapon::ReadWear(address, raw);
            if (rawOk && !g_weaponWearChecked)
            {
                // Once: the raw fields against the getters (degradation's
                // getter answers 0 for some owners, so it is left
                // out of the comparison).
                g_weaponWearChecked = true;
                const float dirt = WEAPON::_GET_WEAPON_DIRT(object);
                const float soot = WEAPON::_GET_WEAPON_SOOT(object);
                const float damage = WEAPON::_GET_WEAPON_DAMAGE(object);
                const bool agree = std::fabs(dirt - raw.dirt) < 0.001f && std::fabs(soot - raw.soot) < 0.001f &&
                    std::fabs(damage - raw.damage) < 0.001f;
                Ctx().log << "WEAPON_LOOK_ENGINE,read=wear,agree=" << (agree ? 1 : 0) << ",native=" << dirt << '/' << soot << '/'
                          << damage << ",raw=" << raw.dirt << '/' << raw.soot << '/' << raw.damage << ",degradation_raw="
                          << raw.degradation << ",degradation_native=" << WEAPON::GET_WEAPON_DEGRADATION(object)
                          << ",permanent_raw=" << raw.permanent << ",permanent_native="
                          << WEAPON::GET_WEAPON_PERMANENT_DEGRADATION(object) << ",e4=" << raw.e4 << '\n';
                if (!agree)
                    g_weaponWearRaw = false;
            }
            if (rawOk && g_weaponWearRaw)
            {
                state.dirt = raw.dirt;
                state.soot = raw.soot;
                state.damage = raw.damage;
                state.degradation = raw.degradation;
                state.permanent = raw.permanent;
                state.wearE4 = raw.e4;
                state.flags |= replay::weaponlook::kFlagWearRaw;
            }
            else
            {
                state.dirt = WEAPON::_GET_WEAPON_DIRT(object);
                state.soot = WEAPON::_GET_WEAPON_SOOT(object);
                state.damage = WEAPON::_GET_WEAPON_DAMAGE(object);
                state.degradation = WEAPON::GET_WEAPON_DEGRADATION(object);
                state.permanent = WEAPON::GET_WEAPON_PERMANENT_DEGRADATION(object);
            }
            const replay::weaponlook::Wear clean = replay::weaponlook::Clean(replay::weaponlook::Wear{
                state.dirt, state.soot, state.damage, state.degradation, state.permanent, state.wearE4});
            state.dirt = clean.dirt;
            state.soot = clean.soot;
            state.damage = clean.damage;
            state.degradation = clean.degradation;
            state.permanent = clean.permanent;
            state.wearE4 = clean.e4;
            if (g_weaponComponentsEngine && replay::weapon::ReadComponentList(address, state.components))
                state.flags |= replay::weaponlook::kFlagComponents;
            else if (fallback && !fallback->empty())
            {
                state.components = *fallback;
                replay::weaponlook::Normalize(state.components);
                state.flags |= replay::weaponlook::kFlagComponentsNames;
            }
            else
                state.components.clear();
        }

        // Appends the state to the object's look track when it changed (or
        // is the first). Returns what changed, for the log.
        const char* NotePoseV2WeaponLook(std::uint32_t recordId, const replay::WeaponCondition& state, std::uint32_t timeMs,
            bool force)
        {
            auto track = std::find_if(Ctx().clip.weaponConditions.begin(), Ctx().clip.weaponConditions.end(),
                [&](const replay::WeaponConditionTrack& entry) { return entry.recordId == recordId; });
            if (track == Ctx().clip.weaponConditions.end())
            {
                Ctx().clip.weaponConditions.push_back({recordId, {}, 0});
                track = Ctx().clip.weaponConditions.end() - 1;
            }
            track->lastCheckMs = timeMs;
            const char* changed = "first";
            if (!track->states.empty())
            {
                const replay::WeaponCondition& last = track->states.back().second;
                const bool components = last.components != state.components;
                const bool wear = replay::weaponlook::WearMoved(WearOf(last), WearOf(state));
                if (!components && !wear && !force)
                    return nullptr;
                changed = components && wear ? "components_wear" : components ? "components" : wear ? "wear" : "forced";
            }
            track->states.push_back({timeMs, state});
            return changed;
        }

        void LogPoseV2WeaponLookRecord(const replay::entity::ReplayId& id, const replay::WeaponCondition& state,
            const char* changed, std::uint32_t timeMs)
        {
            if (!changed || g_weaponLookRecLogs >= kWeaponLookLogCap)
                return;
            ++g_weaponLookRecLogs;
            const replay::logfmt::FormatScope logFormat(Ctx().log);
            Ctx().log << "POSE_V2_WEAPON_LOOK,phase=record,id=" << replay::entity::IdText(id) << ",t=" << timeMs
                      << ",changed=" << changed << ",components=" << state.components.size() << ",components_from="
                      << ((state.flags & replay::weaponlook::kFlagComponents) ? "engine"
                             : (state.flags & replay::weaponlook::kFlagComponentsNames) ? "names" : "none")
                      << ",wear_raw=" << ((state.flags & replay::weaponlook::kFlagWearRaw) ? 1 : 0)
                      << ",wear=" << replay::weaponlook::WearText(WearOf(state))
                      << ",list=" << replay::weaponlook::HashList(state.components, 24) << '\n';
        }

    bool SameStrapState(const replay::weapon::StrapState& a, const replay::weapon::StrapState& b)
    {
        return a.maskBits == b.maskBits && (a.flags & 0x20000u) == (b.flags & 0x20000u) &&
            a.weightA == b.weightA && a.weightB == b.weightB;
    }

    std::string PoseV2WeaponName(Hash weapon)
    {
        const char* name = WEAPON::_GET_WEAPON_NAME(weapon);
        std::string text = name && *name ? name : "unknown";
        std::replace(text.begin(), text.end(), ',', ' ');
        return text;
    }

    std::size_t CountPedBodies()
    {
        std::size_t peds = 0;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            peds += entity.vehicle ? 0u : 1u;
        return peds;
    }

    bool PoseV2ObjectPoseRetryable(const char* status)
    {
        return std::strcmp(status, "skeleton_unresolved") == 0 || std::strcmp(status, "no_place") == 0;
    }

    std::size_t CountWeaponBones(bool posed)
    {
        std::size_t count = 0;
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            // A weapon place is held only while its record runs
            // (posed); a skeleton the engine rebuilt stops it (SamplePoseV2ObjectPose
            // sets posed false) and frees the place.
            count += (posed ? weapon.bonePose && weapon.posed : weapon.boneWatch) ? 1u : 0u;
        return count;
    }

    // object_pose.h kPlayerWeaponReserve: the weapons of
    // the player's ped (the ped whose handle is PLAYER_PED_ID at record).
    bool IsPlayerWeapon(const replay::recorder::RecordedWeapon& weapon)
    {
        return weapon.parent != 0 && weapon.parent == PLAYER::PLAYER_PED_ID();
    }

    void EndPoseV2WeaponBoneWatch(replay::recorder::RecordedWeapon& weapon)
    {
        weapon.boneWatch = false;
        std::vector<float>().swap(weapon.boneWatchLocals);
    }

    void RetirePoseV2Weapon(std::size_t index, const char* reason)
    {
        const replay::recorder::RecordedWeapon weapon = Ctx().recorder.weapons[index];
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        if (weapon.posed)
            replay::pose::capture::Stop(weapon.id.Packed()); // the track stays in the clip
        Ctx().clip.lifecycle.Delete(weapon.id, timeMs);
        Ctx().clip.ids.Release(weapon.id);
        Ctx().recorder.weapons.erase(Ctx().recorder.weapons.begin() + static_cast<std::ptrdiff_t>(index));
        const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(weapon.id.Packed());

        std::ostream& log = Ctx().log;
        log << "POSE_V2_WEAPON_DELETE"
            << ",id=" << replay::entity::IdText(weapon.id)
            << ",handle=" << weapon.object
            << ",reason=" << reason
            << ",samples=" << (track ? track->Size() : 0)
            << ",time_ms=" << timeMs
            << '\n';
        log.flush();
    }

    // A recorded weapon object now on another recorded ped (a
    // disarm, an object handed over). The relative track is sampled against
    // the new parent from now on, so the recording attaches it to the new
    // parent at the same time, with the same kind and attach point the
    // create used; playback composes the track with the parent in force
    // (AttachmentsAt). UpdatePoseV2Attachments reconciles the same state at
    // the end of the scan; this makes the change explicit at the moment it
    // is seen and leaves the evidence line.
    std::uint32_t g_poseV2OwnerChangeLogs = 0;
    void ChangePoseV2WeaponOwner(const replay::recorder::RecordedWeapon& weapon,
        const replay::recorder::RecordedEntity& newOwner, int attachPoint, const char* how)
    {
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        const replay::recorder::RecordedEntity* oldOwner = nullptr;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            if (entity.handle == weapon.parent)
                oldOwner = &entity;
        const bool attached = Ctx().clip.lifecycle.Attach(
            {weapon.id, newOwner.id, replay::entity::AttachmentKind::WeaponPoint, attachPoint}, timeMs);
        if (g_poseV2OwnerChangeLogs >= 64)
            return;
        ++g_poseV2OwnerChangeLogs;
        Ctx().log << "POSE_V2_WEAPON_OWNER_CHANGE,id=" << replay::entity::IdText(weapon.id)
            << ",from=" << (oldOwner ? replay::entity::IdText(oldOwner->id) : std::string("none"))
            << ",to=" << replay::entity::IdText(newOwner.id)
            << ",how=" << how
            << ",attach_point=" << attachPoint
            << ",lifecycle_attach=" << (attached ? 1 : 0)
            << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(weapon.object)) << std::dec
            << ",time_ms=" << timeMs << '\n';
    }

    // The burn level of every recorded weapon object (the meat on the
    // spit, an attached object) and prop (the campfire), the value
    // _SET_OBJECT_BURN_LEVEL writes (its handler and core: entity +0x28,
    // +0x18, block from the virtual +0x88, then the setter's `movss
    // [rcx+0x1C], xmm1`; every script call has affectAsh true, so +0x20 is
    // never the one): burn::Read on the fire path, four times a second per
    // object, stored by NoteBurnLevel (the first read always: the replay's
    // object starts with the model's level, the cooking script's meat with 0).
    // A guarded memory read, no native. Ini ObjectBurnLevel (1 by default)
    // records and plays it.
    bool g_objectBurnLevel = true;
    std::uint32_t g_objectBurnEntryLogs = 0;
    std::uint32_t g_objectBurnLogs = 0;

    // `key` is a packed id, or a scenery index with `scenery`.
    void LogPoseV2ObjectBurn(const char* phase, std::uint32_t key, bool scenery, const replay::burn::State& state,
        Object object, std::uint32_t timeMs)
    {
        Ctx().log << "POSE_V2_BURN_LEVEL,phase=" << phase << ",id="
                  << (scenery ? "scenery:" + std::to_string(key) : replay::entity::IdText(replay::entity::ReplayId::FromPacked(key)))
                  << ",model=0x" << std::hex
                  << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) << ",fire_vt=0x" << state.fireVtableRva << std::dec
                  << ",level=" << state.level << ",no_ash=" << state.levelNoAsh << ",intensity=" << state.intensity
                  << ",opacity=" << state.opacity << ",visible=" << (ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0)
                  << ",time_ms=" << timeMs << '\n';
    }

    // The same for a scenery entry (its index in Recording::scenery), on the
    // scenery watch's beat (250 ms), until a prop takes it over.
    void SamplePoseV2SceneryBurnLevel(std::size_t entry, Object object, std::uint32_t timeMs)
    {
        if (!g_objectBurnLevel || !replay::burn::Enabled())
            return;
        static replay::TrackIndex<replay::BurnLevelTrack> index;
        NotePoseV2ObjectBurn(Ctx().clip.sceneryBurnLevels, index, static_cast<std::uint32_t>(entry), object, true, timeMs);
    }

    // Per frame: one relative sample per weapon at the capture rate.
    // The weapon's look, twice a second, stored when it moves.
    // weapon_look_core.h: the whole look (components the
    // engine lists, the six wear fields), stored when one changes.
    void SamplePoseV2WeaponCondition(const replay::recorder::RecordedWeapon& weapon, std::uint32_t timeMs)
    {
        const auto track = std::find_if(Ctx().clip.weaponConditions.begin(), Ctx().clip.weaponConditions.end(),
            [&](const replay::WeaponConditionTrack& entry) { return entry.recordId == weapon.id.Packed(); });
        if (track != Ctx().clip.weaponConditions.end() && timeMs - track->lastCheckMs < 500)
            return;
        const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(weapon.id.Packed());
        replay::WeaponCondition state;
        ReadPoseV2WeaponLook(weapon.object, create ? &create->weaponComponents : nullptr, state);
        LogPoseV2WeaponLookRecord(weapon.id, state, NotePoseV2WeaponLook(weapon.id.Packed(), state, timeMs, false), timeMs);
    }

    // Props that come off a recorded ped (a knocked off hat), as the
    // reference records a detached ped prop: an object entity of its own,
    // created when it leaves the ped, with its world transform sampled at
    // the capture rate. The ped's pose segment removes the prop's layer at
    // the same moment. The track lives with the weapon tracks, keyed by
    // the prop's ReplayId; its samples are world transforms.

    std::uint32_t g_poseV2PropsDropped = 0; // loose objects not recorded: the registry was full
    std::uint32_t g_poseV2RopeModelsSkipped = 0; // rope entities left to the rope track
    std::uint32_t g_poseV2RopeModelLogs = 0;
    namespace
    {
    void EndPoseV2ProjectileWatchOf(Object object, const char* reason);
    }

    std::size_t CountPropBoneWatches()
    {
        std::size_t watching = 0;
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            watching += prop.boneWatch ? 1u : 0u;
        return watching;
    }

    void EndPoseV2PropBoneWatch(replay::recorder::RecordedProp& prop)
    {
        prop.boneWatch = false;
        std::vector<float>().swap(prop.boneWatchLocals);
    }

    void RetirePoseV2Prop(std::size_t index, const char* reason)
    {
        const replay::recorder::RecordedProp prop = Ctx().recorder.props[index];
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        if (prop.posed)
            replay::pose::capture::Stop(prop.id.Packed()); // the track stays in the clip
        Ctx().clip.lifecycle.Delete(prop.id, timeMs);
        Ctx().clip.ids.Release(prop.id);
        Ctx().recorder.props.erase(Ctx().recorder.props.begin() + static_cast<std::ptrdiff_t>(index));
        EndPoseV2ProjectileWatchOf(prop.object, reason);
        const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(prop.id.Packed());
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PROP_DELETE"
            << ",id=" << replay::entity::IdText(prop.id)
            << ",handle=" << prop.object
            << ",reason=" << reason
            << ",samples=" << (track ? track->Size() : 0)
            << ",posed=" << (prop.posed ? 1 : 0)
            << ",time_ms=" << timeMs
            << '\n';
        log.flush();
    }

    void RetirePoseV2PropOf(Object object, const char* reason)
    {
        for (std::size_t index = Ctx().recorder.props.size(); index-- > 0;)
        {
            if (Ctx().recorder.props[index].object == object)
                RetirePoseV2Prop(index, reason);
        }
    }

    // A weapon that leaves its ped continues as a dropped weapon prop. The
    // engine either leaves the same object in the world, or replaces it with a
    // new object of the same model where it was: that one is looked for for a
    // short while.

    bool IsPoseV2KnownObject(Entity object) { return Ctx().recorder.KnowsObject(object); }


    // Per frame: a new dropped hat of a recorded ped enters the recording,
    // and every recorded prop gets one world sample at the capture rate.
    // Projectiles: an arrow is a plain pool object (model 0x445c85a5), free
    // and fast once shot, that no weapon swap announces. Like the other objects
    // of the scene, every new free object moving fast near a recorded ped is
    // recorded as a prop with its world track, owned by the nearest recorded
    // ped. Only scanned for a short while after a recorded ped shoots.
    ULONGLONG g_poseV2ProjectileScanUntil = 0;
    std::uint32_t g_poseV2Projectiles = 0;
    std::uint32_t g_poseV2ProjectileSuccessors = 0;
    std::vector<ProjectileWatch> g_poseV2ProjectileWatch;
    std::uint32_t g_poseV2ProjectileFateLogs = 0;
    std::uint32_t g_poseV2ProjectileWatchLogs = 0;

    // Evidence, one line per watched projectile when its watch
    // ends: stuck_world / stuck_ped (itself or its successor), converted
    // (a successor of another model), picked_up, lost.
    void LogPoseV2ProjectileFate(const ProjectileWatch& watch, const char* fate, Object successor, const char* how)
    {
        if (g_poseV2ProjectileFateLogs >= 128)
            return;
        ++g_poseV2ProjectileFateLogs;
        Ctx().log << "POSE_V2_PROJECTILE_FATE,id=" << replay::entity::IdText(watch.id)
            << ",model=0x" << std::hex << watch.model << ",weapon=0x" << watch.weaponHash << std::dec
            << ",fate=" << fate << ",how=" << how << ",successor=" << successor
            << ",successor_model=0x" << std::hex
            << (successor && replay::entities::Exists(successor) ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(successor)) : 0u)
            << std::dec << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
    }

    // Where an object ended up: stuck in a ped (attached to one) or in the world.
    const char* PoseV2StuckFate(Object object)
    {
        const Entity parent = ENTITY::IS_ENTITY_ATTACHED(object) ? ENTITY::GET_ENTITY_ATTACHED_TO(object) : 0;
        return parent && replay::entities::IsPed(parent) ? "stuck_ped" : "stuck_world";
    }

    void WatchPoseV2Throwable(Object object, replay::entity::ReplayId id, replay::entity::ReplayId ownerId,
        Entity ownerHandle, Hash weaponHash)
    {
        if (std::any_of(g_poseV2ProjectileWatch.begin(), g_poseV2ProjectileWatch.end(),
                [&](const ProjectileWatch& watch) { return watch.object == object; }))
            return;
        ProjectileWatch watch;
        watch.object = object;
        watch.owner = ownerId;
        watch.ownerHandle = ownerHandle;
        watch.last = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
        watch.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
        watch.id = id;
        watch.weaponHash = weaponHash;
        g_poseV2ProjectileWatch.push_back(watch);
        if (g_poseV2ProjectileWatchLogs >= 128)
            return;
        ++g_poseV2ProjectileWatchLogs;
        Ctx().log << "POSE_V2_PROJECTILE_WATCH,id=" << replay::entity::IdText(id) << ",model=0x" << std::hex << watch.model
            << ",weapon=0x" << weaponHash << std::dec << ",speed=" << ENTITY::GET_ENTITY_SPEED(object)
            << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
    }
    namespace
    {

    // A watched prop leaves the recording while its object still exists:
    // picked up (weapon point, or attached to its thrower) or stuck in a
    // ped (attached to another one). Gone objects keep their watch: the
    // successor search decides.
    void EndPoseV2ProjectileWatchOf(Object object, const char* reason)
    {
        for (std::size_t index = g_poseV2ProjectileWatch.size(); index-- > 0;)
        {
            const ProjectileWatch watch = g_poseV2ProjectileWatch[index];
            if (watch.object != object || !replay::entities::Exists(object))
                continue;
            const bool pickedUp = std::strcmp(reason, "picked_up") == 0;
            const bool attached = std::strcmp(reason, "attached") == 0;
            if (!pickedUp && !attached)
                continue;
            const bool toThrower = attached && ENTITY::GET_ENTITY_ATTACHED_TO(object) == watch.ownerHandle;
            LogPoseV2ProjectileFate(watch, pickedUp || toThrower ? "picked_up" : PoseV2StuckFate(object), object, reason);
            g_poseV2ProjectileWatch.erase(g_poseV2ProjectileWatch.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    }
    // The object listing of the previous scan: a successor is an object the
    // engine creates, absent there (otherwise the dead ped's hat next to
    // the stuck arrow would be taken).
    std::vector<int> g_poseV2ProjectilePrevObjects;
    std::unordered_map<Object, WorldSnap> g_worldSnapshot;
    std::vector<NearWorldObject> g_nearWorldObjects;
    std::uint32_t g_nearWorldTaken = 0;    // recorded by the every-tick read
    std::uint32_t g_openAtStartTaken = 0;  // recorded because open at the snapshot
    std::vector<WorldTaken> g_worldTaken;
    std::uint32_t g_worldAnimatedRecorded = 0;
    std::uint32_t g_worldAnimatedWatched = 0;
    std::uint32_t g_worldLateMapObjects = 0;   // map objects first seen after the snapshot
    std::unordered_set<Object> g_worldLateJoinDecided; // looked at once for the late join
    std::vector<SceneryLive> g_sceneryLive;
    std::unordered_map<Object, std::size_t> g_sceneryOf;
    std::unordered_map<Object, SceneryPendingObject> g_sceneryPending;
    std::unordered_set<Object> g_scenerySeen;
    std::unordered_map<Object, SceneryFar> g_sceneryFar;
    std::uint32_t g_lateSceneryLogs = 0; // POSE_V2_SCENERY_LATE lines
    namespace
    {
    constexpr std::uint32_t kLateSceneryLogCap = 48;
    }
    std::uint32_t g_sceneryNextScanMs = 0;
    std::unordered_map<Object, LootWatch> g_lootWatch;
    std::vector<LootGonePending> g_lootGonePending;
    std::unordered_map<Object, std::uint32_t> g_lootFirstSeenMs; // a late object's first sight
    std::unordered_set<Object> g_lootDecided;                    // looked at within reach, not watched again
    std::uint32_t g_lootLogs = 0;
    // The map's own owner value, learned at the snapshot: the most frequent
    // owner among the objects there that are no scenery. Logged only
    // (map_owner=); the rules use the map data owners proven offline
    // (objcheck::IsMapDataOwner).
    std::uint32_t g_mapOwner = replay::objcheck::kOwnerUnknown;
    std::vector<MapDressingPending> g_mapDressingPending;
    // The scenery_by_rule counts of the snapshot (objcheck::SceneryRule).
    std::uint32_t g_snapshotRules[replay::objcheck::kSceneryRuleCount]{};
    std::vector<DressingGonePending> g_dressingGonePending;
    std::vector<SceneryChildLive> g_sceneryChildLive;
    std::unordered_set<Object> g_sceneryChildObjects;
    std::unordered_set<std::size_t> g_sceneryChildScanned; // scenery entries whose children were looked for once

    // An object of the map (a map data owner, objcheck::IsMapDataOwner), not a
    // script's.
    bool PoseV2MapObject(Object object)
    {
        const std::uint32_t owner = ReadEntityOwner(object);
        return replay::objcheck::IsMapDataOwner(owner) && ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) == FALSE;
    }

    // A pickup object that became a scenery entry (the last one
    // added), one line each up to the loot cap.
    void LogPoseV2PickupEntry(const char* why)
    {
        if (Ctx().clip.scenery.empty() || g_lootLogs >= kLootLogCap)
            return;
        ++g_lootLogs;
        const replay::SceneryObject& entry = Ctx().clip.scenery.back();
        char line[256];
        std::snprintf(line, sizeof(line),
            "POSE_V2_LOOT,phase=record,why=%s,model=0x%x,pos=%.2f;%.2f;%.2f,appear_ms=%u,owner=%u,mission=%u,visible=%u\n", why,
            entry.model, entry.position[0], entry.position[1], entry.position[2], entry.appearMs, entry.owner, entry.mission,
            entry.visible);
        Ctx().log << line;
    }

    replay::entity::ReplayId KnownObjectId(Object object)
    {
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            if (prop.object == object)
                return prop.id;
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            if (weapon.object == object)
                return weapon.id;
        return {};
    }

    // The two models every ped gore piece of 484 recordings had (g_goreModels'
    // seed), the clip's world models, and the counters of the gore piece rules.
    bool IsPedGoreModel(std::uint32_t model) { return model == 0x5595880bu || model == 0x4f6fa8e4u; }
    bool PoseV2ClipWorldModel(std::uint32_t model)
    {
        for (const replay::WorldObjectOrigin& origin : Ctx().clip.worldObjects)
            if (origin.model == model)
                return true;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (packet.modelHash == model && packet.kind == replay::entity::EntityKind::Prop &&
                (packet.propOrigin == replay::entity::PropOrigin::WorldObject ||
                    packet.propOrigin == replay::entity::PropOrigin::WorldPiece))
                return true;
        return false;
    }
    std::uint32_t g_goreWorldPiecesRefused = 0; // recording: a world model's piece not taken as gore
    std::uint32_t g_goreWorldModelRefused = 0;  // playback: an old clip's gore piece of a world model not built
    std::uint32_t g_goreRefrozen = 0;           // playback: an adopted piece frozen again
    std::uint32_t g_worldObjectsRecorded = 0;
    std::uint32_t g_worldPiecesRecorded = 0;
    // Fragment breaks (evidence): the parent's address matched to
    // the snapshot, the groups that came off.
    bool g_fragReady = false;
    namespace
    {
    bool g_fragTried = false;
    }

    void WarmUpFrag()
    {
        if (g_fragTried)
            return;
        g_fragTried = true;
        std::string error;
        g_fragReady = replay::frag::Install(error);
        Ctx().log << "POSE_V2_FRAG_OBSERVER,status=" << (g_fragReady ? "installed" : "failed") << ",error="
            << (g_fragReady ? "none" : error.c_str()) << '\n';
    }
    std::uint64_t g_fragCursor = 0;
    std::uint32_t g_fragBreaks = 0;
    std::uint32_t g_fragBreaksMatched = 0;
    std::uint32_t g_fragLogged = 0;
    std::vector<Object> g_fragBrokenObjects;
    std::vector<std::pair<Object, replay::frag::BreakCall>> g_fragPending;
    std::vector<PendingVehiclePiece> g_vehiclePiecesPending;
    std::uint32_t g_vehiclePiecesRecorded = 0;
    std::uint32_t g_vehiclePiecesLost = 0;
    // The vehicles whose part track samples at once (a break without a
    // fragment piece), and the wheels that came off waiting for their object.
    std::unordered_set<std::uint32_t> g_vehiclePartsUrgent;
    std::vector<PendingWheelPiece> g_wheelPiecesPending;
    std::uint32_t g_wheelPieceLogs = 0;
    int g_wheelLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch (vehicle_pieces.h WheelLayoutProven)
    namespace
    {
    // The object at `address` is recorded, or hangs on a recorded entity.
    bool PoseV2RecordedOrOnRecorded(std::uint64_t address);
    constexpr std::size_t kFragPieceCalls = 64;
    }
    std::vector<FragPieceCall> g_fragPieceCalls;
    std::uint32_t g_fragPiecesRecorded = 0;
    std::uint32_t g_fragPiecesFromCache = 0;
    std::uint32_t g_fragPieceLogs = 0;
    namespace
    {

    // Apart from CollectPoseV2FragBreaks: its __try allows no objects to
    // unwind.
    void RememberPoseV2FragPiece(const replay::frag::BreakCall& call)
    {
        if (!call.pieceInst || !call.copied)
            return;
        FragPieceCall piece;
        piece.entity = call.pieceEntity;
        piece.inst = call.pieceInst;
        piece.groups = replay::fragpiece::GroupsOfBits(call.groups, sizeof(call.groups));
        if (piece.groups.empty())
            return;
        piece.parentEntity = call.parentEntity;
        piece.parentRecorded = PoseV2RecordedOrOnRecorded(call.parentEntity);
        if (g_fragPieceCalls.size() >= kFragPieceCalls)
            g_fragPieceCalls.erase(g_fragPieceCalls.begin());
        g_fragPieceCalls.push_back(std::move(piece));
    }

    // The object is a piece a recorded object's break made (FragPieceCall
    // parentRecorded): it is watched whatever its distance to the peds.
    bool PoseV2RecordedOrOnRecorded(std::uint64_t address)
    {
        if (!address)
            return false;
        bool available = false;
        const Entity parent = replay::entities::HandleAtAddress(address, available);
        if (!parent || !replay::entities::Exists(parent))
            return false;
        if (IsPoseV2KnownObject(parent) || Ctx().recorder.FindByHandle(parent) != nullptr)
            return true;
        const Entity holder = ENTITY::GET_ENTITY_ATTACHED_TO(parent);
        return holder && (IsPoseV2KnownObject(holder) || Ctx().recorder.FindByHandle(holder) != nullptr);
    }
    }

    bool PoseV2PieceOfRecorded(Object object)
    {
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object));
        if (!address)
            return false;
        for (FragPieceCall& piece : g_fragPieceCalls)
        {
            if (!piece.entity)
                piece.entity = replay::frag::EntityOfInst(piece.inst);
            if (piece.entity == address)
                return piece.parentRecorded || (piece.parentRecorded = PoseV2RecordedOrOnRecorded(piece.parentEntity));
        }
        return false;
    }

    // A prop registered for a loose piece keeps the groups it carries: the
    // break that made it (the groups that left the parent), else its own
    // cache entry when its root is not the type's (a piece whose break was
    // not seen). Weapons, hats, gore and vehicle pieces have their own paths.
    void NotePoseV2FragPiece(Object object, replay::entity::ReplayId id, replay::entity::PropOrigin origin, Hash weapon)
    {
        if (weapon || origin == replay::entity::PropOrigin::VehiclePiece || origin == replay::entity::PropOrigin::GorePiece ||
            origin == replay::entity::PropOrigin::DroppedHat)
            return;
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object));
        if (!address)
            return;
        int call = -1;
        for (std::size_t n = g_fragPieceCalls.size(); n-- > 0 && call < 0;)
        {
            FragPieceCall& piece = g_fragPieceCalls[n];
            if (!piece.entity)
                piece.entity = replay::frag::EntityOfInst(piece.inst);
            if (piece.entity == address)
                call = static_cast<int>(n);
        }
        // 2 KiB of parents: static, script thread only.
        static replay::frag::ObjectFragState state;
        const bool read = replay::frag::ReadObjectFrag(address, state);
        std::vector<std::uint16_t> cached;
        if (read && state.cached && state.root > 0)
            cached = replay::fragpiece::CarriedFromCache(state.parents, state.groups, state.root, state.gone);
        replay::FragPiece row;
        row.id = id;
        if (call >= 0)
        {
            row.source = static_cast<std::uint8_t>(replay::fragpiece::Source::Break);
            row.groups = std::move(g_fragPieceCalls[static_cast<std::size_t>(call)].groups);
            g_fragPieceCalls.erase(g_fragPieceCalls.begin() + call);
        }
        else if (!cached.empty())
        {
            row.source = static_cast<std::uint8_t>(replay::fragpiece::Source::Cache);
            row.groups = cached;
            ++g_fragPiecesFromCache;
        }
        if (!replay::fragpiece::ValidGroups(row.groups))
            return;
        ++g_fragPiecesRecorded;
        if (g_fragPieceLogs < 32)
        {
            ++g_fragPieceLogs;
            std::string list;
            for (std::size_t n = 0; n < row.groups.size() && n < 12; ++n)
                list += (n ? "|" : "") + std::to_string(row.groups[n]);
            // cache_*: the piece's own cache entry, the cross-check of the
            // break's groups (the same set when the root reading holds).
            Ctx().log << "POSE_V2_FRAG_PIECE,id=" << replay::entity::IdText(id) << ",source="
                << replay::fragpiece::SourceText(row.source) << ",root=" << replay::fragpiece::Root(row.groups)
                << ",groups=" << row.groups.size() << ",list=" << list << ",type_groups=" << (read ? state.groups : -1)
                << ",cache_root=" << (read && state.cached ? state.root : -1) << ",cache_groups=" << cached.size()
                << ",cache_same=" << (cached == row.groups ? 1 : 0) << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
        }
        Ctx().clip.fragPieces.push_back(std::move(row));
    }

    // Pending breaks whose object is now a recorded prop become events.
    void FlushPoseV2FragBreaks()
    {
        for (std::size_t n = g_fragPending.size(); n-- > 0;)
        {
            const auto& [object, call] = g_fragPending[n];
            const auto prop = std::find_if(Ctx().recorder.props.begin(), Ctx().recorder.props.end(),
                [&](const replay::recorder::RecordedProp& recorded) { return recorded.object == object; });
            if (prop == Ctx().recorder.props.end())
                continue;
            const std::uint64_t ageUs = replay::frag::NowUs() > call.timeUs ? replay::frag::NowUs() - call.timeUs : 0;
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            const std::uint32_t atMs = nowMs > ageUs / 1000 ? nowMs - static_cast<std::uint32_t>(ageUs / 1000) : 0;
            for (int bit = 0; bit < 0x80 * 8; ++bit)
                if ((call.groups[bit / 8] >> (bit % 8)) & 1)
                    Ctx().clip.fragBreaks.push_back({atMs, prop->id, static_cast<std::uint16_t>(bit)});
            g_fragPending.erase(g_fragPending.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    namespace
    {

    // A recorded vehicle's fragment (a wagon losing boards, wheels, cargo):
    // its broken groups become events of the vehicle, so the puppet does not
    // stay whole. Apart from CollectPoseV2FragBreaks: its __try allows no
    // objects to unwind.
    bool RecordVehicleFragBreak(const replay::frag::BreakCall& call)
    {
        if (!call.parentEntity || !call.copied)
            return false;
        const auto vehicle = std::find_if(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
            [&](const replay::recorder::RecordedEntity& entity) {
                return entity.vehicle && replay::entities::Exists(entity.handle) &&
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == call.parentEntity;
            });
        if (vehicle == Ctx().recorder.entities.end())
            return false;
        const std::uint64_t ageUs = replay::frag::NowUs() > call.timeUs ? replay::frag::NowUs() - call.timeUs : 0;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        const std::uint32_t atMs = nowMs > ageUs / 1000 ? nowMs - static_cast<std::uint32_t>(ageUs / 1000) : 0;
        int groups = 0;
        for (int bit = 0; bit < 0x80 * 8; ++bit)
            if ((call.groups[bit / 8] >> (bit % 8)) & 1)
            {
                Ctx().clip.fragBreaks.push_back({atMs, vehicle->id, static_cast<std::uint16_t>(bit)});
                ++groups;
            }
        ++g_fragBreaksMatched;
        // A group that came off as a piece of its own (a wheel,
        // a door, a board flying off) is recorded as that object: found in
        // the pool by its address on the script thread, then a prop.
        const int root = replay::frag::LowestGroup(call.groups);
        if (call.pieceInst && root > 0)
        {
            const std::uint64_t entity = call.pieceEntity ? call.pieceEntity : replay::frag::EntityOfInst(call.pieceInst);
            g_vehiclePiecesPending.push_back({entity, call.pieceInst, vehicle->id, vehicle->handle,
                static_cast<std::uint16_t>(root), GetTickCount64() + 1000});
        }
        else if (!call.pieceInst)
        {
            // Groups deleted with no piece of the fragment: a wheel comes off
            // as an object of its own. The part track samples this vehicle at
            // once (not at its 4 Hz turn), and a new wheel bit there records
            // the wheel object (NoteVehicleWheelOff).
            g_vehiclePartsUrgent.insert(vehicle->id.Packed());
        }
        if (g_fragLogged++ < 20)
            Ctx().log << "POSE_V2_FRAG_BREAK,where=vehicle,id=" << replay::entity::IdText(vehicle->id)
                << ",groups=" << groups << ",root=" << root << ",piece=" << (call.pieceInst ? 1 : 0)
                << ",time_ms=" << atMs << '\n';
        return true;
    }

    // The wheel object layout's code checked once (rung 2: a mismatch leaves
    // the wheels to the old path, logged); then the read of a vehicle's wheel
    // slot.
    bool PoseV2WheelLayout()
    {
        if (g_wheelLayout == 0)
        {
            g_wheelLayout = replay::vehicle_pieces::WheelLayoutProven(
                [](const replay::sig::Signature& s) { return replay::sig::Find(s) != 0; }) ? 1 : -1;
            Ctx().log << "POSE_V2_WHEEL_LAYOUT,status=" << (g_wheelLayout > 0 ? "proven" : "signature_mismatch")
                      << ",engine_writes=0\n";
        }
        return g_wheelLayout > 0;
    }
    }

    const char* ReadPoseV2WheelObject(Entity vehicle, int slot, replay::vehicle_pieces::WheelRead& out)
    {
        out = {};
        if (!PoseV2WheelLayout())
            return "layout";
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(vehicle));
        return replay::vehicle_pieces::ReadWheelObject(
            [](std::uint64_t at, void* into, std::size_t size) {
                return replay::mem::Copy(into, static_cast<std::uintptr_t>(at), size);
            },
            address, slot, out);
    }
    namespace
    {

    // A map object that became real after the snapshot joins it
    // where it stands (apart from CollectPoseV2FragBreaks: its __try allows
    // no objects to unwind).
    bool SnapshotPoseV2LateMapObject(Object object)
    {
        if (g_worldSnapshot.count(object) || IsPoseV2KnownObject(object) || ENTITY::IS_ENTITY_ATTACHED(object) ||
            !PoseV2MapObject(object) || replay::ropemodels::IsRopeModel(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object))))
            return false;
        WorldSnap snap;
        snap.position = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
        snap.rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
        snap.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
        g_worldSnapshot[object] = snap;
        ++g_worldLateMapObjects;
        return true;
    }
    }

    // An object of the map by any of its owners (objcheck::IsMapDataOwner),
    // not a script's, a pickup or a rope's.
    bool PoseV2MapOwned(Object object)
    {
        const std::uint32_t owner = ReadEntityOwner(object);
        if (!replay::objcheck::IsMapDataOwner(owner))
            return false;
        if (ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) || OBJECT::DOES_PICKUP_OBJECT_EXIST(object) ||
            OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object))
            return false;
        return !replay::ropemodels::IsRopeModel(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)));
    }

    // objcheck::LateJoinWatch: the map object joins the world watch where it
    // stands, its skeleton unread (the reread near the actor reads it: a
    // skeleton coming away from its rest pose is the drawer opening).
    bool JoinPoseV2LateMapObject(Object object, const Vector3& at)
    {
        if (g_worldSnapshot.count(object) || IsPoseV2KnownObject(object))
            return false;
        WorldSnap snap;
        snap.position = at;
        snap.rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
        snap.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
        snap.broken = OBJECT::HAS_OBJECT_BEEN_BROKEN(object) != FALSE;
        snap.skeleton = replay::objcheck::SkeletonWatch::Unread;
        g_worldSnapshot[object] = std::move(snap);
        return true;
    }

    void CollectPoseV2FragBreaks()
    {
        if (!g_fragReady)
            return;
        static std::vector<replay::frag::BreakCall> breaks;
        replay::frag::CollectSince(g_fragCursor, breaks);
        for (const replay::frag::BreakCall& call : breaks)
        {
            ++g_fragBreaks;
            if (RecordVehicleFragBreak(call))
                continue;
            RememberPoseV2FragPiece(call);
            Object matched = 0;
            for (const auto& [object, snap] : g_worldSnapshot)
                if (replay::entities::Exists(object) &&
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object)) == call.parentEntity)
                {
                    matched = object;
                    break;
                }
            int groups = 0;
            int first = -1;
            for (int bit = 0; bit < 0x80 * 8; ++bit)
                if (call.copied && (call.groups[bit / 8] >> (bit % 8)) & 1)
                {
                    ++groups;
                    if (first < 0)
                        first = bit;
                }
            const char* where = matched ? "snapshot" : call.parentEntity ? "none" : "no_entity";
            if (!matched)
            {
                // Any listed object (one that appeared after the snapshot).
                static int objects[1024];
                const int count = (std::min)(ListObjects(objects, 1024), 1024);
                for (int slot = 0; slot < count && !matched; ++slot)
                    if (objects[slot] && call.parentEntity && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == call.parentEntity)
                    {
                        matched = objects[slot];
                        where = "listed";
                    }
            }
            std::uint64_t vtable = 0;
            if (!matched && call.parent)
            {
                __try
                {
                    vtable = *reinterpret_cast<const std::uint64_t*>(call.parent);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    vtable = 0;
                }
            }
            // A map object that became real after the snapshot
            // (the crate the cavalry rode into) joins it where it stands, so
            // it is recorded as a world object (stand-in, original hidden,
            // breaks played) and not as a free object.
            if (matched && SnapshotPoseV2LateMapObject(matched))
                where = "late_map";
            if (matched)
            {
                ++g_fragBreaksMatched;
                g_fragBrokenObjects.push_back(matched);
            }
            // The groups become events of the object's prop (registered now
            // by the scan below when it was not yet: a break is a change).
            if (matched && call.copied)
                g_fragPending.push_back({matched, call});
            if (g_fragLogged++ < 20)
                Ctx().log << "POSE_V2_FRAG_BREAK,where=" << where << ",parent_vtable_rva=0x" << std::hex
                    << (vtable ? vtable - reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)) : 0) << std::dec
                    << ",parent_handle=" << matched << ",model=0x" << std::hex
                    << (matched ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(matched)) : 0u) << std::dec
                    << ",groups=" << groups << ",first_group=" << first << ",piece_inst=" << (call.pieceInst ? 1 : 0)
                    << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
        }
    }

    float Distance2(const Vector3& a, const Vector3& b)
    {
        return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
    }

    float AngleDelta(float a, float b)
    {
        float d = std::fabs(a - b);
        while (d > 180.0f)
            d = std::fabs(d - 360.0f);
        return d;
    }
    std::vector<PoseV2ScanObject> g_poseV2ScanObjects;
    int g_poseV2ScanListed = 0;
    std::unordered_map<Entity, PoseV2PointScan> g_poseV2PointScans;
    std::uint64_t g_poseV2PointScansFull = 0;
    std::uint64_t g_poseV2PointScansSkipped = 0;

    void CollectPoseV2ScanObjects()
    {
        REPLAY_PROFILE_ZONE("rec.objects_list");
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        g_poseV2ScanListed = count;
        g_poseV2ScanObjects.clear();
        for (int slot = 0; slot < count; ++slot)
        {
            const Object object = objects[slot];
            if (!object || !replay::entities::Exists(object))
                continue;
            g_poseV2ScanObjects.push_back({object, ENTITY::GET_ENTITY_ATTACHED_TO(object)});
        }
    }

    // objcheck::NearWatch: the bones of the snapshot objects within reach of
    // a recorded actor, read every tick. Four reads a second missed the first
    // quarter second of a drawer that opens in half of one (the stand-in drew
    // it closed while live slid it out: 0.23 m at the first mark). One read
    // is a copy of the object's locals (BoneQts, 48 bytes a bone). An object
    // open at the snapshot is recorded at the first read, with its pose.
    void SamplePoseV2NearWorldObjects()
    {
        if (g_nearWorldObjects.empty())
            return;
        REPLAY_PROFILE_ZONE("rec.world_near");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        static std::vector<float> locals;
        for (const NearWorldObject& entry : g_nearWorldObjects)
        {
            const auto snap = g_worldSnapshot.find(entry.object);
            if (snap == g_worldSnapshot.end() || snap->second.taken || snap->second.locals.empty() ||
                !replay::entities::Exists(entry.object) || IsPoseV2KnownObject(entry.object))
                continue;
            WorldSnap& was = snap->second;
            std::uint32_t bones = 0;
            // A skeleton rebuilt with another bone count is the scan's (AfterRebuild).
            if (!ReadPoseV2SpawnLocals(entry.object, bones, locals) || bones != was.bones)
                continue;
            const bool open = was.openAtStart;
            if (!open && !replay::object_pose::spawn::LocalsChanged(was.locals.data(), locals.data(), bones))
                continue;
            if (open)
                ++g_openAtStartTaken;
            else
                ++g_nearWorldTaken;
            TakePoseV2WorldObject(entry.object, was, entry.actor, entry.actorHandle, open ? "world_open" : "world_animated", true,
                nowMs);
        }
    }

    // object_pose.h spawn objects the engine creates next to a recorded ped
    // during the recording and leaves free in the world (an animation places
    // them without an attachment). The other paths only take objects attached
    // to a recorded ped, objects of the start snapshot that moved, their
    // pieces, shots and drops.
    // Each such object is watched from its first sight; it becomes a prop
    // (world track, owned by the nearest recorded ped, Projectile origin like
    // the other free objects of a ped) when it moves or its skeleton's locals
    // change, and is posed when it is animated. POSE_V2_OBJECT_SPAWN lines name
    // every object first seen within kNearM of a recorded ped (also the ones
    // attached to an entity that is not recorded), with its model.
    // objcheck::SpawnSceneryOwner: a watched object that stands still becomes a
    // late scenery entry when it is scenery by the proven owners (a script's, a
    // temporary object, a prop set's, a pickup); another owner that is not the
    // map's is only counted (would_scenery).
    std::unordered_map<Object, PoseV2SpawnWatch> g_spawnWatch;
    std::unordered_set<Object> g_spawnSeen;
    std::unordered_map<Object, PoseV2SpawnRecheck> g_spawnRecheck;
    std::uint32_t g_spawnLogs = 0;
    // What a rewatch recorded or refused has a line budget of its own (the
    // first sights of a town spend the 64 above in the first seconds: 139 had
    // them all by 8.8 s).
    std::uint32_t g_spawnLateLogs = 0;

    // The object's local pose (rung 2: the resolver's offline-proven walk,
    // then a guarded copy of the locals, the first half of the pose buffer).
    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out)
    {
        bones = 0;
        replay::skeleton::ResolvedPoseSkeleton skeleton{};
        if (!replay::skeleton::FastResolvePedPoseSkeleton(object, skeleton))
            return false;
        bones = skeleton.boneCount;
        if (bones < replay::object_pose::kMinBones || bones > replay::object_pose::spawn::kMaxBones ||
            !skeleton.poseBuffer)
            return false;
        out.resize(static_cast<std::size_t>(bones) * replay::object_pose::spawn::kFloatsPerBone);
        return replay::mem::Copy(out.data(), skeleton.poseBuffer, out.size() * sizeof(float));
    }
    namespace
    {

    // Locals just read against the skeleton's bind pose (rung 2: the layout
    // tpose_check.h proves from the pose finalizer's code, its signature
    // checked once; a mismatch leaves every verdict unknown, the old rule).
    int g_restBindLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch
    }
    replay::objcheck::RestState PoseV2RestOf(Object object, const std::vector<float>& locals, std::uint32_t bones,
        replay::objcheck::RestMatch& match)
    {
        match = {};
        if (g_restBindLayout == 0)
        {
            g_restBindLayout = replay::tpose::BindLayoutProven(
                [](const replay::sig::Signature& site) { return replay::sig::Find(site) != 0; }) ? 1 : -1;
            Ctx().log << "POSE_V2_OBJECT_REST_LAYOUT,status=" << (g_restBindLayout > 0 ? "proven" : "signature_mismatch")
                      << ",engine_writes=0\n";
        }
        if (g_restBindLayout < 0 || locals.size() != static_cast<std::size_t>(bones) * replay::objcheck::kRestFloatsPerBone)
            return replay::objcheck::RestState::Unknown;
        replay::skeleton::ResolvedPoseSkeleton skeleton{};
        if (!replay::skeleton::FastResolvePedPoseSkeleton(object, skeleton) || !skeleton.skeleton ||
            skeleton.boneCount != bones)
            return replay::objcheck::RestState::Unknown;
        static std::vector<replay::pose::BoneQts> bind;
        const char* error = replay::tpose::ReadBindLocals(
            [](std::uintptr_t address, void* out, std::size_t size) { return replay::mem::Copy(out, address, size); },
            reinterpret_cast<std::uintptr_t>(skeleton.skeleton), bind);
        if (error && *error)
            return replay::objcheck::RestState::Unknown;
        const std::uint32_t bindBones = static_cast<std::uint32_t>(bind.size());
        match = replay::objcheck::CompareWithRest(locals.data(), bones, reinterpret_cast<const float*>(bind.data()), bindBones);
        return replay::objcheck::RestOf(match, bones, bindBones);
    }

    // A late scenery entry (the last one added) made by the spawned object
    // watch or first seen out of the radius, one line each up to the cap, with
    // the owner read for the bench.
    void LogPoseV2LateScenery(const char* why, Object object, std::uint32_t owner, bool mission, std::uint32_t nowMs)
    {
        if (Ctx().clip.scenery.empty() || g_lateSceneryLogs >= kLateSceneryLogCap)
            return;
        ++g_lateSceneryLogs;
        const replay::SceneryObject& entry = Ctx().clip.scenery.back();
        char line[288];
        std::snprintf(line, sizeof(line),
            "POSE_V2_SCENERY_LATE,why=%s,handle=%d,model=0x%x,pos=%.2f;%.2f;%.2f,appear_ms=%u,owned_by=%u,mission=%d,"
            "visible=%u,index=%zu,time_ms=%u\n",
            why, object, entry.model, entry.position[0], entry.position[1], entry.position[2], entry.appearMs, owner,
            mission ? 1 : 0, entry.visible, Ctx().clip.scenery.size() - 1, nowMs);
        Ctx().log << line;
    }

    // object_check.h: the scenery over the recording, four
    // times a second: an entry whose object left gets its leaving time, one
    // some other path recorded hands over to that prop, and an object of
    // the scene that is not of the map and appears within the radius (a
    // camp's prop set up during the recording) becomes an entry once it
    // stood still for a second (appearing at its first sight).
    // The nearest recorded ped or horse to a point (-1 none), and the
    // player's distance (-1 unread), positions from memory.
    float PoseV2NearestActorM(const float at[3])
    {
        float best = -1.0f;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            float p[3]{};
            if (entity.vehicle || !replay::entities::Position(entity.handle, p))
                continue;
            const float d = (p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) + (p[2] - at[2]) * (p[2] - at[2]);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best < 0.0f ? -1.0f : std::sqrt(best);
    }

    float PoseV2PlayerM(const float at[3])
    {
        float p[3]{};
        if (!replay::entities::Position(PLAYER::PLAYER_PED_ID(), p))
            return -1.0f;
        return std::sqrt((p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) + (p[2] - at[2]) * (p[2] - at[2]));
    }


    // A weapon object belongs to the first attach point of a recorded ped
    // that reports it; objects no longer reported leave the recording.
    // Every object attached to a recorded ped or horse that is not
    // at one of its weapon points (food and bottles in hand, the weapons in a
    // saddle holster, a lantern on the saddle) is recorded the way a weapon
    // is: an entity with its parent, the engine attach frame (bone and
    // offset) and a parent-relative track, so playback rebuilds it on the
    // parent puppet with the existing weapon path; when it comes off it goes
    // on as a loose prop (ContinuePoseV2WeaponAsProp). Discovery by natives:
    // the object pool, GET_ENTITY_ATTACHED_TO against the recorded handles.
    std::uint32_t g_poseV2AttachedScans = 0;
    // The recorded vehicles and train cars as parents of attached objects. A
    // train car is recorded by replay_trains (Ctx().recorder.trainCars), not in
    // the recorder's entity list: it gets an entity view here (its id and
    // handle) so the weapon registry's paths take it as any parent.
    std::vector<replay::recorder::RecordedEntity> g_vehicleParents; // sorted by handle
    std::unordered_set<Object> g_vehicleChildRefused; // looked at once (far or full), looked at again every few seconds
    std::uint32_t g_vehicleChildRefusedResetMs = 0;

    const replay::recorder::RecordedEntity* PoseV2VehicleParent(Entity handle)
    {
        const auto it = std::lower_bound(g_vehicleParents.begin(), g_vehicleParents.end(), handle,
            [](const replay::recorder::RecordedEntity& entity, Entity value) { return entity.handle < value; });
        return it != g_vehicleParents.end() && it->handle == handle ? &*it : nullptr;
    }

    void CollectPoseV2VehicleParents()
    {
        g_vehicleParents.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            if (entity.vehicle && replay::entities::Exists(entity.handle))
                g_vehicleParents.push_back(entity);
        for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
        {
            if (!car.handle || !replay::entities::Exists(car.handle))
                continue;
            replay::recorder::RecordedEntity view;
            view.handle = car.handle;
            view.id = car.id;
            view.vehicle = true;
            g_vehicleParents.push_back(view);
        }
        std::sort(g_vehicleParents.begin(), g_vehicleParents.end(),
            [](const replay::recorder::RecordedEntity& a, const replay::recorder::RecordedEntity& b) { return a.handle < b.handle; });
    }

    // Whether a ped's weapon points must be asked again (see
    // PoseV2PointScan); otherwise its point weapons still attached to it are
    // seen where they were.
    bool PoseV2PointsNeeded(const replay::recorder::RecordedEntity& owner, Hash held, std::uint32_t nowMs,
        const std::vector<std::pair<Entity, Object>>& attachedByParent)
    {
        const auto state = g_poseV2PointScans.find(owner.handle);
        if (state == g_poseV2PointScans.end() || !state->second.scanned || state->second.held != held ||
            static_cast<std::int32_t>(nowMs - state->second.nextFullMs) >= 0)
            return true;
        const auto first = std::lower_bound(attachedByParent.begin(), attachedByParent.end(),
            std::pair<Entity, Object>{owner.handle, (std::numeric_limits<Object>::min)()});
        auto last = first;
        while (last != attachedByParent.end() && last->first == owner.handle)
            ++last;
        for (auto it = first; it != last; ++it)
        {
            const auto known = std::find_if(Ctx().recorder.weapons.begin(), Ctx().recorder.weapons.end(),
                [&](const replay::recorder::RecordedWeapon& weapon) { return weapon.object == it->second; });
            if (known == Ctx().recorder.weapons.end() ||
                (known->attachPoint != replay::recorder::kAttachedObjectPoint && known->parent != owner.handle))
                return true;
        }
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (weapon.parent != owner.handle || weapon.attachPoint < 0 ||
                weapon.attachPoint >= replay::recorder::kWeaponAttachPoints)
                continue;
            bool stillAttached = false;
            for (auto it = first; it != last && !stillAttached; ++it)
                stillAttached = it->second == weapon.object;
            if (!stillAttached)
                return true;
        }
        return false;
    }
}
