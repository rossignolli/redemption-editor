// replay_playback_world.cpp: the world around the replay: the world pool cleared, hidden and restored, the leftovers, the ropes, the landing.
// Split from replay_playback.cpp (moved code); shared internals in replay_playback_internal.h.

#include "replay_playback_internal.h"

namespace replay::playback
{
    namespace
    {

    // The world clear's two membership tests per listed entity (a puppet's
    // clone? already hidden? ~750 entities against ~100 puppets and ~120 hidden
    // in town, every 250 ms) use sorted handles, built at the start of each
    // pass (HidePoseV2World). The puppets do not change during a pass; every
    // entity hidden goes into the hidden list and its set together.
    // tests/playback_perf_tests.cpp keeps the linear search as the reference.
    replay::playback::HandleSet g_poseV2ClearClones;
    replay::playback::HandleSet g_poseV2ClearHidden;

    // Entities that are never deleted, only hidden: the player's horses and its
    // vehicle. The player itself is left alone (visible, not frozen): the world
    // is not yet prepared for the clip as a whole, and the player keeps control
    // of the character while the editor plays.
    struct PoseV2PlayerOwned
    {
        Entity entities[5] = {};

        bool IsPlayer(Entity entity) const { return entity && entity == entities[0]; }

        void Refresh()
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            entities[0] = player;
            entities[1] = PED::IS_PED_ON_MOUNT(player) ? PED::GET_MOUNT(player) : 0;
            entities[2] = PED::_GET_LAST_MOUNT(player);
            entities[3] = PLAYER::_GET_ACTIVE_HORSE_FOR_PLAYER(PLAYER::PLAYER_ID());
            entities[4] = PED::GET_VEHICLE_PED_IS_IN(player, TRUE);
        }

        bool Contains(Entity entity) const
        {
            return entity &&
                std::find(std::begin(entities), std::end(entities), entity) != std::end(entities);
        }
    };
    std::uint32_t g_poseV2LandingImmediate = 0; // restored at once (frozen before the replay)
    Hash g_poseV2LandingTimeoutModel = 0;        // the first that timed out (log)
    Vector3 g_poseV2LandingTimeoutAt{};
    bool g_poseV2LandingPlayerHorse = false;     // the player's horse was in the queue
    bool g_poseV2LandingPlayerHorseTimedOut = false;
    unsigned long long g_poseV2LandingPlayerHorseMs = 0;
    Entity g_poseV2LandingPlayerHorseHandle = 0;
    replay::log::LogLimit g_poseV2WorldRopeLogs;
    // What the scan leaves around the camera (live peds that are not puppets,
    // visible, within 60 m), logged once every 3 s, at most 30 lines per
    // playback.
    ULONGLONG g_poseV2WorldCensusAt = 0;
    std::uint32_t g_poseV2WorldCensusLogs = 0;
    // Per scan, for the log: what keeps coming back.
    std::uint32_t g_poseV2ClearPeds = 0;
    std::uint32_t g_poseV2ClearVehicles = 0;
    Hash g_poseV2ClearSampleModel = 0;

    void ClearPoseV2WorldPool(
        int* handles,
        int count,
        bool vehicles,
        const PoseV2PlayerOwned& owned,
        std::size_t& hidden)
    {
        for (int index = 0; index < count; ++index)
        {
            const Entity entity = handles[index];
            if (!entity || g_poseV2ClearClones.Contains(entity) || owned.IsPlayer(entity))
                continue;
            // A train car is never deleted one by one (trains go as a whole):
            // hidden, its ambient train halted, and given back at the end
            // (replay_trains).
            if (vehicles && replay::trains::ClearWorldCar(entity))
                continue;
            const bool known = g_poseV2ClearHidden.Contains(entity);
            if (known)
            {
                // A game script may show its ped again (town scripts, the
                // camp): hidden again at once, and frozen and without collision
                // again, so it cannot fall through the ground unseen while the
                // replay plays.
                const std::uint32_t fixed = replay::pose::playback::RehideWorldPed(entity);
                if (fixed)
                {
                    // The owner's change is what goes back at the end
                    // (NoteOwnerChange).
                    for (replay::pose::playback::HiddenPedState& state : g_poseV2HiddenWorld)
                    {
                        if (state.ped != entity)
                            continue;
                        if (replay::pose::playback::NoteOwnerChange(state, fixed))
                            ++g_poseV2WorldOwnerChanges;
                        break;
                    }
                }
                if (fixed & replay::pose::playback::kRehideVisible)
                    ++g_poseV2WorldRehidden;
                if (fixed & replay::pose::playback::kRehideFrozen)
                    ++g_poseV2WorldRefrozen;
                if (fixed & replay::pose::playback::kRehideCollision)
                    ++g_poseV2WorldRecollided;
                continue;
            }
            if (!owned.Contains(entity))
            {
                const Hash model = ENTITY::GET_ENTITY_MODEL(entity);
                const bool deleted = vehicles
                    ? replay::pose::playback::DeleteAmbientVehicle(entity)
                    : replay::pose::playback::DeleteAmbientPed(entity);
                if (deleted)
                {
                    ++g_poseV2WorldDeleted;
                    ++(vehicles ? g_poseV2ClearVehicles : g_poseV2ClearPeds);
                    g_poseV2ClearSampleModel = model;
                    continue;
                }
            }
            replay::pose::playback::HiddenPedState state;
            if (replay::pose::playback::HideWorldPed(entity, state))
            {
                g_poseV2HiddenWorld.push_back(state);
                g_poseV2ClearHidden.Add(state.ped);
                ++hidden;
            }
        }
    }

    // The objects listed at the playback's first pass: a script object that
    // is not among them was made while the game's scripts were paused, by
    // another ASI (objcheck::IsOtherModObject).
    std::unordered_set<Object> g_poseV2LiveObjectsAtStart;
    bool g_poseV2LiveObjectsBaseline = false;

