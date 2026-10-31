// replay_objects_playback.cpp: the playback: begin and end, the object pose and weapons update, wheels, vehicle and frag pieces, their release, the kept wheels, the bench probes.
// Split from replay_objects.cpp (moved code); shared internals in replay_objects_internal.h.

#include "replay_objects_internal.h"

namespace replay::objects
{

    // The fragment instance's constructor writes its vtable: a clone's
    // instance is checked against it before its fragment type is read
    // (GuardedGroupToChild).

    // Defined with the fragment breaks' playback, used by the
    // pieces' shaping (UpdatePoseV2Props).
    int GuardedGroupGone(std::uint64_t entity, std::uint16_t group);
    int GuardedGroupToChild(std::uint64_t entity, std::uintptr_t fragVtable, std::uint16_t group);
    namespace
    {
    struct WheelChildStats
    {
        std::uint32_t released = 0; // objects a wheel child had, given up
        std::uint32_t keptLogs = 0;
        std::uint32_t childLogs = 0;
    } g_wheelChildStats;
    // The pose records of the animated world objects and of the objects on a
    // vehicle or train car, met by the playhead and armed on their puppet. A
    // record met but never armed by the end is an animated_unposed verdict
    // (phase=play): the drawer would play closed.
    struct AnimatedPlayTally
    {
        std::set<std::uint32_t> worldMet, worldArmed;
        std::set<std::uint32_t> vehicleMet, vehicleArmed;
    } g_animatedPlay;
    }

