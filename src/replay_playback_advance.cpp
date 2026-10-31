// replay_playback_advance.cpp: the puppet tick: the override refresh, the pose write check, advance, segment and vehicle record switches, attachments, ropes.
// Split from replay_playback.cpp (moved code); shared internals in replay_playback_internal.h.

#include "replay_playback_internal.h"

namespace replay::playback
{
    namespace
    {
    constexpr std::uint64_t kPoseV2HoldMaxMs = 600;
    }

    // Every tick: the armed skeleton must be the one the puppet has now, and
    // the record the segment's once the skeleton has its bones. Otherwise
    // the override is armed again in the same tick and written before the
    // render.
    void RefreshPoseV2Override(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        // An exact-armed body's signature checked again when its layout at the
        // playhead changes or layers went on it.
        if (puppet.overrideSlot >= 0 && (!puppet.mapped || puppet.sigMapped) && puppet.sigRecheckAt &&
            GetTickCount64() >= puppet.sigRecheckAt &&
            (puppet.packet.kind == replay::entity::EntityKind::Ped || puppet.packet.kind == replay::entity::EntityKind::Horse))
        {
            RecheckPoseV2Signature(puppet, timeMs);
        }
        if (puppet.overrideSlot >= 0)
        {
            replay::skeleton::ResolvedPoseSkeleton resolved{};
            const bool ok = replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved);
            const bool skeletonChanged = !ok || resolved.skeleton != puppet.armedSkeleton ||
                resolved.boneCount != puppet.armedBones;
            const bool segmentArrived = ok && puppet.recordId &&
                puppet.armedRecordId != puppet.recordId && resolved.boneCount == puppet.boneCount;
            // A mapped arm follows the segment at the playhead (its map was
            // built from the record it armed with), by the segment it was armed
            // in (a signature map may pose from another record of the entity
            // with the skeleton's bones, which is not the segment's).
            const bool mapStale = ok && puppet.mapped && puppet.recordId && puppet.mapSegmentRecord != puppet.recordId;
            // An exact arm follows the change in this tick, the new pose
            // written now (segment_switch.h).
            bool rearmed = false;
            if ((skeletonChanged || segmentArrived) && !puppet.mapped)
            {
                replay::segswitch::Facts facts;
                facts.armed = true;
                facts.resolved = ok;
                facts.samePointer = ok && resolved.skeleton == puppet.armedSkeleton;
                facts.armedBones = puppet.armedBones;
                facts.cloneBones = ok ? resolved.boneCount : 0u;
                facts.armedRecord = puppet.armedRecordId;
                facts.segmentRecord = puppet.recordId;
                facts.segmentBones = puppet.boneCount;
                if (ok)
                    facts.recordForClone = resolved.boneCount == puppet.boneCount && puppet.recordId
                        ? puppet.recordId
                        : PoseV2RecordForBones(puppet.packet.id, timeMs, resolved.boneCount);
                facts.bodyBones = puppet.bodyBones;
                const replay::segswitch::Decision decision = replay::segswitch::Decide(facts);
                const bool holdPrefix = decision.action == replay::segswitch::Action::HoldPrefix;
                // A prefix hold going on is the same change, not a new one.
                const bool sameHold = holdPrefix && puppet.prefixHold.Active() && puppet.prefixHold.bones == facts.cloneBones;
                if (!holdPrefix && puppet.prefixHold.Active())
                    EndPoseV2PrefixHold(puppet, decision.why, ok ? resolved.boneCount : 0u, timeMs);
                if (!sameHold)
                {
                    puppet.switchGap.Change(decision.action);
                    ++g_poseV2Switch.switches;
                }
                if (decision.action == replay::segswitch::Action::RearmNow)
                    rearmed = RearmPoseV2InTick(puppet, resolved, decision.record, timeMs, decision.why);
                else if (decision.action == replay::segswitch::Action::Keep)
                    rearmed = true; // nothing to do this tick (wait_rebuild)
                else if (holdPrefix)
                    rearmed = HoldPoseV2Prefix(puppet, resolved, timeMs);
                if (!rearmed && (decision.action == replay::segswitch::Action::RearmNow || holdPrefix))
                    puppet.switchGap.Change(replay::segswitch::Action::Release); // the old path below: a hole
                if (!rearmed)
                    ++g_poseV2Switch.released;
            }
            // The skeleton back to the armed count: a prefix hold is over.
            if (puppet.prefixHold.Active() && !skeletonChanged && !segmentArrived)
                EndPoseV2PrefixHold(puppet, "skeleton_back", ok ? resolved.boneCount : 0u, timeMs);
            // A segment switch that held the previous record while the engine
            // rebuilds: not forever (no rebuild came: the old path).
            if (!rearmed && !skeletonChanged && !segmentArrived && !mapStale && puppet.holdSince &&
                puppet.armedRecordId != puppet.recordId && GetTickCount64() - puppet.holdSince > kPoseV2HoldMaxMs)
            {
                ++g_poseV2Switch.holdTimeouts;
                puppet.holdSince = 0;
                Ctx().log << "POSE_V2_PLAYBACK_REARM,id=" << replay::entity::IdText(puppet.packet.id)
                          << ",reason=hold_timeout,armed_bones=" << puppet.armedBones << ",track_bones=" << puppet.boneCount
                          << ",playhead_ms=" << timeMs << '\n';
                ReleasePoseV2Override(puppet);
                puppet.armFailureLogged = false;
                puppet.armDeadline = GetTickCount64() + 3000;
                puppet.lostToChange = true;
            }
            if (!rearmed && (skeletonChanged || segmentArrived || mapStale))
            {
                // What the released arm wrote (ArmOverride zeroes it): a
                // rebuilt skeleton written by the alternate or the guard, or
                // left to its bind pose (bone_mismatch only).
                const replay::pose::capture::OverrideStats released =
                    replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(puppet.overrideSlot));
                std::ostream& log = Ctx().log;
                log << "POSE_V2_PLAYBACK_REARM"
                    << ",id=" << replay::entity::IdText(puppet.packet.id)
                    << ",reason=" << (skeletonChanged ? "skeleton_changed" : segmentArrived ? "segment_skeleton" : "map_segment")
                    << ",mapped=" << (puppet.mapped ? 1 : 0)
                    << ",applies=" << released.applies << ",alt_applies=" << released.altApplies
                    << ",guard_applies=" << released.guardApplies << ",bone_mismatch=" << released.boneCountMismatch
                    << ",late_writes=" << released.lateWrites
                    << ",armed_bones=" << puppet.armedBones
                    << ",clone_bones=" << resolved.boneCount
                    << ",armed_skeleton=0x" << std::hex
                    << reinterpret_cast<std::uintptr_t>(puppet.armedSkeleton)
                    << ",skeleton=0x" << reinterpret_cast<std::uintptr_t>(resolved.skeleton)
                    << std::dec
                    << ",playhead_ms=" << timeMs
                    << '\n';
                log.flush();
                ReleasePoseV2Override(puppet);
                puppet.armFailureLogged = false;
                puppet.armDeadline = GetTickCount64() + 3000;
                // Posed until this change; no record for the new skeleton: the
                // map at once, not hidden.
                puppet.lostToChange = true;
            }
        }
        // The steps behind a pose mapped for want of a record
        // (no_record_fallback.h).
        if (puppet.overrideSlot >= 0 && puppet.mapped && !puppet.sigMapped && puppet.noRecord.active &&
            puppet.armedBones != puppet.boneCount)
        {
            StepPoseV2NoRecord(puppet, timeMs);
        }
        // A puppet that could not be armed stays hidden; once a second its
        // skeleton is looked at again, and a rebuilt one (another pointer or
        // bone count) is armed again, straight to the record or the map (the
        // layer retries are not repeated).
        if (puppet.overrideSlot < 0 && puppet.armFailureLogged && GetTickCount64() >= puppet.rearmProbeAt)
        {
            puppet.rearmProbeAt = GetTickCount64() + 1000;
            replay::skeleton::ResolvedPoseSkeleton resolved{};
            // A signature refusal is for one layout: the playhead on another
            // layout of the record arms again.
            std::uintptr_t layoutKey = 0;
            const bool layoutMoved = puppet.sigRefusedKey && puppet.recordId &&
                replay::pose::capture::TrackLayoutSignatureAt(puppet.recordId, timeMs, nullptr, nullptr, layoutKey) &&
                layoutKey != puppet.sigRefusedKey;
            if (replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved) &&
                (resolved.skeleton != puppet.failedSkeleton || resolved.boneCount != puppet.failedBones || layoutMoved))
            {
                puppet.sigRefusedKey = 0;
                Ctx().log << "POSE_V2_PLAYBACK_REARM,id=" << replay::entity::IdText(puppet.packet.id)
                          << ",reason=skeleton_after_refusal,failed_bones=" << puppet.failedBones
                          << ",clone_bones=" << resolved.boneCount << ",error=" << puppet.armError
                          << ",playhead_ms=" << timeMs << '\n';
                puppet.armFailureLogged = false;
                puppet.armDeadline = GetTickCount64() + 3000;
            }
        }
        // Arming stops after its deadline; the failure is logged once.
        if (puppet.overrideSlot < 0 && !puppet.armFailureLogged)
            ArmPoseV2Puppet(puppet, timeMs);
        puppet.switchGap.Tick(puppet.overrideSlot >= 0);
    }

    // Once a second per ped or horse puppet (4 times a second while it
    // differs), whether the pose the detour wrote is still in the skeleton at
    // the end of the frame. Evidence only, nothing changes: the skinned pelt of
    // a carcass (bones 259..502 of the 503-bone skeleton) is written every
    // frame (bone_mismatch=0) and still shows as a flat plane, so either the
    // engine rewrites those bones after us (lines with first..last in the pelt)
    // or the pelt mesh does not follow them (no line, write_changed=0). A line
    // on the first change, on each clean/changed flip and up to 24 changes in
    // between.
    void CheckPoseV2Write(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const std::uint32_t slot = static_cast<std::uint32_t>(puppet.overrideSlot);
        const std::uint64_t now = GetTickCount64();
        if (slot >= replay::pose::capture::kMaxOverrides || now < g_poseV2WriteCheckAt[slot])
            return;
        const replay::pose::capture::OverrideWriteCheck check = replay::pose::capture::CheckOverrideWrite(slot);
        if (!check.checked)
            return; // no detour write since the last publish; next tick
        const replay::pose::WrittenPoseChange& change = check.change;
        const bool changed = change.localBones + change.objectBones > 0;
        g_poseV2WriteCheckAt[slot] = now + (changed ? 250u : 1000u);
        const bool flipped = changed != g_poseV2WriteChanged[slot];
        g_poseV2WriteChanged[slot] = changed;
        if (!(flipped || (changed && g_poseV2WriteCheckLogs < 24)) || g_poseV2WriteCheckLogs >= 64)
            return;
        ++g_poseV2WriteCheckLogs;
        std::ostream& log = Ctx().log;
        const replay::logfmt::FormatScope logFormat(log); // precision not left behind
        log << std::fixed << std::setprecision(3)
            << "POSE_V2_PLAYBACK_WRITE_CHECK,id=" << replay::entity::IdText(puppet.packet.id)
            << ",state=" << (changed ? "changed" : "clean")
            << ",bones=" << check.boneCount
            << ",local_bones=" << change.localBones
            << ",object_bones=" << change.objectBones
            << ",first_bone=" << change.firstBone
            << ",last_bone=" << change.lastBone
            << ",worst_bone=" << change.worstBone
            << ",max_m=" << change.maxTranslation
            << ",max_rad=" << change.maxRotation
            << ",record=0x" << std::hex << puppet.armedRecordId << std::dec
            << ",playhead_ms=" << timeMs << '\n';
        log.unsetf(std::ios_base::floatfield);
        log.flush();
    }

    void LogPoseV2Puppet(const replay::playback::Puppet& puppet, const char* status, std::uint32_t timeMs)
    {
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_PUPPET"
            << ",id=" << replay::entity::IdText(puppet.packet.id)
            << ",kind=" << replay::entity::KindText(puppet.packet.kind)
            << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec
            << ",status=" << status
            << ",attempts=" << puppet.createAttempts
            << ",playhead_ms=" << timeMs
            << ",create_ms=" << puppet.packet.timeMs
            << '\n';
        log.flush();
    }

    void QueuePoseV2Puppet(const replay::entity::CreatePacket& packet)
    {
        replay::playback::Puppet puppet;
        puppet.packet = packet;
        puppet.recordId = packet.id.Packed();
        puppet.boneCount = packet.boneCount;
        puppet.stateSince = GetTickCount64();
        if (packet.weaponHash)
        {
            if (std::find(g_poseV2RequestedWeapons.begin(), g_poseV2RequestedWeapons.end(),
                    packet.weaponHash) == g_poseV2RequestedWeapons.end())
            {
                g_poseV2RequestedWeapons.push_back(packet.weaponHash);
            }
            replay::pose::playback::RequestWeaponAsset(packet.weaponHash);
            Ctx().puppets.list.push_back(puppet);
            return;
        }
        // A rope's own entity is never requested: the prop fails at once
        // (UpdatePoseV2Props, status rope_model).
        if (replay::ropemodels::IsRopeModel(packet.modelHash))
        {
            Ctx().puppets.list.push_back(puppet);
            return;
        }
        if (std::find(g_poseV2RequestedModels.begin(), g_poseV2RequestedModels.end(),
                packet.modelHash) == g_poseV2RequestedModels.end())
        {
            g_poseV2RequestedModels.push_back(packet.modelHash);
        }
        replay::pose::playback::RequestModel(packet.modelHash);
        Ctx().puppets.list.push_back(puppet);
    }
    namespace
    {

    const replay::VehicleLook* PoseV2VehicleLook(replay::entity::ReplayId id)
    {
        for (const replay::VehicleLook& look : Ctx().clip.vehicleLooks)
        {
            if (look.id == id)
                return &look;
        }
        return nullptr;
    }
    }

    // Loading -> Dressing -> Ready. Returns true when a ped was created.
    bool AdvancePoseV2Puppet(replay::playback::Puppet& puppet, std::uint32_t timeMs, bool mayCreate)
    {
        REPLAY_PROFILE_ZONE("play.puppet_advance");
        const ULONGLONG now = GetTickCount64();
        if (puppet.state == replay::playback::PuppetState::Loading)
        {
            if (!replay::pose::playback::RequestModel(puppet.packet.modelHash))
            {
                // A rope's entity recorded as a prop by an older clip, or a
                // model the game does not know, never loads
                // (objcheck::StandModelStep): failed at once, not after the
                // model timeout that held the start (QA logs: ROPE_REINS and
                // ROPE_REINS_ROPE props before puppets_ready=0 timeouts).
                const char* why = replay::ropemodels::IsRopeModel(puppet.packet.modelHash) ? "rope_model"
                    : !STREAMING::IS_MODEL_VALID(puppet.packet.modelHash)                ? "model_invalid"
                    : now - puppet.stateSince > replay::playback::kModelTimeoutMs        ? "model_timeout"
                                                                                         : nullptr;
                if (why)
                {
                    puppet.state = replay::playback::PuppetState::Failed;
                    LogPoseV2Puppet(puppet, why, timeMs);
                }
                return false;
            }
            const replay::VehicleLook* look = puppet.packet.kind == replay::entity::EntityKind::Vehicle
                ? PoseV2VehicleLook(puppet.packet.id) : nullptr;
            if (look)
            {
                // Both requested every tick until loaded; past the model
                // timeout the vehicle is created without them.
                const bool cargo = replay::pose::playback::RequestPropSet(look->propSet);
                const bool lights = replay::pose::playback::RequestPropSet(look->lightPropSet);
                if (!(cargo && lights) && now - puppet.stateSince <= replay::playback::kModelTimeoutMs)
                    return false;
            }
            if (!mayCreate || now < puppet.nextCreateAt)
                return false;
            replay::pose::WorldTransform at{};
            const replay::entity::PoseSegment* segment = PoseV2SegmentAt(puppet.packet.id, timeMs);
            const std::uint32_t createRecord = segment ? segment->recordId : puppet.packet.id.Packed();
            if (!replay::pose::capture::SampleTrack(createRecord, timeMs, g_poseV2Sample) ||
                !SampleWorldTransform(g_poseV2Sample, at))
            {
                // A ped registered while the engine was still building it
                // (spawned a frame or two before the entity scan) has a create
                // record without a track (a few captures with the old bone
                // count, no layout) and its pose starts in the next segment
                // ~150 ms later. Failing here would leave the puppet uncreated
                // for the whole playback. While a later segment of the entity
                // exists the create waits for it: SegmentAt picks it up once
                // the playhead reaches its start.
                if (PoseV2RecordHasSuccessor(puppet.packet.id, createRecord))
                {
                    const std::uint32_t packed = puppet.packet.id.Packed();
                    if (std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(), packed) ==
                        g_poseV2WaitSegmentLogged.end())
                    {
                        g_poseV2WaitSegmentLogged.push_back(packed);
                        LogPoseV2Puppet(puppet, "wait_segment", timeMs);
                    }
                    return false;
                }
                puppet.state = replay::playback::PuppetState::Failed;
                LogPoseV2Puppet(puppet, "no_track", timeMs);
                return false;
            }
            ++puppet.createAttempts;
            const bool vehicle = puppet.packet.kind == replay::entity::EntityKind::Vehicle;
            // The latest recorded paint and livery (VEHL v2), else the create
            // packet's (older clips).
            const std::int32_t wantTint = look && look->tint != replay::kLookUnset
                ? look->tint : puppet.packet.appearance.vehicleTint;
            const std::int32_t wantLivery = look && look->livery != replay::kLookUnset
                ? look->livery : puppet.packet.appearance.vehicleLivery;
            puppet.clone = vehicle
                ? replay::pose::playback::CreateVehiclePuppet(
                    puppet.packet.modelHash,
                    wantTint,
                    wantLivery,
                    at)
                : replay::pose::playback::CreatePuppet(
                    puppet.packet.modelHash,
                    puppet.packet.kind == replay::entity::EntityKind::Horse,
                    at);
            puppet.placed = false;
            puppet.visibleState = -1;
            if (!puppet.clone)
            {
                puppet.nextCreateAt = now + replay::playback::kCreateRetryMs;
                // The engine refused the entity (a pool full of the world's
                // population): the world clear runs on the next tick, not in up
                // to 250 ms, to free room before the creates. Evidence in
                // POSE_V2_CREATE_REFUSED and PUPPET_WATCH_SUMMARY.
                g_poseV2NextWorldScanAt = 0;
                ++g_poseV2CreateRefused;
                if (g_poseV2CreateRefusedLogs < 32)
                {
                    ++g_poseV2CreateRefusedLogs;
                    Ctx().log << "POSE_V2_CREATE_REFUSED,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex
                              << puppet.packet.modelHash << std::dec << ",attempts=" << puppet.createAttempts
                              << ",model_loaded=" << (STREAMING::HAS_MODEL_LOADED(puppet.packet.modelHash) ? 1 : 0)
                              << ",puppets=" << Ctx().puppets.list.size() << ",world_deleted_total=" << g_poseV2WorldDeleted
                              << ",hidden_total=" << g_poseV2HiddenWorld.size();
                    // The engine's pools at the refusal (a full ped, vehicle or
                    // script handle pool refuses a creation).
                    const replay::pools::PoolState peds = replay::pools::State(replay::pools::Kind::Ped);
                    const replay::pools::PoolState vehicles = replay::pools::State(replay::pools::Kind::Vehicle);
                    const replay::pools::PoolState guids = replay::pools::GuidState();
                    Ctx().log << ",ped_pool=" << peds.used << "/" << peds.size << ",vehicle_pool=" << vehicles.used
                              << "/" << vehicles.size << ",guid_pool=" << guids.used << "/" << guids.size
                              << ",playhead_ms=" << timeMs << '\n';
                }
                if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                {
                    puppet.state = replay::playback::PuppetState::Failed;
                    LogPoseV2Puppet(puppet, "create_failed", timeMs);
                }
                return true;
            }
            if (vehicle)
            {
                // The canvas cover of WAGON02X (extra 3) and SUPPLYWAGON (extra
                // 1) comes with its extra only when the extra is on at
                // creation: an extra CREATE_VEHICLE left off and
                // SET_VEHICLE_EXTRA turned on later shows the rigid hoops
                // without the canvas. CREATE_VEHICLE picks the extras at random
                // and no native presets them, so while a recorded-on extra came
                // off the puppet is created again (bounded); the recorded
                // extras then only switch off. The canvas is presumably the
                // extra's cloth, built with the vehicle (extras_spawn,
                // extras_after and rerolls in POSE_V2_VEHICLE_LOOK).
                std::uint32_t spawnExtras = 0;
                std::uint32_t rerolls = 0;
                if (look && look->extrasOn)
                {
                    // At most 6 (1 or 2 are usually enough), one re-creation
                    // per call, not a loop: synchronous re-creations of several
                    // wagons in one tick hitch the frame. The vehicle is
                    // dropped and the puppet stays in Loading, so the next
                    // create goes through the frame's create budget
                    // (kMaxCreatesPerFrame) like any other.
                    constexpr std::uint32_t kExtraRerolls = 6;
                    spawnExtras = replay::pose::playback::VehicleExtrasOn(puppet.clone, look->extrasExist);
                    if ((look->extrasOn & look->extrasExist & ~spawnExtras) != 0 && puppet.extraRerolls < kExtraRerolls)
                    {
                        replay::pose::playback::DeleteVehiclePuppet(puppet.clone);
                        puppet.clone = 0;
                        ++puppet.extraRerolls;
                        puppet.nextCreateAt = now; // the next frame with budget
                        return true;
                    }
                    rerolls = puppet.extraRerolls;
                }
                puppet.extraRerolls = 0;
                // Tint and livery were applied at creation. Wheel tracks in mud
                // and snow, like the ped puppets' footprints.
                ENTITY::_ADD_ENTITY_TRACKING_TRAILS(puppet.clone);
                // Cargo, lanterns and extras (older clips keep whatever the
                // model spawns with).
                if (look)
                {
                    replay::pose::playback::ApplyVehicleLook(puppet.clone,
                        look->propSet, look->lightPropSet, look->extrasExist, look->extrasOn);
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_VEHICLE_LOOK,phase=play"
                        << ",id=" << replay::entity::IdText(puppet.packet.id)
                        << std::hex
                        << ",propset=0x" << look->propSet
                        << ",light=0x" << look->lightPropSet
                        << ",extras_on=0x" << look->extrasOn
                        << ",extras_spawn=0x" << spawnExtras
                        << ",extras_after=0x"
                        << replay::pose::playback::VehicleExtrasOn(puppet.clone, look->extrasExist)
                        << std::dec
                        << ",rerolls=" << rerolls
                        << ",tint=" << wantTint
                        << ",tint_after=" << VEHICLE::_GET_VEHICLE_TINT(puppet.clone)
                        << ",livery=" << wantLivery
                        << ",livery_after=" << VEHICLE::_GET_VEHICLE_LIVERY(puppet.clone)
                        << ",propset_loaded=" << (look->propSet ? PROPSET::_HAS_PROP_SET_LOADED(look->propSet) : 1)
                        << ",light_loaded=" << (look->lightPropSet ? PROPSET::_HAS_PROP_SET_LOADED(look->lightPropSet) : 1)
                        << '\n';
                }
                // The paint is checked again once the vehicle has rendered (a
                // vehicle's variation data is verified after the create).
                puppet.lookClone = puppet.clone;
                puppet.lookTint = wantTint;
                puppet.lookLivery = wantLivery;
                puppet.lookChecks = 0;
                puppet.lookCheckAt = now + 500;
                puppet.state = replay::playback::PuppetState::Ready;
                puppet.armDeadline = now + 3000;
                LogPoseV2Puppet(puppet, "created", timeMs);
                return true;
            }
            // Dressed with the appearance at the playhead (DressAt: a later
            // segment's for a ped the engine still built when it entered the
            // recording), then the whole metaped look (expressions, scale,
            // cleanliness), all before the puppet is shown.
            {
                const replay::DressAppearance dress = PoseV2DressAt(puppet, timeMs);
                SetPoseV2Wear(puppet, dress.appearance ? *dress.appearance : puppet.packet.appearance);
                puppet.dressFrom = dress.from;
                puppet.dressLookaheadMs = dress.lookaheadMs;
                puppet.appearanceKey = dress.key;
                puppet.metapedKey = 0;
                puppet.metapedRetried = false;
                puppet.metapedSet = 0;
                puppet.outfitApplied = replay::entity::appearance::ApplyOutfit(
                    puppet.clone, puppet.wear, puppet.packet.modelHash);
                puppet.layersApplied = false;
                puppet.dressedBy = replay::metaped::DressPath::Layers;
                puppet.dressWhy = puppet.outfitApplied ? "outfit" : "none";
                puppet.shopItemsApplied = 0;
                puppet.shopCheckAt = 0;
                if (!puppet.outfitApplied && !dress.Empty())
                {
                    // By the recorded shop items when there are (the story
                    // player), else by layers; hidden until the Dressing below
                    // finds it ready and checked.
                    DressPoseV2Body(puppet, puppet.wear, timeMs, false);
                    puppet.layersApplied = true;
                }
                ApplyPoseV2Metaped(puppet, timeMs, true);
            }
            puppet.state = replay::playback::PuppetState::Dressing;
            puppet.stateSince = now;
            // Footprints in mud and snow and flattened grass come from the
            // engine's entity trails; a puppet spawned by script may miss the
            // model filter: force it.
            ENTITY::_ADD_ENTITY_TRACKING_TRAILS(puppet.clone);
            LogPoseV2Puppet(puppet, "created", timeMs);
            return true;
        }

        if (puppet.state == replay::playback::PuppetState::Dressing)
        {
            // The clone may be gone (world cleanup, the engine) while it
            // dresses; the timeout below tolerates a ped not ready to render,
            // never a missing one. Every tick, before any appearance native: an
            // existing ped, else back to Loading for a new clone (bounded by
            // the create attempts), its pose override released first. A handle
            // reused by another entity is not ours: it is dropped, not deleted.
            if (!puppet.clone || !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
            {
                constexpr std::uint32_t kDressLostCreates = 4;
                ReleasePoseV2Override(puppet);
                puppet.clone = 0;
                puppet.outfitApplied = false;
                puppet.layersApplied = false;
                puppet.stateSince = now;
                puppet.nextCreateAt = now + replay::playback::kCreateRetryMs;
                puppet.state = puppet.createAttempts >= kDressLostCreates
                    ? replay::playback::PuppetState::Failed
                    : replay::playback::PuppetState::Loading;
                LogPoseV2Puppet(puppet,
                    puppet.state == replay::playback::PuppetState::Failed ? "dress_lost_failed" : "dress_lost", timeMs);
                return false;
            }
            const bool timedOut = now - puppet.stateSince > replay::playback::kDressTimeoutMs;
            if (!replay::pose::playback::IsReadyToRender(puppet.clone) && !timedOut)
                return false;
            const replay::entity::Appearance& wear = PoseV2Wear(puppet);
            const replay::entity::appearance::Match match = replay::entity::appearance::Compare(puppet.clone, wear);
            const bool wearEmpty = wear.assets.empty() && !wear.outfit;
            if (!match.Exact() && !puppet.layersApplied && !wearEmpty)
            {
                DressPoseV2Body(puppet, wear, timeMs, false); // shop items or layers
                puppet.layersApplied = true;
                ApplyPoseV2Metaped(puppet, timeMs, true); // the layers reset the look
                puppet.stateSince = now;
                return false;
            }
            // The metaped look read back once ready; expressions that did not
            // take are set once more.
            if (!timedOut && !puppet.metapedRetried && !Ctx().clip.metapeds.empty())
            {
                const replay::metaped::Wanted wanted = PoseV2MetapedAt(puppet, timeMs);
                if (wanted.state && (wanted.state->flags & replay::metaped::kHasExpressions))
                {
                    const replay::metaped::engine::Check check = replay::metaped::engine::Verify(puppet.clone, *wanted.state);
                    if (check.read && !check.expressions.Exact())
                    {
                        puppet.metapedRetried = true;
                        ++g_poseV2MetapedPlay.retries;
                        if (ApplyPoseV2Metaped(puppet, timeMs, true))
                        {
                            puppet.stateSince = now;
                            return false;
                        }
                    }
                }
            }
            // The shop items read back; a shop dress whose layers are not the
            // recorded ones is dressed again by layers (once: the puppet keeps
            // the layer path), still hidden. After the look's retry: one check
            // per dress.
            if (CheckPoseV2ShopDress(puppet, timeMs, "dress", match))
            {
                puppet.stateSince = now;
                return false;
            }
            LogPoseV2Dressed(puppet, match, timedOut, timeMs);
            puppet.state = replay::playback::PuppetState::Ready;
            puppet.armDeadline = now + 3000;
        }
        return false;
    }
    namespace
    {

    // The pose segment at the playhead decides the puppet's record, bone
    // count and appearance. On a change (either direction, so rewinding
    // restores the earlier look) the override is released, the segment's
    // layers are applied and arming waits for the puppet's skeleton to take
    // the segment's bone count.
    // A recorded dropped hat of `owner` still to be knocked off its puppet
    // by the engine (bounded: the knock has a deadline).
    constexpr ULONGLONG kPoseV2KnockPendingMaxMs = 2000;
    }
    bool PoseV2KnockPending(replay::entity::ReplayId owner)
    {
        const ULONGLONG now = GetTickCount64();
        for (const replay::playback::Puppet& prop : Ctx().puppets.list)
        {
            if (prop.packet.kind == replay::entity::EntityKind::Prop &&
                prop.packet.propOrigin == replay::entity::PropOrigin::DroppedHat &&
                prop.packet.owner == owner &&
                prop.state == replay::playback::PuppetState::Loading &&
                (now - prop.stateSince < kPoseV2KnockPendingMaxMs ||
                    // An owner dressed for the knock keeps the hat until the
                    // knock and its drop.
                    (prop.hatWoreAt && now - prop.hatWoreAt < replay::hatlook::kWearWaitMs + replay::hatlook::kDropWaitMs)))
            {
                return true;
            }
        }
        return false;
    }
    namespace
    {

    // A vehicle follows the pose record of its segment like a body, without
    // layers, knocks or early segments (no metaped). A wagon's create record
    // may have no samples (registered while the engine built it, its pose in a
    // segment from the clip start): kept on it, the wagon is never placed and
    // stays hidden, its harnessed horse hidden with it and its seated driver,
    // who rides his own recorded pose, left in the air. The arm is released so
    // the next RefreshPoseV2Override arms the new record in the same tick
    // (ArmPoseV2Exact writes its pose at once).
    void SwitchPoseV2VehicleRecord(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const std::uint32_t recordId =
            replay::entity::PlaybackRecordAt(Ctx().clip.segments, puppet.packet.id, timeMs, PoseV2RecordSampled);
        if (recordId == puppet.recordId)
            return;
        const replay::entity::PoseSegment* segment = PoseV2SegmentAt(puppet.packet.id, timeMs);
        const std::uint32_t previousRecord = puppet.recordId;
        const bool wasArmed = puppet.overrideSlot >= 0;
        if (wasArmed)
            ReleasePoseV2Override(puppet);
        puppet.recordId = recordId;
        puppet.boneCount = segment ? segment->boneCount : puppet.packet.boneCount;
        puppet.armFailureLogged = false;
        puppet.armDeadline = GetTickCount64() + 3000;
        puppet.sampleReuse.Invalidate();
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        replay::pose::capture::TrackSpan(recordId, firstMs, lastMs);
        Ctx().log << "POSE_V2_PLAYBACK_SEGMENT,id=" << replay::entity::IdText(puppet.packet.id) << ",kind=vehicle"
                  << ",record=0x" << std::hex << recordId << ",previous_record=0x" << previousRecord << std::dec
                  << ",bones=" << puppet.boneCount << ",arm=" << (wasArmed ? "released" : "none")
                  << ",segment_ms=" << (segment ? segment->startMs : 0) << ",first_ms=" << firstMs
                  << ",last_ms=" << lastMs << ",playhead_ms=" << timeMs << '\n';
    }
    }

    void SwitchPoseV2Segment(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        if (puppet.packet.kind == replay::entity::EntityKind::Vehicle)
        {
            SwitchPoseV2VehicleRecord(puppet, timeMs);
            return;
        }
        if (puppet.packet.kind == replay::entity::EntityKind::Weapon ||
            puppet.packet.kind == replay::entity::EntityKind::Prop)
        {
            return;
        }
        const replay::entity::PoseSegment* segment = PoseV2SegmentAt(puppet.packet.id, timeMs);
        std::uint32_t recordId = segment ? segment->recordId : puppet.packet.id.Packed();
        // The skeleton already has the next segment's bones (a knock a frame
        // before it): that segment now, no layers put back.
        bool early = false;
        {
            const std::uint32_t wantedBones = segment ? segment->boneCount : puppet.packet.boneCount;
            replay::skeleton::ResolvedPoseSkeleton resolved{};
            if (replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved) && resolved.boneCount != wantedBones)
            {
                if (const replay::entity::PoseSegment* next = replay::entity::EarlySegmentForBones(
                        Ctx().clip.segments, puppet.packet.id, timeMs, resolved.boneCount, PoseV2RecordSampled))
                {
                    segment = next;
                    recordId = next->recordId;
                    early = true;
                }
            }
        }
        if (recordId == puppet.recordId)
            return;
        // A dropped hat leaves by the engine's knock (UpdatePoseV2Props),
        // as recorded, not by a layer change: the segment waits for it.
        if (PoseV2KnockPending(puppet.packet.id))
            return;
        const std::uint32_t previousBones = puppet.boneCount;
        const std::uint32_t previousRecord = puppet.recordId;
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        std::uint32_t previousFirstMs = 0;
        std::uint32_t previousLastMs = 0;
        replay::pose::capture::TrackSpan(recordId, firstMs, lastMs);
        replay::pose::capture::TrackSpan(previousRecord, previousFirstMs, previousLastMs);
        // The arm stays when it already poses the new record (armed in the tick
        // the skeleton changed), or holds the previous record while the engine
        // has not rebuilt the skeleton (RefreshPoseV2Override re-arms in the
        // tick it does; segment_switch.h). Releasing it here would leave hidden
        // ticks: a blink.
        const std::uint32_t newBones = segment ? segment->boneCount : puppet.packet.boneCount;
        const bool armedNow = puppet.overrideSlot >= 0;
        const bool keepArm =
            replay::segswitch::SwitchKeepsArm(armedNow, puppet.mapped, puppet.armedRecordId, puppet.armedBones, recordId, newBones);
        const bool holdArm = !keepArm && replay::segswitch::SwitchHoldsArm(armedNow, puppet.mapped);
        if (keepArm)
        {
            ++g_poseV2Switch.kept;
            puppet.holdSince = 0;
        }
        else if (holdArm)
        {
            ++g_poseV2Switch.held;
            puppet.holdSince = GetTickCount64();
        }
        else
            ReleasePoseV2Override(puppet);
        // The segment's layers, or an appearance change recorded after its
        // start when the playhead is past it (a seek).
        const replay::WantedAppearance wanted = PoseV2AppearanceAt(puppet, timeMs);
        const replay::entity::Appearance& appearance = *wanted.appearance;
        puppet.appearanceKey = wanted.key;
        SetPoseV2Wear(puppet, appearance);
        // Layers only when they differ (the engine already changed them on
        // a knock): applying them rebuilds the skeleton.
        const bool layersApplied = !early &&
            !replay::entity::appearance::Compare(puppet.clone, appearance).Exact();
        if (layersApplied)
        {
            LearnPoseV2Categories(puppet, appearance);
            DressPoseV2Body(puppet, appearance, timeMs, true); // shop items or layers
            ApplyPoseV2Metaped(puppet, timeMs, true);
        }
        puppet.recordId = recordId;
        puppet.boneCount = segment ? segment->boneCount : puppet.packet.boneCount;
        puppet.armFailureLogged = false;
        puppet.armDeadline = GetTickCount64() + 3000;
        puppet.layerRetries = 0;
        puppet.nextLayerRetryAt = GetTickCount64() + 600;
        puppet.noRecord.Reset(); // the steps start again for the new segment
        puppet.noRecordMapRefused = false;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_SEGMENT"
            << ",id=" << replay::entity::IdText(puppet.packet.id)
            << ",record=0x" << std::hex << recordId << std::dec
            << ",bones=" << puppet.boneCount
            << ",previous_bones=" << previousBones
            << ",layers=" << appearance.assets.size()
            << ",layers_applied=" << (layersApplied ? 1 : 0)
            << ",early=" << (early ? 1 : 0)
            << ",arm=" << (keepArm ? "kept" : holdArm ? "held" : "released")
            << ",segment_ms=" << (segment ? segment->startMs : 0)
            << ",first_ms=" << firstMs
            << ",previous_last_ms=" << previousLastMs
            << ",playhead_ms=" << timeMs
            << '\n';
        log.flush();
    }

    replay::playback::Puppet* FindPoseV2Puppet(replay::entity::ReplayId id) { return Ctx().puppets.Find(id); }
    namespace
    {

    // Applies the recorded attachments at the playhead between ready
    // puppets, and releases the ones that no longer hold.
    constexpr std::uint32_t kPoseV2SeatFollowFrames = 10;
    }
    void UpdatePoseV2Attachments(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.attachments");
        static std::vector<replay::entity::Attachment> attachments;
        Ctx().clip.lifecycle.AttachmentsAt(timeMs, attachments);
        for (replay::playback::Puppet& child : Ctx().puppets.list)
        {
            if (!child.attached)
                continue;
            const bool holds = std::find(attachments.begin(), attachments.end(), child.attachment) !=
                attachments.end();
            replay::playback::Puppet* parent = FindPoseV2Puppet(child.attachment.parent);
            // A parent created again (a wagon rebuilt for its windows, parts or
            // burn) takes its horses again: the harness was on the deleted
            // entity.
            const bool sameParent = parent && parent->clone && parent->clone == child.attachedParentClone;
            // A harness refused on a vehicle just created: tried again a few
            // times.
            if (holds && parent && parent->clone && child.attachFailures > 0 && child.attachFailures < 5 &&
                GetTickCount64() >= child.attachRetryAt)
            {
                child.attached = false;
                continue;
            }
            if (holds && sameParent)
                continue;
            if (parent && parent->clone && !sameParent)
            {
                child.attached = false;
                child.seatFollowLeft = 0;
                child.attachFailures = 0;
                continue;
            }
            // A seat is data only (the puppet rides its recorded pose);
            // a harness is an engine attachment to undo.
            if (child.attachment.kind != replay::entity::AttachmentKind::VehicleSeat && parent)
                replay::pose::playback::DetachDraftHarness(parent->clone, child.clone);
            child.attached = false;
            child.seatFollowLeft = 0;
        }
        for (const replay::entity::Attachment& attachment : attachments)
        {
            replay::playback::Puppet* child = FindPoseV2Puppet(attachment.child);
            replay::playback::Puppet* parent = FindPoseV2Puppet(attachment.parent);
            // Weapons follow their parent by transform (replay_objects).
            if (attachment.kind == replay::entity::AttachmentKind::WeaponPoint)
                continue;
            if (!child || !parent || child->attached ||
                child->state != replay::playback::PuppetState::Ready ||
                parent->state != replay::playback::PuppetState::Ready)
            {
                continue;
            }
            const bool seat = attachment.kind == replay::entity::AttachmentKind::VehicleSeat;
            // A seated puppet is never put into the vehicle by the engine (the
            // seat placement lands the ped at the vehicle for a frame, and then
            // holds it 0.15 m off the recorded pose): as a replay editor drives
            // every matrix, it keeps riding its recorded pose and the seat is
            // data. The placement error is logged as evidence of how far the
            // engine seat would be.
            float predictedM = -1.0f;
            if (seat)
            {
                replay::pose::WorldTransform childNow{};
                replay::pose::WorldTransform parentNow{};
                replay::pose::WorldTransform childSeated{};
                replay::pose::WorldTransform parentSeated{};
                const std::uint32_t seatedMs = timeMs + replay::entity::kSeatSteadyAheadMs;
                if (PoseV2RecordedWorldAt(attachment.child, timeMs, childNow) &&
                    PoseV2RecordedWorldAt(attachment.parent, timeMs, parentNow) &&
                    PoseV2RecordedWorldAt(attachment.child, seatedMs, childSeated) &&
                    PoseV2RecordedWorldAt(attachment.parent, seatedMs, parentSeated))
                {
                    predictedM = replay::entity::PlacementErrorM(parentNow,
                        replay::entity::RelativeTransform(parentSeated, childSeated), childNow);
                }
                child->seatFollowLeft = kPoseV2SeatFollowFrames;
            }
            const bool ok = seat ||
                replay::pose::playback::AttachDraftHarness(
                    child->clone, parent->clone, attachment.index);
            child->attached = ok;
            child->attachment = attachment;
            child->attachedParentClone = parent->clone;
            std::ostream& log = Ctx().log;
            log << "POSE_V2_PLAYBACK_ATTACH"
                << ",child=" << replay::entity::IdText(attachment.child)
                << ",parent=" << replay::entity::IdText(attachment.parent)
                << ",kind=" << (seat ? "seat" : "harness")
                << ",index=" << attachment.index
                << ",status=" << (ok ? "attached" : "failed")
                << ",playhead_ms=" << timeMs;
            if (seat)
            {
                char detail[64]{};
                std::snprintf(detail, sizeof(detail), ",predicted_m=%.3f", predictedM);
                log << detail;
            }
            log << '\n';
            log.flush();
            if (!ok)
            {
                child->attached = true; // not every frame: retried a few times, 0.5 s apart
                ++child->attachFailures;
                child->attachRetryAt = GetTickCount64() + 500;
            }
            else
                child->attachFailures = 0;
        }
    }
    namespace
    {

    struct PoseV2RopePuppetIndex final : replay::ropes::PuppetIndex
    {
        std::uint64_t AddressOf(replay::entity::ReplayId id) const override
        {
            const replay::playback::Puppet* puppet = id.Valid() ? FindPoseV2Puppet(id) : nullptr;
            return puppet && puppet->clone && replay::entities::Exists(puppet->clone)
                ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone))
                : 0;
        }

        bool HandOf(replay::entity::ReplayId id, float out[3]) const override
        {
            const replay::playback::Puppet* puppet = id.Valid() ? FindPoseV2Puppet(id) : nullptr;
            if (!puppet || !puppet->clone || !replay::entities::Exists(puppet->clone))
                return false;
            const int bone = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(puppet->clone, "SKEL_R_Hand");
            if (bone < 0)
                return false;
            const Vector3 at = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(puppet->clone, bone);
            out[0] = at.x;
            out[1] = at.y;
            out[2] = at.z;
            return true;
        }
    };
    }

    void PlayPoseV2Ropes(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.ropes");
        if (Ctx().clip.ropes.Records().empty())
            return;
        const PoseV2RopePuppetIndex index;
        Ctx().clip.ropes.Play(index, timeMs, Ctx().log);
    }
}
