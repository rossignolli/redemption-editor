// replay_objects_weapon_play.cpp: the weapons played: components, strap, look, wear, attach, drawn checks.
// Split from replay_objects.cpp (moved code); shared internals in replay_objects_internal.h.

#include "replay_objects_internal.h"

namespace replay::objects
{
    namespace
    {
    // POSE_V2_STRAP_APPLY: one line per strap state change on a puppet,
    // at most this many a playback (a scrub back and forth repeats them).
    constexpr std::uint32_t kStrapApplyLogCap = 400;

    std::string PoseV2WeaponStrapFields(Object object);
    std::vector<replay::pose::BoneQts> g_weaponDrawnPublished;
    std::vector<replay::pose::BoneQts> g_weaponDrawnBuffer;
    }


    // Weapon puppets: created once their parent puppet is ready, then placed
    // every frame at the recorded parent-space transform composed with the
    // parent puppet's current transform (seated parents included), after
    // every parent has been placed.
    // A new weapon object gets the recorded components back (the weapon
    // system builds it with its defaults only).
    void ApplyPoseV2WeaponComponents(const replay::playback::Puppet& puppet)
    {
        if (!puppet.clone || !puppet.packet.weaponHash)
            return;
        std::uint32_t added = 0;
        std::uint32_t removed = 0;
        replay::pose::playback::ApplyWeaponComponents(puppet.clone, puppet.packet.weaponHash,
            puppet.packet.weaponComponents, added, removed);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_WEAPON_COMPONENTS,id=" << replay::entity::IdText(puppet.packet.id)
            << ",recorded=" << puppet.packet.weaponComponents.size()
            << ",added=" << added
            << ",removed=" << removed
            << '\n';
        log.flush();
    }
    Object g_poseV2AttachHelper = 0;