    void PlayPoseV2ObjectPoseBody(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
            !replay::entities::Exists(puppet.clone))
        {
            return;
        }
        const std::uint32_t recordId = puppet.packet.id.Packed();
        // A pose that started after the create has its bone count
        // only in the clip's create (object_pose.h PlaybackBones).
        const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
        const std::uint32_t recordedBones =
            replay::object_pose::PlaybackBones(puppet.packet.boneCount, create ? create->boneCount : 0);
        if (recordedBones < replay::object_pose::kMinBones)
            return;
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        if (!replay::pose::capture::TrackSpan(recordId, firstMs, lastMs))
            return; // the clip has no pose for it
        // The animated play tally (met here, armed below).
        const bool worldAnimated = puppet.packet.kind == replay::entity::EntityKind::Prop &&
            puppet.packet.propOrigin == replay::entity::PropOrigin::WorldObject;
        const bool vehicleChild = puppet.packet.kind == replay::entity::EntityKind::Weapon &&
            g_vehicleChildRecords.count(recordId) != 0;
        if (worldAnimated && timeMs >= firstMs)
            g_animatedPlay.worldMet.insert(recordId);
        if (vehicleChild && timeMs >= firstMs)
            g_animatedPlay.vehicleMet.insert(recordId);
        const auto logLine = [&](const char* status, std::uint32_t bones) {
            if (g_poseV2ObjectPlayLogs >= 48)
                return;
            ++g_poseV2ObjectPlayLogs;
            Ctx().log << "POSE_V2_OBJECT_POSE,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                      << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec << ",status=" << status
                      << ",bones=" << bones << ",track_bones=" << recordedBones
                      << ",slot=" << puppet.overrideSlot << ",track_ms=" << firstMs << "-" << lastMs
                      << ",playhead_ms=" << timeMs << '\n';
        };
        // The drawn check of an armed object, before this
        // tick's publish and write (the slot still holds last tick's pose).
        if (g_objectDrawnCheck && puppet.overrideSlot >= 0)
        {
            REPLAY_PROFILE_ZONE("play.pose.object_drawn");
            if (replay::pose::capture::ReadOverrideDrawn(static_cast<std::uint32_t>(puppet.overrideSlot),
                    g_drawnExpected, g_drawnLocals) &&
                g_drawnExpected.size() == g_drawnLocals.size() && puppet.armedBones &&
                (g_drawnLocals.size() == puppet.armedBones || g_drawnLocals.size() == puppet.armedBones * 2u))
            {
                NoteDrawn(puppet, "armed", g_drawnExpected.data(), g_drawnLocals.data(), puppet.armedBones,
                    g_drawnLocals.size() == puppet.armedBones * 2u, puppet.armedSkeleton, puppet.overrideSlot, timeMs);
            }
        }
        if (puppet.overrideSlot < 0)
        {
            replay::skeleton::ResolvedPoseSkeleton resolved{};
            bool ok = replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved);
            const std::uint64_t askKey = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(puppet.clone)) << 32) |
                recordId;
            PoseV2SkeletonAsk& ask = g_poseV2ObjectSkeletonAsked[askKey];
            const std::uint64_t nowTick = GetTickCount64();
            if (replay::object_pose::ShouldAskSkeletonAgain(recordedBones, ok, ask.asks, nowTick, ask.nextMs))
            {
                // A fresh CREATE_OBJECT has no skeleton
                // until something asks for one; the object skeleton native
                // builds it from the model's skeleton data (CreateSkeleton),
                // on the replay object only.
                ++ask.asks;
                ask.nextMs = nowTick + replay::object_pose::kSkeletonAskIntervalMs;
                OBJECT::_0x491439AEF410A2FC(puppet.clone);
                resolved = {};
                ok = replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved);
                if (g_poseV2ObjectPlayLogs < 48)
                {
                    ++g_poseV2ObjectPlayLogs;
                    Ctx().log << "POSE_V2_OBJECT_POSE,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec
                              << ",status=" << (ok ? "skeleton_built" : "skeleton_not_built")
                              << ",ask=" << ask.asks
                              << ",bones=" << resolved.boneCount << ",track_bones=" << recordedBones
                              << ",fragment_path=" << (resolved.usedFragmentPath ? 1 : 0)
                              << ",getter=" << replay::skeleton::walk::GetterKindName(
                                     static_cast<replay::skeleton::walk::GetterKind>(resolved.getterKind))
                              << ",walk=" << (ok ? std::string("-") : replay::skeleton::DescribePoseSkeletonWalk(puppet.clone))
                              << ",playhead_ms=" << timeMs << '\n';
                }
            }
            if (!ok || resolved.boneCount != recordedBones)
            {
                if (g_poseV2ObjectPlayNoted.insert(recordId).second)
                    logLine(ok ? "bones_differ" : "skeleton_unresolved", resolved.boneCount);
                return;
            }
            // still_pose_core.h: an attached object whose
            // record never changed is left to the engine, posed by its attach
            // as live; decided once per record, one line each.
            if (puppet.stillRecord != recordId)
            {
                puppet.stillRecord = recordId;
                puppet.stillLocals.clear();
                const replay::tpose::TrackSignature& signature = StillSignature(recordId);
                const bool attached = puppet.packet.kind == replay::entity::EntityKind::Weapon;
                const replay::stillpose::Decision decision = replay::stillpose::Decide(
                    true /* the still rule, always on */, attached, signature.parsed, replay::tpose::NeverAnimated(signature));
                puppet.stillSkip = decision == replay::stillpose::Decision::SkipStill;
                if (decision != replay::stillpose::Decision::Arm && g_stillLines < 64)
                {
                    ++g_stillLines;
                    Ctx().log << "POSE_V2_OBJECT_STILL,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec
                              << ",decision=" << replay::stillpose::DecisionName(decision)
                              << ",rule=1,attached=" << (attached ? 1 : 0)
                              << ",attached_now=" << (puppet.attachedToParent ? 1 : 0)
                              << ",frames=" << signature.frames << ",pose_frames=" << signature.poseFrames
                              << ",span_ms=" << replay::tpose::SpanMs(signature)
                              << ",bones=" << resolved.boneCount
                              << ",getter=" << replay::skeleton::walk::GetterKindName(
                                     static_cast<replay::skeleton::walk::GetterKind>(resolved.getterKind))
                              << ",fragment_path=" << (resolved.usedFragmentPath ? 1 : 0)
                              << ",drawn_check=" << (g_objectDrawnCheck ? 1 : 0) << ','
                              << ObjectClassText(resolved.entity) << ",playhead_ms=" << timeMs << '\n';
                }
            }
            if (puppet.stillSkip)
            {
                // Left to the engine on purpose:
                // not a drawer that failed to arm.
                if (worldAnimated)
                    g_animatedPlay.worldArmed.insert(recordId);
                if (vehicleChild)
                    g_animatedPlay.vehicleArmed.insert(recordId);
                if (g_objectDrawnCheck)
                {
                    REPLAY_PROFILE_ZONE("play.pose.object_drawn");
                    CheckStillDrawn(puppet, resolved, recordId, timeMs);
                }
                return;
            }
            if (puppet.armDeadline == 0)
                puppet.armDeadline = GetTickCount64() + 3000;
        }
        const bool wasArmed = puppet.overrideSlot >= 0;
        Ctx().hooks->RefreshOverride(puppet, timeMs);
        if (puppet.overrideSlot < 0)
            return;
        if (worldAnimated)
            g_animatedPlay.worldArmed.insert(recordId);
        if (vehicleChild)
            g_animatedPlay.vehicleArmed.insert(recordId);
        const auto slot = static_cast<std::uint32_t>(puppet.overrideSlot);
        if (!wasArmed)
        {
            replay::pose::capture::SetOverrideLate(slot, true);
            logLine("armed", puppet.armedBones);
        }
        if (!replay::pose::capture::SampleTrack(puppet.armedRecordId, timeMs, g_poseV2ObjectSample))
            return;
        replay::pose::capture::PublishOverrideSample(puppet.overrideSlot, g_poseV2ObjectSample, puppet.armedBones);
        replay::pose::capture::ApplyOverrideNow(slot);
    }

    void UpdatePoseV2Weapons(std::uint32_t timeMs, std::uint32_t& creates)
    {
        REPLAY_PROFILE_ZONE("play.weapons");
        static std::vector<replay::entity::Attachment> attachments;
        Ctx().clip.lifecycle.AttachmentsAt(timeMs, attachments);
        const ULONGLONG now = GetTickCount64();
        PublishPoseV2StrapOwned();
        std::uint32_t copyChecks = 0; // engine copy checks this frame
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Weapon ||
                puppet.state == replay::playback::PuppetState::Failed)
            {
                continue;
            }
            const auto attachment = std::find_if(attachments.begin(), attachments.end(),
                [&](const replay::entity::Attachment& candidate) {
                    return candidate.child == puppet.packet.id &&
                        candidate.kind == replay::entity::AttachmentKind::WeaponPoint;
                });
            const replay::playback::Puppet* parent = attachment != attachments.end()
                ? Ctx().puppets.Find(attachment->parent)
                : nullptr;
            const bool parentReady = parent && parent->state == replay::playback::PuppetState::Ready &&
                parent->clone && replay::entities::Exists(parent->clone);
            // An object on a cart or in a train car.
            const bool vehicleParent = parent && (parent->packet.kind == replay::entity::EntityKind::Vehicle ||
                parent->packet.kind == replay::entity::EntityKind::TrainCar);
            if (vehicleParent)
                g_vehicleChildRecords.insert(puppet.packet.id.Packed());
            const replay::entity::RelativeTrack* track =
                Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed());
            replay::entity::TransformSample sample;
            const bool sampled = track && track->Sample(timeMs, sample, &Ctx().clip.cuts); // cut-aware
            replay::pose::WorldTransform world{};
            if (parentReady && sampled)
            {
                world = replay::entity::ComposeTransform(
                    ReadEntityTransform(parent->clone), sample.relative);
            }

            puppet.waitingForParent = !parentReady;
            // One of the vehicle's own wheels (vehicle_pieces.h wheel children): no
            // object of its own while attached, the vehicle puppet draws the wheel
            // (KeptVehicleWheels keeps it on the puppet).
            const replay::vehicle_pieces::WheelModels* wheelModels =
                parentReady && !puppet.packet.weaponHash ? PoseV2VehicleWheelModels(*parent) : nullptr;
            if (wheelModels && replay::vehicle_pieces::IsVehicleWheelChild(puppet.packet.modelHash, *wheelModels))
            {
                const std::uint32_t key = puppet.packet.id.Packed();
                if (g_wheelChildLogged.insert(key).second && g_wheelChildStats.childLogs < 64)
                {
                    ++g_wheelChildStats.childLogs;
                    const std::uint16_t bits = PoseV2WheelChildBits(key, puppet.packet.modelHash, *wheelModels);
                    Ctx().log << "POSE_V2_VEHICLE_WHEEL_CHILD,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",vehicle=" << replay::entity::IdText(parent->packet.id) << ",model=0x" << std::hex
                              << puppet.packet.modelHash << ",wheels=0x" << bits << std::dec
                              << ",draw=vehicle,playhead_ms=" << timeMs << '\n';
                }
                if (puppet.clone)
                {
                    Ctx().hooks->ReleaseOverride(puppet);
                    if (!replay::objects::ReturnWheelObject(puppet.clone))
                        replay::pose::playback::DeleteObjectPuppet(puppet.clone);
                    puppet.clone = 0;
                    puppet.state = replay::playback::PuppetState::Loading;
                    ++g_wheelChildStats.released;
                }
                puppet.waitingForParent = true; // not a body the start waits for
                puppet.stateSince = now;
                continue;
            }
            if (puppet.state == replay::playback::PuppetState::Loading)
            {
                const bool loaded = (puppet.packet.weaponHash
                    ? replay::pose::playback::RequestWeaponAsset(puppet.packet.weaponHash)
                    : replay::pose::playback::RequestModel(puppet.packet.modelHash)) &&
                    replay::pose::playback::RequestWeaponComponents(puppet.packet.weaponComponents);
                // Waiting for the parent puppet is not a failure: the
                // weapon is built once its ped exists at the playhead.
                if (!parentReady)
                {
                    puppet.stateSince = now;
                    continue;
                }
                if (!loaded || !sampled)
                {
                    if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        Ctx().hooks->LogPuppet(puppet, !loaded ? "model_timeout" : "no_track", timeMs);
                    }
                    continue;
                }
                if (creates >= replay::playback::kMaxCreatesPerFrame || now < puppet.nextCreateAt)
                    continue;
                // A carried wheel is the wheel object a vehicle puppet gave,
                // not a fresh model.
                Object wheelObject = 0;
                if (PoseV2WheelCopy(puppet, world, wheelObject, now, timeMs) == replay::vehicle_pieces::WheelCopy::Wait)
                    continue;
                ++creates;
                ++puppet.createAttempts;
                puppet.clone = wheelObject ? wheelObject : replay::pose::playback::CreateWeaponPuppet(
                    puppet.packet.weaponHash, puppet.packet.modelHash, world);
                puppet.conditionApplied = -1;
                puppet.strapApplied = -1;
                puppet.weaponLookCheckAt = puppet.weaponLookWaitUntil = 0;
                puppet.weaponLookReapplied = 0;
                ApplyPoseV2WeaponComponents(puppet);
                if (!puppet.clone)
                {
                    puppet.nextCreateAt = now + replay::playback::kCreateRetryMs;
                    if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        Ctx().hooks->LogPuppet(puppet, "create_failed", timeMs);
                    }
                    continue;
                }
                // A weapon puppet is a weapon object too
                // (_CREATE_WEAPON_OBJECT), and it had no pickup light
                // suppression: only the loose ones did. Every gun,
                // knife and lasso of a ped puppet, the holstered ones kept
                // invisible included, could carry the gameplay pickup glow at
                // dusk (a town clip at 19:10, 219 weapon
                // puppets). The replay's objects are props.
                SuppressPickupLook(puppet.clone);
                puppet.state = replay::playback::PuppetState::Ready;
                Ctx().hooks->LogPuppet(puppet, "created", timeMs);
            }
            if (!puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            const bool inTrack = track && track->Span(firstMs, lastMs) &&
                timeMs >= firstMs && timeMs <= lastMs + kIntervalMs;
            const bool visible = parentReady && sampled && inTrack && sample.visible &&
                ENTITY::IS_ENTITY_VISIBLE(parent->clone);
            replay::pose::playback::SetCloneVisible(puppet.clone, visible);
            if (parentReady && sampled)
            {
                AttachPoseV2Weapon(puppet, *parent, sample.relative, world, timeMs);
            }
            if (vehicleParent && parentReady && visible)
                HidePoseV2EngineCopy(puppet, parent->clone, copyChecks, now);
            ApplyPoseV2WeaponLook(puppet, timeMs);
            PlayPoseV2ObjectPose(puppet, timeMs);
        }
    }
    namespace
    {
    struct VehicleFragPieceStats
    {
        std::uint32_t early = 0;   // broken off at the group's break
        std::uint32_t adopted = 0; // taken by their piece
        std::uint32_t released = 0;
    } g_vehicleFragPieceStats;
    }
    std::vector<PoseV2WheelObject> g_wheelObjects;
    namespace
    {
    std::unordered_set<std::uint32_t> g_wheelModels;           // wheel models met (puppets' draft data, kept wheels)
    std::unordered_set<std::uint32_t> g_clipPlainObjectModels; // models of the clip's object puppets without a weapon
    std::unordered_set<Entity> g_wheelModelsRead;              // vehicle puppets whose wheel models were read
    std::unordered_map<std::uint32_t, WaitClock> g_wheelCopyWaitSince; // packed id
    bool g_clipHasWheelPieces = false;
    bool g_wheelObjectsOpen = false;
    struct WheelPlayStats
    {
        std::uint32_t kept = 0;          // returned by the part native and kept
        std::uint32_t deleted = 0;       // returned by the part native, no use in the clip
        std::uint32_t piecesFromKept = 0;
        std::uint32_t piecesNative = 0;  // broken off the vehicle puppet by the piece itself
        std::uint32_t piecesFailed = 0;
        std::uint32_t copiesAdopted = 0;
        std::uint32_t copiesPlain = 0;   // a wheel model made plain (no wheel object free in time)
        std::uint32_t copiesWaited = 0;
        std::uint32_t returned = 0;
        std::uint32_t released = 0;      // free ones deleted when their vehicle was built again
        std::uint32_t gone = 0;          // a kept one the engine removed
        std::uint32_t modelReads = 0;
    } g_wheelPlay;
    std::uint32_t g_wheelPlayLogs = 0;
    }
    std::uint32_t g_wheelPlayheadMs = 0; // the last playhead the object puppets saw (log lines)
    namespace
    {

    void LogPoseV2Wheel(const char* action, const PoseV2WheelObject& wheel, const char* puppet, std::uint32_t timeMs)
    {
        if (g_wheelPlayLogs >= 64)
            return;
        ++g_wheelPlayLogs;
        char line[256];
        std::snprintf(line, sizeof(line),
            "POSE_V2_PLAYBACK_WHEEL,action=%s,vehicle=%s,wheel=%d,handle=%d,model=0x%x,puppet=%s,playhead_ms=%u\n", action,
            replay::entity::IdText(wheel.vehicle).c_str(), wheel.wheel, wheel.object, wheel.model, puppet, timeMs);
        Ctx().log << line;
    }

    void HidePoseV2WheelObject(Object object)
    {
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
        if (ENTITY::IS_ENTITY_ATTACHED(object))
            ENTITY::DETACH_ENTITY(object, FALSE, FALSE);
        ENTITY::SET_ENTITY_INVINCIBLE(object, TRUE);
        ENTITY::SET_ENTITY_COLLISION(object, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(object, TRUE);
        ENTITY::SET_ENTITY_VISIBLE(object, FALSE);
    }

    bool PoseV2WheelObjectAlive(const PoseV2WheelObject& wheel)
    {
        return wheel.object && replay::entities::Exists(wheel.object) &&
            static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(wheel.object)) == wheel.model;
    }

    // A free wheel object: the wheel `wheel` of `vehicle` (wheel >= 0), or
    // any of `model` (wheel < 0). Free ones the engine removed are dropped.
    PoseV2WheelObject* FreePoseV2WheelObject(replay::entity::ReplayId vehicle, int wheel, std::uint32_t model, std::uint32_t prop = 0)
    {
        for (std::size_t n = g_wheelObjects.size(); n-- > 0;)
            if (!g_wheelObjects[n].holder && !PoseV2WheelObjectAlive(g_wheelObjects[n]))
            {
                ++g_wheelPlay.gone;
                g_wheelObjects.erase(g_wheelObjects.begin() + static_cast<std::ptrdiff_t>(n));
            }
        // A wheel that came off for a loose wheel is that one's (two rear wheels
        // leave together: each takes its own side's).
        if (prop)
            for (PoseV2WheelObject& entry : g_wheelObjects)
                if (!entry.holder && entry.forProp == prop && entry.model == model)
                    return &entry;
        for (PoseV2WheelObject& entry : g_wheelObjects)
            if (!entry.holder && (wheel >= 0 ? (entry.vehicle == vehicle && entry.wheel == wheel) : entry.model == model) &&
                (!entry.forProp || entry.forProp == prop))
                return &entry;
        return nullptr;
    }

    // The wheel models of a vehicle puppet (rung 2 on the replay's own
    // vehicle, the layout proven by the recording's check), once per puppet.
    void NotePoseV2PuppetWheelModels(Entity vehicleClone)
    {
        if (!vehicleClone || !g_wheelModelsRead.insert(vehicleClone).second)
            return;
        replay::vehicle_pieces::WheelRead read;
        const char* error = ReadPoseV2WheelObject(vehicleClone, 0, read);
        if (error && *error)
            return;
        ++g_wheelPlay.modelReads;
        if (read.frontModel)
            g_wheelModels.insert(read.frontModel);
        if (read.rearModel)
            g_wheelModels.insert(read.rearModel);
    }
    }

    // A plain object puppet (no weapon) of a wheel model: a free wheel object
    // of the model is adopted (`out`, set up as CreateWeaponPuppet makes its
    // objects), or it waits for one a short while (WheelCopyStep), or it is
    // made plain as before. A fragment piece (noWait) never waits: no free
    // wheel now, none comes for it (PoseV2FragPieceFromParent had no parent).
    replay::vehicle_pieces::WheelCopy PoseV2WheelCopy(const replay::playback::Puppet& puppet,
        const replay::pose::WorldTransform& at, Object& out, ULONGLONG now, std::uint32_t timeMs, bool noWait)
    {
        using replay::vehicle_pieces::WheelCopy;
        out = 0;
        if (puppet.packet.weaponHash || puppet.packet.propOrigin == replay::entity::PropOrigin::VehiclePiece ||
            g_wheelModels.empty() || !g_wheelModels.count(puppet.packet.modelHash))
            return WheelCopy::Plain;
        const std::uint32_t key = puppet.packet.id.Packed();
        // A wheel that left its vehicle: its object comes off the vehicle puppet
        // now, not at the vehicle's turn later in the tick.
        if (puppet.packet.propOrigin == replay::entity::PropOrigin::DroppedWeapon)
            if (const replay::playback::Puppet* owner = Ctx().puppets.Find(puppet.packet.owner))
                if (owner->packet.kind == replay::entity::EntityKind::Vehicle && owner->state == replay::playback::PuppetState::Ready)
                    LeftVehicleWheels(*owner, timeMs);
        PoseV2WheelObject* free = FreePoseV2WheelObject({}, -1, puppet.packet.modelHash, puppet.packet.id.Packed());
        const bool firstTry = !g_wheelCopyWaitSince.count(key);
        const ULONGLONG waited = WaitedMs(g_wheelCopyWaitSince, key, now);
        if (firstTry && !free)
            ++g_wheelPlay.copiesWaited;
        const WheelCopy step = replay::vehicle_pieces::WheelCopyStep(true, free != nullptr,
            noWait ? replay::vehicle_pieces::kWheelCopyWaitMs : static_cast<std::uint32_t>(waited));
        if (step == WheelCopy::Wait)
            return step;
        g_wheelCopyWaitSince.erase(key);
        const std::string label = replay::entity::IdText(puppet.packet.id);
        if (step == WheelCopy::Plain)
        {
            ++g_wheelPlay.copiesPlain;
            PoseV2WheelObject none;
            none.model = puppet.packet.modelHash;
            LogPoseV2Wheel("copy_plain", none, label.c_str(), timeMs);
            return step;
        }
        free->holder = key;
        out = free->object;
        HidePoseV2WheelObject(out);
        replay::pose::playback::PlaceClone(out, at);
        ++g_wheelPlay.copiesAdopted;
        LogPoseV2Wheel("copy_adopt", *free, label.c_str(), timeMs);
        return step;
    }
    namespace
    {

    // An object's handle by its address (defined with the fragment pieces).
    Object PoseV2HandleAt(std::uint64_t address);
    }

    bool CreatePoseV2VehiclePiece(replay::playback::Puppet& puppet, bool sampled,
        const replay::entity::TransformSample& sample, std::uint32_t timeMs, std::uint32_t& creates)
    {
        replay::playback::Puppet* owner = Ctx().puppets.Find(puppet.packet.owner);
        if (owner && owner->state == replay::playback::PuppetState::Failed)
        {
            puppet.state = replay::playback::PuppetState::Failed;
            Ctx().hooks->LogPuppet(puppet, "vehicle_piece_owner_failed", timeMs);
            return false;
        }
        if (!sampled && GetTickCount64() - puppet.stateSince > replay::playback::kModelTimeoutMs)
        {
            // No sample at the playhead: nothing to place (and the wheel and
            // door natives must not wait for it).
            puppet.state = replay::playback::PuppetState::Failed;
            Ctx().hooks->LogPuppet(puppet, "no_track", timeMs);
            return false;
        }
        if (!sampled || !owner || owner->packet.kind != replay::entity::EntityKind::Vehicle ||
            owner->state != replay::playback::PuppetState::Ready || !owner->clone ||
            !replay::entities::Exists(owner->clone) || !ENTITY::IS_ENTITY_A_VEHICLE(owner->clone))
            return false;
        if (creates >= replay::playback::kMaxCreatesPerFrame)
            return false;
        ++creates;
        ++puppet.createAttempts;
        const std::uint16_t group = replay::vehicle_pieces::GroupOf(puppet.packet);
        if (replay::vehicle_pieces::IsWheelPiece(group))
        {
            // The wheel object the vehicle puppet gives, made by the engine
            // with its vehicle (the puppet's colours): the one the part native
            // returned and kept, else broken off now (the 4 Hz part track may
            // have the wheel off a sample later); the vehicle puppet keeps that
            // wheel off (PlayPoseV2VehicleParts adds the pieces' wheels to its
            // wanted parts, vehicle_pieces::WheelPieceBits).
            const int wheel = replay::vehicle_pieces::WheelOfPiece(group);
            const std::string label = replay::entity::IdText(puppet.packet.id);
            Object piece = 0;
            const char* how = "kept";
            if (PoseV2WheelObject* kept = FreePoseV2WheelObject(owner->packet.id, wheel, 0))
            {
                kept->holder = puppet.packet.id.Packed();
                piece = kept->object;
                ++g_wheelPlay.piecesFromKept;
                LogPoseV2Wheel("piece_kept", *kept, label.c_str(), timeMs);
            }
            else if (wheel < replay::vehicle_pieces::kWheels)
            {
                how = "native";
                Entity broken = VEHICLE::_BREAK_OFF_VEHICLE_WHEEL(owner->clone, wheel);
                if (broken && broken != owner->clone && replay::entities::Exists(broken) && ENTITY::IS_ENTITY_AN_OBJECT(broken))
                {
                    piece = broken;
                    PoseV2WheelObject entry;
                    entry.object = piece;
                    entry.vehicle = owner->packet.id;
                    entry.wheel = wheel;
                    entry.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(piece));
                    entry.holder = puppet.packet.id.Packed();
                    g_wheelObjects.push_back(entry);
                    g_wheelModels.insert(entry.model);
                    ++g_wheelPlay.piecesNative;
                    LogPoseV2Wheel("piece_native", entry, label.c_str(), timeMs);
                }
                if (owner->partsClone != owner->clone)
                {
                    owner->partsClone = owner->clone;
                    owner->partsApplied = 0;
                }
                owner->partsApplied = static_cast<std::uint16_t>(owner->partsApplied | (1u << wheel));
            }
            if (g_vehiclePieceLogs < 64)
            {
                ++g_vehiclePieceLogs;
                Ctx().log << "POSE_V2_PLAYBACK_VEHICLE_PIECE,id=" << label << ",vehicle=" << replay::entity::IdText(puppet.packet.owner)
                    << ",group=" << group << ",wheel=" << wheel << ",result=wheel_" << how << ",object=" << piece
                    << ",vehicle_tint=" << VEHICLE::_GET_VEHICLE_TINT(owner->clone) << ",playhead_ms=" << timeMs << '\n';
            }
            if (!piece)
            {
                ++g_wheelPlay.piecesFailed;
                puppet.state = replay::playback::PuppetState::Failed;
                Ctx().hooks->LogPuppet(puppet, "vehicle_wheel_failed", timeMs);
                return false;
            }
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
            ENTITY::SET_ENTITY_INVINCIBLE(piece, TRUE);
            ENTITY::SET_ENTITY_COLLISION(piece, FALSE, FALSE);
            ENTITY::FREEZE_ENTITY_POSITION(piece, TRUE);
            replay::pose::playback::PlaceClone(piece, sample.relative);
            puppet.clone = piece;
            puppet.state = replay::playback::PuppetState::Ready;
            Ctx().hooks->LogPuppet(puppet, "created_vehicle_wheel", timeMs);
            return true;
        }
        std::uint64_t entity = 0;
        Object piece = 0;
        replay::frag::VehicleGroupResult result = replay::frag::VehicleGroupResult::Deleted;
        // Broken off already at its group's own break (KeepVehicleFragPiece).
        const auto early = g_vehicleFragPieces.find({puppet.packet.owner.Packed(), group});
        if (early != g_vehicleFragPieces.end() && replay::entities::Exists(early->second))
        {
            piece = early->second;
            g_vehicleFragPieces.erase(early);
            ++g_vehicleFragPieceStats.adopted;
        }
        else
            result = replay::frag::BreakOffVehicleGroup(
                reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(owner->clone)), group, entity);
        if (entity)
            piece = PoseV2HandleAt(entity); // the pools first: the listing was walked before the piece existed
        if (g_vehiclePieceLogs < 64)
        {
            ++g_vehiclePieceLogs;
            Ctx().log << "POSE_V2_PLAYBACK_VEHICLE_PIECE,id=" << replay::entity::IdText(puppet.packet.id)
                << ",vehicle=" << replay::entity::IdText(puppet.packet.owner) << ",group=" << group
                << ",result=" << replay::frag::VehicleGroupResultText(result) << ",object=" << piece
                << ",playhead_ms=" << timeMs << '\n';
        }
        if (!piece || !replay::entities::Exists(piece))
        {
            // The vehicle does not keep the part: the group goes without a
            // piece (an unlisted piece object is debris the world clear
            // removes).
            if (result != replay::frag::VehicleGroupResult::Deleted)
                replay::frag::DeleteVehicleGroup(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(owner->clone)), group);
            puppet.state = replay::playback::PuppetState::Failed;
            Ctx().hooks->LogPuppet(puppet, "vehicle_piece_failed", timeMs);
            return false;
        }
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
        ENTITY::SET_ENTITY_INVINCIBLE(piece, TRUE);
        ENTITY::SET_ENTITY_COLLISION(piece, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(piece, TRUE);
        replay::pose::playback::PlaceClone(piece, sample.relative);
        puppet.clone = piece;
        puppet.state = replay::playback::PuppetState::Ready;
        Ctx().hooks->LogPuppet(puppet, "created_vehicle_piece", timeMs);
        return true;
    }

    // frag_piece.h: a prop recorded as a loose fragment piece (a board that
    // came off a crate) would otherwise be a whole object of the model
    // following the piece's track: a second crate flipping out of the broken
    // one. The gone children are taken off, so the whole puppet becomes the
    // piece: its recorded root group breaks off as a new object the way the
    // engine made the piece (BreakOffObjectGroup), that object is adopted and
    // placed by the piece's track, the whole one goes, and the groups under the
    // root the piece no longer carried are taken off
    // (BREAK_OBJECT_FRAGMENT_CHILD with deletion, as the breaks). A piece that
    // cannot be made is not drawn (never the whole object). A fresh object
    // whose fragment cannot be read yet waits hidden up to 1 s.
    std::unordered_map<std::uint32_t, Object> g_fragPieceShaped; // prop packed id -> the clone made the piece
    std::unordered_map<std::uint32_t, ULONGLONG> g_fragPieceWaitUntil;
    namespace
    {
    std::uint32_t g_fragPiecesShaped = 0;
    std::uint32_t g_fragPiecesFailed = 0;
    std::uint32_t g_fragPieceShapeLogs = 0;
    constexpr ULONGLONG kFragPieceWaitMs = 1000;
    }

    const replay::FragPiece* FragPieceOf(replay::entity::ReplayId id)
    {
        for (const replay::FragPiece& row : Ctx().clip.fragPieces)
            if (row.id == id)
                return &row;
        return nullptr;
    }
    namespace
    {

    void FailPoseV2FragPiece(replay::playback::Puppet& puppet, const char* why, std::uint32_t timeMs)
    {
        Ctx().hooks->ReleaseOverride(puppet);
        if (puppet.clone && replay::entities::Exists(puppet.clone))
        {
            Object object = puppet.clone;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            ENTITY::DELETE_ENTITY(&object);
        }
        puppet.clone = 0;
        puppet.state = replay::playback::PuppetState::Failed;
        g_fragPieceWaitUntil.erase(puppet.packet.id.Packed());
        ++g_fragPiecesFailed;
        Ctx().hooks->LogPuppet(puppet, why, timeMs);
    }

    // A wheel's fragment piece (a frag piece of a vehicle's wheel model) breaks
    // off the wheel object it came from, as the engine made it there: the
    // nearest wheel object of its model within kFragParentM of where the piece
    // starts, the vehicle puppet's own wheel or the wheel object a loose wheel's
    // puppet holds. The piece is drawn as the engine draws a piece of that wheel
    // and the wheel keeps only the groups that did not come off. Pieces of one
    // wheel due together come off the topmost root first (a root takes
    // everything under it), and a piece of a group below breaks off that piece. Returns 1
    // made (`out`), 0 no parent (the plain path), -1 wait.
    constexpr float kFragParentM = 3.0f;
    constexpr ULONGLONG kFragParentWaitMs = 300;
    std::unordered_map<std::uint32_t, WaitClock> g_fragParentWaitSince;
    }
    // The pieces that waited for their turn this tick (another object of the
    // model came first), tried again in the same tick once an object puppet
    // of the props was made (UpdatePoseV2Props): a chain of pieces the engine
    // broke in one frame comes off in one tick.
    std::unordered_set<std::uint32_t> g_fragOrderWaited;
    bool g_propMadeThisPass = false;
    const std::unordered_set<std::uint32_t>* g_propRetryOnly = nullptr;
    namespace
    {
    struct FragParentStats
    {
        std::uint32_t made = 0;
        std::uint32_t noParent = 0;
        std::uint32_t failed = 0;
        std::uint32_t waited = 0;
        std::uint32_t logs = 0;
    } g_fragParentStats;

    Object PoseV2HandleAt(std::uint64_t address)
    {
        if (!address)
            return 0;
        bool poolReader = false;
        Object found = replay::entities::HandleAtAddress(address, poolReader);
        if (!poolReader)
        {
            static int objects[1024];
            const int count = (std::min)(ListObjects(objects, 1024), 1024);
            for (int slot = 0; slot < count && !found; ++slot)
                if (objects[slot] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == address)
                    found = objects[slot];
        }
        return found && replay::entities::Exists(found) ? found : 0;
    }
    }

    int PoseV2FragPieceFromParent(replay::playback::Puppet& puppet, const replay::pose::WorldTransform& at, ULONGLONG now,
        std::uint32_t timeMs, Object& out)
    {
        out = 0;
        if (puppet.packet.weaponHash || puppet.packet.propOrigin == replay::entity::PropOrigin::VehiclePiece)
            return 0;
        const replay::FragPiece* row = FragPieceOf(puppet.packet.id);
        if (!row || row->groups.empty())
            return 0;
        const std::uint32_t model = puppet.packet.modelHash;
        // Where the piece left its wheel: its first sample (a seek past its
        // birth makes it here with the piece already in flight).
        Vector3 from{at.position.x, at.position.y, at.position.z};
        if (const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed()))
        {
            std::uint32_t firstMs = 0, lastMs = 0;
            replay::entity::TransformSample first;
            if (track->Span(firstMs, lastMs) && track->Sample(firstMs, first))
                from = Vector3{first.relative.position.x, first.relative.position.y, first.relative.position.z};
        }
        // The wheel objects of its model near where it started, nearest first.
        struct Candidate
        {
            Object object;
            replay::entity::ReplayId vehicle;
            float d;
        };
        std::vector<Candidate> candidates;
        const float range = kFragParentM * kFragParentM;
        bool wheelModel = false;
        bool ownerBuilding = false; // a vehicle of this wheel model not built yet (a seek rebuilt it)
        for (const replay::playback::Puppet& owner : Ctx().puppets.list)
        {
            if (owner.packet.kind != replay::entity::EntityKind::Vehicle || owner.state == replay::playback::PuppetState::Failed)
                continue;
            if (owner.state != replay::playback::PuppetState::Ready)
            {
                const auto known = g_vehicleWheelModels.find(owner.packet.modelHash);
                ownerBuilding = ownerBuilding || (known != g_vehicleWheelModels.end() &&
                    replay::vehicle_pieces::IsVehicleWheelChild(model, known->second.models));
                continue;
            }
            const replay::vehicle_pieces::WheelModels* models = PoseV2VehicleWheelModels(owner);
            if (!models || !replay::vehicle_pieces::IsVehicleWheelChild(model, *models))
                continue;
            wheelModel = true;
            bool anyWheelObject = false;
            for (int slot = 0; slot < replay::vehicle_pieces::kMaxWheelSlots; ++slot)
            {
                replay::vehicle_pieces::WheelRead read;
                const char* error = ReadPoseV2WheelObject(owner.clone, slot, read);
                if (error && *error)
                    break;
                anyWheelObject = anyWheelObject || read.object != 0;
                if (!read.object || read.model != model)
                    continue;
                const Object wheel = PoseV2HandleAt(read.object);
                // A wheel the part natives broke off is kept or held: not the
                // vehicle's any more.
                if (!wheel || !ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(wheel, owner.clone))
                    continue;
                const float d = Distance2(ENTITY::GET_ENTITY_COORDS(wheel, TRUE, FALSE), from);
                if (d < range)
                    candidates.push_back({wheel, owner.packet.id, d});
            }
            // A puppet just built (a seek rebuilt it) makes its wheel objects
            // over a frame or two.
            ownerBuilding = ownerBuilding || !anyWheelObject || VehicleWheelsPending(owner);
        }
        if (!wheelModel && !ownerBuilding)
            return 0;
        const std::uint32_t key = puppet.packet.id.Packed();
        // The object puppets of its model recorded before it and due now come
        // first (a seek makes them all at once): the loose wheel it broke off,
        // the pieces that left before it. (A wheel child on its vehicle stays
        // Loading for good: it is a weapon puppet, not waited for.)
        for (const replay::playback::Puppet& other : Ctx().puppets.list)
            if (&other != &puppet && other.packet.kind == replay::entity::EntityKind::Prop && other.packet.modelHash == model &&
                other.state == replay::playback::PuppetState::Loading && other.packet.timeMs < puppet.packet.timeMs &&
                other.packet.timeMs <= timeMs &&
                WaitedMs(g_fragParentWaitSince, key, now) < replay::vehicle_pieces::kWheelCopyWaitMs)
            {
                ++g_fragParentStats.waited;
                g_fragOrderWaited.insert(key);
                return -1;
            }
        for (const PoseV2WheelObject& entry : g_wheelObjects)
        {
            if (!entry.holder || entry.model != model || !replay::entities::Exists(entry.object))
                continue;
            // A loose wheel where its puppet's record had it when the piece
            // broke off (after a seek it is elsewhere now).
            Vector3 there = ENTITY::GET_ENTITY_COORDS(entry.object, TRUE, FALSE);
            if (const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(entry.holder))
            {
                replay::entity::TransformSample then;
                if (track->Sample(puppet.packet.timeMs, then))
                    there = Vector3{then.relative.position.x, then.relative.position.y, then.relative.position.z};
            }
            const float d = Distance2(there, from);
            if (d < range)
                candidates.push_back({entry.object, entry.vehicle, d});
        }
        // A piece already made from one of these wheels carries what was under
        // its root: a piece of a group below breaks off that piece.
        for (const auto& [other, object] : g_fragPieceShaped)
        {
            const auto taken = g_fragPieceParents.find(other);
            if (other == puppet.packet.id.Packed() || taken == g_fragPieceParents.end() || !replay::entities::Exists(object) ||
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) != model)
                continue;
            Vector3 there = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            if (const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(other))
            {
                replay::entity::TransformSample then;
                if (track->Sample(puppet.packet.timeMs, then))
                    there = Vector3{then.relative.position.x, then.relative.position.y, then.relative.position.z};
            }
            const float d = Distance2(there, from);
            if (d < range)
                candidates.push_back({object, taken->second.vehicle, d});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
        // The nearest that still has the piece's root (two wheels side by side:
        // the other one's pieces took theirs already).
        static replay::frag::ObjectFragState state; // script thread only
        const int root = replay::fragpiece::Root(row->groups);
        const auto gone = [&](int group) { return ((state.gone[group / 8] >> (group % 8)) & 1) != 0; };
        Object parent = 0;
        replay::entity::ReplayId vehicle;
        bool read = false;
        std::string looked; // each candidate: object:distance cm:read:groups:cache root:root gone
        for (const Candidate& candidate : candidates)
        {
            const bool readable =
                replay::frag::ReadObjectFrag(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(candidate.object)), state);
            read = readable && root > 0 && root < state.groups && !gone(root);
            if (looked.size() < 160)
                looked += (looked.empty() ? "" : ";") + std::to_string(candidate.object) + ":" +
                    std::to_string(static_cast<int>(std::sqrt(candidate.d) * 100.0f)) + ":" + (readable ? "1" : "0") + ":" +
                    std::to_string(state.groups) + ":" + std::to_string(state.root) + ":" +
                    (readable && root > 0 && root < state.groups && gone(root) ? "1" : "0");
            if (read || !parent)
            {
                parent = candidate.object;
                vehicle = candidate.vehicle;
            }
            if (read)
                break;
        }
        if (!read && parent) // none readable: the nearest, its state read again
            read = replay::frag::ReadObjectFrag(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(parent)), state) &&
                root > 0 && root < state.groups;
        if (!parent && ownerBuilding)
        {
            // Its wheel comes with the vehicle puppet being built.
            if (WaitedMs(g_fragParentWaitSince, key, now) < replay::vehicle_pieces::kWheelCopyWaitMs)
            {
                ++g_fragParentStats.waited;
                return -1;
            }
        }
        const auto log = [&](const char* result, Object piece) {
            if (g_fragParentStats.logs >= 64)
                return;
            ++g_fragParentStats.logs;
            Ctx().log << "POSE_V2_PLAYBACK_FRAG_PIECE,id=" << replay::entity::IdText(puppet.packet.id) << ",from=parent,parent=" << parent
                      << ",vehicle=" << replay::entity::IdText(vehicle) << ",root=" << replay::fragpiece::Root(row->groups)
                      << ",result=" << result << ",object=" << piece << ",candidates=" << (looked.empty() ? "-" : looked)
                      << ",playhead_ms=" << timeMs << '\n';
        };
        if (!parent)
        {
            ++g_fragParentStats.noParent;
            log("no_parent", 0);
            return 0;
        }
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(parent));
        // Pieces of this wheel due together (a seek past them, two in one
        // frame) come off in the order the recording had them, which their
        // group lists tell, among the pieces recorded no later than this one:
        // a piece of a group above this one that carried this group goes
        // first and this piece breaks off it; a piece of a group below this
        // one that this piece did not carry went before it (breaking this root
        // first would take that group along, and the trim would drop it).
        bool first = false; // another piece goes first
        for (const replay::playback::Puppet& other : Ctx().puppets.list)
        {
            if (!read)
                break;
            if (&other == &puppet || other.packet.modelHash != model || other.state != replay::playback::PuppetState::Loading ||
                other.packet.timeMs > timeMs || other.packet.timeMs > puppet.packet.timeMs)
                continue;
            const replay::FragPiece* otherRow = FragPieceOf(other.packet.id);
            const int otherRoot = otherRow ? replay::fragpiece::Root(otherRow->groups) : -1;
            if (otherRoot <= 0 || otherRoot >= state.groups || otherRoot == root || gone(otherRoot))
                continue;
            if (replay::fragpiece::CameOffBefore(state.parents, state.groups, row->groups, otherRow->groups))
            {
                first = true;
                break;
            }
        }
        if (first)
        {
            if (WaitedMs(g_fragParentWaitSince, key, now) < kFragParentWaitMs)
            {
                ++g_fragParentStats.waited;
                g_fragOrderWaited.insert(key);
                return -1;
            }
        }
        g_fragParentWaitSince.erase(key);
        // A parent drawn by a puppet (a loose wheel, an earlier piece) may have
        // its pose armed: the break rebuilds its skeleton, so the pose is
        // disarmed first and re-armed on the new one (RefreshOverride).
        for (replay::playback::Puppet& holder : Ctx().puppets.list)
            if (holder.clone == parent && holder.overrideSlot >= 0)
                Ctx().hooks->ReleaseOverride(holder);
        std::uint64_t entity = 0;
        const replay::frag::VehicleGroupResult result =
            replay::frag::BreakOffObjectGroup(address, static_cast<std::uint16_t>(root), entity);
        const Object piece = result == replay::frag::VehicleGroupResult::Deleted ? PoseV2HandleAt(entity) : 0;
        if (!piece)
        {
            ++g_fragParentStats.failed;
            log(replay::frag::VehicleGroupResultText(result), 0);
            return 0;
        }
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
        ENTITY::SET_ENTITY_INVINCIBLE(piece, TRUE);
        ENTITY::SET_ENTITY_COLLISION(piece, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(piece, TRUE);
        replay::pose::playback::PlaceClone(piece, at);
        SuppressPickupLook(piece);
        // The groups under its root it did not carry (they went before, as the
        // recorded breaks the wheel kept for it say) come off the piece.
        int removed = 0;
        if (read)
        {
            const replay::fragpiece::Plan plan = replay::fragpiece::PlanPiece(state.parents, state.groups, row->groups);
            const std::uintptr_t fragVtable = sig::Find(kFragInstVtable);
            const std::uint64_t pieceAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(piece));
            for (std::uint16_t group : plan.ok ? plan.remove : std::vector<std::uint16_t>{})
            {
                if (GuardedGroupGone(pieceAddress, group) == 1)
                    continue;
                const int child = GuardedGroupToChild(pieceAddress, fragVtable, group);
                if (child < 0)
                    continue;
                OBJECT::BREAK_OBJECT_FRAGMENT_CHILD(piece, child, TRUE);
                ++removed;
            }
        }
        g_fragPieceShaped[key] = piece;
        g_fragPieceParents[key] = {vehicle, puppet.packet.timeMs};
        for (PoseV2WheelObject& entry : g_wheelObjects)
            if (entry.object == parent)
                entry.pieced = true;
        ++g_fragParentStats.made;
        log(removed ? "made_trimmed" : "made", piece);
        out = piece;
        return 1;
    }

    // True when the clone may be shown now: no piece row, or already made
    // the piece. False while it waits (hidden) or after it failed.
    bool ShapePoseV2FragPiece(replay::playback::Puppet& puppet, bool sampled,
        const replay::entity::TransformSample& sample, std::uint32_t timeMs)
    {
        if (puppet.packet.propOrigin == replay::entity::PropOrigin::VehiclePiece || puppet.packet.weaponHash)
            return true;
        const replay::FragPiece* row = FragPieceOf(puppet.packet.id);
        if (!row)
            return true;
        const std::uint32_t key = puppet.packet.id.Packed();
        const auto shaped = g_fragPieceShaped.find(key);
        if (shaped != g_fragPieceShaped.end() && shaped->second == puppet.clone)
            return true;
        const ULONGLONG now = GetTickCount64();
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
        static replay::frag::ObjectFragState state; // 2 KiB, script thread only
        const bool read = replay::frag::ReadObjectFrag(address, state);
        const replay::fragpiece::Plan plan =
            read ? replay::fragpiece::PlanPiece(state.parents, state.groups, row->groups) : replay::fragpiece::Plan{};
        if (!plan.ok)
        {
            ULONGLONG& until = g_fragPieceWaitUntil[key];
            if (!until)
                until = now + kFragPieceWaitMs;
            if (read || !replay::frag::ObjectGroupsSupported() || now >= until)
            {
                Ctx().log << "POSE_V2_PLAYBACK_FRAG_PIECE,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",result=" << (read ? "plan_refused" : replay::frag::ObjectGroupsSupported() ? "unreadable" : "unsupported")
                    << ",type_groups=" << (read ? state.groups : -1) << ",root=" << replay::fragpiece::Root(row->groups)
                    << ",playhead_ms=" << timeMs << '\n';
                FailPoseV2FragPiece(puppet, "frag_piece_failed", timeMs);
                return false;
            }
            replay::pose::playback::SetCloneVisible(puppet.clone, false);
            return false;
        }
        const char* result = "kept_root";
        if (plan.breakOff > 0)
        {
            std::uint64_t entity = 0;
            const replay::frag::VehicleGroupResult broke =
                replay::frag::BreakOffObjectGroup(address, static_cast<std::uint16_t>(plan.breakOff), entity);
            result = replay::frag::VehicleGroupResultText(broke);
            // The new object's handle from the pools by its address (the
            // object listing is walked once a frame, before the piece
            // existed); the listing only without the pool reader.
            Object piece = 0;
            if (entity)
            {
                bool poolReader = false;
                piece = replay::entities::HandleAtAddress(entity, poolReader);
                if (!poolReader)
                {
                    static int objects[1024];
                    const int count = (std::min)(ListObjects(objects, 1024), 1024);
                    for (int slot = 0; slot < count && !piece; ++slot)
                        if (objects[slot] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == entity)
                            piece = objects[slot];
                }
            }
            if (!piece || !replay::entities::Exists(piece))
            {
                Ctx().log << "POSE_V2_PLAYBACK_FRAG_PIECE,id=" << replay::entity::IdText(puppet.packet.id) << ",result=" << result
                    << ",root=" << plan.root << ",found=0,playhead_ms=" << timeMs << '\n';
                FailPoseV2FragPiece(puppet, "frag_piece_failed", timeMs);
                return false;
            }
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
            ENTITY::SET_ENTITY_INVINCIBLE(piece, TRUE);
            ENTITY::SET_ENTITY_COLLISION(piece, FALSE, FALSE);
            ENTITY::FREEZE_ENTITY_POSITION(piece, TRUE);
            if (sampled)
                replay::pose::playback::PlaceClone(piece, sample.relative);
            SuppressPickupLook(piece);
            // The whole object is left without its root's subtree: it goes.
            Ctx().hooks->ReleaseOverride(puppet);
            Object whole = puppet.clone;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(whole, TRUE, TRUE);
            ENTITY::DELETE_ENTITY(&whole);
            puppet.clone = piece;
        }
        const std::uintptr_t fragVtable = sig::Find(kFragInstVtable);
        const std::uint64_t pieceAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
        int removed = 0;
        int unmapped = 0;
        for (std::uint16_t group : plan.remove)
        {
            if (GuardedGroupGone(pieceAddress, group) == 1)
                continue;
            const int child = GuardedGroupToChild(pieceAddress, fragVtable, group);
            if (child < 0)
            {
                ++unmapped;
                continue;
            }
            OBJECT::BREAK_OBJECT_FRAGMENT_CHILD(puppet.clone, child, TRUE);
            ++removed;
        }
        g_fragPieceShaped[key] = puppet.clone;
        g_fragPieceWaitUntil.erase(key);
        ++g_fragPiecesShaped;
        if (g_fragPieceShapeLogs < 32)
        {
            ++g_fragPieceShapeLogs;
            Ctx().log << "POSE_V2_PLAYBACK_FRAG_PIECE,id=" << replay::entity::IdText(puppet.packet.id)
                << ",source=" << replay::fragpiece::SourceText(row->source) << ",root=" << plan.root
                << ",break_off=" << plan.breakOff << ",result=" << result << ",object=" << puppet.clone
                << ",remove=" << plan.remove.size() << ",removed=" << removed << ",unmapped=" << unmapped
                << ",stray=" << plan.stray << ",type_groups=" << state.groups << ",playhead_ms=" << timeMs << '\n';
        }
        Ctx().hooks->LogPuppet(puppet, "created_frag_piece", timeMs);
        return true;
    }

    void BeginPlayback()
    {
        g_poseV2AttachLogs = 0;
        g_strapApplyLogs = 0;
        g_weaponLookVerdicts.clear();
        g_weaponLookEnginePlay = true;
        g_weaponLookEnginePlayChecked = false;
        g_weaponLookPlayLogs = 0;
        g_weaponLookTally = {};
        g_fragApplied.clear();
        g_fragPuppet.clear();
        g_fragBreaksPlayed = 0;
        g_fragPieceShaped.clear();
        g_fragPieceWaitUntil.clear();
        g_fragPiecesShaped = g_fragPiecesFailed = g_fragPieceShapeLogs = 0;
        g_vehiclePieceLogs = 0;
        g_poseV2ObjectPlayLogs = 0;
        g_poseV2ObjectPlayNoted.clear();
        g_animatedPlay = {};
        g_goreWorldModelRefused = g_goreRefrozen = 0; // (E)
        g_vehicleChildRecords.clear(); // filled by UpdatePoseV2Weapons as the parents are met
        g_engineCopyNextCheck.clear();
        g_engineCopiesHidden.clear();
        g_engineCopyChecks = 0;
        g_vehicleFragPieces.clear();
        g_vehicleFragPieceStats = {};
        g_vehicleWheelModels.clear();
        g_wheelChildBits.clear();
        g_wheelChildLogged.clear();
        g_leftWheels.clear();
        g_leftWheelsDetached.clear();
        g_leftWheelMissLogged.clear();
        g_wheelsPendingSince.clear();
        g_wheelsMade.clear();
        g_wheelCountSeen.clear();
        g_fragPieceGroupsOf.clear();
        g_fragPieceParents.clear();
        g_fragParentWaitSince.clear();
        g_fragParentStats = {};
        g_wheelChildStats = {};
        g_weaponBonesSeen.clear();
        g_weaponBonesArmed.clear();
        g_weaponBonesPlayLogs = 0;
        g_weaponDrawn.clear();
        g_weaponDrawnLines = 0;
        g_poseV2ObjectSkeletonAsked.clear();
        g_stillSignatures.clear();
        g_stillLines = 0;
        g_drawnLines = 0;
        g_fragFixes = 0;
        g_fragUnmapped = 0;
        g_fragLastError = 0;
        g_fragMaxPlayheadMs = 0;
        g_objectCheckLogs = 0; // the verdicts of this playback
        WarnPoseV2WeaponPlain();
        // hat_look.h: the hat models of the clip (a dropped hat's prop) on top
        // of the known ones, and a fresh dresser.
        EndPoseV2HatDresser(false);
        g_hatModels.clear();
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (packet.kind == replay::entity::EntityKind::Prop && !packet.weaponHash &&
                packet.propOrigin == replay::entity::PropOrigin::DroppedHat)
                g_hatModels.push_back(packet.modelHash);
        std::sort(g_hatModels.begin(), g_hatModels.end());
        g_hatModels.erase(std::unique(g_hatModels.begin(), g_hatModels.end()), g_hatModels.end());
        BeginPoseV2WorldStandIns();
        replay::objlook::BeginPlayback();
        // The wheel objects start empty;
        // the models of the clip's plain object puppets (a kept wheel has a
        // use when one of them is of its model) and its wheel pieces' models.
        g_wheelObjects.clear();
        g_wheelModels.clear();
        g_wheelModelsRead.clear();
        g_wheelCopyWaitSince.clear();
        g_clipPlainObjectModels.clear();
        g_clipHasWheelPieces = false;
        g_wheelPlay = {};
        g_wheelPlayLogs = 0;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
        {
            if ((packet.kind != replay::entity::EntityKind::Prop && packet.kind != replay::entity::EntityKind::Weapon) ||
                packet.weaponHash)
                continue;
            if (packet.kind == replay::entity::EntityKind::Prop && packet.propOrigin == replay::entity::PropOrigin::VehiclePiece)
            {
                if (replay::vehicle_pieces::IsWheelPiece(replay::vehicle_pieces::GroupOf(packet)))
                {
                    g_clipHasWheelPieces = true;
                    g_wheelModels.insert(packet.modelHash);
                }
                continue;
            }
            g_clipPlainObjectModels.insert(packet.modelHash);
        }
        g_wheelObjectsOpen = true;
    }

    void EndPlayback()
    {
        // object_check.h FragUnplayed: a recorded break the
        // playhead went past whose object never took it (no prop puppet of
        // that id got its breaks, vehicles aside: their own path).
        std::set<std::uint32_t> unplayed;
        for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
        {
            if (e.timeMs > g_fragMaxPlayheadMs || g_fragApplied.count(e.id.Packed()))
                continue;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(e.id.Packed());
            if (create && create->kind == replay::entity::EntityKind::Vehicle)
                continue;
            if (unplayed.insert(e.id.Packed()).second)
                LogObjectCheck(replay::objcheck::Verdict::FragUnplayed, replay::entity::IdText(e.id),
                    create ? create->modelHash : 0u, e.timeMs, create ? ",why=no_puppet" : ",why=no_create");
        }
        if (!Ctx().clip.fragPieces.empty())
            Ctx().log << "POSE_V2_PLAYBACK_FRAG_PIECES,recorded=" << Ctx().clip.fragPieces.size() << ",shaped=" << g_fragPiecesShaped
                << ",failed=" << g_fragPiecesFailed << '\n';
        if (!Ctx().clip.fragBreaks.empty())
            Ctx().log << "POSE_V2_PLAYBACK_FRAG_BREAKS,recorded=" << Ctx().clip.fragBreaks.size() << ",played=" << g_fragBreaksPlayed
                << ",rebuilt=" << g_fragFixes << ",unmapped=" << g_fragUnmapped << ",last_error=" << g_fragLastError << '\n';
        // The animated objects met and armed; a record met and never armed is
        // the drawer that played closed.
        {
            std::uint32_t worldUnarmed = 0, vehicleUnarmed = 0;
            for (const std::uint32_t record : g_animatedPlay.worldMet)
                if (!g_animatedPlay.worldArmed.count(record))
                {
                    ++worldUnarmed;
                    const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(record);
                    LogObjectCheck(replay::objcheck::Verdict::AnimatedUnposed,
                        replay::entity::IdText(replay::entity::ReplayId::FromPacked(record)), create ? create->modelHash : 0u,
                        g_fragMaxPlayheadMs, ",phase=play,why=never_armed,path=world");
                }
            for (const std::uint32_t record : g_animatedPlay.vehicleMet)
                if (!g_animatedPlay.vehicleArmed.count(record))
                {
                    ++vehicleUnarmed;
                    const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(record);
                    LogObjectCheck(replay::objcheck::Verdict::AnimatedUnposed,
                        replay::entity::IdText(replay::entity::ReplayId::FromPacked(record)), create ? create->modelHash : 0u,
                        g_fragMaxPlayheadMs, ",phase=play,why=never_armed,path=vehicle_child");
                }
            std::uint32_t worldPosed = 0;
            for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
                worldPosed += packet.kind == replay::entity::EntityKind::Prop &&
                        packet.propOrigin == replay::entity::PropOrigin::WorldObject && packet.boneCount >= 2
                    ? 1u : 0u;
            Ctx().log << "POSE_V2_OBJECT_ANIMATED_PLAY,world_posed_records=" << worldPosed
                      << ",world_met=" << g_animatedPlay.worldMet.size() << ",world_armed=" << g_animatedPlay.worldArmed.size()
                      << ",world_unarmed=" << worldUnarmed << ",vehicle_children=" << g_vehicleChildRecords.size()
                      << ",vehicle_child_met=" << g_animatedPlay.vehicleMet.size()
                      << ",vehicle_child_armed=" << g_animatedPlay.vehicleArmed.size()
                      << ",vehicle_child_unarmed=" << vehicleUnarmed << ",engine_copy_checks=" << g_engineCopyChecks
                      << ",engine_copies_hidden=" << g_engineCopiesHidden.size() << '\n';
            Ctx().log << "POSE_V2_GORE_PIECES,phase=play,world_model_refused=" << g_goreWorldModelRefused
                      << ",adopted_refrozen=" << g_goreRefrozen << '\n'; // (E)
        }
        for (auto& [key, object] : g_vehicleFragPieces)
            if (replay::entities::Exists(object))
            {
                Object leftover = object;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(leftover, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&leftover);
                ++g_vehicleFragPieceStats.released;
            }
        g_vehicleFragPieces.clear();
        if (g_fragParentStats.made || g_fragParentStats.noParent || g_fragParentStats.failed)
            Ctx().log << "POSE_V2_PLAYBACK_FRAG_PIECES_FROM_PARENT,made=" << g_fragParentStats.made
                      << ",no_parent=" << g_fragParentStats.noParent << ",failed=" << g_fragParentStats.failed
                      << ",waited=" << g_fragParentStats.waited << '\n';
        if (g_vehicleFragPieceStats.early)
            Ctx().log << "POSE_V2_VEHICLE_PIECES,phase=play,early=" << g_vehicleFragPieceStats.early
                      << ",adopted=" << g_vehicleFragPieceStats.adopted << ",released=" << g_vehicleFragPieceStats.released << '\n';
        if (!g_wheelChildBits.empty())
            Ctx().log << "POSE_V2_VEHICLE_WHEEL_CHILDREN,phase=play,children=" << g_wheelChildBits.size()
                      << ",vehicle_models=" << g_vehicleWheelModels.size() << ",layout="
                      << (g_modelInfoLayout > 0 ? "proven" : g_modelInfoLayout < 0 ? "signature_mismatch" : "unchecked")
                      << ",released=" << g_wheelChildStats.released << '\n';
        EndPoseV2WorldStandIns();
        EndPoseV2HatDresser(false); // the helper deleted, its requests released
        replay::objlook::EndPlayback(); // the TXDs asked released
        // The free wheel objects deleted (the held ones went with their
        // puppets) and the check: a wheel model made plain (copies_plain) is
        // drawn without its vehicle (yellow), a failed wheel piece is a wheel
        // that vanished.
        std::uint32_t freeLeft = 0;
        for (PoseV2WheelObject& entry : g_wheelObjects)
            if (!entry.holder && replay::entities::Exists(entry.object))
            {
                Object object = entry.object;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
                ++freeLeft;
            }
        std::uint32_t wheelPieces = 0;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            wheelPieces += packet.kind == replay::entity::EntityKind::Prop &&
                    packet.propOrigin == replay::entity::PropOrigin::VehiclePiece &&
                    replay::vehicle_pieces::IsWheelPiece(replay::vehicle_pieces::GroupOf(packet))
                ? 1u : 0u;
        Ctx().log << "POSE_V2_VEHICLE_WHEELS,phase=play,wheel_pieces=" << wheelPieces << ",pieces_kept=" << g_wheelPlay.piecesFromKept
                  << ",pieces_native=" << g_wheelPlay.piecesNative << ",pieces_failed=" << g_wheelPlay.piecesFailed
                  << ",kept=" << g_wheelPlay.kept << ",deleted_no_use=" << g_wheelPlay.deleted
                  << ",copies_adopted=" << g_wheelPlay.copiesAdopted << ",copies_plain=" << g_wheelPlay.copiesPlain
                  << ",copies_waited=" << g_wheelPlay.copiesWaited << ",returned=" << g_wheelPlay.returned
                  << ",released=" << g_wheelPlay.released << ",gone=" << g_wheelPlay.gone << ",free_at_end=" << freeLeft
                  << ",wheel_models=" << g_wheelModels.size() << ",model_reads=" << g_wheelPlay.modelReads << '\n';
        g_wheelObjects.clear();
        g_wheelObjectsOpen = false;
    }
    void ReleaseGorePieces(replay::entity::ReplayId owner, std::uint32_t hitMs)
    {
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Prop ||
                puppet.packet.propOrigin != replay::entity::PropOrigin::GorePiece ||
                !(puppet.packet.owner == owner) || puppet.packet.timeMs < hitMs || puppet.packet.timeMs > hitMs + 1000 ||
                puppet.state != replay::playback::PuppetState::Ready)
            {
                continue;
            }
            if (puppet.clone && replay::entities::Exists(puppet.clone))
            {
                Object object = puppet.clone;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
            }
            puppet.clone = 0;
            puppet.state = replay::playback::PuppetState::Loading;
            puppet.stateSince = GetTickCount64();
            Ctx().log << "POSE_V2_PLAYBACK_GORE_PIECE_RELEASE,id=" << replay::entity::IdText(puppet.packet.id)
                << ",owner=" << replay::entity::IdText(owner) << ",hit_ms=" << hitMs << '\n';
        }
    }

    bool KeepVehicleFragPiece(replay::entity::ReplayId vehicle, std::uint16_t group, std::uint64_t entity, std::uint32_t timeMs)
    {
        Object piece = 0;
        if (entity)
        {
            bool poolReader = false;
            piece = replay::entities::HandleAtAddress(entity, poolReader);
            if (!poolReader)
            {
                static int objects[1024];
                const int count = (std::min)(ListObjects(objects, 1024), 1024);
                for (int slot = 0; slot < count && !piece; ++slot)
                    if (objects[slot] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[slot])) == entity)
                        piece = objects[slot];
            }
        }
        if (!piece || !replay::entities::Exists(piece))
            return false;
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
        ENTITY::SET_ENTITY_INVINCIBLE(piece, TRUE);
        ENTITY::SET_ENTITY_COLLISION(piece, FALSE, FALSE);
        ENTITY::FREEZE_ENTITY_POSITION(piece, TRUE);
        ENTITY::SET_ENTITY_VISIBLE(piece, FALSE);
        Object& slot = g_vehicleFragPieces[{vehicle.Packed(), group}];
        if (slot && slot != piece && replay::entities::Exists(slot))
        {
            Object old = slot;
            ENTITY::DELETE_ENTITY(&old);
        }
        slot = piece;
        ++g_vehicleFragPieceStats.early;
        if (g_vehiclePieceLogs < 64)
        {
            ++g_vehiclePieceLogs;
            Ctx().log << "POSE_V2_PLAYBACK_VEHICLE_PIECE,vehicle=" << replay::entity::IdText(vehicle) << ",group=" << group
                      << ",result=early,object=" << piece << ",playhead_ms=" << timeMs << '\n';
        }
        return true;
    }

    void ReleaseVehiclePieces(replay::entity::ReplayId vehicle)
    {
        // The new puppet's wheel objects go on as the loose wheels again
        // (LeftVehicleWheels), its handle may be the old one's.
        for (auto it = g_leftWheelsDetached.begin(); it != g_leftWheelsDetached.end();)
            it = it->first.first == vehicle.Packed() ? g_leftWheelsDetached.erase(it) : std::next(it);
        // The new puppet makes its wheel objects again (VehicleWheelsPending).
        if (const replay::playback::Puppet* puppet = Ctx().puppets.Find(vehicle))
        {
            g_wheelsMade.erase(puppet->clone);
            g_wheelsPendingSince.erase(puppet->clone);
            g_wheelCountSeen.erase(puppet->clone);
        }
        for (auto it = g_vehicleFragPieces.begin(); it != g_vehicleFragPieces.end();)
        {
            if (it->first.first != vehicle.Packed())
            {
                ++it;
                continue;
            }
            if (replay::entities::Exists(it->second))
            {
                Object object = it->second;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
            }
            ++g_vehicleFragPieceStats.released;
            it = g_vehicleFragPieces.erase(it);
        }
        // The fragment pieces broken off this vehicle's wheels, and a loose
        // wheel's puppet whose wheel lost pieces, are made again from the new
        // puppet's wheels (PoseV2FragPieceFromParent).
        const auto releasePuppet = [](replay::playback::Puppet& puppet) {
            Ctx().hooks->ReleaseOverride(puppet);
            if (puppet.clone && replay::entities::Exists(puppet.clone))
            {
                Object object = puppet.clone;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
            }
            puppet.clone = 0;
            puppet.state = replay::playback::PuppetState::Loading;
            puppet.stateSince = GetTickCount64();
        };
        for (auto it = g_fragPieceParents.begin(); it != g_fragPieceParents.end();)
        {
            if (!(it->second.vehicle == vehicle))
            {
                ++it;
                continue;
            }
            if (replay::playback::Puppet* piece = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(it->first)))
                if (piece->clone && g_fragPieceShaped.count(it->first) && g_fragPieceShaped[it->first] == piece->clone)
                    releasePuppet(*piece);
            g_fragPieceShaped.erase(it->first);
            it = g_fragPieceParents.erase(it);
        }
        for (std::size_t n = g_wheelObjects.size(); n-- > 0;)
        {
            PoseV2WheelObject& entry = g_wheelObjects[n];
            if (entry.vehicle == vehicle && entry.holder && entry.pieced)
                if (replay::playback::Puppet* holder = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(entry.holder)))
                {
                    if (holder->clone == entry.object)
                    {
                        releasePuppet(*holder);
                        g_wheelObjects.erase(g_wheelObjects.begin() + static_cast<std::ptrdiff_t>(n));
                    }
                }
        }
        // The free wheel objects of the vehicle go with it; the ones its pieces
        // hold go with the pieces below; one a plain object puppet holds stays
        // that puppet's object.
        for (std::size_t n = g_wheelObjects.size(); n-- > 0;)
        {
            PoseV2WheelObject& entry = g_wheelObjects[n];
            if (!(entry.vehicle == vehicle))
                continue;
            if (!entry.holder && replay::entities::Exists(entry.object))
            {
                Object object = entry.object;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
                ++g_wheelPlay.released;
            }
            g_wheelObjects.erase(g_wheelObjects.begin() + static_cast<std::ptrdiff_t>(n));
        }
        int released = 0;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (!replay::vehicle_pieces::IsPieceOf(puppet.packet, vehicle))
                continue;
            if (puppet.clone && replay::entities::Exists(puppet.clone))
            {
                Object object = puppet.clone;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
            }
            if (puppet.clone || puppet.state == replay::playback::PuppetState::Failed)
                ++released;
            puppet.clone = 0;
            puppet.state = replay::playback::PuppetState::Loading;
            puppet.stateSince = GetTickCount64();
        }
        if (released)
            Ctx().log << "POSE_V2_PLAYBACK_VEHICLE_PIECE_RELEASE,vehicle=" << replay::entity::IdText(vehicle)
                << ",pieces=" << released << '\n';
    }

    void NoteVehicleWheelOff(replay::entity::ReplayId vehicle, Entity vehicleHandle, int wheel, int slot)
    {
        NotePoseV2WheelOff(vehicle, vehicleHandle, wheel, slot);
    }

    bool ClipHasWheelPieces() { return g_clipHasWheelPieces; }

    void NoteVehiclePuppetWheels(Entity vehicleClone) { NotePoseV2PuppetWheelModels(vehicleClone); }

    bool KeepVehicleWheel(replay::entity::ReplayId vehicle, int wheel, Object piece, std::uint32_t timeMs)
    {
        if (!g_wheelObjectsOpen || !piece || !replay::entities::Exists(piece) || !ENTITY::IS_ENTITY_AN_OBJECT(piece))
            return false;
        PoseV2WheelObject entry;
        entry.object = piece;
        entry.vehicle = vehicle;
        entry.wheel = wheel;
        entry.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(piece));
        g_wheelModels.insert(entry.model);
        bool pieceOfWheel = false;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (replay::vehicle_pieces::IsPieceOf(packet, vehicle) &&
                replay::vehicle_pieces::IsWheelPiece(replay::vehicle_pieces::GroupOf(packet)) &&
                replay::vehicle_pieces::WheelOfPiece(replay::vehicle_pieces::GroupOf(packet)) == wheel)
                pieceOfWheel = true;
        if (!replay::vehicle_pieces::KeepWheelSpare(pieceOfWheel, g_clipPlainObjectModels.count(entry.model) != 0))
        {
            ++g_wheelPlay.deleted;
            LogPoseV2Wheel("delete_no_use", entry, "none", timeMs);
            return false;
        }
        HidePoseV2WheelObject(piece);
        g_wheelObjects.push_back(entry);
        ++g_wheelPlay.kept;
        LogPoseV2Wheel("keep", entry, "none", timeMs);
        return true;
    }

    std::uint16_t KeptVehicleWheels(const replay::playback::Puppet& vehicle, std::uint32_t timeMs)
    {
        const replay::vehicle_pieces::WheelModels* models = PoseV2VehicleWheelModels(vehicle);
        if (!models)
            return 0;
        static std::vector<replay::entity::Attachment> attachments;
        Ctx().clip.lifecycle.AttachmentsAt(timeMs, attachments);
        std::uint16_t kept = 0;
        for (const replay::entity::Attachment& attachment : attachments)
        {
            if (!(attachment.parent == vehicle.packet.id) || attachment.kind != replay::entity::AttachmentKind::WeaponPoint)
                continue;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(attachment.child.Packed());
            if (!create || create->kind != replay::entity::EntityKind::Weapon || create->weaponHash ||
                !replay::vehicle_pieces::IsVehicleWheelChild(create->modelHash, *models))
                continue;
            kept = static_cast<std::uint16_t>(kept | PoseV2WheelChildBits(attachment.child.Packed(), create->modelHash, *models));
        }
        return kept;
    }

    std::uint16_t LeftVehicleWheels(const replay::playback::Puppet& vehicle, std::uint32_t timeMs)
    {
        const replay::vehicle_pieces::WheelModels* models = PoseV2VehicleWheelModels(vehicle);
        if (!models)
            return 0;
        auto found = g_leftWheels.find(vehicle.packet.id.Packed());
        if (found == g_leftWheels.end())
        {
            // Once per vehicle: each loose prop of a wheel model of the vehicle
            // (DroppedWeapon, the recorder's continuation of an attached object)
            // and the wheel child whose track ended as it began.
            std::vector<LeftWheel> left;
            static std::vector<replay::entity::Attachment> attachments;
            for (const replay::entity::CreatePacket& prop : Ctx().clip.created)
            {
                if (prop.kind != replay::entity::EntityKind::Prop || prop.propOrigin != replay::entity::PropOrigin::DroppedWeapon ||
                    !(prop.owner == vehicle.packet.id) || !replay::vehicle_pieces::IsVehicleWheelChild(prop.modelHash, *models))
                    continue;
                for (const replay::entity::CreatePacket& child : Ctx().clip.created)
                {
                    if (child.kind != replay::entity::EntityKind::Weapon || child.modelHash != prop.modelHash || child.weaponHash)
                        continue;
                    const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(child.id.Packed());
                    std::uint32_t firstMs = 0, lastMs = 0;
                    if (!track || !track->Span(firstMs, lastMs) || lastMs > prop.timeMs || prop.timeMs - lastMs > 300)
                        continue;
                    Ctx().clip.lifecycle.AttachmentsAt(firstMs, attachments);
                    const bool onVehicle = std::any_of(attachments.begin(), attachments.end(), [&](const replay::entity::Attachment& a) {
                        return a.child == child.id && a.parent == vehicle.packet.id;
                    });
                    if (onVehicle)
                        left.push_back({prop.timeMs, PoseV2WheelChildBits(child.id.Packed(), child.modelHash, *models), prop.id.Packed(),
                            child.id.Packed()});
                }
            }
            found = g_leftWheels.emplace(vehicle.packet.id.Packed(), std::move(left)).first;
        }
        std::uint16_t bits = 0;
        for (const LeftWheel& wheel : found->second)
        {
            if (wheel.leftMs > timeMs)
                continue;
            bits = static_cast<std::uint16_t>(bits | wheel.bits);
            // The wheel object comes off the puppet as it is (no part native: that
            // one breaks the wheel up, and the loose wheel's own fragment pieces
            // still break off it later): the nearest wheel object of the prop's
            // model to where the prop starts, kept free for the prop.
            const std::pair<std::uint32_t, std::uint32_t> detachedKey{vehicle.packet.id.Packed(), wheel.prop};
            const auto detached = g_leftWheelsDetached.find(detachedKey);
            if (!vehicle.clone || (detached != g_leftWheelsDetached.end() && detached->second == vehicle.clone))
                continue;
            const replay::entity::CreatePacket* prop = Ctx().clip.FindCreate(wheel.prop);
            const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(wheel.prop);
            std::uint32_t firstMs = 0, lastMs = 0;
            replay::entity::TransformSample first;
            if (!prop || !track || !track->Span(firstMs, lastMs) || !track->Sample(firstMs, first))
                continue;
            Vector3 at{first.relative.position.x, first.relative.position.y, first.relative.position.z};
            // Where that wheel is on the vehicle puppet now: the child's last
            // place in the vehicle's frame (the puppet may stand elsewhere than
            // when the wheel left: a seek past the explosion, a wreck thrown).
            if (const replay::entity::RelativeTrack* childTrack = wheel.child ? Ctx().clip.WeaponTrackOf(wheel.child) : nullptr)
            {
                std::uint32_t childFirst = 0, childLast = 0;
                replay::entity::TransformSample onVehicle;
                if (childTrack->Span(childFirst, childLast) && childTrack->Sample(childLast, onVehicle))
                {
                    const replay::pose::WorldTransform there =
                        replay::entity::ComposeTransform(ReadEntityTransform(vehicle.clone), onVehicle.relative);
                    at = Vector3{there.position.x, there.position.y, there.position.z};
                }
            }
            Object nearest = 0;
            float best = 2.0f * 2.0f;
            std::string looked; // each slot of the model: object:attached:distance cm
            for (int slot = 0; slot < replay::vehicle_pieces::kMaxWheelSlots; ++slot)
            {
                replay::vehicle_pieces::WheelRead read;
                const char* error = ReadPoseV2WheelObject(vehicle.clone, slot, read);
                if (error && *error)
                    break;
                if (!read.object || read.model != prop->modelHash)
                    continue;
                const Object object = PoseV2HandleAt(read.object);
                const bool attached = object && ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(object, vehicle.clone);
                if (looked.size() < 120)
                    looked += (looked.empty() ? "" : ";") + std::to_string(slot) + ":" + std::to_string(object) + ":" +
                        (attached ? "1" : "0") + ":" +
                        std::to_string(object ? static_cast<int>(std::sqrt(Distance2(ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE), at)) * 100.0f) : -1);
                if (!attached)
                    continue;
                const float d = Distance2(ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE), at);
                if (d < best)
                {
                    best = d;
                    nearest = object;
                }
            }
            // None yet (a puppet just built has not made its wheel objects):
            // looked for again next tick.
            if (!nearest || !g_wheelObjectsOpen)
            {
                if (g_leftWheelMissLogged.insert({vehicle.clone, wheel.prop}).second && g_leftWheelMissLogged.size() <= 16)
                    Ctx().log << "POSE_V2_PLAYBACK_WHEEL,action=left_detach_wait,vehicle=" << replay::entity::IdText(vehicle.packet.id)
                              << ",puppet=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(wheel.prop))
                              << ",open=" << (g_wheelObjectsOpen ? 1 : 0) << ",slots=" << (looked.empty() ? "-" : looked)
                              << ",playhead_ms=" << timeMs << '\n';
                continue;
            }
            g_leftWheelsDetached[detachedKey] = vehicle.clone;
            HidePoseV2WheelObject(nearest);
            PoseV2WheelObject entry;
            entry.object = nearest;
            entry.vehicle = vehicle.packet.id;
            entry.model = prop->modelHash;
            entry.forProp = wheel.prop;
            g_wheelObjects.push_back(entry);
            g_wheelModels.insert(entry.model);
            ++g_wheelPlay.kept;
            const std::string label = replay::entity::IdText(replay::entity::ReplayId::FromPacked(wheel.prop));
            LogPoseV2Wheel("left_detach", entry, label.c_str(), timeMs);
        }
        return bits;
    }

    bool VehicleWheelsPending(const replay::playback::Puppet& vehicle)
    {
        if (vehicle.state != replay::playback::PuppetState::Ready || g_wheelsMade.count(vehicle.clone))
            return false;
        const replay::vehicle_pieces::WheelModels* models = PoseV2VehicleWheelModels(vehicle);
        if (!models || (!models->front && !models->rear))
            return false;
        // Every wheel slot of the vehicle has its object. The wheel count s32
        // [vehicle+0xBC0] (frag_observer's signature) grows as the wheel maker
        // makes them (its increment): complete when it reached the most
        // wheels a puppet of the model made, or, the model's first puppet, when
        // it stopped growing for kWheelCountSteadyMs. Unreadable: any object.
        std::int32_t count = 0;
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(vehicle.clone));
        if (!address || !replay::mem::Copy(&count, address + 0xBC0, sizeof(count)) || count < 0 ||
            count > replay::vehicle_pieces::kMaxWheelSlots)
            count = 0;
        int made = 0;
        for (int slot = 0; slot < (count ? count : replay::vehicle_pieces::kMaxWheelSlots); ++slot)
        {
            replay::vehicle_pieces::WheelRead read;
            const char* error = ReadPoseV2WheelObject(vehicle.clone, slot, read);
            if (error && *error)
                break;
            made += read.object ? 1 : 0;
        }
        const ULONGLONG now = GetTickCount64();
        const auto since = g_wheelsPendingSince.emplace(vehicle.clone, now);
        WheelCountSeen& seen = g_wheelCountSeen[vehicle.clone];
        if (seen.count != count)
        {
            seen.count = count;
            seen.since = now;
        }
        std::int32_t& most = g_modelWheelCount[vehicle.packet.modelHash];
        const bool known = most > 0;
        most = (std::max)(most, count);
        const bool complete = count ? made == count && (known ? count >= most : now - seen.since >= kWheelCountSteadyMs) : made > 0;
        if (complete || now - since.first->second >= kWheelsPendingMaxMs)
        {
            g_wheelsMade.insert(vehicle.clone);
            static std::uint32_t logs = 0; // per session
            if (logs < 48)
            {
                ++logs;
                Ctx().log << "POSE_V2_VEHICLE_WHEELS_MADE,id=" << replay::entity::IdText(vehicle.packet.id) << ",count=" << count
                          << ",made=" << made << ",waited_ms=" << (now - since.first->second) << ",complete=" << (complete ? 1 : 0)
                          << '\n';
            }
            return false;
        }
        return true;
    }

    bool VehicleWheelPiecesAhead(replay::entity::ReplayId vehicle, std::uint32_t timeMs)
    {
        for (const auto& [piece, taken] : g_fragPieceParents)
            if (taken.vehicle == vehicle && taken.createMs > timeMs)
                return true;
        return false;
    }