    // Another mod's object (objcheck::IsOtherModObject): a script object made
    // by an ASI's thread (ScriptHookRDR2 runs every ASI in one with the script
    // hash joaat("audiotest")), or one born after the playback's first pass.
    // The replay's own objects are told apart before (ReplayOwnObjects, the
    // puppets).
    // Only a script object (a mission entity, or the CREATE_OBJECT owner)
    // asks the engine for its script.
    void PoseV2ScriptFacts(Object object, bool missionEntity, std::uint32_t owner, bool& otherScript, bool& born)
    {
        otherScript = false;
        born = false;
        if (!missionEntity && owner != replay::objcheck::kOwnerMission)
            return;
        born = g_poseV2LiveObjectsBaseline && g_poseV2LiveObjectsAtStart.count(object) == 0;
        Any scriptArgs[8]{};
        otherScript =
            static_cast<std::uint32_t>(ENTITY::_GET_ENTITY_SCRIPT(object, scriptArgs)) == replay::theatre::kScriptHookThreadHash;
    }

    bool PoseV2OtherModObject(Object object, bool missionEntity, std::uint32_t owner)
    {
        bool otherScript = false, born = false;
        PoseV2ScriptFacts(object, missionEntity, owner, otherScript, born);
        return replay::objcheck::IsOtherModObject(otherScript, born, missionEntity, owner);
    }

    // The live world's vehicle debris (wheels, doors, boards an explosion or a
    // fire knocked off: objects of a vehicle's model) is not the replay's: the
    // world's originals are hidden while it plays. Ambient debris is deleted
    // like the ambient vehicles, a mission one hidden and given back. The
    // replay's own pieces are puppets.
    std::uint32_t g_poseV2ClearDebris = 0;
    std::unordered_map<std::uint32_t, bool> g_poseV2VehicleModels;

    void ClearPoseV2VehicleDebris(std::size_t& hidden)
    {
        static int handles[replay::recorder::kPoolCapacity]{};
        const int count = (std::min)(
            (std::max)(0, replay::objects::ListObjects(handles, replay::recorder::kPoolCapacity)),
            replay::recorder::kPoolCapacity);
        for (int index = 0; index < count; ++index)
        {
            Object object = handles[index];
            if (!object || !replay::entities::Exists(object) || g_poseV2ClearClones.Contains(object))
                continue;
            // The model (cached per model) before the attach test, both pure
            // reads: the ~675 objects of a town (a few of a vehicle model) do
            // not each ask IS_ENTITY_ATTACHED.
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            auto known = g_poseV2VehicleModels.find(model);
            if (known == g_poseV2VehicleModels.end())
                known = g_poseV2VehicleModels.emplace(model, STREAMING::IS_MODEL_A_VEHICLE(model) != FALSE).first;
            if (!known->second || ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            if (g_poseV2ClearHidden.Contains(object))
                continue;
            const bool mission = replay::pose::playback::IsMissionEntity(object);
            if (PoseV2OtherModObject(object, mission, replay::objects::EntityOwnerOf(object)))
                continue; // another mod's (objcheck::IsOtherModObject)
            if (!mission)
            {
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                Object handle = object;
                OBJECT::DELETE_OBJECT(&handle);
                if (!replay::entities::Exists(object))
                {
                    ++g_poseV2WorldDeleted;
                    ++g_poseV2ClearDebris;
                    continue;
                }
            }
            replay::pose::playback::HiddenPedState state;
            if (replay::pose::playback::HideWorldPed(object, state))
            {
                g_poseV2HiddenWorld.push_back(state);
                g_poseV2ClearHidden.Add(state.ped);
                ++hidden;
            }
        }
    }

    // The live objects that are not the map's (a script's, a temporary one, a
    // prop set's, a pickup; object_check.h HideLiveObject) in the clip area are
    // hidden while the replay plays, like the mission peds, and given back with
    // them (RestorePoseV2World): otherwise a mission's prop is drawn next to
    // the replay's copy. An object decided not to hide is looked at again after
    // kPoseV2LiveObjectRecheckMs (its owner or attachment may change).
    struct PoseV2LiveObjectStats
    {
        std::uint32_t passes = 0;
        std::uint32_t hidden = 0;
        std::uint32_t reshown = 0; // a script drew a hidden one again: hidden again
        std::uint32_t verdicts[replay::objcheck::kLiveHideCount] = {};
        std::uint32_t logs = 0;
        std::uint32_t otherModLogs = 0;
        std::uint32_t baselineObjects = 0; // listed at the playback's first pass
    } g_poseV2LiveObjects;
    std::unordered_map<Object, ULONGLONG> g_poseV2LiveObjectKept;
    constexpr ULONGLONG kPoseV2LiveObjectRecheckMs = 2000;
    }

    void ResetPoseV2LiveObjects()
    {
        g_poseV2LiveObjects = {};
        g_poseV2LiveObjectKept.clear();
        g_poseV2LiveObjectsAtStart.clear();
        g_poseV2LiveObjectsBaseline = false;
    }
    namespace
    {

