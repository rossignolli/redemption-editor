// replay_objects_recording_world.cpp: the world's recording steps: scenery and its children, the
// vehicle and wheel pieces, the world snapshot, map dressing, world objects, spawned objects, loot,
// dressing removals, the props update, the vehicles' children and the attached objects.
// Split from replay_objects_recording.cpp (moved code); shared internals in
// replay_objects_recording_internal.h.

#include "replay_objects_recording_internal.h"

namespace replay::objects
{
    WorldSkeletonStats g_worldSkeletonStats;
    SceneryStats g_sceneryStats;
    LootReinstanceStats g_lootReinstanceStats;

    void ResetPoseV2Loot()
    {
        g_lootWatch.clear();
        g_lootGonePending.clear();
        g_lootReinstanceStats = {};
        g_lootFirstSeenMs.clear();
        g_lootDecided.clear();
        g_lootLogs = 0;
    }
    MapDressingStats g_mapDressingStats;
    SceneryChildStats g_sceneryChildStats;

    namespace
    {

    bool PoseV2SceneryCandidate(Object object, std::uint32_t& owner, bool& mission, bool* pickupOut = nullptr)
    {
        mission = ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) != FALSE;
        owner = ReadEntityOwner(object);
        // objcheck::SceneryCandidate: a pickup or loot
        // object is scenery whoever owns it. It was refused here for the
        // leftovers rule (replay_playback.cpp), which deletes only the
        // pickups born during the recording; one already there was left to
        // the live world, collected by then.
        const bool pickup = OBJECT::DOES_PICKUP_OBJECT_EXIST(object) || OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object);
        if (pickupOut)
            *pickupOut = pickup;
        const bool rope = replay::ropemodels::IsRopeModel(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)));
        return replay::objcheck::SceneryCandidate(mission, owner, pickup, rope);
    }

    void AddPoseV2Scenery(Object object, const float position[3], const float rotation[3], std::uint32_t appearMs,
        std::uint32_t owner, bool mission, replay::objcheck::SceneryRule rule = replay::objcheck::SceneryRule::Legacy,
        std::uint32_t ipl = 0)
    {
        if (Ctx().clip.scenery.size() >= replay::objcheck::kMaxScenery)
        {
            ++g_sceneryStats.full;
            return;
        }
        replay::SceneryObject entry;
        entry.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
        for (int axis = 0; axis < 3; ++axis)
        {
            entry.position[axis] = position[axis];
            entry.rotation[axis] = rotation[axis];
        }
        entry.appearMs = appearMs;
        entry.owner = static_cast<std::uint8_t>(owner > 0xFF ? 0xFF : owner);
        entry.mission = mission ? 1 : 0;
        entry.rule = static_cast<std::uint8_t>(rule); // SCNY v3
        entry.ipl = ipl;
        entry.visible = ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0; // a mission's hidden objects stay hidden
        if (!entry.visible)
            ++g_sceneryStats.invisible;
        // hat_look.h: a hat that fell before the recording started
        // (a hat by its owner's head) keeps the layer it
        // draws.
        ReadPoseV2HatLook(object, entry.hatLook);
        if (!entry.hatLook.empty())
        {
            ++g_hatLookStats.scenery;
            LogPoseV2HatLook("scenery", "index=" + std::to_string(Ctx().clip.scenery.size()), entry.model, entry.hatLook,
                entry.position);
        }
        g_sceneryOf[object] = Ctx().clip.scenery.size();
        g_sceneryLive.push_back({object, Ctx().clip.scenery.size()});
        Ctx().clip.scenery.push_back(entry);
    }

    // A scenery object some path recorded (moved, broke, animated, picked
    // up): the prop takes over from its stand-in. False when not scenery.
    bool MarkPoseV2SceneryTaken(Object object, replay::entity::ReplayId prop)
    {
        const auto it = g_sceneryOf.find(object);
        if (it == g_sceneryOf.end() || it->second >= Ctx().clip.scenery.size())
            return false;
        replay::SceneryObject& entry = Ctx().clip.scenery[it->second];
        if (!entry.prop.Valid() && prop.Valid())
        {
            entry.prop = prop;
            ++g_sceneryStats.taken;
        }
        return true;
    }

    // object_check.h: an object attached to the live scenery entry
    // `parentIndex` (its object `parent`) becomes a child entry: the engine
    // attach frame when it reads plausible, its transform in the parent's
    // frame, where it stands, when it appeared, its visibility.
    void AddPoseV2SceneryChild(Object child, Object parent, std::size_t parentIndex, std::uint32_t appearMs,
        std::uint32_t nowMs, bool atStart)
    {
        if (Ctx().clip.sceneryChildren.size() >= replay::objcheck::kMaxSceneryChildren)
        {
            ++g_sceneryChildStats.full;
            return;
        }
        replay::SceneryChild entry;
        entry.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(child));
        entry.parent = static_cast<std::uint32_t>(parentIndex);
        replay::weapon::AttachFrame frame;
        if (replay::weapon::ReadAttachFrame(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(child)), frame) &&
            FoldPoseV2AttachChildBone(child, frame, "scenery_child") &&
            replay::objcheck::FramePlausible(frame.bone, frame.position, frame.rotation))
        {
            entry.hasFrame = 1;
            entry.frameBone = frame.bone;
            std::memcpy(entry.framePosition, frame.position, sizeof(entry.framePosition));
            std::memcpy(entry.frameRotation, frame.rotation, sizeof(entry.frameRotation));
            ++g_sceneryChildStats.frames;
        }
        const replay::pose::WorldTransform p = ReadEntityTransform(parent);
        const replay::pose::WorldTransform c = ReadEntityTransform(child);
        const float pp[3]{p.position.x, p.position.y, p.position.z};
        const float pq[4]{p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w};
        const float cp[3]{c.position.x, c.position.y, c.position.z};
        const float cq[4]{c.rotation.x, c.rotation.y, c.rotation.z, c.rotation.w};
        replay::objcheck::RelativeFrame(pp, pq, cp, cq, entry.relativePosition, entry.relativeRotation);
        const Vector3 rotation = ENTITY::GET_ENTITY_ROTATION(child, 2);
        for (int axis = 0; axis < 3; ++axis)
            entry.position[axis] = cp[axis];
        entry.rotation[0] = rotation.x;
        entry.rotation[1] = rotation.y;
        entry.rotation[2] = rotation.z;
        entry.appearMs = appearMs;
        entry.visible = ENTITY::IS_ENTITY_VISIBLE(child) ? 1 : 0;
        ++g_sceneryChildStats.added;
        g_sceneryChildStats.atStart += atStart ? 1u : 0u;
        if (g_sceneryChildStats.logs++ < 32)
        {
            const replay::logfmt::FormatScope logFormat(Ctx().log);
            Ctx().log << std::fixed << std::setprecision(3) << "POSE_V2_SCENERY_CHILD,status=attached,handle=" << child
                      << ",model=0x" << std::hex << entry.model << ",parent_model=0x"
                      << (parentIndex < Ctx().clip.scenery.size() ? Ctx().clip.scenery[parentIndex].model : 0u) << std::dec
                      << ",parent_entry=" << parentIndex << ",frame=" << static_cast<int>(entry.hasFrame)
                      << ",bone=" << entry.frameBone << ",offset=" << entry.relativePosition[0] << ';'
                      << entry.relativePosition[1] << ';' << entry.relativePosition[2]
                      << ",visible=" << static_cast<int>(entry.visible) << ",appear_ms=" << appearMs
                      << ",time_ms=" << nowMs << '\n';
            Ctx().log.unsetf(std::ios_base::floatfield);
        }
        g_sceneryChildLive.push_back({child, parent, Ctx().clip.sceneryChildren.size()});
        g_sceneryChildObjects.insert(child);
        Ctx().clip.sceneryChildren.push_back(std::move(entry));
    }
    }
    WheelPieceStats g_wheelPieceStats;

    namespace
    {

    void FlushPoseV2VehiclePieces()
    {
        FlushPoseV2WheelPieces();
        if (g_vehiclePiecesPending.empty())
            return;
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        const ULONGLONG now = GetTickCount64();
        for (std::size_t n = g_vehiclePiecesPending.size(); n-- > 0;)
        {
            PendingVehiclePiece& pending = g_vehiclePiecesPending[n];
            if (!pending.entity)
                pending.entity = replay::frag::EntityOfInst(pending.inst);
            Object found = 0;
            for (int slot = 0; pending.entity && slot < count && !found; ++slot)
                if (objects[slot] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == pending.entity)
                    found = objects[slot];
            if (found && replay::entities::Exists(found) && !IsPoseV2KnownObject(found) &&
                replay::entities::Exists(pending.vehicleHandle))
            {
                const std::size_t before = Ctx().recorder.props.size();
                RegisterPoseV2Prop(found, pending.vehicle, pending.vehicleHandle, replay::entity::PropOrigin::VehiclePiece, 0,
                    "vehicle_piece", {}, pending.group);
                if (Ctx().recorder.props.size() > before)
                    ++g_vehiclePiecesRecorded;
            }
            else if (!found && now < pending.deadline)
                continue;
            else if (!found)
                ++g_vehiclePiecesLost;
            g_vehiclePiecesPending.erase(g_vehiclePiecesPending.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    }

    // A wheel bit of a recorded vehicle came on in its part track (the
    // sampler, replay_capture.cpp): its wheel object is looked for now and
    // for a second (FlushPoseV2WheelPieces).
    void NotePoseV2WheelOff(replay::entity::ReplayId vehicle, Entity vehicleHandle, int wheel, int slot)
    {
        ++g_wheelPieceStats.off;
        if (wheel < 0 || wheel >= replay::vehicle_pieces::kWheels || slot < 0)
            return;
        PendingWheelPiece pending;
        pending.vehicle = vehicle;
        pending.vehicleHandle = vehicleHandle;
        pending.wheel = wheel;
        pending.slot = slot;
        pending.offMs = replay::pose::capture::NowMs();
        pending.deadline = GetTickCount64() + 1000;
        g_wheelPiecesPending.push_back(pending);
        FlushPoseV2WheelPieces();
    }

    void FlushPoseV2WheelPieces()
    {
        if (g_wheelPiecesPending.empty())
            return;
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        const ULONGLONG now = GetTickCount64();
        for (std::size_t n = g_wheelPiecesPending.size(); n-- > 0;)
        {
            const PendingWheelPiece pending = g_wheelPiecesPending[n];
            const char* status = nullptr;
            Object found = 0;
            replay::vehicle_pieces::WheelRead read;
            const char* error = replay::entities::Exists(pending.vehicleHandle)
                ? ReadPoseV2WheelObject(pending.vehicleHandle, pending.slot, read) : "vehicle_gone";
            if (error && *error)
                ++g_wheelPieceStats.readFailed;
            else if (!read.object)
                ++g_wheelPieceStats.noObject;
            else
            {
                for (int slot = 0; slot < count && !found; ++slot)
                    if (objects[slot] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == read.object)
                        found = objects[slot];
                if (!found)
                    ++g_wheelPieceStats.unlisted;
            }
            replay::entity::ReplayId id;
            if (found && IsPoseV2KnownObject(found))
            {
                ++g_wheelPieceStats.known;
                status = "known";
                id = KnownObjectId(found);
            }
            else if (found && replay::entities::Exists(found))
            {
                const std::size_t before = Ctx().recorder.props.size();
                RegisterPoseV2Prop(found, pending.vehicle, pending.vehicleHandle, replay::entity::PropOrigin::VehiclePiece, 0,
                    "vehicle_wheel", {}, replay::vehicle_pieces::WheelPieceGroup(pending.wheel));
                if (Ctx().recorder.props.size() > before)
                {
                    ++g_wheelPieceStats.recorded;
                    ++g_vehiclePiecesRecorded;
                    status = "recorded";
                    id = Ctx().recorder.props.back().id;
                }
                else
                    status = "refused";
            }
            else if (now < pending.deadline && !(error && *error && std::strcmp(error, "layout") == 0))
                continue;
            else
            {
                ++g_wheelPieceStats.lost;
                status = "lost";
            }
            if (g_wheelPieceLogs < 32)
            {
                ++g_wheelPieceLogs;
                char line[320];
                std::snprintf(line, sizeof(line),
                    "POSE_V2_VEHICLE_WHEEL_PIECE,phase=record,status=%s,vehicle=%s,wheel=%d,slot=%d,handle=%d,model=0x%x,"
                    "slot_model=0x%x,front_model=0x%x,rear_model=0x%x,id=%s,error=%s,off_ms=%u,time_ms=%u\n",
                    status, replay::entity::IdText(pending.vehicle).c_str(), pending.wheel, pending.slot, found,
                    found ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(found)) : 0u, read.model, read.frontModel,
                    read.rearModel, id.Valid() ? replay::entity::IdText(id).c_str() : "none",
                    error && *error ? error : "none", pending.offMs, replay::pose::capture::NowMs());
                Ctx().log << line;
            }
            g_wheelPiecesPending.erase(g_wheelPiecesPending.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    void SnapshotPoseV2World()
    {
        g_worldSnapshot.clear();
        g_worldTaken.clear();
        g_worldObjectsRecorded = 0;
        g_worldPiecesRecorded = 0;
        g_fragBreaks = g_fragBreaksMatched = g_fragLogged = 0;
        g_fragBrokenObjects.clear();
        g_fragPending.clear();
        g_vehiclePiecesPending.clear();
        g_vehiclePiecesRecorded = g_vehiclePiecesLost = 0;
        g_vehiclePartsUrgent.clear();
        g_wheelPiecesPending.clear();
        g_wheelPieceStats = {};
        g_wheelPieceLogs = 0;
        g_fragPieceCalls.clear();
        g_fragPiecesRecorded = g_fragPiecesFromCache = g_fragPieceLogs = 0;
        g_worldAnimatedRecorded = g_worldAnimatedWatched = g_worldLateMapObjects = 0;
        g_nearWorldObjects.clear();
        g_nearWorldTaken = g_openAtStartTaken = 0;
        g_worldSkeletonStats = {};
        g_worldLateJoinDecided.clear();
        g_sceneryChildLive.clear();
        g_sceneryChildObjects.clear();
        g_sceneryChildScanned.clear();
        g_sceneryChildStats = {};
        g_sceneryLive.clear();
        g_sceneryOf.clear();
        g_sceneryPending.clear();
        g_scenerySeen.clear();
        g_sceneryFar.clear();
        g_lateSceneryLogs = 0;
        g_sceneryNextScanMs = 0;
        g_sceneryStats = {};
        g_hatLookStats = {};
        ResetPoseV2Loot();
        g_mapOwner = replay::objcheck::kOwnerUnknown;
        g_mapDressingPending.clear();
        g_mapDressingStats = {};
        g_dressingGonePending.clear();
        for (std::uint32_t& n : g_snapshotRules)
            n = 0;
        g_objectWeaponFromObject = g_objectWeaponUnresolved = 0;
        g_objectCheckLogs = 0;
        std::map<std::uint32_t, std::uint32_t> owners;
        WarmUpFrag(); // already done at mod load
        g_fragCursor = replay::frag::Total();
        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        // objcheck::WatchBefore: the bones are read nearest first (to the
        // player or a recorded actor), not in list order: the cap of
        // kMaxWatchedAnimated skeletons goes to the objects the recording can
        // reach.
        static std::vector<Vector3> watchers;
        watchers.clear();
        watchers.push_back(player);
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            float p[3]{};
            if (!entity.vehicle && replay::entities::Position(entity.handle, p))
                watchers.push_back(Vector3{p[0], p[1], p[2]});
        }
        struct SnapCandidate
        {
            Object object = 0;
            Vector3 position{};
            float distanceM = 0.0f;
            std::uint32_t index = 0;
        };
        static std::vector<SnapCandidate> candidates;
        candidates.clear();
        for (int slot = 0; slot < count; ++slot)
        {
            const Object object = objects[slot];
            if (!object || !replay::entities::Exists(object) || ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            if (Distance2(at, player) > 60.0f * 60.0f)
                continue;
            float best = Distance2(at, player);
            for (const Vector3& watcher : watchers)
                best = (std::min)(best, Distance2(at, watcher));
            candidates.push_back({object, at, std::sqrt(best), static_cast<std::uint32_t>(slot)});
        }
        std::sort(candidates.begin(), candidates.end(), [](const SnapCandidate& a, const SnapCandidate& b) {
            return replay::objcheck::WatchBefore(a.distanceM, a.index, b.distanceM, b.index);
        });
        for (const SnapCandidate& candidate : candidates)
        {
            const Object object = candidate.object;
            WorldSnap snap;
            snap.position = candidate.position;
            snap.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            if (replay::ropemodels::IsRopeModel(snap.model))
            {
                ++g_poseV2RopeModelsSkipped; // the rope track's
                continue;
            }
            snap.rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
            snap.broken = OBJECT::HAS_OBJECT_BEEN_BROKEN(object) != FALSE;
            // The bones of an object with a skeleton.
            // Read or not, the state the reread starts from (an
            // object past the cap is unread, one without a skeleton absent).
            if (g_worldAnimatedWatched < replay::objcheck::kMaxWatchedAnimated)
            {
                if (ReadPoseV2SpawnLocals(object, snap.bones, snap.locals))
                {
                    ++g_worldAnimatedWatched;
                    snap.skeleton = replay::objcheck::SkeletonWatch::Present;
                    g_worldSkeletonStats.watchedFarthestM = candidate.distanceM;
                    // objcheck::OpenAtStart: within reach and away from its
                    // rest pose (a drawer or a lid left open): recorded with
                    // its pose from the first tick, never drawn closed.
                    if (replay::objcheck::NearWatch(candidate.distanceM))
                    {
                        replay::objcheck::RestMatch rest;
                        snap.openAtStart = replay::objcheck::OpenAtStart(PoseV2RestOf(object, snap.locals, snap.bones, rest));
                    }
                }
                else
                {
                    snap.locals.clear();
                    snap.skeleton = replay::objcheck::SkeletonWatch::Absent;
                }
            }
            else
            {
                snap.locals.clear();
                g_worldSkeletonStats.unreadNearM10 += candidate.distanceM <= 10.0f ? 1u : 0u;
            }
            // An object of the scene that is not of the map.
            std::uint32_t owner = replay::objcheck::kOwnerUnknown;
            bool mission = false;
            bool pickup = false;
            const bool scenery = PoseV2SceneryCandidate(object, owner, mission, &pickup);
            // Why (scenery_by_rule).
            const replay::objcheck::SceneryRule rule = replay::objcheck::SceneryRuleOf(mission, owner, pickup);
            ++g_snapshotRules[static_cast<std::size_t>(rule)];
            if (!scenery && rule == replay::objcheck::SceneryRule::MapData &&
                g_mapDressingPending.size() < replay::objcheck::kMaxMapDressing)
            {
                // Map data: a map dressing entry if a clip IPL holds it
                // (ResolvePoseV2MapDressing, once the spheres are read).
                MapDressingPending dressing;
                dressing.object = object;
                dressing.position[0] = snap.position.x;
                dressing.position[1] = snap.position.y;
                dressing.position[2] = snap.position.z;
                dressing.rotation[0] = snap.rotation.x;
                dressing.rotation[1] = snap.rotation.y;
                dressing.rotation[2] = snap.rotation.z;
                dressing.owner = owner;
                g_mapDressingPending.push_back(dressing);
            }
            else if (!scenery && rule == replay::objcheck::SceneryRule::MapData)
                ++g_mapDressingStats.full;
            if (scenery)
            {
                const float p[3]{snap.position.x, snap.position.y, snap.position.z};
                const float r[3]{snap.rotation.x, snap.rotation.y, snap.rotation.z};
                const std::size_t before = Ctx().clip.scenery.size();
                AddPoseV2Scenery(object, p, r, 0, owner, mission, rule);
                ++g_sceneryStats.snapshot;
                if (pickup && Ctx().clip.scenery.size() > before)
                {
                    ++g_sceneryStats.pickups;
                    LogPoseV2PickupEntry("pickup_snapshot");
                }
            }
            else
                ++owners[owner];
            g_worldSnapshot[object] = snap;
        }
        // The map's owner value: the most frequent among the objects that are
        // no scenery, trusted from 8 objects on (logged for the bench).
        std::uint32_t best = 0;
        for (const auto& [owner, n] : owners)
            if (owner != replay::objcheck::kOwnerUnknown && n > best)
            {
                best = n;
                g_mapOwner = owner;
            }
        if (best < 8)
            g_mapOwner = replay::objcheck::kOwnerUnknown;
        g_mapDressingStats.listFull = count >= 1024;
        Ctx().log << "POSE_V2_WORLD_SNAPSHOT,objects=" << g_worldSnapshot.size() << ",listed=" << count
                  << ",animated_watched=" << g_worldAnimatedWatched
                  << ",watch_order=nearest,watched_farthest_m=" << static_cast<int>(g_worldSkeletonStats.watchedFarthestM)
                  << ",unread_within_10m=" << g_worldSkeletonStats.unreadNearM10
                  << ",scenery=" << g_sceneryStats.snapshot
                  << ",map_owner=" << g_mapOwner << ",owners=";
        for (const auto& [owner, n] : owners)
            Ctx().log << owner << ':' << n << '|';
        // The rule of each snapshot object (objcheck::SceneryRule; map_data
        // ones wait for the IPL spheres: map_dressing_pending).
        Ctx().log << ",scenery_by_rule=";
        for (std::size_t rule = 0; rule < replay::objcheck::kSceneryRuleCount; ++rule)
            if (g_snapshotRules[rule])
                Ctx().log << replay::objcheck::SceneryRuleText(static_cast<replay::objcheck::SceneryRule>(rule)) << ':'
                          << g_snapshotRules[rule] << '|';
        Ctx().log << ",map_dressing_pending=" << g_mapDressingPending.size() << ",map_dressing_over_cap=" << g_mapDressingStats.full;
        Ctx().log << '\n';
    }

    namespace
    {

    // object_check.h map dressing: the snapshot's map data (nearest first,
    // kMaxMapDressing) becomes a map dressing entry with the clip IPL whose
    // sphere holds it (the smallest one on now: a camp's own IPL), 0 for the
    // always-on map. Once, as soon as the world state's recording has its
    // spheres (it starts after the snapshot).
    void ResolvePoseV2MapDressing()
    {
        if (g_mapDressingStats.resolved || !Ctx().clip.world.recorded)
            return;
        g_mapDressingStats.resolved = true;
        std::vector<replay::objcheck::DressingSphere> spheres;
        for (const replay::IplSphere& sphere : Ctx().clip.world.spheres)
        {
            if (!(sphere.radius > 0.0f) || !STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(sphere.hash)))
                continue;
            replay::objcheck::DressingSphere s;
            s.hash = sphere.hash;
            for (int axis = 0; axis < 3; ++axis)
                s.center[axis] = sphere.center[axis];
            s.radius = sphere.radius;
            spheres.push_back(s);
        }
        g_mapDressingStats.spheres = static_cast<std::uint32_t>(spheres.size());
        for (const MapDressingPending& pending : g_mapDressingPending)
        {
            ++g_mapDressingStats.candidates;
            if (!replay::entities::Exists(pending.object))
            {
                ++g_mapDressingStats.gone;
                continue;
            }
            if (g_sceneryOf.count(pending.object) || g_lootWatch.count(pending.object) || IsPoseV2KnownObject(pending.object))
                continue; // another path has it already
            const float at[3]{pending.position[0], pending.position[1], pending.position[2]};
            const std::uint32_t ipl = replay::objcheck::DressingIplAt(at, spheres);
            if (!ipl)
                ++g_mapDressingStats.noIpl; // the always-on map's: built only when missing at playback
            const std::size_t before = Ctx().clip.scenery.size();
            AddPoseV2Scenery(pending.object, pending.position, pending.rotation, 0, pending.owner, false,
                replay::objcheck::SceneryRule::MapDressing, ipl);
            if (Ctx().clip.scenery.size() == before)
            {
                ++g_mapDressingStats.full;
                continue;
            }
            ++g_mapDressingStats.entries;
            if (ipl)
                g_mapDressingStats.ipls.insert(ipl);
        }
        Ctx().log << "POSE_V2_MAP_DRESSING,phase=record,candidates=" << g_mapDressingStats.candidates
                  << ",entries=" << g_mapDressingStats.entries << ",no_ipl=" << g_mapDressingStats.noIpl
                  << ",gone=" << g_mapDressingStats.gone << ",full=" << g_mapDressingStats.full
                  << ",spheres_on=" << g_mapDressingStats.spheres << ",ipls=";
        for (const std::uint32_t ipl : g_mapDressingStats.ipls)
            Ctx().log << "0x" << std::hex << ipl << std::dec << '|';
        Ctx().log << '\n';
        // With Debug=0 too (_SUMMARY): the map data of the snapshot the clip
        // does not have (missing: past the cap or the scenery cap), and whether
        // the snapshot's list was full (more map data may lie beyond it).
        Ctx().log << "POSE_V2_MAP_DRESSING_SUMMARY,entries=" << g_mapDressingStats.entries
                  << ",cap=" << replay::objcheck::kMaxMapDressing << ",missing=" << g_mapDressingStats.full
                  << ",gone=" << g_mapDressingStats.gone << ",list_full=" << (g_mapDressingStats.listFull ? 1 : 0)
                  << ",scenery=" << Ctx().clip.scenery.size() << ",scenery_cap=" << replay::objcheck::kMaxScenery << '\n';
        g_mapDressingPending.clear();
    }
    }

    // A snapshot object that moved, broke or animated becomes a prop (with
    // its pose when it has a skeleton, whatever made it change), owned by the
    // nearest recorded actor; the scenery or the world object origin hands
    // its stand-in over.
    void TakePoseV2WorldObject(Object object, WorldSnap& was, replay::entity::ReplayId actor, Entity actorHandle,
        const char* how, bool animated, std::uint32_t nowMs)
    {
        was.taken = true;
        const std::size_t before = Ctx().recorder.props.size();
        RegisterPoseV2Prop(object, actor, actorHandle, replay::entity::PropOrigin::WorldObject, 0, how, {}, 0, nullptr,
            was.bones >= replay::object_pose::kMinBones);
        if (Ctx().recorder.props.size() == before)
            return;
        if (animated)
            ++g_worldAnimatedRecorded;
        if (was.bones >= replay::object_pose::kMinBones && !Ctx().recorder.props.back().posed &&
            !Ctx().recorder.props.back().poseCandidate)
            LogObjectCheck(replay::objcheck::Verdict::AnimatedUnposed, replay::entity::IdText(Ctx().recorder.props.back().id),
                was.model, nowMs, ",phase=record");
        // Scenery hands over to the prop (its stand-in and the original's
        // hide are the scenery's).
        if (MarkPoseV2SceneryTaken(object, Ctx().recorder.props.back().id))
        {
            g_worldTaken.push_back({object, was.model});
            ++g_worldObjectsRecorded;
            return;
        }
        replay::WorldObjectOrigin origin;
        origin.id = Ctx().recorder.props.back().id;
        origin.model = was.model;
        origin.position[0] = was.position.x;
        origin.position[1] = was.position.y;
        origin.position[2] = was.position.z;
        origin.rotation[0] = was.rotation.x;
        origin.rotation[1] = was.rotation.y;
        origin.rotation[2] = was.rotation.z;
        Ctx().clip.worldObjects.push_back(origin);
        g_worldTaken.push_back({object, was.model});
        ++g_worldObjectsRecorded;
    }

    void ScanPoseV2WorldObjects()
    {
        if (g_worldSnapshot.empty())
            return;
        CollectPoseV2FragBreaks();
        FlushPoseV2VehiclePieces();
        REPLAY_PROFILE_ZONE("rec.world_objects");
        // The actors' and the objects' positions read from memory, the objects
        // from the scan's single walk (g_poseV2ScanObjects: existence and
        // parent already known).
        static std::vector<std::pair<Vector3, const replay::recorder::RecordedEntity*>> actors;
        actors.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            float p[3]{};
            if (replay::entities::Position(entity.handle, p))
                actors.emplace_back(Vector3{p[0], p[1], p[2]}, &entity);
        }
        g_nearWorldObjects.clear();
        if (actors.empty())
            return;
        std::uint32_t skeletonReads = 0; // this scan's rereads
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
        {
            const Object object = listed.object;
            if (listed.parent || IsPoseV2KnownObject(object))
                continue;
            const auto snap = g_worldSnapshot.find(object);
            if (snap != g_worldSnapshot.end() && snap->second.taken)
                continue;
            float atXyz[3]{};
            if (!replay::entities::Position(object, atXyz))
                continue;
            const Vector3 at{atXyz[0], atXyz[1], atXyz[2]};
            const replay::recorder::RecordedEntity* nearest = nullptr;
            float best = 30.0f * 30.0f;
            for (const auto& [position, entity] : actors)
            {
                const float d = Distance2(at, position);
                if (d < best)
                {
                    best = d;
                    nearest = entity;
                }
            }
            if (!nearest)
                continue;
            // Attached to something that is not an entity (no parent
            // handle): not a world object. One native, only for
            // the candidates left.
            if (ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            if (snap != g_worldSnapshot.end())
            {
                WorldSnap& was = snap->second;
                // objcheck::NearWatch: read every tick until the next scan.
                was.watchedNear = !was.locals.empty() && replay::objcheck::NearWatch(std::sqrt(best));
                if (was.watchedNear)
                    g_nearWorldObjects.push_back({object, nearest->id, nearest->handle});
                const Vector3 rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
                const bool moved = Distance2(at, was.position) > 0.15f * 0.15f ||
                    AngleDelta(rotation.x, was.rotation.x) > 8.0f || AngleDelta(rotation.y, was.rotation.y) > 8.0f ||
                    AngleDelta(rotation.z, was.rotation.z) > 8.0f;
                const bool broke = (!was.broken && OBJECT::HAS_OBJECT_BEEN_BROKEN(object)) ||
                    std::find(g_fragBrokenObjects.begin(), g_fragBrokenObjects.end(), object) != g_fragBrokenObjects.end();
                // Bones moving in place (a lid opening, a book's
                // pages), checked four times a second.
                bool animated = false;
                bool skeletonAppeared = false;
                const std::uint32_t nowMs = replay::pose::capture::NowMs();
                if (!moved && !broke && !was.locals.empty() && nowMs >= was.nextLocalsMs)
                {
                    was.nextLocalsMs = nowMs + 250;
                    static std::vector<float> locals;
                    std::uint32_t bones = 0;
                    const bool read = ReadPoseV2SpawnLocals(object, bones, locals);
                    if (read && bones == was.bones)
                        animated = replay::object_pose::spawn::LocalsChanged(was.locals.data(), locals.data(), bones);
                    else if (read)
                    {
                        // objcheck::AfterRebuild: the skeleton rebuilt with
                        // another bone count was never compared again; on
                        // scenery it is an animation, on a map object the new
                        // baseline.
                        const bool scenery = g_sceneryOf.count(object) != 0;
                        was.bones = bones;
                        was.locals = locals;
                        // objcheck::RestOf: a map object's skeleton rebuilt
                        // away from its bind pose is an animation too.
                        replay::objcheck::RestMatch rest;
                        const replay::objcheck::RestState restState =
                            scenery ? replay::objcheck::RestState::Unknown : PoseV2RestOf(object, locals, bones, rest);
                        if (!scenery)
                            NotePoseV2Rest(restState);
                        const bool appeared = replay::objcheck::AfterRebuild(scenery, restState) ==
                            replay::objcheck::SkeletonReread::Appeared;
                        if (appeared)
                        {
                            ++g_worldSkeletonStats.appeared;
                            g_worldSkeletonStats.appearedMap += scenery ? 0u : 1u;
                            skeletonAppeared = animated = true;
                        }
                        if (g_worldSkeletonStats.logs++ < 32)
                        {
                            const replay::logfmt::FormatScope logFormat(Ctx().log);
                            Ctx().log << std::fixed << std::setprecision(3) << "POSE_V2_WORLD_SKELETON,status="
                                      << (appeared ? "rebuilt_appeared" : "rebuilt_baseline")
                                      << ",handle=" << object << ",model=0x" << std::hex << was.model << std::dec
                                      << ",bones=" << bones << ",scenery=" << (scenery ? 1 : 0)
                                      << ",rest=" << replay::objcheck::RestText(restState) << ",turned=" << rest.turned
                                      << ",moved=" << rest.moved << ",max_turn_rad=" << rest.maxTurnRad
                                      << ",max_move_m=" << rest.maxMoveM << ",time_ms=" << nowMs << '\n';
                            Ctx().log.unsetf(std::ios_base::floatfield);
                        }
                    }
                }
                else if (!moved && !broke && was.locals.empty() &&
                    replay::objcheck::ShouldRereadSkeleton(was.skeleton, std::sqrt(best), nowMs, was.nextSkeletonMs, skeletonReads))
                {
                    // objcheck::AfterReread: the skeleton of an object near a
                    // recorded actor, read again (a mission chest has none at
                    // the snapshot); a skeleton the engine builds on a scenery
                    // object while the recording runs is the start of an
                    // animation (the lid), recorded now with its pose through
                    // the animated world object path below; on a map object it
                    // is the baseline.
                    ++skeletonReads;
                    ++g_worldSkeletonStats.rereads;
                    was.nextSkeletonMs = nowMs + replay::objcheck::kSkeletonRereadMs;
                    static std::vector<float> fresh;
                    std::uint32_t bones = 0;
                    const bool resolved = ReadPoseV2SpawnLocals(object, bones, fresh);
                    const replay::objcheck::SkeletonWatch before = was.skeleton;
                    // objcheck::RestOf: a skeleton that appears on a map object
                    // away from its bind pose (the drawer already sliding out)
                    // is an animation.
                    const bool rereadScenery = g_sceneryOf.count(object) != 0;
                    replay::objcheck::RestMatch rest;
                    replay::objcheck::RestState restState = replay::objcheck::RestState::Unknown;
                    if (resolved && !rereadScenery && before == replay::objcheck::SkeletonWatch::Absent)
                    {
                        restState = PoseV2RestOf(object, fresh, bones, rest);
                        NotePoseV2Rest(restState);
                    }
                    const replay::objcheck::SkeletonReread result =
                        replay::objcheck::AfterReread(before, resolved, bones, g_worldSkeletonStats.baselines,
                            rereadScenery, restState);
                    was.skeleton = replay::objcheck::NextSkeletonWatch(before, result, resolved);
                    if (result != replay::objcheck::SkeletonReread::Nothing)
                    {
                        was.bones = bones;
                        was.locals = fresh;
                        was.nextLocalsMs = nowMs + 250;
                    }
                    if (result == replay::objcheck::SkeletonReread::Baseline)
                        ++g_worldSkeletonStats.baselines;
                    else if (result == replay::objcheck::SkeletonReread::Appeared)
                    {
                        ++g_worldSkeletonStats.appeared;
                        g_worldSkeletonStats.appearedMap += rereadScenery ? 0u : 1u;
                        skeletonAppeared = animated = true;
                    }
                    if (result != replay::objcheck::SkeletonReread::Nothing && g_worldSkeletonStats.logs++ < 32)
                    {
                        const replay::logfmt::FormatScope logFormat(Ctx().log);
                        Ctx().log << std::fixed << std::setprecision(2) << "POSE_V2_WORLD_SKELETON,status="
                                  << (skeletonAppeared ? "appeared" : "baseline") << ",handle=" << object << ",model=0x"
                                  << std::hex << was.model << std::dec << ",bones=" << bones
                                  << ",scenery=" << (rereadScenery ? 1 : 0)
                                  << ",was=" << (before == replay::objcheck::SkeletonWatch::Unread ? "unread" : "absent")
                                  << ",rest=" << replay::objcheck::RestText(restState) << ",turned=" << rest.turned
                                  << ",moved=" << rest.moved << ",max_move_m=" << rest.maxMoveM
                                  << ",distance_m=" << std::sqrt(best) << ",nearest=" << replay::entity::IdText(nearest->id)
                                  << ",time_ms=" << nowMs << '\n';
                        Ctx().log.unsetf(std::ios_base::floatfield);
                    }
                }
                if (!moved && !broke && !animated)
                    continue;
                TakePoseV2WorldObject(object, was, nearest->id, nearest->handle,
                    broke ? "world_broke" : skeletonAppeared ? "world_skeleton" : animated ? "world_animated" : "world_moved",
                    animated, nowMs);
                continue;
            }
            // A new object: a piece when it has the model of a taken world
            // object close by.
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            bool piece = false;
            for (const WorldTaken& taken : g_worldTaken)
            {
                if (taken.model != model || !replay::entities::Exists(taken.object) ||
                    Distance2(at, ENTITY::GET_ENTITY_COORDS(taken.object, TRUE, FALSE)) > 5.0f * 5.0f)
                    continue;
                RegisterPoseV2Prop(object, nearest->id, nearest->handle, replay::entity::PropOrigin::WorldPiece, 0, "world_piece");
                ++g_worldPiecesRecorded;
                piece = true;
                break;
            }
            // objcheck::LateJoinWatch: a map object first listed after the
            // snapshot (the player rode into town) joins the watch once a
            // recorded actor comes within reach, so its drawer's skeleton is
            // read again like a snapshot object's. Never a piece: one of the
            // model of a watched object within 5 m is that object's fragment
            // (the world piece rule above).
            if (piece || best > replay::objcheck::kSkeletonNearM * replay::objcheck::kSkeletonNearM ||
                !g_worldLateJoinDecided.insert(object).second)
                continue;
            bool fragmentLike = false;
            for (const auto& [watched, was] : g_worldSnapshot)
                if (was.model == model && Distance2(at, was.position) <= 5.0f * 5.0f)
                {
                    fragmentLike = true;
                    break;
                }
            if (!fragmentLike &&
                replay::objcheck::LateJoinWatch(false, false, std::sqrt(best), PoseV2MapOwned(object), g_worldSkeletonStats.lateJoins) &&
                JoinPoseV2LateMapObject(object, at))
                ++g_worldSkeletonStats.lateJoins;
        }
    }
    PoseV2SpawnStats g_spawnStats;

    void ResetPoseV2Spawned()
    {
        g_spawnWatch.clear();
        g_spawnSeen.clear();
        g_spawnRecheck.clear();
        g_spawnStats = {};
        g_spawnLogs = 0;
        g_spawnLateLogs = 0;
    }

    void LogPoseV2SpawnedSummary()
    {
        Ctx().log << "POSE_V2_SPAWNED_OBJECTS,watched=" << g_spawnStats.watched << ",recorded=" << g_spawnStats.recorded
                  << ",moved=" << g_spawnStats.moved << ",animated=" << g_spawnStats.animated
                  << ",expired=" << g_spawnStats.expired << ",gone=" << g_spawnStats.gone
                  << ",taken=" << g_spawnStats.taken << ",attached=" << g_spawnStats.attached
                  << ",elsewhere=" << g_spawnStats.elsewhere << ",excluded=" << g_spawnStats.excluded
                  << ",full=" << g_spawnStats.full << ",refused=" << g_spawnStats.refused
                  << ",scenery=" << g_spawnStats.scenery << ",scenery_map=" << g_spawnStats.sceneryMap
                  << ",scenery_already=" << g_spawnStats.sceneryAlready << ",scenery_full=" << g_spawnStats.sceneryFull
                  << ",scenery_would=" << g_spawnStats.sceneryWould
                  << ",static_scenery=" << g_spawnStats.staticScenery
                  << ",watching=" << g_spawnWatch.size() << ",seen=" << g_spawnSeen.size()
                  // The recheck; recorded_rewatched is what the old rule lost
                  // (watch_order=nearest: candidates nearest first, a nearer
                  // one takes a far still place).
                  << ",watch_order=nearest,first_far=" << g_spawnStats.firstFar << ",rechecks=" << g_spawnStats.rechecks
                  << ",rewatched=" << g_spawnStats.rewatched << ",recorded_rewatched=" << g_spawnStats.recordedRewatched
                  << ",rearmed=" << g_spawnStats.rearmed << ",left=" << g_spawnStats.left
                  << ",evicted=" << g_spawnStats.evicted << ",recheck_table=" << g_spawnRecheck.size()
                  << ",recheck_resets=" << g_spawnStats.recheckResets << '\n';
    }

    void NotePoseV2Rest(replay::objcheck::RestState rest)
    {
        if (rest == replay::objcheck::RestState::Away)
            ++g_worldSkeletonStats.restAway;
        else if (rest == replay::objcheck::RestState::AtRest)
            ++g_worldSkeletonStats.restAtRest;
        else
            ++g_worldSkeletonStats.restUnknown;
    }

    namespace
    {

    // A late scenery entry joins the world watch like the snapshot's scenery,
    // so it is recorded when it moves, breaks or animates (its skeleton read
    // now, or again near an actor). Shared by the scenery's settle and the
    // spawned object watch's end.
    void JoinPoseV2WorldWatch(Object object, const float position[3], const float rotation[3])
    {
        if (g_worldSnapshot.count(object))
            return;
        WorldSnap snap;
        snap.position = {position[0], position[1], position[2]};
        snap.rotation = {rotation[0], rotation[1], rotation[2]};
        snap.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
        snap.broken = OBJECT::HAS_OBJECT_BEEN_BROKEN(object) != FALSE;
        if (g_worldAnimatedWatched < replay::objcheck::kMaxWatchedAnimated)
        {
            if (ReadPoseV2SpawnLocals(object, snap.bones, snap.locals))
            {
                ++g_worldAnimatedWatched;
                snap.skeleton = replay::objcheck::SkeletonWatch::Present;
            }
            else
            {
                snap.locals.clear();
                snap.skeleton = replay::objcheck::SkeletonWatch::Absent;
            }
        }
        g_worldSnapshot[object] = std::move(snap);
        ++g_worldSkeletonStats.lateScenery;
    }

    // A watched object still for a second becomes a late
    // scenery entry where it was first seen, appearing then, unless the
    // scenery has it already or its owner is the map's (or a rope's).
    replay::objcheck::SpawnScenery DecidePoseV2SpawnScenery(Object object, PoseV2SpawnWatch& watch, std::uint32_t ageMs,
        std::uint32_t nowMs, std::uint32_t& owner, bool& mission, bool& wouldAnyOwner)
    {
        wouldAnyOwner = false;
        owner = replay::objcheck::kOwnerUnknown;
        mission = false;
        // No natives for the owner while the first second
        // runs (the watch scan runs with every recorded frame).
        if (ageMs < replay::objcheck::kSpawnSceneryAfterMs)
            return replay::objcheck::SpawnScenery::Wait;
        const bool already = g_sceneryOf.count(object) || g_sceneryPending.count(object) || g_sceneryChildObjects.count(object);
        bool qualifies = false;
        if (!already)
        {
            const bool candidate = PoseV2SceneryCandidate(object, owner, mission);
            const bool anyOwner = replay::objcheck::SpawnSceneryOwner(mission, owner,
                replay::ropemodels::IsRopeModel(watch.model), replay::objcheck::MapOwnerMask());
            qualifies = candidate;
            wouldAnyOwner = !qualifies && anyOwner;
        }
        replay::objcheck::SpawnScenery verdict =
            replay::objcheck::SpawnSceneryVerdict(ageMs, already, qualifies, g_spawnStats.scenery);
        if (verdict == replay::objcheck::SpawnScenery::Wait)
            return verdict;
        watch.sceneryDecided = true;
        if (verdict == replay::objcheck::SpawnScenery::Add)
        {
            const std::size_t before = Ctx().clip.scenery.size();
            AddPoseV2Scenery(object, watch.position, watch.rotation, watch.firstMs, owner, mission,
                replay::objcheck::SceneryRuleOf(mission, owner, false));
            if (Ctx().clip.scenery.size() == before)
                verdict = replay::objcheck::SpawnScenery::Full; // the scenery's own cap (counted there)
            else
            {
                watch.scenery = true;
                g_scenerySeen.insert(object);
                ++g_spawnStats.scenery;
                ++g_sceneryStats.late;
                ++g_sceneryStats.spawnWatch;
                LogPoseV2LateScenery("spawn_watch", object, owner, mission, nowMs);
            }
        }
        if (verdict == replay::objcheck::SpawnScenery::Already)
            ++g_spawnStats.sceneryAlready;
        else if (verdict == replay::objcheck::SpawnScenery::Map)
        {
            ++g_spawnStats.sceneryMap;
            g_spawnStats.sceneryWould += wouldAnyOwner ? 1u : 0u;
        }
        else if (verdict == replay::objcheck::SpawnScenery::Full)
            ++g_spawnStats.sceneryFull;
        return verdict;
    }
    }

    void ScanPoseV2SpawnedObjects()
    {
        namespace spawn = replay::object_pose::spawn;
        REPLAY_PROFILE_ZONE("rec.spawned_objects");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        static std::vector<std::pair<Vector3, const replay::recorder::RecordedEntity*>> peds;
        peds.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            float p[3]{};
            if (!entity.vehicle && replay::entities::Position(entity.handle, p))
                peds.emplace_back(Vector3{p[0], p[1], p[2]}, &entity);
        }
        if (peds.empty())
            return;
        const auto nearestPed = [&](const float at[3], float& meters) -> const replay::recorder::RecordedEntity* {
            const replay::recorder::RecordedEntity* nearest = nullptr;
            float best = std::numeric_limits<float>::max();
            for (const auto& [position, entity] : peds)
            {
                const float d = Distance2(Vector3{at[0], at[1], at[2]}, position);
                if (d < best)
                {
                    best = d;
                    nearest = entity;
                }
            }
            meters = nearest ? std::sqrt(best) : -1.0f;
            return nearest;
        };
        // `late` lines (what a rewatch
        // recorded or refused) on their own budget.
        const auto logLine = [&](const char* status, Object object, std::uint32_t model, Entity ownerHandle,
                                 float meters, std::uint32_t bones, const char* extra, bool late = false) {
            std::uint32_t& spent = late ? g_spawnLateLogs : g_spawnLogs;
            if (spent >= (late ? kSpawnLateLogCap : kSpawnLogCap))
                return;
            ++spent;
            const replay::recorder::RecordedEntity* owner = Ctx().recorder.FindByHandle(ownerHandle);
            const replay::logfmt::FormatScope logFormat(Ctx().log); // precision not left behind
            Ctx().log << std::fixed << std::setprecision(2) << "POSE_V2_OBJECT_SPAWN,status=" << status
                      << ",handle=" << object << ",model=0x" << std::hex << model << std::dec
                      << ",owner=" << (owner ? replay::entity::IdText(owner->id) : std::string("none"))
                      << ",distance_m=" << meters << ",bones=" << bones << extra << ",time_ms=" << nowMs << '\n';
            Ctx().log.unsetf(std::ios_base::floatfield);
        };

        // An object refused, expired or left goes to the recheck table (seen,
        // not decided). Past kMaxRecheck the table and the seen set start
        // over (the known and snapshot objects drop out again at once).
        const auto toRecheck = [&](Object object, const float at[3], bool armed, Entity expiredBy) -> PoseV2SpawnRecheck& {
            if (g_spawnRecheck.size() >= spawn::kMaxRecheck && !g_spawnRecheck.count(object))
            {
                g_spawnRecheck.clear();
                g_spawnSeen.clear();
                for (const auto& [watched, unused] : g_spawnWatch)
                    g_spawnSeen.insert(watched);
                ++g_spawnStats.recheckResets;
            }
            PoseV2SpawnRecheck& state = g_spawnRecheck[object];
            state.nextMs = nowMs + spawn::kRecheckMs;
            for (int axis = 0; axis < 3; ++axis)
                state.position[axis] = at[axis];
            state.armed = armed;
            state.expiredBy = expiredBy;
            return state;
        };
        const auto recordedParent = [&](Entity parent) {
            return Ctx().recorder.FindByHandle(parent) != nullptr || IsPoseV2KnownObject(parent) ||
                PoseV2VehicleParent(parent) != nullptr; // a recorded train car
        };

        // Watched objects: recorded once they move or animate.
        static std::vector<float> locals;
        for (auto it = g_spawnWatch.begin(); it != g_spawnWatch.end();)
        {
            const Object object = it->first;
            PoseV2SpawnWatch& watch = it->second;
            const std::uint32_t ageMs = nowMs - watch.firstMs;
            if (!replay::entities::Exists(object))
            {
                ++g_spawnStats.gone;
                logLine("gone", object, watch.model, watch.ownerHandle, -1.0f, watch.bones, "");
                it = g_spawnWatch.erase(it);
                continue;
            }
            if (IsPoseV2KnownObject(object))
            {
                ++g_spawnStats.taken;
                logLine("taken", object, watch.model, watch.ownerHandle, -1.0f, watch.bones, "");
                it = g_spawnWatch.erase(it);
                continue;
            }
            if (const Entity parent = ENTITY::GET_ENTITY_ATTACHED_TO(object))
            {
                // Attached to an entity: the attached scan's when it is a
                // recorded one (an object picked up after an animation).
                // On an entity that is not recorded it is looked at again (the
                // ped may be recorded later, or put it down).
                ++g_spawnStats.attached;
                logLine("attached", object, watch.model, watch.ownerHandle, -1.0f, watch.bones, "");
                if (!recordedParent(parent))
                    toRecheck(object, watch.position, true, 0).elsewhere = true;
                it = g_spawnWatch.erase(it);
                continue;
            }
            float at[3]{};
            if (!replay::entities::Position(object, at))
            {
                ++it;
                continue;
            }
            const Vector3 rotationNow = ENTITY::GET_ENTITY_ROTATION(object, 2);
            const float rotation[3]{rotationNow.x, rotationNow.y, rotationNow.z};
            const bool moved = spawn::RootMoved(watch.position, at, watch.rotation, rotation);
            bool animated = false;
            std::uint32_t bones = 0;
            if (ReadPoseV2SpawnLocals(object, bones, locals))
            {
                if (!watch.locals.empty() && bones == watch.bones)
                    animated = spawn::LocalsChanged(watch.locals.data(), locals.data(), bones);
                else if (watch.locals.empty())
                {
                    // The skeleton came after the first sight: the baseline,
                    // and an animation when the watch had seen it without one
                    // (a still chair can become a late scenery entry at 1 s
                    // and get its skeleton later).
                    animated = watch.absentReads >= 2;
                    watch.bones = bones;
                    watch.locals = locals;
                }
            }
            else if (watch.locals.empty())
                ++watch.absentReads;
            // A piece a recorded object's break made is recorded at its first
            // sight (it is there from the break on; waiting for it to move
            // left the first 200 ms of the blast without it).
            const spawn::Verdict verdict =
                PoseV2PieceOfRecorded(object) ? spawn::Verdict::Record : spawn::Decide(moved, animated, ageMs);
            {
                float nearestM = -1.0f;
                nearestPed(at, nearestM);
                watch.nearestM = nearestM;
            }
            if (verdict == spawn::Verdict::Keep)
            {
                // objcheck::SpawnSceneryVerdict still for a second, it is a
                // late scenery entry when its owner is not the map's (a
                // camp's prop set objects end static and would be dropped: no
                // tent, bedroll or grate in the replay). Still watched: a move or an animation later makes it a
                // prop that takes over from the stand-in.
                if (!watch.sceneryDecided)
                {
                    std::uint32_t ownedBy = replay::objcheck::kOwnerUnknown;
                    bool mission = false;
                    bool would = false;
                    const replay::objcheck::SpawnScenery decided =
                        DecidePoseV2SpawnScenery(object, watch, ageMs, nowMs, ownedBy, mission, would);
                    if (decided != replay::objcheck::SpawnScenery::Wait && decided != replay::objcheck::SpawnScenery::Add)
                    {
                        char extra[64];
                        std::snprintf(extra, sizeof(extra), ",owned_by=%u,mission=%d,age_ms=%u", ownedBy, mission ? 1 : 0, ageMs);
                        logLine(would ? "would_scenery" : replay::objcheck::SpawnSceneryText(decided), object, watch.model,
                            watch.ownerHandle, -1.0f, watch.bones, extra);
                    }
                }
                // Still and no recorded ped near any more: its place goes to
                // the objects a ped can touch; looked at again (armed) while
                // listed. An entry of the late scenery joins the world watch,
                // as at the expiry.
                if (spawn::LeaveWatch(ageMs, watch.nearestM))
                {
                    ++g_spawnStats.left;
                    if (watch.scenery)
                        JoinPoseV2WorldWatch(object, watch.position, watch.rotation);
                    else
                        toRecheck(object, at, true, 0);
                    it = g_spawnWatch.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            float meters = -1.0f;
            const replay::recorder::RecordedEntity* owner = Ctx().recorder.FindByHandle(watch.ownerHandle);
            if (!owner)
                owner = nearestPed(at, meters);
            if (verdict == spawn::Verdict::Expire || !owner)
            {
                ++g_spawnStats.expired;
                // An entry of the watch joins the world
                // watch now (a later move, break or animation records it).
                if (watch.scenery)
                {
                    ++g_spawnStats.staticScenery;
                    JoinPoseV2WorldWatch(object, watch.position, watch.rotation);
                }
                else
                    toRecheck(object, at, false, watch.ownerHandle); // re-armed later
                char extra[48];
                std::snprintf(extra, sizeof(extra), ",scenery=%d,owned_by=%u", watch.scenery ? 1 : 0, ReadEntityOwner(object));
                logLine(owner ? "static" : "no_owner", object, watch.model, watch.ownerHandle, -1.0f, watch.bones, extra);
                it = g_spawnWatch.erase(it);
                continue;
            }
            const bool pose = spawn::PoseSpawned(animated, bones);
            const std::size_t before = Ctx().recorder.props.size();
            // A map object that became real after the snapshot
            // and then moved (the crate pushed before it broke) is a world
            // object: its stand-in where it was first seen, the original
            // hidden at playback, its breaks played.
            const bool mapObject = PoseV2MapObject(object);
            RegisterPoseV2Prop(object, owner->id, owner->handle,
                mapObject ? replay::entity::PropOrigin::WorldObject : replay::entity::PropOrigin::Projectile, 0,
                mapObject ? (animated ? "late_map_animated" : "late_map_moved")
                          : (animated ? "spawned_animated" : "spawned_moved"),
                {}, 0, nullptr, pose);
            const bool recorded = Ctx().recorder.props.size() > before;
            if (recorded && mapObject)
            {
                replay::WorldObjectOrigin origin;
                origin.id = Ctx().recorder.props.back().id;
                origin.model = watch.model;
                for (int axis = 0; axis < 3; ++axis)
                {
                    origin.position[axis] = watch.position[axis];
                    origin.rotation[axis] = watch.rotation[axis];
                }
                Ctx().clip.worldObjects.push_back(origin);
                g_worldTaken.push_back({object, watch.model});
                ++g_worldObjectsRecorded;
                ++g_worldLateMapObjects;
            }
            else if (recorded)
                MarkPoseV2SceneryTaken(object, Ctx().recorder.props.back().id);
            if (recorded)
            {
                ++g_spawnStats.recorded;
                g_spawnStats.moved += moved ? 1u : 0u;
                g_spawnStats.animated += animated ? 1u : 0u;
                g_spawnStats.recordedRewatched += watch.rewatched ? 1u : 0u;
            }
            else
                ++g_spawnStats.refused;
            char extra[160];
            std::snprintf(extra, sizeof(extra), ",moved=%d,animated=%d,age_ms=%u,pose=%d,posed=%d,id=%s",
                moved ? 1 : 0, animated ? 1 : 0, ageMs, pose ? 1 : 0,
                recorded && Ctx().recorder.props.back().posed ? 1 : 0,
                recorded ? replay::entity::IdText(Ctx().recorder.props.back().id).c_str() : "none");
            logLine(recorded ? "recorded" : "refused", object, watch.model, owner->handle, meters, bones, extra, watch.rewatched);
            it = g_spawnWatch.erase(it);
        }

        // First sights, and the objects of the recheck table that are due (an
        // object is not marked seen for good after one look).
        struct SpawnCandidate
        {
            Object object = 0;
            float meters = 0.0f;
            const replay::recorder::RecordedEntity* ped = nullptr;
            float at[3]{};
            std::uint32_t model = 0;
            bool rewatch = false;
        };
        static std::vector<SpawnCandidate> candidates;
        candidates.clear();
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
        {
            const Object object = listed.object;
            if (g_spawnWatch.count(object))
                continue;
            auto recheck = g_spawnRecheck.find(object);
            const bool rechecked = recheck != g_spawnRecheck.end();
            if (rechecked)
            {
                // A piece of a recorded object first seen before its break was
                // collected is looked at again at once.
                if (!spawn::RecheckDue(nowMs, recheck->second.nextMs) && !PoseV2PieceOfRecorded(object))
                    continue;
                ++g_spawnStats.rechecks;
                recheck->second.nextMs = nowMs + spawn::kRecheckMs;
            }
            else if (!g_spawnSeen.insert(object).second)
                continue;
            if (IsPoseV2KnownObject(object) || g_worldSnapshot.count(object))
            {
                if (rechecked)
                    g_spawnRecheck.erase(recheck);
                continue;
            }
            float at[3]{};
            if (!replay::entities::Position(object, at))
                continue;
            float meters = -1.0f;
            const replay::recorder::RecordedEntity* ped = nearestPed(at, meters);
            // A piece of a recorded object is within reach wherever it flies
            // (the wheel a blast threw 9 m from the player bursts there).
            if (ped && PoseV2PieceOfRecorded(object))
                meters = 0.0f;
            if (rechecked)
            {
                PoseV2SpawnRecheck& state = recheck->second;
                if (!state.armed)
                {
                    const float dx = at[0] - state.position[0];
                    const float dy = at[1] - state.position[1];
                    const float dz = at[2] - state.position[2];
                    const bool movedSince = dx * dx + dy * dy + dz * dz > spawn::kMoveM * spawn::kMoveM;
                    if (spawn::Rearm(false, movedSince, meters, ped && ped->handle == state.expiredBy))
                    {
                        state.armed = true;
                        ++g_spawnStats.rearmed;
                    }
                }
                if (!state.armed || !ped || !spawn::WithinReach(meters))
                    continue;
            }
            else if (!ped || !spawn::WithinReach(meters))
            {
                ++g_spawnStats.firstFar;
                toRecheck(object, at, true, 0);
                continue;
            }
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            if (listed.parent)
            {
                // On a recorded ped, weapon or prop: the attached scan's.
                if (recordedParent(listed.parent))
                {
                    if (rechecked)
                        g_spawnRecheck.erase(recheck);
                    continue;
                }
                PoseV2SpawnRecheck& state = rechecked ? recheck->second : toRecheck(object, at, true, 0);
                if (!state.elsewhere)
                {
                    state.elsewhere = true;
                    ++g_spawnStats.elsewhere;
                    char extra[96];
                    std::snprintf(extra, sizeof(extra), ",parent=%d,parent_type=%d", listed.parent,
                        static_cast<int>(ENTITY::GET_ENTITY_TYPE(listed.parent)));
                    logLine("attached_elsewhere", object, model, ped->handle, meters, 0, extra);
                }
                continue;
            }
            bool excluded = IsGorePieceModel(model) || STREAMING::IS_MODEL_A_VEHICLE(model) != FALSE ||
                replay::ropemodels::IsRopeModel(model);
            for (const replay::recorder::WeaponDrop& drop : Ctx().recorder.drops)
                excluded = excluded || drop.model == model;
            if (!spawn::WatchAtFirstSight(true, false, false, false, excluded, meters))
            {
                ++g_spawnStats.excluded;
                logLine("excluded", object, model, ped->handle, meters, 0, "");
                if (rechecked)
                    g_spawnRecheck.erase(recheck);
                continue;
            }
            SpawnCandidate candidate;
            candidate.object = object;
            candidate.meters = meters;
            candidate.ped = ped;
            for (int axis = 0; axis < 3; ++axis)
                candidate.at[axis] = at[axis];
            candidate.model = model;
            candidate.rewatch = rechecked;
            candidates.push_back(candidate);
        }
        // The nearest first; a full watch gives a still far place to a nearer
        // candidate (spawn::EvictFor), the evicted one goes back armed.
        std::sort(candidates.begin(), candidates.end(),
            [](const SpawnCandidate& a, const SpawnCandidate& b) { return a.meters < b.meters; });
        static std::vector<Object> evictable;
        static std::vector<float> evictableM;
        for (const SpawnCandidate& candidate : candidates)
        {
            if (g_spawnWatch.size() >= spawn::kMaxWatched)
            {
                evictable.clear();
                evictableM.clear();
                for (const auto& [watched, state] : g_spawnWatch)
                    if (!state.scenery && !state.rewatched)
                    {
                        evictable.push_back(watched);
                        evictableM.push_back(state.nearestM);
                    }
                const int victim = spawn::EvictFor(candidate.meters, evictableM.data(), evictableM.size());
                if (victim < 0)
                {
                    ++g_spawnStats.full;
                    if (!candidate.rewatch)
                        toRecheck(candidate.object, candidate.at, true, 0);
                    continue;
                }
                const auto out = g_spawnWatch.find(evictable[static_cast<std::size_t>(victim)]);
                toRecheck(out->first, out->second.position, true, 0);
                g_spawnWatch.erase(out);
                ++g_spawnStats.evicted;
            }
            const Object object = candidate.object;
            PoseV2SpawnWatch watch;
            watch.firstMs = nowMs;
            const Vector3 rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
            for (int axis = 0; axis < 3; ++axis)
                watch.position[axis] = candidate.at[axis];
            watch.rotation[0] = rotation.x;
            watch.rotation[1] = rotation.y;
            watch.rotation[2] = rotation.z;
            watch.ownerHandle = candidate.ped->handle;
            watch.model = candidate.model;
            watch.attachedNoParent = ENTITY::IS_ENTITY_ATTACHED(object) != FALSE;
            watch.nearestM = candidate.meters;
            watch.rewatched = candidate.rewatch;
            if (!ReadPoseV2SpawnLocals(object, watch.bones, watch.locals))
                watch.locals.clear();
            ++g_spawnStats.watched;
            if (candidate.rewatch)
            {
                ++g_spawnStats.rewatched;
                g_spawnRecheck.erase(object);
            }
            logLine(candidate.rewatch ? "rewatch" : "watch", object, candidate.model, candidate.ped->handle, candidate.meters,
                watch.bones, watch.attachedNoParent ? ",attached_no_parent=1" : ",attached_no_parent=0");
            g_spawnWatch.emplace(object, std::move(watch));
        }
    }

    // object_check.h LootTakeOf: a watched object taken (hidden or removed with
    // a recorded ped within reach) becomes a scenery entry: there from the
    // watch's appearance, drawn until the take. Still in the world, it stays a
    // live entry (later flips, its leaving) and hands over to the prop some
    // path made of it (the drop search takes the hidden shelf original as the
    // hand copy's replacement when the hand copy goes: 008 83:893 at 136853 ms,
    // 1.1 s after the hand copy 83:892 appeared; here at once when it is known
    // already, else at the live entries' next scan).
    void AddPoseV2Loot(Object object, const LootWatch& watch, replay::objcheck::LootTake take, std::uint32_t nowMs,
        bool exists)
    {
        if (Ctx().clip.scenery.size() >= replay::objcheck::kMaxScenery)
        {
            ++g_sceneryStats.full;
            return;
        }
        replay::SceneryObject entry;
        entry.model = watch.model;
        for (int axis = 0; axis < 3; ++axis)
        {
            entry.position[axis] = watch.position[axis];
            entry.rotation[axis] = watch.rotation[axis];
        }
        entry.appearMs = watch.appearMs;
        entry.owner = static_cast<std::uint8_t>(watch.owner > 0xFF ? 0xFF : watch.owner);
        entry.mission = watch.mission ? 1 : 0;
        replay::objcheck::LootLife(take, nowMs, entry.goneMs, entry.visible, entry.visibleFlipsMs);
        if (exists) // a hat taken (picked up) keeps its look
        {
            ReadPoseV2HatLook(object, entry.hatLook);
            if (!entry.hatLook.empty())
            {
                ++g_hatLookStats.scenery;
                LogPoseV2HatLook("loot", "index=" + std::to_string(Ctx().clip.scenery.size()), entry.model, entry.hatLook,
                    entry.position);
            }
        }
        const std::size_t index = Ctx().clip.scenery.size();
        Ctx().clip.scenery.push_back(entry);
        if (take == replay::objcheck::LootTake::Gone)
            ++g_sceneryStats.lootGone;
        else
            ++g_sceneryStats.lootHidden;
        std::string prop = "none";
        if (exists)
        {
            g_sceneryOf[object] = index;
            g_sceneryLive.push_back({object, index});
            if (IsPoseV2KnownObject(object) && MarkPoseV2SceneryTaken(object, KnownObjectId(object)))
                prop = replay::entity::IdText(Ctx().clip.scenery[index].prop);
        }
        if (g_lootLogs < kLootLogCap)
        {
            ++g_lootLogs;
            char line[320];
            std::snprintf(line, sizeof(line),
                "POSE_V2_LOOT,phase=record,why=%s,model=0x%x,pos=%.2f;%.2f;%.2f,appear_ms=%u,owner=%u,mission=%d,prop=%s,"
                "time_ms=%u\n",
                take == replay::objcheck::LootTake::Gone ? "gone" : "hidden", watch.model, watch.position[0],
                watch.position[1], watch.position[2], watch.appearMs, watch.owner, watch.mission ? 1 : 0, prop.c_str(), nowMs);
            Ctx().log << line;
        }
    }

    namespace
    {

    // object_check.h: loot and pickups present before the take, four times a
    // second with the scenery. A free object that is no scenery, not recorded
    // and within reach of a recorded ped (actors' and objects' positions from
    // memory; the natives only for those) is watched while drawn; hidden or
    // removed with a ped still within reach, it becomes an entry
    // (AddPoseV2Loot). A pickup is scenery itself (PoseV2SceneryCandidate) and
    // never watched here.
    void ScanPoseV2Loot(std::uint32_t nowMs)
    {
        REPLAY_PROFILE_ZONE("rec.loot");
        static std::vector<Vector3> actors;
        actors.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            float p[3]{};
            if (!entity.vehicle && replay::entities::Position(entity.handle, p))
                actors.push_back(Vector3{p[0], p[1], p[2]});
        }
        float playerXyz[3]{};
        const bool havePlayer = replay::entities::Position(PLAYER::PLAYER_PED_ID(), playerXyz);
        const Vector3 player{playerXyz[0], playerXyz[1], playerXyz[2]};
        const auto nearestActorM = [&](const Vector3& at) {
            float best = -1.0f;
            for (const Vector3& actor : actors)
            {
                const float d = Distance2(at, actor);
                if (best < 0.0f || d < best)
                    best = d;
            }
            return best < 0.0f ? -1.0f : std::sqrt(best);
        };
        const auto playerM = [&](const Vector3& at) { return havePlayer ? std::sqrt(Distance2(at, player)) : -1.0f; };
        for (auto it = g_lootWatch.begin(); it != g_lootWatch.end();)
        {
            const Object object = it->first;
            const auto snap = g_worldSnapshot.find(object);
            if (g_sceneryOf.count(object) || (snap != g_worldSnapshot.end() && snap->second.taken))
            {
                // Scenery or a world object's prop (moved, broke) took it.
                g_lootDecided.insert(object);
                it = g_lootWatch.erase(it);
                continue;
            }
            const bool exists = replay::entities::Exists(object) != FALSE;
            const LootWatch& watch = it->second;
            float nowXyz[3]{};
            if (exists && replay::entities::Position(object, nowXyz) && !replay::objcheck::LootInPlace(watch.position, nowXyz))
            {
                // Picked up or pushed: its removal later is not this spot's take.
                ++g_sceneryStats.lootMoved;
                g_lootDecided.insert(object);
                it = g_lootWatch.erase(it);
                continue;
            }
            const bool visible = exists && ENTITY::IS_ENTITY_VISIBLE(object) != FALSE;
            const Vector3 at{watch.position[0], watch.position[1], watch.position[2]};
            const float reach = nearestActorM(at);
            const replay::objcheck::LootTake take = replay::objcheck::LootTakeOf(exists, visible, reach, playerM(at));
            if (take == replay::objcheck::LootTake::None)
            {
                if (replay::objcheck::LootKeepWatching(reach))
                    ++it;
                else
                    it = g_lootWatch.erase(it); // looked at again when a ped reaches it
                continue;
            }
            if (take == replay::objcheck::LootTake::Dropped)
                ++g_sceneryStats.lootDropped;
            else if (take == replay::objcheck::LootTake::Gone)
            {
                // A removal waits for the same object made again
                // (objcheck::LootGoneStepOf).
                g_lootGonePending.push_back({object, watch, nowMs});
                ++g_lootReinstanceStats.pending;
            }
            else
                AddPoseV2Loot(object, watch, take, nowMs, exists);
            g_lootDecided.insert(object);
            it = g_lootWatch.erase(it);
        }
        // The removals waiting: an object of the same model at the spot is the
        // same object made again (its watch goes on, appearing when the first
        // did); none within kReinstanceMs is the take, at the time of the
        // removal.
        for (std::size_t n = g_lootGonePending.size(); n-- > 0;)
        {
            LootGonePending& pending = g_lootGonePending[n];
            Object again = 0;
            for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
            {
                const Object candidate = listed.object;
                if (listed.parent || candidate == pending.object || IsPoseV2KnownObject(candidate) ||
                    g_sceneryOf.count(candidate))
                    continue;
                float p[3]{};
                if (!replay::entities::Position(candidate, p) ||
                    !replay::objcheck::Near(pending.watch.position, p, replay::objcheck::kOriginalRadiusM))
                    continue;
                if (replay::objcheck::ReinstanceOf(pending.watch.model, pending.watch.position,
                        static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(candidate)), p))
                {
                    again = candidate;
                    break;
                }
            }
            const replay::objcheck::LootGoneStep step = replay::objcheck::LootGoneStepOf(again != 0, pending.goneMs, nowMs);
            if (step == replay::objcheck::LootGoneStep::Wait)
                continue;
            if (step == replay::objcheck::LootGoneStep::Reinstance)
            {
                ++g_lootReinstanceStats.reinstanced;
                g_lootWatch[again] = pending.watch; // the spot, the owner and the appearance of the first
                g_lootDecided.erase(again);
                if (g_lootLogs < kLootLogCap)
                {
                    ++g_lootLogs;
                    char line[256];
                    std::snprintf(line, sizeof(line),
                        "POSE_V2_LOOT,phase=record,why=reinstance,model=0x%x,pos=%.2f;%.2f;%.2f,old_handle=%d,new_handle=%d,"
                        "gone_ms=%u,time_ms=%u\n",
                        pending.watch.model, pending.watch.position[0], pending.watch.position[1], pending.watch.position[2],
                        pending.object, again, pending.goneMs, nowMs);
                    Ctx().log << line;
                }
            }
            else
            {
                ++g_lootReinstanceStats.gone;
                AddPoseV2Loot(pending.object, pending.watch, replay::objcheck::LootTake::Gone, pending.goneMs, false);
            }
            g_lootGonePending.erase(g_lootGonePending.begin() + static_cast<std::ptrdiff_t>(n));
        }
        if (actors.empty() || !havePlayer)
            return;
        if (g_lootFirstSeenMs.size() > kLootSeenCap)
            g_lootFirstSeenMs.clear();
        if (g_lootDecided.size() > kLootSeenCap)
            g_lootDecided.clear();
        bool fullCounted = false;
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
        {
            const Object object = listed.object;
            if (listed.parent || g_lootWatch.count(object) || g_lootDecided.count(object) || g_sceneryOf.count(object) ||
                g_sceneryPending.count(object))
                continue;
            const auto snap = g_worldSnapshot.find(object);
            const bool inSnapshot = snap != g_worldSnapshot.end();
            const std::uint32_t firstSeenMs = inSnapshot ? 0u : g_lootFirstSeenMs.emplace(object, nowMs).first->second;
            if (inSnapshot && snap->second.taken)
                continue;
            float atXyz[3]{};
            if (!replay::entities::Position(object, atXyz))
                continue;
            const Vector3 at{atXyz[0], atXyz[1], atXyz[2]};
            const float reach = nearestActorM(at);
            if (reach < 0.0f || reach > replay::objcheck::kLootReachM)
                continue;
            const float fromPlayer = playerM(at);
            if (fromPlayer > replay::objcheck::kLootPlayerRadiusM || IsPoseV2KnownObject(object))
                continue;
            if (g_lootWatch.size() >= replay::objcheck::kMaxLootWatch)
            {
                if (!fullCounted)
                    ++g_sceneryStats.lootWatchFull;
                fullCounted = true;
                continue;
            }
            std::uint32_t owner = replay::objcheck::kOwnerUnknown;
            bool mission = false;
            const bool scenery = PoseV2SceneryCandidate(object, owner, mission);
            const bool attached = ENTITY::IS_ENTITY_ATTACHED(object) != FALSE;
            const bool visible = ENTITY::IS_ENTITY_VISIBLE(object) != FALSE;
            if (!replay::objcheck::LootWatchable(scenery, attached, visible, reach, fromPlayer))
            {
                g_lootDecided.insert(object);
                continue;
            }
            LootWatch watch;
            watch.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            const Vector3 rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
            for (int axis = 0; axis < 3; ++axis)
                watch.position[axis] = atXyz[axis];
            watch.rotation[0] = rotation.x;
            watch.rotation[1] = rotation.y;
            watch.rotation[2] = rotation.z;
            watch.owner = owner;
            watch.mission = mission;
            const bool mapObject = replay::objcheck::IsMapDataOwner(owner) && !mission;
            watch.appearMs = replay::objcheck::LootAppearMs(inSnapshot, mapObject, firstSeenMs);
            g_lootWatch.emplace(object, watch);
            ++g_sceneryStats.lootWatched;
        }
    }

    // object_check.h: the children of the live scenery entries,
    // with the scenery's scan (four times a second): an entry leaves when its
    // object is gone, no longer attached to that parent or taken by another
    // path; an object attached to a live entry becomes one (there when the
    // parent appeared when seen in the first scan that sees the parent).
    void ScanPoseV2SceneryChildren(std::uint32_t nowMs)
    {
        for (std::size_t n = g_sceneryChildLive.size(); n-- > 0;)
        {
            const SceneryChildLive live = g_sceneryChildLive[n];
            if (live.index >= Ctx().clip.sceneryChildren.size())
                continue;
            replay::SceneryChild& entry = Ctx().clip.sceneryChildren[live.index];
            const bool exists = replay::entities::Exists(live.object);
            const bool attached = exists && replay::entities::Exists(live.parent) &&
                ENTITY::GET_ENTITY_ATTACHED_TO(live.object) == live.parent;
            const bool elsewhere = exists && IsPoseV2KnownObject(live.object);
            if (replay::objcheck::ChildLeft(exists, attached, elsewhere))
            {
                entry.goneMs = nowMs;
                ++g_sceneryChildStats.left;
                if (g_sceneryChildStats.logs++ < 32)
                    Ctx().log << "POSE_V2_SCENERY_CHILD,status=left,handle=" << live.object << ",model=0x" << std::hex
                              << entry.model << std::dec << ",why=" << (!exists ? "gone" : elsewhere ? "recorded" : "detached")
                              << ",time_ms=" << nowMs << '\n';
                g_sceneryChildObjects.erase(live.object);
                g_sceneryChildLive.erase(g_sceneryChildLive.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            if (replay::objcheck::NoteVisibility(entry.visible != 0, entry.visibleFlipsMs,
                    ENTITY::IS_ENTITY_VISIBLE(live.object) != FALSE, nowMs))
                ++g_sceneryChildStats.visibilityFlips;
        }
        if (g_sceneryLive.empty())
            return;
        // The entries scanned for children for the first time now.
        static std::vector<std::size_t> firstScan;
        firstScan.clear();
        for (const SceneryLive& live : g_sceneryLive)
            if (!g_sceneryChildScanned.count(live.index))
                firstScan.push_back(live.index);
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
        {
            if (!listed.parent)
                continue;
            const auto parent = g_sceneryOf.find(static_cast<Object>(listed.parent));
            if (parent == g_sceneryOf.end() || parent->second >= Ctx().clip.scenery.size())
                continue;
            // No children on map dressing (left to the map at playback, it
            // would hide a child's original and not draw the child).
            if (Ctx().clip.scenery[parent->second].rule == static_cast<std::uint8_t>(replay::objcheck::SceneryRule::MapDressing))
                continue;
            const Object object = listed.object;
            if (g_sceneryChildObjects.count(object) || IsPoseV2KnownObject(object) || !replay::entities::Exists(object))
                continue;
            if (replay::ropemodels::IsRopeModel(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object))))
                continue; // a rope's entity, the rope track's
            // A scenery entry of its own (it stood free, then was attached to
            // the parent): its entry leaves now and the child entry follows.
            if (const auto own = g_sceneryOf.find(object); own != g_sceneryOf.end())
            {
                if (own->second < Ctx().clip.scenery.size())
                    Ctx().clip.scenery[own->second].goneMs = nowMs;
                ++g_sceneryChildStats.fromScenery;
                g_sceneryLive.erase(std::remove_if(g_sceneryLive.begin(), g_sceneryLive.end(),
                                        [&](const SceneryLive& live) { return live.object == object; }),
                    g_sceneryLive.end());
                g_sceneryOf.erase(own);
                // Out of the world watch too. A snapshot entry left there was
                // recorded as world_moved once detached, its WorldObjectOrigin
                // stand drawn at the snapshot spot from 0 (a second copy next
                // to the child stand while attached); unseen again, a detached
                // one that stands still becomes a late entry where it rests, as
                // any detached child.
                g_worldSnapshot.erase(object);
                g_scenerySeen.erase(object);
            }
            const bool first = std::find(firstScan.begin(), firstScan.end(), parent->second) != firstScan.end();
            AddPoseV2SceneryChild(object, static_cast<Object>(listed.parent), parent->second,
                replay::objcheck::ChildAppearMs(first, Ctx().clip.scenery[parent->second].appearMs, nowMs), nowMs, first);
        }
        for (const std::size_t index : firstScan)
            g_sceneryChildScanned.insert(index);
    }

    // The map dressing removals waiting (objcheck::LootGoneStepOf): an object
    // of the entry's model at its spot is the same object made again (an
    // interior's entities rebuilt), watched on its new handle; none within
    // kReinstanceMs, the removal was the take, at its time.
    void ScanPoseV2DressingRemovals(std::uint32_t nowMs)
    {
        for (std::size_t n = g_dressingGonePending.size(); n-- > 0;)
        {
            const DressingGonePending pending = g_dressingGonePending[n];
            if (pending.index >= Ctx().clip.scenery.size())
            {
                g_dressingGonePending.erase(g_dressingGonePending.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            replay::SceneryObject& entry = Ctx().clip.scenery[pending.index];
            Object again = 0;
            for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
            {
                float p[3]{};
                if (listed.parent || IsPoseV2KnownObject(listed.object) || g_sceneryOf.count(listed.object) ||
                    !replay::entities::Position(listed.object, p) ||
                    !replay::objcheck::Near(entry.position, p, replay::objcheck::kOriginalRadiusM))
                    continue;
                if (replay::objcheck::ReinstanceOf(entry.model, entry.position,
                        static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(listed.object)), p))
                {
                    again = listed.object;
                    break;
                }
            }
            const replay::objcheck::LootGoneStep step = replay::objcheck::LootGoneStepOf(again != 0, pending.goneMs, nowMs);
            if (step == replay::objcheck::LootGoneStep::Wait)
                continue;
            if (step == replay::objcheck::LootGoneStep::Reinstance)
            {
                ++g_mapDressingStats.reinstanced;
                g_sceneryOf[again] = pending.index;
                g_sceneryLive.push_back({again, pending.index});
            }
            else
            {
                entry.goneMs = pending.goneMs;
                ++g_sceneryStats.gone;
                ++g_mapDressingStats.takenAway;
                if (g_lootLogs < kLootLogCap)
                {
                    ++g_lootLogs;
                    char line[224];
                    std::snprintf(line, sizeof(line),
                        "POSE_V2_LOOT,phase=record,why=dressing_taken,model=0x%x,pos=%.2f;%.2f;%.2f,owner=%u,prop=%s,time_ms=%u\n",
                        entry.model, entry.position[0], entry.position[1], entry.position[2], entry.owner,
                        entry.prop.Valid() ? replay::entity::IdText(entry.prop).c_str() : "none", pending.goneMs);
                    Ctx().log << line;
                }
            }
            g_dressingGonePending.erase(g_dressingGonePending.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    }

    void ScanPoseV2Scenery()
    {
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (nowMs < g_sceneryNextScanMs || g_worldSnapshot.empty())
            return;
        g_sceneryNextScanMs = nowMs + 250;
        REPLAY_PROFILE_ZONE("rec.scenery");
        ResolvePoseV2MapDressing(); // once the world state has its spheres
        for (std::size_t n = g_sceneryLive.size(); n-- > 0;)
        {
            const SceneryLive live = g_sceneryLive[n];
            if (live.index >= Ctx().clip.scenery.size())
                continue;
            replay::SceneryObject& entry = Ctx().clip.scenery[live.index];
            if (!replay::entities::Exists(live.object))
            {
                // objcheck::MapDressingGone: map dressing that streamed out
                // while its IPL stays on is no gone (the map brings it back);
                // with its IPL off it is. objcheck::DressingRemovalOf: one
                // removed within reach of a recorded ped, with the player
                // close, was taken (an item picked up), unless the same object
                // is made again at its spot.
                const bool dressing = entry.rule == static_cast<std::uint8_t>(replay::objcheck::SceneryRule::MapDressing);
                if (!dressing || replay::objcheck::MapDressingGone(!entry.ipl ||
                        STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(entry.ipl)) != FALSE))
                {
                    entry.goneMs = nowMs;
                    ++g_sceneryStats.gone;
                    g_mapDressingStats.leftWithIpl += dressing ? 1u : 0u;
                }
                else if (replay::objcheck::DressingRemovalOf(PoseV2NearestActorM(entry.position), PoseV2PlayerM(entry.position)))
                    g_dressingGonePending.push_back({live.index, nowMs});
                else
                    ++g_mapDressingStats.streamedOut;
                g_sceneryOf.erase(live.object);
                g_sceneryLive.erase(g_sceneryLive.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            if (!entry.prop.Valid() && IsPoseV2KnownObject(live.object))
                MarkPoseV2SceneryTaken(live.object, KnownObjectId(live.object));
            // The visibility flips over the recording.
            if (replay::objcheck::NoteVisibility(entry.visible != 0, entry.visibleFlipsMs,
                    ENTITY::IS_ENTITY_VISIBLE(live.object) != FALSE, nowMs))
                ++g_sceneryStats.visibilityFlips;
            // Its burn level while it stands
            // (a prop that took it over has its own track).
            if (!entry.prop.Valid())
            {
                SamplePoseV2SceneryBurnLevel(live.index, live.object, nowMs);
                // Its custom texture and tint (a
                // letter on a table, a poster).
                replay::objlook::SampleScenery(static_cast<std::uint32_t>(live.index), live.object, nowMs);
            }
        }
        ScanPoseV2DressingRemovals(nowMs);
        for (auto it = g_sceneryPending.begin(); it != g_sceneryPending.end();)
        {
            const Object object = it->first;
            SceneryPendingObject& pending = it->second;
            float at[3]{};
            // An object attached to a scenery entry while pending is that
            // entry's child (an attached one does not move, so it would also
            // have become an entry: drawn twice); unseen again, so once
            // detached it may still become one.
            if (g_sceneryChildObjects.count(object))
            {
                g_scenerySeen.erase(object);
                it = g_sceneryPending.erase(it);
                continue;
            }
            if (!replay::entities::Exists(object) || IsPoseV2KnownObject(object) || !replay::entities::Position(object, at))
            {
                it = g_sceneryPending.erase(it);
                continue;
            }
            if (!replay::objcheck::Near(at, pending.position, 0.15f))
            {
                ++g_sceneryStats.moving;
                it = g_sceneryPending.erase(it);
                continue;
            }
            if (nowMs - pending.firstMs < 1000)
            {
                ++it;
                continue;
            }
            // The spawned object watch made it
            // an entry meanwhile (no second stand-in).
            if (g_sceneryOf.count(object))
            {
                it = g_sceneryPending.erase(it);
                continue;
            }
            const std::size_t before = Ctx().clip.scenery.size();
            // From its first sight when it was seen out of
            // the radius and stood there since (objcheck::SceneryLateAppearMs).
            AddPoseV2Scenery(object, pending.position, pending.rotation, pending.appearMs, pending.owner, pending.mission,
                replay::objcheck::SceneryRuleOf(pending.mission, pending.owner, pending.pickup));
            ++g_sceneryStats.late;
            if (Ctx().clip.scenery.size() > before && pending.appearMs < pending.firstMs)
            {
                ++g_sceneryStats.farLate;
                LogPoseV2LateScenery("far", object, pending.owner, pending.mission, nowMs);
            }
            if (pending.pickup && Ctx().clip.scenery.size() > before)
            {
                ++g_sceneryStats.pickups;
                LogPoseV2PickupEntry("pickup_late");
            }
            JoinPoseV2WorldWatch(object, pending.position, pending.rotation);
            it = g_sceneryPending.erase(it);
        }
        ScanPoseV2Loot(nowMs);
        ScanPoseV2SceneryChildren(nowMs);
        float player[3]{};
        if (!replay::entities::Position(PLAYER::PLAYER_PED_ID(), player))
            return;
        if (g_sceneryFar.size() > replay::objcheck::kMaxSceneryFar)
        {
            g_sceneryFar.clear();
            ++g_sceneryStats.farResets;
        }
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
        {
            const Object object = listed.object;
            if (listed.parent || g_scenerySeen.count(object))
                continue;
            // The spawned object watch decides the objects
            // it watches (an entry once still, or a prop); the ones it leaves
            // (the map's) are looked at here when the watch ends.
            if (g_spawnWatch.count(object))
                continue;
            // An object out of the radius is not marked seen; it is looked at
            // again every second while listed (objcheck::kSceneryFarRecheckMs)
            // and decided once within it.
            const auto farIt = g_sceneryFar.find(object);
            if (!replay::objcheck::SceneryLookDue(farIt != g_sceneryFar.end(), nowMs,
                    farIt != g_sceneryFar.end() ? farIt->second.nextMs : 0u))
                continue;
            if (g_worldSnapshot.count(object) || IsPoseV2KnownObject(object))
            {
                g_scenerySeen.insert(object);
                if (farIt != g_sceneryFar.end())
                    g_sceneryFar.erase(farIt);
                continue;
            }
            float at[3]{};
            if (!replay::entities::Position(object, at))
                continue;
            if (!replay::objcheck::Near(at, player, replay::objcheck::kSceneryRadiusM))
            {
                if (farIt == g_sceneryFar.end())
                {
                    SceneryFar first;
                    first.firstMs = nowMs;
                    first.nextMs = nowMs + replay::objcheck::kSceneryFarRecheckMs;
                    for (int axis = 0; axis < 3; ++axis)
                        first.position[axis] = at[axis];
                    g_sceneryFar.emplace(object, first);
                }
                else
                {
                    farIt->second.nextMs = nowMs + replay::objcheck::kSceneryFarRecheckMs;
                    ++g_sceneryStats.farRechecks;
                }
                continue;
            }
            g_scenerySeen.insert(object);
            bool seenFar = false;
            std::uint32_t firstSeenMs = nowMs;
            bool movedSinceFirst = false;
            if (farIt != g_sceneryFar.end())
            {
                seenFar = true;
                firstSeenMs = farIt->second.firstMs;
                movedSinceFirst = !replay::objcheck::Near(at, farIt->second.position, 0.15f);
                g_sceneryFar.erase(farIt);
            }
            if (ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            std::uint32_t owner = replay::objcheck::kOwnerUnknown;
            bool mission = false;
            bool pickup = false;
            if (!PoseV2SceneryCandidate(object, owner, mission, &pickup))
                continue;
            SceneryPendingObject pending;
            pending.pickup = pickup;
            pending.firstMs = nowMs;
            pending.appearMs = replay::objcheck::SceneryLateAppearMs(seenFar, firstSeenMs, movedSinceFirst, nowMs);
            const Vector3 rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
            for (int axis = 0; axis < 3; ++axis)
                pending.position[axis] = at[axis];
            pending.rotation[0] = rotation.x;
            pending.rotation[1] = rotation.y;
            pending.rotation[2] = rotation.z;
            pending.owner = owner;
            pending.mission = mission;
            g_sceneryPending.emplace(object, pending);
        }
    }

    void UpdatePoseV2Props()
    {
        SamplePoseV2NearWorldObjects();
        REPLAY_PROFILE_ZONE("rec.props");
        for (replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
        {
            if (owner.vehicle)
                continue;
            const Object hat = PED::_GET_PED_LAST_DROPPED_HAT(owner.handle);
            if (!hat || hat == owner.lastHat || !replay::entities::Exists(hat))
                continue;
            owner.lastHat = hat;
            // A hat the spawned object watch took first (it fell
            // next to its ped) is recorded as the hat it is.
            RetirePoseV2PropOf(hat, "dropped_hat");
            RegisterPoseV2Prop(hat, owner.id, owner.handle,
                replay::entity::PropOrigin::DroppedHat, 0, "last_dropped_hat");
        }
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        std::uint32_t boneReads = 0; // this tick's bone watch reads
        for (std::size_t index = Ctx().recorder.props.size(); index-- > 0;)
        {
            replay::recorder::RecordedProp& prop = Ctx().recorder.props[index];
            if (!replay::entities::Exists(prop.object))
            {
                RetirePoseV2Prop(index, "entity_gone");
                continue;
            }
            SamplePoseV2PropBones(prop, timeMs, boneReads);
            SamplePoseV2ObjectPose(prop, timeMs);
            SamplePoseV2ObjectBurnLevel(prop, timeMs);
            replay::objlook::SampleObject(prop.id.Packed(), prop.object, timeMs, prop.lookChecked,
                prop.lookCheckMs);
            if (prop.sampled && timeMs - prop.lastSampleMs < kIntervalMs)
                continue;
            replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(prop.id.Packed());
            if (!track)
                continue;
            replay::entity::TransformSample sample;
            sample.timeMs = timeMs;
            sample.relative = ReadEntityTransform(prop.object);
            sample.visible = ENTITY::IS_ENTITY_VISIBLE(prop.object) != FALSE;
            if (track->Append(sample))
            {
                prop.lastSampleMs = timeMs;
                prop.sampled = true;
            }
        }
    }
    VehicleChildStats g_vehicleChildStats;

    namespace
    {

    // An object attached to a recorded vehicle or train car: recorded with
    // its parent the way the objects on a ped are. True when it is (or was
    // already) recorded.
    bool RecordPoseV2VehicleChild(Object object, const replay::recorder::RecordedEntity& parent, std::size_t vehicleChildren)
    {
        if (g_vehicleChildRefused.count(object))
            return false;
        float at[3]{};
        if (!replay::entities::Position(object, at))
            return false;
        float nearest = -1.0f;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            float p[3]{};
            if (entity.vehicle || !replay::entities::Position(entity.handle, p))
                continue;
            const float d = std::sqrt((p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) +
                (p[2] - at[2]) * (p[2] - at[2]));
            if (nearest < 0.0f || d < nearest)
                nearest = d;
        }
        const replay::objcheck::VehicleChild verdict = replay::objcheck::VehicleChildVerdict(
            nearest, vehicleChildren, Ctx().recorder.weapons.size(), replay::recorder::kWeaponCapacity);
        if (verdict != replay::objcheck::VehicleChild::Record)
        {
            g_vehicleChildRefused.insert(object);
            ++(verdict == replay::objcheck::VehicleChild::Far ? g_vehicleChildStats.refusedFar : g_vehicleChildStats.refusedFull);
            return false;
        }
        // A loose prop or a world object's prop put on the cart stops being one.
        RetirePoseV2PropOf(object, "attached_vehicle");
        if (!RegisterPoseV2Weapon(object, parent, replay::recorder::kAttachedObjectPoint, 0))
            return false;
        Ctx().recorder.weapons.back().seen = true;
        ++g_vehicleChildStats.recorded;
        const bool trainCar = std::none_of(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
            [&](const replay::recorder::RecordedEntity& entity) { return entity.handle == parent.handle; });
        g_vehicleChildStats.onTrainCars += trainCar ? 1u : 0u;
        const replay::recorder::RecordedWeapon& weapon = Ctx().recorder.weapons.back();
        g_vehicleChildStats.posed += weapon.posed || weapon.poseCandidate ? 1u : 0u;
        if (g_vehicleChildStats.logs < 48)
        {
            ++g_vehicleChildStats.logs;
            const replay::logfmt::FormatScope logFormat(Ctx().log);
            Ctx().log << std::fixed << std::setprecision(2) << "POSE_V2_VEHICLE_CHILD,id=" << replay::entity::IdText(weapon.id)
                      << ",parent=" << replay::entity::IdText(parent.id) << ",parent_kind=" << (trainCar ? "train_car" : "vehicle")
                      << ",handle=" << object << ",model=0x" << std::hex
                      << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) << std::dec
                      << ",posed=" << (weapon.posed ? 1 : 0) << ",pose_retry=" << (weapon.poseCandidate ? 1 : 0)
                      << ",bones=" << weapon.poseBones << ",nearest_ped_m=" << nearest
                      << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            Ctx().log.unsetf(std::ios_base::floatfield);
        }
        return true;
    }
    }

    void ScanPoseV2AttachedObjects()
    {
        REPLAY_PROFILE_ZONE("rec.attached_objects");
        static std::vector<Entity> owners;
        owners.clear();
        for (const replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
            if (!owner.vehicle && replay::entities::Exists(owner.handle))
                owners.push_back(owner.handle);
        std::sort(owners.begin(), owners.end());
        // The vehicles and train cars, and how
        // many of their children the registry holds now.
        CollectPoseV2VehicleParents();
        std::size_t vehicleChildren = 0;
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            vehicleChildren += PoseV2VehicleParent(weapon.parent) ? 1u : 0u;
        {
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            if (nowMs - g_vehicleChildRefusedResetMs >= 3000u || nowMs < g_vehicleChildRefusedResetMs)
            {
                g_vehicleChildRefused.clear(); // a ped may have come near, a place freed
                g_vehicleChildRefusedResetMs = nowMs;
            }
        }
        const int count = g_poseV2ScanListed; // the scan's single walk
        int attached = 0;
        int created = 0;
        int onWeapons = 0;
        int onVehicles = 0;
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
        {
            const Object object = listed.object;
            const Entity parent = listed.parent;
            if (!parent)
                continue;
            // On a recorded vehicle or train car.
            if (const replay::recorder::RecordedEntity* vehicle = PoseV2VehicleParent(parent))
            {
                const auto known = std::find_if(Ctx().recorder.weapons.begin(), Ctx().recorder.weapons.end(),
                    [&](const replay::recorder::RecordedWeapon& weapon) { return weapon.object == object; });
                if (known != Ctx().recorder.weapons.end())
                {
                    if (!known->seen && known->attachPoint == replay::recorder::kAttachedObjectPoint)
                    {
                        known->seen = true;
                        if (known->parent != parent)
                            ChangePoseV2WeaponOwner(*known, *vehicle, known->attachPoint, "attached_vehicle");
                        known->parent = parent;
                    }
                    continue;
                }
                if (RecordPoseV2VehicleChild(object, *vehicle, vehicleChildren))
                {
                    ++vehicleChildren;
                    ++onVehicles;
                }
                continue;
            }
            // An object on a recorded weapon (the arrow nocked on a bow): a
            // prop of the weapon's holder with its world track, so it follows
            // the drawn bow.
            if (!std::binary_search(owners.begin(), owners.end(), parent))
            {
                if (IsPoseV2KnownObject(object))
                    continue;
                const auto weapon = std::find_if(Ctx().recorder.weapons.begin(), Ctx().recorder.weapons.end(),
                    [&](const replay::recorder::RecordedWeapon& known) { return known.object == parent; });
                if (weapon == Ctx().recorder.weapons.end())
                    continue;
                const auto holder = std::find_if(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
                    [&](const replay::recorder::RecordedEntity& entity) { return entity.handle == weapon->parent; });
                if (holder == Ctx().recorder.entities.end())
                    continue;
                ++onWeapons;
                RegisterPoseV2Prop(object, holder->id, holder->handle, replay::entity::PropOrigin::Projectile, 0, "on_weapon");
                continue;
            }
            ++attached;
            const auto known = std::find_if(Ctx().recorder.weapons.begin(), Ctx().recorder.weapons.end(),
                [&](const replay::recorder::RecordedWeapon& weapon) { return weapon.object == object; });
            if (known != Ctx().recorder.weapons.end())
            {
                if (!known->seen && known->attachPoint == replay::recorder::kAttachedObjectPoint)
                {
                    known->seen = true;
                    if (known->parent != parent)
                        if (const replay::recorder::RecordedEntity* newOwner = Ctx().recorder.FindByHandle(parent))
                            ChangePoseV2WeaponOwner(*known, *newOwner, known->attachPoint, "attached_object");
                    known->parent = parent;
                }
                continue;
            }
            const auto owner = std::find_if(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
                [&](const replay::recorder::RecordedEntity& entity) { return entity.handle == parent; });
            if (owner == Ctx().recorder.entities.end())
                continue;
            // A loose prop picked up again, or a gore piece, stops being a prop.
            RetirePoseV2PropOf(object, "attached");
            // The saddle holster: its weapon, when the object has its model,
            // so the weapon system rebuilds it (straps, components).
            const Hash holster = WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(parent);
            const Hash hint = holster && WEAPON::_GET_WEAPONTYPE_MODEL(holster) == ENTITY::GET_ENTITY_MODEL(object) ? holster : 0;
            if (RegisterPoseV2Weapon(object, *owner, replay::recorder::kAttachedObjectPoint, hint))
            {
                Ctx().recorder.weapons.back().seen = true;
                ++created;
            }
        }
        // An attached object the listing does not return (the tomahawk's
        // successor stuck in a ped, found by model) stays while it is still
        // attached to a recorded ped, checked by its own handle.
        for (replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (weapon.seen || weapon.attachPoint != replay::recorder::kAttachedObjectPoint ||
                !replay::entities::Exists(weapon.object))
                continue;
            const Entity parent = ENTITY::GET_ENTITY_ATTACHED_TO(weapon.object);
            if (!parent || (!std::binary_search(owners.begin(), owners.end(), parent) && !PoseV2VehicleParent(parent)))
                continue;
            weapon.seen = true;
            if (weapon.parent != parent)
            {
                const replay::recorder::RecordedEntity* newOwner = Ctx().recorder.FindByHandle(parent);
                if (!newOwner)
                    newOwner = PoseV2VehicleParent(parent); // a train car
                if (newOwner)
                    ChangePoseV2WeaponOwner(weapon, *newOwner, weapon.attachPoint, "attached_object");
            }
            weapon.parent = parent;
        }
        // Item interaction props (the whisky bottle, the can of beans): not in
        // the ScriptHook object listing; the native gives the task's first
        // prop with prop id 0 (offline: the task's prop list).
        int itemProps = 0;
        for (const replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
        {
            if (owner.vehicle || !replay::entities::Exists(owner.handle) || !TASK::IS_PED_RUNNING_TASK_ITEM_INTERACTION(owner.handle))
                continue;
            const Entity prop = TASK::_GET_ITEM_INTERACTION_ENTITY_FROM_PED(owner.handle, 0);
            if (!prop || !replay::entities::Exists(prop) || !ENTITY::IS_ENTITY_AN_OBJECT(prop))
                continue;
            ++itemProps;
            const auto known = std::find_if(Ctx().recorder.weapons.begin(), Ctx().recorder.weapons.end(),
                [&](const replay::recorder::RecordedWeapon& weapon) { return weapon.object == prop; });
            if (known != Ctx().recorder.weapons.end())
            {
                if (known->attachPoint == replay::recorder::kAttachedObjectPoint)
                    known->seen = true;
                continue;
            }
            RetirePoseV2PropOf(prop, "attached");
            if (RegisterPoseV2Weapon(prop, owner, replay::recorder::kAttachedObjectPoint, 0))
            {
                Ctx().recorder.weapons.back().seen = true;
                ++created;
                Ctx().log << "POSE_V2_ITEM_PROP,ped=" << replay::entity::IdText(owner.id) << ",handle=" << prop << ",model=0x" << std::hex
                    << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(prop)) << std::dec << ",attached_to=" << ENTITY::GET_ENTITY_ATTACHED_TO(prop)
                    << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            }
        }
        if (created || onWeapons || onVehicles || (g_poseV2AttachedScans++ % 25) == 0)
            Ctx().log << "POSE_V2_ATTACHED_SCAN,objects=" << count << ",attached_to_recorded=" << attached
                << ",new=" << created << ",on_weapons=" << onWeapons << ",item_props=" << itemProps << ",owners=" << owners.size()
                << ",on_vehicles=" << onVehicles << ",vehicle_children=" << vehicleChildren
                << ",vehicle_parents=" << g_vehicleParents.size()
                << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
    }
}