    bool PoseV2EngineEuler(const replay::pose::Quat& rotation, Hash model, Vector3& euler)
    {
        if (!g_poseV2AttachHelper || !replay::entities::Exists(g_poseV2AttachHelper))
        {
            g_poseV2AttachHelper = 0;
            if (!model || !STREAMING::HAS_MODEL_LOADED(model))
                return false;
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
            g_poseV2AttachHelper = OBJECT::CREATE_OBJECT(model, at.x, at.y, at.z - 50.0f,
                FALSE, FALSE, FALSE, FALSE, FALSE);
            if (!g_poseV2AttachHelper)
                return false;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_poseV2AttachHelper, TRUE, TRUE);
            ENTITY::SET_ENTITY_VISIBLE(g_poseV2AttachHelper, FALSE);
            ENTITY::SET_ENTITY_COLLISION(g_poseV2AttachHelper, FALSE, FALSE);
            ENTITY::FREEZE_ENTITY_POSITION(g_poseV2AttachHelper, TRUE);
        }
        ENTITY::SET_ENTITY_QUATERNION(g_poseV2AttachHelper, rotation.x, rotation.y, rotation.z, rotation.w);
        euler = ENTITY::GET_ENTITY_ROTATION(g_poseV2AttachHelper, kPoseV2AttachRotationOrder);
        return true;
    }

    void DeletePoseV2AttachHelper()
    {
        if (g_poseV2AttachHelper && replay::entities::Exists(g_poseV2AttachHelper))
            OBJECT::DELETE_OBJECT(&g_poseV2AttachHelper);
        g_poseV2AttachHelper = 0;
    }
    namespace
    {

    // Evidence: the strap fields of a weapon object's weapon.
    std::string PoseV2WeaponStrapFields(Object object)
    {
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object));
        std::uint64_t weapon = 0;
        if (!address || !replay::audio::ReadU64(address + 0x448, weapon) || !weapon)
            return "no_weapon";
        std::uint32_t flags = 0;
        std::uint64_t cloth = 0;
        std::uint64_t shoulder = 0;
        float weightA = 0.0f;
        float weightB = 0.0f;
        replay::audio::ReadBytes(weapon + 0x62C, &flags, 4);
        replay::audio::ReadU64(weapon + 0x440, cloth);
        replay::audio::ReadU64(weapon + 0x450, shoulder);
        replay::audio::ReadBytes(weapon + 0x5D4, &weightA, 4);
        replay::audio::ReadBytes(weapon + 0x5D8, &weightB, 4);
        std::uint32_t signature = 0;
        std::int32_t firstBone = -2;
        if (shoulder)
        {
            replay::audio::ReadBytes(shoulder + 0x28, &signature, 4);
            replay::audio::ReadBytes(shoulder + 0x2C, &firstBone, 4);
        }
        char text[200];
        std::snprintf(text, sizeof(text),
            "flags=0x%x,bit15=%u,s440=%d,s450=%d,w5d4=%.3f,w5d8=%.3f,shoulder_sig=0x%x,shoulder_bone0=%d",
            flags, (flags >> 15) & 1u, cloth ? 1 : 0, shoulder ? 1 : 0, weightA, weightB, signature, firstBone);
        return text;
    }
    }
    std::map<std::uint32_t, WeaponLookVerdict> g_weaponLookVerdicts;
    bool g_weaponLookEnginePlay = true;
    bool g_weaponLookEnginePlayChecked = false;
    namespace
    {
    constexpr std::uint64_t kWeaponLookModelWaitMs = 2000;
    constexpr std::uint32_t kWeaponLookReapplyMax = 3;

    bool ReadPoseV2PuppetComponents(Object clone, std::vector<std::uint32_t>& out)
    {
        out.clear();
        if (!g_weaponLookEnginePlay ||
            !replay::weapon::ReadComponentList(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(clone)), out))
            return false;
        if (!g_weaponLookEnginePlayChecked)
        {
            g_weaponLookEnginePlayChecked = true;
            for (const std::uint32_t hash : out)
                if (!WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(clone, hash))
                {
                    g_weaponLookEnginePlay = false;
                    Ctx().log << "WEAPON_LOOK_ENGINE_OFF,read=components,phase=play,reason=hash_refused_by_native,hash="
                              << replay::weaponlook::HashList({hash}) << '\n';
                    out.clear();
                    return false;
                }
        }
        return true;
    }

    // Recorded components onto the puppet. engineList: the state carries the
    // engine's whole list, so whatever else the puppet has is removed.
    void ApplyPoseV2LookComponents(const replay::playback::Puppet& puppet, const std::vector<std::uint32_t>& recorded,
        bool engineList, std::uint32_t& added, std::uint32_t& removed, std::uint32_t& refused)
    {
        added = removed = refused = 0;
        for (const std::uint32_t hash : recorded)
        {
            if (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, hash))
                continue;
            // A model component whose model did not stream in (the wait ran
            // out) is left out rather than given without its drawable.
            const Hash model = WEAPON::_GET_WEAPON_COMPONENT_TYPE_MODEL(hash);
            if (model && STREAMING::IS_MODEL_VALID(model) && !STREAMING::HAS_MODEL_LOADED(model))
            {
                ++refused;
                continue;
            }
            WEAPON::_GIVE_WEAPON_COMPONENT_TO_ENTITY(puppet.clone, hash, puppet.packet.weaponHash, TRUE);
            ++added;
            if (!WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, hash))
                ++refused; // unknown to this weapon (or to the build): left out
        }
        std::vector<std::uint32_t> actual;
        if (engineList && ReadPoseV2PuppetComponents(puppet.clone, actual))
        {
            std::vector<std::uint32_t> missing, extra;
            replay::weaponlook::Diff(recorded, actual, missing, extra);
            for (const std::uint32_t hash : extra)
                if (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, hash))
                {
                    WEAPON::REMOVE_WEAPON_COMPONENT_FROM_WEAPON_OBJECT(puppet.clone, hash);
                    ++removed;
                }
            return;
        }
        // A names list (clips before the engine list): only named extras.
        for (const replay::weapon::Component& candidate : replay::weapon::kComponents)
        {
            if (std::binary_search(recorded.begin(), recorded.end(), candidate.hash) ||
                !WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, candidate.hash))
                continue;
            WEAPON::REMOVE_WEAPON_COMPONENT_FROM_WEAPON_OBJECT(puppet.clone, candidate.hash);
            ++removed;
        }
    }

    bool ApplyPoseV2Wear(Object clone, const replay::WeaponCondition& state)
    {
        bool rawWritten = false;
        if (g_weaponWearWrite && (state.flags & replay::weaponlook::kFlagWearRaw))
            rawWritten = replay::weapon::WriteWearRaw(
                reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(clone)), state.permanent, state.wearE4);
        WEAPON::_SET_WEAPON_DEGRADATION(clone, state.degradation);
        WEAPON::_SET_WEAPON_DAMAGE(clone, state.damage, FALSE);
        WEAPON::_SET_WEAPON_DIRT(clone, state.dirt, FALSE);
        WEAPON::_SET_WEAPON_SOOT(clone, state.soot, FALSE);
        return rawWritten;
    }

    bool ReadPoseV2PuppetWear(Object clone, replay::weaponlook::Wear& out)
    {
        replay::weapon::WearFields raw;
        if (replay::weapon::ReadWear(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(clone)), raw))
        {
            out = {raw.dirt, raw.soot, raw.damage, raw.degradation, raw.permanent, raw.e4};
            return true;
        }
        out = {WEAPON::_GET_WEAPON_DIRT(clone), WEAPON::_GET_WEAPON_SOOT(clone), WEAPON::_GET_WEAPON_DAMAGE(clone),
            WEAPON::GET_WEAPON_DEGRADATION(clone), WEAPON::GET_WEAPON_PERMANENT_DEGRADATION(clone), 0.0f};
        return true;
    }

    void CheckPoseV2WeaponLook(replay::playback::Puppet& puppet, const replay::WeaponConditionTrack* track, int index,
        std::uint32_t timeMs, ULONGLONG now)
    {
        const replay::WeaponCondition* state = track && index >= 0 ? &track->states[index].second : nullptr;
        // What the clip knows of the components: the state's list, else the
        // create's (older clips).
        const std::vector<std::uint32_t>* recorded = nullptr;
        std::vector<std::uint32_t> createList;
        bool engineList = false;
        if (state && (state->flags & (replay::weaponlook::kFlagComponents | replay::weaponlook::kFlagComponentsNames)))
        {
            recorded = &state->components;
            engineList = (state->flags & replay::weaponlook::kFlagComponents) != 0;
        }
        else if (puppet.packet.weaponHash)
        {
            createList = puppet.packet.weaponComponents;
            replay::weaponlook::Normalize(createList);
            recorded = &createList;
        }
        std::vector<std::uint32_t> actual;
        bool actualRead = false;
        std::vector<std::uint32_t> missing, extra;
        if (recorded)
        {
            if (engineList && ReadPoseV2PuppetComponents(puppet.clone, actual))
            {
                actualRead = true;
                replay::weaponlook::Diff(*recorded, actual, missing, extra);
            }
            else
            {
                // Names (or no engine read): the recorded ones present?
                actualRead = true;
                for (const std::uint32_t hash : *recorded)
                    if (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, hash))
                        actual.push_back(hash);
                replay::weaponlook::Diff(*recorded, actual, missing, extra);
            }
        }
        replay::weaponlook::Wear recordedWear{}, actualWear{};
        bool wearKnown = false;
        if (state)
        {
            recordedWear = WearOf(*state);
            wearKnown = ReadPoseV2PuppetWear(puppet.clone, actualWear);
        }
        const bool withRaw = state && (state->flags & replay::weaponlook::kFlagWearRaw) && g_weaponWearWrite;
        // The dirt the drawing uses: set before the drawable had its weapon
        // effect, the field agrees and the drawn value does not.
        float shaderDirt = 0.0f;
        const bool shaderRead = state && replay::weapon::ReadWearShaderDirt(
            reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), shaderDirt);
        const bool shaderOff = shaderRead && std::fabs(shaderDirt - recordedWear.dirt) > replay::weaponlook::kWearTolerance;
        replay::weaponlook::Check check;
        check.componentsKnown = recorded != nullptr;
        check.componentsRead = actualRead;
        check.missing = missing.size();
        check.extra = extra.size();
        check.wearRead = wearKnown;
        check.wearDiff = wearKnown ? replay::weaponlook::MaxWearDiff(recordedWear, actualWear, withRaw) : 0.0f;
        if (shaderOff)
            check.wearDiff = (std::max)(check.wearDiff, std::fabs(shaderDirt - recordedWear.dirt));
        WeaponLookVerdict& verdict = g_weaponLookVerdicts[puppet.packet.id.Packed()];
        const bool first = !verdict.checked;
        verdict.checked = true;
        verdict.components = check.ComponentsMismatch();
        verdict.wear = check.WearMismatch();
        bool reapplied = false;
        if (verdict.wear && state && puppet.weaponLookReapplied < kWeaponLookReapplyMax)
        {
            ++puppet.weaponLookReapplied;
            ++g_weaponLookTally.reapplied;
            ApplyPoseV2Wear(puppet.clone, *state);
            reapplied = true;
        }
        if ((first || verdict.components || verdict.wear) && g_weaponLookPlayLogs < kWeaponLookLogCap)
        {
            ++g_weaponLookPlayLogs;
            const replay::logfmt::FormatScope logFormat(Ctx().log);
            Ctx().log << "POSE_V2_WEAPON_LOOK_CHECK,id=" << replay::entity::IdText(puppet.packet.id) << ",phase=play,t=" << timeMs
                      << ",state=" << index << ",source=" << (state ? (engineList ? "engine" : recorded == &createList ? "create" : "names") : "create")
                      << ",components_rec=" << (recorded ? recorded->size() : 0) << ",components_play=" << actual.size()
                      << ",missing=" << missing.size() << ",extra=" << extra.size()
                      << ",missing_list=" << replay::weaponlook::HashList(missing) << ",extra_list=" << replay::weaponlook::HashList(extra)
                      << ",wear_rec=" << (state ? replay::weaponlook::WearText(recordedWear) : std::string("none"))
                      << ",wear_play=" << (wearKnown ? replay::weaponlook::WearText(actualWear) : std::string("none"))
                      << ",wear_raw=" << (withRaw ? 1 : 0) << ",dirt_drawn=";
            if (shaderRead)
                Ctx().log << shaderDirt;
            else
                Ctx().log << "none";
            Ctx().log << ",wear_diff=" << check.wearDiff
                      << ",verdict=" << (verdict.components && verdict.wear ? "components_wear_mismatch"
                                            : verdict.components ? "weapon_components_mismatch"
                                            : verdict.wear ? "weapon_wear_mismatch" : "ok")
                      << ",reapplied=" << (reapplied ? 1 : 0) << '\n';
        }
        puppet.weaponLookCheckAt = now + replay::weaponlook::kRecheckMs;
    }
    }

    void ApplyPoseV2WeaponLook(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        if (!puppet.clone || !replay::entities::Exists(puppet.clone) || !puppet.packet.weaponHash)
            return;
        const ULONGLONG now = GetTickCount64();
        const auto found = std::find_if(Ctx().clip.weaponConditions.begin(), Ctx().clip.weaponConditions.end(),
            [&](const replay::WeaponConditionTrack& entry) { return entry.recordId == puppet.packet.id.Packed(); });
        const replay::WeaponConditionTrack* track =
            found != Ctx().clip.weaponConditions.end() && !found->states.empty() ? &*found : nullptr;
        const int index = track ? replay::weaponlook::StateAt(track->states, timeMs) : -1;
        if (track && index != puppet.conditionApplied)
        {
            const replay::WeaponCondition& state = track->states[index].second;
            const bool hasComponents =
                (state.flags & (replay::weaponlook::kFlagComponents | replay::weaponlook::kFlagComponentsNames)) != 0;
            if (hasComponents && !replay::pose::playback::RequestWeaponComponents(state.components))
            {
                if (!puppet.weaponLookWaitUntil)
                    puppet.weaponLookWaitUntil = now + kWeaponLookModelWaitMs;
                if (now < puppet.weaponLookWaitUntil)
                    return; // the components' models are on their way
            }
            puppet.weaponLookWaitUntil = 0;
            std::uint32_t added = 0, removed = 0, refused = 0;
            if (hasComponents)
                ApplyPoseV2LookComponents(puppet, state.components, (state.flags & replay::weaponlook::kFlagComponents) != 0,
                    added, removed, refused);
            const bool rawWritten = ApplyPoseV2Wear(puppet.clone, state);
            puppet.conditionApplied = index;
            puppet.weaponLookReapplied = 0;
            puppet.weaponLookCheckAt = now + replay::weaponlook::kCheckDelayMs;
            ++g_weaponLookTally.applies;
            if ((added || removed || refused) && g_weaponLookPlayLogs < kWeaponLookLogCap)
            {
                ++g_weaponLookPlayLogs;
                Ctx().log << "POSE_V2_WEAPON_LOOK_APPLY,id=" << replay::entity::IdText(puppet.packet.id) << ",t=" << timeMs
                          << ",state=" << index << ",components=" << state.components.size() << ",added=" << added
                          << ",removed=" << removed << ",refused=" << refused << ",raw_write=" << (rawWritten ? 1 : 0)
                          << ",wear=" << replay::weaponlook::WearText(WearOf(state)) << '\n';
            }
            return; // read back after the engine took it
        }
        if (!track && !puppet.weaponLookCheckAt && !g_weaponLookVerdicts.count(puppet.packet.id.Packed()))
            puppet.weaponLookCheckAt = now + replay::weaponlook::kCheckDelayMs; // create-only check, once
        if (puppet.weaponLookCheckAt && now >= puppet.weaponLookCheckAt)
        {
            CheckPoseV2WeaponLook(puppet, track, index, timeMs, now);
            if (!track)
                puppet.weaponLookCheckAt = 0; // nothing changes without a track: one check
        }
    }
    namespace
    {

    void ApplyPoseV2StrapTrack(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        // The look is set by ApplyPoseV2WeaponLook for every
        // weapon puppet, attached or not (UpdatePoseV2Weapons, props).
        const auto strapTrack = std::find_if(Ctx().clip.strapTracks.begin(), Ctx().clip.strapTracks.end(),
            [&](const replay::StrapTrack& entry) { return entry.recordId == puppet.packet.id.Packed(); });
        if (strapTrack == Ctx().clip.strapTracks.end() || strapTrack->states.empty())
            return;
        const int index = replay::strap::StateIndexAt(strapTrack->states, timeMs);
        const auto& [stateMs, state] = strapTrack->states[static_cast<std::size_t>(index)];
        // Every frame (the mask and weights as before); the rebuild request
        // only where the recording had it (strap_state_core.h).
#if defined(REDEMPTION_DEV)
        {
            // Developer bench (strap_test regive): the component removed and
            // given again on this puppet, what the engine says before/after.
            std::uint32_t generation = 0;
            const std::uint32_t regive = replay::weapon::StrapTestRegive(generation);
            if (regive && generation != puppet.strapTestRegive && puppet.clone && puppet.packet.weaponHash)
            {
                puppet.strapTestRegive = generation;
                const auto& recorded = puppet.packet.weaponComponents;
                const bool inRecording = std::find(recorded.begin(), recorded.end(), regive) != recorded.end();
                const bool before = WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, regive) != FALSE;
                if (inRecording)
                {
                    WEAPON::REMOVE_WEAPON_COMPONENT_FROM_WEAPON_OBJECT(puppet.clone, regive);
                    WEAPON::_GIVE_WEAPON_COMPONENT_TO_ENTITY(puppet.clone, regive, puppet.packet.weaponHash, TRUE);
                }
                Ctx().log << "STRAP_TEST_REGIVE,id=" << replay::entity::IdText(puppet.packet.id) << ",hash=0x" << std::hex
                          << regive << std::dec << ",in_recording=" << (inRecording ? 1 : 0) << ",had=" << (before ? 1 : 0)
                          << ",has=" << (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(puppet.clone, regive) ? 1 : 0) << '\n';
            }
        }
#endif
        replay::weapon::StrapApplyResult result;
        const bool applied = replay::weapon::ApplyStrapState(
            reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), state, &result);
        if (index == puppet.strapApplied && puppet.strapClone == puppet.clone)
            return;
        puppet.strapApplied = index;
        puppet.strapClone = puppet.clone;
        // One line per change: the recorded state, who held the strap, the
        // request made, and the puppet's flags read back after the write.
        if (g_strapApplyLogs >= kStrapApplyLogCap)
            return;
        ++g_strapApplyLogs;
        std::ostream& log = Ctx().log;
        const replay::logfmt::FormatScope logFormat(log);
        log << "POSE_V2_STRAP_APPLY,id=" << replay::entity::IdText(puppet.packet.id)
            << ",time_ms=" << timeMs
            << ",state=" << index << '/' << strapTrack->states.size()
            << ",state_ms=" << stateMs
            << ",recorded=0x" << std::hex << state.flags << std::dec
            << ",holder=" << replay::strap::HolderName(state.flags)
            << ",task_hold=0x" << std::hex << (state.flags & replay::strap::kFlagTaskHold) << std::dec
            << ",held=" << ((state.flags & replay::strap::kFlagHeld) ? 1 : 0)
            << ",cloth_state=" << ((state.flags & replay::strap::kFlagClothState) ? 1 : 0)
            << ",rebuild=" << (replay::strap::RebuildWanted(state.flags) ? 1 : 0)
            << ",weight_a=" << state.weightA << ",weight_b=" << state.weightB
            << ",applied=" << (applied ? 1 : 0);
        if (result.readBack)
        {
            log << ",before=0x" << std::hex << result.before << ",wanted=0x" << result.wanted
                << ",read=0x" << result.after << std::dec
                << ",read_ok=" << (result.after == result.wanted ? 1 : 0);
        }
        log << ",mask=";
        for (const auto& bit : state.maskBits)
            log << std::hex << bit.first << std::dec << ':' << (bit.second ? 1 : 0) << ' ';
        log << '\n';
    }
    }

    // The weapon records met with a vehicle or a train car as their parent (a
    // hay bale on a cart, a cabinet in a train car). Their engine attach frame
    // names a bone of the vehicle's skeleton, which a train car built as an
    // object does not have: they ride at the parent's root with the
    // parent-relative transform, which the recording samples every frame.
    std::unordered_set<std::uint32_t> g_vehicleChildRecords;
    namespace
    {
    // A train car built as a vehicle puppet gets its prop set again from the
    // engine (replay_trains ApplyVehicleLook): the cargo the recording kept
    // as children is then drawn twice. The engine's copy (an object of the
    // child's model attached to the parent puppet, within kEngineCopyM of
    // the child) is hidden: the recorded child carries the pose (the
    // cabinet's doors) and the look. Looked at again every second (the
    // prop set may come late), two children a frame.
    constexpr float kEngineCopyM = 0.5f;
    constexpr ULONGLONG kEngineCopyCheckMs = 1000;
    constexpr std::uint32_t kEngineCopyChecksPerFrame = 2;
    }
    std::unordered_map<std::uint32_t, ULONGLONG> g_engineCopyNextCheck;
    std::unordered_set<Object> g_engineCopiesHidden;
    std::uint32_t g_engineCopyChecks = 0;

    void HidePoseV2EngineCopy(const replay::playback::Puppet& puppet, Entity parentClone, std::uint32_t& checks, ULONGLONG now)
    {
        if (checks >= kEngineCopyChecksPerFrame || !puppet.clone)
            return;
        ULONGLONG& next = g_engineCopyNextCheck[puppet.packet.id.Packed()];
        if (now < next)
            return;
        next = now + kEngineCopyCheckMs;
        ++checks;
        ++g_engineCopyChecks;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        for (int slot = 0; slot < count; ++slot)
        {
            const Object object = objects[slot];
            if (!object || object == puppet.clone || g_engineCopiesHidden.count(object) || !replay::entities::Exists(object) ||
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) != puppet.packet.modelHash ||
                ENTITY::GET_ENTITY_ATTACHED_TO(object) != parentClone ||
                std::any_of(Ctx().puppets.list.begin(), Ctx().puppets.list.end(),
                    [&](const replay::playback::Puppet& other) { return other.clone == object; }))
                continue;
            const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            if (Distance2(p, at) > kEngineCopyM * kEngineCopyM)
                continue;
            ENTITY::SET_ENTITY_VISIBLE(object, FALSE);
            g_engineCopiesHidden.insert(object);
            if (g_engineCopiesHidden.size() <= 16)
                Ctx().log << "POSE_V2_VEHICLE_CHILD_ENGINE_COPY,id=" << replay::entity::IdText(puppet.packet.id)
                          << ",copy=" << object << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec << ",action=hide\n";
        }
    }

    // vehicle_pieces.h wheel children: the wheel models of each vehicle model,
    // read once from a puppet of it (rung 2 on the replay's own vehicle, the model
    // info layout proven by its signatures), and the wheel bits of each child.
    int g_modelInfoLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch
    std::unordered_map<std::uint32_t, VehicleWheelModelsRead> g_vehicleWheelModels; // vehicle model hash
    std::unordered_map<std::uint32_t, std::uint16_t> g_wheelChildBits;              // child packed id
    std::unordered_set<std::uint32_t> g_wheelChildLogged;
    namespace
    {
    constexpr ULONGLONG kWaitClockStaleMs = 250;
    }
    ULONGLONG WaitedMs(std::unordered_map<std::uint32_t, WaitClock>& clocks, std::uint32_t key, ULONGLONG now)
    {
        WaitClock& clock = clocks[key];
        if (!clock.last || now - clock.last > kWaitClockStaleMs)
            clock.since = now;
        clock.last = now;
        return now - clock.since;
    }
    std::unordered_map<std::uint32_t, std::vector<LeftWheel>> g_leftWheels; // vehicle packed id
    // (vehicle packed id, prop) -> the vehicle puppet whose wheel object the
    // prop's went on as (a rebuilt puppet gives one again).
    std::map<std::pair<std::uint32_t, std::uint32_t>, Entity> g_leftWheelsDetached;
    std::set<std::pair<Entity, std::uint32_t>> g_leftWheelMissLogged; // (vehicle puppet, prop): one wait line each
    std::unordered_map<Entity, ULONGLONG> g_wheelsPendingSince; // vehicle puppet -> first asked
    std::unordered_map<Entity, WheelCountSeen> g_wheelCountSeen;   // vehicle puppet -> its count and since when
    std::unordered_map<std::uint32_t, std::int32_t> g_modelWheelCount; // vehicle model -> the most wheels a puppet made
    std::unordered_set<Entity> g_wheelsMade;                     // vehicle puppets seen with a wheel object
    std::unordered_map<std::uint32_t, FragPieceParent> g_fragPieceParents;
    std::unordered_map<std::uint32_t, std::vector<std::uint16_t>> g_fragPieceGroupsOf; // prop packed id -> PoseV2FragPieceTakes

    const replay::vehicle_pieces::WheelModels* PoseV2VehicleWheelModels(const replay::playback::Puppet& vehicle)
    {
        if (vehicle.packet.kind != replay::entity::EntityKind::Vehicle || !vehicle.clone ||
            !replay::entities::Exists(vehicle.clone) || !ENTITY::IS_ENTITY_A_VEHICLE(vehicle.clone))
            return nullptr;
        VehicleWheelModelsRead& entry = g_vehicleWheelModels[vehicle.packet.modelHash];
        if (!entry.read)
        {
            entry.read = true;
            if (g_modelInfoLayout == 0)
            {
                g_modelInfoLayout = replay::vehicle_pieces::ModelInfoLayoutProven(
                    [](const replay::sig::Signature& s) { return replay::sig::Find(s) != 0; }) ? 1 : -1;
            }
            const bool read = g_modelInfoLayout > 0 &&
                replay::vehicle_pieces::ReadVehicleWheelModels(
                    [](std::uint64_t at, void* into, std::size_t size) {
                        return replay::mem::Copy(into, static_cast<std::uintptr_t>(at), size);
                    },
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(vehicle.clone)), entry.models);
            // The wheel maker's own source (the draft data's model info), when the
            // puppet has one: the same models.
            replay::vehicle_pieces::WheelRead draft;
            const char* draftError = ReadPoseV2WheelObject(vehicle.clone, 0, draft);
            Ctx().log << "POSE_V2_VEHICLE_WHEEL_MODELS,vehicle=" << replay::entity::IdText(vehicle.packet.id) << ",model=0x"
                      << std::hex << vehicle.packet.modelHash << ",status="
                      << (g_modelInfoLayout < 0 ? "signature_mismatch" : read ? "read" : "none") << ",front=0x"
                      << entry.models.front << ",rear=0x" << entry.models.rear << ",draft_front=0x" << draft.frontModel
                      << ",draft_rear=0x" << draft.rearModel << std::dec
                      << ",draft=" << (draftError && *draftError ? draftError : "ok") << '\n';
        }
        return entry.models.front || entry.models.rear ? &entry.models : nullptr;
    }

    std::uint16_t PoseV2WheelChildBits(std::uint32_t child, std::uint32_t childModel,
        const replay::vehicle_pieces::WheelModels& models)
    {
        const auto found = g_wheelChildBits.find(child);
        if (found != g_wheelChildBits.end())
            return found->second;
        std::uint16_t bits = 0;
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        replay::entity::TransformSample first;
        if (const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(child))
            if (track->Span(firstMs, lastMs) && track->Sample(firstMs, first))
                bits = replay::vehicle_pieces::WheelChildBits(childModel, models, first.relative.position.x,
                    first.relative.position.y);
        g_wheelChildBits[child] = bits;
        return bits;
    }

    // entity_transform.h AttachFrameAgreement: the frame entry that starts
    // at frameFrom (and ends at frameTo) against the recording, at one time
    // inside it (settled after the change): the parent's recorded pose of
    // the frame's bone composed with the frame, against the weapon track.
    // Recorded data only, no engine read. True (kept) when the data to judge
    // is missing: the frame then plays as before.
    bool JudgePoseV2AttachFrame(const replay::playback::Puppet& puppet, const replay::playback::Puppet& parentPuppet,
        const replay::weapon::AttachFrame& frame, std::uint32_t frameFrom, std::uint32_t frameTo)
    {
        constexpr std::uint32_t kSettleMs = 250;
        static std::uint32_t logs = 0;
        const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed());
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        if (!track || !track->Span(firstMs, lastMs) || frame.bone < 0)
            return true;
        // Two times inside the entry, both must miss: a moment where the
        // track is between two attaches does not throw a good frame away.
        const std::uint32_t startMs = (std::max)(frameFrom, firstMs);
        const std::uint32_t endMs = (std::min)(frameTo, lastMs);
        const std::uint32_t span = endMs > startMs ? endMs - startMs : 0u;
        const std::uint32_t times[2] = {startMs + (std::min)(kSettleMs, span / 2u), startMs + span / 2u + span / 4u};
        static replay::pose::PoseSample parentPose;
        replay::pose::WorldTransform offset;
        offset.position = {frame.position[0], frame.position[1], frame.position[2]};
        offset.rotation = {frame.rotation[0], frame.rotation[1], frame.rotation[2], frame.rotation[3]};
        const char* verdict = "relative_track";
        replay::entity::FrameAgreement agreement;
        std::uint32_t judgeMs = times[0];
        for (const std::uint32_t at : times)
        {
            judgeMs = at;
            replay::entity::TransformSample recorded;
            const replay::entity::PoseSegment* segment = Ctx().clip.segments.At(parentPuppet.packet.id, at);
            const std::uint32_t recordId = segment ? segment->recordId : parentPuppet.packet.id.Packed();
            if (!track->Sample(at, recorded, &Ctx().clip.cuts) ||
                !replay::pose::capture::SampleTrack(recordId, at, parentPose) ||
                parentPose.objects.size() <= static_cast<std::size_t>(frame.bone))
            {
                verdict = "no_parent_pose";
                break;
            }
            const replay::pose::BoneQts& bone = parentPose.objects[static_cast<std::size_t>(frame.bone)];
            replay::pose::WorldTransform boneObject;
            boneObject.position = {bone.translation[0], bone.translation[1], bone.translation[2]};
            boneObject.rotation = bone.rotation;
            agreement = replay::entity::AttachFrameAgreement(boneObject, offset, recorded.relative);
            if (replay::entity::AttachFrameAgrees(agreement))
            {
                verdict = "agrees";
                break;
            }
        }
        const bool keep = std::strcmp(verdict, "relative_track") != 0;
        if (!keep || logs < 32)
        {
            ++logs;
            if (logs <= 200)
            {
                std::ostream& log = Ctx().log;
                const replay::logfmt::FormatScope logFormat(log); // precision not left behind
                log << std::fixed << std::setprecision(3) << "POSE_V2_ATTACH_FRAME_JUDGE,id="
                    << replay::entity::IdText(puppet.packet.id) << ",parent=" << replay::entity::IdText(parentPuppet.packet.id)
                    << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec << ",bone=" << frame.bone
                    << ",frame_from=" << frameFrom << ",judge_ms=" << judgeMs << ",angle_deg=" << agreement.angleDeg
                    << ",pos_err=" << agreement.distanceM << ",verdict=" << verdict << '\n';
                log.unsetf(std::ios_base::floatfield);
            }
        }
        return keep;
    }

    void AttachPoseV2Weapon(replay::playback::Puppet& puppet, const replay::playback::Puppet& parentPuppet,
        const replay::pose::WorldTransform& relative, const replay::pose::WorldTransform& world,
        std::uint32_t timeMs)
    {
        const Ped parent = parentPuppet.clone;
        // Evidence: the first 60 frames, then every 500 ms of playhead.
        // At most kAttachCheckLogCap lines a playback (the
        // shootout clip wrote 13635, one per weapon every 500 ms, each with
        // strap reads and a flush to the log on the game disk).
        constexpr std::uint32_t kAttachCheckLogCap = 600;
        const bool periodic = puppet.attachCheckPending &&
            (puppet.attachCheckLoggedMs == 0 || timeMs < puppet.attachCheckLoggedMs ||
                timeMs - puppet.attachCheckLoggedMs >= 500);
        // The check reads the engine only for its line,
        // so Debug=1 only.
        if (puppet.attachCheckPending && replay::log::Verbose() &&
            (g_poseV2AttachLogs < 60 || (periodic && g_poseV2AttachLogs < kAttachCheckLogCap)))
        {
            ++g_poseV2AttachLogs;
            puppet.attachCheckLoggedMs = timeMs;
            const replay::pose::WorldTransform actual = ReadEntityTransform(puppet.clone);
            const float angle =
                replay::pose::AngleBetween(actual.rotation, puppet.attachWanted.rotation) * 57.29578f;
            const float dx = actual.position.x - puppet.attachWanted.position.x;
            const float dy = actual.position.y - puppet.attachWanted.position.y;
            const float dz = actual.position.z - puppet.attachWanted.position.z;
            std::ostream& log = Ctx().log;
            const replay::logfmt::FormatScope logFormat(log); // precision not left behind
            log << std::fixed << std::setprecision(3)
                << "POSE_V2_WEAPON_ATTACH_CHECK,id=" << replay::entity::IdText(puppet.packet.id)
                << ",time_ms=" << timeMs
                << ",angle_deg=" << angle
                << ",pos_err=" << std::sqrt(dx * dx + dy * dy + dz * dz)
                << ",bone=" << (puppet.attachFrameApplied ? puppet.attachFrame.bone : -1)
                << ",frame_attach=" << (puppet.attachFrameApplied ? 1 : 0)
                << ",detour_skipped=" << replay::weapon::StrapVisibilitySnapshot().skipped
                << "," << PoseV2WeaponStrapFields(puppet.clone);
            replay::weapon::StrapState strap;
            if (replay::weapon::ReadStrapState(
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), strap) &&
                strap.hasStraps)
            {
                log << ",mask=";
                for (const auto& bit : strap.maskBits)
                    log << std::hex << bit.first << std::dec << ':' << (bit.second ? 1 : 0) << ' ';
            }
            log << '\n';
            log.unsetf(std::ios_base::floatfield);
            log.flush();
        }
        // Recorded attach frame (parent bone + offsets): the engine attach
        // the original had, so the strap update sees the same parent bone.
        const replay::weapon::AttachFrame* frame = nullptr;
        const auto frameTrack = std::find_if(Ctx().clip.attachFrames.begin(), Ctx().clip.attachFrames.end(),
            [&](const replay::AttachFrameTrack& entry) { return entry.recordId == puppet.packet.id.Packed(); });
        if (frameTrack != Ctx().clip.attachFrames.end() && !frameTrack->frames.empty() &&
            !g_vehicleChildRecords.count(puppet.packet.id.Packed()))
        {
            frame = &frameTrack->frames.front().second;
            std::uint32_t frameFrom = frameTrack->frames.front().first;
            std::uint32_t frameTo = 0xFFFFFFFFu;
            for (const auto& entry : frameTrack->frames)
            {
                if (entry.first <= timeMs)
                {
                    frame = &entry.second;
                    frameFrom = entry.first;
                }
                else
                {
                    frameTo = entry.first;
                    break;
                }
            }
            // A frame that does not put the object where it was recorded
            // (the crowd torch of a clip recorded before the child bone was
            // read) is left out: the relative track below places it.
            if (puppet.attachJudgedFrom != frameFrom)
            {
                puppet.attachJudgedFrom = frameFrom;
                puppet.attachJudgedOk = JudgePoseV2AttachFrame(puppet, parentPuppet, *frame, frameFrom, frameTo);
            }
            if (!puppet.attachJudgedOk)
                frame = nullptr;
        }
        if (frame)
        {
            const bool same = puppet.attachFrameApplied && puppet.attachFrameParent == parent &&
                SameAttachFrame(puppet.attachFrame, *frame) &&
                ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(puppet.clone, parent);
            if (!same)
            {
                const replay::pose::Quat rotation{
                    frame->rotation[0], frame->rotation[1], frame->rotation[2], frame->rotation[3]};
                Vector3 frameEuler{};
                if (PoseV2EngineEuler(rotation, puppet.packet.modelHash, frameEuler))
                {
                    if (!puppet.attachedToParent)
                        ENTITY::FREEZE_ENTITY_POSITION(puppet.clone, FALSE);
                    ENTITY::ATTACH_ENTITY_TO_ENTITY(puppet.clone, parent, frame->bone,
                        frame->position[0], frame->position[1], frame->position[2],
                        frameEuler.x, frameEuler.y, frameEuler.z,
                        FALSE, FALSE, FALSE, FALSE, kPoseV2AttachRotationOrder, TRUE, FALSE, FALSE);
                    puppet.attachedToParent = true;
                    puppet.attachFrameApplied = true;
                    puppet.attachFrameParent = parent;
                    puppet.attachFrame = *frame;
                }
            }
            if (puppet.attachFrameApplied)
            {
                puppet.attachWanted = world;
                puppet.attachCheckPending = true;
                ApplyPoseV2StrapTrack(puppet, timeMs);
                return;
            }
        }
        Vector3 euler{};
        if (!PoseV2EngineEuler(relative.rotation, puppet.packet.modelHash, euler))
        {
            replay::pose::playback::PlaceClone(puppet.clone, world);
            return;
        }
        if (!puppet.attachedToParent)
            ENTITY::FREEZE_ENTITY_POSITION(puppet.clone, FALSE);
        // Bone -1: the parent entity's own frame, the frame the relative
        // transform was recorded in (recordings without attach frames).
        ENTITY::ATTACH_ENTITY_TO_ENTITY(puppet.clone, parent, -1,
            relative.position.x, relative.position.y, relative.position.z,
            euler.x, euler.y, euler.z,
            FALSE, FALSE, FALSE, FALSE, kPoseV2AttachRotationOrder, TRUE, FALSE, FALSE);
        puppet.attachedToParent = true;
        puppet.attachFrameApplied = false;
        puppet.attachWanted = world;
        puppet.attachCheckPending = true;
        ApplyPoseV2StrapTrack(puppet, timeMs);
    }

    // Weapon puppets whose straps keep the script-side mask: the detour
    // skips the engine's not-in-inventory hide for these objects. Published
    // before and after the weapon update, so an object created this frame
    // is protected before the engine's weapon pass runs.
    void PublishPoseV2StrapOwned()
    {
        static std::vector<std::uint64_t> owned;
        static std::vector<std::uint64_t> published;
        // WeaponPassPose: the puppet weapons with an armed
        // pose slot, for the write after the weapon system's pass.
        static std::vector<replay::weapon::WeaponPoseSlot> slots;
        static std::vector<replay::weapon::WeaponPoseSlot> slotsPublished;
        owned.clear();
        slots.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Weapon || !puppet.clone ||
                puppet.state == replay::playback::PuppetState::Failed)
            {
                continue;
            }
            const auto object = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
            if (object)
            {
                owned.push_back(object);
                if (g_weaponPassReady && puppet.overrideSlot >= 0)
                    slots.push_back({object, static_cast<std::uint32_t>(puppet.overrideSlot)});
            }
        }
        if (owned != published)
        {
            published = owned;
            replay::weapon::SetStrapOwnedObjects(published);
        }
        const bool slotsChanged = slots.size() != slotsPublished.size() ||
            !std::equal(slots.begin(), slots.end(), slotsPublished.begin(),
                [](const replay::weapon::WeaponPoseSlot& a, const replay::weapon::WeaponPoseSlot& b) {
                    return a.object == b.object && a.slot == b.slot;
                });
        if (slotsChanged)
        {
            slotsPublished = slots;
            replay::weapon::SetWeaponPoseSlots(slotsPublished);
        }
    }

    // object_pose.h: an attached object recorded with a pose
    // (the carried pelt) has it written on its replay object. Armed through
    // the bodies' own arming (RefreshOverride: the same slot table, re-arm on
    // a rebuilt skeleton), only on a skeleton with the recorded bone count
    // (the body path would re-apply metaped layers otherwise). Published and
    // written from the script thread every tick, since the pose finalizer
    // may never visit a prop's skeleton (as with vehicles), and also after
    // the finalizers (late), so no engine pass leaves the bind pose, the flat
    // sheet seen in a test clip.
    replay::pose::PoseSample g_poseV2ObjectSample;
    std::uint32_t g_poseV2ObjectPlayLogs = 0;
    std::set<std::uint32_t> g_poseV2ObjectPlayNoted;
    std::map<std::uint64_t, PoseV2SkeletonAsk> g_poseV2ObjectSkeletonAsked;

    // still_pose_core.h the still rule (StillPoseEngine) and the drawn check
    // (ObjectDrawnCheck).
    bool g_objectDrawnCheck = false;
    std::unordered_map<std::uint32_t, replay::tpose::TrackSignature> g_stillSignatures;
    std::uint32_t g_stillLines = 0;
    std::uint32_t g_drawnLines = 0;
    namespace
    {
    int g_drawnBindLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch
    }
    std::vector<replay::pose::BoneQts> g_drawnExpected;
    std::vector<replay::pose::BoneQts> g_drawnLocals;

    const replay::tpose::TrackSignature& StillSignature(std::uint32_t recordId)
    {
        auto found = g_stillSignatures.find(recordId);
        if (found == g_stillSignatures.end())
        {
            replay::tpose::TrackSignature signature;
            if (!replay::pose::capture::ReadTrackSignature(recordId, signature))
                signature = {};
            found = g_stillSignatures.emplace(recordId, signature).first;
        }
        return found->second;
    }
    namespace
    {

    // The bind rotations of `skeleton`, read once per skeleton (rung 2: the
    // layout tpose_check.h proves from the finalizer's code; a mismatch reads
    // nothing and the check reports bind=- instead of Bind).
    bool DrawnBind(replay::playback::Puppet& puppet, std::uint8_t* skeleton)
    {
        if (!skeleton)
            return false;
        if (puppet.bindSkeleton == skeleton)
            return !puppet.bindRotations.empty();
        puppet.bindSkeleton = skeleton;
        puppet.bindRotations.clear();
        if (g_drawnBindLayout == 0)
        {
            g_drawnBindLayout = replay::tpose::BindLayoutProven(
                [](const replay::sig::Signature& site) { return replay::sig::Find(site) != 0; }) ? 1 : -1;
            Ctx().log << "POSE_V2_OBJECT_DRAWN_BIND_LAYOUT,status="
                      << (g_drawnBindLayout > 0 ? "proven" : "signature_mismatch") << ",engine_writes=0\n";
        }
        if (g_drawnBindLayout < 0)
        {
            puppet.bindError = "layout_signature";
            return false;
        }
        puppet.bindError = replay::tpose::ReadBindRotations(
            [](std::uintptr_t address, void* out, std::size_t size) { return replay::mem::Copy(out, address, size); },
            reinterpret_cast<std::uintptr_t>(skeleton), puppet.bindRotations);
        return !puppet.bindRotations.empty();
    }
    }

    // One drawn check: what the last frame drew against what the replay
    // expects, the line on the first check and on each flip (capped).
    // `expected` and `drawn` hold `bones` locals, then `bones` object
    // matrices when withObjects.
    void NoteDrawn(replay::playback::Puppet& puppet, const char* mode, const replay::pose::BoneQts* expected,
        const replay::pose::BoneQts* drawn, std::uint32_t bones, bool withObjects, std::uint8_t* skeleton, int slot,
        std::uint32_t timeMs)
    {
        const bool haveBind = DrawnBind(puppet, skeleton) && puppet.bindRotations.size() == bones;
        const replay::stillpose::FullComparison c = replay::stillpose::CompareFull(
            expected, drawn, haveBind ? puppet.bindRotations.data() : nullptr, bones, withObjects);
        const bool first = puppet.drawn.checks == 0;
        const bool flipped = puppet.drawn.Add(c);
        if (slot >= 0)
            replay::pose::capture::NoteOverrideDrawn(static_cast<std::uint32_t>(slot),
                c.shape != replay::stillpose::Shape::Same);
        if (!replay::stillpose::ShouldLogCheck(first, flipped, g_drawnLines))
            return;
        ++g_drawnLines;
        std::ostream& log = Ctx().log;
        const replay::logfmt::FormatScope logFormat(log);
        log << std::fixed << std::setprecision(3) << "POSE_V2_OBJECT_DRAWN,id=" << replay::entity::IdText(puppet.packet.id)
            << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec << ",mode=" << mode
            << ",shape=" << replay::stillpose::ShapeName(c.shape) << ",bones=" << bones << ",changed=" << c.locals.changed
            << ",near_bind=" << c.locals.nearBind << ",worst_bone=" << c.locals.worstBone << ",max_rad=" << c.locals.maxRad
            << ",max_m=" << c.locals.maxM << ",objects=" << (withObjects ? 1 : 0) << ",obj_changed=" << c.objects.changed
            << ",obj_worst_bone=" << c.objects.worstBone << ",obj_max_rad=" << c.objects.maxRad
            << ",obj_max_m=" << c.objects.maxM << ",bind=" << (haveBind ? "read" : (puppet.bindError[0] ? puppet.bindError : "-"))
            << ",checks=" << puppet.drawn.checks << ",flips=" << puppet.drawn.flips << ",first=" << (first ? 1 : 0)
            << ",playhead_ms=" << timeMs << '\n';
    }

    // The drawn check of an object the still rule left to the engine: its
    // buffer against the record's constant pose (the rule is right when the
    // engine draws that pose: shape=same; bind means the engine shows its
    // bind pose, the flat sheet, and the rule should go off for it).
    void CheckStillDrawn(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved,
        std::uint32_t recordId, std::uint32_t timeMs)
    {
        const std::uint32_t bones = resolved.boneCount;
        if (puppet.stillLocals.empty())
        {
            // The record's constant pose: locals, then object matrices when
            // the record has them.
            replay::pose::PoseSample sample;
            if (!replay::pose::capture::SampleTrack(recordId, timeMs, sample) || sample.locals.size() != bones)
                return;
            puppet.stillLocals = std::move(sample.locals);
            if (sample.objects.size() == bones)
                puppet.stillLocals.insert(puppet.stillLocals.end(), sample.objects.begin(), sample.objects.end());
        }
        const bool withObjects = puppet.stillLocals.size() == static_cast<std::size_t>(bones) * 2u;
        g_drawnLocals.resize(puppet.stillLocals.size());
        if (!resolved.poseBuffer ||
            !replay::mem::Copy(g_drawnLocals.data(), resolved.poseBuffer, g_drawnLocals.size() * sizeof(replay::pose::BoneQts)))
            return;
        NoteDrawn(puppet, "still", puppet.stillLocals.data(), g_drawnLocals.data(), bones, withObjects,
            resolved.skeleton, -1, timeMs);
    }
    namespace
    {

    // A real weapon whose pose the clip has (WeaponBonePose), reached and armed
    // or not (WEAPON_BONES,phase=play; weapon_bones_static in the fx check).
    void CheckWeaponDrawn(const replay::playback::Puppet& puppet, std::uint32_t timeMs);
    }

    void PlayPoseV2ObjectPose(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        CheckWeaponDrawn(puppet, timeMs); // before this tick's publish
        PlayPoseV2ObjectPoseBody(puppet, timeMs);
        if (puppet.packet.kind != replay::entity::EntityKind::Weapon || !puppet.packet.weaponHash || !puppet.clone)
            return;
        const std::uint32_t recordId = puppet.packet.id.Packed();
        const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
        if (replay::object_pose::PlaybackBones(puppet.packet.boneCount, create ? create->boneCount : 0) <
            replay::object_pose::kMinBones)
            return;
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        if (!replay::pose::capture::TrackSpan(recordId, firstMs, lastMs) || timeMs < firstMs || timeMs > lastMs)
            return;
        // Every weapon in a hand has a track;
        // one that never moved is left to the engine on purpose (StillPoseEngine,
        // POSE_V2_OBJECT_STILL) and is not "recorded and never armed".
        if (puppet.stillSkip)
            return;
        const bool firstSeen = g_weaponBonesSeen.insert(recordId).second;
        const bool armed = puppet.overrideSlot >= 0;
        const bool firstArmed = armed && g_weaponBonesArmed.insert(recordId).second;
        if ((firstArmed || (firstSeen && !armed)) && g_weaponBonesPlayLogs < 24)
        {
            ++g_weaponBonesPlayLogs;
            Ctx().log << "WEAPON_BONES,phase=play,id=" << replay::entity::IdText(puppet.packet.id) << ",weapon=0x" << std::hex
                      << puppet.packet.weaponHash << std::dec << ",status=" << (armed ? "armed" : "not_armed")
                      << ",slot=" << puppet.overrideSlot << ",track_ms=" << firstMs << "-" << lastMs << ",playhead_ms=" << timeMs
                      << '\n';
        }
    }

    void LogWeaponDrawn(std::uint32_t recordId, const WeaponDrawn& drawn, const char* when)
    {
        if (g_weaponDrawnLines >= 96)
            return;
        ++g_weaponDrawnLines;
        const replay::weaponbones::PlayCheck& c = drawn.check;
        // The bones the record turns and the drawing does not.
        std::string frozen;
        std::uint32_t frozenCount = 0;
        for (std::size_t n = 0; n < drawn.recTicks.size(); ++n)
            if (replay::weaponbones::FrozenBone(drawn.recTicks[n], drawn.drawnTicks[n]))
            {
                ++frozenCount;
                if (frozen.size() < 120)
                    frozen += (frozen.empty() ? "" : " ") + std::to_string(n) + ":" + std::to_string(drawn.recTicks[n]) + "/" +
                        std::to_string(drawn.drawnTicks[n]);
            }
        char line[400];
        std::snprintf(line, sizeof(line),
            "WEAPON_BONES_DRAWN,when=%s,id=%s,weapon=0x%08x,model=0x%08x,checks=%u,track_moving=%u,drawn_same=%u,"
            "drawn_other=%u,lost_motion=%u,max_deg=%.1f,worst_bone=%d,weapon_pass=%s,verdict=%s",
            when, replay::entity::IdText(replay::entity::ReplayId::FromPacked(recordId)).c_str(), drawn.weapon, drawn.model, c.checks, c.trackMoving, c.drawnSame,
            c.drawnOther, c.drawnMovingOther, c.maxRad * 57.29578f, c.worstBone,
            g_weaponPassReady ? "installed" : g_weaponPassError.c_str(),
            replay::weaponbones::VerdictName(replay::weaponbones::Verdict(c)));
        Ctx().log << line << ",frozen_bones=" << frozenCount << ",frozen=" << (frozen.empty() ? "-" : frozen) << '\n';
    }
    namespace
    {

    // weapon_bones_core.h PlayCheck: an armed weapon pose
    // inside its track; the buffer as the last frame left it against the
    // pose the replay wrote last tick (ReadOverrideDrawn, read only).
    void CheckWeaponDrawn(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        if (puppet.packet.kind != replay::entity::EntityKind::Weapon || !puppet.packet.weaponHash ||
            puppet.overrideSlot < 0 || !puppet.armedBones)
            return;
        const std::uint32_t recordId = puppet.packet.id.Packed();
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        if (!replay::pose::capture::TrackSpan(recordId, firstMs, lastMs) || timeMs < firstMs || timeMs > lastMs)
            return;
        if (!replay::pose::capture::ReadOverrideDrawn(static_cast<std::uint32_t>(puppet.overrideSlot),
                g_weaponDrawnPublished, g_weaponDrawnBuffer))
            return;
        const std::size_t bones = puppet.armedBones;
        if (g_weaponDrawnPublished.size() < bones || g_weaponDrawnBuffer.size() < bones)
            return;
        WeaponDrawn& drawn = g_weaponDrawn[recordId];
        drawn.model = puppet.packet.modelHash;
        drawn.weapon = puppet.packet.weaponHash;
        bool trackMoved = false;
        if (drawn.previous.size() == bones)
            trackMoved = replay::weaponbones::DiffLocals(drawn.previous.data(), g_weaponDrawnPublished.data(), bones).moved > 0;
        // Per bone, written turn against drawn turn (object
        // matrices too when both were read: what the mesh uses).
        {
            const bool objects = g_weaponDrawnBuffer.size() >= bones * 2u && g_weaponDrawnPublished.size() >= bones * 2u;
            const std::size_t width = objects ? bones * 2u : bones;
            if (drawn.recTicks.size() != bones)
            {
                drawn.recTicks.assign(bones, 0u);
                drawn.drawnTicks.assign(bones, 0u);
            }
            if (drawn.previous.size() == bones && drawn.previousDrawn.size() == width)
            {
                for (std::size_t n = 1; n < bones; ++n)
                {
                    const float rec = replay::weaponbones::QuatAngle(drawn.previous[n].rotation, g_weaponDrawnPublished[n].rotation);
                    float now = replay::weaponbones::QuatAngle(drawn.previousDrawn[n].rotation, g_weaponDrawnBuffer[n].rotation);
                    if (objects)
                        now = (std::max)(now, replay::weaponbones::QuatAngle(drawn.previousDrawn[bones + n].rotation,
                                                  g_weaponDrawnBuffer[bones + n].rotation));
                    drawn.recTicks[n] += rec > replay::weaponbones::kMovedRad ? 1u : 0u;
                    drawn.drawnTicks[n] += now > replay::weaponbones::kMovedRad ? 1u : 0u;
                }
            }
            drawn.previousDrawn.assign(g_weaponDrawnBuffer.begin(), g_weaponDrawnBuffer.begin() + width);
        }
        drawn.previous.assign(g_weaponDrawnPublished.begin(), g_weaponDrawnPublished.begin() + bones);
        const replay::weaponbones::PoseDiff diff =
            replay::weaponbones::DiffLocals(g_weaponDrawnPublished.data(), g_weaponDrawnBuffer.data(), bones);
        replay::weaponbones::NoteCheck(drawn.check, trackMoved, diff);
        // One line per weapon once there was motion to show (a log cut before
        // the playback's end still has it), the totals at the end.
        if (!drawn.interimLogged && drawn.check.trackMoving >= 15)
        {
            drawn.interimLogged = true;
            LogWeaponDrawn(recordId, drawn, "interim");
        }
    }
    }


    // Prop puppets: a plain object of the recorded model, without physics
    // or collision, placed by its recorded world transform. Shown only
    // inside its track, so it appears when it leaves the ped.
    // A vehicle piece puppet is the object the engine makes
    // when the recorded group breaks off the vehicle puppet
    // (replay::frag::BreakOffVehicleGroup), adopted and placed by the
    // piece's recorded world track. Waits for its vehicle puppet; the budget
    // of creations per frame applies. A group already gone on the puppet
    // (nothing to break off) fails the piece until the vehicle is built
    // again (ReleaseVehiclePieces). True when the piece is Ready now.
    std::uint32_t g_vehiclePieceLogs = 0;

    // The pieces broken off a vehicle puppet at their group's own break, before
    // their record starts (the recorder finds a piece's object a few ticks after
    // the break, and a later break of a group above it would take it with it):
    // (vehicle packed id, group) -> the object, hidden until its piece adopts it.
    std::map<std::pair<std::uint32_t, std::uint16_t>, Object> g_vehicleFragPieces;
}
