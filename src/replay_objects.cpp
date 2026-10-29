#include "replay_objects.h"
#include "replay_trains.h" // the train cars' pose places

#include "audio_observer.h"
#include "burn_level.h" // object burn level
#include "frag_observer.h"
#include "frag_piece.h" // a loose piece is not the whole object
#include "entity_appearance.h"
#include "entity_pools.h"
#include "log_format.h"
#include "pose_playback.h"
#include "pose_capture.h"
#include "pose_segments.h"
#include "profiler.h"
#include "entity_cache.h"
#include "guarded_memory.h"
#include "hat_look.h" // the look of a loose hat
#include "log_policy.h"
#include "object_pose.h"
#include "object_check.h"
#include "object_look.h" // custom texture and tint
#include "replay_world.h" // the map dressing decides after the clip's map state
#include "rope_models.h" // a rope's own entity is no prop
#include "skeleton_resolve.h"
#include "skeleton_walk.h"
#include "track_index.h"
#include "vehicle_pieces.h"
#include "weapon_components.h"
#include "weapon_observer.h"

#include "main.h"
#include "natives.h"
#include "weapon_bones_core.h" // weapon bones play check
#include "weapon_look_core.h" // the weapon look
#include "strap_state_core.h" // the strap state played on a puppet weapon