#if defined(REDEMPTION_DEV)
    std::string BenchBoneWorld(Entity entity);

    void BenchWatchStands(const char* where, std::uint32_t timeMs)
    {
        static std::unordered_map<Object, Vector3> last;
        static std::uint32_t logs = 0;
        if (g_objectStands.empty())
        {
            last.clear();
            logs = 0;
            return;
        }
        for (const ObjectStand& stand : g_objectStands)
        {
            // A child rides its host; the others are frozen where they stood.
            if (stand.parentStand >= 0 || !stand.standIn || !replay::entities::Exists(stand.standIn))
                continue;
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(stand.standIn, TRUE, FALSE);
            const auto seen = last.find(stand.standIn);
            if (seen != last.end() && Distance2(seen->second, at) > 0.01f && logs < 48)
            {
                ++logs;
                char line[320];
                std::snprintf(line, sizeof(line),
                    "BENCH_STAND_MOVED,where=%s,handle=%d,model=0x%x,from=%.3f;%.3f;%.3f,to=%.3f;%.3f;%.3f,"
                    "attached=%d,visible=%d,playhead_ms=%u\n",
                    where, stand.standIn, stand.model, seen->second.x, seen->second.y,
                    seen->second.z, at.x, at.y, at.z, ENTITY::IS_ENTITY_ATTACHED(stand.standIn) ? 1 : 0,
                    ENTITY::IS_ENTITY_VISIBLE(stand.standIn) ? 1 : 0, timeMs);
                Ctx().log << line;
            }
            last[stand.standIn] = at;
        }
    }

    void BenchObjectCensus(const std::string& tag, bool playback, std::uint32_t timeMs, const float center[3], float radius)
    {
        static int objects[1024];
        const int listed = (std::min)(ListObjects(objects, 1024), 1024);
        const auto handleAt = [&](std::uint64_t address) -> Object {
            for (int n = 0; address && n < listed; ++n)
                if (objects[n] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(objects[n])) == address)
                    return objects[n];
            return 0;
        };
        const auto line = [&](const char* kind, const std::string& id, Entity entity, std::uint32_t model, const char* extra) {
            const bool exists = entity && replay::entities::Exists(entity);
            const Vector3 p = exists ? ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE) : Vector3{};
            if (exists && radius > 0.0f)
            {
                const float dx = p.x - center[0], dy = p.y - center[1], dz = p.z - center[2];
                if (dx * dx + dy * dy + dz * dz > radius * radius)
                    return;
            }
            float sx = -1.0f, sy = -1.0f;
            const bool onScreen = exists && GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(p.x, p.y, p.z, &sx, &sy);
            char text[400];
            std::snprintf(text, sizeof(text),
                "BENCH_OBJECT_CENSUS,phase=%s,tag=%s,ms=%u,kind=%s,id=%s,handle=%d,model=0x%x,exists=%d,pos=%.3f;%.3f;%.3f,"
                "visible=%d,attached=%d,screen=%.4f;%.4f,on_screen=%d%s",
                playback ? "play" : "rec", tag.c_str(), timeMs, kind, id.c_str(), entity, model, exists ? 1 : 0, p.x, p.y, p.z,
                exists && ENTITY::IS_ENTITY_VISIBLE(entity) ? 1 : 0, exists && ENTITY::IS_ENTITY_ATTACHED(entity) ? 1 : 0, sx, sy,
                onScreen ? 1 : 0, extra);
            // The bones' world places of an object with a skeleton (a drawer, a
            // lid): live and replay compared bone by bone.
            const bool boned = std::strcmp(kind, "vehicle") != 0 && std::strcmp(kind, "wheel_slot") != 0;
            Ctx().log << text << (exists && boned ? BenchBoneWorld(entity) : std::string()) << '\n';
        };
        // The wreck: the lost wheels (_IS_VEHICLE_WHEEL_DESTROYED takes a slot)
        // and each wheel slot object, as "<vehicle>/slotN".
        const auto vehicleLines = [&](const std::string& id, Entity vehicle) {
            if (!vehicle || !replay::entities::Exists(vehicle) || !ENTITY::IS_ENTITY_A_VEHICLE(vehicle))
                return;
            unsigned slotsOff = 0;
            for (int slot = 0; slot < 10; ++slot)
                if (VEHICLE::_IS_VEHICLE_WHEEL_DESTROYED(vehicle, slot))
                    slotsOff |= 1u << slot;
            char extra[96];
            std::snprintf(extra, sizeof(extra), ",wheel_slots_off=0x%x,tint=%d", slotsOff, VEHICLE::_GET_VEHICLE_TINT(vehicle));
            line("vehicle", id, vehicle, static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(vehicle)), extra);
            for (int slot = 0; slot < replay::vehicle_pieces::kMaxWheelSlots; ++slot)
            {
                replay::vehicle_pieces::WheelRead read;
                const char* error = ReadPoseV2WheelObject(vehicle, slot, read);
                if ((error && *error) || !read.object)
                    continue;
                const Object wheel = handleAt(read.object);
                std::snprintf(extra, sizeof(extra), ",of=%s,slot=%d,listed=%d,on_vehicle=%d", id.c_str(), slot, wheel ? 1 : 0,
                    wheel && ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(wheel, vehicle) ? 1 : 0);
                line("wheel_slot", id + "/slot" + std::to_string(slot), wheel, read.model, extra);
            }
        };
        // Every other object drawn there (stage, scenery, debris), live and replay
        // matched by model and place: what one side has and the other lacks.
        std::unordered_set<Entity> named;
        const auto worldLines = [&]() {
            for (int n = 0; n < listed; ++n)
            {
                const Object object = objects[n];
                if (!object || named.count(object) || !replay::entities::Exists(object) || !ENTITY::IS_ENTITY_VISIBLE(object))
                    continue;
                char extra[64];
                std::snprintf(extra, sizeof(extra), ",mission=%d,owner=%u", ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) ? 1 : 0,
                    ReadEntityOwner(object));
                line("world", "world", object, static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)), extra);
            }
        };
        if (!playback)
        {
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                named.insert(entity.handle);
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                named.insert(weapon.object);
            for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                named.insert(prop.object);
            worldLines();
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                if (entity.vehicle)
                    vehicleLines(replay::entity::IdText(entity.id), entity.handle);
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                if (replay::entities::Exists(weapon.object))
                    line("attached", replay::entity::IdText(weapon.id), weapon.object,
                        static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(weapon.object)), "");
            for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                if (replay::entities::Exists(prop.object))
                    line("prop", replay::entity::IdText(prop.id), prop.object,
                        static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(prop.object)), "");
            return;
        }
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            named.insert(puppet.clone);
        worldLines();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            const std::string id = replay::entity::IdText(puppet.packet.id);
            if (puppet.packet.kind == replay::entity::EntityKind::Vehicle)
                vehicleLines(id, puppet.clone);
            else if (puppet.packet.kind == replay::entity::EntityKind::Weapon || puppet.packet.kind == replay::entity::EntityKind::Prop)
            {
                char extra[64];
                const auto shaped = g_fragPieceShaped.find(puppet.packet.id.Packed());
                // A piece broken off a wheel object, or a wheel object adopted (a
                // loose wheel): drawn as the vehicle draws its wheels.
                const bool fromWheel = (g_fragPieceParents.count(puppet.packet.id.Packed()) && shaped != g_fragPieceShaped.end() &&
                    shaped->second == puppet.clone) ||
                    std::any_of(g_wheelObjects.begin(), g_wheelObjects.end(), [&](const PoseV2WheelObject& entry) {
                        return puppet.clone && entry.object == puppet.clone && entry.holder == puppet.packet.id.Packed();
                    });
                std::snprintf(extra, sizeof(extra), ",state=%d,from_wheel=%d", static_cast<int>(puppet.state), fromWheel ? 1 : 0);
                line(puppet.packet.kind == replay::entity::EntityKind::Weapon ? "attached" : "prop", id, puppet.clone,
                    puppet.packet.modelHash, extra);
            }
        }
    }
    namespace
    {

    // Bench loot watch: every object within the radius, sampled every 100 ms,
    // one line per change (first sight, gone, visibility, attach, root move,
    // skeleton built or rebuilt, bone pose away from the first sight).
        struct LootWatchState
        {
            std::uint32_t model = 0;
            std::uint32_t owner = 0;
            bool visible = false;
            Entity parent = 0;
            Vector3 position{};
            Vector3 rotation{};
            std::uint32_t bones = 0;
            std::vector<float> firstLocals;
            float loggedMoveM = 0.0f;
            float loggedTurnDeg = 0.0f;
            bool seen = false;
        };
        std::unordered_map<Object, LootWatchState> g_lootWatchObjects;
        std::uint64_t g_lootWatchNext = 0;
        std::uint64_t g_lootWatchStart = 0;
        std::uint32_t g_lootWatchLines = 0;

        // The largest bone translation (m) and rotation (deg) of `now` against
        // `from` (BoneQts: quaternion, translation, scale, four floats each).
        void LocalsDelta(const std::vector<float>& from, const std::vector<float>& now, std::uint32_t bones, float& moveM,
            float& turnDeg)
        {
            moveM = 0.0f;
            turnDeg = 0.0f;
            const std::size_t per = replay::object_pose::spawn::kFloatsPerBone;
            if (from.size() < bones * per || now.size() < bones * per)
                return;
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                const float* a = from.data() + bone * per;
                const float* b = now.data() + bone * per;
                const float dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
                const float angle = 2.0f * std::acos((std::min)(1.0f, dot)) * 57.29578f;
                const float dx = a[4] - b[4], dy = a[5] - b[5], dz = a[6] - b[6];
                moveM = (std::max)(moveM, std::sqrt(dx * dx + dy * dy + dz * dz));
                turnDeg = (std::max)(turnDeg, std::isfinite(angle) ? angle : 0.0f);
            }
        }
    }

    void BenchLootWatchReset()
    {
        g_lootWatchObjects.clear();
        g_lootWatchNext = 0;
        g_lootWatchStart = GetTickCount64();
        g_lootWatchLines = 0;
    }

    void BenchLootWatch(const float center[3], float radius)
    {
        const std::uint64_t now = GetTickCount64();
        if (now < g_lootWatchNext)
            return;
        g_lootWatchNext = now + 100;
        static int objects[1024];
        const int listed = (std::min)(ListObjects(objects, 1024), 1024);
        std::unordered_set<Object> present;
        const auto log = [&](const char* event, Object object, const LootWatchState& state, float moveM, float turnDeg) {
            if (g_lootWatchLines >= 4000)
                return;
            ++g_lootWatchLines;
            const bool known = IsPoseV2KnownObject(object);
            char line[512];
            std::snprintf(line, sizeof(line),
                "BENCH_LOOT_OBJ,ev=%s,t_ms=%llu,rec_ms=%u,handle=%d,model=0x%x,owner=%u,visible=%d,parent=%d,pos=%.3f;%.3f;%.3f,"
                "rot=%.1f;%.1f;%.1f,bones=%u,pose_move_m=%.3f,pose_turn_deg=%.1f,recorded=%s,scenery=%d\n",
                event, static_cast<unsigned long long>(now - g_lootWatchStart), replay::pose::capture::NowMs(), object,
                state.model, state.owner, state.visible ? 1 : 0, state.parent, state.position.x, state.position.y,
                state.position.z, state.rotation.x, state.rotation.y, state.rotation.z, state.bones, moveM, turnDeg,
                known ? replay::entity::IdText(KnownObjectId(object)).c_str() : "-", g_sceneryOf.count(object) ? 1 : 0);
            Ctx().log << line;
        };
        static std::vector<float> locals;
        for (int n = 0; n < listed; ++n)
        {
            const Object object = objects[n];
            if (!object || !replay::entities::Exists(object))
                continue;
            float at[3]{};
            if (!replay::entities::Position(object, at))
                continue;
            const float dx = at[0] - center[0], dy = at[1] - center[1], dz = at[2] - center[2];
            if (dx * dx + dy * dy + dz * dz > radius * radius)
                continue;
            present.insert(object);
            LootWatchState& state = g_lootWatchObjects[object];
            LootWatchState was = state;
            state.model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            state.owner = ReadEntityOwner(object);
            state.visible = ENTITY::IS_ENTITY_VISIBLE(object) != FALSE;
            state.parent = ENTITY::GET_ENTITY_ATTACHED_TO(object);
            state.position = Vector3{at[0], at[1], at[2]};
            state.rotation = ENTITY::GET_ENTITY_ROTATION(object, 2);
            std::uint32_t bones = 0;
            const bool read = ReadPoseV2SpawnLocals(object, bones, locals);
            state.bones = read ? bones : 0;
            if (!was.seen)
            {
                state.seen = true;
                if (read)
                    state.firstLocals = locals;
                log("seen", object, state, 0.0f, 0.0f);
                continue;
            }
            float moveM = 0.0f, turnDeg = 0.0f;
            if (read && state.firstLocals.empty())
                state.firstLocals = locals;
            else if (read && state.firstLocals.size() == locals.size())
                LocalsDelta(state.firstLocals, locals, bones, moveM, turnDeg);
            if (state.visible != was.visible)
                log("visible", object, state, moveM, turnDeg);
            if (state.parent != was.parent)
                log("attach", object, state, moveM, turnDeg);
            if (state.bones != was.bones)
            {
                if (read && state.firstLocals.size() != locals.size())
                    state.firstLocals = locals;
                log("bones", object, state, moveM, turnDeg);
            }
            const float mx = state.position.x - was.position.x, my = state.position.y - was.position.y,
                        mz = state.position.z - was.position.z;
            // An attached object moves with its holder: only its attach, its
            // visibility and its end are told.
            if (!state.parent &&
                (mx * mx + my * my + mz * mz > 0.01f * 0.01f || std::fabs(state.rotation.z - was.rotation.z) > 1.0f ||
                    std::fabs(state.rotation.x - was.rotation.x) > 1.0f || std::fabs(state.rotation.y - was.rotation.y) > 1.0f))
                log("moved", object, state, moveM, turnDeg);
            if (!state.parent && (std::fabs(moveM - was.loggedMoveM) > 0.005f || std::fabs(turnDeg - was.loggedTurnDeg) > 2.0f))
            {
                state.loggedMoveM = moveM;
                state.loggedTurnDeg = turnDeg;
                log("pose", object, state, moveM, turnDeg);
            }
            else
            {
                state.loggedMoveM = was.loggedMoveM;
                state.loggedTurnDeg = was.loggedTurnDeg;
            }
        }
        for (auto it = g_lootWatchObjects.begin(); it != g_lootWatchObjects.end();)
        {
            if (present.count(it->first))
            {
                ++it;
                continue;
            }
            log(replay::entities::Exists(it->first) ? "left" : "gone", it->first, it->second, 0.0f, 0.0f);
            it = g_lootWatchObjects.erase(it);
        }
    }

    // The world position of each bone (at most 24) of an object with a
    // skeleton, for the census (live and replay compared bone by bone).
    std::string BenchBoneWorld(Entity entity)
    {
        replay::skeleton::ResolvedPoseSkeleton skeleton{};
        if (!entity || !replay::entities::Exists(entity) || !replay::skeleton::FastResolvePedPoseSkeleton(entity, skeleton) ||
            skeleton.boneCount < replay::object_pose::kMinBones || skeleton.boneCount > 64)
            return std::string();
        std::string out = ",bones=" + std::to_string(skeleton.boneCount) + ",bw=";
        const std::uint32_t count = (std::min)(skeleton.boneCount, 24u);
        char part[64];
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            const Vector3 p = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(entity, static_cast<int>(bone));
            std::snprintf(part, sizeof(part), "%s%.3f;%.3f;%.3f", bone ? "|" : "", p.x, p.y, p.z);
            out += part;
        }
        return out;
    }