    void ClearPoseV2LiveObjects(const PoseV2PlayerOwned& owned, std::size_t& hidden)
    {
        REPLAY_PROFILE_ZONE("play.hide_world.objects");
        static int handles[replay::recorder::kPoolCapacity]{};
        const int count = (std::min)(
            (std::max)(0, replay::objects::ListObjects(handles, replay::recorder::kPoolCapacity)),
            replay::recorder::kPoolCapacity);
        static std::vector<Object> own;
        replay::objects::ReplayOwnObjects(own);
        const ULONGLONG now = GetTickCount64();
        ++g_poseV2LiveObjects.passes;
        if (!g_poseV2LiveObjectsBaseline && count > 0)
        {
            // The first pass with a listing: what the world had when the
            // playback began (the game's scripts are paused from here on).
            g_poseV2LiveObjectsAtStart.insert(handles, handles + count);
            g_poseV2LiveObjectsBaseline = true;
            g_poseV2LiveObjects.baselineObjects = static_cast<std::uint32_t>(count);
        }
        if (g_poseV2LiveObjectKept.size() > 16384)
            g_poseV2LiveObjectKept.clear();
        const auto ownObject = [&](Entity entity) { return entity && std::binary_search(own.begin(), own.end(), entity); };
        for (int index = 0; index < count; ++index)
        {
            const Object object = handles[index];
            if (!object || !replay::entities::Exists(object) || g_poseV2ClearClones.Contains(object))
                continue;
            if (g_poseV2ClearHidden.Contains(object))
            {
                // A script drew it again (or gave its collision back): hidden again.
                const std::uint32_t fixed = replay::pose::playback::RehideWorldPed(object);
                if (fixed & replay::pose::playback::kRehideVisible)
                    ++g_poseV2LiveObjects.reshown;
                continue;
            }
            const auto kept = g_poseV2LiveObjectKept.find(object);
            if (kept != g_poseV2LiveObjectKept.end() && now < kept->second)
                continue;
            replay::objcheck::LiveObject live;
            live.replayOwn = ownObject(object);
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            auto vehicleModel = g_poseV2VehicleModels.find(model);
            if (vehicleModel == g_poseV2VehicleModels.end())
                vehicleModel = g_poseV2VehicleModels.emplace(model, STREAMING::IS_MODEL_A_VEHICLE(model) != FALSE).first;
            live.vehicleModel = vehicleModel->second;
            live.rope = replay::ropemodels::IsRopeModel(model);
            bool otherMod = false;
            if (!live.replayOwn)
            {
                // Another mod's object (a lighting mod's lantern) is never
                // hidden, wherever it stands and whatever holds it.
                live.owner = replay::objects::EntityOwnerOf(object);
                live.missionEntity = replay::pose::playback::IsMissionEntity(object);
                PoseV2ScriptFacts(object, live.missionEntity, live.owner, live.otherScript, live.bornInPlayback);
                otherMod = replay::objcheck::IsOtherModObject(
                    live.otherScript, live.bornInPlayback, live.missionEntity, live.owner);
                if (otherMod && g_poseV2LiveObjects.otherModLogs < 24)
                {
                    ++g_poseV2LiveObjects.otherModLogs;
                    float at[3]{};
                    replay::entities::Position(object, at);
                    char line[224];
                    std::snprintf(line, sizeof(line),
                        "POSE_V2_WORLD_LIVE_OBJECT,action=other_mod,handle=%d,model=0x%x,owner=%u,mission=%d,"
                        "asi_thread=%d,born_in_playback=%d,pos=%.2f;%.2f;%.2f,playhead_ms=%u\n",
                        object, model, live.owner, live.missionEntity ? 1 : 0, live.otherScript ? 1 : 0,
                        live.bornInPlayback ? 1 : 0, at[0], at[1], at[2],
                        static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()));
                    Ctx().log << line;
                }
            }
            if (!live.replayOwn && !otherMod && !live.vehicleModel && !live.rope)
            {
                // Held by the player, what he owns or a puppet (two levels:
                // the item on the holster on the saddle).
                const Entity parent = ENTITY::GET_ENTITY_ATTACHED_TO(object);
                if (parent)
                {
                    live.onKept = owned.Contains(parent) || g_poseV2ClearClones.Contains(parent) || ownObject(parent);
                    if (!live.onKept)
                    {
                        const Entity grand = ENTITY::GET_ENTITY_ATTACHED_TO(parent);
                        live.onKept = grand && (owned.Contains(grand) || g_poseV2ClearClones.Contains(grand) || ownObject(grand));
                    }
                }
                if (!live.onKept)
                {
                    float at[3]{};
                    live.inArea = !replay::entities::Position(object, at) ||
                        replay::objcheck::InClipArea(Ctx().clip.hasArea, at[0], at[1], Ctx().clip.areaMin[0],
                            Ctx().clip.areaMin[1], Ctx().clip.areaMax[0], Ctx().clip.areaMax[1]);
                    if (live.inArea)
                    {
                        live.pickup = OBJECT::DOES_PICKUP_OBJECT_EXIST(object) || OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object);
                    }
                }
            }
            const replay::objcheck::LiveHide verdict = replay::objcheck::HideLiveObject(live);
            ++g_poseV2LiveObjects.verdicts[static_cast<std::size_t>(verdict)];
            if (verdict != replay::objcheck::LiveHide::Hide)
            {
                g_poseV2LiveObjectKept[object] = now + kPoseV2LiveObjectRecheckMs;
                continue;
            }
            replay::pose::playback::HiddenPedState state;
            if (!replay::pose::playback::HideWorldPed(object, state))
                continue;
            g_poseV2HiddenWorld.push_back(state);
            g_poseV2ClearHidden.Add(state.ped);
            ++hidden;
            ++g_poseV2LiveObjects.hidden;
            if (g_poseV2LiveObjects.logs < 48)
            {
                ++g_poseV2LiveObjects.logs;
                float at[3]{};
                replay::entities::Position(object, at);
                char line[256];
                std::snprintf(line, sizeof(line),
                    "POSE_V2_WORLD_LIVE_OBJECT,action=hide,handle=%d,model=0x%x,owner=%u,mission=%d,pickup=%d,attached=%d,"
                    "was_visible=%d,pos=%.2f;%.2f;%.2f,playhead_ms=%u\n",
                    object, model, live.owner, live.missionEntity ? 1 : 0, live.pickup ? 1 : 0,
                    ENTITY::IS_ENTITY_ATTACHED(object) ? 1 : 0, state.visible ? 1 : 0, at[0], at[1], at[2],
                    static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()));
                Ctx().log << line;
            }
        }
    }

    // The ScriptHook pool listing can come back empty in town (worldGetAllPeds
    // and the object listing return 0 even into an 8192 entry array), which
    // would leave the town's NPCs walking through the replay. The engine's own
    // list of the peds and vehicles near a ped (GET_PED_NEARBY_PEDS and
    // _VEHICLES, the size in the first slot) fills the list then, around the
    // hidden player (he follows the replay's focus) and every ped puppet.
    std::uint32_t g_poseV2NearFallbacks = 0;
    int g_poseV2NearLastCount = -1;
    int ListPoseV2NearEntities(int* handles, int capacity, bool wantVehicles)
    {
        int count = 0;
        const auto add = [&](Ped around) {
            if (!around || !replay::entities::Exists(around) || !replay::entities::IsPed(around))
                return;
            constexpr int kSlots = 64;
            Any list[1 + kSlots]{};
            list[0] = kSlots;
            const int found = wantVehicles ? PED::GET_PED_NEARBY_VEHICLES(around, list)
                                           : PED::GET_PED_NEARBY_PEDS(around, list, -1, -1);
            for (int n = 0; n < found && n < kSlots && count < capacity; ++n)
            {
                const Entity entity = static_cast<Entity>(list[1 + n]);
                if (!entity || !replay::entities::Exists(entity))
                    continue;
                if (std::find(handles, handles + count, entity) == handles + count)
                    handles[count++] = entity;
            }
        };
        add(PLAYER::PLAYER_PED_ID());
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone && (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                                    puppet.packet.kind == replay::entity::EntityKind::Horse))
                add(puppet.clone);
        g_poseV2NearLastCount = count;
        return count;
    }

    // The world clear reads the engine's pools (entity_pools.h), as a replay
    // editor's world clear walks its pools: every live ped or vehicle, wherever
    // it is. A handle is made for those without one (at most
    // kPoseV2ClearHandleBudget a pass; the ambient ones are deleted with their
    // handle). -1 when the reader is unavailable (the listing and the nearby
    // lists take over, logged once per playback).
    constexpr int kPoseV2ClearHandleBudget = 256;
    std::uint32_t g_poseV2ClearPoolPasses = 0;
    std::uint32_t g_poseV2ClearPoolCreated = 0;
    std::uint32_t g_poseV2ClearPoolFallbacks = 0;
    replay::pools::ListStats g_poseV2ClearPoolPeds;
    int ListPoseV2WorldPool(replay::pools::Kind kind, int* handles, int capacity, replay::pools::ListStats& stats)
    {
        REPLAY_PROFILE_ZONE("play.hide_world.pools");
        const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const replay::pools::Position center{cam.x, cam.y, cam.z};
        constexpr float kEverywhere = 1.0e7f;
        const int count = replay::pools::ListHandles(
            kind, &center, 1, kEverywhere, kEverywhere, kPoseV2ClearHandleBudget, handles, capacity, &stats);
        if (count >= 0)
            g_poseV2ClearPoolCreated += stats.created;
        return count;
    }

    // The world's ropes are hidden while the replay plays (rope_world.h). Rung
    // 1 (the public _ROPE_CHANGE_VISIBILITY) on ropes listed by the rope
    // manager view (guarded reads, rope_observer.h); nothing is written.
    void HidePoseV2WorldRopes(const PoseV2PlayerOwned& owned)
    {
        REPLAY_PROFILE_ZONE("play.hide_world.ropes");
        static std::vector<replay::rope::RopeInfo> ropes;
        std::uint32_t managerCount = 0;
        if (!replay::rope::Snapshot(ropes, managerCount))
        {
            ++g_poseV2WorldRopeSnapshotFails;
            return;
        }
        ++g_poseV2WorldRopeScans;
        static std::vector<std::uint64_t> puppets;
        puppets.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone && replay::entities::Exists(puppet.clone))
                puppets.push_back(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)));
        std::sort(puppets.begin(), puppets.end());
        const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const auto distance = [&](const float at[3]) {
            const float dx = at[0] - cam.x, dy = at[1] - cam.y, dz = at[2] - cam.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const unsigned long long now = GetTickCount64();
        g_poseV2WorldRopes.BeginScan();
        for (const replay::rope::RopeInfo& rope : ropes)
        {
            replay::ropes::WorldRopeView view;
            view.id = rope.id;
            view.replay = Ctx().clip.ropes.Plays(rope.id);
            std::uint64_t worldEntity = 0;
            const std::uint32_t count = (std::min)(rope.attachmentCount, replay::rope::kMaxAttachments);
            for (std::uint32_t slot = 0; slot < count && !view.replay; ++slot)
            {
                const std::uint64_t entity = rope.attachments[slot].entity;
                if (!entity)
                    continue;
                if (std::binary_search(puppets.begin(), puppets.end(), entity))
                    view.replay = true;
                else if (!worldEntity)
                    worldEntity = entity;
            }
            view.onWorldEntity = worldEntity != 0;
            view.visible = (rope.flags & replay::ropes::kRopeVisibleFlag) != 0;
            view.cameraM = (std::min)(distance(rope.first), distance(rope.last));
            const replay::ropes::WorldRopeStep step = g_poseV2WorldRopes.Observe(view, now);
            if (step.action != replay::ropes::WorldRopeAction::Leave)
            {
                int handle = static_cast<int>(rope.id);
                const bool exists = PHYSICS::DOES_ROPE_EXIST(handle) != FALSE;
                const bool show = step.action == replay::ropes::WorldRopeAction::Show;
                if (exists)
                    PHYSICS::_ROPE_CHANGE_VISIBILITY(&handle, show ? TRUE : FALSE);
                else
                    ++g_poseV2WorldRopeNoHandle;
                if (replay::log::Verbose() && g_poseV2WorldRopeLogs.Take(40))
                    Ctx().log << "POSE_V2_WORLD_ROPE,action=" << (show ? "show" : "hide") << ",engine_id=" << rope.id
                              << ",type=" << rope.type << ",flags=0x" << std::hex << rope.flags << std::dec
                              << ",exists=" << (exists ? 1 : 0) << ",replay=" << (view.replay ? 1 : 0)
                              << ",camera_m=" << static_cast<int>(view.cameraM)
                              << ",visible_bit=" << replay::ropes::VisibleBitText(g_poseV2WorldRopes.Bit()) << '\n';
            }
            if (step.stray)
            {
                // rope_stray (qa_health.h): drawn near the camera after it
                // was hidden. Who holds it: the player, a hidden world
                // entity, or another entity of the world.
                const char* holder = "world";
                const std::uint64_t player = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(owned.entities[0]));
                for (std::uint32_t slot = 0; slot < count; ++slot)
                {
                    const std::uint64_t entity = rope.attachments[slot].entity;
                    if (!entity)
                        continue;
                    if (entity == player)
                    {
                        holder = "player";
                        break;
                    }
                    for (const Entity own : owned.entities)
                        if (own && entity == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(own)))
                            holder = "player_owned";
                    if (std::strcmp(holder, "world") == 0)
                        for (const replay::pose::playback::HiddenPedState& state : g_poseV2HiddenWorld)
                            if (entity == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(state.ped)))
                            {
                                holder = "hidden";
                                break;
                            }
                }
                Ctx().log << "POSE_V2_ROPE_STRAY,engine_id=" << rope.id << ",type=" << rope.type
                          << ",camera_m=" << static_cast<int>(view.cameraM) << ",holder=" << holder
                          << ",entity=0x" << std::hex << worldEntity << std::dec
                          << ",playhead_ms=" << static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()) << '\n';
            }
        }
        g_poseV2WorldRopes.EndScan();
    }
    }

    // World clear: ambient population is deleted (it respawns after playback
    // and frees pool room for puppets); mission and player entities are hidden
    // and restored.
    void HidePoseV2World()
    {
        REPLAY_PROFILE_ZONE("play.hide_world");
        // Room for a whole pool. A listing into a smaller array may come back
        // empty (the town pools; see ListPoseV2NearEntities).
        constexpr int kListCapacity = 8192;
        static int handles[kListCapacity]{};
        const std::uint64_t deletedBefore = g_poseV2WorldDeleted;
        std::size_t hidden = 0;
        PoseV2PlayerOwned owned;
        owned.Refresh();
        g_poseV2ClearPeds = 0;
        g_poseV2ClearVehicles = 0;
        g_poseV2ClearSampleModel = 0;
        // This pass's membership sets.
        g_poseV2ClearClones.Assign(Ctx().puppets.list, [](const replay::playback::Puppet& puppet) { return puppet.clone; });
        g_poseV2ClearHidden.Assign(g_poseV2HiddenWorld,
            [](const replay::pose::playback::HiddenPedState& state) { return state.ped; });
        // The pools first (ListPoseV2WorldPool).
        replay::pools::ListStats poolStats;
        int count = ListPoseV2WorldPool(replay::pools::Kind::Vehicle, handles, kListCapacity, poolStats);
        const bool fromPool = count >= 0;
        if (fromPool)
        {
            ++g_poseV2ClearPoolPasses;
        }
        else
        {
            ++g_poseV2ClearPoolFallbacks;
            count = (std::min)((std::max)(0, worldGetAllVehicles(handles, kListCapacity)), kListCapacity);
            if (count == 0)
                count = ListPoseV2NearEntities(handles, kListCapacity, true);
        }
        ClearPoseV2WorldPool(handles, count, true, owned, hidden);
        count = fromPool ? ListPoseV2WorldPool(replay::pools::Kind::Ped, handles, kListCapacity, g_poseV2ClearPoolPeds) : -1;
        if (count < 0)
            count = (std::min)((std::max)(0, worldGetAllPeds(handles, kListCapacity)), kListCapacity);
        const int poolPeds = count;
        if (count == 0 && !fromPool)
        {
            // The pool listing failed (see ListPoseV2NearEntities).
            count = ListPoseV2NearEntities(handles, kListCapacity, false);
            ++g_poseV2NearFallbacks;
        }
        ClearPoseV2WorldPool(handles, count, false, owned, hidden);
        g_poseV2ClearDebris = 0;
        ClearPoseV2VehicleDebris(hidden);
        ClearPoseV2LiveObjects(owned, hidden);
        HidePoseV2WorldRopes(owned);
        // The census after the clear (see g_poseV2WorldCensusAt).
        const ULONGLONG censusNow = GetTickCount64();
        // The census reads the world only for its line: Debug=1 only.
        if (replay::log::Verbose() && censusNow >= g_poseV2WorldCensusAt && g_poseV2WorldCensusLogs < 30)
        {
            g_poseV2WorldCensusAt = censusNow + 3000;
            ++g_poseV2WorldCensusLogs;
            const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
            int nearCount = 0, nearVisible = 0, puppets = 0;
            Hash sample = 0;
            int samplePopulation = -1;
            bool sampleMission = false;
            for (int index = 0; index < count; ++index)
            {
                const Entity entity = handles[index];
                if (!entity || !replay::entities::Exists(entity))
                    continue;
                if (g_poseV2ClearClones.Contains(entity))
                {
                    ++puppets;
                    continue;
                }
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
                const float dx = at.x - cam.x, dy = at.y - cam.y, dz = at.z - cam.z;
                if (dx * dx + dy * dy + dz * dz > 60.0f * 60.0f)
                    continue;
                ++nearCount;
                if (ENTITY::IS_ENTITY_VISIBLE(entity))
                {
                    ++nearVisible;
                    sample = ENTITY::GET_ENTITY_MODEL(entity);
                    samplePopulation = ENTITY::GET_ENTITY_POPULATION_TYPE(entity);
                    sampleMission = replay::pose::playback::IsMissionEntity(entity);
                }
            }
            Ctx().log << "POSE_V2_WORLD_CENSUS,source=" << (fromPool ? "pool" : "listing") << ",pool_peds=" << poolPeds
                      << ",pool_peds_live=" << (fromPool ? static_cast<int>(g_poseV2ClearPoolPeds.live) : -1)
                      << ",pool_passes=" << g_poseV2ClearPoolPasses << ",pool_fallbacks=" << g_poseV2ClearPoolFallbacks
                      << ",pool_handles_created=" << g_poseV2ClearPoolCreated
                      << ",guid_free=" << (fromPool ? static_cast<int>(g_poseV2ClearPoolPeds.guidFree) : -1)
                      << ",pool_us=" << (fromPool ? static_cast<int>(g_poseV2ClearPoolPeds.us + poolStats.us) : -1)
                      << ",near_fallbacks=" << g_poseV2NearFallbacks << ",near_count=" << g_poseV2NearLastCount
                      << ",peds_listed=" << count << ",puppets=" << puppets << ",near_60m=" << nearCount
                      << ",near_visible=" << nearVisible << ",sample_model=0x" << std::hex << sample << std::dec
                      << ",sample_population=" << samplePopulation << ",sample_mission=" << (sampleMission ? 1 : 0)
                      << ",hidden_total=" << g_poseV2HiddenWorld.size() << ",rehidden_total=" << g_poseV2WorldRehidden << '\n';
        }
        const std::uint64_t deleted = g_poseV2WorldDeleted - deletedBefore;
        if ((hidden > 0 || deleted > 0) && replay::log::Verbose()) // every clearing tick, Debug=1 only
        {
            std::ostream& log = Ctx().log;
            log << "POSE_V2_WORLD_CLEAR"
                << ",deleted=" << deleted
                << ",peds=" << g_poseV2ClearPeds
                << ",vehicles=" << g_poseV2ClearVehicles
                << ",vehicle_debris=" << g_poseV2ClearDebris
                << ",sample_model=0x" << std::hex << g_poseV2ClearSampleModel << std::dec
                << ",hidden=" << hidden
                << ",hidden_total=" << g_poseV2HiddenWorld.size()
                << ",deleted_total=" << g_poseV2WorldDeleted
                << '\n';
            log.flush();
        }
    }

    // Leftovers of the recording, deleted before any puppet exists (nothing
    // comes back): every pickup that appeared during the recording (a dead
    // NPC's weapon), and every loose object that appeared during it with the
    // model of a weapon or prop the recording registered (knocked hats, dropped
    // weapons). Map objects that streamed in meanwhile are left alone: deleting
    // every new object takes hundreds of map props with it.
    void DeletePoseV2RecordingLeftovers()
    {
        // A clip from a file has no objects-before list of this session:
        // everything would look like a leftover of it.
        if (Ctx().clip.fromDisk)
        {
            Ctx().log << "POSE_V2_WORLD_LEFTOVERS,skipped=from_disk\n";
            return;
        }
        static int handles[replay::recorder::kPoolCapacity]{};
        const std::vector<int>& objectsBefore = Ctx().clip.objectsBefore;
        const std::vector<int>& pickupsBefore = Ctx().clip.pickupsBefore;
        std::vector<std::uint32_t> models;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (packet.kind != replay::entity::EntityKind::Ped && packet.kind != replay::entity::EntityKind::Horse &&
                packet.kind != replay::entity::EntityKind::Vehicle && packet.kind != replay::entity::EntityKind::TrainCar)
                models.push_back(packet.modelHash);
        std::sort(models.begin(), models.end());
        std::size_t pickupsRemoved = 0;
        const int pickups = (std::min)((std::max)(0, worldGetAllPickups(handles, replay::recorder::kPoolCapacity)),
            replay::recorder::kPoolCapacity);
        std::size_t pickupsKeptBefore = 0;
        std::size_t pickupsNotExisting = 0;
        std::size_t pickupsDeletedAsObject = 0;
        for (int n = 0; n < pickups; ++n)
        {
            if (std::binary_search(pickupsBefore.begin(), pickupsBefore.end(), handles[n]))
            {
                ++pickupsKeptBefore;
                continue;
            }
            if (!OBJECT::DOES_PICKUP_EXIST(handles[n]))
            {
                // The ScriptHook pickup listing hands out entity handles of
                // pickup objects (DOES_PICKUP_EXIST is false for them): delete
                // the object when it was born during the recording, inside its
                // area.
                Object object = handles[n];
                bool inArea = !Ctx().clip.hasArea;
                if (replay::entities::Exists(object) && !ENTITY::IS_ENTITY_ATTACHED(object) && !inArea)
                {
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    inArea = p.x >= Ctx().clip.areaMin[0] - 30.0f && p.x <= Ctx().clip.areaMax[0] + 30.0f &&
                        p.y >= Ctx().clip.areaMin[1] - 30.0f && p.y <= Ctx().clip.areaMax[1] + 30.0f;
                }
                if (replay::entities::Exists(object) && !ENTITY::IS_ENTITY_ATTACHED(object) && inArea &&
                    !replay::pose::playback::IsMissionEntity(object))
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                    OBJECT::DELETE_OBJECT(&object);
                    ++pickupsDeletedAsObject;
                }
                else
                    ++pickupsNotExisting;
                continue;
            }
            OBJECT::REMOVE_PICKUP(handles[n]);
            ++pickupsRemoved;
        }
        std::size_t objectsDeleted = 0;
        std::size_t pickupObjectsDeleted = 0;
        std::size_t adoptedDeleted = 0;
        std::size_t scriptsKept = 0; // a script's (a mod's) object: objcheck::LeftoverDeletable
        for (int handle : Ctx().clip.adoptedObjects)
        {
            Object object = handle;
            if (!object || !replay::entities::Exists(object) || ENTITY::IS_ENTITY_ATTACHED(object) ||
                std::binary_search(objectsBefore.begin(), objectsBefore.end(), object))
                continue;
            if (!replay::objcheck::LeftoverDeletable(replay::pose::playback::IsMissionEntity(object)))
            {
                ++scriptsKept;
                continue;
            }
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            OBJECT::DELETE_OBJECT(&object);
            ++adoptedDeleted;
        }
        Ctx().clip.adoptedObjects.clear();
        // Guarded like the recording's objects-before list (with the pool
        // short, only objects that already have a handle are looked at).
        const char* objectsMode = "listing";
        const int objects = (std::min)((std::max)(0, replay::objects::ListAllObjectsGuarded(handles,
            replay::recorder::kPoolCapacity, objectsMode)), replay::recorder::kPoolCapacity);
        const int listedObjects = objects;
        for (int n = 0; n < objects; ++n)
        {
            Object object = handles[n];
            if (!object || !replay::entities::Exists(object) || std::binary_search(objectsBefore.begin(), objectsBefore.end(), object))
                continue;
            if (ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            // RDR2 pickups (a dead NPC's dropped weapon, loot) are objects with
            // a pickup record; worldGetAllPickups never lists them. Pickup
            // objects born during the recording near its area go.
            const bool pickup = OBJECT::DOES_PICKUP_OBJECT_EXIST(object) || OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object);
            if (pickup && !replay::objcheck::LeftoverDeletable(replay::pose::playback::IsMissionEntity(object)))
            {
                ++scriptsKept;
                continue;
            }
            if (pickup)
            {
                bool inArea = !Ctx().clip.hasArea;
                if (!inArea)
                {
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    inArea = p.x >= Ctx().clip.areaMin[0] - 30.0f && p.x <= Ctx().clip.areaMax[0] + 30.0f &&
                        p.y >= Ctx().clip.areaMin[1] - 30.0f && p.y <= Ctx().clip.areaMax[1] + 30.0f;
                }
                if (inArea)
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                    OBJECT::DELETE_OBJECT(&object);
                    ++pickupObjectsDeleted;
                }
                continue;
            }
            if (replay::pose::playback::IsMissionEntity(object))
                continue;
            if (!std::binary_search(models.begin(), models.end(), static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object))))
                continue;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            OBJECT::DELETE_OBJECT(&object);
            ++objectsDeleted;
        }
        Ctx().log << "POSE_V2_WORLD_LEFTOVERS,objects_mode=" << objectsMode << ",pickups_before=" << pickupsBefore.size() << ",pickups_removed=" << pickupsRemoved
            << ",pickups_kept_before=" << pickupsKeptBefore << ",pickups_not_existing=" << pickupsNotExisting
            << ",pickups_deleted_as_object=" << pickupsDeletedAsObject
            << ",pickup_objects_deleted=" << pickupObjectsDeleted << ",leftover_models=" << models.size()
            << ",objects_deleted=" << objectsDeleted << ",adopted_deleted=" << adoptedDeleted
            << ",scripts_kept=" << scriptsKept
            << ",listed_objects=" << listedObjects << ",listed_pickups=" << pickups << '\n';
    }

    // Every hidden entity is shown at once (world_landing.h); one that was not
    // frozen before the replay stays frozen without collision until the map
    // collision around it is in (UpdateWorldLanding), the player's horse
    // included. One frozen before the replay has nothing to fall from: restored
    // at once.
    void RestorePoseV2World()
    {
        REPLAY_PROFILE_ZONE("play.restore_world");
        const unsigned long long now = GetTickCount64();
        if (g_poseV2Landing.Empty())
        {
            g_poseV2Landing.ResetStats();
            g_poseV2LandingImmediate = 0;
            g_poseV2LandingTimeoutModel = 0;
            g_poseV2LandingPlayerHorse = false;
            g_poseV2LandingPlayerHorseTimedOut = false;
            g_poseV2LandingPlayerHorseMs = 0;
        }
        PoseV2PlayerOwned owned;
        owned.Refresh();
        const Entity playerHorse = owned.entities[1] ? owned.entities[1] : owned.entities[3];
        std::size_t immediate = 0, queued = 0, gone = 0, invisible = 0;
        std::uint32_t invisibleLogs = 0;
        for (const replay::pose::playback::HiddenPedState& state : g_poseV2HiddenWorld)
        {
            // Gone, or its handle now another entity's (its owner deleted it).
            if (!replay::pose::playback::StillHiddenEntity(state))
            {
                ++gone;
                continue;
            }
            // The detector: an entity given back invisible was invisible when
            // the replay first hid it and nobody showed it meanwhile; a mission
            // ped among them would be an invisible enemy after the replay. One
            // line each (16).
            if (!state.visible)
            {
                ++invisible;
                if (invisibleLogs < 16)
                {
                    ++invisibleLogs;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(state.ped, TRUE, FALSE);
                    Ctx().log << "POSE_V2_WORLD_RESTORE_INVISIBLE,handle=" << state.ped << ",model=0x" << std::hex
                           << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(state.ped)) << std::dec
                           << ",ped=" << (ENTITY::IS_ENTITY_A_PED(state.ped) ? 1 : 0)
                           << ",mission=" << (replay::pose::playback::IsMissionEntity(state.ped) ? 1 : 0)
                           << ",dead=" << (ENTITY::IS_ENTITY_DEAD(state.ped) ? 1 : 0)
                           << ",alpha=" << ENTITY::GET_ENTITY_ALPHA(state.ped) << ",pos=" << at.x << ";" << at.y << ";"
                           << at.z << '\n';
                }
            }
            if (state.frozen)
            {
                replay::pose::playback::RestoreWorldPed(state);
                ++immediate;
                continue;
            }
            replay::pose::playback::BeginWorldPedLanding(state);
            g_poseV2Landing.Add(state.ped, replay::pose::playback::MountOfWorldPed(state.ped), now, state);
            if (state.ped == playerHorse || state.ped == owned.entities[2])
            {
                g_poseV2LandingPlayerHorse = true;
                g_poseV2LandingPlayerHorseHandle = state.ped;
            }
            ++queued;
        }
        g_poseV2LandingImmediate += static_cast<std::uint32_t>(immediate);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_WORLD_RESTORE_SUMMARY" // kept with Debug=0
            << ",restored=" << g_poseV2HiddenWorld.size()
            << ",immediate=" << immediate
            << ",landing=" << queued
            << ",gone=" << gone
            << ",player_horse=" << (g_poseV2LandingPlayerHorse ? 1 : 0)
            << ",refrozen=" << g_poseV2WorldRefrozen
            << ",recollided=" << g_poseV2WorldRecollided
            << ",reshown=" << g_poseV2WorldRehidden
            << ",owner_changes=" << g_poseV2WorldOwnerChanges
            << ",restored_invisible=" << invisible
            << '\n';
        // The live objects that are not the map's.
        {
            const std::uint32_t* v = g_poseV2LiveObjects.verdicts;
            using LH = replay::objcheck::LiveHide;
            log << "POSE_V2_WORLD_LIVE_OBJECTS,passes=" << g_poseV2LiveObjects.passes << ",hidden=" << g_poseV2LiveObjects.hidden
                << ",reshown=" << g_poseV2LiveObjects.reshown
                << ",looked_hide=" << v[static_cast<int>(LH::Hide)] << ",looked_replay=" << v[static_cast<int>(LH::Replay)]
                << ",looked_kept=" << v[static_cast<int>(LH::OnKept)] << ",looked_outside=" << v[static_cast<int>(LH::Outside)]
                << ",looked_map=" << v[static_cast<int>(LH::Map)] << ",looked_rope=" << v[static_cast<int>(LH::Rope)]
                << ",looked_debris=" << v[static_cast<int>(LH::Debris)]
                << ",looked_other_mod=" << v[static_cast<int>(LH::OtherMod)]
                << ",baseline_objects=" << g_poseV2LiveObjects.baselineObjects << '\n';
            ResetPoseV2LiveObjects();
        }
        log.flush();
        g_poseV2HiddenWorld.clear();
    }

    // The world's ropes shown again when the playback stops.
    void RestorePoseV2WorldRopes()
    {
        const replay::ropes::WorldRopeHider::Counters stats = g_poseV2WorldRopes.Stats();
        const replay::ropes::VisibleBit bit = g_poseV2WorldRopes.Bit();
        const std::size_t held = g_poseV2WorldRopes.Size();
        std::uint32_t shown = 0, missing = 0;
        for (const std::uint32_t id : g_poseV2WorldRopes.TakeRestore())
        {
            int handle = static_cast<int>(id);
            if (!PHYSICS::DOES_ROPE_EXIST(handle))
            {
                ++missing;
                continue;
            }
            PHYSICS::_ROPE_CHANGE_VISIBILITY(&handle, TRUE);
            ++shown;
        }
        if (!g_poseV2WorldRopeScans && !stats.hidden)
            return;
        Ctx().log << "POSE_V2_WORLD_ROPES_SUMMARY,scans=" << g_poseV2WorldRopeScans
                  << ",snapshot_fails=" << g_poseV2WorldRopeSnapshotFails << ",hidden=" << stats.hidden
                  << ",rehidden=" << stats.rehidden << ",released=" << stats.released << ",gone=" << stats.gone
                  << ",strays=" << stats.strays << ",held_at_stop=" << held << ",shown=" << shown
                  << ",missing=" << missing << ",no_handle=" << g_poseV2WorldRopeNoHandle
                  << ",hide_held=" << stats.hideHeld << ",hide_missed=" << stats.hideMissed
                  << ",visible_bit=" << replay::ropes::VisibleBitText(bit) << '\n';
    }

    // One tick of the landing queue (any time, also after the playback
    // stopped).
    void UpdatePoseV2WorldLanding()
    {
        if (g_poseV2Landing.Empty())
            return;
        REPLAY_PROFILE_ZONE("tick.world_landing");
        const unsigned long long now = GetTickCount64();
        static std::vector<std::int32_t> requested;
        requested.clear();
        g_poseV2Landing.ForRequests(replay::landing::kRequestsPerTick,
            [&](const auto& entry) { requested.push_back(entry.handle); });
        g_poseV2Landing.Tick(
            now, [](std::int32_t handle) { return replay::entities::Exists(handle); },
            [&](const auto& entry) {
                const bool request = std::find(requested.begin(), requested.end(), entry.handle) != requested.end();
                return replay::pose::playback::WorldPedCollisionLoaded(entry.handle, request);
            },
            [&](const auto& entry, replay::landing::Action action, unsigned long long waitedMs) {
                if (action == replay::landing::Action::CollisionOn)
                {
                    replay::pose::playback::LandWorldPedCollision(entry.payload);
                    if (entry.timedOut && !g_poseV2LandingTimeoutModel)
                    {
                        g_poseV2LandingTimeoutModel = ENTITY::GET_ENTITY_MODEL(entry.handle);
                        g_poseV2LandingTimeoutAt = ENTITY::GET_ENTITY_COORDS(entry.handle, TRUE, FALSE);
                    }
                    if (entry.handle == g_poseV2LandingPlayerHorseHandle)
                    {
                        g_poseV2LandingPlayerHorseMs = waitedMs;
                        g_poseV2LandingPlayerHorseTimedOut = entry.timedOut;
                    }
                    return;
                }
                replay::pose::playback::RestoreWorldPed(entry.payload);
            });
        if (!g_poseV2Landing.Empty())
            return;
        const replay::landing::Stats& stats = g_poseV2Landing.Totals();
        Ctx().log << "POSE_V2_WORLD_LANDING_SUMMARY,queued=" << stats.queued << ",immediate=" << g_poseV2LandingImmediate
                  << ",waited=" << stats.waited << ",landed=" << stats.landed << ",timed_out=" << stats.timedOut
                  << ",gone=" << stats.gone << ",max_wait_ms=" << stats.maxWaitMs
                  << ",avg_wait_ms=" << (stats.waited ? stats.totalWaitMs / stats.waited : 0)
                  << ",player_horse=" << (g_poseV2LandingPlayerHorse ? 1 : 0)
                  << ",player_horse_ms=" << g_poseV2LandingPlayerHorseMs
                  << ",player_horse_timed_out=" << (g_poseV2LandingPlayerHorseTimedOut ? 1 : 0)
                  << ",timeout_model=0x" << std::hex << g_poseV2LandingTimeoutModel << std::dec
                  << ",timeout_at=" << g_poseV2LandingTimeoutAt.x << ';' << g_poseV2LandingTimeoutAt.y << ';'
                  << g_poseV2LandingTimeoutAt.z << '\n';
        g_poseV2LandingPlayerHorseHandle = 0;
    }
}
