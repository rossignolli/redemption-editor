// replay_objects_recording.cpp: the recording: begin and end, its counters and every recording step that counts into them or calls a register with its default arguments (hat look, object pose, weapon and prop bones, weapon and prop register, drops, projectiles, scenery, the world snapshot and scans, spawned objects, loot, attached objects, hand ammo, world objects taken).
// Split from replay_objects.cpp (moved code); shared internals in replay_objects_internal.h.

#include "replay_objects_recording_internal.h"

namespace replay::objects
{
        HatLookStats g_hatLookStats;

        const char* ReadPoseV2HatLook(Object object, std::vector<replay::entity::MetaPedAsset>& out)
        {
            out.clear();
            if (g_hatLookCore < 0)
            {
                g_hatLookCore = replay::sig::Find(replay::hatlook::kTagCore) ? 1 : 0;
                g_hatLookWalk = replay::sig::Find(replay::hatlook::kTagWalk) ? 1 : 0;
                Ctx().log << "POSE_V2_HAT_LOOK_LAYOUT,core=" << g_hatLookCore << ",walk=" << g_hatLookWalk
                          << ",engine_writes=0\n";
            }
            if (g_hatLookCore != 1)
                return "unproven";
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(object));
            std::uint8_t type = 0;
            std::uint16_t count = 0;
            std::uint64_t tags = 0;
            if (!address || !replay::mem::Read(address + replay::hatlook::kEntityTypeOffset, type) ||
                type != replay::hatlook::kEntityTypeObject ||
                !replay::mem::Read(address + replay::hatlook::kTagCountOffset, count))
            {
                ++g_hatLookStats.unread;
                return "unread";
            }
            if (!count)
                return "no_tags";
            if (!replay::mem::Read(address + replay::hatlook::kTagArrayOffset, tags) || !tags)
            {
                ++g_hatLookStats.unread;
                return "unread";
            }
            const std::size_t read = replay::hatlook::TagsToRead(count, g_hatLookWalk == 1);
            for (std::size_t index = 0; index < read; ++index)
            {
                std::uint8_t bytes[replay::hatlook::kTagBytes]{};
                if (!replay::mem::Copy(bytes, static_cast<std::uintptr_t>(tags) + index * replay::hatlook::kTagStride, sizeof(bytes)))
                {
                    ++g_hatLookStats.unread;
                    out.clear();
                    return "unread";
                }
                const replay::entity::MetaPedAsset asset = replay::hatlook::DecodeTag(bytes);
                if (replay::hatlook::UsableTag(asset))
                    out.push_back(asset);
            }
            return out.empty() ? "no_tags" : "read";
        }

        // One evidence line per hat look read (capped per recording).
        void LogPoseV2HatLook(const char* path, const std::string& what, std::uint32_t model,
            const std::vector<replay::entity::MetaPedAsset>& look, const float* position)
        {
            if (look.empty() || g_hatLookStats.logs >= kHatLookLogCap)
                return;
            ++g_hatLookStats.logs;
            const replay::entity::MetaPedAsset& asset = look.front();
            char line[320];
            std::snprintf(line, sizeof(line),
                "POSE_V2_HAT_LOOK,phase=record,path=%s,what=%s,model=0x%x,tags=%zu,drawable=0x%x,albedo=0x%x,normal=0x%x,"
                "material=0x%x,palette=0x%x,tints=%d;%d;%d,pos=%.2f;%.2f;%.2f,time_ms=%u\n",
                path, what.c_str(), model, look.size(), asset.drawable, asset.albedo, asset.normal, asset.material,
                asset.palette, asset.tint0, asset.tint1, asset.tint2, position ? position[0] : 0.0f,
                position ? position[1] : 0.0f, position ? position[2] : 0.0f, replay::pose::capture::NowMs());
            Ctx().log << line;
        }
    ObjectPoseStats g_objectPoseStats;

    namespace
    {
    struct WeaponBoneStats
    {
        std::uint32_t watched = 0;  // weapons whose locals were watched
        std::uint32_t full = 0;     // left out: kMaxWeaponBoneWatches watched already
        std::uint32_t reads = 0;
        std::uint32_t deferred = 0;
        std::uint32_t moved = 0;    // verdicts that pose (moved, appeared, rebuilt)
        std::uint32_t posed = 0;
        std::uint32_t noPlace = 0;
        std::uint32_t refused = 0;  // rigid, start failed, no skeleton
        std::uint32_t ended = 0;    // watches ended when the weapon left the hand
        std::uint32_t peak = 0;
        std::uint32_t logs = 0;
        // The player's weapons apart.
        std::uint32_t playerWatched = 0;
        std::uint32_t playerPosed = 0;
        std::uint32_t playerNoPlace = 0;
        std::uint32_t playerLogs = 0;
        // Cost of the weapons' own bones on the recorder tick (the skeleton
        // checks, polls and watch reads of the weapons in a hand), measured
        // instead of capped.
        std::uint64_t tickUsSum = 0;
        std::uint32_t tickUsMax = 0;
        std::uint32_t ticks = 0;
        std::uint32_t posedMaxPerTick = 0;
    };
    WeaponBoneStats g_weaponBoneStats;
    struct AmmoStats
    {
        std::uint32_t ticks = 0;
        std::uint32_t fresh = 0;
        std::uint32_t asked = 0;
        std::uint32_t registered = 0;
        std::uint32_t expired = 0;
        std::uint32_t overflow = 0; // new objects left to the 200 ms scan (caps)
        std::uint32_t logs = 0;
    };
    AmmoStats g_ammoStats;
    }

    const char* TryStartPoseV2ObjectPose(Object object, replay::entity::ReplayId id,
        replay::skeleton::ResolvedPoseSkeleton& objectSkeleton, std::string& poseError, bool& posed,
        std::size_t* posedOut = nullptr)
    {
        posed = false;
        const std::size_t posedObjects = CountPosedObjects();
        // object_pose.h ShouldPoseShared: the objects and the weapons in a
        // hand share the slots the bodies leave.
        const std::size_t posedWeapons = CountWeaponBones(true);
        if (posedOut)
            *posedOut = posedObjects + posedWeapons;
        if (!replay::skeleton::FastResolvePedPoseSkeleton(object, objectSkeleton))
            return "skeleton_unresolved";
        const bool place = replay::object_pose::ShouldPoseShared(false, objectSkeleton.boneCount, posedObjects, posedWeapons,
            g_weaponBonePlaces);
        if (!place)
        {
            if (objectSkeleton.boneCount < replay::object_pose::kMinBones)
                return "rigid";
            ++g_objectPoseStats.noPlaceTries;
            return "no_place";
        }
        if (!replay::pose::capture::Start(id.Packed(), objectSkeleton.skeleton, objectSkeleton.boneCount,
                replay::recorder::kSampleHz, poseError))
            return "start_failed";
        posed = true;
        ++g_objectPoseStats.started;
        g_objectPoseStats.peak = (std::max)(g_objectPoseStats.peak, static_cast<std::uint32_t>(posedObjects + 1));
        return "started";
    }

    namespace
    {

    void LogPoseV2WeaponBones(const replay::recorder::RecordedWeapon& weapon, const char* status, std::uint32_t bones,
        std::uint32_t timeMs, const char* extra)
    {
        // 48 lines were spent on NPC no_place lines in 1156;
        // the player's weapon lines are always written (bounded apart).
        const bool player = IsPlayerWeapon(weapon);
        if (player ? g_weaponBoneStats.playerLogs >= 64 : g_weaponBoneStats.logs >= 48)
            return;
        ++(player ? g_weaponBoneStats.playerLogs : g_weaponBoneStats.logs);
        Ctx().log << "WEAPON_BONES,phase=rec,status=" << status << ",id=" << replay::entity::IdText(weapon.id)
                  << ",weapon=0x" << std::hex << weapon.weaponHash << ",model=0x"
                  << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(weapon.object)) << std::dec
                  << ",point=" << weapon.attachPoint << ",player=" << (IsPlayerWeapon(weapon) ? 1 : 0)
                  << ",bones=" << bones << extra << ",time_ms=" << timeMs << '\n';
    }

    bool StartPoseV2WeaponBones(replay::recorder::RecordedWeapon& weapon, std::uint32_t bones, std::uint32_t timeMs,
        const char* why)
    {
        ++g_weaponBoneStats.moved; // tries to start (field name kept for the log)
        replay::skeleton::ResolvedPoseSkeleton skeleton{};
        const char* status = "skeleton_unresolved";
        std::string error;
        if (replay::skeleton::FastResolvePedPoseSkeleton(weapon.object, skeleton))
        {
            // The weapons share their places with the posed objects
            // (object_pose.h ShouldPoseShared).
            const std::size_t taken = CountWeaponBones(true) + CountPosedObjects();
            if (!replay::object_pose::ShouldPoseWeapon(g_weaponBonePose, skeleton.boneCount, taken, g_weaponBonePlaces))
                status = skeleton.boneCount < replay::object_pose::kMinBones ? "rigid" : "no_place";
            else if (!replay::pose::capture::Start(weapon.id.Packed(), skeleton.skeleton, skeleton.boneCount,
                         replay::recorder::kSampleHz, error))
                status = "start_failed";
            else
                status = "started";
        }
        const bool player = IsPlayerWeapon(weapon);
        bool started = false;
        if (std::strcmp(status, "started") == 0)
        {
            started = true;
            ++g_weaponBoneStats.posed;
            g_weaponBoneStats.playerPosed += player ? 1u : 0u;
            weapon.posed = true;
            weapon.bonePose = true;
            weapon.poseSkeleton = skeleton.skeleton;
            weapon.poseBones = skeleton.boneCount;
            for (auto packet = Ctx().clip.created.rbegin(); packet != Ctx().clip.created.rend(); ++packet)
                if (packet->id == weapon.id)
                {
                    packet->boneCount = skeleton.boneCount;
                    break;
                }
            EndPoseV2WeaponBoneWatch(weapon);
            const std::uint32_t posedNow = static_cast<std::uint32_t>(CountWeaponBones(true));
            g_weaponBoneStats.peak = (std::max)(g_weaponBoneStats.peak, posedNow);
        }
        else if (std::strcmp(status, "no_place") == 0 || std::strcmp(status, "skeleton_unresolved") == 0)
        {
            // Watched again (a slot may come free, the skeleton may build).
            ++g_weaponBoneStats.noPlace;
            g_weaponBoneStats.playerNoPlace += player && std::strcmp(status, "no_place") == 0 ? 1u : 0u;
        }
        else
        {
            ++g_weaponBoneStats.refused;
            EndPoseV2WeaponBoneWatch(weapon);
        }
        char extra[176];
        std::snprintf(extra, sizeof(extra), ",why=%s,pose=%s,places=%u/%u,error=%s", why, status,
            static_cast<unsigned>(CountWeaponBones(true)), static_cast<unsigned>(g_weaponBonePlaces),
            error.empty() ? "none" : error.c_str());
        LogPoseV2WeaponBones(weapon, "pose", bones, timeMs, extra);
        return started;
    }

    void WatchPoseV2WeaponBones(replay::recorder::RecordedWeapon& weapon, std::uint32_t timeMs)
    {
        // BonePose, a weapon whose record already started
        // once (the record id is never started twice, as poseCandidate).
        if (!replay::object_pose::WatchWeaponBones(g_weaponBonePose, weapon.weaponHash, weapon.attachPoint, weapon.posed,
                weapon.boneWatch) || weapon.bonePose)
            return;
        const bool player = IsPlayerWeapon(weapon);
        std::uint32_t bones = 0;
        const bool read = ReadPoseV2SpawnLocals(weapon.object, bones, weapon.boneWatchLocals);
        ++g_weaponBoneStats.reads;
        std::vector<float>().swap(weapon.boneWatchLocals); // no baseline kept
        const replay::objcheck::SkeletonWatch state = replay::objcheck::PropBonesAtRegister(read, bones);
        weapon.boneWatch = true;
        weapon.boneWatchState = static_cast<std::uint8_t>(state);
        weapon.boneWatchBones = read ? bones : 0;
        weapon.nextBoneWatchMs = timeMs + replay::objcheck::kPropBoneIntervalMs;
        ++g_weaponBoneStats.watched;
        g_weaponBoneStats.playerWatched += player ? 1u : 0u;
        LogPoseV2WeaponBones(weapon, "watch", bones, timeMs,
            state == replay::objcheck::SkeletonWatch::Present  ? ",state=present"
            : state == replay::objcheck::SkeletonWatch::Absent ? ",state=absent"
                                                               : ",state=unread");
        if (state == replay::objcheck::SkeletonWatch::Present && bones >= replay::object_pose::kMinBones)
            StartPoseV2WeaponBones(weapon, bones, timeMs, "in_hand");
    }

    // Per recorder tick: a watched weapon (in a hand, skeleton not read yet
    // or no free slot at its last try) is tried again at the props' watch
    // rate; it starts at the first read with a skeleton.
    void SamplePoseV2WeaponBones(replay::recorder::RecordedWeapon& weapon, std::uint32_t timeMs, std::uint32_t& readsThisTick)
    {
        if (!weapon.boneWatch)
            return;
        if (weapon.posed || weapon.poseCandidate || !replay::object_pose::InHandPoint(weapon.attachPoint))
        {
            ++g_weaponBoneStats.ended;
            EndPoseV2WeaponBoneWatch(weapon); // posed by another path, or left the hand
            return;
        }
        if (timeMs < weapon.nextBoneWatchMs)
            return;
        if (!replay::objcheck::PropBonesDue(timeMs, weapon.nextBoneWatchMs, readsThisTick))
        {
            ++g_weaponBoneStats.deferred;
            return;
        }
        REPLAY_PROFILE_ZONE("rec.weapon_bones");
        ++readsThisTick;
        ++g_weaponBoneStats.reads;
        weapon.nextBoneWatchMs = timeMs + replay::objcheck::kPropBoneIntervalMs;
        static std::vector<float> fresh;
        std::uint32_t bones = 0;
        const bool read = ReadPoseV2SpawnLocals(weapon.object, bones, fresh);
        weapon.boneWatchState = static_cast<std::uint8_t>(read && bones > 0 ? replay::objcheck::SkeletonWatch::Present
                                                                             : replay::objcheck::SkeletonWatch::Absent);
        weapon.boneWatchBones = read ? bones : 0;
        if (read && bones >= replay::object_pose::kMinBones)
            StartPoseV2WeaponBones(weapon, bones, timeMs, "skeleton_read");
    }
    }

    bool FoldPoseV2AttachChildBone(Object child, replay::weapon::AttachFrame& frame, const std::string& who)
    {
        if (frame.childBone < 0)
            return true;
        static std::uint32_t logs = 0;
        replay::skeleton::ResolvedPoseSkeleton skeleton{};
        replay::pose::BoneQts bone{};
        const char* why = "folded";
        if (!replay::skeleton::FastResolvePedPoseSkeleton(child, skeleton) || !skeleton.poseBuffer)
            why = "skeleton_unresolved";
        else if (static_cast<std::uint32_t>(frame.childBone) >= skeleton.boneCount)
            why = "bone_out_of_range";
        // The pose buffer: boneCount locals, then boneCount object matrices
        // (pose_skeleton_fields.h).
        else if (!replay::mem::Copy(&bone,
                     skeleton.poseBuffer + (static_cast<std::size_t>(skeleton.boneCount) +
                                               static_cast<std::size_t>(frame.childBone)) *
                         sizeof(replay::pose::BoneQts),
                     sizeof(bone)))
            why = "read_failed";
        const bool folded = std::strcmp(why, "folded") == 0;
        replay::pose::WorldTransform offset{};
        replay::pose::WorldTransform childBone{};
        if (folded)
        {
            offset.position = {frame.position[0], frame.position[1], frame.position[2]};
            offset.rotation = {frame.rotation[0], frame.rotation[1], frame.rotation[2], frame.rotation[3]};
            childBone.position = {bone.translation[0], bone.translation[1], bone.translation[2]};
            childBone.rotation = bone.rotation;
            const replay::pose::WorldTransform result = replay::entity::FoldChildBone(offset, childBone);
            frame.position[0] = result.position.x;
            frame.position[1] = result.position.y;
            frame.position[2] = result.position.z;
            frame.rotation[0] = result.rotation.x;
            frame.rotation[1] = result.rotation.y;
            frame.rotation[2] = result.rotation.z;
            frame.rotation[3] = result.rotation.w;
        }
        if (logs < 64)
        {
            ++logs;
            std::ostream& log = Ctx().log;
            const replay::logfmt::FormatScope logFormat(log); // precision not left behind
            log << std::fixed << std::setprecision(4) << "POSE_V2_ATTACH_CHILD_BONE,id=" << who
                << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(child)) << std::dec
                << ",bone=" << frame.bone << ",child_bone=" << frame.childBone << ",bones=" << skeleton.boneCount
                << ",status=" << why << ",offset=" << offset.position.x << ' ' << offset.position.y << ' '
                << offset.position.z << ",child_bone_t=" << childBone.position.x << ' ' << childBone.position.y << ' '
                << childBone.position.z << ",child_bone_deg="
                << replay::pose::AngleBetween(childBone.rotation, replay::pose::Quat{}) * 57.29578f
                << ",folded=" << frame.position[0] << ' ' << frame.position[1] << ' ' << frame.position[2]
                << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            log.unsetf(std::ios_base::floatfield);
        }
        return folded;
    }

    bool RegisterPoseV2Weapon(Object object, const replay::recorder::RecordedEntity& owner, int attachPoint,
        Hash weaponHint = 0)
    {
        if (Ctx().recorder.weapons.size() >= replay::recorder::kWeaponCapacity)
            return false;
        const replay::entity::ReplayId id = Ctx().clip.ids.Allocate();
        if (!id.Valid())
            return false;
        const Hash model = ENTITY::GET_ENTITY_MODEL(object);
        Hash weapon = weaponHint;
        if (attachPoint >= 0)
            WEAPON::GET_CURRENT_PED_WEAPON(owner.handle, &weapon, FALSE, attachPoint, FALSE);
        // The weapon system rebuilds the object only when the weapon at this
        // point is the one whose model this object has; otherwise playback
        // creates a plain object of the model.
        Hash weaponModel = weapon ? WEAPON::_GET_WEAPONTYPE_MODEL(weapon) : 0;
        bool weaponMatches = weapon && weaponModel == model;
        // Not the weapon of this point: the weapon the object
        // itself belongs to (a script's weapon object in the hand).
        const char* weaponFrom = weaponMatches ? "slot" : "none";
        if (!weaponMatches)
        {
            bool hasWeapon = false;
            const Hash fromObject = ReadObjectWeapon(object, model, hasWeapon);
            if (fromObject)
            {
                weapon = fromObject;
                weaponModel = model;
                weaponMatches = true;
                weaponFrom = "object";
                ++g_objectWeaponFromObject;
            }
            else if (hasWeapon)
            {
                ++g_objectWeaponUnresolved;
                LogObjectCheck(replay::objcheck::Verdict::WeaponPlain, replay::entity::IdText(id), model,
                    replay::pose::capture::NowMs(), ",phase=record,why=weapon_unresolved");
            }
        }

        replay::entity::CreatePacket packet;
        packet.id = id;
        packet.timeMs = replay::pose::capture::NowMs();
        packet.kind = replay::entity::EntityKind::Weapon;
        packet.modelHash = model;
        packet.sourceHandle = object;
        packet.weaponHash = weaponMatches ? weapon : 0;
        // No native lists a weapon object's components: each known one is
        // tested once, when the object enters the recording.
        // weapon_look_core.h: the engine's own list (array
        // and hash table of the CWeapon) instead, the names kept for the
        // first kComponentsCrossChecks weapons of the session as its check
        // (and for every weapon once the engine read is off).
        const std::uint64_t componentsStartedAt = replay::prof::Now();
        const char* componentsFrom = "none";
        std::size_t componentsNamed = 0;
        if (weaponMatches)
        {
            std::vector<std::uint32_t> engineComponents;
            const bool engineRead = g_weaponComponentsEngine &&
                replay::weapon::ReadComponentList(
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object)), engineComponents);
            constexpr std::uint32_t kComponentsCrossChecks = 8;
            std::uint32_t& crossChecks = g_weaponComponentsCrossChecks;
            if (!engineRead || crossChecks < kComponentsCrossChecks)
            {
                for (const replay::weapon::Component& component : replay::weapon::kComponents)
                {
                    if (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(object, component.hash))
                        packet.weaponComponents.push_back(component.hash);
                }
                componentsNamed = packet.weaponComponents.size();
                componentsFrom = "names";
            }
            if (engineRead && crossChecks < kComponentsCrossChecks)
            {
                ++crossChecks;
                g_weaponComponentsChecked = true;
                std::string why;
                const bool agree = PoseV2ComponentsAgree(object, engineComponents, packet.weaponComponents, why);
                Ctx().log << "WEAPON_LOOK_ENGINE,read=components,id=" << replay::entity::IdText(id) << ",agree=" << (agree ? 1 : 0)
                          << ",engine=" << engineComponents.size() << ",named=" << packet.weaponComponents.size()
                          << ",check=" << crossChecks << ",why=" << why << '\n';
                if (!agree)
                {
                    g_weaponComponentsEngine = false;
                    Ctx().log << "WEAPON_LOOK_ENGINE_OFF,read=components,reason=disagree_with_natives\n";
                }
            }
            if (engineRead && g_weaponComponentsEngine)
            {
                packet.weaponComponents = engineComponents;
                componentsFrom = "engine";
            }
        }
        const double componentsUs = static_cast<double>(
            ElapsedUs(componentsStartedAt, replay::prof::Now()));
        // object_pose.h: an attached object with a skeleton (the
        // carried pelt, which the skinning and carry animations roll up by
        // its bones) also gets a pose record under its own id, like the
        // reference's animated object bones; the create carries its bone
        // count, so playback arms the override on the replay object.
        replay::skeleton::ResolvedPoseSkeleton objectSkeleton{};
        bool posed = false;
        const char* poseStatus = "weapon_object";
        std::string poseError;
        if (!weaponMatches)
        {
            poseStatus = TryStartPoseV2ObjectPose(object, id, objectSkeleton, poseError, posed);
            if (posed)
                packet.boneCount = objectSkeleton.boneCount;
        }
        // No skeleton yet (or no place): resolved again while it
        // stays recorded (RetryPoseV2ObjectPose).
        const bool poseCandidate = !weaponMatches && !posed && PoseV2ObjectPoseRetryable(poseStatus);
        if (!weaponMatches && g_poseV2ObjectPoseLogs < 48)
        {
            ++g_poseV2ObjectPoseLogs;
            Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(id)
                      << ",owner=" << replay::entity::IdText(owner.id) << ",model=0x" << std::hex << model << std::dec
                      << ",status=" << poseStatus << ",bones=" << objectSkeleton.boneCount
                      << ",fragment_path=" << (objectSkeleton.usedFragmentPath ? 1 : 0)
                      << ",getter=" << replay::skeleton::walk::GetterKindName(
                             static_cast<replay::skeleton::walk::GetterKind>(objectSkeleton.getterKind))
                      << ",retry=" << (poseCandidate ? 1 : 0)
                      << ',' << ObjectClassText(replay::entities::AddressOf(object))
                      << ",error=" << (poseError.empty() ? "none" : poseError.c_str())
                      << ",walk=" << (std::strcmp(poseStatus, "skeleton_unresolved") == 0
                                         ? replay::skeleton::DescribePoseSkeletonWalk(object)
                                         : std::string("-"))
                      << ",time_ms=" << packet.timeMs << '\n';
        }
        Ctx().clip.lifecycle.Create(packet);
        // The parent goes with the create:
        // same time, so the weapon is never alive without its ped.
        Ctx().clip.lifecycle.Attach(
            {id, owner.id, replay::entity::AttachmentKind::WeaponPoint, attachPoint}, packet.timeMs);
        Ctx().clip.created.push_back(packet);
        Ctx().recorder.weapons.push_back({object, owner.handle, id, attachPoint, true});
        Ctx().recorder.weapons.back().weaponHash = packet.weaponHash;
        WatchPoseV2WeaponBones(Ctx().recorder.weapons.back(), packet.timeMs);
        if (posed)
        {
            Ctx().recorder.weapons.back().posed = true;
            Ctx().recorder.weapons.back().poseSkeleton = objectSkeleton.skeleton;
            Ctx().recorder.weapons.back().poseBones = objectSkeleton.boneCount;
        }
        if (poseCandidate)
        {
            Ctx().recorder.weapons.back().poseCandidate = true;
            Ctx().recorder.weapons.back().nextPoseTryMs = packet.timeMs + replay::object_pose::kRetryIntervalMs;
            // object_pose.h ReservedPlaces
            Ctx().recorder.weapons.back().poseNoPlace = std::strcmp(poseStatus, "no_place") == 0;
        }
        Ctx().clip.weaponTracks.push_back({id.Packed(), replay::entity::RelativeTrack(replay::recorder::kSampleHz)});

        std::ostream& log = Ctx().log;
        log << "POSE_V2_WEAPON_CREATE"
            << ",id=" << replay::entity::IdText(id)
            << ",parent=" << replay::entity::IdText(owner.id)
            << ",attach_point=" << attachPoint
            << ",handle=" << object
            << ",model=0x" << std::hex << model
            << ",weapon=0x" << weapon
            << ",weapon_model=0x" << weaponModel << std::dec
            << ",weapon_object=" << (weaponMatches ? 1 : 0)
            << ",weapon_from=" << weaponFrom
            << ",name=" << (weapon ? PoseV2WeaponName(weapon) : "none")
            << ",components=" << packet.weaponComponents.size()
            << ",components_from=" << componentsFrom
            << ",components_named=" << componentsNamed
            << ",components_us=" << static_cast<std::uint64_t>(componentsUs)
            << ",straps=" << [&] {
                   std::string names;
                   for (const std::uint32_t hash : packet.weaponComponents)
                   {
                       for (const replay::weapon::Component& component : replay::weapon::kComponents)
                       {
                           if (component.hash == hash && std::strstr(component.name, "STRAP"))
                               names += std::string(component.name) + " ";
                       }
                   }
                   return names.empty() ? std::string("none") : names;
               }()
            << ",visible=" << (ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0)
            << ",time_ms=" << packet.timeMs
            << '\n';
        log.flush();
        return true;
    }

    namespace
    {
    struct ObjectBurnStats
    {
        std::uint32_t reads = 0;      // objects read with a block
        std::uint32_t noBlock = 0;    // reads without one (unreadable, no handler yet, a class with no block)
        std::uint32_t tracks = 0;     // weapon object and prop tracks begun
        std::uint32_t changes = 0;    // states stored after the first
        std::uint32_t sceneryTracks = 0;
        std::uint32_t sceneryChanges = 0;
    };
    ObjectBurnStats g_objectBurnStats;
    }

    // One track of `tracks` (id `key`) takes the read; logs the first read
    // (record_entry, 32) and the changes (record, 64).
    void NotePoseV2ObjectBurn(std::vector<replay::BurnLevelTrack>& tracks, replay::TrackIndex<replay::BurnLevelTrack>& index,
        std::uint32_t key, Object object, bool scenery, std::uint32_t timeMs)
    {
        replay::burn::State state;
        if (!replay::burn::Read(replay::entities::AddressOf(object), state) || !state.fireBlock)
        {
            ++g_objectBurnStats.noBlock;
            return;
        }
        ++g_objectBurnStats.reads;
        replay::BurnLevelTrack* track = index.Find(tracks, key);
        const bool first = !track;
        if (first)
        {
            tracks.push_back({key, {}, 0, false});
            track = &tracks.back();
            ++(scenery ? g_objectBurnStats.sceneryTracks : g_objectBurnStats.tracks);
        }
        if (!replay::NoteBurnLevel(*track, timeMs, state.level))
            return;
        if (!first)
            ++(scenery ? g_objectBurnStats.sceneryChanges : g_objectBurnStats.changes);
        std::uint32_t& logs = first ? g_objectBurnEntryLogs : g_objectBurnLogs;
        if (logs < (first ? 32u : 64u))
        {
            ++logs;
            LogPoseV2ObjectBurn(first ? (scenery ? "record_entry_scenery" : "record_entry") : (scenery ? "record_scenery" : "record"),
                key, scenery, state, object, timeMs);
        }
    }

    void SamplePoseV2Weapons()
    {
        REPLAY_PROFILE_ZONE("rec.weapons");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        std::uint32_t weaponBoneReads = 0; // this tick's weapon locals reads
        std::uint64_t weaponBoneQpc = 0;    // this tick's weapon bone work
        std::uint32_t weaponBonePosed = 0;
        for (std::size_t index = Ctx().recorder.weapons.size(); index-- > 0;)
        {
            replay::recorder::RecordedWeapon& weapon = Ctx().recorder.weapons[index];
            if (!replay::entities::Exists(weapon.object) || !replay::entities::Exists(weapon.parent))
            {
                if (replay::entities::Exists(weapon.parent))
                    ContinuePoseV2WeaponAsProp(weapon, "object_gone");
                RetirePoseV2Weapon(index, "entity_gone");
                continue;
            }
            const bool weaponBones = weapon.bonePose || weapon.boneWatch; // timed
            LARGE_INTEGER boneStart{};
            if (weaponBones)
                QueryPerformanceCounter(&boneStart);
            SamplePoseV2ObjectPose(weapon, timeMs);
            SamplePoseV2WeaponBones(weapon, timeMs, weaponBoneReads);
            if (weaponBones)
            {
                LARGE_INTEGER boneEnd{};
                QueryPerformanceCounter(&boneEnd);
                weaponBoneQpc += static_cast<std::uint64_t>(boneEnd.QuadPart - boneStart.QuadPart);
                weaponBonePosed += weapon.bonePose && weapon.posed ? 1u : 0u;
            }
            SamplePoseV2ObjectBurnLevel(weapon, timeMs);
            replay::objlook::SampleObject(weapon.id.Packed(), weapon.object, timeMs, weapon.lookChecked,
                weapon.lookCheckMs); // the held note's text
            if (weapon.sampled && timeMs - weapon.lastSampleMs < kIntervalMs)
                continue;
            replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(weapon.id.Packed());
            if (!track)
                continue;
            replay::entity::TransformSample sample;
            sample.timeMs = timeMs;
            const replay::pose::WorldTransform world = ReadEntityTransform(weapon.object);
            sample.relative = replay::entity::RelativeTransform(
                ReadEntityTransform(weapon.parent), world);
            sample.visible = ENTITY::IS_ENTITY_VISIBLE(weapon.object) != FALSE;
            SamplePoseV2WeaponCondition(weapon, timeMs);
            replay::weapon::StrapState strap;
            if (replay::weapon::ReadStrapState(
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), strap) &&
                strap.hasStraps)
            {
                auto strapTrack = std::find_if(Ctx().clip.strapTracks.begin(), Ctx().clip.strapTracks.end(),
                    [&](const replay::StrapTrack& entry) { return entry.recordId == weapon.id.Packed(); });
                if (strapTrack == Ctx().clip.strapTracks.end())
                {
                    Ctx().clip.strapTracks.push_back({weapon.id.Packed(), {}});
                    strapTrack = Ctx().clip.strapTracks.end() - 1;
                }
                if (strapTrack->states.empty() || !SameStrapState(strapTrack->states.back().second, strap))
                {
                    strapTrack->states.push_back({timeMs, strap});
                    if (g_poseV2WeaponEvidenceLines < 200)
                    {
                        ++g_poseV2WeaponEvidenceLines;
                        std::ostream& log = Ctx().log;
                        log << "POSE_V2_STRAP_STATE,id=" << replay::entity::IdText(weapon.id)
                            << ",flags=0x" << std::hex << strap.flags << std::dec
                            << ",weight_a=" << strap.weightA
                            << ",weight_b=" << strap.weightB
                            << ",mask=";
                        for (const auto& bit : strap.maskBits)
                            log << std::hex << bit.first << std::dec << ':' << (bit.second ? 1 : 0) << ' ';
                        log << ",time_ms=" << timeMs << '\n';
                        log.flush();
                    }
                }
            }
            replay::weapon::AttachFrame frame;
            if (ENTITY::IS_ENTITY_ATTACHED(weapon.object) &&
                replay::weapon::ReadAttachFrame(
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), frame) &&
                FoldPoseV2AttachChildBone(weapon.object, frame, replay::entity::IdText(weapon.id)))
            {
                auto frameTrack = std::find_if(Ctx().clip.attachFrames.begin(), Ctx().clip.attachFrames.end(),
                    [&](const replay::AttachFrameTrack& entry) { return entry.recordId == weapon.id.Packed(); });
                if (frameTrack == Ctx().clip.attachFrames.end())
                {
                    Ctx().clip.attachFrames.push_back({weapon.id.Packed(), {}});
                    frameTrack = Ctx().clip.attachFrames.end() - 1;
                }
                if (frameTrack->frames.empty() || !SameAttachFrame(frameTrack->frames.back().second, frame))
                {
                    frameTrack->frames.push_back({timeMs, frame});
                    if (g_poseV2WeaponEvidenceLines < 200)
                    {
                        ++g_poseV2WeaponEvidenceLines;
                        std::ostream& log = Ctx().log;
                        const replay::logfmt::FormatScope logFormat(log); // precision not left behind
                        log << std::fixed << std::setprecision(4)
                            << "POSE_V2_ATTACH_FRAME,id=" << replay::entity::IdText(weapon.id)
                            << ",bone=" << frame.bone << ",child_bone=" << frame.childBone
                            << ",pos=" << frame.position[0] << ' ' << frame.position[1] << ' ' << frame.position[2]
                            << ",quat=" << frame.rotation[0] << ' ' << frame.rotation[1] << ' '
                            << frame.rotation[2] << ' ' << frame.rotation[3]
                            << ",count=" << frameTrack->frames.size()
                            << ",time_ms=" << timeMs << '\n';
                        log.unsetf(std::ios_base::floatfield);
                        log.flush();
                    }
                }
            }
            if (track->Append(sample))
            {
                weapon.lastWorld = world;
                weapon.lastSampleMs = timeMs;
                weapon.sampled = true;
            }
        }
        // The weapons whose bones are recorded, to the weapon pass detour
        // (sampled right after the pass that turns the cylinder).
        if (g_weaponPassReady)
        {
            static std::vector<replay::weapon::WeaponRecordEntry> entries;
            entries.clear();
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            {
                if (!weapon.bonePose || !weapon.posed)
                    continue;
                const auto object = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object));
                if (!object)
                    continue;
                entries.push_back({object, weapon.id.Packed()});
                g_passRecWeapons.emplace(weapon.id.Packed(), weapon.weaponHash);
            }
            const bool changed = entries.size() != g_passRecPublished.size() ||
                !std::equal(entries.begin(), entries.end(), g_passRecPublished.begin(),
                    [](const replay::weapon::WeaponRecordEntry& a, const replay::weapon::WeaponRecordEntry& b) {
                        return a.object == b.object && a.recordId == b.recordId;
                    });
            if (changed)
            {
                g_passRecPublished = entries;
                replay::weapon::SetWeaponRecordIds(g_passRecPublished);
            }
        }
        if (weaponBoneQpc || weaponBonePosed) // WEAPON_BONES_TOTAL tick_us_*
        {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const std::uint64_t us = frequency.QuadPart ? weaponBoneQpc * 1000000ull / static_cast<std::uint64_t>(frequency.QuadPart) : 0;
            g_weaponBoneStats.tickUsSum += us;
            g_weaponBoneStats.tickUsMax = (std::max)(g_weaponBoneStats.tickUsMax, static_cast<std::uint32_t>(us));
            ++g_weaponBoneStats.ticks;
            g_weaponBoneStats.posedMaxPerTick = (std::max)(g_weaponBoneStats.posedMaxPerTick, weaponBonePosed);
        }
    }

    namespace
    {

    // object_check.h AfterPropBoneRead: the bones of a prop registered without
    // a pose, watched until they move (the deputy's chair of 043-Valentine,
    // recorded rigid because its root moved first). The recording's counters
    // (POSE_V2_PROP_BONE_WATCH at the end) and a capped line per watch and per
    // verdict (POSE_V2_PROP_BONES).
    struct PropBoneStats
    {
        std::uint32_t watched = 0;   // props that entered the watch
        std::uint32_t full = 0;      // left out: kMaxPropBoneWatches watched already
        std::uint32_t evicted = 0;   // absent watches that left for a prop with a skeleton
        std::uint32_t peak = 0;      // most watched at once
        std::uint32_t reads = 0;     // skeleton reads (resolve and locals copy)
        std::uint32_t deferred = 0;  // reads due but left to a later tick (kPropBoneReadsPerTick)
        std::uint32_t baselines = 0; // baselines taken after the register
        std::uint32_t appeared = 0;  // verdicts: a skeleton came
        std::uint32_t rebuilt = 0;   //   rebuilt with another bone count
        std::uint32_t moved = 0;     //   bones moved
        std::uint32_t posed = 0;     // late pose started at once
        std::uint32_t waiting = 0;   // left to the retry (no place or no skeleton at the start)
        std::uint32_t refused = 0;   // the start refused for good (rigid, start failed)
        std::uint32_t logs = 0;
    };
    PropBoneStats g_propBoneStats;

    void LogPoseV2PropBones(const replay::recorder::RecordedProp& prop, const char* status, std::uint32_t bones,
        std::uint32_t timeMs, const char* extra)
    {
        if (g_propBoneStats.logs >= kPropBoneLogCap)
            return;
        ++g_propBoneStats.logs;
        Ctx().log << "POSE_V2_PROP_BONES,status=" << status << ",id=" << replay::entity::IdText(prop.id)
                  << ",handle=" << prop.object << ",model=0x" << std::hex
                  << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(prop.object)) << std::dec << ",bones=" << bones
                  << extra << ",time_ms=" << timeMs << '\n';
    }

    // At the register of a prop without a pose: its baseline (the spawned
    // watch's locals read, rung 2 of the resolver).
    void WatchPoseV2PropBones(replay::recorder::RecordedProp& prop, replay::entity::PropOrigin origin, Hash weapon,
        std::uint32_t timeMs)
    {
        const bool poseable = replay::object_pose::PoseableProp(origin == replay::entity::PropOrigin::DroppedWeapon,
            origin == replay::entity::PropOrigin::Projectile || origin == replay::entity::PropOrigin::WorldObject, weapon);
        if (!poseable || prop.posed || prop.poseCandidate)
            return;
        const std::size_t watching = CountPropBoneWatches();
        std::uint32_t bones = 0;
        const bool read = ReadPoseV2SpawnLocals(prop.object, bones, prop.boneWatchLocals);
        ++g_propBoneStats.reads;
        if (!read)
            prop.boneWatchLocals.clear();
        const replay::objcheck::SkeletonWatch state = replay::objcheck::PropBonesAtRegister(read, bones);
        // The watch full, a prop with a skeleton takes the place of
        // the oldest watched one without (object_check.h PropBoneWatchRoom).
        replay::recorder::RecordedProp* oldestAbsent = nullptr;
        for (replay::recorder::RecordedProp& other : Ctx().recorder.props)
            if (&other != &prop && other.boneWatch &&
                other.boneWatchState == static_cast<std::uint8_t>(replay::objcheck::SkeletonWatch::Absent))
            {
                oldestAbsent = &other; // props are appended: the first is the oldest
                break;
            }
        const replay::objcheck::PropBoneRoom room = replay::objcheck::PropBoneWatchRoom(
            watching, state == replay::objcheck::SkeletonWatch::Absent, oldestAbsent != nullptr);
        if (room == replay::objcheck::PropBoneRoom::Full ||
            !replay::objcheck::WatchPropBones(poseable, prop.posed, prop.poseCandidate,
                room == replay::objcheck::PropBoneRoom::Evict ? watching - 1 : watching))
        {
            ++g_propBoneStats.full;
            std::vector<float>().swap(prop.boneWatchLocals);
            LogPoseV2PropBones(prop, "full", bones, timeMs, "");
            return;
        }
        if (room == replay::objcheck::PropBoneRoom::Evict)
        {
            ++g_propBoneStats.evicted;
            LogPoseV2PropBones(*oldestAbsent, "evicted", 0, timeMs, "");
            EndPoseV2PropBoneWatch(*oldestAbsent);
        }
        prop.boneWatch = true;
        prop.boneWatchState = static_cast<std::uint8_t>(state);
        prop.boneWatchBones = read ? bones : 0;
        prop.nextBoneWatchMs = timeMs + replay::objcheck::kPropBoneIntervalMs;
        ++g_propBoneStats.watched;
        const std::size_t watchingNow = room == replay::objcheck::PropBoneRoom::Evict ? watching : watching + 1;
        g_propBoneStats.peak = (std::max)(g_propBoneStats.peak, static_cast<std::uint32_t>(watchingNow));
        LogPoseV2PropBones(prop, "watch", bones, timeMs,
            state == replay::objcheck::SkeletonWatch::Present  ? ",state=present"
            : state == replay::objcheck::SkeletonWatch::Absent ? ",state=absent"
                                                               : ",state=unread");
    }
    }

    // Per recorder tick, per watched prop when due: the locals read and
    // compared; a verdict that poses hands the prop to the retry path at once
    // (TryStartPoseV2ObjectPose under the pose places, the create patched
    // with the bone count, POSE_V2_OBJECT_POSE late=1), which keeps trying
    // every 250 ms while there is no place.
    void SamplePoseV2PropBones(replay::recorder::RecordedProp& prop, std::uint32_t timeMs, std::uint32_t& readsThisTick)
    {
        if (!prop.boneWatch)
            return;
        if (prop.posed || prop.poseCandidate)
        {
            EndPoseV2PropBoneWatch(prop); // another path posed it (or waits for a place)
            return;
        }
        if (timeMs < prop.nextBoneWatchMs)
            return;
        if (!replay::objcheck::PropBonesDue(timeMs, prop.nextBoneWatchMs, readsThisTick))
        {
            ++g_propBoneStats.deferred; // due, over this tick's reads
            return;
        }
        REPLAY_PROFILE_ZONE("rec.prop_bones");
        ++readsThisTick;
        ++g_propBoneStats.reads;
        prop.nextBoneWatchMs = timeMs + replay::objcheck::kPropBoneIntervalMs;
        static std::vector<float> fresh;
        std::uint32_t bones = 0;
        const bool read = ReadPoseV2SpawnLocals(prop.object, bones, fresh);
        const auto state = static_cast<replay::objcheck::SkeletonWatch>(prop.boneWatchState);
        const bool comparable = read && state == replay::objcheck::SkeletonWatch::Present && bones == prop.boneWatchBones &&
            prop.boneWatchLocals.size() == fresh.size();
        const bool changed = comparable &&
            replay::object_pose::spawn::LocalsChanged(prop.boneWatchLocals.data(), fresh.data(), bones);
        const replay::objcheck::PropBoneStep step =
            replay::objcheck::AfterPropBoneRead(state, read, bones, prop.boneWatchBones, changed);
        if (!replay::objcheck::PropBonesPose(step))
        {
            if (step == replay::objcheck::PropBoneStep::Baseline)
            {
                ++g_propBoneStats.baselines;
                prop.boneWatchLocals = fresh;
                prop.boneWatchBones = bones;
                LogPoseV2PropBones(prop, "baseline", bones, timeMs, "");
            }
            const replay::objcheck::SkeletonWatch next = replay::objcheck::NextPropBoneWatch(state, read, bones);
            if (next == replay::objcheck::SkeletonWatch::Absent)
            {
                prop.boneWatchLocals.clear();
                prop.boneWatchBones = 0;
            }
            prop.boneWatchState = static_cast<std::uint8_t>(next);
            return;
        }
        g_propBoneStats.appeared += step == replay::objcheck::PropBoneStep::Appeared ? 1u : 0u;
        g_propBoneStats.rebuilt += step == replay::objcheck::PropBoneStep::Rebuilt ? 1u : 0u;
        g_propBoneStats.moved += step == replay::objcheck::PropBoneStep::Moved ? 1u : 0u;
        EndPoseV2PropBoneWatch(prop);
        prop.poseCandidate = true;
        prop.poseNoPlace = false;
        prop.nextPoseTryMs = timeMs;
        RetryPoseV2ObjectPose(prop, timeMs);
        const char* outcome = prop.posed ? "posed" : prop.poseCandidate ? "waiting" : "refused";
        g_propBoneStats.posed += prop.posed ? 1u : 0u;
        g_propBoneStats.waiting += !prop.posed && prop.poseCandidate ? 1u : 0u;
        g_propBoneStats.refused += !prop.posed && !prop.poseCandidate ? 1u : 0u;
        char extra[96];
        std::snprintf(extra, sizeof(extra), ",why=%s,pose=%s,no_place=%d", replay::objcheck::PropBoneStepText(step), outcome,
            prop.poseNoPlace ? 1 : 0);
        LogPoseV2PropBones(prop, "pose", bones, timeMs, extra);
    }
    void RegisterPoseV2Prop(Object object, replay::entity::ReplayId ownerId, Entity ownerHandle,
        replay::entity::PropOrigin origin, Hash weaponHash, const char* how,
        const std::vector<std::uint32_t>& weaponComponents, std::uint32_t pieceGroup,
        const PoseContinuation* continuation, bool spawnedPose)
    {
        // An object of a vehicle's model is a piece of a vehicle's
        // fragment; only the vehicle piece path records it (the projectile
        // successor took a wagon board, and playback built a whole wagon
        // from the model: a test clip, prop 60:1080).
        const bool vehicleModel = STREAMING::IS_MODEL_A_VEHICLE(ENTITY::GET_ENTITY_MODEL(object)) != FALSE;
        if (!replay::vehicle_pieces::PropModelAllowed(origin, vehicleModel))
        {
            Ctx().log << "POSE_V2_PROP_REFUSED,reason=vehicle_model,how=" << how << ",handle=" << object
                << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) << std::dec
                << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            return;
        }
        // rope_models.h: a rope's own entity (one ROPE_REINS by every horse) is
        // the rope track's; as a prop it never loaded in playback. A gore piece
        // keeps its own path (adopted).
        if (origin != replay::entity::PropOrigin::GorePiece)
            if (const char* rope = replay::ropemodels::RopeModelName(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object))))
            {
                ++g_poseV2RopeModelsSkipped;
                if (g_poseV2RopeModelLogs < 8)
                {
                    ++g_poseV2RopeModelLogs;
                    Ctx().log << "POSE_V2_PROP_REFUSED,reason=rope_model,model=" << rope << ",how=" << how << ",handle=" << object
                              << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
                }
                return;
            }
        if (Ctx().recorder.props.size() >= replay::recorder::kPropCapacity)
        {
            ++g_poseV2PropsDropped;
            return;
        }
        const replay::entity::ReplayId id = Ctx().clip.ids.Allocate();
        if (!id.Valid())
            return;
        const Hash model = ENTITY::GET_ENTITY_MODEL(object);
        // A loose weapon object (the knife left stuck in the
        // table) is rebuilt as its weapon with its components, read from the
        // object when the caller had no weapon for it.
        Hash propWeapon = weaponHash;
        std::vector<std::uint32_t> propComponents = weaponComponents;
        const char* propWeaponFrom = weaponHash ? "caller" : "none";
        if (!propWeapon && origin != replay::entity::PropOrigin::DroppedHat &&
            origin != replay::entity::PropOrigin::VehiclePiece && origin != replay::entity::PropOrigin::GorePiece)
        {
            bool hasWeapon = false;
            propWeapon = ReadObjectWeapon(object, model, hasWeapon);
            if (propWeapon)
            {
                ReadObjectComponents(object, propComponents);
                propWeaponFrom = "object";
                ++g_objectWeaponFromObject;
            }
            else if (hasWeapon)
            {
                ++g_objectWeaponUnresolved;
                LogObjectCheck(replay::objcheck::Verdict::WeaponPlain, replay::entity::IdText(id), model,
                    replay::pose::capture::NowMs(), ",phase=record,why=weapon_unresolved");
            }
        }
        replay::entity::CreatePacket packet;
        packet.id = id;
        packet.timeMs = replay::pose::capture::NowMs();
        packet.kind = replay::entity::EntityKind::Prop;
        packet.modelHash = model;
        packet.sourceHandle = object;
        packet.owner = ownerId;
        packet.propOrigin = origin;
        packet.weaponHash = propWeapon;
        packet.weaponComponents = propComponents;
        if (origin == replay::entity::PropOrigin::VehiclePiece)
            packet.boneCount = pieceGroup; // the group (vehicle_pieces.h)
        const bool layersRead = origin == replay::entity::PropOrigin::DroppedHat &&
            GuardedCaptureLayers(object, &packet.appearance);
        // hat_look.h: a hat object keeps the layer it draws in its own tags,
        // whatever path records it (dropped, moved world object, picked up):
        // the metaped natives read none from an object (layers_read=1,layers=0
        // on all 18 dropped hats of the Session), so playback built the bare
        // model, a grey octagon.
        const char* hatLook = "skipped";
        if (packet.appearance.assets.empty() && !propWeapon && origin != replay::entity::PropOrigin::VehiclePiece &&
            origin != replay::entity::PropOrigin::GorePiece)
        {
            hatLook = ReadPoseV2HatLook(object, packet.appearance.assets);
            if (!packet.appearance.assets.empty())
            {
                ++g_hatLookStats.props;
                const Vector3 hatAt = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                const float hatPosition[3] = {hatAt.x, hatAt.y, hatAt.z};
                LogPoseV2HatLook("prop", replay::entity::IdText(id) + std::string(";how=") + how, model,
                    packet.appearance.assets, hatPosition);
            }
        }
        // object_pose.h ContinuesPose: the posed object that left
        // the hand (the rolled pelt) goes on with its pose under the prop's
        // id. The weapon's record was stopped first (ContinuePoseV2WeaponAsProp),
        // which freed its place and its skeleton; the start comes before the
        // create, so the create carries the bone count.
        bool posed = false;
        bool poseCandidate = false;
        bool poseNoPlace = false;
        replay::skeleton::ResolvedPoseSkeleton objectSkeleton{};
        // object_pose.h spawn::PoseSpawned: a free object spawned
        // next to a recorded ped whose bones were seen moving starts its pose
        // the same way.
        if ((continuation && replay::object_pose::ContinuesPose(origin == replay::entity::PropOrigin::DroppedWeapon,
                                 weaponHash, continuation->posed, continuation->candidate)) ||
            (spawnedPose && propWeapon == 0))
        {
            std::string poseError;
            std::size_t posedBefore = 0;
            const char* poseStatus =
                TryStartPoseV2ObjectPose(object, id, objectSkeleton, poseError, posed, &posedBefore);
            if (posed)
            {
                packet.boneCount = objectSkeleton.boneCount;
                if (continuation)
                    ++g_objectPoseStats.continued;
                else
                    ++g_objectPoseStats.spawned;
            }
            poseCandidate = !posed && PoseV2ObjectPoseRetryable(poseStatus);
            poseNoPlace = poseCandidate && std::strcmp(poseStatus, "no_place") == 0;
            if (g_poseV2ObjectPoseLogs < 48)
            {
                ++g_poseV2ObjectPoseLogs;
                Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(id)
                          << ",from=" << (continuation ? replay::entity::IdText(continuation->from) : std::string("none"))
                          << ",model=0x" << std::hex << model << std::dec << ",status=" << poseStatus
                          << (continuation ? ",continued=1" : ",spawned=1")
                          << ",how=" << how << ",bones=" << objectSkeleton.boneCount
                          << ",fragment_path=" << (objectSkeleton.usedFragmentPath ? 1 : 0)
                          << ",places=" << (posedBefore + (posed ? 1 : 0)) << "/" << ObjectPlacesLimit()
                          << ",retry=" << (poseCandidate ? 1 : 0)
                          << ",error=" << (poseError.empty() ? "none" : poseError.c_str())
                          << ",time_ms=" << packet.timeMs << '\n';
            }
        }
        Ctx().clip.lifecycle.Create(packet);
        Ctx().clip.created.push_back(packet);
        Ctx().recorder.props.push_back({object, id});
        NotePoseV2FragPiece(object, id, origin, propWeapon);
        // weapon_look_core.h: a weapon that leaves the hand
        // keeps its look (components and wear) as a one-state track.
        if (propWeapon && replay::entities::Exists(object))
        {
            replay::WeaponCondition look;
            ReadPoseV2WeaponLook(object, &propComponents, look);
            LogPoseV2WeaponLookRecord(id, look, NotePoseV2WeaponLook(id.Packed(), look, packet.timeMs, true), packet.timeMs);
        }
        if (posed)
        {
            Ctx().recorder.props.back().posed = true;
            Ctx().recorder.props.back().poseSkeleton = objectSkeleton.skeleton;
            Ctx().recorder.props.back().poseBones = objectSkeleton.boneCount;
        }
        if (poseCandidate)
        {
            Ctx().recorder.props.back().poseCandidate = true;
            Ctx().recorder.props.back().poseNoPlace = poseNoPlace;
            Ctx().recorder.props.back().nextPoseTryMs = packet.timeMs + replay::object_pose::kRetryIntervalMs;
        }
        // No pose and none awaited: its bones watched (a vehicle piece, hat,
        // gore piece or weapon never is).
        WatchPoseV2PropBones(Ctx().recorder.props.back(), origin, propWeapon, packet.timeMs);
        Ctx().clip.weaponTracks.push_back({id.Packed(), replay::entity::RelativeTrack(replay::recorder::kSampleHz)});

        const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
        const Vector3 ped = ENTITY::GET_ENTITY_COORDS(ownerHandle, TRUE, FALSE);
        std::ostream& log = Ctx().log;
        const replay::logfmt::FormatScope logFormat(log); // precision not left behind
        log << std::fixed << std::setprecision(2) << "POSE_V2_PROP_CREATE"
            << ",id=" << replay::entity::IdText(id)
            << ",owner=" << replay::entity::IdText(ownerId)
            << ",origin=" << (origin == replay::entity::PropOrigin::DroppedHat ? "hat"
                : origin == replay::entity::PropOrigin::GorePiece ? "gore_piece"
                : origin == replay::entity::PropOrigin::Projectile ? "projectile"
                : origin == replay::entity::PropOrigin::WorldObject ? "world_object"
                : origin == replay::entity::PropOrigin::WorldPiece ? "world_piece"
                : origin == replay::entity::PropOrigin::VehiclePiece ? "vehicle_piece" : "weapon")
            << ",how=" << how
            << ",handle=" << object
            << ",model=0x" << std::hex << model
            << ",weapon=0x" << propWeapon << std::dec
            << ",weapon_from=" << propWeaponFrom
            << ",components=" << propComponents.size()
            << ",model_valid=" << (STREAMING::IS_MODEL_VALID(model) ? 1 : 0)
            << ",attached=" << (ENTITY::IS_ENTITY_ATTACHED(object) ? 1 : 0)
            << ",visible=" << (ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0)
            << ",layers_read=" << (layersRead ? 1 : 0)
            << ",layers=" << packet.appearance.assets.size()
            << ",hat_look=" << hatLook
            << ",distance_m=" << std::sqrt(
                   (at.x - ped.x) * (at.x - ped.x) + (at.y - ped.y) * (at.y - ped.y) +
                   (at.z - ped.z) * (at.z - ped.z))
            << ",time_ms=" << packet.timeMs
            << '\n';
        log.unsetf(std::ios_base::floatfield);
        log.flush();
        if (weaponHash && WEAPON::_IS_WEAPON_THROWABLE(weaponHash))
            WatchPoseV2Throwable(object, id, ownerId, ownerHandle, weaponHash);
    }

    void ContinuePoseV2WeaponAsProp(const replay::recorder::RecordedWeapon& weapon, const char* reason)
    {
        const replay::recorder::RecordedEntity* owner = nullptr;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.handle == weapon.parent)
                owner = &entity;
        }
        const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(weapon.id.Packed());
        if (!owner || !create)
            return;
        const bool exists = replay::entities::Exists(weapon.object) != FALSE;
        const bool attached = exists && ENTITY::IS_ENTITY_ATTACHED(weapon.object);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_WEAPON_DROP"
            << ",weapon=" << replay::entity::IdText(weapon.id)
            << ",owner=" << replay::entity::IdText(owner->id)
            << ",reason=" << reason
            << ",exists=" << (exists ? 1 : 0)
            << ",attached=" << (attached ? 1 : 0)
            << ",attached_to_owner=" << (attached &&
                   ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(weapon.object, weapon.parent) ? 1 : 0)
            << ",dead=" << (PED::IS_PED_DEAD_OR_DYING(weapon.parent, TRUE) ? 1 : 0)
            << ",time_ms=" << replay::pose::capture::NowMs()
            << '\n';
        log.flush();
        // Still on the ped (holstered): not a drop.
        if (attached)
            return;
        // object_pose.h ContinuesPose: what the prop continues.
        const PoseContinuation continuation{weapon.id, weapon.posed, weapon.poseCandidate};
        if (exists)
        {
            // The weapon's record stops before the prop starts one on the
            // same skeleton (capture::Start refuses a skeleton still armed),
            // and the weapon no longer counts as posed (it is retired right
            // after this by the caller).
            if (weapon.posed)
            {
                replay::pose::capture::Stop(weapon.id.Packed());
                for (replay::recorder::RecordedWeapon& known : Ctx().recorder.weapons)
                {
                    if (known.id == weapon.id)
                    {
                        known.posed = false;
                        known.poseCandidate = false;
                        known.poseNoPlace = false;
                    }
                }
            }
            RegisterPoseV2Prop(weapon.object, owner->id, owner->handle,
                replay::entity::PropOrigin::DroppedWeapon, create->weaponHash, "same_object",
                create->weaponComponents, 0, &continuation);
            return;
        }
        Ctx().recorder.drops.push_back({owner->id, owner->handle, weapon.object, create->modelHash,
            create->weaponHash, create->weaponComponents, weapon.lastWorld,
            replay::pose::capture::NowMs() + replay::recorder::kDropSearchMs,
            weapon.attachPoint == replay::recorder::kAttachedObjectPoint ? weapon.parent : 0,
            weapon.posed || weapon.poseCandidate});
        Ctx().recorder.drops.back().poseFrom = weapon.id;
    }

    // Per frame while a replacement is expected: the nearest free object of
    // the weapon's model around where the weapon was, or the nearest pickup
    // (a weapon left on the ground can become a pickup, in its own pool and
    // with a model of its own).
    void SearchPoseV2WeaponDrops()
    {
        if (Ctx().recorder.drops.empty())
            return;
        static int objects[1024];
        static int pickups[256];
        const int objectCount = (std::min)(ListObjects(objects, 1024), 1024);
        const int pickupCount = (std::min)(ListPickups(pickups, 256), 256);
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (std::size_t index = Ctx().recorder.drops.size(); index-- > 0;)
        {
            const replay::recorder::WeaponDrop drop = Ctx().recorder.drops[index];
            const auto distanceTo = [&](Entity entity) {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
                return std::sqrt(
                    (at.x - drop.at.position.x) * (at.x - drop.at.position.x) +
                    (at.y - drop.at.position.y) * (at.y - drop.at.position.y) +
                    (at.z - drop.at.position.z) * (at.z - drop.at.position.z));
            };
            const auto free = [&](Entity entity) {
                return entity != drop.previous && replay::entities::Exists(entity) &&
                    !ENTITY::IS_ENTITY_ATTACHED(entity) && !IsPoseV2KnownObject(entity);
            };
            // An attached object that went (the engine swaps a
            // stuck knife for a new object on the same ped): the closest
            // object of its model that is attached to the same entity, not
            // known yet, continues it as an attached object of that entity.
            if (drop.attachedTo && replay::entities::Exists(drop.attachedTo))
            {
                const Object attached = OBJECT::GET_CLOSEST_OBJECT_OF_TYPE(drop.at.position.x, drop.at.position.y,
                    drop.at.position.z, replay::recorder::kDropSearchRadiusM, drop.model, FALSE, FALSE, FALSE);
                replay::recorder::RecordedEntity* parent = Ctx().recorder.FindByHandle(drop.attachedTo);
                if (attached && attached != drop.previous && parent && replay::entities::Exists(attached) &&
                    ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(attached, drop.attachedTo) && !IsPoseV2KnownObject(attached))
                {
                    RetirePoseV2PropOf(attached, "attached");
                    const bool registered = RegisterPoseV2Weapon(attached, *parent, replay::recorder::kAttachedObjectPoint,
                        drop.weaponHash && WEAPON::_GET_WEAPONTYPE_MODEL(drop.weaponHash) == ENTITY::GET_ENTITY_MODEL(attached)
                            ? drop.weaponHash : 0);
                    if (registered)
                        Ctx().recorder.weapons.back().seen = true;
                    Ctx().log << "POSE_V2_ATTACHED_SUCCESSOR,owner=" << replay::entity::IdText(drop.owner)
                        << ",parent=" << replay::entity::IdText(parent->id) << ",handle=" << attached
                        << ",model=0x" << std::hex << drop.model << std::dec
                        << ",id=" << (registered ? replay::entity::IdText(Ctx().recorder.weapons.back().id) : std::string("none"))
                        << ",time_ms=" << nowMs << '\n';
                    Ctx().recorder.drops.erase(Ctx().recorder.drops.begin() + static_cast<std::ptrdiff_t>(index));
                    continue;
                }
            }
            Entity found = 0;
            const char* how = "";
            float best = replay::recorder::kDropSearchRadiusM;
            for (int slot = 0; slot < objectCount; ++slot)
            {
                const Entity object = objects[slot];
                if (!free(object) || ENTITY::GET_ENTITY_MODEL(object) != drop.model)
                    continue;
                const float distance = distanceTo(object);
                if (distance < best)
                {
                    best = distance;
                    found = object;
                    how = "replacement_object";
                }
            }
            // A pickup continues the weapon only when it has the
            // weapon's model or its weapon type's model (any pickup within
            // 3 m made a phantom prop of another object); a plain object
            // (no weapon hash) is not looked for among the pickups at all.
            const std::uint32_t weaponTypeModel = drop.weaponHash
                ? static_cast<std::uint32_t>(WEAPON::_GET_WEAPONTYPE_MODEL(drop.weaponHash)) : 0;
            int pickupsOtherModel = 0;
            if (!found && drop.weaponHash)
            {
                best = replay::recorder::kDropSearchRadiusM;
                for (int slot = 0; slot < pickupCount; ++slot)
                {
                    const Entity pickup = pickups[slot];
                    if (!free(pickup))
                        continue;
                    const std::uint32_t pickupModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(pickup));
                    if (pickupModel != drop.model && (!weaponTypeModel || pickupModel != weaponTypeModel))
                    {
                        if (distanceTo(pickup) < replay::recorder::kDropSearchRadiusM)
                            ++pickupsOtherModel;
                        continue;
                    }
                    const float distance = distanceTo(pickup);
                    if (distance < best)
                    {
                        best = distance;
                        found = pickup;
                        how = pickupModel == drop.model ? "pickup_same_model" : "pickup_weapon_model";
                    }
                }
            }
            if (!found)
            {
                // The pool listing stops when the script handle pool has 256
                // free slots or fewer and then returns 0 (ScriptHookRDR2,
                // offline disassembly; live: objects=0,pickups=0 in a large
                // fight). The closest object of the model needs no listing;
                // a dead ped's weapon pickup has the weapon's model (live).
                const Object closest = OBJECT::GET_CLOSEST_OBJECT_OF_TYPE(drop.at.position.x, drop.at.position.y,
                    drop.at.position.z, replay::recorder::kDropSearchRadiusM, drop.model, FALSE, FALSE, FALSE);
                if (closest && free(closest))
                {
                    found = closest;
                    how = "closest_of_type";
                }
            }
            if (found)
            {
                // The replacement continues the weapon's pose.
                const PoseContinuation continuation{drop.poseFrom, false, drop.poseCandidate};
                RegisterPoseV2Prop(found, drop.owner, drop.ownerHandle,
                    replay::entity::PropOrigin::DroppedWeapon, drop.weaponHash, how,
                    drop.weaponComponents, 0, &continuation);
            }
            else if (static_cast<std::int32_t>(nowMs - drop.deadlineMs) < 0)
            {
                continue;
            }
            else
            {
                // Evidence: everything near where the weapon was.
                std::ostream& log = Ctx().log;
                log << "POSE_V2_WEAPON_DROP_NOT_FOUND"
                    << ",owner=" << replay::entity::IdText(drop.owner)
                    << ",model=0x" << std::hex << drop.model << std::dec
                    << ",objects=" << objectCount
                    << ",pickups=" << pickupCount
                    << ",pickup_search=" << (drop.weaponHash ? "model" : "skipped_no_weapon")
                    << ",pickups_other_model=" << pickupsOtherModel
                    << ",near=";
                int listed = 0;
                for (int pass = 0; pass < 2 && listed < 12; ++pass)
                {
                    const int* pool = pass ? pickups : objects;
                    const int count = pass ? pickupCount : objectCount;
                    for (int slot = 0; slot < count && listed < 12; ++slot)
                    {
                        const Entity entity = pool[slot];
                        if (!replay::entities::Exists(entity) ||
                            distanceTo(entity) >= replay::recorder::kDropSearchRadiusM)
                        {
                            continue;
                        }
                        ++listed;
                        log << (pass ? "p:" : "o:") << entity << ":0x" << std::hex
                            << ENTITY::GET_ENTITY_MODEL(entity) << std::dec
                            << ':' << (ENTITY::IS_ENTITY_ATTACHED(entity) ? 'a' : 'f') << ' ';
                    }
                }
                log << ",time_ms=" << nowMs << '\n';
                log.flush();
            }
            Ctx().recorder.drops.erase(Ctx().recorder.drops.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    namespace
    {

    void FollowPoseV2Projectiles(const int* objects, int count, ULONGLONG now)
    {
        constexpr ULONGLONG kSuccessorWindowMs = 500;
        // The last sample can be a tick (about 2 m at arrow speed) short of
        // where it stuck.
        constexpr float kSuccessorRadiusM = 4.0f;
        for (std::size_t index = g_poseV2ProjectileWatch.size(); index-- > 0;)
        {
            ProjectileWatch& watch = g_poseV2ProjectileWatch[index];
            if (!watch.goneAt)
            {
                if (replay::entities::Exists(watch.object))
                {
                    watch.last = ENTITY::GET_ENTITY_COORDS(watch.object, TRUE, FALSE);
                    // A throwable at rest for 2 s stays where it is
                    // (the same object stuck, or lying): its watch ends there,
                    // so a knife on the ground does not keep the per-frame
                    // object listing running.
                    if (watch.weaponHash)
                    {
                        if (ENTITY::GET_ENTITY_SPEED(watch.object) > 0.5f)
                            watch.restSince = 0;
                        else if (!watch.restSince)
                            watch.restSince = now;
                        else if (now - watch.restSince > 2000)
                        {
                            LogPoseV2ProjectileFate(watch, PoseV2StuckFate(watch.object), watch.object, "same_object");
                            g_poseV2ProjectileWatch.erase(g_poseV2ProjectileWatch.begin() + static_cast<std::ptrdiff_t>(index));
                        }
                    }
                    continue;
                }
                watch.goneAt = now;
            }
            Object found = 0;
            float best = kSuccessorRadiusM * kSuccessorRadiusM;
            for (int slot = 0; slot < count; ++slot)
            {
                const Object object = objects[slot];
                if (!object || object == watch.object ||
                    std::binary_search(g_poseV2ProjectilePrevObjects.begin(), g_poseV2ProjectilePrevObjects.end(), object) ||
                    !replay::entities::Exists(object) ||
                    ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) || IsPoseV2KnownObject(object) ||
                    STREAMING::IS_MODEL_A_VEHICLE(ENTITY::GET_ENTITY_MODEL(object))) // a vehicle piece
                {
                    continue;
                }
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                const float d = (at.x - watch.last.x) * (at.x - watch.last.x) + (at.y - watch.last.y) * (at.y - watch.last.y) +
                    (at.z - watch.last.z) * (at.z - watch.last.z);
                if (d < best)
                {
                    best = d;
                    found = object;
                }
            }
            // Not every object is in the script listing (the item props were
            // not either): by model near the spot, the flying model and the
            // three the network attach path treats as arrows.
            const char* how = "successor";
            if (!found)
            {
                // Plus the thrown weapon's own model (a hatchet is
                // not among the arrow models).
                const std::uint32_t weaponModel = watch.weaponHash
                    ? static_cast<std::uint32_t>(WEAPON::_GET_WEAPONTYPE_MODEL(watch.weaponHash)) : 0u;
                const std::uint32_t models[] = {watch.model, 0x445C85A5u, 0xC82FC323u, 0x8F44D14Au, 0x5F30F384u, weaponModel};
                for (const std::uint32_t model : models)
                {
                    if (!model)
                        continue;
                    const Object object = OBJECT::GET_CLOSEST_OBJECT_OF_TYPE(watch.last.x, watch.last.y, watch.last.z,
                        kSuccessorRadiusM, model, FALSE, FALSE, FALSE);
                    if (object && object != watch.object && replay::entities::Exists(object) && !IsPoseV2KnownObject(object) &&
                        !std::binary_search(g_poseV2ProjectilePrevObjects.begin(), g_poseV2ProjectilePrevObjects.end(), object))
                    {
                        found = object;
                        how = "successor_by_model";
                        break;
                    }
                }
            }
            if (found && replay::entities::Exists(watch.ownerHandle))
            {
                ++g_poseV2ProjectileSuccessors;
                // A successor stuck in a recorded ped is an
                // attached object of that ped (as the attached object scan
                // would record it a scan later), parent-relative.
                const Entity parent = ENTITY::IS_ENTITY_ATTACHED(found) ? ENTITY::GET_ENTITY_ATTACHED_TO(found) : 0;
                replay::recorder::RecordedEntity* victim = parent ? Ctx().recorder.FindByHandle(parent) : nullptr;
                if (victim && !victim->vehicle &&
                    RegisterPoseV2Weapon(found, *victim, replay::recorder::kAttachedObjectPoint,
                        watch.weaponHash && WEAPON::_GET_WEAPONTYPE_MODEL(watch.weaponHash) == ENTITY::GET_ENTITY_MODEL(found)
                            ? watch.weaponHash : 0))
                    Ctx().recorder.weapons.back().seen = true;
                else
                    RegisterPoseV2Prop(found, watch.owner, watch.ownerHandle, replay::entity::PropOrigin::Projectile, 0, how);
            }
            if (found)
            {
                const char* fate = PoseV2StuckFate(found);
                if (std::strcmp(fate, "stuck_world") == 0 && static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(found)) != watch.model)
                    fate = "converted";
                LogPoseV2ProjectileFate(watch, fate, found, how);
            }
            else if (now - watch.goneAt > kSuccessorWindowMs)
                LogPoseV2ProjectileFate(watch, "lost", 0, "none");
            if (found || now - watch.goneAt > kSuccessorWindowMs)
                g_poseV2ProjectileWatch.erase(g_poseV2ProjectileWatch.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    }

    void SearchPoseV2Projectiles()
    {
        constexpr ULONGLONG kWindowMs = 1500;
        constexpr float kMinSpeed = 8.0f;
        constexpr float kRadiusM = 60.0f;
        const ULONGLONG now = GetTickCount64();
        for (const replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
            if (!owner.vehicle && replay::entities::Exists(owner.handle) && PED::IS_PED_SHOOTING(owner.handle))
                g_poseV2ProjectileScanUntil = now + kWindowMs;
        if (now > g_poseV2ProjectileScanUntil && g_poseV2ProjectileWatch.empty())
            return;
        REPLAY_PROFILE_ZONE("rec.projectiles");
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        FollowPoseV2Projectiles(objects, count, now);
        g_poseV2ProjectilePrevObjects.assign(objects, objects + count);
        std::sort(g_poseV2ProjectilePrevObjects.begin(), g_poseV2ProjectilePrevObjects.end());
        if (now > g_poseV2ProjectileScanUntil)
            return;
        for (int slot = 0; slot < count; ++slot)
        {
            const Object object = objects[slot];
            if (!object || !replay::entities::Exists(object) || ENTITY::IS_ENTITY_ATTACHED(object) ||
                ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) || IsPoseV2KnownObject(object) ||
                ENTITY::GET_ENTITY_SPEED(object) < kMinSpeed)
            {
                continue;
            }
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            const replay::recorder::RecordedEntity* nearest = nullptr;
            float best = kRadiusM * kRadiusM;
            for (const replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
            {
                if (owner.vehicle || !replay::entities::Exists(owner.handle))
                    continue;
                const Vector3 ped = ENTITY::GET_ENTITY_COORDS(owner.handle, TRUE, FALSE);
                const float d = (at.x - ped.x) * (at.x - ped.x) + (at.y - ped.y) * (at.y - ped.y) + (at.z - ped.z) * (at.z - ped.z);
                if (d < best)
                {
                    best = d;
                    nearest = &owner;
                }
            }
            if (!nearest)
                continue;
            ++g_poseV2Projectiles;
            const std::size_t before = Ctx().recorder.props.size();
            RegisterPoseV2Prop(object, nearest->id, nearest->handle, replay::entity::PropOrigin::Projectile, 0, "moving_after_shot");
            g_poseV2ProjectileWatch.push_back({object, nearest->id, nearest->handle, at, 0,
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)),
                Ctx().recorder.props.size() > before ? Ctx().recorder.props.back().id : replay::entity::ReplayId{}});
        }
    }

    void ScanPoseV2Weapons()
    {
        for (replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            weapon.seen = false;
        CollectPoseV2ScanObjects();
        static std::vector<std::pair<Entity, Object>> attachedByParent;
        attachedByParent.clear();
        for (const PoseV2ScanObject& listed : g_poseV2ScanObjects)
            if (listed.parent)
                attachedByParent.push_back({listed.parent, listed.object});
        std::sort(attachedByParent.begin(), attachedByParent.end());
        const std::uint32_t scanMs = replay::pose::capture::NowMs();
        REPLAY_PROFILE_ZONE("rec.weapon_points");
        for (replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
        {
            if (owner.vehicle || !replay::entities::Exists(owner.handle))
                continue;
            // Evidence: the weapon in hand and every attach point's object
            // whenever the weapon in hand changes.
            const Hash held = WEAPON::_GET_PED_CURRENT_HELD_WEAPON(owner.handle);
            if (!PoseV2PointsNeeded(owner, held, scanMs, attachedByParent))
            {
                ++g_poseV2PointScansSkipped;
                for (replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                    if (weapon.parent == owner.handle && weapon.attachPoint >= 0 &&
                        weapon.attachPoint < replay::recorder::kWeaponAttachPoints)
                        weapon.seen = true;
                continue;
            }
            ++g_poseV2PointScansFull;
            {
                PoseV2PointScan& state = g_poseV2PointScans[owner.handle];
                state.scanned = true;
                state.held = held;
                // Staggered by id so the refreshes spread over the scans.
                state.nextFullMs = scanMs + kPoseV2PointRefreshMs + (owner.id.Packed() % 5u) * 200u;
            }
            // The 32 points are read once per full scan and serve the
            // evidence line and the registration below (the same tick, the
            // same answers); the first scan of a recording read them twice
            // for ~60 peds: ~3800 natives, 42 ms in one tick.
            Entity pointObjects[replay::recorder::kWeaponAttachPoints]{};
            for (int point = 0; point < replay::recorder::kWeaponAttachPoints; ++point)
                pointObjects[point] = WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(owner.handle, point);
            if (held != owner.lastHeld && g_poseV2WeaponEvidenceLines < 200)
            {
                ++g_poseV2WeaponEvidenceLines;
                owner.lastHeld = held;
                std::ostream& log = Ctx().log;
                log << "POSE_V2_PED_HELD_WEAPON,ped=" << replay::entity::IdText(owner.id)
                    << ",held=0x" << std::hex << held << std::dec
                    << ",name=" << (held ? PoseV2WeaponName(held) : "none")
                    << ",points=";
                for (int point = 0; point < replay::recorder::kWeaponAttachPoints; ++point)
                {
                    const Entity object = pointObjects[point];
                    if (!object)
                        continue;
                    Hash weapon = 0;
                    WEAPON::GET_CURRENT_PED_WEAPON(owner.handle, &weapon, FALSE, point, FALSE);
                    log << point << ':' << object << ":0x" << std::hex << ENTITY::GET_ENTITY_MODEL(object)
                        << ":0x" << weapon << std::dec
                        << ':' << (ENTITY::IS_ENTITY_VISIBLE(object) ? 'v' : 'h') << ' ';
                }
                log << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            }
            for (int point = 0; point < replay::recorder::kWeaponAttachPoints; ++point)
            {
                const Entity object = pointObjects[point];
                if (!object || !replay::entities::Exists(object))
                    continue;
                const auto known = std::find_if(Ctx().recorder.weapons.begin(), Ctx().recorder.weapons.end(),
                    [&](const replay::recorder::RecordedWeapon& weapon) { return weapon.object == object; });
                if (known == Ctx().recorder.weapons.end())
                {
                    // Picked up again: the prop it was ends here.
                    RetirePoseV2PropOf(object, "picked_up");
                    RegisterPoseV2Weapon(object, owner, point);
                    continue;
                }
                if (known->seen)
                    continue;
                known->seen = true;
                if ((known->parent != owner.handle || known->attachPoint != point) &&
                    g_poseV2WeaponEvidenceLines < 200)
                {
                    ++g_poseV2WeaponEvidenceLines;
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_WEAPON_MOVE,id=" << replay::entity::IdText(known->id)
                        << ",ped=" << replay::entity::IdText(owner.id)
                        << ",from_point=" << known->attachPoint
                        << ",to_point=" << point
                        << ",visible=" << (ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0)
                        << ",time_ms=" << replay::pose::capture::NowMs()
                        << '\n';
                    log.flush();
                }
                if (known->parent != owner.handle)
                    ChangePoseV2WeaponOwner(*known, owner, point, "weapon_point");
                known->parent = owner.handle;
                known->attachPoint = point;
                WatchPoseV2WeaponBones(*known, scanMs); // a weapon drawn into a hand
            }
        }
        ScanPoseV2AttachedObjects();
        ScanPoseV2WorldObjects();
        ScanPoseV2SpawnedObjects();
        ScanPoseV2Scenery();
        FlushPoseV2FragBreaks();
        for (std::size_t index = Ctx().recorder.weapons.size(); index-- > 0;)
        {
            if (!Ctx().recorder.weapons[index].seen)
            {
                ContinuePoseV2WeaponAsProp(Ctx().recorder.weapons[index], "not_attached");
                RetirePoseV2Weapon(index, "not_attached");
            }
        }
    }

    void BeginRecording()
    {
        replay::objlook::BeginRecording();
        SnapshotPoseV2World();
        g_poseV2ProjectileScanUntil = 0;
        g_poseV2Projectiles = 0;
        g_poseV2ProjectileSuccessors = 0;
        g_poseV2ProjectileWatch.clear();
        g_poseV2ProjectileFateLogs = 0;
        g_poseV2ProjectileWatchLogs = 0;
        g_poseV2OwnerChangeLogs = 0;
        g_poseV2PropsDropped = 0;
        g_poseV2RopeModelsSkipped = 0;
        g_poseV2RopeModelLogs = 0;
        g_poseV2ProjectilePrevObjects.clear();
        g_poseV2WeaponEvidenceLines = 0;
        // The engine reads are checked again each recording.
        g_weaponComponentsEngine = true;
        g_weaponComponentsChecked = false;
        g_weaponComponentsCrossChecks = 0;
        g_weaponWearRaw = true;
        g_weaponWearChecked = false;
        g_weaponLookRecLogs = 0;
        g_poseV2ObjectPoseLogs = 0;
        g_objectBurnStats = {};
        g_objectBurnEntryLogs = g_objectBurnLogs = 0;
        // object_pose.h: the capture places of this recording.
        g_objectPoseStats = {};
        g_propBoneStats = {};
        g_weaponBoneStats = {};
        replay::weapon::ResetWeaponPassRecords();
        g_passRecWeapons.clear();
        g_passRecPublished.clear();
        g_ammoStats = {}; // (C)
        g_ammoPrevHandles.clear();
        g_ammoPending.clear();
        // object_pose.h WeaponPlaces: every capture slot the
        // bodies and vehicles do not use.
        g_weaponBonePlaces = replay::object_pose::WeaponPlaces(replay::pose::capture::kLiveTargetSlots,
            static_cast<std::uint32_t>(replay::pose::capture::kMaxLiveTargets));
        // The train cars' pose records hold spare slots of their own, out of
        // the weapons' count, so the bodies, the weapons and the cars never
        // take each other's.
        const std::uint32_t trainPlaces = (std::min)(replay::trains::PosePlaces(), g_weaponBonePlaces);
        g_weaponBonePlaces -= trainPlaces;
        replay::pose::capture::SetExtraPlaces(g_weaponBonePlaces + trainPlaces);
        {
            const std::uint32_t pedPlaces = replay::recorder::kPedCapacity;
            const std::uint32_t vehiclePlaces = replay::pose::capture::kMaxLiveTargets - replay::recorder::kPedCapacity;
            Ctx().log << "POSE_V2_CAPTURE_PLACES,peds=" << pedPlaces << ",vehicles=" << vehiclePlaces
                      << ",objects=" << ObjectPlacesLimit()
                      << ",objects_from=shared_with_weapons"
                      << ",peds_with_objects=" << pedPlaces
                      << ",total=" << replay::pose::capture::kMaxLiveTargets << '\n';
        }
        g_poseV2PointScans.clear();
        g_poseV2PointScansFull = g_poseV2PointScansSkipped = 0;
        g_poseV2ScanObjects.clear();
        ResetPoseV2Spawned();
        g_vehicleParents.clear();
        g_vehicleChildStats = {};
        g_vehicleChildRefused.clear();
        g_vehicleChildRefusedResetMs = 0;
        g_lootReinstanceStats = {};
        g_goreWorldPiecesRefused = 0; // (E)
    }

    void EndRecording()
    {
        replay::objlook::EndRecording();
        Ctx().log << "POSE_V2_PROJECTILES,recorded=" << g_poseV2Projectiles << ",successors=" << g_poseV2ProjectileSuccessors
            << ",watching=" << g_poseV2ProjectileWatch.size() << ",props_dropped_full=" << g_poseV2PropsDropped
            << ",rope_models_skipped=" << g_poseV2RopeModelsSkipped << '\n';
        Ctx().log << "POSE_V2_FRAG_BREAKS,total=" << g_fragBreaks << ",matched=" << g_fragBreaksMatched
            << ",events=" << Ctx().clip.fragBreaks.size() << ",unassigned=" << g_fragPending.size()
            << ",vehicle_pieces=" << g_vehiclePiecesRecorded << ",vehicle_pieces_lost=" << g_vehiclePiecesLost
            << ",vehicle_pieces_pending=" << g_vehiclePiecesPending.size()
            << ",frag_pieces=" << g_fragPiecesRecorded << ",frag_pieces_cache=" << g_fragPiecesFromCache
            << ",frag_piece_calls_left=" << g_fragPieceCalls.size() << '\n';
        // The wheels that came off the recorded vehicles and their objects
        // (recorded + known + lost = off when the layout is proven; lost > 0 is
        // a wheel that will vanish).
        Ctx().log << "POSE_V2_VEHICLE_WHEELS,phase=record,layout=" << (g_wheelLayout > 0 ? "proven" : g_wheelLayout < 0 ? "mismatch" : "unchecked")
            << ",off=" << g_wheelPieceStats.off << ",recorded=" << g_wheelPieceStats.recorded << ",known=" << g_wheelPieceStats.known
            << ",lost=" << g_wheelPieceStats.lost << ",pending=" << g_wheelPiecesPending.size()
            << ",no_object_reads=" << g_wheelPieceStats.noObject << ",read_failed=" << g_wheelPieceStats.readFailed
            << ",unlisted_reads=" << g_wheelPieceStats.unlisted << ",urgent_samples=" << g_wheelPieceStats.urgent << '\n';
        Ctx().log << "POSE_V2_WORLD_OBJECTS,recorded=" << g_worldObjectsRecorded << ",pieces=" << g_worldPiecesRecorded
            << ",snapshot=" << g_worldSnapshot.size() << ",animated=" << g_worldAnimatedRecorded
            << ",animated_watched=" << g_worldAnimatedWatched << ",late_map=" << g_worldLateMapObjects
            << ",skeleton_rereads=" << g_worldSkeletonStats.rereads << ",skeleton_appeared=" << g_worldSkeletonStats.appeared
            << ",skeleton_baselines=" << g_worldSkeletonStats.baselines << ",late_scenery=" << g_worldSkeletonStats.lateScenery
            << ",skeleton_appeared_map=" << g_worldSkeletonStats.appearedMap << ",rest_away=" << g_worldSkeletonStats.restAway
            << ",rest_at_rest=" << g_worldSkeletonStats.restAtRest << ",rest_unknown=" << g_worldSkeletonStats.restUnknown
            << ",late_map_joins=" << g_worldSkeletonStats.lateJoins << ",near_taken=" << g_nearWorldTaken
            << ",open_at_start=" << g_openAtStartTaken << '\n';
        // The objects on vehicles and train cars.
        Ctx().log << "POSE_V2_VEHICLE_CHILDREN,phase=record,recorded=" << g_vehicleChildStats.recorded
            << ",on_train_cars=" << g_vehicleChildStats.onTrainCars << ",posed=" << g_vehicleChildStats.posed
            << ",refused_far=" << g_vehicleChildStats.refusedFar << ",refused_full=" << g_vehicleChildStats.refusedFull
            << ",attached_elsewhere=" << g_spawnStats.elsewhere << '\n';
        Ctx().log << "POSE_V2_GORE_PIECES,phase=record,world_pieces_refused=" << g_goreWorldPiecesRefused << '\n'; // (E)
        // Removals still waiting for a remake
        // when the recording stops are takes.
        for (const LootGonePending& pending : g_lootGonePending)
        {
            ++g_lootReinstanceStats.gone;
            AddPoseV2Loot(pending.object, pending.watch, replay::objcheck::LootTake::Gone, pending.goneMs, false);
        }
        g_lootGonePending.clear();
        for (const DressingGonePending& pending : g_dressingGonePending)
            if (pending.index < Ctx().clip.scenery.size())
            {
                Ctx().clip.scenery[pending.index].goneMs = pending.goneMs;
                ++g_sceneryStats.gone;
                ++g_mapDressingStats.takenAway;
            }
        g_dressingGonePending.clear();
        // (object_check.h).
        Ctx().log << "POSE_V2_SCENERY,phase=record,entries=" << Ctx().clip.scenery.size()
                  << ",snapshot=" << g_sceneryStats.snapshot << ",late=" << g_sceneryStats.late
                  << ",gone=" << g_sceneryStats.gone << ",taken=" << g_sceneryStats.taken
                  << ",moving=" << g_sceneryStats.moving << ",full=" << g_sceneryStats.full
                  << ",invisible=" << g_sceneryStats.invisible << ",visibility_flips=" << g_sceneryStats.visibilityFlips
                  << ",pickups=" << g_sceneryStats.pickups << ",loot_watched=" << g_sceneryStats.lootWatched
                  << ",loot_hidden=" << g_sceneryStats.lootHidden << ",loot_gone=" << g_sceneryStats.lootGone
                  << ",loot_dropped=" << g_sceneryStats.lootDropped << ",loot_moved=" << g_sceneryStats.lootMoved
                  << ",loot_watch_full=" << g_sceneryStats.lootWatchFull
                  << ",loot_gone_waited=" << g_lootReinstanceStats.pending
                  << ",loot_reinstanced=" << g_lootReinstanceStats.reinstanced
                  << ",loot_gone_confirmed=" << g_lootReinstanceStats.gone
                  << ",loot_gone_still_waiting=" << g_lootGonePending.size()
                  << ",spawn_watch=" << g_sceneryStats.spawnWatch << ",far=" << g_sceneryStats.farLate
                  << ",far_rechecks=" << g_sceneryStats.farRechecks << ",far_resets=" << g_sceneryStats.farResets
                  << ",map_owner=" << g_mapOwner;
        // The entries by rule (objcheck::SceneryRule) and the map dressing's
        // fate.
        {
            std::uint32_t byRule[replay::objcheck::kSceneryRuleCount]{};
            for (const replay::SceneryObject& entry : Ctx().clip.scenery)
                ++byRule[entry.rule < replay::objcheck::kSceneryRuleCount ? entry.rule : 0];
            Ctx().log << ",scenery_by_rule=";
            for (std::size_t rule = 0; rule < replay::objcheck::kSceneryRuleCount; ++rule)
                if (byRule[rule])
                    Ctx().log << replay::objcheck::SceneryRuleText(static_cast<replay::objcheck::SceneryRule>(rule)) << ':'
                              << byRule[rule] << '|';
            Ctx().log << ",map_dressing_resolved=" << (g_mapDressingStats.resolved ? 1 : 0)
                      << ",map_dressing_entries=" << g_mapDressingStats.entries
                      << ",map_dressing_streamed_out=" << g_mapDressingStats.streamedOut
                      << ",map_dressing_left_with_ipl=" << g_mapDressingStats.leftWithIpl
                      << ",map_dressing_taken_away=" << g_mapDressingStats.takenAway
                      << ",map_dressing_reinstanced=" << g_mapDressingStats.reinstanced;
        }
        Ctx().log << '\n';
        // hat_look.h: the hat looks recorded.
        Ctx().log << "POSE_V2_HAT_LOOK,phase=record_end,core=" << g_hatLookCore
                  << ",walk=" << g_hatLookWalk << ",props=" << g_hatLookStats.props << ",scenery=" << g_hatLookStats.scenery
                  << ",unread=" << g_hatLookStats.unread << '\n';
        // object_check.h: the scenery's children.
        Ctx().log << "POSE_V2_SCENERY_CHILDREN,phase=record,entries=" << Ctx().clip.sceneryChildren.size()
                  << ",at_start=" << g_sceneryChildStats.atStart << ",left=" << g_sceneryChildStats.left
                  << ",with_frame=" << g_sceneryChildStats.frames << ",visibility_flips=" << g_sceneryChildStats.visibilityFlips
                  << ",from_scenery=" << g_sceneryChildStats.fromScenery << ",full=" << g_sceneryChildStats.full << '\n';
        Ctx().log << "POSE_V2_WEAPON_OBJECTS,from_object=" << g_objectWeaponFromObject
                  << ",unresolved=" << g_objectWeaponUnresolved << '\n';
        // The object burn levels.
        Ctx().log << "POSE_V2_BURN_LEVEL,phase=record_end,enabled=" << (g_objectBurnLevel && replay::burn::Enabled() ? 1 : 0)
                  << ",reads=" << g_objectBurnStats.reads << ",no_block=" << g_objectBurnStats.noBlock
                  << ",tracks=" << g_objectBurnStats.tracks << ",changes=" << g_objectBurnStats.changes
                  << ",scenery_tracks=" << g_objectBurnStats.sceneryTracks << ",scenery_changes=" << g_objectBurnStats.sceneryChanges
                  << ",clip_tracks=" << Ctx().clip.burnLevels.size() << ",clip_scenery_tracks=" << Ctx().clip.sceneryBurnLevels.size()
                  << '\n';
        g_sceneryLive.clear();
        g_sceneryOf.clear();
        g_sceneryPending.clear();
        g_scenerySeen.clear();
        g_sceneryFar.clear();
        ResetPoseV2Loot();
        g_sceneryChildLive.clear();
        g_sceneryChildObjects.clear();
        g_sceneryChildScanned.clear();
        Ctx().log << "POSE_V2_WEAPON_POINTS,full=" << g_poseV2PointScansFull << ",skipped=" << g_poseV2PointScansSkipped
            << '\n';
        // What the weapon pass changed on each recorded
        // weapon after the script's read (before against after, per bone).
        {
            replay::weapon::SetWeaponRecordIds({});
            g_passRecPublished.clear();
            std::uint32_t lines = 0, turned = 0, sampled = 0;
            for (const auto& [recordId, weaponHash] : g_passRecWeapons)
            {
                replay::weapon::WeaponPassRecordStats stats;
                if (!replay::weapon::WeaponPassRecord(recordId, stats))
                    continue;
                sampled += stats.calls ? 1u : 0u;
                turned += stats.changed ? 1u : 0u;
                if (lines >= 48)
                    continue;
                ++lines;
                std::string bones;
                for (std::uint32_t n = 0; n < replay::weapon::kPassBones; ++n)
                    if (stats.boneChanged[n] && bones.size() < 160)
                        bones += (bones.empty() ? "" : " ") + std::to_string(n) + ":" + std::to_string(stats.boneChanged[n]);
                char line[420];
                std::snprintf(line, sizeof(line),
                    "WEAPON_PASS_REC,id=%s,weapon=0x%08x,calls=%u,compared=%u,changed=%u,max_deg=%.1f,worst_bone=%d,bones=%s",
                    replay::entity::IdText(replay::entity::ReplayId::FromPacked(recordId)).c_str(), weaponHash, stats.calls,
                    stats.compared, stats.changed, stats.maxMilliDeg / 1000.0f, stats.worstBone,
                    bones.empty() ? "-" : bones.c_str());
                Ctx().log << line << '\n';
            }
            Ctx().log << "WEAPON_PASS_REC_TOTAL,weapons=" << g_passRecWeapons.size() << ",sampled_after_pass=" << sampled
                      << ",pass_turned=" << turned << ",weapon_pass=" << (g_weaponPassReady ? "installed" : g_weaponPassError.c_str())
                      << '\n';
            g_passRecWeapons.clear();
        }
        Ctx().log << "WEAPON_BONES_TOTAL,phase=rec,enabled=" << (g_weaponBonePose ? 1 : 0) << ",places=" << g_weaponBonePlaces
                  << ",watched=" << g_weaponBoneStats.watched << ",full=" << g_weaponBoneStats.full << ",reads=" << g_weaponBoneStats.reads
                  << ",deferred=" << g_weaponBoneStats.deferred << ",moved=" << g_weaponBoneStats.moved
                  << ",posed=" << g_weaponBoneStats.posed << ",no_place=" << g_weaponBoneStats.noPlace
                  << ",refused=" << g_weaponBoneStats.refused << ",ended=" << g_weaponBoneStats.ended
                  << ",peak=" << g_weaponBoneStats.peak
                  // Continuous capture, the player apart, the cost per tick
                  << ",player_watched=" << g_weaponBoneStats.playerWatched << ",player_posed=" << g_weaponBoneStats.playerPosed
                  << ",player_no_place=" << g_weaponBoneStats.playerNoPlace << ",ticks=" << g_weaponBoneStats.ticks
                  << ",tick_us_avg=" << (g_weaponBoneStats.ticks ? g_weaponBoneStats.tickUsSum / g_weaponBoneStats.ticks : 0)
                  << ",tick_us_max=" << g_weaponBoneStats.tickUsMax << ",posed_max_tick=" << g_weaponBoneStats.posedMaxPerTick
                  << ",mode=continuous" << '\n';
        Ctx().log << "AMMO_HAND_TOTAL,enabled=" << (g_ammoScanEveryTick ? 1 : 0) << ",ticks=" << g_ammoStats.ticks
                  << ",fresh=" << g_ammoStats.fresh << ",asked=" << g_ammoStats.asked << ",registered=" << g_ammoStats.registered
                  << ",expired=" << g_ammoStats.expired << ",overflow=" << g_ammoStats.overflow << '\n'; // (C)
        Ctx().log << "POSE_V2_OBJECT_POSES,places=" << ObjectPlacesLimit()
                  << ",mode=shared,peak=" << g_objectPoseStats.peak
                  << ",started=" << g_objectPoseStats.started << ",late=" << g_objectPoseStats.late
                  << ",continued=" << g_objectPoseStats.continued
                  << ",skeleton_changed=" << g_objectPoseStats.skeletonChanged
                  << ",no_place_tries=" << g_objectPoseStats.noPlaceTries
                  << ",spawned=" << g_objectPoseStats.spawned << '\n';
        LogPoseV2SpawnedSummary();
        // The bone watch of the props without a pose (object_check.h
        // AfterPropBoneRead): its cost (reads, deferred) and its verdicts.
        Ctx().log << "POSE_V2_PROP_BONE_WATCH,watched=" << g_propBoneStats.watched << ",full=" << g_propBoneStats.full
                  << ",evicted=" << g_propBoneStats.evicted
                  << ",peak=" << g_propBoneStats.peak << ",cap=" << replay::objcheck::kMaxPropBoneWatches
                  << ",watching=" << CountPropBoneWatches() << ",reads=" << g_propBoneStats.reads
                  << ",deferred=" << g_propBoneStats.deferred << ",baselines=" << g_propBoneStats.baselines
                  << ",appeared=" << g_propBoneStats.appeared << ",rebuilt=" << g_propBoneStats.rebuilt
                  << ",moved=" << g_propBoneStats.moved << ",posed=" << g_propBoneStats.posed
                  << ",waiting=" << g_propBoneStats.waiting << ",refused=" << g_propBoneStats.refused << '\n';
        g_worldSnapshot.clear();
        g_worldTaken.clear();
        g_poseV2WeaponEvidenceLines = 0;
    }
    // AmmoScanEveryTick: the cartridge a reload takes into the hand is an
    // object attached to the recorded ped, found by the 200 ms scan
    // (ScanPoseV2AttachedObjects): it entered the clip 88 to 250 ms after its
    // life began, so the first part of the reload drew an empty hand [log].
    // Every tick while a recorded ped holds a weapon (a real weapon at point
    // 0), the objects that are new in the pool listing since the last tick (one
    // listing a frame, cached) are asked for their parent, again each tick for
    // 250 ms while they have none, and one attached to such a ped enters the
    // recording as the scan would enter it (an attached object of its owner).
    // Only new objects are asked: a few natives a tick, not the scan's walk.

    void ScanPoseV2HandAmmo()
    {
        if (!g_ammoScanEveryTick)
            return;
        static std::vector<Entity> holders;
        holders.clear();
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            if (weapon.attachPoint == 0 && weapon.weaponHash && weapon.parent)
                holders.push_back(weapon.parent);
        if (holders.empty())
        {
            g_ammoPrevHandles.clear();
            g_ammoPending.clear();
            return;
        }
        REPLAY_PROFILE_ZONE("rec.hand_ammo");
        std::sort(holders.begin(), holders.end());
        holders.erase(std::unique(holders.begin(), holders.end()), holders.end());
        ++g_ammoStats.ticks;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        static int handles[1024];
        const int count = (std::min)((std::max)(0, ListObjects(handles, 1024)), 1024);
        static std::vector<int> listed;
        listed.assign(handles, handles + count);
        std::sort(listed.begin(), listed.end());
        // The first tick with a holder only takes the listing as the baseline.
        // Before, every object of the listing not recorded yet (hundreds in a
        // town) became pending and was asked for its parent every tick for 250
        // ms, each time a ped drew a weapon after none held one; a cartridge
        // made in that same tick is still found by the 200 ms scan. New objects
        // per tick are capped too.
        const bool first = g_ammoPrevHandles.empty();
        std::uint32_t freshThisTick = 0;
        for (const int handle : listed)
        {
            if (first)
                break;
            if (std::binary_search(g_ammoPrevHandles.begin(), g_ammoPrevHandles.end(), handle))
                continue;
            if (Ctx().recorder.KnowsObject(handle))
                continue;
            if (freshThisTick >= kAmmoFreshPerTick || g_ammoPending.size() >= kAmmoPendingMax)
            {
                ++g_ammoStats.overflow;
                continue;
            }
            if (std::none_of(g_ammoPending.begin(), g_ammoPending.end(), [&](const AmmoPending& p) { return p.object == handle; }))
            {
                g_ammoPending.push_back({handle, nowMs});
                ++g_ammoStats.fresh;
                ++freshThisTick;
            }
        }
        g_ammoPrevHandles.swap(listed);
        for (std::size_t n = 0; n < g_ammoPending.size();)
        {
            const AmmoPending pending = g_ammoPending[n];
            if (!replay::entities::Exists(pending.object) || nowMs - pending.firstMs > kAmmoAskMs)
            {
                g_ammoStats.expired += replay::entities::Exists(pending.object) ? 1u : 0u;
                g_ammoPending.erase(g_ammoPending.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            ++g_ammoStats.asked;
            const Entity parent = ENTITY::GET_ENTITY_ATTACHED_TO(pending.object);
            if (!parent)
            {
                ++n;
                continue;
            }
            g_ammoPending.erase(g_ammoPending.begin() + static_cast<std::ptrdiff_t>(n));
            if (!std::binary_search(holders.begin(), holders.end(), parent) || IsPoseV2KnownObject(pending.object))
                continue;
            const replay::recorder::RecordedEntity* owner = Ctx().recorder.FindByHandle(parent);
            if (!owner || owner->vehicle)
                continue;
            RetirePoseV2PropOf(pending.object, "attached");
            if (!RegisterPoseV2Weapon(pending.object, *owner, replay::recorder::kAttachedObjectPoint, 0))
                continue;
            Ctx().recorder.weapons.back().seen = true;
            ++g_ammoStats.registered;
            if (g_ammoStats.logs < 24)
            {
                ++g_ammoStats.logs;
                Ctx().log << "AMMO_HAND_TICK,ped=" << replay::entity::IdText(owner->id) << ",handle=" << pending.object
                          << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(pending.object)) << std::dec
                          << ",age_ms=" << (nowMs - pending.firstMs) << ",time_ms=" << nowMs << '\n';
            }
        }
    }

    bool RegisterGorePiece(Object object, Entity ownerHandle)
    {
        // A piece the spawned object watch took first (a free
        // object recorded by model, Projectile origin) is recorded as the
        // gore piece it is.
        if (object)
        {
            for (std::size_t index = Ctx().recorder.props.size(); index-- > 0;)
            {
                const replay::recorder::RecordedProp& prop = Ctx().recorder.props[index];
                const replay::entity::CreatePacket* create = prop.object == object
                    ? Ctx().clip.FindCreate(prop.id.Packed()) : nullptr;
                if (create && create->propOrigin == replay::entity::PropOrigin::Projectile)
                    RetirePoseV2Prop(index, "gore_piece");
            }
        }
        if (!object || IsPoseV2KnownObject(object))
            return false;
        // A piece of a map object (the railing shot through next to the ped) is
        // that object's, recorded by the world paths; its model never becomes a
        // gore model.
        {
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            bool worldPiece = false;
            bool worldBreak = false;
            if (!IsPedGoreModel(model))
            {
                for (const WorldTaken& taken : g_worldTaken)
                    worldPiece = worldPiece || taken.model == model;
                for (const Object broken : g_fragBrokenObjects)
                    worldBreak = worldBreak ||
                        (replay::entities::Exists(broken) && static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(broken)) == model);
                for (const auto& [snapObject, snap] : g_worldSnapshot)
                    if (!worldPiece && snap.model == model && Distance2(at, snap.position) <= 8.0f * 8.0f)
                        worldPiece = true;
            }
            if (!replay::objcheck::GorePieceCandidate(worldPiece, worldBreak))
            {
                ++g_goreWorldPiecesRefused;
                if (g_goreWorldPiecesRefused <= 16)
                    Ctx().log << "POSE_V2_GORE_PIECE_REFUSED,why=" << (worldBreak ? "world_break_model" : "world_object_model")
                              << ",handle=" << object << ",model=0x" << std::hex << model << std::dec
                              << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
                return false;
            }
        }
        g_goreModels.insert(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)));
        for (const replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
        {
            if (owner.handle != ownerHandle)
                continue;
            const std::size_t before = Ctx().recorder.props.size();
            RegisterPoseV2Prop(object, owner.id, owner.handle, replay::entity::PropOrigin::GorePiece, 0, "gore_piece");
            return Ctx().recorder.props.size() > before;
        }
        return false;
    }

    replay::entity::ReplayId RegisterRopeEndObject(std::uint64_t address, replay::entity::ReplayId owner, const char*& status)
    {
        status = "not_object";
        if (!address)
            return {};
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        Object object = 0;
        for (int slot = 0; slot < count && !object; ++slot)
            if (objects[slot] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == address)
                object = objects[slot];
        if (!object || !replay::entities::Exists(object))
            return {};
        if (IsPoseV2KnownObject(object))
        {
            status = "known";
            return {};
        }
        // A static map object that stood there before the recording (a
        // hitching rail holding the reins) stays the world's.
        if (std::binary_search(Ctx().clip.objectsBefore.begin(), Ctx().clip.objectsBefore.end(), object) &&
            ENTITY::IS_ENTITY_STATIC(object))
        {
            status = "static_map_object";
            return {};
        }
        // The prop's owner: the recorded ped, or the holder of the recorded
        // weapon (the fishing rod) at the rope's other end.
        const replay::recorder::RecordedEntity* ped = nullptr;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            if (entity.id == owner)
                ped = &entity;
        if (!ped)
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                if (weapon.id == owner)
                    ped = Ctx().recorder.FindByHandle(weapon.parent);
        if (!ped)
        {
            status = "no_owner";
            return {};
        }
        const std::size_t before = Ctx().recorder.props.size();
        RegisterPoseV2Prop(object, ped->id, ped->handle, replay::entity::PropOrigin::Projectile, 0, "rope_end");
        if (Ctx().recorder.props.size() == before)
        {
            status = "not_registered";
            return {};
        }
        status = "registered";
        return Ctx().recorder.props.back().id;
    }

    // See the
    // declarations in replay_objects.h.
    bool TakeVehiclePartsUrgent(replay::entity::ReplayId vehicle)
    {
        if (!g_vehiclePartsUrgent.erase(vehicle.Packed()))
            return false;
        ++g_wheelPieceStats.urgent;
        return true;
    }
}