#endif

    bool ReturnWheelObject(Object object)
    {
        if (!g_wheelObjectsOpen || !object)
            return false;
        for (std::size_t n = 0; n < g_wheelObjects.size(); ++n)
        {
            PoseV2WheelObject& entry = g_wheelObjects[n];
            if (entry.object != object || !entry.holder)
                continue;
            if (!PoseV2WheelObjectAlive(entry))
            {
                g_wheelObjects.erase(g_wheelObjects.begin() + static_cast<std::ptrdiff_t>(n));
                return false;
            }
            const std::string holder = replay::entity::IdText(replay::entity::ReplayId::FromPacked(entry.holder));
            HidePoseV2WheelObject(object);
            entry.holder = 0;
            ++g_wheelPlay.returned;
            LogPoseV2Wheel("return", entry, holder.c_str(), g_wheelPlayheadMs);
            return true;
        }
        return false;
    }

    void VehiclePieceStates(std::vector<replay::vehicle_pieces::PieceState>& out)
    {
        // Every piece of the clip, alive or not yet (the break's groups are
        // due a tick before the piece's object was found), with the state
        // of its puppet when it has one.
        out.clear();
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
        {
            if (packet.kind != replay::entity::EntityKind::Prop ||
                packet.propOrigin != replay::entity::PropOrigin::VehiclePiece)
                continue;
            replay::vehicle_pieces::PieceState state;
            state.owner = packet.owner;
            state.group = replay::vehicle_pieces::GroupOf(packet);
            state.createMs = packet.timeMs;
            if (const replay::playback::Puppet* puppet = Ctx().puppets.Find(packet.id))
            {
                state.alive = true;
                state.made = puppet->state == replay::playback::PuppetState::Ready && puppet->clone != 0;
                state.failed = puppet->state == replay::playback::PuppetState::Failed;
            }
            out.push_back(state);
        }
    }

    // Fragment breaks in playback: each world object puppet gets
    // the recorded groups up to the playhead; a scrub back fixes the puppet
    // whole (FIX_OBJECT_FRAGMENT) and applies them again. Groups go by index
    // to BREAK_OBJECT_FRAGMENT_CHILD with deletion (no new piece: the
    // recorded pieces are props of their own).

    // BREAK_OBJECT_FRAGMENT_CHILD takes a child, not a group (a group gives
    // wrong pieces). The group's first child comes from the clone's fragment
    // type, guarded reads (offline): inst [ent+0x38] (vtable kFragInstVtable),
    // type [inst+0x138], LOD group [type+0x90], LOD u32 [inst+0x178] (0..2),
    // physLOD [lg+0x10+8*lod]; groups u16 [pl+0x140], children s16 [pl+0x146],
    // group g [[pl+0xC8]+8*g], its first child u16 [grp+8] and count u16
    // [grp+0xA]; the inverse table u16 [[pl+0x110]+2*child] must give g back.
    constexpr bool kPlayFragBreaks = true;

    // After a break: the clone's cache entry [inst+0x128] (0: nothing broke,
    // every group present) and whether the group is gone (s16
    // [[cache+0x148]+2*g] == -1, as IsGroupBroken reads it). 1 gone, 0
    // present, -1 no cache entry, -2 unreadable.
    int GuardedGroupGone(std::uint64_t entity, std::uint16_t group)
    {
        __try
        {
            const std::uint64_t inst = *reinterpret_cast<const std::uint64_t*>(entity + 0x38);
            const std::uint64_t cache = inst ? *reinterpret_cast<const std::uint64_t*>(inst + 0x128) : 0;
            if (!cache)
                return -1;
            const std::uint64_t slots = *reinterpret_cast<const std::uint64_t*>(cache + 0x148);
            return slots && *reinterpret_cast<const std::int16_t*>(slots + 2ull * group) == -1 ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -2;
        }
    }
    // `fragVtable`: kFragInstVtable (0: not in this game, nothing is read).
    int GuardedGroupToChild(std::uint64_t entity, std::uintptr_t fragVtable, std::uint16_t group)
    {
        if (!fragVtable)
            return -2;
        __try
        {
            const std::uint64_t inst = *reinterpret_cast<const std::uint64_t*>(entity + 0x38);
            if (!inst || *reinterpret_cast<const std::uint64_t*>(inst) != fragVtable)
                return -2;
            const std::uint64_t type = *reinterpret_cast<const std::uint64_t*>(inst + 0x138);
            const std::uint64_t lodGroup = type ? *reinterpret_cast<const std::uint64_t*>(type + 0x90) : 0;
            const std::uint32_t lod = *reinterpret_cast<const std::uint32_t*>(inst + 0x178);
            if (!lodGroup || lod > 2)
                return -3;
            const std::uint64_t pl = *reinterpret_cast<const std::uint64_t*>(lodGroup + 0x10 + 8ull * lod);
            if (!pl)
                return -3;
            const std::uint16_t groups = *reinterpret_cast<const std::uint16_t*>(pl + 0x140);
            const std::int16_t children = *reinterpret_cast<const std::int16_t*>(pl + 0x146);
            if (group >= groups)
                return -4;
            const std::uint64_t table = *reinterpret_cast<const std::uint64_t*>(pl + 0xC8);
            const std::uint64_t grp = table ? *reinterpret_cast<const std::uint64_t*>(table + 8ull * group) : 0;
            if (!grp)
                return -4;
            const std::uint16_t first = *reinterpret_cast<const std::uint16_t*>(grp + 8);
            const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(grp + 0xA);
            if (!count || first + count > children)
                return -5;
            const std::uint64_t inverse = *reinterpret_cast<const std::uint64_t*>(pl + 0x110);
            if (!inverse || *reinterpret_cast<const std::uint16_t*>(inverse + 2ull * first) != group)
                return -6;
            return first;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The root groups of the recorded fragment pieces of a wheel's model that
    // started within kFragParentM of where the puppet was then.
    bool PoseV2FragPieceTakes(const replay::playback::Puppet& puppet, std::uint16_t group)
    {
        // Only a vehicle's wheel (a crate's pieces are made from a fresh
        // object and the crate loses the groups by its breaks).
        if (std::none_of(g_vehicleWheelModels.begin(), g_vehicleWheelModels.end(), [&](const auto& entry) {
                return replay::vehicle_pieces::IsVehicleWheelChild(puppet.packet.modelHash, entry.second.models);
            }))
            return false;
        auto found = g_fragPieceGroupsOf.find(puppet.packet.id.Packed());
        if (found == g_fragPieceGroupsOf.end())
        {
            std::vector<std::uint16_t> groups;
            const replay::entity::RelativeTrack* own = Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed());
            for (const replay::FragPiece& row : Ctx().clip.fragPieces)
            {
                const replay::entity::CreatePacket* piece = Ctx().clip.FindCreate(row.id.Packed());
                const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(row.id.Packed());
                std::uint32_t firstMs = 0, lastMs = 0;
                replay::entity::TransformSample first, then;
                if (!piece || piece->modelHash != puppet.packet.modelHash || !(piece->id != puppet.packet.id) || !own || !track ||
                    !track->Span(firstMs, lastMs) || !track->Sample(firstMs, first) || !own->Sample(piece->timeMs, then))
                    continue;
                const float dx = first.relative.position.x - then.relative.position.x;
                const float dy = first.relative.position.y - then.relative.position.y;
                const float dz = first.relative.position.z - then.relative.position.z;
                if (dx * dx + dy * dy + dz * dz <= kFragParentM * kFragParentM)
                    groups.push_back(static_cast<std::uint16_t>(replay::fragpiece::Root(row.groups)));
            }
            found = g_fragPieceGroupsOf.emplace(puppet.packet.id.Packed(), std::move(groups)).first;
        }
        if (found->second.empty())
            return false;
        // The group or one above it is a piece's root: the piece takes it along
        // and gives up the groups it did not carry (PoseV2FragPieceFromParent).
        static replay::frag::ObjectFragState state; // script thread only
        if (!replay::frag::ReadObjectFrag(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), state) ||
            group >= state.groups)
            return std::find(found->second.begin(), found->second.end(), group) != found->second.end();
        for (int walk = group; walk > 0 && walk < state.groups; walk = state.parents[walk])
            if (std::find(found->second.begin(), found->second.end(), walk) != found->second.end())
                return true;
        return false;
    }

    void PlayPoseV2FragBreaks(std::uint32_t timeMs)
    {
        if (!kPlayFragBreaks || Ctx().clip.fragBreaks.empty())
            return;
        g_fragMaxPlayheadMs = (std::max)(g_fragMaxPlayheadMs, timeMs);
        const std::uintptr_t fragVtable = sig::Find(kFragInstVtable);
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            // Off for vehicles: BREAK_OBJECT_FRAGMENT_CHILD with the object's
            // group map breaks nothing on a burning wagon (gone=0 every time)
            // and the game can crash a second later in the engine's own update
            // (no mod frame). The breaks are still recorded. Offline, the
            // native's handler takes the entity through the object-typed
            // getter, which gives null for a
            // vehicle: the call never reaches the engine (gone=0). Wheels and
            // doors have their own path (PlayPoseV2VehicleParts); a vehicle's
            // other groups (boards, canvas, cargo, the wreck) are played by
            // PlayPoseV2VehicleFrags (replay_playback.cpp) through the engine's
            // DeleteAboveGroup, not here.
            constexpr bool kVehicleFragBreaks = false;
            const bool vehicle = kVehicleFragBreaks && puppet.packet.kind == replay::entity::EntityKind::Vehicle;
            // The breaks of any recorded object prop, not only of
            // a world object of the start snapshot: the crate broken by the
            // cavalry in 053 was first seen after the snapshot and recorded as
            // a free object (Projectile), and its 14 breaks were never played
            // (ReplayClipTool objects: frag_unplayed). Vehicle pieces break
            // their own way.
            if ((!vehicle && (puppet.packet.kind != replay::entity::EntityKind::Prop ||
                    puppet.packet.propOrigin == replay::entity::PropOrigin::VehiclePiece)) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            const std::uint32_t key = puppet.packet.id.Packed();
            std::size_t due = 0;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
                if (e.id == puppet.packet.id && e.timeMs <= timeMs)
                    ++due;
            std::size_t& applied = g_fragApplied[key];
            Object& on = g_fragPuppet[key];
            if (on != puppet.clone)
            {
                on = puppet.clone;
                applied = 0;
            }
            if (due < applied)
            {
                // Back before a break: the puppet is rebuilt whole (FIX only
                // acts on a cached inst) and the breaks up to now come again.
                Object object = puppet.clone;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&object);
                puppet.clone = 0;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                ++g_fragFixes;
                applied = 0;
                on = 0;
                continue;
            }
            if (due == applied)
                continue;
            std::size_t index = 0;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
            {
                if (!(e.id == puppet.packet.id) || e.timeMs > timeMs)
                    continue;
                if (index++ < applied)
                    continue;
                // The root group: the object is destroyed and not drawn
                // (UpdatePoseV2Props), nothing to break.
                if (e.group == 0)
                    continue;
                // A group a recorded fragment piece of this object carries is that
                // piece's to break off (PoseV2FragPieceFromParent): the recorder found
                // the piece's object a few ticks after the break.
                if (PoseV2FragPieceTakes(puppet, e.group))
                    continue;
                const int child = GuardedGroupToChild(
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), fragVtable, e.group);
                if (child < 0)
                {
                    ++g_fragUnmapped;
                    g_fragLastError = child;
                    continue;
                }
                OBJECT::BREAK_OBJECT_FRAGMENT_CHILD(puppet.clone, child, TRUE);
                ++g_fragBreaksPlayed;
                if (g_fragBreaksPlayed <= 16)
                    Ctx().log << "POSE_V2_PLAYBACK_FRAG,id=" << replay::entity::IdText(puppet.packet.id) << ",group=" << e.group
                        << ",child=" << child << ",gone=" << GuardedGroupGone(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), e.group)
                        << ",broken_flag=" << (OBJECT::HAS_OBJECT_BEEN_BROKEN(puppet.clone) ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
            }
            applied = due;
        }
    }

    // The stand-ins due at the playhead not built yet (the start
    // waits for them like for the map's doors, object_check.h SceneryReady).
    std::uint32_t PendingObjectStands(std::uint32_t timeMs)
    {
        std::uint32_t pending = 0;
        for (const ObjectStand& stand : g_objectStands)
        {
            if (stand.failed || stand.hatHidden || (stand.standIn && replay::entities::Exists(stand.standIn)))
                continue;
            if (stand.mapDressing && stand.mapVerdict != replay::objcheck::MapDressing::Build)
                continue; // left to the map, or not decided yet
            if (stand.parentStand >= 0)
            {
                // A child waits only once its parent is drawn.
                replay::objcheck::ChildHost kind = replay::objcheck::ChildHost::None;
                if (static_cast<std::size_t>(stand.parentStand) >= g_objectStands.size() ||
                    !PoseV2ChildHost(g_objectStands[static_cast<std::size_t>(stand.parentStand)], kind))
                    continue;
            }
            const replay::entity::CreatePacket* create = stand.prop.Valid() ? Ctx().clip.FindCreate(stand.prop.Packed()) : nullptr;
            if (replay::objcheck::SceneryShown(stand.appearMs, stand.goneMs, create != nullptr, create ? create->timeMs : 0, true, timeMs) &&
                replay::objcheck::VisibleAt(stand.visibleWhenRecorded, stand.visibleFlipsMs, timeMs)) // a hidden one is not waited for
                ++pending;
        }
        return pending;
    }

    void UpdatePropPuppets(std::uint32_t timeMs, std::uint32_t& creates)
    {
        // Each vehicle puppet's wheel objects are looked at from its build on
        // (VehicleWheelsPending: a wreck that lost wheels later was whole once).
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.packet.kind == replay::entity::EntityKind::Vehicle)
                VehicleWheelsPending(puppet);
        UpdatePoseV2Props(timeMs, creates);
        UpdatePoseV2WorldStandIns(timeMs);
        PlayPoseV2FragBreaks(timeMs);
    }
    void DeleteAttachHelper() { DeletePoseV2AttachHelper(); }

    // See the header.
    void PlayRecordedPose(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        PlayPoseV2ObjectPoseBody(puppet, timeMs);
    }
}
