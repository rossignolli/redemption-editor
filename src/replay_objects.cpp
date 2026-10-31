// replay_objects.cpp: the module's entry points (attach, the switches, the recording scan) and the helpers every part uses.
// The other parts are in replay_objects_*.cpp; shared internals in replay_objects_internal.h.

#include "replay_objects_internal.h"

namespace replay::objects
{
    namespace
    {

        Context* g_context = nullptr;
    }
        Context& Ctx() { return *g_context; }

        // The object's vtable and its pre-render slot (0x258, the slot the
        // ped pre-render detour uses) as module RVAs, read only (rung 2): the
        // class of the pelt on the horse, live and replay, before any
        // end-of-frame bone capture is hooked there. "-" when unreadable.
        std::string ObjectClassText(const void* entity)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::uintptr_t vtable = 0;
            std::uintptr_t preRender = 0;
            if (!entity || !replay::mem::Read(reinterpret_cast<std::uintptr_t>(entity), vtable) || vtable < base ||
                !replay::mem::Read(vtable + 0x258, preRender) || preRender < base)
                return "vtable=-,prerender=-";
            char text[64];
            std::snprintf(text, sizeof(text), "vtable=0x%llX,prerender=0x%llX",
                static_cast<unsigned long long>(vtable - base), static_cast<unsigned long long>(preRender - base));
            return text;
        }

        // object_check.h: the engine owner of an entity (5 bits
        // of [entity+0xB4], rung 2), objcheck::kOwnerUnknown when unread.
        std::uint32_t ReadEntityOwner(Entity entity)
        {
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(entity));
            std::uint32_t field = 0;
            if (!address || !replay::mem::Read(address + replay::objcheck::kOwnerFieldOffset, field))
                return replay::objcheck::kOwnerUnknown;
            return replay::objcheck::OwnerOf(field);
        }

        // hat_look.h: the layer a hat object draws, read from its own tags
        // ([object+0x410], count +0x418) after the two code sites that prove
        // the layout matched their bytes once per session (-1 unchecked, 0
        // mismatch, 1 proven). Returns the status: "unproven", "unread",
        // "no_tags" or "read".
        int g_hatLookCore = -1;
        int g_hatLookWalk = -1;

        std::uint32_t g_objectWeaponFromObject = 0; // weapon objects resolved from the object
        std::uint32_t g_objectWeaponUnresolved = 0; // a weapon object whose weapon did not validate
        std::uint32_t g_objectCheckLogs = 0;
    namespace
    {
        constexpr std::uint32_t kObjectCheckLogCap = 64;
    }

        // The QA ring's line (qa_health.h ObjectWrong) for a verdict.
        void LogObjectCheck(replay::objcheck::Verdict verdict, const std::string& id, std::uint32_t model,
            std::uint32_t playheadMs, const char* extra)
        {
            if (g_objectCheckLogs >= kObjectCheckLogCap)
                return;
            ++g_objectCheckLogs;
            Ctx().log << "POSE_V2_OBJECT_CHECK,id=" << id << ",verdict=" << replay::objcheck::VerdictText(verdict)
                      << ",model=0x" << std::hex << model << std::dec << extra;
            // 0: no playhead of its own (the QA ring takes the current one).
            if (playheadMs)
                Ctx().log << ",playhead_ms=" << playheadMs;
            Ctx().log << '\n';
        }

        // weapon_look_core.h: the engine's own component list and wear fields
        // (rung 2, guarded reads). Each is checked against the natives the
        // first time (component list against HAS_WEAPON_GOT_WEAPON_COMPONENT,
        // wear against the four getters); a disagreement turns that read off
        // for the session (logged) and the natives are used.
        bool g_weaponComponentsEngine = true;
        bool g_weaponComponentsChecked = false;
        std::uint32_t g_weaponComponentsCrossChecks = 0;
        bool g_weaponWearRaw = true;
        bool g_weaponWearChecked = false;
        // Playback: the two wear fields no native sets (rust +0xD4, the
        // sixth channel +0xE4) written on the replay's own weapon (rung 3).
        bool g_weaponWearWrite = true;
        std::uint32_t g_weaponLookRecLogs = 0;
        std::uint32_t g_weaponLookPlayLogs = 0;
        replay::weaponlook::Tally g_weaponLookTally;
    namespace
    {

        const char* PoseV2ComponentName(std::uint32_t hash)
        {
            for (const replay::weapon::Component& component : replay::weapon::kComponents)
                if (component.hash == hash)
                    return component.name;
            return nullptr;
        }
    }

        replay::weaponlook::Wear WearOf(const replay::WeaponCondition& state)
        {
            return {state.dirt, state.soot, state.damage, state.degradation, state.permanent, state.wearE4};
        }
    namespace
    {

    // Strap visibility detour: installed once, on the first
    // capture or playback start; logged like the other observers.
    bool g_poseV2StrapVisibilityTried = false;
    bool g_poseV2StrapVisibilityReady = false;

    // WeaponPassPose (ini, default 1): weapon_observer.h InstallWeaponPassPose.
    bool g_weaponPassPose = true;
    bool g_weaponPassTried = false;
    }
    bool g_weaponPassReady = false;
    std::string g_weaponPassError = "not_tried";
    namespace
    {

    // The weapon pass detour, installed with the strap one
    // (first capture or playback start, and WarmUp).
    void EnsurePoseV2WeaponPass()
    {
        if (g_weaponPassTried)
            return;
        g_weaponPassTried = true;
        if (!g_weaponPassPose)
            g_weaponPassError = "off";
        else
        {
            std::string error;
            g_weaponPassReady = replay::weapon::InstallWeaponPassPose(error);
            g_weaponPassError = g_weaponPassReady ? "none" : error;
        }
        Ctx().log << "WEAPON_PASS_POSE,status=" << (g_weaponPassReady ? "installed" : g_weaponPassPose ? "failed" : "off")
                  << ",error=" << g_weaponPassError << ",rva=0x117D49C,engine_writes=puppet_weapon_bones" << '\n';
    }

    void EnsurePoseV2StrapVisibility()
    {
        EnsurePoseV2WeaponPass();
        if (g_poseV2StrapVisibilityTried)
            return;
        g_poseV2StrapVisibilityTried = true;
        std::string error;
        g_poseV2StrapVisibilityReady = replay::weapon::InstallStrapVisibility(error);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_STRAP_VISIBILITY"
            << ",status=" << (g_poseV2StrapVisibilityReady ? "installed" : "failed")
            << ",error=" << (error.empty() ? "none" : error.c_str())
            << ",engine_writes=0"
            << '\n';
        log.flush();
    }
    }

    std::uint32_t g_poseV2AttachLogs = 0; // weapon attach evidence cap
    std::uint32_t g_strapApplyLogs = 0;

    std::uint32_t g_poseV2WeaponEvidenceLines = 0; // weapon evidence cap


    bool SameAttachFrame(const replay::weapon::AttachFrame& a, const replay::weapon::AttachFrame& b)
    {
        if (a.bone != b.bone)
            return false;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::fabs(a.position[axis] - b.position[axis]) > 0.0005f)
                return false;
        }
        const replay::pose::Quat qa{a.rotation[0], a.rotation[1], a.rotation[2], a.rotation[3]};
        const replay::pose::Quat qb{b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]};
        return replay::pose::AngleBetween(qa, qb) * 57.29578f < 0.02f;
    }

    // Entity frame as the engine applies it: the world position of the
    // local origin and of the unit axes, so no assumption about the order
    // GET_ENTITY_MATRIX returns its vectors in.
    replay::pose::WorldTransform ReadEntityTransform(Entity entity)
    {
        const Vector3 origin = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 0.0f, 0.0f, 0.0f);
        const Vector3 x = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 1.0f, 0.0f, 0.0f);
        const Vector3 y = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 0.0f, 1.0f, 0.0f);
        const Vector3 z = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 0.0f, 0.0f, 1.0f);
        replay::pose::WorldTransform transform;
        transform.position = {origin.x, origin.y, origin.z};
        transform.rotation = replay::pose::QuatFromBasis(
            {x.x - origin.x, x.y - origin.y, x.z - origin.z},
            {y.x - origin.x, y.y - origin.y, y.z - origin.z},
            {z.x - origin.x, z.y - origin.y, z.z - origin.z});
        return transform;
    }

    std::uint32_t g_poseV2ObjectPoseLogs = 0;

    // object_pose.h capture places of posed objects (shared with the weapons
    // in a hand) and the recording's evidence counters (POSE_V2_OBJECT_POSES at
    // the end).

    // WeaponBonePose: the weapons in a hand posed by their own bones, in places
    // of their own.
    bool g_weaponBonePose = true;
    std::uint32_t g_weaponBonePlaces = 0; // WeaponPlaces at BeginRecording
    // Playback: the weapons with a recorded pose the playhead reached and those armed.
    std::set<std::uint32_t> g_weaponBonesSeen;
    std::set<std::uint32_t> g_weaponBonesArmed;
    std::uint32_t g_weaponBonesPlayLogs = 0;
    std::map<std::uint32_t, WeaponDrawn> g_weaponDrawn;
    // The recorded weapons published to the weapon pass
    // detour (sampled after the pass), record id -> weapon hash.
    std::map<std::uint32_t, std::uint32_t> g_passRecWeapons;
    std::vector<replay::weapon::WeaponRecordEntry> g_passRecPublished;
    std::uint32_t g_weaponDrawnLines = 0;
    // AmmoScanEveryTick: see ScanPoseV2HandAmmo.
    bool g_ammoScanEveryTick = true;
    std::vector<int> g_ammoPrevHandles;
    std::vector<AmmoPending> g_ammoPending;

    std::size_t CountPosedObjects()
    {
        std::size_t posed = 0;
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            posed += weapon.posed && !weapon.bonePose ? 1u : 0u; // the weapon places are apart
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            posed += prop.posed ? 1u : 0u;
        return posed;
    }
    // The places limit of the log lines: the shared places.
    std::uint32_t ObjectPlacesLimit() { return g_weaponBonePlaces; }


    void Attach(Context& context)
    {
        g_context = &context;
        replay::objlook::Attach(context);
    }
    void SetObjectBurnLevel(bool enabled) { g_objectBurnLevel = enabled; }
    bool ObjectBurnLevel() { return g_objectBurnLevel; }
    void SetObjectDrawnCheck(bool drawnCheck) { g_objectDrawnCheck = drawnCheck; }
    bool ObjectDrawnCheck() { return g_objectDrawnCheck; }

    void EnsureStrapVisibility() { EnsurePoseV2StrapVisibility(); }

    void WarmUp()
    {
        EnsurePoseV2StrapVisibility();
        WarmUpFrag();
    }
    bool StrapVisibilityReady() { return g_poseV2StrapVisibilityReady; }

    void Scan() { ScanPoseV2Weapons(); }
    void SetWeaponBonePose(bool on) // places from the free slots at each recording
    {
        g_weaponBonePose = on;
    }
    void WeaponBonesPlayed(std::uint32_t& seen, std::uint32_t& armed)
    {
        seen = static_cast<std::uint32_t>(g_weaponBonesSeen.size());
        armed = static_cast<std::uint32_t>(g_weaponBonesArmed.size());
    }
    // The drawn verdicts of this playback (one line per
    // weapon checked, then the totals), for the fx check.
    std::uint32_t g_weaponDrawnFrozen = 0; // weapons with a frozen bone, last WeaponBonesDrawn

    void WeaponBonesDrawn(std::uint32_t& overwritten, std::uint32_t& mixed, std::uint32_t& shown, std::uint32_t& none)
    {
        overwritten = mixed = shown = none = 0;
        g_weaponDrawnFrozen = 0;
        std::uint64_t checks = 0, other = 0, lost = 0, moving = 0;
        for (const auto& [recordId, drawn] : g_weaponDrawn)
        {
            const replay::weaponbones::PlayVerdict verdict = replay::weaponbones::Verdict(drawn.check);
            overwritten += verdict == replay::weaponbones::PlayVerdict::Overwritten ? 1u : 0u;
            mixed += verdict == replay::weaponbones::PlayVerdict::Mixed ? 1u : 0u;
            shown += verdict == replay::weaponbones::PlayVerdict::Shown ? 1u : 0u;
            none += verdict == replay::weaponbones::PlayVerdict::None ? 1u : 0u;
            checks += drawn.check.checks;
            other += drawn.check.drawnOther;
            lost += drawn.check.drawnMovingOther;
            moving += drawn.check.trackMoving;
            for (std::size_t n = 0; n < drawn.recTicks.size(); ++n)
                if (replay::weaponbones::FrozenBone(drawn.recTicks[n], drawn.drawnTicks[n]))
                {
                    ++g_weaponDrawnFrozen;
                    break;
                }
            LogWeaponDrawn(recordId, drawn, "end");
        }
        const replay::weapon::WeaponPassStats pass = replay::weapon::WeaponPassSnapshot();
        Ctx().log << "WEAPON_BONES_DRAWN_TOTAL,weapons=" << g_weaponDrawn.size() << ",overwritten=" << overwritten
                  << ",mixed=" << mixed << ",shown=" << shown << ",none=" << none << ",checks=" << checks
                  << ",drawn_other=" << other << ",track_moving=" << moving << ",lost_motion=" << lost
                  << ",weapon_pass=" << (g_weaponPassReady ? "installed" : g_weaponPassError.c_str())
                  << ",pass_calls=" << pass.calls << ",pass_hits=" << pass.hits << ",pass_writes=" << pass.writes
                  << ",pass_faults=" << pass.faults << ",frozen_weapons=" << g_weaponDrawnFrozen << '\n';
    }
    std::uint32_t WeaponBonesFrozen() { return g_weaponDrawnFrozen; }
    // weapon_look_core.h: the weapons whose look was read
    // back this playback and the ones whose last check disagreed, with the
    // totals line.
    void WeaponLookPlayed(std::uint32_t& checked, std::uint32_t& componentsMismatch, std::uint32_t& wearMismatch)
    {
        checked = componentsMismatch = wearMismatch = 0;
        for (const auto& [id, verdict] : g_weaponLookVerdicts)
        {
            checked += verdict.checked ? 1u : 0u;
            componentsMismatch += verdict.components ? 1u : 0u;
            wearMismatch += verdict.wear ? 1u : 0u;
        }
        g_weaponLookTally.checked = checked;
        g_weaponLookTally.componentsMismatch = componentsMismatch;
        g_weaponLookTally.wearMismatch = wearMismatch;
        Ctx().log << "WEAPON_LOOK_TOTAL,checked=" << checked << ",components_mismatch=" << componentsMismatch
                  << ",wear_mismatch=" << wearMismatch << ",applies=" << g_weaponLookTally.applies
                  << ",reapplied=" << g_weaponLookTally.reapplied << ",engine_components=" << (g_weaponLookEnginePlay ? 1 : 0)
                  << ",wear_write=" << (g_weaponWearWrite ? 1 : 0) << '\n';
    }
    void SetWeaponWearWrite(bool on) { g_weaponWearWrite = on; }
    void SetWeaponPassPose(bool on) { g_weaponPassPose = on; }
    // object_pose.h spawn
    void DiscardPendingEvents()
    {
        if (!g_fragReady)
            return;
        static std::vector<replay::frag::BreakCall> breaks;
        replay::frag::CollectSince(g_fragCursor, breaks);
    }

    void Sample()
    {
        SamplePoseV2Weapons();
        ScanPoseV2HandAmmo();
    }
    void SetAmmoScanEveryTick(bool on) { g_ammoScanEveryTick = on; }
    void UpdateRecordedProps() { UpdatePoseV2Props(); }
}