#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace replay::objects
{
    // Defined with the fragment breaks' playback, used by the
    // pieces' shaping (UpdatePoseV2Props).
    int GuardedGroupGone(std::uint64_t entity, std::uint16_t group);
    int GuardedGroupToChild(std::uint64_t entity, std::uintptr_t base, std::uint16_t group);
    // The stand-ins and the originals the
    // stands hide, sorted (defined with the stands).
    void CollectPoseV2ReplayOwnObjects(std::vector<Object>& out);

    namespace
    {
        Context* g_context = nullptr;
        Context& Ctx() { return *g_context; }

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;

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
        // mismatch, 1 proven). Returns the status: "off", "unproven", "unread",
        // "no_tags" or "read".
        bool g_hatLookEnabled = true; // ini HatLook
        int g_hatLookCore = -1;
        int g_hatLookWalk = -1;
        struct HatLookStats
        {
            std::uint32_t props = 0;   // prop creates that read a look
            std::uint32_t scenery = 0; // scenery entries that read a look
            std::uint32_t unread = 0;  // a guarded read failed
            std::uint32_t logs = 0;    // POSE_V2_HAT_LOOK lines this recording
        } g_hatLookStats;
        constexpr std::uint32_t kHatLookLogCap = 32;

        const char* ReadPoseV2HatLook(Object object, std::vector<replay::entity::MetaPedAsset>& out)
        {
            out.clear();
            if (!g_hatLookEnabled)
                return "off";
            if (g_hatLookCore < 0)
            {
                const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
                g_hatLookCore = base && replay::mem::Matches(base + replay::hatlook::kTagCoreRva,
                    replay::hatlook::kTagCoreSignature, sizeof(replay::hatlook::kTagCoreSignature)) ? 1 : 0;
                g_hatLookWalk = base && replay::mem::Matches(base + replay::hatlook::kTagWalkRva,
                    replay::hatlook::kTagWalkSignature, sizeof(replay::hatlook::kTagWalkSignature)) ? 1 : 0;
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

        std::uint32_t g_objectWeaponFromObject = 0; // weapon objects resolved from the object
        std::uint32_t g_objectWeaponUnresolved = 0; // a weapon object whose weapon did not validate
        std::uint32_t g_objectCheckLogs = 0;
        constexpr std::uint32_t kObjectCheckLogCap = 64;

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

        // The components a weapon object carries (no native lists them).
        void ReadObjectComponents(Object object, std::vector<std::uint32_t>& out)
        {
            out.clear();
            for (const replay::weapon::Component& component : replay::weapon::kComponents)
                if (WEAPON::HAS_WEAPON_GOT_WEAPON_COMPONENT(object, component.hash))
                    out.push_back(component.hash);
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
        constexpr std::uint32_t kWeaponLookLogCap = 400;

        const char* PoseV2ComponentName(std::uint32_t hash)
        {
            for (const replay::weapon::Component& component : replay::weapon::kComponents)
                if (component.hash == hash)
                    return component.name;
            return nullptr;
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
                // getter answers 0 for some owners, 0x1164FD0, so it is left
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

        replay::weaponlook::Wear WearOf(const replay::WeaponCondition& state)
        {
            return {state.dirt, state.soot, state.damage, state.degradation, state.permanent, state.wearE4};
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

    // Strap visibility detour: installed once, on the first
    // capture or playback start; logged like the other observers.
    bool g_poseV2StrapVisibilityTried = false;
    bool g_poseV2StrapVisibilityReady = false;

    // WeaponPassPose (ini, default 1): weapon_observer.h InstallWeaponPassPose.
    bool g_weaponPassPose = true;
    bool g_weaponPassTried = false;
    bool g_weaponPassReady = false;
    std::string g_weaponPassError = "not_tried";

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

        // The layers a prop wears: its own, or the ones its owner lost at the
        // pose segment nearest to the prop's create.
        const char* PropLayersOf(const replay::entity::CreatePacket& prop, replay::entity::Appearance& out)
        {
            return replay::entity::PropLayers(
                Ctx().clip.segments, Ctx().clip.FindCreate(prop.owner.Packed()), prop, out);
        }

    // Weapon objects: an object entity whose create carries the weapon,
    // attached to its ped. Every recorded ped's weapon attach points are read
    // at the scan; each object found gets its own ReplayId and a WeaponPoint
    // attachment, and its transform is sampled every frame in the ped's space
    // (both matrices read in the same frame).

    // A weapon that leaves its attach point continues as a prop (below).
    void ContinuePoseV2WeaponAsProp(const replay::recorder::RecordedWeapon& weapon, const char* reason);
    void RetirePoseV2PropOf(Object object, const char* reason);

    std::uint32_t g_poseV2AttachLogs = 0; // weapon attach evidence cap
    // POSE_V2_STRAP_APPLY: one line per strap state change on a puppet,
    // at most this many a playback (a scrub back and forth repeats them).
    constexpr std::uint32_t kStrapApplyLogCap = 400;
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

    bool SameStrapState(const replay::weapon::StrapState& a, const replay::weapon::StrapState& b)
    {
        return a.maskBits == b.maskBits && (a.flags & 0x20000u) == (b.flags & 0x20000u) &&
            a.weightA == b.weightA && a.weightB == b.weightB;
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

    std::string PoseV2WeaponName(Hash weapon)
    {
        const char* name = WEAPON::_GET_WEAPON_NAME(weapon);
        std::string text = name && *name ? name : "unknown";
        std::replace(text.begin(), text.end(), ',', ' ');
        return text;
    }

    std::string PoseV2WeaponStrapFields(Object object);

    std::uint32_t g_poseV2ObjectPoseLogs = 0;

    // object_pose.h capture places of posed objects (dev ini ObjectPosePlaces:
    // 0, the default, the shared places; N the old rule, N places given up by
    // the ped class) and the recording's evidence counters
    // (POSE_V2_OBJECT_POSES at the end).
    std::uint32_t g_objectPosePlaces = 0;
    bool ObjectPoseShared() { return g_objectPosePlaces == 0; }
    struct ObjectPoseStats
    {
        std::uint32_t started = 0;
        std::uint32_t late = 0;
        std::uint32_t continued = 0;
        std::uint32_t noPlaceTries = 0;
        std::uint32_t skeletonChanged = 0;
        std::uint32_t peak = 0;
        std::uint32_t spawned = 0; // spawned free objects posed at their register
    } g_objectPoseStats;

    // WeaponBonePose: the weapons in a hand posed by their own bones, in places
    // of their own.
    bool g_weaponBonePose = true;
    std::uint32_t g_weaponBonePlaces = 0; // WeaponPlaces at BeginRecording
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
    } g_weaponBoneStats;
    // Playback: the weapons with a recorded pose the playhead reached and those armed.
    std::set<std::uint32_t> g_weaponBonesSeen;
    std::set<std::uint32_t> g_weaponBonesArmed;
    std::uint32_t g_weaponBonesPlayLogs = 0;
    // weapon_bones_core.h the drawn check of every armed weapon pose,
    // independent of ObjectDrawnCheck: at the start of the tick (before this
    // tick's publish) the buffer against the pose written last tick, and
    // whether that pose moved since the tick before (there was motion to show).
    struct WeaponDrawn
    {
        std::vector<replay::pose::BoneQts> previous; // the written locals of the last check
        // Per bone, ticks the record turned it and ticks the
        // drawn buffer (locals and objects) turned it.
        std::vector<replay::pose::BoneQts> previousDrawn;
        std::vector<std::uint32_t> recTicks;
        std::vector<std::uint32_t> drawnTicks;
        replay::weaponbones::PlayCheck check;
        std::uint32_t model = 0;
        std::uint32_t weapon = 0;
        bool interimLogged = false;
    };
    std::map<std::uint32_t, WeaponDrawn> g_weaponDrawn;
    // The recorded weapons published to the weapon pass
    // detour (sampled after the pass), record id -> weapon hash.
    std::map<std::uint32_t, std::uint32_t> g_passRecWeapons;
    std::vector<replay::weapon::WeaponRecordEntry> g_passRecPublished;
    std::vector<replay::pose::BoneQts> g_weaponDrawnPublished;
    std::vector<replay::pose::BoneQts> g_weaponDrawnBuffer;
    std::uint32_t g_weaponDrawnLines = 0;
    // AmmoScanEveryTick: see ScanPoseV2HandAmmo.
    bool g_ammoScanEveryTick = true;
    std::vector<int> g_ammoPrevHandles;
    struct AmmoPending
    {
        Object object = 0;
        std::uint32_t firstMs = 0;
    };
    std::vector<AmmoPending> g_ammoPending;
    struct AmmoStats
    {
        std::uint32_t ticks = 0;
        std::uint32_t fresh = 0;
        std::uint32_t asked = 0;
        std::uint32_t registered = 0;
        std::uint32_t expired = 0;
        std::uint32_t overflow = 0; // new objects left to the 200 ms scan (caps)
        std::uint32_t logs = 0;
    } g_ammoStats;
    constexpr std::uint32_t kAmmoAskMs = 250;
    constexpr std::uint32_t kAmmoFreshPerTick = 32;
    constexpr std::size_t kAmmoPendingMax = 128;

    std::size_t CountPosedObjects()
    {
        std::size_t posed = 0;
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            posed += weapon.posed && !weapon.bonePose ? 1u : 0u; // the weapon places are apart
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            posed += prop.posed ? 1u : 0u;
        return posed;
    }

    std::size_t CountWaitingObjects()
    {
        std::size_t waiting = 0;
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            waiting += weapon.poseCandidate && weapon.poseNoPlace ? 1u : 0u;
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            waiting += prop.poseCandidate && prop.poseNoPlace ? 1u : 0u;
        return waiting;
    }

    std::size_t CountPedBodies()
    {
        std::size_t peds = 0;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            peds += entity.vehicle ? 0u : 1u;
        return peds;
    }

    // object_pose.h: resolves an attached object's skeleton and,
    // when it has bones and a capture place is free, starts its pose record
    // under the object's own id. Shared by the register and by the retry of
    // an object whose skeleton did not exist yet at the attach.
    // The place rule counts the posed weapons and props, and with
    // ObjectPosePlaces > 0 an object takes a place the ped class gave up
    // (object_pose.h ShouldPose); `posedOut` is the posed count before it.
    std::size_t CountWeaponBones(bool posed);

    // The places limit of the log lines: the shared places, or ObjectPosePlaces.
    std::uint32_t ObjectPlacesLimit() { return ObjectPoseShared() ? g_weaponBonePlaces : g_objectPosePlaces; }

    const char* TryStartPoseV2ObjectPose(Object object, replay::entity::ReplayId id,
        replay::skeleton::ResolvedPoseSkeleton& objectSkeleton, std::string& poseError, bool& posed,
        std::size_t* posedOut = nullptr)
    {
        posed = false;
        const std::size_t posedObjects = CountPosedObjects();
        // object_pose.h ShouldPoseShared: the objects and the weapons in a
        // hand share the slots the bodies leave.
        const std::size_t posedWeapons = ObjectPoseShared() ? CountWeaponBones(true) : 0;
        if (posedOut)
            *posedOut = posedObjects + posedWeapons;
        if (!replay::skeleton::FastResolvePedPoseSkeleton(object, objectSkeleton))
            return "skeleton_unresolved";
        const bool place = ObjectPoseShared()
            ? replay::object_pose::ShouldPoseShared(false, objectSkeleton.boneCount, posedObjects, posedWeapons,
                  g_weaponBonePlaces)
            : replay::object_pose::ShouldPose(false, objectSkeleton.boneCount, CountPedBodies(), posedObjects,
                  g_objectPosePlaces, replay::recorder::kPedCapacity, Ctx().recorder.entities.size(),
                  replay::pose::capture::kMaxLiveTargets);
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

    bool PoseV2ObjectPoseRetryable(const char* status)
    {
        return std::strcmp(status, "skeleton_unresolved") == 0 || std::strcmp(status, "no_place") == 0;
    }

    // The retry (object_pose.h ShouldRetryResolve). A late start
    // writes the bone count into the create the clip keeps (the last one
    // with this id), which FindCreate and the pose segments read on
    // playback; the lifecycle blocks already written keep 0 (PlaybackBones).
    // Shared by the weapons and the props (RecordedWeapon and
    // RecordedProp have the same pose fields); the create patched is the
    // weapon's or the prop's.
    template <typename Recorded>
    void RetryPoseV2ObjectPose(Recorded& recorded, std::uint32_t timeMs)
    {
        recorded.nextPoseTryMs = timeMs + replay::object_pose::kRetryIntervalMs;
        ++recorded.poseTries;
        replay::skeleton::ResolvedPoseSkeleton objectSkeleton{};
        std::string poseError;
        bool posed = false;
        std::size_t posedBefore = 0;
        const char* status =
            TryStartPoseV2ObjectPose(recorded.object, recorded.id, objectSkeleton, poseError, posed, &posedBefore);
        if (PoseV2ObjectPoseRetryable(status))
        {
            recorded.poseNoPlace = std::strcmp(status, "no_place") == 0; // ReservedPlaces
            // Evidence at about 1 s and 10 s of retries, not every try.
            if ((recorded.poseTries == 4 || recorded.poseTries == 40) && g_poseV2ObjectPoseLogs < 48)
            {
                ++g_poseV2ObjectPoseLogs;
                Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(recorded.id)
                          << ",status=retry_" << status << ",tries=" << recorded.poseTries
                          << ",bones=" << objectSkeleton.boneCount << ",places=" << posedBefore << "/"
                          << ObjectPlacesLimit() << ",peds=" << CountPedBodies()
                          << ",walk=" << replay::skeleton::DescribePoseSkeletonWalk(recorded.object)
                          << ",time_ms=" << timeMs << '\n';
            }
            return;
        }
        recorded.poseCandidate = false;
        recorded.poseNoPlace = false;
        bool createPatched = false;
        if (posed)
        {
            ++g_objectPoseStats.late;
            recorded.posed = true;
            recorded.poseSkeleton = objectSkeleton.skeleton;
            recorded.poseBones = objectSkeleton.boneCount;
            for (auto packet = Ctx().clip.created.rbegin(); packet != Ctx().clip.created.rend(); ++packet)
            {
                if (packet->id == recorded.id && (packet->kind == replay::entity::EntityKind::Weapon ||
                                                     packet->kind == replay::entity::EntityKind::Prop))
                {
                    packet->boneCount = objectSkeleton.boneCount;
                    createPatched = true;
                    break;
                }
            }
        }
        if (g_poseV2ObjectPoseLogs < 48)
        {
            ++g_poseV2ObjectPoseLogs;
            Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(recorded.id)
                      << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(recorded.object))
                      << std::dec << ",status=" << status << ",late=1,tries=" << recorded.poseTries
                      << ",bones=" << objectSkeleton.boneCount
                      << ",fragment_path=" << (objectSkeleton.usedFragmentPath ? 1 : 0)
                      << ",getter=" << replay::skeleton::walk::GetterKindName(
                             static_cast<replay::skeleton::walk::GetterKind>(objectSkeleton.getterKind))
                      << ",places=" << (posedBefore + (posed ? 1 : 0)) << "/" << ObjectPlacesLimit()
                      << ",peds=" << CountPedBodies()
                      << ",create_patched=" << (createPatched ? 1 : 0)
                      << ",error=" << (poseError.empty() ? "none" : poseError.c_str()) << ",time_ms=" << timeMs
                      << '\n';
        }
    }

    // Per sample of a weapon or a prop with a pose (or waiting for
    // one): the retry, then the skeleton check of a posed object. Moved out
    // of SamplePoseV2Weapons so a prop that continues a posed
    // object is sampled the same way.
    template <typename Recorded>
    void SamplePoseV2ObjectPose(Recorded& recorded, std::uint32_t timeMs)
    {
        if (replay::object_pose::ShouldRetryResolve(recorded.poseCandidate, recorded.posed, timeMs, recorded.nextPoseTryMs))
            RetryPoseV2ObjectPose(recorded, timeMs);
        if (!recorded.posed)
            return;
        // object_pose.h: the object's pose, sampled here when the
        // pose finalizer did not visit its skeleton (as a vehicle's). A
        // skeleton the engine rebuilt ends the record: the armed pointer
        // would no longer be the object's.
        replay::skeleton::ResolvedPoseSkeleton current{};
        if (replay::skeleton::FastResolvePedPoseSkeleton(recorded.object, current) &&
            current.skeleton == recorded.poseSkeleton && current.boneCount == recorded.poseBones)
        {
            replay::pose::capture::PollIfStalled(recorded.id.Packed());
            return;
        }
        replay::pose::capture::Stop(recorded.id.Packed());
        recorded.posed = false;
        recorded.poseCandidate = false; // the record id is not started twice
        ++g_objectPoseStats.skeletonChanged;
        Ctx().log << "POSE_V2_OBJECT_POSE,phase=record,id=" << replay::entity::IdText(recorded.id)
                  << ",status=skeleton_changed,bones=" << current.boneCount
                  << ",armed_bones=" << recorded.poseBones << ",time_ms=" << timeMs << '\n';
    }

    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out);

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

    // object_pose.h WeaponPlaces: a real weapon in a hand records its own
    // bones from now on, every tick, like every animated object (no wait for
    // the locals to move, no places but the free capture slots). A weapon
    // whose skeleton does not read yet is watched and started at the first read
    // that has one.
    void EndPoseV2WeaponBoneWatch(replay::recorder::RecordedWeapon& weapon);

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
            const std::size_t taken = CountWeaponBones(true) + (ObjectPoseShared() ? CountPosedObjects() : 0);
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

    void EndPoseV2WeaponBoneWatch(replay::recorder::RecordedWeapon& weapon)
    {
        weapon.boneWatch = false;
        std::vector<float>().swap(weapon.boneWatchLocals);
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
    // _SET_OBJECT_BURN_LEVEL writes (handler 0x10C10B8 -> 0x10CEAFC: entity
    // +0x28, +0x18, block from the virtual +0x88, then 0x6FD2E4 `movss
    // [rcx+0x1C], xmm1`; every script call has affectAsh true, so +0x20 is
    // never the one): burn::Read on the fire path, four times a second per
    // object, stored by NoteBurnLevel (the first read always: the replay's
    // object starts with the model's level, the cooking script's meat with 0).
    // A guarded memory read, no native. Ini ObjectBurnLevel (1 by default)
    // records and plays it.
    bool g_objectBurnLevel = true;
    // The ticks a scenery stand-in stays shown after the prop that takes it
    // over published its first pose (ini SceneryHandoffTicks, 0..8, default 1;
    // 0: hidden in the tick the prop puppet is Ready, as before).
    std::uint32_t g_sceneryHandoffTicks = 1;
    constexpr std::uint32_t kObjectBurnCheckMs = 250;
    struct ObjectBurnStats
    {
        std::uint32_t reads = 0;      // objects read with a block
        std::uint32_t noBlock = 0;    // reads without one (unreadable, no handler yet, a class with no block)
        std::uint32_t tracks = 0;     // weapon object and prop tracks begun
        std::uint32_t changes = 0;    // states stored after the first
        std::uint32_t sceneryTracks = 0;
        std::uint32_t sceneryChanges = 0;
    } g_objectBurnStats;
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

    template <class Recorded>
    void SamplePoseV2ObjectBurnLevel(Recorded& recorded, std::uint32_t timeMs)
    {
        if (!g_objectBurnLevel || !replay::burn::Enabled())
            return;
        if (recorded.burnChecked && timeMs - recorded.burnCheckMs < kObjectBurnCheckMs)
            return;
        recorded.burnChecked = true;
        recorded.burnCheckMs = timeMs;
        static replay::TrackIndex<replay::BurnLevelTrack> index;
        NotePoseV2ObjectBurn(Ctx().clip.burnLevels, index, recorded.id.Packed(), recorded.object, false, timeMs);
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
                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), frame))
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
                            << ",bone=" << frame.bone
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

    // Props that come off a recorded ped (a knocked off hat), as the
    // reference records a detached ped prop: an object entity of its own,
    // created when it leaves the ped, with its world transform sampled at
    // the capture rate. The ped's pose segment removes the prop's layer at
    // the same moment. The track lives with the weapon tracks, keyed by
    // the prop's ReplayId; its samples are world transforms.

    std::uint32_t g_poseV2PropsDropped = 0; // loose objects not recorded: the registry was full
    std::uint32_t g_poseV2RopeModelsSkipped = 0; // rope entities left to the rope track
    std::uint32_t g_poseV2RopeModelLogs = 0;
    // A prop of a throwable weapon (knife, tomahawk, hatchet,
    // from any origin) goes under the projectile successor watch; its fate
    // is logged when the watch ends (below).
    void WatchPoseV2Throwable(Object object, replay::entity::ReplayId id, replay::entity::ReplayId ownerId,
        Entity ownerHandle, Hash weaponHash);
    void EndPoseV2ProjectileWatchOf(Object object, const char* reason);

    // object_pose.h ContinuesPose: the weapon a dropped weapon
    // prop continues, and whether it was posed or waited for a pose.
    struct PoseContinuation
    {
        replay::entity::ReplayId from;
        bool posed = false;
        bool candidate = false;
    };

    // The groups of a loose fragment piece (below, with the breaks).
    void NotePoseV2FragPiece(Object object, replay::entity::ReplayId id, replay::entity::PropOrigin origin, Hash weapon);

    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out);

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
    } g_propBoneStats;
    constexpr std::uint32_t kPropBoneLogCap = 48;

    std::size_t CountPropBoneWatches()
    {
        std::size_t watching = 0;
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            watching += prop.boneWatch ? 1u : 0u;
        return watching;
    }

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

    void EndPoseV2PropBoneWatch(replay::recorder::RecordedProp& prop)
    {
        prop.boneWatch = false;
        std::vector<float>().swap(prop.boneWatchLocals);
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
        const std::vector<std::uint32_t>& weaponComponents = {}, std::uint32_t pieceGroup = 0,
        const PoseContinuation* continuation = nullptr, bool spawnedPose = false)
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
    // A recorded projectile until it is gone: where it was last. When it
    // sticks the engine replaces it with a new plain object in the same spot
    // (always against anything that is not a ped): that object continues it.
    struct ProjectileWatch
    {
        Object object = 0;
        replay::entity::ReplayId owner;
        Entity ownerHandle = 0;
        Vector3 last{};
        ULONGLONG goneAt = 0;
        std::uint32_t model = 0;
        // The watched prop's id (fate evidence), the throwable
        // weapon it came from (0 for a projectile seen after a shot), and
        // since when it has been at rest (a throwable left where it stuck
        // ends its watch there).
        replay::entity::ReplayId id;
        Hash weaponHash = 0;
        ULONGLONG restSince = 0;
    };
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
    // The object listing of the previous scan: a successor is an object the
    // engine creates, absent there (otherwise the dead ped's hat next to
    // the stuck arrow would be taken).
    std::vector<int> g_poseV2ProjectilePrevObjects;

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
            // three the network attach path treats as arrows (0x235C580).
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

    // World objects. As a replay editor turns a map object into a replay
    // object once it moves, an object near the recorded actors that moves,
    // turns or breaks becomes a prop with its world track, and the clip keeps
    // how it stood when the recording started; a new object of its model
    // next to it (a broken-off piece, same model as its parent) becomes a
    // prop too. Which fragment groups broke is not known without a detour:
    // the stand-in shows the object whole until its prop takes over.
    struct WorldSnap
    {
        std::uint32_t model = 0;
        Vector3 position{};
        Vector3 rotation{};
        bool broken = false;
        bool taken = false;
        // object_check.h: the bones of an object with a
        // skeleton (the camp's donation box and ledger), watched so the
        // object is recorded, with its pose, when it animates in place.
        std::uint32_t bones = 0;
        std::vector<float> locals;
        std::uint32_t nextLocalsMs = 0;
        // objcheck::SkeletonWatch: whether the bones were read, and when a
        // reread near a recorded actor is next due (a skeleton that appears is
        // an animation: the mission chest's lid).
        replay::objcheck::SkeletonWatch skeleton = replay::objcheck::SkeletonWatch::Unread;
        std::uint32_t nextSkeletonMs = 0;
        // objcheck::NearWatch: within reach of a recorded actor at the last
        // scan (its bones then read every tick, not four times a second), and
        // away from its rest pose at the snapshot (a drawer left open).
        bool watchedNear = false;
        bool openAtStart = false;
    };
    std::unordered_map<Object, WorldSnap> g_worldSnapshot;
    // The snapshot objects with bones within reach of a recorded actor, read
    // every tick (SamplePoseV2NearWorldObjects), with that actor.
    struct NearWorldObject
    {
        Object object = 0;
        replay::entity::ReplayId actor{};
        Entity actorHandle = 0;
    };
    std::vector<NearWorldObject> g_nearWorldObjects;
    std::uint32_t g_nearWorldTaken = 0;    // recorded by the every-tick read
    std::uint32_t g_openAtStartTaken = 0;  // recorded because open at the snapshot
    struct WorldTaken
    {
        Object object = 0;
        std::uint32_t model = 0;
    };
    std::vector<WorldTaken> g_worldTaken;
    std::uint32_t g_worldAnimatedRecorded = 0;
    std::uint32_t g_worldAnimatedWatched = 0;
    std::uint32_t g_worldLateMapObjects = 0;   // map objects first seen after the snapshot
    // objcheck::AfterReread: the skeleton rereads near recorded
    // actors, the skeletons that appeared (recorded as animated), the
    // baselines taken for objects the snapshot cap left unread, the late
    // scenery joined to the watch.
    struct WorldSkeletonStats
    {
        std::uint32_t rereads = 0;
        std::uint32_t appeared = 0;
        std::uint32_t baselines = 0;
        std::uint32_t lateScenery = 0;
        std::uint32_t logs = 0;
        // Rereads and rebuilds judged against the bind pose (away: recorded as
        // an animation, map objects included; rest: a baseline; unknown: the
        // bind pose not read), the map objects that joined the watch late
        // (objcheck::LateJoinWatch) and the snapshot's watch order.
        std::uint32_t restAway = 0;
        std::uint32_t restAtRest = 0;
        std::uint32_t restUnknown = 0;
        std::uint32_t appearedMap = 0; // appeared on a map object (the new rule)
        std::uint32_t lateJoins = 0;
        float watchedFarthestM = -1.0f; // the farthest object the snapshot read the bones of
        std::uint32_t unreadNearM10 = 0; // snapshot objects within 10 m left unread (the cap)
    } g_worldSkeletonStats;
    std::unordered_set<Object> g_worldLateJoinDecided; // looked at once for the late join

    // object_check.h: the scenery of the recording, the
    // objects of the scene that are not of the map. Live entries are
    // watched for leaving; late ones settle 1 s (an object still moving is
    // left to the paths that follow moving objects).
    struct SceneryLive
    {
        Object object = 0;
        std::size_t index = 0;
    };
    std::vector<SceneryLive> g_sceneryLive;
    std::unordered_map<Object, std::size_t> g_sceneryOf;
    struct SceneryPendingObject
    {
        std::uint32_t firstMs = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t owner = replay::objcheck::kOwnerUnknown;
        bool mission = false;
        bool pickup = false;
        std::uint32_t appearMs = 0; // the first sight when it was seen far and stood there since
    };
    std::unordered_map<Object, SceneryPendingObject> g_sceneryPending;
    std::unordered_set<Object> g_scenerySeen;
    // objcheck::kSceneryFarRecheckMs: the listed objects out of the scenery
    // radius, looked at again every second until they come within it (marked
    // seen only then): their first sight and where they stood.
    struct SceneryFar
    {
        std::uint32_t firstMs = 0;
        std::uint32_t nextMs = 0;
        float position[3]{};
    };
    std::unordered_map<Object, SceneryFar> g_sceneryFar;
    std::uint32_t g_lateSceneryLogs = 0; // POSE_V2_SCENERY_LATE lines
    constexpr std::uint32_t kLateSceneryLogCap = 48;
    std::uint32_t g_sceneryNextScanMs = 0;
    struct SceneryStats
    {
        std::uint32_t snapshot = 0;
        std::uint32_t late = 0;
        std::uint32_t gone = 0;
        std::uint32_t taken = 0;
        std::uint32_t moving = 0;
        std::uint32_t full = 0;
        std::uint32_t invisible = 0;       // hidden when recorded
        std::uint32_t visibilityFlips = 0;
        std::uint32_t pickups = 0;         // pickup objects among the entries
        std::uint32_t lootWatched = 0;     // objects a recorded ped came within reach of
        std::uint32_t lootHidden = 0;      // taken: hidden with a ped within reach
        std::uint32_t lootGone = 0;        // taken: removed with a ped within reach
        std::uint32_t lootDropped = 0;     // hidden or removed with nobody within reach
        std::uint32_t lootMoved = 0;       // left the spot while watched (carried, pushed)
        std::uint32_t lootWatchFull = 0;
        std::uint32_t spawnWatch = 0;      // late entries the spawned object watch made
        std::uint32_t farLate = 0;         // late entries first seen out of the radius
        std::uint32_t farRechecks = 0;     // looks again at an object out of the radius
        std::uint32_t farResets = 0;       // the far table started over (kMaxSceneryFar)
    } g_sceneryStats;
    // objcheck::LootWatchable: the free objects that are not scenery and that a
    // recorded ped came within reach of, watched four times a second for a take
    // (hidden or removed). Where it stood when the watch began (it does not
    // move: a moved object is the world objects' prop), its owner then and when
    // it entered the world.
    struct LootWatch
    {
        std::uint32_t model = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t owner = replay::objcheck::kOwnerUnknown;
        bool mission = false;
        std::uint32_t appearMs = 0;
    };
    std::unordered_map<Object, LootWatch> g_lootWatch;
    // Removals waiting kReinstanceMs for the same object made again before they
    // are a take.
    struct LootGonePending
    {
        Object object = 0;
        LootWatch watch;
        std::uint32_t goneMs = 0;
    };
    std::vector<LootGonePending> g_lootGonePending;
    struct LootReinstanceStats
    {
        std::uint32_t pending = 0;
        std::uint32_t reinstanced = 0;
        std::uint32_t gone = 0;
    } g_lootReinstanceStats;
    std::unordered_map<Object, std::uint32_t> g_lootFirstSeenMs; // a late object's first sight
    std::unordered_set<Object> g_lootDecided;                    // looked at within reach, not watched again
    constexpr std::size_t kLootSeenCap = 16384;                  // a long buffer recording starts these over
    std::uint32_t g_lootLogs = 0;
    constexpr std::uint32_t kLootLogCap = 64;

    void ResetPoseV2Loot()
    {
        g_lootWatch.clear();
        g_lootGonePending.clear();
        g_lootReinstanceStats = {};
        g_lootFirstSeenMs.clear();
        g_lootDecided.clear();
        g_lootLogs = 0;
    }
    // The map's own owner value, learned at the snapshot: the most frequent
    // owner among the objects there that are no scenery. Logged only
    // (map_owner=); the rules use the map data owners proven offline
    // (objcheck::IsMapDataOwner).
    std::uint32_t g_mapOwner = replay::objcheck::kOwnerUnknown;

    // The map data objects of the snapshot, nearest first, waiting for the
    // clip's IPL spheres (the world state's recording starts after the
    // snapshot): ResolvePoseV2MapDressing keeps the ones inside a sphere of an
    // IPL on.
    struct MapDressingPending
    {
        Object object = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t owner = replay::objcheck::kOwnerUnknown;
    };
    std::vector<MapDressingPending> g_mapDressingPending;
    struct MapDressingStats
    {
        std::uint32_t candidates = 0; // map data of the snapshot looked at
        std::uint32_t entries = 0;    // kept with an IPL
        std::uint32_t noIpl = 0;      // of the entries, in no sphere of an IPL on (the always-on map's)
        std::uint32_t gone = 0;       // gone before the spheres were there
        std::uint32_t full = 0;       // past kMaxMapDressing or the scenery cap
        bool listFull = false;        // the snapshot's object list was full: farther map data never seen
        std::uint32_t spheres = 0;    // the spheres of IPLs on at the resolve
        std::uint32_t streamedOut = 0; // left while their IPL stayed on (no gone time)
        std::uint32_t leftWithIpl = 0; // left with their IPL off (a gone time)
        std::uint32_t takenAway = 0;   // removed within reach of a recorded ped (a gone time: the take)
        std::uint32_t reinstanced = 0; // removed and made again at its spot (the watch goes on)
        std::set<std::uint32_t> ipls; // the IPLs that hold an entry
        bool resolved = false;
    } g_mapDressingStats;
    // The scenery_by_rule counts of the snapshot (objcheck::SceneryRule).
    std::uint32_t g_snapshotRules[replay::objcheck::kSceneryRuleCount]{};
    // objcheck::DressingRemovalOf: map dressing removed within reach of a
    // recorded ped, waiting kReinstanceMs for the same object made again
    // before its removal is the take.
    struct DressingGonePending
    {
        std::size_t index = 0; // into Ctx().clip.scenery
        std::uint32_t goneMs = 0;
    };
    std::vector<DressingGonePending> g_dressingGonePending;

    // object_check.h: the children attached to a live scenery
    // entry, watched until they leave it (gone, attached elsewhere, taken by
    // another path).
    struct SceneryChildLive
    {
        Object object = 0;
        Object parent = 0;
        std::size_t index = 0; // into Ctx().clip.sceneryChildren
    };
    std::vector<SceneryChildLive> g_sceneryChildLive;
    std::unordered_set<Object> g_sceneryChildObjects;
    std::unordered_set<std::size_t> g_sceneryChildScanned; // scenery entries whose children were looked for once
    struct SceneryChildStats
    {
        std::uint32_t added = 0;
        std::uint32_t atStart = 0; // there when the parent appeared
        std::uint32_t left = 0;
        std::uint32_t frames = 0;  // with the engine attach frame
        std::uint32_t full = 0;
        std::uint32_t visibilityFlips = 0;
        std::uint32_t fromScenery = 0; // a scenery entry of its own before it was attached
        std::uint32_t logs = 0;
    } g_sceneryChildStats;

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

    // An object of the map (a map data owner, objcheck::IsMapDataOwner), not a
    // script's.
    bool PoseV2MapObject(Object object)
    {
        const std::uint32_t owner = ReadEntityOwner(object);
        return replay::objcheck::IsMapDataOwner(owner) && ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) == FALSE;
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

    bool ReadPoseV2SpawnLocals(Object object, std::uint32_t& bones, std::vector<float>& out);
    replay::objcheck::RestState PoseV2RestOf(Object object, const std::vector<float>& locals, std::uint32_t bones,
        replay::objcheck::RestMatch& match);
    void NotePoseV2Rest(replay::objcheck::RestState rest);
    const replay::recorder::RecordedEntity* PoseV2VehicleParent(Entity handle);
    std::uint32_t g_worldObjectsRecorded = 0;
    std::uint32_t g_worldPiecesRecorded = 0;
    // Fragment breaks (evidence): the parent's address matched to
    // the snapshot, the groups that came off.
    bool g_fragReady = false;
    bool g_fragTried = false;

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
    // Pieces broken off a recorded vehicle, waiting for their
    // object (FlushPoseV2VehiclePieces).
    struct PendingVehiclePiece
    {
        std::uint64_t entity = 0;
        std::uint64_t inst = 0;
        replay::entity::ReplayId vehicle;
        Entity vehicleHandle = 0;
        std::uint16_t group = 0;
        ULONGLONG deadline = 0;
    };
    std::vector<PendingVehiclePiece> g_vehiclePiecesPending;
    std::uint32_t g_vehiclePiecesRecorded = 0;
    std::uint32_t g_vehiclePiecesLost = 0;
    // The vehicles whose part track samples at once (a break without a
    // fragment piece), and the wheels that came off waiting for their object.
    std::unordered_set<std::uint32_t> g_vehiclePartsUrgent;
    struct PendingWheelPiece
    {
        replay::entity::ReplayId vehicle;
        Entity vehicleHandle = 0;
        int wheel = 0;
        int slot = 0;
        std::uint32_t offMs = 0;
        ULONGLONG deadline = 0;
    };
    std::vector<PendingWheelPiece> g_wheelPiecesPending;
    struct WheelPieceStats
    {
        std::uint32_t off = 0;       // wheel bits that came on in the part track
        std::uint32_t recorded = 0;  // wheel objects recorded as pieces
        std::uint32_t known = 0;     // the wheel object was recorded already (another path)
        std::uint32_t lost = 0;      // not found within 1 s
        std::uint32_t noObject = 0;  // the slot held no object (reads)
        std::uint32_t readFailed = 0;
        std::uint32_t unlisted = 0;  // read, not in the listing (reads)
        std::uint32_t urgent = 0;    // samples made at once after a break
    } g_wheelPieceStats;
    std::uint32_t g_wheelPieceLogs = 0;
    int g_wheelLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch (vehicle_pieces.h WheelLayoutProven)
    // frag_piece.h: the pieces the object breaks made (PartsBrokeOff with a new
    // inst, vehicles aside), kept until a prop is registered for the piece's
    // object; the last kFragPieceCalls only.
    struct FragPieceCall
    {
        std::uint64_t entity = 0;
        std::uint64_t inst = 0;
        std::vector<std::uint16_t> groups;
        // The object it broke off (its entity), recorded or on a recorded
        // entity (a loose wheel, a vehicle's wheel): the piece is recorded at
        // any distance from the peds (PoseV2PieceOfRecorded).
        std::uint64_t parentEntity = 0;
        bool parentRecorded = false;
    };
    // The object at `address` is recorded, or hangs on a recorded entity.
    bool PoseV2RecordedOrOnRecorded(std::uint64_t address);
    constexpr std::size_t kFragPieceCalls = 64;
    std::vector<FragPieceCall> g_fragPieceCalls;
    std::uint32_t g_fragPiecesRecorded = 0;
    std::uint32_t g_fragPiecesFromCache = 0;
    std::uint32_t g_fragPieceLogs = 0;

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

    // The pieces of the breaks above become props (VehiclePiece)
    // once their object is in the listing; a piece not found within 1 s is
    // counted as lost.
    void FlushPoseV2WheelPieces();

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

    // The wheel object layout's code checked once (rung 2: a mismatch leaves
    // the wheels to the old path, logged); then the read of a vehicle's wheel
    // slot.
    bool PoseV2WheelLayout()
    {
        if (g_wheelLayout == 0)
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
            g_wheelLayout = base && replay::vehicle_pieces::WheelLayoutProven(
                [&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
                    return replay::mem::Matches(base + rva, bytes, size);
                }) ? 1 : -1;
            Ctx().log << "POSE_V2_WHEEL_LAYOUT,status=" << (g_wheelLayout > 0 ? "proven" : "signature_mismatch")
                      << ",engine_writes=0\n";
        }
        return g_wheelLayout > 0;
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

    // The scan walks the object list once. The weapon points, the attached
    // objects and the world objects each walked it with DOES_ENTITY_EXIST and
    // GET_ENTITY_ATTACHED_TO or IS_ENTITY_ATTACHED per object (0.63 and 0.60 ms
    // per scan in town); now one GET_ENTITY_ATTACHED_TO per object, existence
    // from memory.
    struct PoseV2ScanObject
    {
        Object object = 0;
        Entity parent = 0; // GET_ENTITY_ATTACHED_TO
    };
    std::vector<PoseV2ScanObject> g_poseV2ScanObjects;
    int g_poseV2ScanListed = 0;
    // Weapon points: the 32 GET_CURRENT_PED_WEAPON_ENTITY_INDEX of a
    // ped only when something may have moved: the weapon in hand changed, an
    // object attached to it is not one of its known weapons, one of its point
    // weapons is no longer attached to it, or a second went by (staggered).
    struct PoseV2PointScan
    {
        Hash held = 0;
        std::uint32_t nextFullMs = 0;
        bool scanned = false;
    };
    std::unordered_map<Entity, PoseV2PointScan> g_poseV2PointScans;
    std::uint64_t g_poseV2PointScansFull = 0;
    std::uint64_t g_poseV2PointScansSkipped = 0;
    constexpr std::uint32_t kPoseV2PointRefreshMs = 1000;

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
    bool g_spawnedObjects = true; // ini SpawnedObjects
    // objcheck::SpawnSceneryOwner: a watched object that stands still becomes a
    // late scenery entry (ini SpawnedStaticScenery): 1 (default) when it is
    // scenery by the proven owners (a script's, a temporary object, a prop
    // set's, a pickup); 2 also for any other owner that is not the map's (not
    // measured yet in a town: the would_scenery lines of mode 1 count them);
    // 0 dropped as static after the watch.
    int g_spawnedStaticScenery = 1;
    struct PoseV2SpawnWatch
    {
        std::uint32_t firstMs = 0;
        float position[3]{};
        float rotation[3]{};
        Entity ownerHandle = 0;
        std::uint32_t model = 0;
        std::uint32_t bones = 0;
        bool attachedNoParent = false;
        std::vector<float> locals; // the first sight's locals (bones >= 2)
        // The late scenery decided (once), and whether this
        // watch made the entry.
        bool sceneryDecided = false;
        bool scenery = false;
        // Watch scans that read no skeleton; a skeleton appearing after two of
        // them is an animation (the engine builds it when the object animates,
        // object_check.h).
        std::uint32_t absentReads = 0;
        // The nearest recorded ped at the last scan (-1: none), and whether the
        // watch came from the recheck.
        float nearestM = -1.0f;
        bool rewatched = false;
    };
    std::unordered_map<Object, PoseV2SpawnWatch> g_spawnWatch;
    std::unordered_set<Object> g_spawnSeen;
    // The objects refused for distance or a full watch, those whose watch ended
    // still or left it, and those attached to an entity that is not recorded,
    // looked at again every kRecheckMs while listed (seen, not decided). Where
    // it stood when it went in, and for an expired one the ped of that watch
    // (it is watched again only once re-armed).
    struct PoseV2SpawnRecheck
    {
        std::uint32_t nextMs = 0;
        float position[3]{};
        Entity expiredBy = 0;
        bool armed = true;
        bool elsewhere = false; // counted once as attached elsewhere
    };
    std::unordered_map<Object, PoseV2SpawnRecheck> g_spawnRecheck;
    struct PoseV2SpawnStats
    {
        std::uint32_t watched = 0;
        std::uint32_t recorded = 0;
        std::uint32_t moved = 0;
        std::uint32_t animated = 0;
        std::uint32_t expired = 0;
        std::uint32_t gone = 0;
        std::uint32_t taken = 0;    // recorded by another path (attached, drop)
        std::uint32_t attached = 0; // attached to an entity while watched
        std::uint32_t elsewhere = 0; // first seen attached to an entity that is not recorded
        std::uint32_t excluded = 0;  // gore, vehicle or pending drop model
        std::uint32_t full = 0;
        std::uint32_t refused = 0;
        // Still objects made late scenery entries, left as
        // the map's, found an entry already, past the watch's cap; static
        // verdicts of objects with an entry (joined the world watch).
        std::uint32_t scenery = 0;
        std::uint32_t sceneryMap = 0;
        std::uint32_t sceneryAlready = 0;
        std::uint32_t sceneryFull = 0;
        std::uint32_t sceneryWould = 0; // mode 1: mode 2 would have made an entry
        std::uint32_t staticScenery = 0;
        // First sights out of reach, rechecks made, watches started from the
        // recheck and recorded from them, expired objects re-armed, still
        // watches left (nobody near) and taken over by a nearer candidate,
        // recheck table resets.
        std::uint32_t firstFar = 0;
        std::uint32_t rechecks = 0;
        std::uint32_t rewatched = 0;
        std::uint32_t recordedRewatched = 0;
        std::uint32_t rearmed = 0;
        std::uint32_t left = 0;
        std::uint32_t evicted = 0;
        std::uint32_t recheckResets = 0;
    } g_spawnStats;
    std::uint32_t g_spawnLogs = 0;
    constexpr std::uint32_t kSpawnLogCap = 64;
    // What a rewatch recorded or refused has a line budget of its own (the
    // first sights of a town spend the 64 above in the first seconds: 139 had
    // them all by 8.8 s).
    std::uint32_t g_spawnLateLogs = 0;
    constexpr std::uint32_t kSpawnLateLogCap = 48;

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
        Ctx().log << "POSE_V2_SPAWNED_OBJECTS,enabled=" << (g_spawnedObjects ? 1 : 0)
                  << ",watched=" << g_spawnStats.watched << ",recorded=" << g_spawnStats.recorded
                  << ",moved=" << g_spawnStats.moved << ",animated=" << g_spawnStats.animated
                  << ",expired=" << g_spawnStats.expired << ",gone=" << g_spawnStats.gone
                  << ",taken=" << g_spawnStats.taken << ",attached=" << g_spawnStats.attached
                  << ",elsewhere=" << g_spawnStats.elsewhere << ",excluded=" << g_spawnStats.excluded
                  << ",full=" << g_spawnStats.full << ",refused=" << g_spawnStats.refused
                  << ",static_scenery_mode=" << g_spawnedStaticScenery
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

    // Locals just read against the skeleton's bind pose (rung 2: the layout
    // tpose_check.h proves from the pose finalizer's code, its signature
    // checked once; a mismatch leaves every verdict unknown, the old rule).
    int g_restBindLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch
    replay::objcheck::RestState PoseV2RestOf(Object object, const std::vector<float>& locals, std::uint32_t bones,
        replay::objcheck::RestMatch& match)
    {
        match = {};
        if (g_restBindLayout == 0)
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
            g_restBindLayout = replay::tpose::BindLayoutProven(
                [&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
                    return replay::mem::Matches(base + rva, bytes, size);
                }) ? 1 : -1;
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

    void NotePoseV2Rest(replay::objcheck::RestState rest)
    {
        if (rest == replay::objcheck::RestState::Away)
            ++g_worldSkeletonStats.restAway;
        else if (rest == replay::objcheck::RestState::AtRest)
            ++g_worldSkeletonStats.restAtRest;
        else
            ++g_worldSkeletonStats.restUnknown;
    }

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
            qualifies = candidate || (g_spawnedStaticScenery >= 2 && anyOwner);
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
                if (g_spawnedStaticScenery > 0 && !watch.sceneryDecided)
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
            if (!g_spawnedObjects)
            {
                // SpawnedObjects=0: the evidence only.
                char extra[96];
                std::snprintf(extra, sizeof(extra), ",moved=%d,animated=%d,age_ms=%u", moved ? 1 : 0,
                    animated ? 1 : 0, ageMs);
                logLine("would_record", object, watch.model, owner->handle, meters, bones, extra);
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
            if (g_spawnedStaticScenery > 0 && g_spawnWatch.count(object))
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
    struct VehicleChildStats
    {
        std::uint32_t recorded = 0;
        std::uint32_t onTrainCars = 0;
        std::uint32_t posed = 0;  // with a pose record (or waiting for a skeleton)
        std::uint32_t refusedFar = 0;
        std::uint32_t refusedFull = 0;
        std::uint32_t logs = 0;
    } g_vehicleChildStats;
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

    // Weapon puppets ride on their parent puppet as an entity attachment,
    // like the recorded weapon (attached to its ped): the engine updates a
    // weapon's straps only for an object attached to a ped (the strap
    // update reads the object's attach parent and requires a ped), so a
    // free object never draws them. The recorded parent-relative transform
    // is the attach offset, set every frame. The attach native takes Euler
    // degrees; the engine itself converts the rotation (a hidden helper
    // object gets the quaternion and returns its rotation in the same
    // order), so no Euler convention is assumed.
    constexpr int kPoseV2AttachRotationOrder = 2;
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

    // Straps: the recorded state at the playhead, and the update request
    // the owner ped's weapon manager would make. Every frame, for every
    // attach path.
    // The recorded weapon look at the playhead, set when the
    // state changes (both directions).
    // weapon_look_core.h: the whole look at the playhead, every frame for every
    // weapon puppet (attached, loose or waiting for its parent), set when the
    // object disagrees:
    // the state's components (missing given, extra removed: the engine list of
    // the puppet when the state carries the engine's), the rust and sixth wear
    // channel (rung 3, ini WeaponWearWrite), then degradation, damage, dirt and
    // soot (natives, the order of the game's own cleaning script). The puppet
    // is read back kCheckDelayMs later and then every kRecheckMs:
    // POSE_V2_WEAPON_LOOK_CHECK, and the wear set again when the engine moved
    // it (it adds soot when the replay fires the puppet weapon).
    struct WeaponLookVerdict
    {
        bool checked = false;
        bool components = false;
        bool wear = false;
    };
    std::map<std::uint32_t, WeaponLookVerdict> g_weaponLookVerdicts;
    bool g_weaponLookEnginePlay = true;
    bool g_weaponLookEnginePlayChecked = false;
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

    // The weapon records met with a vehicle or a train car as their parent (a
    // hay bale on a cart, a cabinet in a train car). Their engine attach frame
    // names a bone of the vehicle's skeleton, which a train car built as an
    // object does not have: they ride at the parent's root with the
    // parent-relative transform, which the recording samples every frame.
    std::unordered_set<std::uint32_t> g_vehicleChildRecords;
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
    struct VehicleWheelModelsRead
    {
        bool read = false;
        replay::vehicle_pieces::WheelModels models;
    };
    std::unordered_map<std::uint32_t, VehicleWheelModelsRead> g_vehicleWheelModels; // vehicle model hash
    std::unordered_map<std::uint32_t, std::uint16_t> g_wheelChildBits;              // child packed id
    std::unordered_set<std::uint32_t> g_wheelChildLogged;
    // How long a puppet has waited for something (its first try), restarted
    // when it was not asked for a while: a puppet deleted while it waited (a
    // seek) starts again with its full wait when it comes back.
    struct WaitClock
    {
        ULONGLONG since = 0;
        ULONGLONG last = 0;
    };
    constexpr ULONGLONG kWaitClockStaleMs = 250;
    ULONGLONG WaitedMs(std::unordered_map<std::uint32_t, WaitClock>& clocks, std::uint32_t key, ULONGLONG now)
    {
        WaitClock& clock = clocks[key];
        if (!clock.last || now - clock.last > kWaitClockStaleMs)
            clock.since = now;
        clock.last = now;
        return now - clock.since;
    }

    struct LeftWheel
    {
        std::uint32_t leftMs = 0;
        std::uint16_t bits = 0;
        std::uint32_t prop = 0; // the loose prop the wheel's object went on as (packed id)
        std::uint32_t child = 0; // the wheel child on the vehicle it was before (packed id)
    };
    std::unordered_map<std::uint32_t, std::vector<LeftWheel>> g_leftWheels; // vehicle packed id
    // (vehicle packed id, prop) -> the vehicle puppet whose wheel object the
    // prop's went on as (a rebuilt puppet gives one again).
    std::map<std::pair<std::uint32_t, std::uint32_t>, Entity> g_leftWheelsDetached;
    std::set<std::pair<Entity, std::uint32_t>> g_leftWheelMissLogged; // (vehicle puppet, prop): one wait line each
    std::unordered_map<Entity, ULONGLONG> g_wheelsPendingSince; // vehicle puppet -> first asked
    struct WheelCountSeen
    {
        std::int32_t count = -1;
        ULONGLONG since = 0;
    };
    std::unordered_map<Entity, WheelCountSeen> g_wheelCountSeen;   // vehicle puppet -> its count and since when
    std::unordered_map<std::uint32_t, std::int32_t> g_modelWheelCount; // vehicle model -> the most wheels a puppet made
    constexpr ULONGLONG kWheelCountSteadyMs = 100;
    std::unordered_set<Entity> g_wheelsMade;                     // vehicle puppets seen with a wheel object
    constexpr ULONGLONG kWheelsPendingMaxMs = 1000;
    // Fragment pieces broken off a wheel object of a vehicle (frag piece packed id
    // -> the vehicle and the piece's create time).
    struct FragPieceParent
    {
        replay::entity::ReplayId vehicle;
        std::uint32_t createMs = 0;
    };
    std::unordered_map<std::uint32_t, FragPieceParent> g_fragPieceParents;
    std::unordered_map<std::uint32_t, std::vector<std::uint16_t>> g_fragPieceGroupsOf; // prop packed id -> PoseV2FragPieceTakes
    struct WheelChildStats
    {
        std::uint32_t released = 0; // objects a wheel child had, given up
        std::uint32_t keptLogs = 0;
        std::uint32_t childLogs = 0;
    } g_wheelChildStats;

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
                const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
                g_modelInfoLayout = base && replay::vehicle_pieces::ModelInfoLayoutProven(
                    [&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
                        return replay::mem::Matches(base + rva, bytes, size);
                    }) ? 1 : -1;
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

    void AttachPoseV2Weapon(replay::playback::Puppet& puppet, Ped parent,
        const replay::pose::WorldTransform& relative, const replay::pose::WorldTransform& world,
        std::uint32_t timeMs)
    {
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
            for (const auto& entry : frameTrack->frames)
            {
                if (entry.first <= timeMs)
                    frame = &entry.second;
            }
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
    // The pose records of the animated world objects and of the objects on a
    // vehicle or train car, met by the playhead and armed on their puppet. A
    // record met but never armed by the end is an animated_unposed verdict
    // (phase=play): the drawer would play closed.
    struct AnimatedPlayTally
    {
        std::set<std::uint32_t> worldMet, worldArmed;
        std::set<std::uint32_t> vehicleMet, vehicleArmed;
    } g_animatedPlay;
    // Replay objects whose skeleton was asked for (clone << 32 |
    // record id), once each (object_pose.h ShouldAskSkeleton).
    // Asked again later when it is gone (ShouldAskSkeletonAgain):
    // how many asks, and the tick of the next one allowed.
    struct PoseV2SkeletonAsk
    {
        std::uint32_t asks = 0;
        std::uint64_t nextMs = 0;
    };
    std::map<std::uint64_t, PoseV2SkeletonAsk> g_poseV2ObjectSkeletonAsked;

    // still_pose_core.h the still rule (StillPoseEngine) and the drawn check
    // (ObjectDrawnCheck).
    bool g_stillPoseEngine = true;
    bool g_objectDrawnCheck = false;
    std::unordered_map<std::uint32_t, replay::tpose::TrackSignature> g_stillSignatures;
    std::uint32_t g_stillLines = 0;
    std::uint32_t g_drawnLines = 0;
    int g_drawnBindLayout = 0; // 0 not checked, 1 proven, -1 signature mismatch
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
            const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
            g_drawnBindLayout = replay::tpose::BindLayoutProven(
                [&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
                    return replay::mem::Matches(base + rva, bytes, size);
                }) ? 1 : -1;
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

    void PlayPoseV2ObjectPoseBody(replay::playback::Puppet& puppet, std::uint32_t timeMs);

    // A real weapon whose pose the clip has (WeaponBonePose), reached and armed
    // or not (WEAPON_BONES,phase=play; weapon_bones_static in the fx check).
    void CheckWeaponDrawn(const replay::playback::Puppet& puppet, std::uint32_t timeMs);

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
                // builds it from the model's skeleton data (CreateSkeleton,
                // 0xE34A18), on the replay object only.
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
                    g_stillPoseEngine, attached, signature.parsed, replay::tpose::NeverAnimated(signature));
                puppet.stillSkip = decision == replay::stillpose::Decision::SkipStill;
                if (decision != replay::stillpose::Decision::Arm && g_stillLines < 64)
                {
                    ++g_stillLines;
                    Ctx().log << "POSE_V2_OBJECT_STILL,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec
                              << ",decision=" << replay::stillpose::DecisionName(decision)
                              << ",rule=" << (g_stillPoseEngine ? 1 : 0) << ",attached=" << (attached ? 1 : 0)
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

    // Defined with the vehicle pieces.
    replay::vehicle_pieces::WheelCopy PoseV2WheelCopy(const replay::playback::Puppet& puppet,
        const replay::pose::WorldTransform& at, Object& out, ULONGLONG now, std::uint32_t timeMs, bool noWait = false);

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
                if (creates >= replay::playback::kMaxCreatesPerFrame || now < puppet.nextCreateAt ||
                    !replay::playback::StartBatchLeft()) // ClipStartStagger
                    continue;
                // A carried wheel is the wheel object a vehicle puppet gave,
                // not a fresh model.
                Object wheelObject = 0;
                if (PoseV2WheelCopy(puppet, world, wheelObject, now, timeMs) == replay::vehicle_pieces::WheelCopy::Wait)
                    continue;
                ++creates;
                replay::playback::StartBatchUse();
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
                AttachPoseV2Weapon(puppet, parent->clone, sample.relative, world, timeMs);
            }
            if (vehicleParent && parentReady && visible)
                HidePoseV2EngineCopy(puppet, parent->clone, copyChecks, now);
            ApplyPoseV2WeaponLook(puppet, timeMs);
            PlayPoseV2ObjectPose(puppet, timeMs);
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
    struct VehicleFragPieceStats
    {
        std::uint32_t early = 0;   // broken off at the group's break
        std::uint32_t adopted = 0; // taken by their piece
        std::uint32_t released = 0;
    } g_vehicleFragPieceStats;

    // The wheel objects the vehicle puppets gave (_BREAK_OFF_VEHICLE_WHEEL),
    // kept while the playback has a use for them: free (hidden) or the object
    // of a puppet (`holder`, its packed id). A wheel piece takes the wheel of
    // its own vehicle and index; a plain object puppet of a wheel model (the
    // carried wheel of 134: a held object, then a dropped prop) takes a free
    // one of its model instead of a fresh CREATE_OBJECT, which draws without
    // its vehicle (yellow); a puppet's wheel object is free again at the end of
    // its span (ReturnWheelObject). Free ones go when their vehicle puppet is
    // built again and at the playback's end.
    struct PoseV2WheelObject
    {
        Object object = 0;
        replay::entity::ReplayId vehicle;
        int wheel = -1;
        std::uint32_t model = 0;
        std::uint32_t holder = 0; // 0: free (hidden)
        bool pieced = false;      // fragment pieces broke off it (PoseV2FragPieceFromParent)
        std::uint32_t forProp = 0; // the loose wheel it came off for (LeftVehicleWheels), 0: any
    };
    std::vector<PoseV2WheelObject> g_wheelObjects;
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
    std::uint32_t g_wheelPlayheadMs = 0; // the last playhead the object puppets saw (log lines)

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

    // An object's handle by its address (defined with the fragment pieces).
    Object PoseV2HandleAt(std::uint64_t address);

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
    std::uint32_t g_fragPiecesShaped = 0;
    std::uint32_t g_fragPiecesFailed = 0;
    std::uint32_t g_fragPieceShapeLogs = 0;
    constexpr ULONGLONG kFragPieceWaitMs = 1000;

    const replay::FragPiece* FragPieceOf(replay::entity::ReplayId id)
    {
        for (const replay::FragPiece& row : Ctx().clip.fragPieces)
            if (row.id == id)
                return &row;
        return nullptr;
    }

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
    constexpr std::uint32_t kMaxPieceCreatesPerFrame = 24;
    constexpr ULONGLONG kFragParentWaitMs = 300;
    std::unordered_map<std::uint32_t, WaitClock> g_fragParentWaitSince;
    // The pieces that waited for their turn this tick (another object of the
    // model came first), tried again in the same tick once an object puppet
    // of the props was made (UpdatePoseV2Props): a chain of pieces the engine
    // broke in one frame comes off in one tick.
    std::unordered_set<std::uint32_t> g_fragOrderWaited;
    bool g_propMadeThisPass = false;
    const std::unordered_set<std::uint32_t>* g_propRetryOnly = nullptr;
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
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const std::uint64_t pieceAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(piece));
            for (std::uint16_t group : plan.ok ? plan.remove : std::vector<std::uint16_t>{})
            {
                if (GuardedGroupGone(pieceAddress, group) == 1)
                    continue;
                const int child = GuardedGroupToChild(pieceAddress, base, group);
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
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uint64_t pieceAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
        int removed = 0;
        int unmapped = 0;
        for (std::uint16_t group : plan.remove)
        {
            if (GuardedGroupGone(pieceAddress, group) == 1)
                continue;
            const int child = GuardedGroupToChild(pieceAddress, base, group);
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

    // hat_look.h: the hat dresser. A hat with a recorded look and no owner
    // puppet to drop it (a scenery hat, a moved world object, a dropped hat
    // whose owner is not built or did not drop it) is made the way the engine
    // made it: a helper ped wears the layer and the engine drops it
    // (KNOCK_OFF_PED_PROP); the dropped object carries the look and becomes the
    // stand-in or the prop's puppet. The helper (the player's model: the story
    // script puts any picked up hat's layer on the player with
    // _SET_META_PED_TAG, short_update.c EVENT_PLAYER_HAT_EQUIPPED) stands
    // kDresserDepthM under the hat, frozen, without collision. One hat at a
    // time; each client asks every tick until it gets its object or is told it
    // failed (then nothing is drawn).
    struct HatDresser
    {
        enum class Step : std::uint8_t
        {
            Helper,
            Assets,
            Wear,
            Drop,
        };
        Ped helper = 0;
        std::uint64_t client = 0; // 0: idle
        Step step = Step::Helper;
        std::uint64_t since = 0;  // the job's start (GetTickCount64)
        std::uint64_t stepAt = 0; // the step's start
        std::uint64_t woreAt = 0;
        Object hatBefore = 0;
        std::vector<int> requests; // metaped asset requests held for the job
        bool assetsLoaded = false;
        bool knockLate = false;
        std::unordered_set<std::uint64_t> failedClients;
        std::uint32_t jobs = 0;
        std::uint32_t made = 0;
        std::uint32_t failed = 0;
        std::uint32_t logs = 0;
    } g_hatDresser;
    // The hat models of the clip (hatlook::IsHatModel's learned list) and
    // whether a hat is rebuilt by its look (ini HatLook).
    std::vector<std::uint32_t> g_hatModels;
    std::uint32_t g_hatHiddenLogs = 0;
    constexpr std::uint32_t kHatDressLogCap = 48;

    std::uint64_t HatClientOfStand(std::size_t stand) { return (1ull << 32) | static_cast<std::uint32_t>(stand); }
    std::uint64_t HatClientOfProp(replay::entity::ReplayId id) { return (2ull << 32) | id.Packed(); }

    void ReleasePoseV2HatRequests()
    {
        for (const int request : g_hatDresser.requests)
            PED::_RELEASE_META_PED_ASSET_REQUEST(request);
        g_hatDresser.requests.clear();
    }

    // The helper is drawn like the owner puppets that drop
    // their hats (the knock of an undrawn ped is unproven) only while it
    // wears a hat for a job, out of sight under the ground; hidden otherwise.
    void PoseV2HatHelperDrawn(bool drawn)
    {
        const HatDresser& d = g_hatDresser;
        if (d.helper && replay::entities::Exists(d.helper))
            ENTITY::SET_ENTITY_VISIBLE(d.helper, drawn ? TRUE : FALSE);
    }

    void LogPoseV2HatDress(const char* status, std::uint64_t client, std::uint32_t model,
        const std::vector<replay::entity::MetaPedAsset>& look, const char* verify, Object hat, std::uint32_t timeMs)
    {
        if (g_hatDresser.logs >= kHatDressLogCap)
            return;
        ++g_hatDresser.logs;
        const HatDresser& d = g_hatDresser;
        char line[320];
        std::snprintf(line, sizeof(line),
            "POSE_V2_HAT_DRESS,status=%s,client=%s:%u,model=0x%x,drawable=0x%x,tags=%zu,assets_loaded=%d,knock_late=%d,"
            "verify=%s,hat=%d,hat_model=0x%x,job_ms=%llu,playhead_ms=%u\n",
            status, (client >> 32) == 1 ? "stand" : "prop", static_cast<std::uint32_t>(client & 0xFFFFFFFFu), model,
            look.empty() ? 0u : look.front().drawable, look.size(), d.assetsLoaded ? 1 : 0, d.knockLate ? 1 : 0, verify,
            hat, hat && replay::entities::Exists(hat) ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(hat)) : 0u,
            static_cast<unsigned long long>(GetTickCount64() - d.since), timeMs);
        Ctx().log << line;
    }

    // The job of `client` ends without a hat; the client is told so on its
    // next ask (and never asks again this playback).
    void FailPoseV2HatJob(const char* why, std::uint32_t model, const std::vector<replay::entity::MetaPedAsset>& look,
        std::uint32_t timeMs)
    {
        HatDresser& d = g_hatDresser;
        LogPoseV2HatDress(why, d.client, model, look, "none", 0, timeMs);
        ReleasePoseV2HatRequests();
        PoseV2HatHelperDrawn(false);
        d.failedClients.insert(d.client);
        d.client = 0;
        ++d.failed;
    }

    // The job's client stopped asking (its puppet deleted, the playhead
    // moved away): the dresser is freed, the client may ask again later (a
    // hat the helper dropped meanwhile is deleted).
    void AbandonPoseV2HatJob(std::uint32_t timeMs)
    {
        HatDresser& d = g_hatDresser;
        if (d.step == HatDresser::Step::Drop && d.helper && replay::entities::Exists(d.helper))
        {
            Object stray = PED::_GET_PED_LAST_DROPPED_HAT(d.helper);
            if (stray && stray != d.hatBefore && replay::entities::Exists(stray))
            {
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stray, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&stray);
            }
        }
        LogPoseV2HatDress("abandoned", d.client, 0, {}, "none", 0, timeMs);
        ReleasePoseV2HatRequests();
        PoseV2HatHelperDrawn(false);
        d.client = 0;
    }

    bool EnsurePoseV2HatHelper(const float at[3])
    {
        HatDresser& d = g_hatDresser;
        if (d.helper && replay::entities::Exists(d.helper))
            return true;
        d.helper = 0;
        const Hash model = ENTITY::GET_ENTITY_MODEL(PLAYER::PLAYER_PED_ID());
        if (!replay::pose::playback::RequestModel(model))
            return false;
        replay::pose::WorldTransform where{};
        where.position.x = at[0];
        where.position.y = at[1];
        where.position.z = at[2] - replay::hatlook::kDresserDepthM;
        d.helper = replay::pose::playback::CreatePuppet(model, false, where);
        if (!d.helper)
            return false;
        // Hidden (CreatePuppet) until it wears a hat: drawn only from the
        // wear to the drop (PoseV2HatHelperDrawn).
        return true;
    }


    // The object of the hat `client` asked for once it is made (0 while the
    // dresser works on it or on another hat); `failed` when it cannot be.
    Object DressPoseV2Hat(std::uint64_t client, std::uint32_t model, const std::vector<replay::entity::MetaPedAsset>& look,
        const float at[3], bool& failed, std::uint32_t timeMs)
    {
        failed = false;
        HatDresser& d = g_hatDresser;
        if (look.empty() || d.failedClients.count(client))
        {
            failed = true;
            return 0;
        }
        const std::uint64_t now = GetTickCount64();
        if (d.client && d.client != client)
        {
            if (now - d.since <= replay::hatlook::kJobTimeoutMs)
                return 0; // another hat first
            AbandonPoseV2HatJob(timeMs); // every step has its own limit: its client stopped asking
        }
        if (!d.client)
        {
            d.client = client;
            d.step = HatDresser::Step::Helper;
            d.since = d.stepAt = now;
            d.woreAt = 0;
            d.hatBefore = 0;
            d.assetsLoaded = false;
            d.knockLate = false;
            ReleasePoseV2HatRequests();
            ++d.jobs;
        }
        switch (d.step)
        {
        case HatDresser::Step::Helper:
            if (!EnsurePoseV2HatHelper(at))
            {
                if (now - d.stepAt > replay::hatlook::kHelperWaitMs)
                {
                    FailPoseV2HatJob("no_helper", model, look, timeMs);
                    failed = true;
                }
                return 0;
            }
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(d.helper, at[0], at[1], at[2] - replay::hatlook::kDresserDepthM, FALSE, FALSE, FALSE);
            // The layer's assets held for the job, as the story script holds
            // a picked up hat's (_0x3FCBB5FCFD968698, released by
            // _RELEASE_META_PED_ASSET_REQUEST).
            for (const replay::entity::MetaPedAsset& asset : look)
                d.requests.push_back(PED::_0x3FCBB5FCFD968698(asset.drawable, asset.albedo, asset.normal, asset.material, 0));
            d.step = HatDresser::Step::Assets;
            d.stepAt = now;
            return 0;
        case HatDresser::Step::Assets:
        {
            bool loaded = true;
            for (const int request : d.requests)
                loaded = loaded && PED::_HAS_META_PED_ASSET_LOADED(request);
            if (!loaded && now - d.stepAt < replay::hatlook::kAssetWaitMs)
                return 0;
            d.assetsLoaded = loaded;
            PoseV2HatHelperDrawn(true);
            for (const replay::entity::MetaPedAsset& asset : look)
                PED::_SET_META_PED_TAG(d.helper, asset.drawable, asset.albedo, asset.normal, asset.material, asset.palette,
                    asset.tint0, asset.tint1, asset.tint2);
            PED::_UPDATE_PED_VARIATION(d.helper, FALSE, TRUE, TRUE, TRUE, FALSE);
            d.woreAt = now;
            d.step = HatDresser::Step::Wear;
            d.stepAt = now;
            return 0;
        }
        case HatDresser::Step::Wear:
        {
            replay::entity::Appearance layers;
            layers.assets = look;
            const replay::entity::appearance::Match worn = replay::entity::appearance::Compare(d.helper, layers);
            const replay::hatlook::KnockStep knock =
                replay::hatlook::WhenToKnock(d.woreAt, now, worn.recorded && worn.present >= worn.recorded);
            if (knock == replay::hatlook::KnockStep::Wait)
                return 0;
            d.knockLate = knock == replay::hatlook::KnockStep::KnockLate;
            d.hatBefore = PED::_GET_PED_LAST_DROPPED_HAT(d.helper);
            PED::KNOCK_OFF_PED_PROP(d.helper, TRUE, TRUE, TRUE, TRUE);
            d.step = HatDresser::Step::Drop;
            d.stepAt = now;
            return 0;
        }
        case HatDresser::Step::Drop:
        {
            Object hat = PED::_GET_PED_LAST_DROPPED_HAT(d.helper);
            if (!hat || hat == d.hatBefore || !replay::entities::Exists(hat))
            {
                if (now - d.stepAt < replay::hatlook::kDropWaitMs)
                    return 0;
                FailPoseV2HatJob("no_drop", model, look, timeMs);
                failed = true;
                return 0;
            }
            // The dropped object read back: the helper's own hat (the layer
            // not taken) is refused, an object whose tags are not readable is
            // kept (logged).
            std::vector<replay::entity::MetaPedAsset> dropped;
            const char* status = ReadPoseV2HatLook(hat, dropped);
            const replay::hatlook::Verify verify =
                replay::hatlook::VerifyDropped(look, dropped, std::strcmp(status, "read") == 0);
            if (verify == replay::hatlook::Verify::Other)
            {
                LogPoseV2HatDress("other_look", d.client, model, look, replay::hatlook::VerifyText(verify), hat, timeMs);
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(hat, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&hat);
                FailPoseV2HatJob("refused", model, look, timeMs);
                failed = true;
                return 0;
            }
            LogPoseV2HatDress("made", d.client, model, look, replay::hatlook::VerifyText(verify), hat, timeMs);
            ReleasePoseV2HatRequests();
            PoseV2HatHelperDrawn(false);
            d.client = 0;
            ++d.made;
            return hat;
        }
        }
        return 0;
    }

    // Playback end (and a script restart: forget, no native).
    void EndPoseV2HatDresser(bool forget)
    {
        HatDresser& d = g_hatDresser;
        if (!forget)
        {
            if (d.client)
                AbandonPoseV2HatJob(0);
            ReleasePoseV2HatRequests();
            if (d.helper && replay::entities::Exists(d.helper))
            {
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(d.helper, TRUE, TRUE);
                PED::DELETE_PED(&d.helper);
            }
            if (d.jobs)
                Ctx().log << "POSE_V2_HAT_DRESS_SUMMARY,jobs=" << d.jobs << ",made=" << d.made << ",failed=" << d.failed
                          << ",hidden_bare=" << g_hatHiddenLogs << '\n';
        }
        d = HatDresser{};
        g_hatHiddenLogs = 0;
    }

    // How a prop or stand of `model` with `look` is built (and the evidence
    // line of a hidden bare hat, once per object).
    replay::hatlook::Rebuild PoseV2HatRebuild(std::uint32_t model, const std::vector<replay::entity::MetaPedAsset>& look)
    {
        return replay::hatlook::ChooseRebuild(g_hatLookEnabled, !look.empty(),
            replay::hatlook::IsHatModel(model, g_hatModels));
    }

    void LogPoseV2HatHidden(const char* path, const std::string& what, std::uint32_t model, std::uint32_t timeMs)
    {
        ++g_hatHiddenLogs;
        if (g_hatHiddenLogs > kHatDressLogCap)
            return;
        Ctx().log << "POSE_V2_HAT_DRESS,status=hidden_bare,path=" << path << ",what=" << what << ",model=0x" << std::hex << model
                  << std::dec << ",why=no_recorded_look,playhead_ms=" << timeMs << '\n';
    }

    void UpdatePoseV2Props(std::uint32_t timeMs, std::uint32_t& creates)
    {
        REPLAY_PROFILE_ZONE("play.props");
        g_wheelPlayheadMs = timeMs;
        const ULONGLONG now = GetTickCount64();
        if (!g_propRetryOnly)
        {
            g_fragOrderWaited.clear();
            g_propMadeThisPass = false;
        }
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Prop ||
                puppet.state == replay::playback::PuppetState::Failed ||
                (g_propRetryOnly && !g_propRetryOnly->count(puppet.packet.id.Packed())))
            {
                continue;
            }
            const replay::entity::RelativeTrack* track =
                Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed());
            replay::entity::TransformSample sample;
            const bool sampled = track && track->Sample(timeMs, sample, &Ctx().clip.cuts); // cut-aware
            if (puppet.state == replay::playback::PuppetState::Loading &&
                puppet.packet.propOrigin == replay::entity::PropOrigin::VehiclePiece)
            {
                if (!CreatePoseV2VehiclePiece(puppet, sampled, sample, timeMs, creates))
                    continue;
            }
            else if (puppet.state == replay::playback::PuppetState::Loading)
            {
                // A puppet built again (scrub back before a break, PROP_LOST)
                // is a whole object: it is made the piece again even when the
                // new handle repeats the old one (the shaped map compares
                // handles only).
                g_fragPieceShaped.erase(puppet.packet.id.Packed());
                // A prop of a vehicle's model that is no vehicle
                // piece (clips before it: a wagon board taken as a
                // projectile's successor) would be built as the whole vehicle.
                if (STREAMING::IS_MODEL_A_VEHICLE(puppet.packet.modelHash))
                {
                    puppet.state = replay::playback::PuppetState::Failed;
                    Ctx().hooks->LogPuppet(puppet, "vehicle_model_refused", timeMs);
                    continue;
                }
                const bool hat = puppet.packet.propOrigin == replay::entity::PropOrigin::DroppedHat;
                const bool gorePiece = puppet.packet.propOrigin == replay::entity::PropOrigin::GorePiece;
                const bool loaded = (puppet.packet.weaponHash
                    ? replay::pose::playback::RequestWeaponAsset(puppet.packet.weaponHash)
                    : replay::pose::playback::RequestModel(puppet.packet.modelHash)) &&
                    replay::pose::playback::RequestWeaponComponents(puppet.packet.weaponComponents);
                if (!sampled)
                {
                    // A gore piece lost to a gore clear before its span waits
                    // for the span (the reconcile drops a real piece again).
                    if (puppet.packet.propOrigin == replay::entity::PropOrigin::GorePiece)
                    {
                        puppet.stateSince = now;
                        continue;
                    }
                    if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        Ctx().hooks->LogPuppet(puppet, "no_track", timeMs);
                    }
                    continue;
                }
                replay::playback::Puppet* owner = Ctx().puppets.Find(puppet.packet.owner);
                const bool ownerReady = owner && owner->state == replay::playback::PuppetState::Ready &&
                    owner->clone && replay::entities::Exists(owner->clone);
                // The engine builds the prop the way it did in the game: the
                // owner puppet wears the prop's layers and drops it, and the
                // dropped object carries the look (the generic prop model
                // takes no metaped layers from script).
                if (hat && !puppet.knockRequested && ownerReady)
                {
                    replay::entity::Appearance layers;
                    const char* source = PropLayersOf(puppet.packet, layers);
                    const replay::entity::appearance::Match worn =
                        replay::entity::appearance::Compare(owner->clone, layers);
                    const bool wear = !layers.assets.empty() && worn.present < worn.recorded;
                    if (wear && !puppet.hatWoreAt)
                    {
                        for (const replay::entity::MetaPedAsset& asset : layers.assets)
                        {
                            PED::_SET_META_PED_TAG(owner->clone, asset.drawable, asset.albedo,
                                asset.normal, asset.material, asset.palette,
                                asset.tint0, asset.tint1, asset.tint2);
                        }
                        PED::_UPDATE_PED_VARIATION(owner->clone, FALSE, TRUE, TRUE, TRUE, FALSE);
                        // The owner's skeleton was rebuilt with the hat: its
                        // pose again before this frame renders.
                        Ctx().hooks->RefreshOverride(*owner, timeMs);
                        // hatlook::WhenToKnock the knock waits for the layer to
                        // land (a same-tick knock drops nothing: the plain
                        // model, an octagon).
                        if (g_hatLookEnabled)
                        {
                            puppet.hatWoreAt = now;
                            continue;
                        }
                    }
                    bool knockLate = false;
                    if (puppet.hatWoreAt)
                    {
                        const replay::hatlook::KnockStep step = replay::hatlook::WhenToKnock(puppet.hatWoreAt, now, !wear);
                        if (step == replay::hatlook::KnockStep::Wait)
                            continue;
                        knockLate = step == replay::hatlook::KnockStep::KnockLate;
                    }
                    puppet.hatBefore = PED::_GET_PED_LAST_DROPPED_HAT(owner->clone);
                    PED::KNOCK_OFF_PED_PROP(owner->clone, TRUE, TRUE, TRUE, TRUE);
                    Ctx().hooks->RefreshOverride(*owner, timeMs);
                    puppet.knockRequested = true;
                    puppet.knockDeadline = now + 1000;
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_PLAYBACK_PROP_KNOCK"
                        << ",id=" << replay::entity::IdText(puppet.packet.id)
                        << ",owner=" << replay::entity::IdText(puppet.packet.owner)
                        << ",source=" << source
                        << ",layers=" << layers.assets.size()
                        << ",drawable=0x" << std::hex
                        << (layers.assets.empty() ? 0u : layers.assets[0].drawable) << std::dec
                        << ",was_worn=" << (puppet.hatWoreAt || wear ? 0 : 1)
                        << ",wear_wait_ms=" << (puppet.hatWoreAt ? now - puppet.hatWoreAt : 0)
                        << ",knock_late=" << (knockLate ? 1 : 0)
                        << ",playhead_ms=" << timeMs
                        << '\n';
                    log.flush();
                    continue;
                }
                Object dropped = 0;
                // A clip recorded before took a map object's fragment (the
                // railing) for a gore piece: it is not built whole.
                if (gorePiece && !replay::objcheck::GorePiecePlayable(
                                     IsPedGoreModel(puppet.packet.modelHash), PoseV2ClipWorldModel(puppet.packet.modelHash)))
                {
                    ++g_goreWorldModelRefused;
                    puppet.state = replay::playback::PuppetState::Failed;
                    Ctx().hooks->LogPuppet(puppet, "gore_piece_world_model", timeMs);
                    continue;
                }
                if (gorePiece && sampled)
                {
                    // The gore event replayed on the owner puppet makes the
                    // engine drop its own piece (with the owner's look): the
                    // nearest free object of the recorded model near the
                    // recorded position. Waits briefly, then the bare model.
                    static int objects[1024];
                    const int count = (std::min)(ListObjects(objects, 1024), 1024);
                    // Never a stand-in or the original a stand hides (the stand
                    // hid it, the prop showed it: duplicate why=reshown).
                    static std::vector<Object> standObjects;
                    CollectPoseV2ReplayOwnObjects(standObjects);
                    // First only a piece at the recorded spot (the engine
                    // drops it there within a frame or two); later anything
                    // of the model close enough.
                    const bool strict = now - puppet.stateSince < 300;
                    float best = strict ? 0.3f : 2.5f;
                    for (int slot = 0; slot < count; ++slot)
                    {
                        const Object object = objects[slot];
                        if (!object || !replay::entities::Exists(object) ||
                            static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) != puppet.packet.modelHash ||
                            ENTITY::IS_ENTITY_ATTACHED(object) ||
                            !replay::objcheck::AdoptForGore(ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) != FALSE,
                                std::binary_search(standObjects.begin(), standObjects.end(), object), false))
                        {
                            continue;
                        }
                        bool taken = false;
                        for (const replay::playback::Puppet& other : Ctx().puppets.list)
                            if (other.clone == object)
                                taken = true;
                        if (taken)
                            continue;
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                        const float dx = at.x - sample.relative.position.x;
                        const float dy = at.y - sample.relative.position.y;
                        const float dz = at.z - sample.relative.position.z;
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (distance < best)
                        {
                            best = distance;
                            dropped = object;
                        }
                    }
                    if (!dropped && count == 0)
                    {
                        Vector3 at{};
                        at.x = sample.relative.position.x;
                        at.y = sample.relative.position.y;
                        at.z = sample.relative.position.z;
                        dropped = FindLooseObject(puppet.packet.modelHash, at, best, true);
                        if (dropped && (ENTITY::IS_ENTITY_A_MISSION_ENTITY(dropped) ||
                                           std::binary_search(standObjects.begin(), standObjects.end(), dropped)))
                            dropped = 0;
                    }
                    if (!dropped && now - puppet.stateSince < 600)
                        continue;
                    Ctx().log << "POSE_V2_PLAYBACK_GORE_PIECE,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",owner=" << replay::entity::IdText(puppet.packet.owner) << ",model=0x" << std::hex
                        << puppet.packet.modelHash << std::dec << ",adopted=" << dropped << ",distance="
                        << (dropped ? best : -1.0f) << ",objects=" << count << ",strict=" << strict << ",wait_ms=" << (now - puppet.stateSince)
                        << ",playhead_ms=" << timeMs << '\n';
                }
                else if (puppet.knockRequested)
                {
                    const Object hat = ownerReady ? PED::_GET_PED_LAST_DROPPED_HAT(owner->clone) : 0;
                    if (hat && hat != puppet.hatBefore && replay::entities::Exists(hat))
                        dropped = hat;
                    else if (now < puppet.knockDeadline)
                        continue;
                    // The drop changed (or tried to change) the owner's
                    // layers and skeleton: it takes its segment again next
                    // tick, and its pose now.
                    if (owner)
                    {
                        owner->recordId = 0;
                        if (ownerReady)
                            Ctx().hooks->RefreshOverride(*owner, timeMs);
                    }
                }
                else if (hat && now - puppet.stateSince < replay::playback::kModelTimeoutMs)
                {
                    continue; // waiting for the owner puppet
                }
                // hat_look.h: a hat object not dropped by its owner puppet (no
                // owner, the knock dropped nothing, or a hat recorded by
                // another path: a moved world object, a picked up hat) is made
                // by the hat dresser with its recorded look; a hat model
                // without a look is not drawn (the bare model is the grey
                // octagon).
                bool dressed = false;
                if (!dropped && !gorePiece && !puppet.packet.weaponHash && g_hatLookEnabled)
                {
                    replay::entity::Appearance look;
                    if (hat)
                        PropLayersOf(puppet.packet, look);
                    else
                        look.assets = puppet.packet.appearance.assets;
                    const replay::hatlook::Rebuild rebuild = PoseV2HatRebuild(puppet.packet.modelHash, look.assets);
                    if (rebuild == replay::hatlook::Rebuild::Dress)
                    {
                        bool dressFailed = false;
                        const float at[3] = {sample.relative.position.x, sample.relative.position.y, sample.relative.position.z};
                        dropped = DressPoseV2Hat(HatClientOfProp(puppet.packet.id), puppet.packet.modelHash, look.assets, at,
                            dressFailed, timeMs);
                        if (!dropped && !dressFailed)
                            continue;
                        if (!dropped)
                        {
                            puppet.state = replay::playback::PuppetState::Failed;
                            Ctx().hooks->LogPuppet(puppet, "hat_dress_failed", timeMs);
                            continue;
                        }
                        dressed = true;
                    }
                    else if (rebuild == replay::hatlook::Rebuild::Hide)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        LogPoseV2HatHidden("prop", replay::entity::IdText(puppet.packet.id), puppet.packet.modelHash, timeMs);
                        Ctx().hooks->LogPuppet(puppet, "hat_hidden_bare", timeMs);
                        continue;
                    }
                }
                if (dropped)
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(dropped, TRUE, TRUE);
                    ENTITY::SET_ENTITY_INVINCIBLE(dropped, TRUE);
                    ENTITY::SET_ENTITY_COLLISION(dropped, FALSE, FALSE);
                    ENTITY::FREEZE_ENTITY_POSITION(dropped, TRUE);
                    replay::pose::playback::PlaceClone(dropped, sample.relative);
                    SuppressPickupLook(dropped);
                    puppet.clone = dropped;
                }
                else
                {
                    // A dropped weapon is its weapon object again; a hat
                    // without its owner falls back to the bare prop model.
                    if (!loaded)
                    {
                        if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                        {
                            puppet.state = replay::playback::PuppetState::Failed;
                            Ctx().hooks->LogPuppet(puppet, "model_timeout", timeMs);
                        }
                        continue;
                    }
                    // A fragment piece comes off in the frame the engine broke it
                    // (an explosion breaks a wheel into a dozen at once): the
                    // per-frame stagger of the creates is wider for them.
                    const std::uint32_t createCap = FragPieceOf(puppet.packet.id) ? kMaxPieceCreatesPerFrame
                                                                                  : replay::playback::kMaxCreatesPerFrame;
                    if (creates >= createCap || now < puppet.nextCreateAt ||
                        !replay::playback::StartBatchLeft()) // ClipStartStagger
                        continue;
                    // A dropped wheel is the wheel object a vehicle puppet
                    // gave; a wheel's fragment piece breaks off its wheel.
                    Object wheelObject = 0;
                    const int fromParent = PoseV2FragPieceFromParent(puppet, sample.relative, now, timeMs, wheelObject);
                    if (fromParent < 0)
                        continue;
                    if (!fromParent && PoseV2WheelCopy(puppet, sample.relative, wheelObject, now, timeMs,
                                           FragPieceOf(puppet.packet.id) != nullptr) == replay::vehicle_pieces::WheelCopy::Wait)
                        continue;
                    ++creates;
                    replay::playback::StartBatchUse();
                    ++puppet.createAttempts;
                    puppet.clone = wheelObject ? wheelObject : replay::pose::playback::CreateWeaponPuppet(
                        puppet.packet.weaponHash, puppet.packet.modelHash, sample.relative);
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
                    // A loose weapon object would show the gameplay pickup
                    // light: the replay object is a prop.
                    SuppressPickupLook(puppet.clone);
                }
                puppet.state = replay::playback::PuppetState::Ready;
                g_propMadeThisPass = true;
                Ctx().hooks->LogPuppet(puppet,
                    gorePiece ? (dropped ? "adopted_gore_piece" : "created_gore_piece")
                    : dressed ? "created_dressed"
                    : dropped ? "created_knock_off" : hat ? "created_plain" : "created_weapon", timeMs);
            }
            if (!puppet.clone || !replay::entities::Exists(puppet.clone))
            {
                // An adopted engine object can be removed by the engine (a
                // gore piece pool): build it again, as the bare model when
                // nothing new of the model is around.
                if (puppet.state == replay::playback::PuppetState::Ready && puppet.clone)
                {
                    Ctx().log << "POSE_V2_PLAYBACK_PROP_LOST,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",handle=" << puppet.clone << ",playhead_ms=" << timeMs << '\n';
                    // A posed prop's override is disarmed before its handle is
                    // dropped, as DeletePoseV2Puppet does, so the detour never
                    // writes into the skeleton of an object the engine removed
                    // and the override slot is free for the new puppet.
                    Ctx().hooks->ReleaseOverride(puppet);
                    puppet.clone = 0;
                    puppet.state = replay::playback::PuppetState::Loading;
                    puppet.stateSince = now;
                }
                continue;
            }
            // A loose fragment piece is made from the whole object
            // first (hidden while it waits; never drawn whole).
            if (!ShapePoseV2FragPiece(puppet, sampled, sample, timeMs))
                continue;
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            const bool inTrack = track && track->Span(firstMs, lastMs) &&
                timeMs >= firstMs && timeMs <= lastMs + kIntervalMs;
            // A world object whose root group came off was destroyed (all of
            // it broke away; in a recorded test the small crate lost
            // groups 0 to 5): from then on it is not drawn whole.
            bool destroyed = false;
            if (puppet.packet.propOrigin != replay::entity::PropOrigin::VehiclePiece) // any prop
                for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
                    if (e.id == puppet.packet.id && e.group == 0 && e.timeMs <= timeMs)
                        destroyed = true;
            replay::pose::playback::SetCloneVisible(
                puppet.clone, sampled && inTrack && sample.visible && !destroyed);
            // An adopted engine object stays frozen (a fragment woken by a hit
            // runs its physics again).
            if (puppet.packet.propOrigin == replay::entity::PropOrigin::GorePiece && ENTITY::_IS_ENTITY_FROZEN(puppet.clone) == FALSE)
            {
                ENTITY::FREEZE_ENTITY_POSITION(puppet.clone, TRUE);
                ENTITY::SET_ENTITY_COLLISION(puppet.clone, FALSE, FALSE);
                ++g_goreRefrozen;
            }
            if (sampled)
                replay::pose::playback::PlaceClone(puppet.clone, sample.relative);
            if (puppet.packet.weaponHash) // a loose weapon keeps its look
                ApplyPoseV2WeaponLook(puppet, timeMs);
            // object_pose.h PoseableProp: a dropped object with no
            // weapon (the rolled pelt that left the hand) plays the pose its
            // prop id recorded; the others have no pose record.
            // And a free object spawned next to a recorded ped (Projectile
            // origin) plays its pose when it has one. And a world object
            // animated in place (the donation box's lid, the ledger) plays the
            // pose it recorded.
            if (replay::object_pose::PoseableProp(
                    puppet.packet.propOrigin == replay::entity::PropOrigin::DroppedWeapon,
                    puppet.packet.propOrigin == replay::entity::PropOrigin::Projectile ||
                        puppet.packet.propOrigin == replay::entity::PropOrigin::WorldObject,
                    puppet.packet.weaponHash))
                PlayPoseV2ObjectPose(puppet, timeMs);
        }
        // The pieces whose turn came in this tick, again (a few rounds: one
        // per step of the chain).
        for (int round = 0; !g_propRetryOnly && round < 6 && g_propMadeThisPass && !g_fragOrderWaited.empty(); ++round)
        {
            const std::unordered_set<std::uint32_t> retry = std::move(g_fragOrderWaited);
            g_fragOrderWaited.clear();
            g_propMadeThisPass = false;
            g_propRetryOnly = &retry;
            UpdatePoseV2Props(timeMs, creates);
            g_propRetryOnly = nullptr;
        }
    }

    }

    void Attach(Context& context)
    {
        g_context = &context;
        replay::objlook::Attach(context);
    }
    void SetSceneryHandoffTicks(std::uint32_t ticks) { g_sceneryHandoffTicks = ticks > 8 ? 8u : ticks; }
    std::uint32_t SceneryHandoffTicks() { return g_sceneryHandoffTicks; }
    void SetObjectBurnLevel(bool enabled) { g_objectBurnLevel = enabled; }
    bool ObjectBurnLevel() { return g_objectBurnLevel; }
    void SetStillPose(bool engineRule, bool drawnCheck)
    {
        g_stillPoseEngine = engineRule;
        g_objectDrawnCheck = drawnCheck;
    }
    bool StillPoseEngine() { return g_stillPoseEngine; }
    bool ObjectDrawnCheck() { return g_objectDrawnCheck; }

    void EnsureStrapVisibility() { EnsurePoseV2StrapVisibility(); }

    void WarmUp()
    {
        EnsurePoseV2StrapVisibility();
        WarmUpFrag();
    }
    bool StrapVisibilityReady() { return g_poseV2StrapVisibilityReady; }

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
        replay::pose::capture::SetExtraPlaces(((g_weaponBonePose || ObjectPoseShared()) ? g_weaponBonePlaces : 0u) + trainPlaces);
        {
            const std::uint32_t pedPlaces = replay::object_pose::PedPlaces(replay::recorder::kPedCapacity, 0);
            const std::uint32_t vehiclePlaces = replay::pose::capture::kMaxLiveTargets - replay::recorder::kPedCapacity;
            Ctx().log << "POSE_V2_CAPTURE_PLACES,peds=" << pedPlaces << ",vehicles=" << vehiclePlaces
                      << ",objects=" << ObjectPlacesLimit()
                      << ",objects_from=" << (ObjectPoseShared() ? "shared_with_weapons" : "peds_while_posed")
                      << ",peds_with_objects="
                      << replay::object_pose::PedPlaces(replay::recorder::kPedCapacity, ObjectPoseShared() ? 0u : g_objectPosePlaces)
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
        Ctx().log << "POSE_V2_HAT_LOOK,phase=record_end,enabled=" << (g_hatLookEnabled ? 1 : 0) << ",core=" << g_hatLookCore
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
                  << ",mode=" << (ObjectPoseShared() ? "shared" : "ped_class") << ",peak=" << g_objectPoseStats.peak
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

    void Scan() { ScanPoseV2Weapons(); }
    // object_pose.h
    void SetObjectPosePlaces(std::uint32_t places) { g_objectPosePlaces = replay::object_pose::ClampPlaces(places); }
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
    void SetHatLook(bool on) { g_hatLookEnabled = on; }
    // object_pose.h spawn
    void SetSpawnedObjects(bool enabled) { g_spawnedObjects = enabled; }
    bool SpawnedObjects() { return g_spawnedObjects; }
    void SetSpawnedStaticScenery(int mode) { g_spawnedStaticScenery = mode < 0 ? 0 : mode > 2 ? 2 : mode; }
    int SpawnedStaticScenery() { return g_spawnedStaticScenery; }
    std::uint32_t ObjectPoseReservedPlaces()
    {
        if (ObjectPoseShared())
            return 0; // the ped class keeps every place (object_pose.h ShouldPoseShared)
        return replay::object_pose::ReservedPlaces(g_objectPosePlaces, CountPosedObjects(), CountWaitingObjects());
    }
    void DiscardPendingEvents()
    {
        if (!g_fragReady)
            return;
        static std::vector<replay::frag::BreakCall> breaks;
        replay::frag::CollectSince(g_fragCursor, breaks);
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

    void Sample()
    {
        SamplePoseV2Weapons();
        ScanPoseV2HandAmmo();
    }
    void SetAmmoScanEveryTick(bool on) { g_ammoScanEveryTick = on; }
    void UpdateRecordedProps() { UpdatePoseV2Props(); }

    namespace
    {
        // Log evidence (484 recordings): every registered piece had model
        // 0x5595880b or 0x4f6fa8e4, whatever the ped model.
        std::set<std::uint32_t> g_goreModels{0x5595880bu, 0x4f6fa8e4u};

        bool OwnedByPuppet(Entity entity)
        {
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                if (puppet.clone == entity)
                    return true;
            return false;
        }
    }

    bool IsGorePieceModel(std::uint32_t model) { return g_goreModels.count(model) != 0; }

    namespace
    {
        bool g_poolListingDisabled = false;
        // The object pool walk of this frame (ListObjects).
        std::vector<int> g_poolObjects;
        int g_poolObjectsFrame = -1;
        replay::pools::ListStats g_poolObjectStats;
        replay::log::LogLimit g_poolObjectLogs; // per session (was never reset)
        constexpr float kPoolObjectRadius = 80.0f;          // new handles: the nearest objects
        constexpr float kPoolObjectExistingRadius = 200.0f; // objects that have a handle
        constexpr int kPoolObjectBudget = 96;               // new handles a frame
        constexpr int kPoolObjectCapacity = 4096;
    }
    // The object listing reads the object pool (entity_pools.h)
    // instead of worldGetAllObjects, which gives every object in the world a
    // script handle and returns 0 once that pool is near full (the town).
    // Handles for the objects near the player or the camera (new ones within
    // 80 m, nearest first, 96 a frame; those with a handle within 200 m);
    // one walk per frame, the callers ask several times. Every caller looks
    // near a recorded entity, a puppet or the camera. The ScriptHook listing
    // only when the reader is unavailable.
    int ListObjects(int* handles, int capacity)
    {
        if (replay::pools::Available())
        {
            const int frame = MISC::GET_FRAME_COUNT();
            if (frame != g_poolObjectsFrame)
            {
                REPLAY_PROFILE_ZONE("objects.pool_list");
                const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const replay::pools::Position centers[2] = {{player.x, player.y, player.z}, {camera.x, camera.y, camera.z}};
                g_poolObjects.resize(kPoolObjectCapacity);
                const int count = replay::pools::ListHandles(replay::pools::Kind::Object, centers, 2, kPoolObjectRadius,
                    kPoolObjectExistingRadius, kPoolObjectBudget, g_poolObjects.data(), kPoolObjectCapacity, &g_poolObjectStats);
                g_poolObjects.resize(static_cast<std::size_t>((std::max)(0, count)));
                g_poolObjectsFrame = count >= 0 ? frame : -1;
                if (count >= 0 && (g_poolObjectLogs.Seen() < 4 || frame % 1800 == 0) && g_poolObjectLogs.Take(16))
                {
                    Ctx().log << "OBJECT_POOL_LIST,live=" << g_poolObjectStats.live << ",handled=" << g_poolObjectStats.listed
                              << ",existing=" << g_poolObjectStats.existing << ",created=" << g_poolObjectStats.created
                              << ",far=" << g_poolObjectStats.skippedFar << ",budget=" << g_poolObjectStats.skippedBudget
                              << ",guid_free=" << g_poolObjectStats.guidFree << "/" << g_poolObjectStats.guidSize
                              << ",us=" << static_cast<int>(g_poolObjectStats.us) << '\n';
                }
            }
            if (g_poolObjectsFrame == frame)
            {
                const int count = (std::min)(capacity, static_cast<int>(g_poolObjects.size()));
                std::copy(g_poolObjects.begin(), g_poolObjects.begin() + count, handles);
                return count;
            }
        }
        return g_poolListingDisabled ? 0 : worldGetAllObjects(handles, capacity);
    }
    int ListAllObjectsGuarded(int* handles, int capacity, const char*& mode)
    {
        mode = "listing";
        if (replay::pools::Available())
        {
            const replay::pools::PoolState objects = replay::pools::State(replay::pools::Kind::Object);
            if (objects.ok && replay::pools::GuidRoom() < objects.used)
            {
                mode = "existing_only";
                const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                const replay::pools::Position center{player.x, player.y, player.z};
                const int count = replay::pools::ListHandles(replay::pools::Kind::Object, &center, 1, 0.0f, 1.0e9f, 0,
                    handles, capacity, nullptr);
                if (count >= 0)
                    return count;
                mode = "listing";
            }
            else if (objects.ok)
                mode = "all";
        }
        return (std::max)(0, worldGetAllObjects(handles, capacity));
    }
    int ListPickups(int* handles, int capacity) { return g_poolListingDisabled ? 0 : worldGetAllPickups(handles, capacity); }
    // The recorder's ped and vehicle candidates go through the
    // same switch, so the lake stage reproduces the town (listing 0).
    int ListPeds(int* handles, int capacity) { return g_poolListingDisabled ? 0 : worldGetAllPeds(handles, capacity); }
    int ListVehicles(int* handles, int capacity) { return g_poolListingDisabled ? 0 : worldGetAllVehicles(handles, capacity); }
    void SetPoolListingDisabled(bool disabled) { g_poolListingDisabled = disabled; }

    Object FindLooseObject(std::uint32_t model, const Vector3& at, float radius, bool excludePuppets)
    {
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        Object best = 0;
        float bestDistance = radius;
        const auto consider = [&](Object object) {
            if (!object || !replay::entities::Exists(object) ||
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) != model ||
                ENTITY::IS_ENTITY_ATTACHED(object) || (excludePuppets && OwnedByPuppet(object)))
                return;
            const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = object;
            }
        };
        if (count > 0)
        {
            for (int slot = 0; slot < count; ++slot)
                consider(objects[slot]);
            return best;
        }
        consider(OBJECT::GET_CLOSEST_OBJECT_OF_TYPE(at.x, at.y, at.z, radius, model, FALSE, FALSE, FALSE));
        return best;
    }

    int DeleteStrayGorePieces(Entity around, float radius)
    {
        if (!around || !replay::entities::Exists(around))
            return 0;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(around, TRUE, FALSE);
        int deleted = 0;
        for (std::uint32_t model : g_goreModels)
        {
            // One at a time: closest-of-type returns the same object until it goes.
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                Object piece = FindLooseObject(model, at, radius, true);
                if (!piece || ENTITY::IS_ENTITY_A_MISSION_ENTITY(piece))
                    break;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&piece);
                ++deleted;
            }
        }
        return deleted;
    }

    void SuppressPickupLook(Object object)
    {
        if (!object || !replay::entities::Exists(object))
            return;
        GRAPHICS::SET_PICKUP_LIGHT(object, FALSE);
        GRAPHICS::_BLOCK_PICKUP_OBJECT_LIGHT(object, TRUE);
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
    void SearchDrops()
    {
        SearchPoseV2WeaponDrops();
        SearchPoseV2Projectiles();
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

    void RetireWeaponsOf(Entity parent, const char* reason)
    {
        std::vector<replay::recorder::RecordedWeapon>& weapons = Ctx().recorder.weapons;
        for (std::size_t weapon = weapons.size(); weapon-- > 0;)
        {
            if (weapons[weapon].parent == parent)
                RetirePoseV2Weapon(weapon, reason);
        }
    }

    std::map<std::uint32_t, std::size_t> g_fragApplied; // prop packed id -> events applied
    std::map<std::uint32_t, Object> g_fragPuppet;       // prop packed id -> puppet it was applied to
    std::uint32_t g_fragBreaksPlayed = 0;
    std::uint32_t g_fragFixes = 0;
    std::uint32_t g_fragUnmapped = 0;
    int g_fragLastError = 0;
    std::uint32_t g_fragMaxPlayheadMs = 0; // how far the playback went

    // World objects in playback: a stand-in of the model where
    // the object stood at the start, shown while its prop is not alive (before
    // it moved, and after a scrub back); the original, when this session still
    // has it, is hidden for the playback and given back at the end.
    // object_check.h: the same stand-in for the scenery
    // (the objects of the scene that are not of the map, alive from appearMs
    // to goneMs), and the original's hide is deferred: the closed theater's
    // clear turns the map objects into dummies that stream back after the
    // playback started, and a camp's own props are its script's mission
    // entities; both would be left drawn next to the replay's copy. The search
    // excludes only the replay's own objects (stand-ins and puppets), every
    // 500 ms for the first 10 s of the playback and every 3 s after it, by one
    // walk of the object list.
    struct ObjectStand
    {
        std::uint32_t model = 0;
        float position[3]{};
        float rotation[3]{};
        std::uint32_t appearMs = 0;
        std::uint32_t goneMs = replay::objcheck::kNeverGone;
        replay::entity::ReplayId prop; // the prop that takes over (invalid: none)
        bool scenery = false;
        // objcheck::VisibleAt: the recorded visibility
        // (a mission's hidden object is not drawn).
        bool visibleWhenRecorded = true;
        std::vector<std::uint32_t> visibleFlipsMs;
        bool loot = false; // objcheck::IsLootEntry: logged when it shows and hides
        // hat_look.h: a hat's recorded layer (its prop's create for a world
        // object, SCHT for scenery), how it is built, and a hat model without a
        // look that is not drawn.
        std::vector<replay::entity::MetaPedAsset> hatLook;
        replay::hatlook::Rebuild hatRebuild = replay::hatlook::Rebuild::Model;
        bool hatHidden = false;
        Object standIn = 0;
        Object original = 0;
        bool originalWasVisible = true;
        bool shown = false;
        bool failed = false;
        bool missingLogged = false;
        std::uint64_t loadingSince = 0;
        // objcheck::ChooseChildHost: a scenery child (parentStand >= 0) is
        // attached to the stand at that index: its stand-in, or the prop that
        // took it over (the frame, parent bone and offsets, only on a posed
        // prop; else the transform in the parent's frame at the root). `host`
        // is what it is attached to now.
        int parentStand = -1;
        bool hasFrame = false;
        std::int16_t frameBone = -1;
        float framePosition[3]{};
        float frameRotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float relativePosition[3]{};
        float relativeRotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        Object host = 0;
        bool hostByFrame = false;
        // Set every update for the children: the ready puppet of `prop`, and
        // whether it took over and plays a pose.
        Object propClone = 0;
        bool propTookOver = false;
        bool propPosed = false;
        // The scenery entry's burn level track (index into
        // Recording::sceneryBurnLevels, -1 none).
        int burnTrack = -1;
        // The scenery entry's look track (index
        // into Recording::sceneryObjectLooks, -1 none).
        int lookTrack = -1;
        // objcheck::HandoffPropReady: the consecutive ticks the prop puppet has
        // been Ready and posed (the stand-in is hidden a tick after the prop's
        // first published pose), whether the prop has a pose track (-1 not
        // looked up yet), and whether this handoff was logged.
        std::uint32_t propReadyTicks = 0;
        std::uint32_t propPosedTicks = 0;
        std::int8_t propHasPose = -1;
        bool handoffLogged = false;
        // objcheck::MapDressingVerdict a map dressing entry, the IPL that held
        // it, the verdict (built only when the map does not have it) and the
        // live original left to the map.
        bool mapDressing = false;
        std::uint32_t mapIpl = 0;
        replay::objcheck::MapDressing mapVerdict = replay::objcheck::MapDressing::Wait;
        Object mapOriginal = 0;
    };
    std::vector<ObjectStand> g_objectStands;
    std::unordered_set<Object> g_objectStandObjects; // the stand-ins, for the searches
    std::uint32_t g_lootStandLogs = 0;               // POSE_V2_PLAYBACK_LOOT lines this playback
    std::uint32_t g_lateStandLogs = 0;               // POSE_V2_PLAYBACK_SCENERY_LATE lines this playback
    std::uint64_t g_objectStandsBegan = 0;
    std::uint64_t g_objectStandsNextSearch = 0;
    struct ObjectStandStats
    {
        std::uint32_t hidden = 0;
        std::uint32_t hiddenLate = 0; // found after the start wait: drawn twice until then
        std::uint32_t reshown = 0;    // made visible again by someone else, hidden again
        std::uint32_t created = 0;
        std::uint32_t failed = 0;
        std::uint32_t searches = 0;
        std::uint32_t childAttaches = 0;  // attaches of a child to a host
        std::uint32_t childOnProp = 0;    // of them on the prop that took the parent over
        std::uint32_t childByFrame = 0;   // of them by the engine frame
        std::uint32_t childNoEuler = 0;   // the rotation not converted yet (helper model)
        std::uint32_t burnWrites = 0;     // burn levels written on stand-ins
        std::uint32_t burnLogs = 0;
        std::uint32_t handoffs = 0;            // stand-ins hidden for a ready prop
        std::uint32_t handoffOverlapTicks = 0; // the ticks both were shown (the overlap kept)
        std::uint32_t handoffLogs = 0;
        // The map dressing verdicts of this playback.
        std::uint32_t mapDressing = 0;
        std::uint32_t mapLive = 0;
        std::uint32_t mapBuildIplOff = 0;
        std::uint32_t mapBuildMissing = 0; // IPL on, no original within kMapDressingWaitMs
        std::uint32_t mapBuildChanged = 0; // its visibility flipped while recorded
        std::uint32_t mapTaken = 0; // built: a prop took it over
        std::uint32_t mapDecidedWithoutWorld = 0; // the map state never looked at (the fallback wait)
        std::uint32_t mapLogs = 0;
        std::set<std::uint32_t> mapIplsOff;
    } g_objectStandStats;
    // The map dressing decides once the clip's map state was looked at
    // (replay::world::PlaybackApplied), or after this long without it.
    constexpr std::uint64_t kMapDressingWorldFallbackMs = 15000;
    constexpr std::uint64_t kStandFastSearchMs = 10000;
    constexpr std::uint64_t kStandSlowSearchMs = 3000;
    constexpr std::uint64_t kStandLateMs = 3000;

    void DeletePoseV2Stand(ObjectStand& stand)
    {
        if (stand.standIn && replay::entities::Exists(stand.standIn))
        {
            Object object = stand.standIn;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            ENTITY::DELETE_ENTITY(&object);
        }
        g_objectStandObjects.erase(stand.standIn);
        stand.standIn = 0;
    }

    void HidePoseV2Original(ObjectStand& stand, Object original, const char* how)
    {
        stand.original = original;
        stand.originalWasVisible = ENTITY::IS_ENTITY_VISIBLE(original) != FALSE;
        ENTITY::SET_ENTITY_VISIBLE(original, FALSE);
        ENTITY::SET_ENTITY_COLLISION(original, FALSE, FALSE);
        ++g_objectStandStats.hidden;
        const std::uint64_t since = GetTickCount64() - g_objectStandsBegan;
        if (since > kStandLateMs && stand.originalWasVisible)
        {
            // The original was drawn until now next to the replay's copy.
            ++g_objectStandStats.hiddenLate;
            char extra[96];
            std::snprintf(extra, sizeof(extra), ",phase=play,why=hidden_late,how=%s,after_ms=%llu", how,
                static_cast<unsigned long long>(since));
            LogObjectCheck(replay::objcheck::Verdict::Duplicate, stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "scenery",
                stand.model, 0, extra);
        }
    }

    void EndPoseV2WorldStandIns()
    {
        for (ObjectStand& stand : g_objectStands)
        {
            DeletePoseV2Stand(stand);
            if (stand.original && replay::entities::Exists(stand.original))
            {
                ENTITY::SET_ENTITY_VISIBLE(stand.original, stand.originalWasVisible ? TRUE : FALSE);
                ENTITY::SET_ENTITY_COLLISION(stand.original, TRUE, TRUE);
            }
        }
        if (!g_objectStands.empty())
        {
            std::uint32_t world = 0, never = 0, children = 0;
            for (const ObjectStand& stand : g_objectStands)
            {
                world += stand.scenery ? 0u : 1u;
                never += stand.original ? 0u : 1u;
                children += stand.parentStand >= 0 ? 1u : 0u;
            }
            Ctx().log << "POSE_V2_PLAYBACK_WORLD_OBJECTS,end=" << world << ",scenery=" << (g_objectStands.size() - world - children)
                      << ",children=" << children << ",child_attaches=" << g_objectStandStats.childAttaches
                      << ",child_on_prop=" << g_objectStandStats.childOnProp << ",child_by_frame=" << g_objectStandStats.childByFrame
                      << ",child_no_euler=" << g_objectStandStats.childNoEuler
                      << ",originals_hidden=" << g_objectStandStats.hidden << ",hidden_late=" << g_objectStandStats.hiddenLate
                      << ",reshown=" << g_objectStandStats.reshown << ",no_original=" << never
                      << ",stand_ins=" << g_objectStandStats.created << ",failed=" << g_objectStandStats.failed
                      << ",searches=" << g_objectStandStats.searches
                      << ",burn_tracks=" << Ctx().clip.sceneryBurnLevels.size() << ",burn_writes=" << g_objectStandStats.burnWrites
                      << ",handoff_ticks=" << g_sceneryHandoffTicks << ",handoffs=" << g_objectStandStats.handoffs
                      << ",handoff_overlap_ticks=" << g_objectStandStats.handoffOverlapTicks
                      << '\n';
            // The map dressing's verdicts.
            std::uint32_t dressing = 0, waiting = 0;
            for (const ObjectStand& stand : g_objectStands)
            {
                dressing += stand.mapDressing ? 1u : 0u;
                waiting += stand.mapDressing && stand.mapVerdict == replay::objcheck::MapDressing::Wait ? 1u : 0u;
            }
            if (dressing)
            {
                Ctx().log << "POSE_V2_MAP_DRESSING,phase=play_end,entries=" << dressing << ",live=" << g_objectStandStats.mapLive
                          << ",built_ipl_off=" << g_objectStandStats.mapBuildIplOff
                          << ",built_missing=" << g_objectStandStats.mapBuildMissing
                          << ",built_changed=" << g_objectStandStats.mapBuildChanged << ",built_taken=" << g_objectStandStats.mapTaken
                          << ",undecided=" << waiting << ",decided_without_world=" << g_objectStandStats.mapDecidedWithoutWorld
                          << ",ipls_off=";
                for (const std::uint32_t ipl : g_objectStandStats.mapIplsOff)
                    Ctx().log << "0x" << std::hex << ipl << std::dec << '|';
                Ctx().log << '\n';
            }
        }
        g_objectStands.clear();
        g_objectStandObjects.clear();
    }

    // See replay_objects.h.
    void CollectPoseV2ReplayOwnObjects(std::vector<Object>& out)
    {
        out.clear();
        if (g_poseV2AttachHelper)
            out.push_back(g_poseV2AttachHelper);
        for (const ObjectStand& stand : g_objectStands)
        {
            if (stand.standIn)
                out.push_back(stand.standIn);
            if (stand.original)
                out.push_back(stand.original);
        }
        for (const PoseV2WheelObject& wheel : g_wheelObjects)
            out.push_back(wheel.object);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void BeginPoseV2WorldStandIns()
    {
        EndPoseV2WorldStandIns();
        g_objectStandStats = {};
        g_lootStandLogs = 0;
        g_lateStandLogs = 0;
        g_objectStandsBegan = GetTickCount64();
        g_objectStandsNextSearch = 0;
        int hidden = 0;
        int invisible = 0;
        int loot = 0;
        int mapDressing = 0;
        for (const replay::WorldObjectOrigin& origin : Ctx().clip.worldObjects)
        {
            ObjectStand stand;
            stand.model = origin.model;
            for (int axis = 0; axis < 3; ++axis)
            {
                stand.position[axis] = origin.position[axis];
                stand.rotation[axis] = origin.rotation[axis];
            }
            stand.prop = origin.id;
            // The same session: the object recorded, wherever it is now.
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(origin.id.Packed());
            if (create && !create->weaponHash)
                stand.hatLook = create->appearance.assets; // the prop's recorded hat look
            const Object source = create && !Ctx().clip.fromDisk ? static_cast<Object>(create->sourceHandle) : 0;
            g_objectStands.push_back(stand);
            if (source && replay::entities::Exists(source) && !OwnedByPuppet(source) &&
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(source)) == origin.model)
            {
                HidePoseV2Original(g_objectStands.back(), source, "session_handle");
                ++hidden;
            }
        }
        for (const replay::SceneryObject& object : Ctx().clip.scenery)
        {
            ObjectStand stand;
            stand.model = object.model;
            for (int axis = 0; axis < 3; ++axis)
            {
                stand.position[axis] = object.position[axis];
                stand.rotation[axis] = object.rotation[axis];
            }
            stand.appearMs = object.appearMs;
            stand.goneMs = object.goneMs;
            stand.prop = object.prop;
            stand.scenery = true;
            stand.visibleWhenRecorded = object.visible != 0;
            stand.visibleFlipsMs = object.visibleFlipsMs;
            if (!stand.visibleWhenRecorded || !stand.visibleFlipsMs.empty())
                ++invisible;
            // A map dressing entry waits for its verdict
            // (UpdatePoseV2WorldStandIns) and is no loot.
            stand.mapDressing = object.rule == static_cast<std::uint8_t>(replay::objcheck::SceneryRule::MapDressing);
            stand.mapIpl = object.ipl;
            mapDressing += stand.mapDressing ? 1 : 0;
            // The loot recorded at its take (objcheck::IsLootEntry);
            // its stand-in and the original's hide are the scenery's.
            stand.loot = !stand.mapDressing && replay::objcheck::IsLootEntry(object.mission != 0, object.owner);
            loot += stand.loot ? 1 : 0;
            // Its burn level track (the index is the entry's).
            const std::size_t entry = static_cast<std::size_t>(&object - Ctx().clip.scenery.data());
            for (std::size_t n = 0; n < Ctx().clip.sceneryBurnLevels.size(); ++n)
                if (Ctx().clip.sceneryBurnLevels[n].id == entry && !Ctx().clip.sceneryBurnLevels[n].states.empty())
                    stand.burnTrack = static_cast<int>(n);
            stand.lookTrack = replay::objlook::SceneryTrackOf(static_cast<std::uint32_t>(entry));
            stand.hatLook = object.hatLook;
            g_objectStands.push_back(stand);
        }
        // The scenery's children, after the scenery stands (a scenery entry n
        // is the stand worldObjects + n).
        const std::size_t sceneryBase = Ctx().clip.worldObjects.size();
        std::uint32_t children = 0;
        for (const replay::SceneryChild& child : Ctx().clip.sceneryChildren)
        {
            if (child.parent >= Ctx().clip.scenery.size())
                continue;
            ObjectStand stand;
            stand.model = child.model;
            for (int axis = 0; axis < 3; ++axis)
            {
                stand.position[axis] = child.position[axis];
                stand.rotation[axis] = child.rotation[axis];
                stand.framePosition[axis] = child.framePosition[axis];
                stand.relativePosition[axis] = child.relativePosition[axis];
            }
            for (int axis = 0; axis < 4; ++axis)
            {
                stand.frameRotation[axis] = child.frameRotation[axis];
                stand.relativeRotation[axis] = child.relativeRotation[axis];
            }
            stand.appearMs = child.appearMs;
            stand.goneMs = child.goneMs;
            stand.scenery = true;
            stand.visibleWhenRecorded = child.visible != 0;
            stand.visibleFlipsMs = child.visibleFlipsMs;
            stand.parentStand = static_cast<int>(sceneryBase + child.parent);
            stand.hasFrame = child.hasFrame != 0;
            stand.frameBone = child.frameBone;
            g_objectStands.push_back(stand);
            ++children;
        }
        // hat_look.h: how each hat is built.
        std::uint32_t hatsDressed = 0;
        std::uint32_t hatsHidden = 0;
        for (std::size_t index = 0; index < g_objectStands.size(); ++index)
        {
            ObjectStand& stand = g_objectStands[index];
            if (stand.parentStand >= 0)
                continue;
            stand.hatRebuild = PoseV2HatRebuild(stand.model, stand.hatLook);
            hatsDressed += stand.hatRebuild == replay::hatlook::Rebuild::Dress ? 1u : 0u;
            if (stand.hatRebuild == replay::hatlook::Rebuild::Hide)
            {
                stand.hatHidden = true;
                ++hatsHidden;
                LogPoseV2HatHidden(stand.scenery ? "scenery" : "world_object",
                    stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "stand=" + std::to_string(index), stand.model, 0);
            }
        }
        if (hatsDressed || hatsHidden)
            Ctx().log << "POSE_V2_HAT_LOOK,phase=play_begin,stands_dressed=" << hatsDressed << ",stands_hidden_bare=" << hatsHidden
                      << ",hat_models=" << g_hatModels.size() << '\n';
        if (!g_objectStands.empty())
            Ctx().log << "POSE_V2_PLAYBACK_WORLD_OBJECTS,begin=" << Ctx().clip.worldObjects.size()
                      << ",scenery=" << Ctx().clip.scenery.size() << ",children=" << children << ",originals_hidden=" << hidden
                      << ",scenery_hidden_or_flipping=" << invisible << ",loot=" << loot
                      << ",map_dressing=" << mapDressing << '\n';
    }

    // One walk of the object list for every stand without its original.
    void SearchPoseV2Originals()
    {
        const std::uint64_t now = GetTickCount64();
        if (now < g_objectStandsNextSearch)
            return;
        g_objectStandsNextSearch = now + (now - g_objectStandsBegan < kStandFastSearchMs ? 500 : kStandSlowSearchMs);
        std::unordered_set<std::uint32_t> models;
        for (const ObjectStand& stand : g_objectStands)
        {
            // Map dressing left to the map or to a prop is never looked for;
            // one waiting looks for its original only until it is found (kept
            // in mapOriginal, not hidden).
            if (stand.mapDressing && stand.mapVerdict != replay::objcheck::MapDressing::Build)
            {
                if (stand.mapVerdict == replay::objcheck::MapDressing::Wait &&
                    (!stand.mapOriginal || !replay::entities::Exists(stand.mapOriginal)))
                    models.insert(stand.model);
                continue;
            }
            if (!stand.original || !replay::entities::Exists(stand.original))
                models.insert(stand.model);
            else if (ENTITY::IS_ENTITY_VISIBLE(stand.original))
            {
                // Someone drew it again (a script, the streaming): hidden again.
                ++g_objectStandStats.reshown;
                ENTITY::SET_ENTITY_VISIBLE(stand.original, FALSE);
                LogObjectCheck(replay::objcheck::Verdict::Duplicate,
                    stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "scenery", stand.model, 0, ",phase=play,why=reshown");
            }
        }
        if (models.empty())
            return;
        ++g_objectStandStats.searches;
        std::unordered_set<Object> puppets;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone)
                puppets.insert(puppet.clone);
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        std::unordered_set<Object> taken;
        for (const ObjectStand& stand : g_objectStands)
        {
            if (stand.original)
                taken.insert(stand.original);
            if (stand.mapOriginal)
                taken.insert(stand.mapOriginal);
        }
        // objcheck::NearestOriginals: the closest object of the model wins, not
        // the first listed (a shelf's row of one model lost a neighbor next to
        // the taken item's stand-in).
        static std::vector<Object> candidates;
        static std::vector<replay::objcheck::OriginalPair> pairs;
        candidates.clear();
        pairs.clear();
        for (int slot = 0; slot < count; ++slot)
        {
            const Object object = objects[slot];
            if (!object || g_objectStandObjects.count(object) || puppets.count(object) || taken.count(object))
                continue;
            if (!replay::entities::Exists(object))
                continue;
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            if (!models.count(model))
                continue;
            float at[3]{};
            if (!replay::entities::Position(object, at))
                continue;
            for (std::size_t index = 0; index < g_objectStands.size(); ++index)
            {
                const ObjectStand& stand = g_objectStands[index];
                if (stand.model != model || (stand.original && replay::entities::Exists(stand.original)) ||
                    !replay::objcheck::Near(at, stand.position, replay::objcheck::kOriginalRadiusM))
                    continue;
                if (stand.mapDressing && (stand.mapVerdict == replay::objcheck::MapDressing::Live ||
                                             (stand.mapOriginal && replay::entities::Exists(stand.mapOriginal))))
                    continue;
                const float dx = at[0] - stand.position[0];
                const float dy = at[1] - stand.position[1];
                const float dz = at[2] - stand.position[2];
                pairs.push_back({index, candidates.size(), dx * dx + dy * dy + dz * dz});
            }
            candidates.push_back(object);
        }
        for (const auto& [index, candidate] : replay::objcheck::NearestOriginals(pairs))
        {
            ObjectStand& stand = g_objectStands[index];
            // A waiting map dressing stand only notes the map's original (the
            // verdict leaves it drawn).
            if (stand.mapDressing && stand.mapVerdict == replay::objcheck::MapDressing::Wait)
                stand.mapOriginal = candidates[candidate];
            else
                HidePoseV2Original(stand, candidates[candidate], "search");
        }
    }

    // objcheck::MapDressingVerdict a waiting map dressing stand's verdict, once
    // the clip's map state was looked at. False while it is not to be built
    // (wait, live).
    bool DecidePoseV2MapDressing(ObjectStand& stand, std::uint32_t timeMs, std::uint64_t now)
    {
        using replay::objcheck::MapDressing;
        if (!stand.mapDressing || stand.mapVerdict == MapDressing::Build)
            return true;
        if (stand.mapVerdict != MapDressing::Wait)
            return false;
        const std::uint64_t waited = now - g_objectStandsBegan;
        const bool worldLooked = replay::world::PlaybackApplied();
        if (!worldLooked && waited < kMapDressingWorldFallbackMs)
            return false;
        const bool taken = stand.prop.Valid();
        const bool changed = !stand.visibleWhenRecorded || !stand.visibleFlipsMs.empty() ||
            stand.goneMs != replay::objcheck::kNeverGone;
        // An entry of the always-on map (no IPL) counts as on: built only when missing.
        const bool iplActive = !stand.mapIpl || STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(stand.mapIpl)) != FALSE;
        const bool found = stand.mapOriginal && replay::entities::Exists(stand.mapOriginal);
        stand.mapVerdict = replay::objcheck::MapDressingVerdict(taken, changed, iplActive, found, waited);
        if (stand.mapVerdict == MapDressing::Wait)
            return false;
        g_objectStandStats.mapDecidedWithoutWorld += worldLooked ? 0u : 1u;
        const char* why = "";
        switch (stand.mapVerdict)
        {
        case MapDressing::Live: ++g_objectStandStats.mapLive; why = "original_live"; break;
        default:
            if (taken)
            {
                ++g_objectStandStats.mapTaken;
                why = "taken";
            }
            else if (changed)
            {
                ++g_objectStandStats.mapBuildChanged;
                why = "visibility_changed";
            }
            else if (!iplActive)
            {
                ++g_objectStandStats.mapBuildIplOff;
                g_objectStandStats.mapIplsOff.insert(stand.mapIpl);
                why = "ipl_off";
            }
            else
            {
                ++g_objectStandStats.mapBuildMissing;
                why = "original_missing";
            }
            break;
        }
        if (stand.mapVerdict == MapDressing::Live)
            stand.mapOriginal = 0; // the map's: neither hidden nor given back
        else if (stand.mapOriginal && replay::entities::Exists(stand.mapOriginal) && !stand.original)
        {
            // Built: the original noted while waiting is hidden now (the
            // search skips noted originals, so it stayed drawn next to the
            // stand-in and the prop).
            HidePoseV2Original(stand, stand.mapOriginal, "map_dressing");
            stand.mapOriginal = 0;
        }
        if (g_objectStandStats.mapLogs < 32 && stand.mapVerdict == MapDressing::Build)
        {
            ++g_objectStandStats.mapLogs;
            char line[256];
            std::snprintf(line, sizeof(line),
                "POSE_V2_PLAYBACK_MAP_DRESSING,verdict=%s,why=%s,model=0x%x,pos=%.2f;%.2f;%.2f,ipl=0x%x,world_looked=%d,waited_ms=%llu,playhead_ms=%u\n",
                replay::objcheck::MapDressingText(stand.mapVerdict), why, stand.model, stand.position[0], stand.position[1],
                stand.position[2], stand.mapIpl, worldLooked ? 1 : 0, static_cast<unsigned long long>(waited), timeMs);
            Ctx().log << line;
        }
        return stand.mapVerdict == MapDressing::Build;
    }

    // A loot stand-in built, shown and hidden (the take), for the
    // bench and the QA logs; 64 lines per playback.
    void LogPoseV2LootStand(const ObjectStand& stand, const char* what, std::uint32_t timeMs)
    {
        if (!stand.loot || g_lootStandLogs >= 64)
            return;
        ++g_lootStandLogs;
        char line[256];
        std::snprintf(line, sizeof(line),
            "POSE_V2_PLAYBACK_LOOT,what=%s,model=0x%x,pos=%.2f;%.2f;%.2f,original=%d,flips=%zu,gone_ms=%d,playhead_ms=%u\n", what,
            stand.model, stand.position[0], stand.position[1], stand.position[2], stand.original ? 1 : 0,
            stand.visibleFlipsMs.size(), stand.goneMs == replay::objcheck::kNeverGone ? -1 : static_cast<int>(stand.goneMs), timeMs);
        Ctx().log << line;
    }

    // object_check.h ChooseChildHost: where a child hangs now, the
    // parent's prop once it took over (its puppet ready), else the parent's
    // stand-in while shown; 0 when the parent is not drawn.
    Object PoseV2ChildHost(const ObjectStand& parent, replay::objcheck::ChildHost& kind)
    {
        const bool standInShown = parent.standIn && parent.shown && replay::entities::Exists(parent.standIn);
        kind = replay::objcheck::ChooseChildHost(parent.propTookOver, parent.propClone != 0, standInShown);
        if (kind == replay::objcheck::ChildHost::Prop)
            return parent.propClone;
        return kind == replay::objcheck::ChildHost::StandIn ? parent.standIn : 0;
    }

    // A scenery child's object (kept in `standIn`), created within
    // its life while its parent is drawn, attached to the host and attached
    // again when the host changes (the prop taking over, a puppet rebuilt
    // after a scrub). Shown with its recorded visibility and only while the
    // host is visible.
    void UpdatePoseV2ChildStand(ObjectStand& stand, const ObjectStand& parent, std::uint32_t timeMs, std::uint32_t& creates,
        std::uint64_t now)
    {
        replay::objcheck::ChildHost kind = replay::objcheck::ChildHost::None;
        const Object host = PoseV2ChildHost(parent, kind);
        const bool hostVisible = host && (kind == replay::objcheck::ChildHost::StandIn || ENTITY::IS_ENTITY_VISIBLE(host));
        const bool wantShown = hostVisible && replay::objcheck::SceneryShown(stand.appearMs, stand.goneMs, false, 0, false, timeMs) &&
            replay::objcheck::VisibleAt(stand.visibleWhenRecorded, stand.visibleFlipsMs, timeMs);
        if (stand.standIn && !replay::entities::Exists(stand.standIn))
        {
            g_objectStandObjects.erase(stand.standIn);
            stand.standIn = 0;
            stand.host = 0;
        }
        if (!stand.standIn && wantShown && !stand.failed)
        {
            if (!stand.loadingSince)
                stand.loadingSince = now;
            if (!replay::pose::playback::RequestModel(stand.model))
            {
                if (now - stand.loadingSince > replay::playback::kModelTimeoutMs)
                {
                    stand.failed = true;
                    ++g_objectStandStats.failed;
                }
                return;
            }
            if (creates >= replay::objcheck::kSceneryCreatesPerTick || !replay::playback::StartBatchLeft()) // ClipStartStagger
                return;
            ++creates;
            replay::playback::StartBatchUse();
            const float* p = stand.position;
            stand.standIn = OBJECT::CREATE_OBJECT(stand.model, p[0], p[1], p[2], FALSE, FALSE, FALSE, FALSE, FALSE);
            if (!stand.standIn)
                return;
            ++g_objectStandStats.created;
            g_objectStandObjects.insert(stand.standIn);
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stand.standIn, TRUE, TRUE);
            ENTITY::SET_ENTITY_COLLISION(stand.standIn, FALSE, FALSE);
            // Frozen where it stood until the attach (a failed Euler left it
            // falling, shown, collision off).
            ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, TRUE);
            ENTITY::SET_ENTITY_ROTATION(stand.standIn, stand.rotation[0], stand.rotation[1], stand.rotation[2], 2, TRUE);
            stand.host = 0;
            stand.shown = true;
        }
        if (stand.failed && wantShown && !stand.missingLogged)
        {
            stand.missingLogged = true;
            LogObjectCheck(replay::objcheck::Verdict::SceneryMissing, "scenery", stand.model, timeMs, ",why=model_timeout,child=1");
        }
        if (stand.standIn && host)
        {
            const bool byFrame = replay::objcheck::AttachByFrame(kind, stand.hasFrame, parent.propPosed);
            if (host != stand.host || byFrame != stand.hostByFrame || !ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(stand.standIn, host))
            {
                const float* q = byFrame ? stand.frameRotation : stand.relativeRotation;
                const float* o = byFrame ? stand.framePosition : stand.relativePosition;
                Vector3 euler{};
                // The engine's own Euler for the quaternion (the weapon
                // attach's helper object, rotation order 2).
                if (PoseV2EngineEuler(replay::pose::Quat{q[0], q[1], q[2], q[3]}, stand.model, euler))
                {
                    ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, FALSE); // as the puppet attach does
                    ENTITY::ATTACH_ENTITY_TO_ENTITY(stand.standIn, host, byFrame ? stand.frameBone : -1, o[0], o[1], o[2],
                        euler.x, euler.y, euler.z, FALSE, FALSE, FALSE, FALSE, kPoseV2AttachRotationOrder, TRUE, FALSE, FALSE);
                    stand.host = host;
                    stand.hostByFrame = byFrame;
                    ++g_objectStandStats.childAttaches;
                    g_objectStandStats.childOnProp += kind == replay::objcheck::ChildHost::Prop ? 1u : 0u;
                    g_objectStandStats.childByFrame += byFrame ? 1u : 0u;
                    if (g_objectStandStats.childAttaches <= 32)
                        Ctx().log << "POSE_V2_PLAYBACK_SCENERY_CHILD,model=0x" << std::hex << stand.model << ",parent_model=0x"
                                  << parent.model << std::dec << ",host=" << (kind == replay::objcheck::ChildHost::Prop ? "prop" : "stand_in")
                                  << ",by_frame=" << (byFrame ? 1 : 0) << ",bone=" << (byFrame ? stand.frameBone : -1)
                                  << ",playhead_ms=" << timeMs << '\n';
                }
                else
                    ++g_objectStandStats.childNoEuler;
            }
        }
        if (stand.standIn && stand.shown != wantShown)
        {
            stand.shown = wantShown;
            ENTITY::SET_ENTITY_VISIBLE(stand.standIn, stand.shown ? TRUE : FALSE);
        }
    }

    // The recorded burn level at the playhead on a scenery stand-in (the
    // replay's own object, deleted at the end: nothing to give back), written
    // into its effect block (burn::Write, the store _SET_OBJECT_BURN_LEVEL
    // makes) whenever its own differs, in both directions (scrub back lowers
    // it).
    void PlayPoseV2StandBurnLevel(ObjectStand& stand, std::uint32_t timeMs)
    {
        if (stand.burnTrack < 0 || !g_objectBurnLevel || !replay::burn::Enabled() ||
            static_cast<std::size_t>(stand.burnTrack) >= Ctx().clip.sceneryBurnLevels.size())
            return;
        const replay::BurnLevelTrack& track = Ctx().clip.sceneryBurnLevels[static_cast<std::size_t>(stand.burnTrack)];
        const float level = replay::BurnLevelAt(track, timeMs);
        const float wanted = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
        const void* address = replay::entities::AddressOf(stand.standIn);
        replay::burn::State state;
        if (!replay::burn::Read(address, state) || !state.fireBlock || std::fabs(state.level - wanted) < 0.001f)
            return;
        const int blocks = replay::burn::Write(address, wanted);
        ++g_objectStandStats.burnWrites;
        if (g_objectStandStats.burnLogs < 32)
        {
            ++g_objectStandStats.burnLogs;
            Ctx().log << "POSE_V2_BURN_LEVEL,phase=play_scenery,id=scenery:" << track.id << ",model=0x" << std::hex << stand.model
                      << std::dec << ",from=" << state.level << ",to=" << wanted << ",blocks=" << blocks
                      << ",writes=" << g_objectStandStats.burnWrites << ",playhead_ms=" << timeMs << '\n';
        }
    }

    void UpdatePoseV2WorldStandIns(std::uint32_t timeMs)
    {
        if (g_objectStands.empty())
            return;
        REPLAY_PROFILE_ZONE("play.object_stands");
        SearchPoseV2Originals();
        std::uint32_t creates = 0;
        const std::uint64_t now = GetTickCount64();
        for (ObjectStand& stand : g_objectStands)
        {
            if (stand.parentStand >= 0)
            {
                // A child, after its parent (lower index) this tick.
                if (static_cast<std::size_t>(stand.parentStand) < g_objectStands.size())
                    UpdatePoseV2ChildStand(stand, g_objectStands[static_cast<std::size_t>(stand.parentStand)], timeMs,
                        creates, now);
                continue;
            }
            // Map dressing is built only when the map does not have it
            // (objcheck::MapDressingVerdict).
            if (!DecidePoseV2MapDressing(stand, timeMs, now))
                continue;
            // Before the prop took over: the stand-in; from then on the prop
            // (and nothing once the prop is gone: broken, removed), with the
            // stand-in kept until the prop puppet is built.
            const replay::entity::CreatePacket* create = stand.prop.Valid() ? Ctx().clip.FindCreate(stand.prop.Packed()) : nullptr;
            const std::uint32_t takeoverMs = create ? create->timeMs : 0;
            bool propReady = false;
            stand.propClone = 0; // for the children
            stand.propPosed = false;
            stand.propTookOver = create && timeMs >= takeoverMs;
            if (stand.prop.Valid())
                for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                    if (puppet.packet.id == stand.prop && puppet.state == replay::playback::PuppetState::Ready &&
                        puppet.clone && replay::entities::Exists(puppet.clone))
                    {
                        propReady = true;
                        stand.propClone = puppet.clone;
                        stand.propPosed = puppet.overrideSlot >= 0;
                    }
            // The prop puppet is created, given its skeleton and armed in one
            // tick; hiding the stand-in in that tick would leave a frame with
            // neither drawn. The stand-in stays until the tick after the
            // prop's first published pose (objcheck::HandoffPropReady): both
            // stand in the same place and shape for that one frame.
            if (propReady)
            {
                ++stand.propReadyTicks;
                stand.propPosedTicks = stand.propPosed ? stand.propPosedTicks + 1 : 0;
                if (stand.propHasPose < 0)
                {
                    std::uint32_t firstMs = 0, lastMs = 0;
                    stand.propHasPose = replay::pose::capture::TrackSpan(stand.prop.Packed(), firstMs, lastMs) ? 1 : 0;
                }
            }
            else
            {
                stand.propReadyTicks = 0;
                stand.propPosedTicks = 0;
                stand.handoffLogged = false;
            }
            const bool handoffReady = replay::objcheck::HandoffPropReady(propReady, stand.propHasPose > 0, stand.propReadyTicks,
                stand.propPosedTicks, g_sceneryHandoffTicks);
            if (propReady && !handoffReady && stand.shown && stand.standIn && create && timeMs >= takeoverMs)
                ++g_objectStandStats.handoffOverlapTicks;
            const bool wantShown = replay::objcheck::SceneryShown(stand.appearMs, stand.goneMs,
                stand.prop.Valid() && create, takeoverMs, handoffReady, timeMs) &&
                replay::objcheck::VisibleAt(stand.visibleWhenRecorded, stand.visibleFlipsMs, timeMs);
            if (handoffReady && !wantShown && stand.shown && stand.standIn && !stand.handoffLogged && create &&
                timeMs >= takeoverMs)
            {
                stand.handoffLogged = true;
                ++g_objectStandStats.handoffs;
                if (g_objectStandStats.handoffLogs < 32)
                {
                    ++g_objectStandStats.handoffLogs;
                    Ctx().log << "POSE_V2_SCENERY_HANDOFF,prop=" << replay::entity::IdText(stand.prop) << ",model=0x" << std::hex
                              << stand.model << std::dec << ",scenery=" << (stand.scenery ? 1 : 0)
                              << ",ready_ticks=" << stand.propReadyTicks << ",posed_ticks=" << stand.propPosedTicks
                              << ",has_pose=" << static_cast<int>(stand.propHasPose) << ",overlap=" << g_sceneryHandoffTicks
                              << ",takeover_ms=" << takeoverMs << ",playhead_ms=" << timeMs << '\n';
                }
            }
            if (stand.standIn && !replay::entities::Exists(stand.standIn))
            {
                g_objectStandObjects.erase(stand.standIn);
                stand.standIn = 0;
            }
            if (!stand.standIn && wantShown && !stand.failed && !stand.hatHidden &&
                stand.hatRebuild == replay::hatlook::Rebuild::Dress)
            {
                // hat_look.h: the hat made with its recorded look by the hat
                // dresser (the bare model is the grey octagon); nothing drawn
                // when it cannot be made.
                bool dressFailed = false;
                const Object hat = DressPoseV2Hat(HatClientOfStand(static_cast<std::size_t>(&stand - g_objectStands.data())),
                    stand.model, stand.hatLook, stand.position, dressFailed, timeMs);
                if (dressFailed)
                    stand.hatHidden = true;
                if (hat)
                {
                    stand.standIn = hat;
                    ++g_objectStandStats.created;
                    g_objectStandObjects.insert(stand.standIn);
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stand.standIn, TRUE, TRUE);
                    ENTITY::SET_ENTITY_INVINCIBLE(stand.standIn, TRUE);
                    ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, TRUE);
                    ENTITY::SET_ENTITY_COLLISION(stand.standIn, FALSE, FALSE);
                    const float* p = stand.position;
                    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(stand.standIn, p[0], p[1], p[2], FALSE, FALSE, FALSE);
                    const float* r = stand.rotation;
                    ENTITY::SET_ENTITY_ROTATION(stand.standIn, r[0], r[1], r[2], 2, TRUE);
                    SuppressPickupLook(stand.standIn);
                    ENTITY::SET_ENTITY_VISIBLE(stand.standIn, TRUE);
                    stand.shown = true;
                    LogPoseV2LootStand(stand, "created", timeMs);
                }
            }
            if (!stand.standIn && wantShown && !stand.failed && !stand.hatHidden &&
                stand.hatRebuild != replay::hatlook::Rebuild::Dress)
            {
                if (!stand.loadingSince)
                    stand.loadingSince = now;
                if (!replay::pose::playback::RequestModel(stand.model))
                {
                    if (now - stand.loadingSince > replay::playback::kModelTimeoutMs)
                    {
                        stand.failed = true;
                        ++g_objectStandStats.failed;
                    }
                    continue;
                }
                if (creates >= replay::objcheck::kSceneryCreatesPerTick || !replay::playback::StartBatchLeft()) // ClipStartStagger
                    continue;
                ++creates;
                replay::playback::StartBatchUse();
                const float* p = stand.position;
                stand.standIn = OBJECT::CREATE_OBJECT(stand.model, p[0], p[1], p[2], FALSE, FALSE, FALSE, FALSE, FALSE);
                if (!stand.standIn)
                    continue;
                ++g_objectStandStats.created;
                g_objectStandObjects.insert(stand.standIn);
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stand.standIn, TRUE, TRUE);
                ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, TRUE);
                ENTITY::SET_ENTITY_COLLISION(stand.standIn, FALSE, FALSE);
                ENTITY::SET_ENTITY_COORDS_NO_OFFSET(stand.standIn, p[0], p[1], p[2], FALSE, FALSE, FALSE);
                const float* r = stand.rotation;
                ENTITY::SET_ENTITY_ROTATION(stand.standIn, r[0], r[1], r[2], 2, TRUE);
                stand.shown = true;
                LogPoseV2LootStand(stand, "created", timeMs);
                // A late scenery entry's stand-in (the
                // camp's prop set objects, a mission camp reached later).
                if (stand.scenery && stand.appearMs > 0 && g_lateStandLogs < 48)
                {
                    ++g_lateStandLogs;
                    char line[224];
                    std::snprintf(line, sizeof(line),
                        "POSE_V2_PLAYBACK_SCENERY_LATE,what=created,model=0x%x,pos=%.2f;%.2f;%.2f,appear_ms=%u,taken=%d,playhead_ms=%u\n",
                        stand.model, p[0], p[1], p[2], stand.appearMs, stand.prop.Valid() ? 1 : 0, timeMs);
                    Ctx().log << line;
                }
            }
            if (stand.failed && wantShown && !stand.missingLogged)
            {
                stand.missingLogged = true;
                LogObjectCheck(replay::objcheck::Verdict::SceneryMissing,
                    stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "scenery", stand.model, timeMs, ",why=model_timeout");
            }
            if (stand.standIn && stand.shown != wantShown)
            {
                stand.shown = wantShown;
                ENTITY::SET_ENTITY_VISIBLE(stand.standIn, stand.shown ? TRUE : FALSE);
                LogPoseV2LootStand(stand, stand.shown ? "shown" : "hidden", timeMs);
            }
            if (stand.standIn)
            {
                PlayPoseV2StandBurnLevel(stand, timeMs);
                // The stand-in's custom texture and tint (a scenery entry's own
                // track, else the prop's that takes a world object over).
                if (stand.scenery && stand.lookTrack >= 0)
                    replay::objlook::PlayStand(stand.standIn, true, static_cast<std::uint32_t>(stand.lookTrack), stand.model, timeMs);
                else if (!stand.scenery && stand.prop.Valid())
                    replay::objlook::PlayStand(stand.standIn, false, stand.prop.Packed(), stand.model, timeMs);
            }
        }
    }

    // object_check.h WeaponPlain: a weapon or prop of the clip
    // built as its bare model although its model is a weapon's in the same
    // clip (clips before the fix: the five finger fillet knife without its
    // grip), one line per object at the playback start.
    void WarnPoseV2WeaponPlain()
    {
        const std::map<std::uint32_t, std::uint32_t> weaponModels = replay::objcheck::WeaponModels(Ctx().clip.created);
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (replay::objcheck::WeaponPlain(packet, weaponModels))
                LogObjectCheck(replay::objcheck::Verdict::WeaponPlain, replay::entity::IdText(packet.id), packet.modelHash,
                    packet.timeMs, ",phase=play,why=bare_model");
    }

    void ReplayOwnObjects(std::vector<Object>& out) { CollectPoseV2ReplayOwnObjects(out); }
    std::uint32_t EntityOwnerOf(Entity entity) { return ReadEntityOwner(entity); }

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

    // A script restart with a playback open: the stand-ins and the attach
    // helper are the old world's handles (the next BeginPlayback would delete
    // them by number): forgotten, not deleted.
    std::uint32_t ForgetPlaybackAfterRestart()
    {
        std::uint32_t forgotten = static_cast<std::uint32_t>(g_objectStands.size()) + (g_poseV2AttachHelper ? 1u : 0u);
        g_objectStands.clear();
        g_objectStandObjects.clear();
        g_poseV2AttachHelper = 0;
        forgotten += g_hatDresser.helper ? 1u : 0u;
        EndPoseV2HatDresser(true);
        g_fragApplied.clear();
        g_fragPuppet.clear();
        g_fragPieceShaped.clear();
        g_fragPieceWaitUntil.clear();
        replay::objlook::ForgetAfterRestart();
        return forgotten;
    }

    void UpdateWeaponPuppets(std::uint32_t timeMs, std::uint32_t& creates) { UpdatePoseV2Weapons(timeMs, creates); }
    void PublishStrapOwned() { PublishPoseV2StrapOwned(); }
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

    // See the
    // declarations in replay_objects.h.
    bool TakeVehiclePartsUrgent(replay::entity::ReplayId vehicle)
    {
        if (!g_vehiclePartsUrgent.erase(vehicle.Packed()))
            return false;
        ++g_wheelPieceStats.urgent;
        return true;
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
        // makes them (inc at 0x14A94DA): complete when it reached the most
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

    // Bench loot watch: every object within the radius, sampled every 100 ms,
    // one line per change (first sight, gone, visibility, attach, root move,
    // skeleton built or rebuilt, bone pose away from the first sight).
    namespace
    {
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
    // type, guarded reads (offline): inst [ent+0x38] (vtable RVA 0x33CBDB8),
    // type [inst+0x138], LOD group [type+0x90], LOD u32 [inst+0x178] (0..2),
    // physLOD [lg+0x10+8*lod]; groups u16 [pl+0x140], children s16 [pl+0x146],
    // group g [[pl+0xC8]+8*g], its first child u16 [grp+8] and count u16
    // [grp+0xA]; the inverse table u16 [[pl+0x110]+2*child] must give g back.
    constexpr bool kPlayFragBreaks = true;

    // After a break: the clone's cache entry [inst+0x128] (0: nothing broke,
    // every group present) and whether the group is gone (s16
    // [[cache+0x148]+2*g] == -1, IsGroupBroken 0x296EEC0). 1 gone, 0
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
    constexpr std::uintptr_t kFragInstVtableRva = 0x33CBDB8;

    int GuardedGroupToChild(std::uint64_t entity, std::uintptr_t base, std::uint16_t group)
    {
        __try
        {
            const std::uint64_t inst = *reinterpret_cast<const std::uint64_t*>(entity + 0x38);
            if (!inst || *reinterpret_cast<const std::uint64_t*>(inst) != base + kFragInstVtableRva)
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
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            // Off for vehicles: BREAK_OBJECT_FRAGMENT_CHILD with the object's
            // group map breaks nothing on a burning wagon (gone=0 every time)
            // and the game can crash a second later in the engine's own update
            // (RDR2.exe+0x15E528, no mod frame). The breaks are still recorded.
            // Offline, the native's handler (0x10CDEC0) takes the entity
            // through the object-typed getter 0xEB39D8, which gives null for a
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
                const int child = GuardedGroupToChild(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), base, e.group);
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
