#include "replay_capture.h"
#include "object_check.h"
#include "replay_buffer.h" // VisibilityIndex

#include "burn_level.h"
#include "entity_appearance.h"
#include "metaped_reader.h"
#include "entity_discovery.h"
#include "entity_pools.h"
#include "entity_selection.h"
#include "frag_observer.h" // the break count: a scan every tick after a break
#include "log_format.h"
#include "object_pose.h"
#include "pose_capture.h"
#include "pose_fidelity.h"
#include "profiler.h"
#include "entity_cache.h"
#include "deferred_log.h"
#include "replay_events.h"
#include "replay_objects.h"
#include "object_look.h"
#include "replay_trains.h"
#include "vehicle_doors_engine.h"
#include "skeleton_resolve.h"
#include "track_index.h"
#include "engine_hud.h"
#include "guarded_memory.h" // the always-prerender bit read before touching it
#include "anim_visible.h"   // the "animate as if visible" byte
#include "anim_lod_hook.h"  // LOD 0 for the recorded peds' directors
#include "shader_vars_replay.h" // the horses' muscles (SHVR)

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace replay::recorder
{
    namespace
    {
        Context* g_context = nullptr;
        Context& Ctx() { return *g_context; }

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;

        std::uint64_t ElapsedUs(std::uint64_t start, std::uint64_t end)
        {
            return (end - start) * 1000000ull / replay::prof::TicksPerSecond();
        }

        // What the recording does so the engine computes a recorded ped as if
        // the game camera saw it, and what to give back. The anim-visible byte
        // (anim_visible.h) goes to every recorded ped and animal; the natives
        // (bench experiment) to the recorded humans within radiusM of the
        // camera.
        FullAnimConfig g_fullAnim;
        struct FullAnimState
        {
            bool prerenderSet = false;     // ours to clear (the game had not set it)
            bool prerenderForeign = false; // the game had it set: never touched
            bool lodSet = false;
            float lodBefore = 1.0f;
            bool visibleSet = false;       // the anim-visible byte was 0 and is ours to clear
            bool visibleForeign = false;   // it was set already (a pedshot): never touched
            std::uintptr_t ped = 0;        // the entity's address and its graphics component,
            std::uintptr_t component = 0;  // resolved once (owner checked every tick)
        };
        std::unordered_map<Entity, FullAnimState> g_fullAnimPeds;
        struct FullAnimTotals
        {
            std::uint32_t peds = 0;
            std::uint32_t alreadyPrerender = 0;
            std::uint64_t forcedTicks = 0;
            // anim_visible.h: peds whose component resolved, did not, found
            // the byte already set (a pedshot), byte writes (0 -> 1, each
            // tick that found it 0), given back.
            std::uint32_t visiblePeds = 0;
            std::uint32_t visibleUnresolved = 0;
            std::uint32_t visibleAlready = 0;
            std::uint64_t visibleWrites = 0;
            std::uint32_t visibleGivenBack = 0;
            // anim_lod_hook.h: the most directors forced at once, and the
            // recorded peds whose director could not be read.
            std::uint32_t lodDirectorsMax = 0;
            std::uint32_t lodUnresolved = 0;
        } g_fullAnimTotals;
        // The detour of 0x2BD410C: tried once per load (installed on the
        // first recording that wants it), the error kept for the log.
        bool g_animLodTried = false;
        std::string g_animLodError = "not_tried";
        std::vector<std::uintptr_t> g_animLodDirectors;
        // The always-prerender bit (entity +0xB0 bit 23, set by 0x561B58, the
        // native's handler 0xF9EFB8): a ped the game already keeps there is
        // not touched (nor cleared afterwards).
        constexpr std::uintptr_t kEntityRenderFlags = 0xB0;
        constexpr std::uint32_t kAlwaysPrerenderBit = 1u << 23;

        std::uintptr_t AnimVisibleComponent(Entity ped, FullAnimState& state)
        {
            // The cached component while it still belongs to the entity (one
            // read); the chain again otherwise (anim_visible.h).
            if (state.component && state.ped)
            {
                std::uintptr_t owner = 0;
                if (replay::mem::Copy(&owner, state.component + replay::animvisible::kComponentOwnerOffset, sizeof(owner)) &&
                    (owner & ~std::uintptr_t{0xF}) == state.ped)
                {
                    return state.component;
                }
            }
            static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            state.ped = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
            state.component = replay::animvisible::GraphicsComponent(base, state.ped,
                [](std::uintptr_t at, void* out, std::size_t size) { return replay::mem::Copy(out, at, size); });
            return state.component;
        }

        // The byte on, re-asserted every tick (a pedshot release writes 0);
        // a byte the game had set already is left alone.
        void ApplyAnimVisible(Entity ped, FullAnimState& state, bool first)
        {
            if (state.visibleForeign)
                return;
            const std::uintptr_t component = AnimVisibleComponent(ped, state);
            std::uint8_t now = 0;
            if (!component || !replay::mem::Copy(&now, component + replay::animvisible::kAnimVisibleOffset, 1))
            {
                if (first)
                    ++g_fullAnimTotals.visibleUnresolved;
                return;
            }
            if (first)
            {
                ++g_fullAnimTotals.visiblePeds;
                if (now != 0)
                {
                    ++g_fullAnimTotals.visibleAlready;
                    state.visibleForeign = true;
                    return;
                }
            }
            if (now != 0)
                return;
            const std::uint8_t on = 1;
            if (replay::mem::Put(component + replay::animvisible::kAnimVisibleOffset, &on, 1))
            {
                state.visibleSet = true;
                ++g_fullAnimTotals.visibleWrites;
            }
        }

        void GiveBackAnimVisible(Entity ped, FullAnimState& state)
        {
            if (!state.visibleSet)
                return;
            state.visibleSet = false;
            state.component = 0; // resolved again: the entity may have been reused
            const std::uintptr_t component = AnimVisibleComponent(ped, state);
            const std::uint8_t off = 0;
            if (component && replay::mem::Put(component + replay::animvisible::kAnimVisibleOffset, &off, 1))
                ++g_fullAnimTotals.visibleGivenBack;
        }

        // The natives of the experiment off for a ped that left the radius.
        void GiveBackNatives(Entity ped, FullAnimState& state)
        {
            if (state.prerenderSet)
                ENTITY::SET_ENTITY_ALWAYS_PRERENDER(ped, FALSE);
            if (state.lodSet)
                PED::SET_PED_LOD_MULTIPLIER(ped, state.lodBefore);
            state.prerenderSet = false;
            state.lodSet = false;
        }

        void GiveBackFullAnim(Entity ped, FullAnimState state)
        {
            if (!replay::entities::Exists(ped))
                return;
            GiveBackNatives(ped, state);
            GiveBackAnimVisible(ped, state);
        }


        void EndFullAnim()
        {
            replay::muscles::EndRecord(); // MUSCLE_RECORD_END (and the evidence)
            for (const auto& [ped, state] : g_fullAnimPeds)
                GiveBackFullAnim(ped, state);
            const replay::animlod::Stats lodStats = replay::animlod::Snapshot();
            if (replay::animlod::Installed())
            {
                g_animLodDirectors.clear();
                replay::animlod::SetForcedFor(replay::animlod::Owner::Capture, g_animLodDirectors);
                replay::animlod::ResetStats();
            }
            if (g_fullAnim.animLod0)
                Ctx().log << "RECORD_ANIM_LOD,installed=" << (replay::animlod::Installed() ? 1 : 0)
                          << ",error=" << g_animLodError << ",calls=" << lodStats.calls
                          << ",forced=" << lodStats.forced << ",changed=" << lodStats.changed
                          << ",directors_max=" << g_fullAnimTotals.lodDirectorsMax
                          << ",unresolved=" << g_fullAnimTotals.lodUnresolved << '\n';
            if (g_fullAnim.prerender || g_fullAnim.forceAnim || g_fullAnim.lodMultiplier || g_fullAnim.animVisible)
                Ctx().log << "RECORD_FULL_ANIM,prerender=" << (g_fullAnim.prerender ? 1 : 0) << ",force_anim="
                          << (g_fullAnim.forceAnim ? 1 : 0) << ",lod_multiplier=" << g_fullAnim.lodMultiplier
                          << ",anim_visible=" << (g_fullAnim.animVisible ? 1 : 0)
                          << ",radius_m=" << g_fullAnim.radiusM << ",peds=" << g_fullAnimTotals.peds
                          << ",already_prerender=" << g_fullAnimTotals.alreadyPrerender
                          << ",forced_ticks=" << g_fullAnimTotals.forcedTicks
                          << ",visible_peds=" << g_fullAnimTotals.visiblePeds
                          << ",visible_unresolved=" << g_fullAnimTotals.visibleUnresolved
                          << ",visible_already=" << g_fullAnimTotals.visibleAlready
                          << ",visible_writes=" << g_fullAnimTotals.visibleWrites
                          << ",visible_given_back=" << g_fullAnimTotals.visibleGivenBack << '\n';
            g_fullAnimPeds.clear();
            g_fullAnimTotals = {};
        }

        void UpdateFullAnim()
        {
            const bool natives = g_fullAnim.prerender || g_fullAnim.forceAnim || g_fullAnim.lodMultiplier;
            if (!natives && !g_fullAnim.animVisible && !g_fullAnim.animLod0)
                return;
            REPLAY_PROFILE_ZONE("rec.full_anim");
            if (g_fullAnim.animLod0 && !g_animLodTried)
            {
                g_animLodTried = true;
                std::string error;
                g_animLodError = replay::animlod::Install(error) ? "none" : error;
                Ctx().log << "ANIM_LOD_HOOK,installed=" << (replay::animlod::Installed() ? 1 : 0)
                          << ",error=" << g_animLodError << '\n';
            }
            const bool lod0 = g_fullAnim.animLod0 && replay::animlod::Installed();
            static const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            g_animLodDirectors.clear();
            Vector3 camera{};
            if (natives)
                camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const float radius2 = g_fullAnim.radiusM * g_fullAnim.radiusM;
            std::unordered_map<Entity, FullAnimState> keep;
            keep.reserve(g_fullAnimPeds.size() + 8);
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                const Entity ped = entity.handle;
                if (entity.vehicle || !replay::entities::IsPed(ped))
                    continue;
                const auto found = g_fullAnimPeds.find(ped);
                const bool first = found == g_fullAnimPeds.end();
                FullAnimState state = first ? FullAnimState{} : found->second;
                if (first)
                    ++g_fullAnimTotals.peds;
                if (g_fullAnim.animVisible)
                    ApplyAnimVisible(ped, state, first);
                if (lod0)
                {
                    const std::uintptr_t address = state.ped ? state.ped
                        : reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
                    const std::uintptr_t director = replay::animlod::DirectorOf(moduleBase, address,
                        [](std::uintptr_t at, void* out, std::size_t size) { return replay::mem::Copy(out, at, size); });
                    if (director)
                        g_animLodDirectors.push_back(director);
                    else if (first)
                        ++g_fullAnimTotals.lodUnresolved;
                }
                bool inRadius = false;
                if (natives && PED::IS_PED_HUMAN(ped))
                {
                    float at[3]{};
                    if (replay::entities::Position(ped, at))
                    {
                        const float dx = at[0] - camera.x, dy = at[1] - camera.y, dz = at[2] - camera.z;
                        inRadius = dx * dx + dy * dy + dz * dz <= radius2;
                    }
                }
                if (inRadius)
                {
                    if (g_fullAnim.prerender && !state.prerenderSet && !state.prerenderForeign)
                    {
                        std::uint32_t flags = 0;
                        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
                        if (address && replay::mem::Copy(&flags, address + kEntityRenderFlags, sizeof(flags)) &&
                            (flags & kAlwaysPrerenderBit) == 0)
                        {
                            ENTITY::SET_ENTITY_ALWAYS_PRERENDER(ped, TRUE);
                            state.prerenderSet = true;
                        }
                        else
                        {
                            state.prerenderForeign = true;
                            ++g_fullAnimTotals.alreadyPrerender;
                        }
                    }
                    if (g_fullAnim.lodMultiplier && !state.lodSet)
                    {
                        state.lodBefore = PED::_GET_PED_LOD_MULTIPLIER(ped);
                        PED::SET_PED_LOD_MULTIPLIER(ped, static_cast<float>(g_fullAnim.lodMultiplier));
                        state.lodSet = true;
                    }
                    if (g_fullAnim.forceAnim)
                    {
                        PED::FORCE_PED_AI_AND_ANIMATION_UPDATE(ped, TRUE, FALSE); // as the puppets (puppet_lod.h)
                        ++g_fullAnimTotals.forcedTicks;
                    }
                }
                else
                {
                    GiveBackNatives(ped, state);
                }
                keep[ped] = state;
                if (!first)
                    g_fullAnimPeds.erase(found); // kept: not given back below
            }
            // Out of the recording: given back.
            for (const auto& [ped, state] : g_fullAnimPeds)
                GiveBackFullAnim(ped, state);
            g_fullAnimPeds.swap(keep);
            if (lod0)
            {
                replay::animlod::SetForcedFor(replay::animlod::Owner::Capture, g_animLodDirectors);
                g_fullAnimTotals.lodDirectorsMax = (std::max)(g_fullAnimTotals.lodDirectorsMax,
                    static_cast<std::uint32_t>(g_animLodDirectors.size()));
            }
        }

    // The components CREATE_VEHICLE does not reproduce (cargo
    // prop set, lanterns, extras). The prop set is read by hash and through
    // its handle; both go to the log until the bench shows which one holds.
    // And the paint (tint) and livery: CREATE_VEHICLE picks its
    // own, the look carries the recorded ones (the latest, see VehicleLook).
    void ReadVehicleLook(Vehicle vehicle, replay::VehicleLook& look, Hash& byHash, Hash& byHandle)
    {
        const bool anyPropSet = PROPSET::_DOES_VEHICLE_HAVE_ANY_PROP_SET(vehicle) != FALSE;
        byHash = anyPropSet ? PROPSET::_GET_VEHICLE_PROP_SET_HASH(vehicle) : 0;
        byHandle = anyPropSet
            ? PROPSET::_GET_PROP_SET_MODEL(PROPSET::_GET_VEHICLE_PROP_SET(vehicle)) : 0;
        look.propSet = byHash ? byHash : byHandle;
        look.lightPropSet = PROPSET::_DOES_VEHICLE_HAVE_ANY_LIGHT_PROP_SET(vehicle)
            ? PROPSET::_GET_PROP_SET_MODEL(PROPSET::_GET_VEHICLE_LIGHT_PROP_SET(vehicle)) : 0;
        look.extrasExist = 0;
        look.extrasOn = 0;
        for (int extra = 0; extra < 32; ++extra)
        {
            if (!VEHICLE::DOES_EXTRA_EXIST(vehicle, extra))
                continue;
            look.extrasExist |= 1u << extra;
            if (VEHICLE::IS_VEHICLE_EXTRA_TURNED_ON(vehicle, extra))
                look.extrasOn |= 1u << extra;
        }
        look.tint = VEHICLE::_GET_VEHICLE_TINT(vehicle);
        look.livery = VEHICLE::_GET_VEHICLE_LIVERY(vehicle);
    }

    void LogVehicleLook(const char* phase, const replay::VehicleLook& look, Hash byHash, Hash byHandle,
        std::uint32_t timeMs)
    {
        std::ostream& log = Ctx().log;
        log << "POSE_V2_VEHICLE_LOOK,phase=" << phase
            << ",id=" << replay::entity::IdText(look.id)
            << std::hex
            << ",propset=0x" << look.propSet
            << ",propset_hash=0x" << byHash
            << ",propset_model=0x" << byHandle
            << ",light=0x" << look.lightPropSet
            << ",extras=0x" << look.extrasExist
            << ",extras_on=0x" << look.extrasOn
            << std::dec
            << ",tint=" << look.tint
            << ",livery=" << look.livery
            << ",time_ms=" << timeMs
            << '\n';
    }

    void CaptureVehicleLook(Vehicle vehicle, replay::entity::ReplayId id)
    {
        replay::VehicleLook look;
        look.id = id;
        Hash byHash = 0;
        Hash byHandle = 0;
        ReadVehicleLook(vehicle, look, byHash, byHandle);
        look.lastCheckMs = replay::pose::capture::NowMs();
        Ctx().clip.vehicleLooks.push_back(look);
        LogVehicleLook("record", look, byHash, byHandle, look.lastCheckMs);
    }

    // Skeleton resolve failures per handle (retried, see
    // RegisterPoseV2Entity), cleared at each recording start.
    constexpr std::uint32_t kUnresolvedTries = 4;
    std::vector<std::pair<Entity, std::uint32_t>> g_unresolvedTries;
    std::uint32_t g_unresolvedRetryLogs = 0;

    // metaped_state.h, metaped_reader.h: the whole metaped look per ped and
    // horse, beyond the layers. A state is stored at once (entry, skeleton
    // change, layer change) and refined by a second read 250 ms later: only the
    // expressions with the same value in both stay (a morph the ped is dressed
    // with; the face the animation moves is left out). A ped read while the
    // engine still built it (no layers) is read again until it has them, in
    // place (source late). Once a second the stable expressions are compared
    // with the ped's; a change seen on two checks in a row is a new state
    // (source change).
    struct PoseV2MetapedProbe
    {
        std::uint32_t id = 0;
        bool pending = false;          // the state at stateIndex waits for its second read
        std::uint32_t firstMs = 0;
        std::size_t stateIndex = 0;
        std::vector<replay::metaped::Expression> first;
        std::uint32_t lastCheckMs = 0;
        bool candidate = false;        // a change seen once
        std::uint64_t candidatePrint = 0;
        std::uint32_t candidateMs = 0;
    };
    struct PoseV2MetapedStats
    {
        std::uint64_t states = 0;
        std::uint64_t confirms = 0;
        std::uint64_t late = 0;
        std::uint64_t changes = 0;
        std::uint64_t unstableDropped = 0; // expressions left out by the second read
        std::uint64_t readFailures = 0;
        std::uint64_t noExpressions = 0;   // states stored without the reader's part
    };
    std::vector<PoseV2MetapedProbe> g_poseV2MetapedProbes;
    PoseV2MetapedStats g_poseV2MetapedStats;
    std::size_t g_poseV2MetapedCursor = 0;
    std::uint32_t g_poseV2MetapedLogs = 0;
    constexpr std::uint32_t kPoseV2MetapedLogCap = 400;
    constexpr std::uint32_t kPoseV2MetapedConfirmMs = 250;
    constexpr std::uint32_t kPoseV2MetapedGiveUpMs = 3000;
    constexpr std::uint32_t kPoseV2MetapedCheckMs = 1000;
    constexpr float kPoseV2MetapedStableEps = 1e-4f;
    constexpr float kPoseV2MetapedChangeEps = 0.01f;

    PoseV2MetapedProbe& PoseV2MetapedProbeOf(std::uint32_t packed)
    {
        for (PoseV2MetapedProbe& probe : g_poseV2MetapedProbes)
            if (probe.id == packed)
                return probe;
        g_poseV2MetapedProbes.push_back({});
        g_poseV2MetapedProbes.back().id = packed;
        return g_poseV2MetapedProbes.back();
    }

    void LogPoseV2Metaped(const char* phase, replay::entity::ReplayId id, const replay::metaped::State& state,
        std::size_t dropped)
    {
        if (g_poseV2MetapedLogs >= kPoseV2MetapedLogCap)
            return;
        ++g_poseV2MetapedLogs;
        Ctx().log << "POSE_V2_METAPED,phase=" << phase << ",id=" << replay::entity::IdText(id)
                  << ",source=" << replay::metaped::SourceText(state.source) << ",outfit=0x" << std::hex << state.outfit
                  << std::dec << ",type=" << state.metapedType << ",layers=" << state.layerCount
                  << ",expressions=" << state.expressions.size() << ",unstable_dropped=" << dropped
                  << ",reader=" << ((state.flags & replay::metaped::kHasExpressions) ? "on" : "off")
                  << ",scale=" << ((state.flags & replay::metaped::kHasScale) ? state.scale : 0.0f)
                  << ",cleanliness=" << state.cleanliness
                  << ",shop_items=" << state.shopItems.size()
                  << ",shop_states=" << std::count_if(state.shopItems.begin(), state.shopItems.end(),
                         [](const replay::metaped::ShopItem& item) { return item.state != 0; })
                  << ",time_ms=" << state.timeMs << '\n';
    }

    // A new state of `id` (create, segment or change) from the ped now.
    void RecordPoseV2Metaped(Entity handle, replay::entity::ReplayId id, replay::metaped::Source source,
        std::uint32_t timeMs, const replay::entity::Appearance& layers)
    {
        REPLAY_PROFILE_ZONE("rec.metaped");
        const std::uint32_t packed = id.Packed();
        replay::metaped::State state;
        state.timeMs = timeMs;
        state.source = source;
        replay::metaped::engine::Capture(handle, state);
        state.layerCount = static_cast<std::uint32_t>(layers.assets.size());
        state.layersPrint = replay::AppearancePrint(layers);
        if (!(state.flags & replay::metaped::kHasExpressions))
            ++g_poseV2MetapedStats.noExpressions;
        replay::metaped::Track* track = replay::metaped::FindTrack(Ctx().clip.metapeds, packed);
        if (!track)
        {
            Ctx().clip.metapeds.push_back({});
            track = &Ctx().clip.metapeds.back();
            track->id = packed;
        }
        // Times per track ascend (a segment and a change in the same tick:
        // the later one replaces the earlier).
        if (!track->states.empty() && track->states.back().timeMs >= state.timeMs)
        {
            state.timeMs = track->states.back().timeMs;
            track->states.back() = state;
        }
        else
            track->states.push_back(state);
        ++g_poseV2MetapedStats.states;
        PoseV2MetapedProbe& probe = PoseV2MetapedProbeOf(packed);
        probe.pending = (state.flags & replay::metaped::kHasExpressions) != 0 || state.layerCount == 0;
        probe.first = state.expressions;
        probe.firstMs = replay::pose::capture::NowMs();
        probe.stateIndex = track->states.size() - 1;
        probe.lastCheckMs = probe.firstMs;
        probe.candidate = false;
        LogPoseV2Metaped("record", id, track->states.back(), 0);
    }

    // The second read of a pending state, and the change check.
    void SamplePoseV2Metaped()
    {
        REPLAY_PROFILE_ZONE("rec.metaped_poll");
        std::vector<replay::recorder::RecordedEntity>& entities = Ctx().recorder.entities;
        if (entities.empty())
            return;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        constexpr std::size_t kVisits = 3;
        std::size_t reads = 0;
        const std::size_t visits = (std::min)(entities.size(), kVisits * 2);
        for (std::size_t visit = 0; visit < visits && reads < kVisits; ++visit)
        {
            const replay::recorder::RecordedEntity& entity = entities[g_poseV2MetapedCursor++ % entities.size()];
            if (entity.vehicle)
                continue;
            const std::uint32_t packed = entity.id.Packed();
            replay::metaped::Track* track = replay::metaped::FindTrack(Ctx().clip.metapeds, packed);
            if (!track || track->states.empty())
                continue;
            PoseV2MetapedProbe& probe = PoseV2MetapedProbeOf(packed);
            if (probe.pending)
            {
                if (nowMs - probe.firstMs < kPoseV2MetapedConfirmMs || probe.stateIndex >= track->states.size())
                    continue;
                ++reads;
                replay::metaped::State& state = track->states[probe.stateIndex];
                if (state.layerCount == 0)
                {
                    // Read while the engine built the ped: read it all again
                    // once it has layers, in place.
                    replay::entity::Appearance layers;
                    replay::entity::appearance::Capture(entity.handle, layers);
                    if (layers.assets.empty() && !layers.outfit)
                    {
                        if (nowMs - probe.firstMs > kPoseV2MetapedGiveUpMs)
                            probe.pending = false;
                        continue;
                    }
                    replay::metaped::State again;
                    again.timeMs = state.timeMs;
                    again.source = replay::metaped::Source::Late;
                    replay::metaped::engine::Capture(entity.handle, again);
                    again.layerCount = static_cast<std::uint32_t>(layers.assets.size());
                    again.layersPrint = replay::AppearancePrint(layers);
                    state = std::move(again);
                    probe.first = state.expressions;
                    probe.firstMs = nowMs;
                    ++g_poseV2MetapedStats.late;
                    LogPoseV2Metaped("late", entity.id, state, 0);
                    continue;
                }
                if (!(state.flags & replay::metaped::kHasExpressions))
                {
                    probe.pending = false;
                    continue;
                }
                static std::vector<replay::metaped::Expression> second;
                static std::vector<replay::metaped::Expression> stable;
                std::uint32_t notSet = 0;
                if (replay::metaped::engine::ReadExpressions(entity.handle, second, notSet) !=
                    replay::metaped::engine::ReadResult::Ok)
                {
                    ++g_poseV2MetapedStats.readFailures;
                    if (nowMs - probe.firstMs > kPoseV2MetapedGiveUpMs)
                        probe.pending = false; // the single read stays
                    continue;
                }
                replay::metaped::StableExpressions(probe.first, second, kPoseV2MetapedStableEps, stable);
                const std::size_t dropped = probe.first.size() - stable.size();
                g_poseV2MetapedStats.unstableDropped += dropped;
                state.expressions = stable;
                probe.pending = false;
                probe.first.clear();
                ++g_poseV2MetapedStats.confirms;
                LogPoseV2Metaped("confirm", entity.id, state, dropped);
                continue;
            }
            if (nowMs - probe.lastCheckMs < kPoseV2MetapedCheckMs)
                continue;
            probe.lastCheckMs = nowMs;
            const replay::metaped::State& last = track->states.back();
            if (!(last.flags & replay::metaped::kHasExpressions) || last.expressions.empty())
                continue;
            ++reads;
            static std::vector<replay::metaped::Expression> now;
            std::uint32_t notSet = 0;
            if (replay::metaped::engine::ReadExpressions(entity.handle, now, notSet) !=
                replay::metaped::engine::ReadResult::Ok)
            {
                ++g_poseV2MetapedStats.readFailures;
                continue;
            }
            const replay::metaped::Diff diff = replay::metaped::Compare(last.expressions, now, kPoseV2MetapedChangeEps);
            if (diff.differ == 0)
            {
                probe.candidate = false;
                continue;
            }
            // The ped's values for the recorded ids: the change's print.
            static std::vector<replay::metaped::Expression> mine;
            mine.clear();
            for (const replay::metaped::Expression& e : last.expressions)
                if (const replay::metaped::Expression* got = replay::metaped::FindExpression(now, e.id))
                    mine.push_back(*got);
            const std::uint64_t print = replay::metaped::ExpressionsPrint(mine);
            if (!probe.candidate || probe.candidatePrint != print)
            {
                probe.candidate = true;
                probe.candidatePrint = print;
                probe.candidateMs = nowMs;
                continue;
            }
            ++g_poseV2MetapedStats.changes;
            replay::entity::Appearance layers;
            replay::entity::appearance::Capture(entity.handle, layers);
            RecordPoseV2Metaped(entity.handle, entity.id, replay::metaped::Source::Change, probe.candidateMs, layers);
        }
    }

    void RejectPoseV2Entity(Ped ped, const char* reason, const std::string& error)
    {
        Ctx().recorder.rejected.push_back(ped);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_ENTITY_REJECT"
            << ",handle=" << ped
            << ",reason=" << reason
            << ",error=" << (error.empty() ? "none" : error.c_str())
            << '\n';
        log.flush();
    }

    void RegisterPoseV2Entity(Entity ped, bool vehicle)
    {
        REPLAY_PROFILE_ZONE("rec.register");
        if (!ped || !replay::entities::Exists(ped))
            return;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.handle == ped)
                return;
        }
        if (std::find(Ctx().recorder.rejected.begin(), Ctx().recorder.rejected.end(), ped) !=
            Ctx().recorder.rejected.end())
        {
            return;
        }
        if (Ctx().recorder.entities.size() >= replay::pose::capture::kMaxLiveTargets)
            return;

        replay::skeleton::ResolvedPoseSkeleton resolved{};
        if (!replay::skeleton::FastResolvePedPoseSkeleton(ped, resolved))
        {
            // A ped spawned a moment ago may have no skeleton yet;
            // with the ring running (and the eviction rule) it is offered a
            // place at once, so it is retried a few times, a second apart,
            // before the rejection sticks.
            auto tries = std::find_if(g_unresolvedTries.begin(), g_unresolvedTries.end(),
                [&](const std::pair<Entity, std::uint32_t>& entry) { return entry.first == ped; });
            if (tries == g_unresolvedTries.end())
            {
                g_unresolvedTries.push_back({ped, 0});
                tries = g_unresolvedTries.end() - 1;
            }
            if (++tries->second < kUnresolvedTries)
            {
                std::vector<replay::recorder::IgnoredHandle>& ignored = Ctx().recorder.ignored;
                const ULONGLONG now = GetTickCount64();
                ignored.erase(std::remove_if(ignored.begin(), ignored.end(),
                                  [&](const replay::recorder::IgnoredHandle& entry) { return entry.until <= now; }),
                    ignored.end());
                ignored.push_back({ped, now + 1000});
                if (g_unresolvedRetryLogs < 16)
                {
                    ++g_unresolvedRetryLogs;
                    Ctx().log << "POSE_V2_ENTITY_RETRY,handle=" << ped << ",reason=skeleton_unresolved,try="
                              << tries->second << "/" << kUnresolvedTries << '\n';
                }
                return;
            }
            RejectPoseV2Entity(ped, vehicle ? "vehicle_skeleton_unresolved" : "skeleton_unresolved",
                replay::skeleton::DescribePoseSkeletonWalk(ped));
            return;
        }
        const replay::entity::ReplayId id = Ctx().clip.ids.Allocate();
        if (!id.Valid())
        {
            RejectPoseV2Entity(ped, "no_free_id", {});
            return;
        }
        std::string error;
        bool started = false;
        {
            REPLAY_PROFILE_ZONE("rec.register.capture_start");
            started = replay::pose::capture::Start(
                id.Packed(), resolved.skeleton, resolved.boneCount, replay::recorder::kSampleHz, error);
        }
        if (!started)
        {
            Ctx().clip.ids.Release(id);
            RejectPoseV2Entity(ped, "capture_start_failed", error);
            return;
        }

        const Hash model = ENTITY::GET_ENTITY_MODEL(ped);
        replay::entity::CreatePacket packet;
        packet.id = id;
        packet.timeMs = replay::pose::capture::NowMs();
        packet.kind = vehicle
            ? replay::entity::EntityKind::Vehicle
            : PED::_IS_THIS_MODEL_A_HORSE(model)
                ? replay::entity::EntityKind::Horse
                : replay::entity::EntityKind::Ped;
        packet.modelHash = model;
        packet.sourceHandle = ped;
        packet.boneCount = resolved.boneCount;
        if (vehicle)
        {
            packet.appearance.vehicleTint = VEHICLE::_GET_VEHICLE_TINT(ped);
            packet.appearance.vehicleLivery = VEHICLE::_GET_VEHICLE_LIVERY(ped);
            CaptureVehicleLook(ped, id);
        }
        else
        {
            REPLAY_PROFILE_ZONE("rec.register.appearance");
            replay::entity::appearance::Capture(ped, packet.appearance);
        }
        if (!vehicle) // the whole metaped look at the entry
            RecordPoseV2Metaped(ped, id, replay::metaped::Source::Create, packet.timeMs, packet.appearance);
        Ctx().clip.lifecycle.Create(packet);
        Ctx().clip.created.push_back(packet);
        Ctx().recorder.entities.push_back({ped, id, resolved.skeleton, vehicle, id.Packed(), resolved.boneCount});
        // A hat dropped before the ped entered the recording is not part of
        // it; only the ones that come off from now on are (a dropped hat's
        // layers read empty, and without the owner re-dropping it the playback
        // would show the bare grey model).
        if (!vehicle)
            Ctx().recorder.entities.back().lastHat = PED::_GET_PED_LAST_DROPPED_HAT(ped);

        std::ostream& log = Ctx().log;
        log << "POSE_V2_ENTITY_CREATE"
            << ",id=" << replay::entity::IdText(id)
            << ",kind=" << replay::entity::KindText(packet.kind)
            << ",handle=" << ped
            << ",model=0x" << std::hex << model << std::dec
            << ",bones=" << resolved.boneCount
            << ",outfit=0x" << std::hex << packet.appearance.outfit << std::dec
            << ",layers=" << packet.appearance.assets.size()
            << ",time_ms=" << packet.timeMs
            << ",hz=" << replay::recorder::kSampleHz
            << ",fragment_path=" << (resolved.usedFragmentPath ? 1 : 0)
            << ",engine_writes=0"
            << '\n';
        log.flush();
    }


    // Rope tracks live in src/replay_ropes (the clip holds them); the
    // recorder only maps engine entity addresses to the replay ids it
    // registered (peds, weapons, props).

    struct PoseV2RopeEntityIndex final : replay::ropes::EntityIndex
    {
        // Built on the first lookup: most frames have no new rope
        // and never ask, and the handle-to-address walk costs a lookup per
        // recorded entity, weapon and prop.
        mutable std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> known;
        mutable bool built = false;

        void Build() const
        {
            if (built)
                return;
            built = true;
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                known.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)), entity.id});
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                known.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), weapon.id});
            for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                known.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(prop.object)), prop.id});
        }

        replay::entity::ReplayId IdOf(std::uint64_t address) const override
        {
            Build();
            for (const auto& entry : known)
            {
                if (address && entry.first == address)
                    return entry.second;
            }
            return replay::entity::ReplayId{};
        }

        // Recorded ped positions, read once per frame on the first lookup:
        // every rope the recorder does not follow yet (world ropes, lassos
        // on the saddle) asks again each frame, and three natives per
        // recorded entity per rope grew with the crowd.
        mutable std::vector<std::pair<replay::entity::ReplayId, Vector3>> pedPositions;
        mutable bool positionsBuilt = false;

        replay::entity::ReplayId NearestRecordedPed(const float xyz[3], float radius) const override
        {
            if (!positionsBuilt)
            {
                positionsBuilt = true;
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                {
                    if (!replay::entities::Exists(entity.handle) || !replay::entities::IsPed(entity.handle))
                        continue;
                    pedPositions.push_back({entity.id, ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, FALSE)});
                }
            }
            replay::entity::ReplayId nearest;
            float best = radius * radius;
            for (const auto& [id, at] : pedPositions)
            {
                const float dx = at.x - xyz[0];
                const float dy = at.y - xyz[1];
                const float dz = at.z - xyz[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= best)
                {
                    best = d2;
                    nearest = id;
                }
            }
            return nearest;
        }

        // The fishing bobber and any other unknown object at an
        // end of a recorded rope becomes a prop (replay_objects).
        replay::entity::ReplayId AdoptEndObject(std::uint64_t address, replay::entity::ReplayId owner,
            const char*& status) const override
        {
            const replay::entity::ReplayId id = replay::objects::RegisterRopeEndObject(address, owner, status);
            if (id.Valid())
            {
                Build();
                known.push_back({address, id});
            }
            return id;
        }

        // The player and the rendered camera, read once per frame on the first
        // rope that asks.
        std::size_t Focus(float out[2][3]) const override
        {
            std::size_t count = 0;
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (player && ENTITY::DOES_ENTITY_EXIST(player))
            {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                out[count][0] = at.x;
                out[count][1] = at.y;
                out[count][2] = at.z;
                ++count;
            }
            const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
            if (camera.x != 0.0f || camera.y != 0.0f || camera.z != 0.0f)
            {
                out[count][0] = camera.x;
                out[count][1] = camera.y;
                out[count][2] = camera.z;
                ++count;
            }
            return count;
        }

        bool HandOf(replay::entity::ReplayId id, float out[3]) const override
        {
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.id != id || !replay::entities::Exists(entity.handle))
                    continue;
                const int bone = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(entity.handle, "SKEL_R_Hand");
                if (bone < 0)
                    return false;
                const Vector3 at = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(entity.handle, bone);
                out[0] = at.x;
                out[1] = at.y;
                out[2] = at.z;
                return true;
            }
            return false;
        }
    };

    void SamplePoseV2Ropes()
    {
        REPLAY_PROFILE_ZONE("rec.ropes");
        const PoseV2RopeEntityIndex index;
        Ctx().clip.ropes.Sample(index, replay::pose::capture::NowMs(), kIntervalMs, Ctx().log);
    }


    void RetirePoseV2Entity(std::size_t index, const char* reason)
    {
        const replay::recorder::RecordedEntity entity = Ctx().recorder.entities[index];
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        replay::pose::capture::Stop(entity.recordId);
        Ctx().clip.lifecycle.Delete(entity.id, timeMs);
        Ctx().clip.ids.Release(entity.id);
        Ctx().recorder.entities.erase(Ctx().recorder.entities.begin() + static_cast<std::ptrdiff_t>(index));
        replay::objects::RetireWeaponsOf(entity.handle, "parent_gone");

        std::ostream& log = Ctx().log;
        log << "POSE_V2_ENTITY_DELETE"
            << ",id=" << replay::entity::IdText(entity.id)
            << ",handle=" << entity.handle
            << ",reason=" << reason
            << ",time_ms=" << timeMs
            << '\n';
        log.flush();
    }

    // Script-thread cost of the registry, reported in POSE_V2_PERF.
    struct PoseV2ScriptTiming
    {
        std::uint64_t updates = 0;
        double updateTotalUs = 0.0;
        double updateMaxUs = 0.0;
        std::uint64_t scans = 0;
        double scanTotalUs = 0.0;
        double scanMaxUs = 0.0;
        std::uint64_t resolves = 0;
        std::uint64_t checks = 0;
        // Scan breakdown.
        double checkTotalUs = 0.0;
        double poolTotalUs = 0.0;
        double registerTotalUs = 0.0;
        std::uint64_t poolPeds = 0;
    };
    PoseV2ScriptTiming g_poseV2ScriptTiming;

    // Candidates come from a scan of the ped pool (people and animals) at
    // kPoseV2ScanIntervalMs; the player and its current mount are pinned.
    // replay::entity::SelectEntities decides who enters and leaves.
    constexpr ULONGLONG kPoseV2ScanIntervalMs = 200;
    ULONGLONG g_poseV2NextScanAt = 0;
    // A fragment break (a blast breaking a wheel, a crate) makes its pieces in
    // that frame: the scan runs every tick for a while after one, so the
    // pieces are recorded from the frame they appear (at 200 ms they began up
    // to 200 ms late, the first frames of the blast without them).
    constexpr ULONGLONG kPoseV2BreakScanMs = 500;
    std::uint64_t g_poseV2BreaksSeen = 0;
    ULONGLONG g_poseV2BreakScanUntil = 0;

    struct PoseV2Candidates
    {
        std::vector<replay::entity::Candidate> peds;
        std::vector<replay::entity::Candidate> vehicles;
    };

    // Entities that belong with a recorded one get the same priority as the
    // player: the draft animals and occupants of a recorded vehicle and the
    // mount of a recorded rider.
    void CollectPoseV2Dependents(std::vector<Entity>& dependents)
    {
        dependents.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!replay::entities::Exists(entity.handle))
                continue;
            if (entity.vehicle)
            {
                for (int harness = 0; harness < 6; ++harness)
                {
                    if (const Ped animal = VEHICLE::_GET_PED_IN_DRAFT_HARNESS(entity.handle, harness))
                        dependents.push_back(animal);
                }
                const int seats = (std::min)(
                    VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(entity.handle), 16);
                for (int seat = -1; seat < seats; ++seat)
                {
                    if (const Ped occupant = VEHICLE::GET_PED_IN_VEHICLE_SEAT(entity.handle, seat))
                        dependents.push_back(occupant);
                }
            }
            else if (PED::IS_PED_ON_MOUNT(entity.handle))
            {
                dependents.push_back(PED::GET_MOUNT(entity.handle));
            }
        }
    }

    void CollectPoseV2Pool(
        const int* handles,
        int count,
        const Vector3& origin,
        const Vector3& cameraOrigin,
        Entity pinnedA,
        Entity pinnedB,
        const std::vector<Entity>& dependents,
        const std::vector<Entity>& locked,
        std::vector<replay::entity::Candidate>& out)
    {
        out.clear();
        for (int index = 0; index < count; ++index)
        {
            const Entity entity = handles[index];
            if (!entity || !replay::entities::Exists(entity) ||
                Ctx().recorder.IsIgnored(entity, GetTickCount64()) ||
                std::find(Ctx().recorder.rejected.begin(), Ctx().recorder.rejected.end(), entity) !=
                    Ctx().recorder.rejected.end())
            {
                continue;
            }
            const Vector3 position = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
            // The nearer of the player and the rendered camera, so
            // what the camera looks at stays in the recording when the
            // player walks away (the capture radius bench).
            // Horizontal, the height only beyond a slack
            // (entity_selection.h SelectionDistance).
            replay::entity::Candidate candidate;
            candidate.handle = entity;
            candidate.distance = (std::min)(
                replay::entity::SelectionDistance(
                    position.x - origin.x, position.y - origin.y, position.z - origin.z),
                replay::entity::SelectionDistance(
                    position.x - cameraOrigin.x, position.y - cameraOrigin.y, position.z - cameraOrigin.z));
            candidate.pinned = (pinnedA && entity == pinnedA) || (pinnedB && entity == pinnedB) ||
                std::find(dependents.begin(), dependents.end(), entity) != dependents.end();
            candidate.locked = std::find(locked.begin(), locked.end(), entity) != locked.end();
            out.push_back(candidate);
        }
    }

    // Recorded entities a recorded relation holds (either end of
    // a draft harness or vehicle seat attachment, an entity on a rope
    // sampled in the last second): the selection never evicts them for a
    // nearer newcomer, so no attachment or rope loses an end mid-clip.
    void CollectPoseV2Locked(std::vector<Entity>& locked)
    {
        locked.clear();
        std::vector<replay::entity::ReplayId> ids;
        for (const replay::entity::Attachment& attachment : Ctx().clip.lifecycle.LiveAttachments())
        {
            if (attachment.kind == replay::entity::AttachmentKind::WeaponPoint)
                continue; // a weapon follows its holder out (RetireWeaponsOf)
            ids.push_back(attachment.child);
            ids.push_back(attachment.parent);
        }
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::ropes::RopeRecord& record : Ctx().clip.ropes.Records())
        {
            if (record.times.empty() || record.times.back() + 1000u < nowMs)
                continue;
            for (const replay::ropes::RopeSlotEvent& slot : record.slotState)
            {
                if (slot.holder == replay::ropes::RopeHolder::Recorded && slot.id.Valid())
                    ids.push_back(slot.id);
            }
            if (record.nearPed.Valid())
                ids.push_back(record.nearPed);
        }
        // The owner of an object that holds a pose place or waits for one
        // (object_pose.h ReservedPlaces) is never evicted to make room: the
        // eviction would retire the very ped the object belongs to (a weapon
        // follows its holder out). A weapon names its ped; a prop (the posed
        // object that left the hand) names its owner in its create packet.
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (weapon.parent && (weapon.posed || (weapon.poseCandidate && weapon.poseNoPlace)))
                locked.push_back(weapon.parent);
        }
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
        {
            if (!prop.posed && !(prop.poseCandidate && prop.poseNoPlace))
                continue;
            for (const replay::entity::CreatePacket& packet : Ctx().clip.lifecycle.Live())
            {
                if (packet.id == prop.id)
                {
                    if (packet.owner.Valid())
                        ids.push_back(packet.owner);
                    break;
                }
            }
        }
        if (ids.empty())
            return;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (std::find(ids.begin(), ids.end(), entity.id) != ids.end())
                locked.push_back(entity.handle);
        }
    }

    // Entity discovery without the pool listing (entity_discovery.h): per
    // recording, for POSE_V2_DISCOVERY.
    struct PoseV2DiscoveryStats
    {
        std::uint32_t scans = 0;
        std::uint32_t pedListingEmpty = 0;
        std::uint32_t vehicleListingEmpty = 0;
        std::uint32_t nearbyCalls = 0;
        std::uint32_t nearbyPeds = 0;       // peds the nearby walk added
        std::uint32_t nearbyVehicles = 0;   // vehicles the nearby lists added
        std::uint32_t budgetSpent = 0;      // walks stopped by the call budget
        std::uint32_t keptRecorded = 0;     // recorded entities added back as candidates
        std::uint32_t dependentsAdded = 0;  // draft animals, occupants and mounts missing from the candidates
        std::uint32_t lateEntries = 0;      // registered after the opening census within 40 m of the camera
        // Recorded entities after each scan (the bench's per-vehicle cost).
        std::uint64_t recordedPedsSum = 0;
        std::uint64_t recordedVehiclesSum = 0;
        std::uint32_t recordedVehiclesMax = 0;
        std::uint32_t lateLogs = 0;
        std::uint32_t flipLogs = 0;
        bool lastPedListingEmpty = false;
        std::uint32_t lastListingOkMs = 0;
        // The pool reader (entity_pools.h), the primary source.
        std::uint32_t poolScans = 0;        // scans whose candidates came from the pools
        std::uint32_t poolFallbacks = 0;    // scans that fell back to the listing
        std::uint64_t poolPedsLive = 0;     // sums, for the averages
        std::uint64_t poolPedCandidates = 0;
        std::uint64_t poolVehiclesLive = 0;
        std::uint64_t poolVehicleCandidates = 0;
        std::uint32_t poolPedsLiveMax = 0;
        std::uint32_t poolCreated = 0;      // script handles made for new candidates
        std::uint32_t poolFailed = 0;       // candidates left without a handle
        std::uint32_t poolSkippedBudget = 0;
        std::uint32_t guidFreeMin = 0xFFFFFFFFu;
        std::uint32_t guidSize = 0;
        double poolUsTotal = 0.0;
        double poolUsMax = 0.0;
        std::uint32_t scanLogs = 0;
        bool unavailableLogged = false;
        // This scan's (logged after the selection).
        replay::pools::ListStats lastPeds;
        replay::pools::ListStats lastVehicles;
        bool lastFromPool = false;
    };
    PoseV2DiscoveryStats g_poseV2Discovery;
    // New script handles per scan (the nearest first): the
    // capacity and a margin; the ones with a handle cost nothing.
    // From the session's PedCapacity.
    int PoseV2PoolPedBudget() { return replay::capacity::PoolPedBudget(replay::capacity::PedCapacity()); }
    int PoseV2PoolVehicleBudget() { return replay::capacity::PoolVehicleBudget(replay::capacity::VehicleCapacity()); }
    constexpr std::uint32_t kPoseV2ScanLogCap = 80;

    // The primary source, the engine's pools read directly (entity_pools.h):
    // every live entity is seen, and a script handle is made only for the
    // nearest ones inside the selection's enter radius (those with a handle
    // count up to the exit radius and a margin). -1 when the reader is not
    // available (signature mismatch or EntityPoolReader=0): the listing and
    // the nearby walk below take over.
    int ListPoseV2Pool(replay::pools::Kind kind, const Vector3& origin, const Vector3& cameraOrigin, int createBudget,
        int* handles, replay::pools::ListStats& stats)
    {
        REPLAY_PROFILE_ZONE("rec.scan_pools");
        const replay::pools::Position centers[2] = {
            {origin.x, origin.y, origin.z}, {cameraOrigin.x, cameraOrigin.y, cameraOrigin.z}};
        const replay::entity::SelectionConfig config;
        const int count = replay::pools::ListHandles(kind, centers, 2, config.enterRadius, config.exitRadius + 25.0f,
            createBudget, handles, replay::recorder::kPoolCapacity, &stats);
        if (count < 0 && !g_poseV2Discovery.unavailableLogged)
        {
            g_poseV2Discovery.unavailableLogged = true;
            std::string error;
            replay::pools::Check(error);
            Ctx().log << "POSE_V2_DISCOVERY,phase=pool_unavailable,enabled=" << (replay::pools::Enabled() ? 1 : 0)
                      << ",error=" << (error.empty() ? "none" : error.c_str()) << ",fallback=listing,"
                      << replay::pools::Describe() << '\n';
        }
        return count;
    }

    void NotePoseV2PoolScan(const replay::pools::ListStats& peds, const replay::pools::ListStats& vehicles)
    {
        PoseV2DiscoveryStats& d = g_poseV2Discovery;
        ++d.poolScans;
        d.poolPedsLive += peds.live;
        d.poolPedCandidates += peds.listed;
        d.poolVehiclesLive += vehicles.live;
        d.poolVehicleCandidates += vehicles.listed;
        d.poolPedsLiveMax = (std::max)(d.poolPedsLiveMax, peds.live);
        d.poolCreated += peds.created + vehicles.created;
        d.poolFailed += peds.failed + vehicles.failed;
        d.poolSkippedBudget += peds.skippedBudget + vehicles.skippedBudget;
        d.guidFreeMin = (std::min)(d.guidFreeMin, (std::min)(peds.guidFree, vehicles.guidFree));
        d.guidSize = peds.guidSize;
        const double us = peds.us + vehicles.us;
        d.poolUsTotal += us;
        d.poolUsMax = (std::max)(d.poolUsMax, us);
    }
    constexpr std::uint32_t kPoseV2NearbyPedCalls = 64;     // per scan (200 ms)
    constexpr std::uint32_t kPoseV2NearbyVehicleCalls = 24; // per scan

    int PoseV2NearbyList(Entity seed, std::int32_t* out, int capacity, bool vehicles)
    {
        if (!seed || !replay::entities::Exists(seed) || !replay::entities::IsPed(seed))
            return 0;
        constexpr int kSlots = 64;
        Any list[1 + kSlots]{};
        list[0] = kSlots;
        const int found = vehicles ? PED::GET_PED_NEARBY_VEHICLES(seed, list) : PED::GET_PED_NEARBY_PEDS(seed, list, -1, -1);
        int count = 0;
        for (int n = 0; n < found && n < kSlots && count < capacity; ++n)
        {
            const Entity entity = static_cast<Entity>(list[1 + n]);
            if (entity && replay::entities::Exists(entity))
                out[count++] = entity;
        }
        return count;
    }

    // The player and its mount are pinned peds; the vehicle it is in is a
    // pinned vehicle.
    // When a listing comes back empty (the town: the script handle
    // pool near full, entity_discovery.h) the candidates come from the
    // engine's nearby lists, spreading from the player and the recorded
    // peds; the recorded entities and their dependents are candidates in
    // any case, so a listing of 0 no longer leaves a recorded wagon's horses
    // or a rider's mount out.
    void CollectPoseV2Candidates(Ped mount, PoseV2Candidates& candidates)
    {
        const Ped player = PLAYER::PLAYER_PED_ID();
        const Vector3 origin = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        const Vector3 cameraOrigin = CAM::GET_FINAL_RENDERED_CAM_COORD();
        static int handles[replay::recorder::kPoolCapacity]{};
        static std::vector<Entity> dependents;
        static std::vector<Entity> locked;
        static std::vector<std::int32_t> seeds;
        static std::vector<std::int32_t> recordedPeds;
        static std::vector<std::int32_t> recordedVehicles;
        CollectPoseV2Dependents(dependents);
        CollectPoseV2Locked(locked);
        // On foot, the player's last mount is no longer pinned; it is locked
        // instead, so the eviction never retires the horse the player just got
        // off (and will ride again).
        if (!mount)
        {
            const Ped lastMount = PED::_GET_LAST_MOUNT(player);
            if (lastMount && lastMount != player)
                locked.push_back(lastMount);
        }
        recordedPeds.clear();
        recordedVehicles.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            (entity.vehicle ? recordedVehicles : recordedPeds).push_back(entity.handle);
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        ++g_poseV2Discovery.scans;
        // The pools first (ListPoseV2Pool); the listing and the
        // nearby walk only when the reader is unavailable.
        replay::pools::ListStats pedPool;
        replay::pools::ListStats vehiclePool;
        int count = ListPoseV2Pool(replay::pools::Kind::Ped, origin, cameraOrigin, PoseV2PoolPedBudget(), handles, pedPool);
        const bool fromPool = count >= 0;
        g_poseV2Discovery.lastFromPool = fromPool;
        if (fromPool)
        {
            // Recorded peds beyond the radius stay candidates (the exit
            // radius retires them).
            g_poseV2Discovery.keptRecorded += replay::entity::AddMissing(recordedPeds, handles, count, replay::recorder::kPoolCapacity);
        }
        else
        {
            ++g_poseV2Discovery.poolFallbacks;
            count = (std::min)((std::max)(0, replay::objects::ListPeds(handles, replay::recorder::kPoolCapacity)),
                replay::recorder::kPoolCapacity);
        }
        const bool pedListingEmpty = !fromPool && count == 0;
        if (pedListingEmpty)
        {
            ++g_poseV2Discovery.pedListingEmpty;
            seeds.clear();
            seeds.push_back(player);
            seeds.push_back(mount);
            seeds.insert(seeds.end(), recordedPeds.begin(), recordedPeds.end());
            seeds.insert(seeds.end(), dependents.begin(), dependents.end());
            const replay::entity::NearbyWalkStats walk = replay::entity::NearbyWalk(seeds,
                [](std::int32_t seed, std::int32_t* out, int capacity) { return PoseV2NearbyList(seed, out, capacity, false); },
                handles, count, replay::recorder::kPoolCapacity, kPoseV2NearbyPedCalls);
            g_poseV2Discovery.nearbyCalls += walk.calls;
            g_poseV2Discovery.nearbyPeds += walk.found;
            g_poseV2Discovery.budgetSpent += walk.budgetSpent ? 1 : 0;
            g_poseV2Discovery.keptRecorded += replay::entity::AddMissing(recordedPeds, handles, count, replay::recorder::kPoolCapacity);
        }
        else
        {
            g_poseV2Discovery.lastListingOkMs = nowMs;
        }
        if (!fromPool && pedListingEmpty != g_poseV2Discovery.lastPedListingEmpty && g_poseV2Discovery.flipLogs < 32)
        {
            // When the listing stops and comes back (the handle pool filling
            // and emptying): the town's pattern.
            ++g_poseV2Discovery.flipLogs;
            Ctx().log << "POSE_V2_DISCOVERY,phase=" << (pedListingEmpty ? "listing_empty" : "listing_back")
                      << ",candidates=" << count << ",recorded=" << Ctx().recorder.entities.size()
                      << ",time_ms=" << nowMs << '\n';
        }
        g_poseV2Discovery.lastPedListingEmpty = pedListingEmpty;
        g_poseV2Discovery.dependentsAdded +=
            replay::entity::AddMissing(std::vector<std::int32_t>(dependents.begin(), dependents.end()), handles, count,
                replay::recorder::kPoolCapacity);
        CollectPoseV2Pool(handles, count, origin, cameraOrigin, player, mount, dependents, locked, candidates.peds);
        const bool playerListed = std::any_of(candidates.peds.begin(), candidates.peds.end(),
            [&](const replay::entity::Candidate& candidate) { return candidate.handle == player; });
        if (!playerListed)
            candidates.peds.push_back({player, 0.0f, true});

        const Vehicle playerVehicle = PED::IS_PED_IN_ANY_VEHICLE(player, FALSE)
            ? PED::GET_VEHICLE_PED_IS_IN(player, FALSE)
            : 0;
        count = fromPool
            ? ListPoseV2Pool(replay::pools::Kind::Vehicle, origin, cameraOrigin, PoseV2PoolVehicleBudget(), handles, vehiclePool)
            : -1;
        const bool vehiclesFromPool = count >= 0;
        if (vehiclesFromPool)
        {
            g_poseV2Discovery.keptRecorded +=
                replay::entity::AddMissing(recordedVehicles, handles, count, replay::recorder::kPoolCapacity);
            if (playerVehicle)
                replay::entity::AddMissing({playerVehicle}, handles, count, replay::recorder::kPoolCapacity);
        }
        else
        {
            count = (std::min)((std::max)(0, replay::objects::ListVehicles(handles, replay::recorder::kPoolCapacity)),
                replay::recorder::kPoolCapacity);
        }
        if (count == 0 && !vehiclesFromPool)
        {
            ++g_poseV2Discovery.vehicleListingEmpty;
            // The vehicles near the player and the ped candidates (recorded
            // first): no spreading, a vehicle has no nearby list.
            seeds.clear();
            seeds.push_back(player);
            seeds.insert(seeds.end(), recordedPeds.begin(), recordedPeds.end());
            for (const replay::entity::Candidate& candidate : candidates.peds)
                seeds.push_back(candidate.handle);
            std::uint32_t calls = 0;
            static std::int32_t list[64]{};
            for (std::size_t n = 0; n < seeds.size() && calls < kPoseV2NearbyVehicleCalls; ++n)
            {
                if (!seeds[n] || std::find(seeds.begin(), seeds.begin() + static_cast<std::ptrdiff_t>(n), seeds[n]) !=
                                     seeds.begin() + static_cast<std::ptrdiff_t>(n))
                    continue;
                ++calls;
                const int got = PoseV2NearbyList(seeds[n], list, 64, true);
                g_poseV2Discovery.nearbyVehicles += replay::entity::AddMissing(
                    std::vector<std::int32_t>(list, list + got), handles, count, replay::recorder::kPoolCapacity);
            }
            g_poseV2Discovery.nearbyCalls += calls;
            g_poseV2Discovery.keptRecorded += replay::entity::AddMissing(recordedVehicles, handles, count, replay::recorder::kPoolCapacity);
            if (playerVehicle)
                replay::entity::AddMissing({playerVehicle}, handles, count, replay::recorder::kPoolCapacity);
        }
        CollectPoseV2Pool(handles, count, origin, cameraOrigin, playerVehicle, 0, dependents, locked, candidates.vehicles);
        if (fromPool)
        {
            NotePoseV2PoolScan(pedPool, vehiclePool);
            g_poseV2Discovery.lastPeds = pedPool;
            g_poseV2Discovery.lastVehicles = vehiclePool;
        }
    }

    // One line per scan (capped: the first 20, then one every 25
    // scans, 5 s): where the candidates came from and what was recorded.
    void LogPoseV2DiscoveryScan(std::size_t pedCandidates, std::size_t vehicleCandidates)
    {
        PoseV2DiscoveryStats& d = g_poseV2Discovery;
        std::uint32_t recordedPeds = 0;
        std::uint32_t recordedVehicles = 0;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            ++(entity.vehicle ? recordedVehicles : recordedPeds);
        d.recordedPedsSum += recordedPeds;
        d.recordedVehiclesSum += recordedVehicles;
        d.recordedVehiclesMax = (std::max)(d.recordedVehiclesMax, recordedVehicles);
        if (d.scanLogs >= kPoseV2ScanLogCap || (d.scans > 20 && d.scans % 25 != 0))
            return;
        ++d.scanLogs;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_DISCOVERY,phase=scan,source=" << (d.lastFromPool ? "pool" : "listing") << ",scan=" << d.scans;
        if (d.lastFromPool)
        {
            log << ",peds_live=" << d.lastPeds.live << ",peds_handled=" << d.lastPeds.listed
                << ",peds_created=" << d.lastPeds.created << ",peds_far=" << d.lastPeds.skippedFar
                << ",vehicles_live=" << d.lastVehicles.live << ",vehicles_handled=" << d.lastVehicles.listed
                << ",vehicles_created=" << d.lastVehicles.created << ",guid_free=" << d.lastPeds.guidFree << "/"
                << d.lastPeds.guidSize << ",pool_us=" << static_cast<int>(d.lastPeds.us + d.lastVehicles.us);
        }
        log << ",ped_candidates=" << pedCandidates << ",vehicle_candidates=" << vehicleCandidates
            << ",recorded_peds=" << recordedPeds << ",recorded_vehicles=" << recordedVehicles
            << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
    }

    // The dependents of the vehicles this scan just registered
    // (vehicles are selected first now): pinned in the same scan. Before,
    // peds went first and a wagon entering the recording with a full ped
    // capacity left its horses to a later scan, which in town never listed
    // them (a town clip: wagon 76:4513 recorded at 48.8 s, its two
    // horses only at 100.0 s, a wagon without horses for 51 s).
    void PinPoseV2NewDependents(std::vector<replay::entity::Candidate>& peds)
    {
        static std::vector<Entity> dependents;
        CollectPoseV2Dependents(dependents);
        if (dependents.empty())
            return;
        const Vector3 origin = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        for (const Entity dependent : dependents)
        {
            auto found = std::find_if(peds.begin(), peds.end(),
                [&](const replay::entity::Candidate& candidate) { return candidate.handle == dependent; });
            if (found != peds.end())
            {
                found->pinned = true;
                continue;
            }
            if (!replay::entities::Exists(dependent) || Ctx().recorder.IsIgnored(dependent, GetTickCount64()) ||
                std::find(Ctx().recorder.rejected.begin(), Ctx().recorder.rejected.end(), dependent) !=
                    Ctx().recorder.rejected.end())
                continue;
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(dependent, TRUE, FALSE);
            replay::entity::Candidate candidate;
            candidate.handle = dependent;
            candidate.distance = replay::entity::SelectionDistance(at.x - origin.x, at.y - origin.y, at.z - origin.z);
            candidate.pinned = true;
            peds.push_back(candidate);
            ++g_poseV2Discovery.dependentsAdded;
        }
    }

    // Selection evidence, capped per recording (these lines are
    // not in the buffer's quiet list: the ring is where the capacity fills).
    struct PoseV2SelectionStats
    {
        std::uint32_t evicted = 0;
        std::uint32_t skippedScans = 0;  // scans that left a newcomer out for lack of a place
        std::uint32_t skippedPeak = 0;   // most newcomers left out in one scan
        std::uint32_t evictLogs = 0;
        std::uint32_t fullLogs = 0;
        std::uint32_t reentries = 0;
        std::uint32_t reentryLogs = 0;
    };
    PoseV2SelectionStats g_poseV2Selection;
    // Handles the selection retired, for the re-entry evidence.
    replay::entity::RecentRetirements g_poseV2Retired;
    constexpr std::uint32_t kPoseV2ReentryLogCap = 24;
    constexpr std::uint32_t kPoseV2EvictLogCap = 48;
    constexpr std::uint32_t kPoseV2FullLogCap = 24;

    float CandidateDistance(const std::vector<replay::entity::Candidate>& candidates, std::int32_t handle)
    {
        for (const replay::entity::Candidate& candidate : candidates)
        {
            if (candidate.handle == handle)
                return candidate.distance;
        }
        return -1.0f;
    }

    void SelectPoseV2Class(
        const std::vector<replay::entity::Candidate>& candidates,
        bool vehicle,
        std::uint32_t capacity)
    {
        static std::vector<std::int32_t> recorded;
        static std::vector<std::int32_t> retire;
        static std::vector<std::int32_t> add;
        static replay::entity::SelectionReport report;
        recorded.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle == vehicle)
                recorded.push_back(entity.handle);
        }
        replay::entity::SelectionConfig config;
        config.capacity = capacity;
        replay::entity::SelectEntities(candidates, recorded, config, retire, add, &report);

        std::ostream& log = Ctx().log;
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        // Retire first: that frees capture places for the entities that enter.
        // An evicted entity's track ends cleanly (lifecycle delete, pose
        // capture stopped, its weapons retired) like one out of range.
        for (const std::int32_t handle : retire)
        {
            const bool evicted = std::find(report.evicted.begin(), report.evicted.end(), handle) != report.evicted.end();
            if (evicted)
            {
                ++g_poseV2Selection.evicted;
                if (g_poseV2Selection.evictLogs < kPoseV2EvictLogCap)
                {
                    ++g_poseV2Selection.evictLogs;
                    log << "POSE_V2_ENTITY_EVICT"
                        << ",kind=" << (vehicle ? "vehicle" : "ped")
                        << ",handle=" << handle
                        << ",distance_m=" << static_cast<int>(CandidateDistance(candidates, handle))
                        << ",capacity=" << capacity
                        << ",time_ms=" << timeMs
                        << ",logged=" << g_poseV2Selection.evictLogs << "/" << kPoseV2EvictLogCap
                        << '\n';
                }
            }
            for (std::size_t index = 0; index < Ctx().recorder.entities.size(); ++index)
            {
                if (Ctx().recorder.entities[index].handle == handle)
                {
                    const float retireZ = replay::entities::Exists(handle)
                        ? ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE).z
                        : 0.0f;
                    g_poseV2Retired.Note(handle, timeMs, retireZ, evicted);
                    RetirePoseV2Entity(index, evicted ? "evicted_for_nearer" : "out_of_range");
                    break;
                }
            }
        }
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        for (const std::int32_t handle : add)
        {
            const std::size_t before = Ctx().recorder.entities.size();
            RegisterPoseV2Entity(handle, vehicle);
            if (Ctx().recorder.entities.size() == before)
                continue;
            // The same engine entity back after the selection retired it (a new
            // replay id, a pop-in in the replay).
            replay::entity::RecentRetirements::Entry retired;
            if (g_poseV2Retired.Take(handle, timeMs, retired))
            {
                ++g_poseV2Selection.reentries;
                if (g_poseV2Selection.reentryLogs < kPoseV2ReentryLogCap)
                {
                    ++g_poseV2Selection.reentryLogs;
                    const Vector3 entryAt = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
                    log << "POSE_V2_REENTRY,id=" << replay::entity::IdText(Ctx().recorder.entities.back().id)
                        << ",kind=" << (vehicle ? "vehicle" : "ped") << ",handle=" << handle
                        << ",gap_ms=" << (timeMs - retired.timeMs)
                        << ",retired=" << (retired.evicted ? "evicted_for_nearer" : "out_of_range")
                        << ",retire_z=" << static_cast<int>(retired.z) << ",entry_z=" << static_cast<int>(entryAt.z)
                        << ",distance_m=" << static_cast<int>(CandidateDistance(candidates, handle))
                        << ",time_ms=" << timeMs
                        << ",logged=" << g_poseV2Selection.reentryLogs << "/" << kPoseV2ReentryLogCap << '\n';
                }
            }
            // An entity entering the recording close to the camera
            // after the opening census pops into the replay; in town it meant
            // the discovery had missed it (entity_discovery.h).
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
            const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
            const float cameraMeters = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!replay::entity::IsLateEntry(timeMs, cameraMeters))
                continue;
            ++g_poseV2Discovery.lateEntries;
            if (g_poseV2Discovery.lateLogs < 48)
            {
                ++g_poseV2Discovery.lateLogs;
                log << "POSE_V2_LATE_ENTRY,id=" << replay::entity::IdText(Ctx().recorder.entities.back().id)
                    << ",kind=" << (vehicle ? "vehicle" : "ped") << ",camera_m=" << static_cast<int>(cameraMeters)
                    << ",listing_empty=" << (g_poseV2Discovery.lastPedListingEmpty ? 1 : 0)
                    << ",listing_ok_ago_ms=" << (timeMs - g_poseV2Discovery.lastListingOkMs) << ",time_ms=" << timeMs
                    << '\n';
            }
        }

        if (!report.skippedFull.empty())
        {
            ++g_poseV2Selection.skippedScans;
            g_poseV2Selection.skippedPeak = (std::max)(g_poseV2Selection.skippedPeak,
                static_cast<std::uint32_t>(report.skippedFull.size()));
            if (g_poseV2Selection.fullLogs < kPoseV2FullLogCap)
            {
                ++g_poseV2Selection.fullLogs;
                const std::int32_t nearest = report.skippedFull.front(); // entering is nearest first
                log << "POSE_V2_SELECT_FULL"
                    << ",kind=" << (vehicle ? "vehicle" : "ped")
                    << ",skipped=" << report.skippedFull.size()
                    << ",nearest_handle=" << nearest
                    << ",nearest_m=" << static_cast<int>(CandidateDistance(candidates, nearest))
                    << ",evicted=" << report.evicted.size()
                    << ",capacity=" << capacity
                    << ",time_ms=" << timeMs
                    << ",logged=" << g_poseV2Selection.fullLogs << "/" << kPoseV2FullLogCap
                    << '\n';
            }
        }
    }

    // The visibility of every recorded ped and horse, each frame. A mission
    // hides peds (created hidden before they appear); without it the replay
    // would show them whenever it had a sample.
    void UpdatePoseV2Visibility()
    {
        if (replay::pose::capture::IsClockPaused())
            return;
        REPLAY_PROFILE_ZONE("rec.visibility");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        std::vector<replay::PedVisibility>& all = Ctx().clip.pedVisibility;
        // The rows by replay id (the ring prunes the rows of entities it no
        // longer keeps, replay_buffer.h PruneVisibilityRows).
        static replay::buffer::VisibilityIndex index;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !entity.handle || !replay::entities::Exists(entity.handle))
                continue;
            const bool visible = ENTITY::IS_ENTITY_VISIBLE(entity.handle) != FALSE;
            replay::PedVisibility* row = index.Find(all, entity.id);
            if (!row)
            {
                replay::PedVisibility fresh;
                fresh.id = entity.id;
                fresh.visible = visible ? 1 : 0;
                index.Add(all, fresh); // nothing at the cap (kMaxVisibilityRows)
                continue;
            }
            replay::objcheck::NoteVisibility(row->visible != 0, row->flipsMs, visible, timeMs);
        }
    }

    // Draft harness, seat and weapon state of the recorded entities, as
    // attach/detach packets between replay ids.
    void UpdatePoseV2Attachments()
    {
        REPLAY_PROFILE_ZONE("rec.attachments");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        const auto idOf = [](Entity handle) -> const replay::recorder::RecordedEntity* {
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.handle == handle)
                    return &entity;
            }
            return nullptr;
        };
        static std::vector<replay::entity::Attachment> desired;
        desired.clear();
        for (const replay::recorder::RecordedEntity& vehicle : Ctx().recorder.entities)
        {
            if (!vehicle.vehicle || !replay::entities::Exists(vehicle.handle))
                continue;
            for (int harness = 0; harness < 6; ++harness)
            {
                const Ped animal = VEHICLE::_GET_PED_IN_DRAFT_HARNESS(vehicle.handle, harness);
                if (const replay::recorder::RecordedEntity* child = animal ? idOf(animal) : nullptr)
                {
                    desired.push_back({child->id, vehicle.id,
                        replay::entity::AttachmentKind::DraftHarness, harness});
                }
            }
            const int seats = (std::min)(
                VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(vehicle.handle), 16);
            for (int seat = -1; seat < seats; ++seat)
            {
                // Seated only once the ped actually sits: while it gets in
                // or out it moves by its own recorded pose (the seat holds
                // it from the moment the engine assigns it).
                const Ped occupant = VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle.handle, seat);
                // The engine reports sitting slightly before the enter
                // animation ends; the seat waits for the entry to finish.
                const bool sitting = occupant && PED::IS_PED_SITTING_IN_VEHICLE(occupant, vehicle.handle) &&
                    !PED::IS_PED_GETTING_INTO_A_VEHICLE(occupant);
                if (const replay::recorder::RecordedEntity* child = sitting ? idOf(occupant) : nullptr)
                {
                    desired.push_back({child->id, vehicle.id,
                        replay::entity::AttachmentKind::VehicleSeat, seat});
                }
            }
        }
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (const replay::recorder::RecordedEntity* parent = idOf(weapon.parent))
            {
                desired.push_back({weapon.id, parent->id,
                    replay::entity::AttachmentKind::WeaponPoint, weapon.attachPoint});
                continue;
            }
            // An object in a recorded train car (replay_trains keeps the cars
            // outside the entity list).
            for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
                if (car.handle && car.handle == weapon.parent)
                {
                    desired.push_back({weapon.id, car.id, replay::entity::AttachmentKind::WeaponPoint, weapon.attachPoint});
                    break;
                }
        }
        const std::vector<replay::entity::Attachment> live = Ctx().clip.lifecycle.LiveAttachments();
        for (const replay::entity::Attachment& attachment : live)
        {
            if (std::find(desired.begin(), desired.end(), attachment) == desired.end())
                Ctx().clip.lifecycle.Detach(attachment.child, timeMs);
        }
        for (const replay::entity::Attachment& attachment : desired)
            Ctx().clip.lifecycle.Attach(attachment, timeMs);
    }

    void ScanPoseV2Entities(Ped mount)
    {
        REPLAY_PROFILE_ZONE("rec.scan_entities");
        static PoseV2Candidates candidates;
        const std::uint64_t poolStartedAt = replay::prof::Now();
        CollectPoseV2Candidates(mount, candidates);
        g_poseV2ScriptTiming.poolTotalUs += static_cast<double>(
            ElapsedUs(poolStartedAt, replay::prof::Now()));
        g_poseV2ScriptTiming.poolPeds += candidates.peds.size() + candidates.vehicles.size();

        const std::uint64_t registerStartedAt = replay::prof::Now();
        // Train cars never take a vehicle place (no skeleton, a
        // rigid track): the whole consist enters or leaves in replay_trains.
        replay::trains::TakeCandidates(candidates.vehicles);
        // Vehicles first, so the dependents of a vehicle entering
        // now are pinned in this same scan (PinPoseV2NewDependents).
        SelectPoseV2Class(candidates.vehicles, true, replay::capacity::VehicleCapacity());
        PinPoseV2NewDependents(candidates.peds);
        // object_pose.h ReservedPlaces: the ped class gives up the
        // places of the posed objects and of those waiting for one.
        SelectPoseV2Class(candidates.peds, false,
            replay::object_pose::PedPlaces(replay::recorder::kPedCapacity, replay::objects::ObjectPoseReservedPlaces()));
        LogPoseV2DiscoveryScan(candidates.peds.size(), candidates.vehicles.size());
        {
            REPLAY_PROFILE_ZONE("rec.objects_scan");
            replay::objects::Scan();
        }
        UpdatePoseV2Attachments();
        UpdatePoseV2Visibility();
        g_poseV2ScriptTiming.registerTotalUs += static_cast<double>(
            ElapsedUs(registerStartedAt, replay::prof::Now()));
    }

    std::uint32_t g_poseV2ScorchLogs = 0; // POSE_V2_SCORCHED record lines
    std::uint32_t g_poseV2WindowLogs = 0; // POSE_V2_VEHICLE_WINDOWS record lines
    std::uint32_t g_poseV2LookLogs = 0;   // POSE_V2_VEHICLE_LOOK change lines
    std::uint32_t g_poseV2PartLogs = 0;   // POSE_V2_VEHICLE_PARTS record lines
    std::uint32_t g_poseV2BurntLogs = 0;  // POSE_V2_VEHICLE_BURNT record lines
    std::uint32_t g_poseV2FireCharLogs = 0;       // POSE_V2_FIRE_CHAR record lines
    std::uint32_t g_poseV2FireCharNextMs = 0;     // next fire record sample
    std::uint32_t g_poseV2LateLogs = 0;   // POSE_V2_LATE_POSE record lines

    // An entity's track through an index kept per track vector (track_index.h),
    // in place of a find_if over every entity the recording has seen; end()
    // when it has none yet.
    template <class Track>
    typename std::vector<Track>::iterator FindPoseV2Track(
        replay::TrackIndex<Track>& index, std::vector<Track>& tracks, std::uint32_t id)
    {
        Track* found = index.Find(tracks, id);
        return found ? tracks.begin() + (found - tracks.data()) : tracks.end();
    }

    // The appearance change probes (UpdatePoseV2Appearance below).
    struct PoseV2AppearanceProbe
    {
        std::uint32_t id = 0;
        std::uint32_t lastCheckMs = 0;
        std::uint32_t lastFullMs = 0;
        int lastCount = -1; // -1: never checked
        std::uint64_t pendingPrint = 0;
        std::uint32_t pendingSinceMs = 0;
        bool pending = false;
    };
    struct PoseV2AppearanceStats
    {
        std::uint64_t ticks = 0;
        std::uint64_t visits = 0;
        std::uint64_t fullReads = 0;
        std::uint64_t empty = 0;       // no layers (the metaped rebuilding)
        std::uint64_t unconfirmed = 0; // a pending print that did not hold
        std::uint64_t changes = 0;
        std::uint64_t entitiesChanged = 0;
        double totalUs = 0.0;
        double maxUs = 0.0;
    };
    std::vector<PoseV2AppearanceProbe> g_poseV2AppearanceProbes;
    PoseV2AppearanceStats g_poseV2AppearanceStats;
    std::size_t g_poseV2AppearanceCursor = 0;
    std::uint32_t g_poseV2AppearanceLogs = 0; // POSE_V2_APPEARANCE_CHANGE record lines

    void StartPoseV2Capture()
    {
        replay::objects::EnsureStrapVisibility();
        replay::events::BeginRecording();
        replay::pose::capture::BeginRecording();
        replay::pose::fidelity::Reset();
        Ctx().clip.Begin();
        {
            static int handles[replay::recorder::kPoolCapacity]{};
            // Not a handle for every object of a town when the
            // handle pool cannot spare them (replay::objects::ListAllObjectsGuarded).
            const char* objectsMode = "listing";
            const int objects = (std::min)((std::max)(0, replay::objects::ListAllObjectsGuarded(handles,
                replay::recorder::kPoolCapacity, objectsMode)), replay::recorder::kPoolCapacity);
            Ctx().clip.objectsBefore.assign(handles, handles + objects);
            if (std::strcmp(objectsMode, "all") != 0)
                Ctx().log << "RECORDING_OBJECTS_BEFORE,mode=" << objectsMode << ",objects=" << objects
                          << ",guid_room=" << replay::pools::GuidRoom() << '\n';
            const int pickups = (std::min)((std::max)(0, worldGetAllPickups(handles, replay::recorder::kPoolCapacity)),
                replay::recorder::kPoolCapacity);
            Ctx().clip.pickupsBefore.assign(handles, handles + pickups);
            for (int n = 0; n < pickups; ++n)
                if (const Object object = OBJECT::GET_PICKUP_OBJECT(handles[n]))
                    Ctx().clip.objectsBefore.push_back(object);
            std::sort(Ctx().clip.objectsBefore.begin(), Ctx().clip.objectsBefore.end());
            std::sort(Ctx().clip.pickupsBefore.begin(), Ctx().clip.pickupsBefore.end());
        }
        Ctx().recorder.entities.clear();
        Ctx().recorder.weapons.clear();
        Ctx().recorder.props.clear();
        Ctx().recorder.drops.clear();
        replay::objects::BeginRecording();
        replay::trains::BeginRecording();
        replay::vehdoors::BeginRecording();
        Ctx().recorder.rejected.clear();
        g_poseV2NextScanAt = 0;
        g_poseV2ScorchLogs = 0;
        g_poseV2AppearanceProbes.clear();
        g_poseV2MetapedProbes.clear();
        g_poseV2MetapedStats = {};
        g_poseV2MetapedCursor = 0;
        g_poseV2MetapedLogs = 0;
        Ctx().log << replay::metaped::engine::ReaderStatus() << '\n';
        g_poseV2AppearanceStats = {};
        g_poseV2AppearanceCursor = 0;
        g_poseV2AppearanceLogs = 0;
        g_poseV2WindowLogs = 0;
        g_poseV2LookLogs = 0;
        g_poseV2BurntLogs = 0;
        g_poseV2FireCharLogs = 0;
        g_poseV2FireCharNextMs = 0;
        g_poseV2PartLogs = 0;
        g_poseV2LateLogs = 0;
        g_poseV2ScriptTiming = {};
        g_poseV2Selection = {};
        g_poseV2Retired.Clear();
        g_poseV2Discovery = {};
        g_unresolvedTries.clear();
        g_unresolvedRetryLogs = 0;
        Ctx().recorder.recording = true;
    }

    // Existence is checked every frame (one native per entity), and so is
    // the skeleton (CheckPoseV2Skeletons from UpdatePoseV2Capture): the
    // resolver walks a few pointers under SEH, and the capture detour already
    // rejects a bone-count change.
    // The entity keeps its ReplayId; its pose continues in a new segment
    // with the new skeleton and the appearance it has now.
    bool StartPoseV2Segment(replay::recorder::RecordedEntity& entity, const replay::skeleton::ResolvedPoseSkeleton& resolved)
    {
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        const std::uint32_t previousRecord = entity.recordId;
        const std::uint32_t previousBones = entity.boneCount;
        replay::pose::capture::Stop(previousRecord);
        replay::entity::PoseSegment segment;
        segment.entity = entity.id;
        segment.startMs = timeMs;
        segment.recordId = Ctx().clip.segments.NextRecordId();
        segment.boneCount = resolved.boneCount;
        std::string error;
        const bool started = replay::pose::capture::Start(
            segment.recordId, resolved.skeleton, resolved.boneCount, replay::recorder::kSampleHz, error);
        if (started)
        {
            if (!entity.vehicle)
            {
                replay::entity::appearance::Capture(entity.handle, segment.appearance);
                RecordPoseV2Metaped(entity.handle, entity.id, replay::metaped::Source::Segment, timeMs,
                    segment.appearance);
            }
            Ctx().clip.segments.Add(segment);
            entity.recordId = segment.recordId;
            entity.boneCount = resolved.boneCount;
            entity.skeleton = resolved.skeleton;
        }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_POSE_SEGMENT"
            << ",id=" << replay::entity::IdText(entity.id)
            << ",status=" << (started ? "started" : "failed")
            << ",error=" << (started ? "none" : error.c_str())
            << ",record=0x" << std::hex << segment.recordId
            << ",previous_record=0x" << previousRecord << std::dec
            << ",bones=" << resolved.boneCount
            << ",previous_bones=" << previousBones
            << ",layers=" << segment.appearance.assets.size()
            << ",time_ms=" << timeMs
            << '\n';
        log.flush();
        return started;
    }

    // Every frame: a skeleton that changed (new object or new bone count)
    // starts a new pose segment, so no frame is lost to a mismatch.
    void CheckPoseV2Skeletons()
    {
        for (std::size_t index = Ctx().recorder.entities.size(); index-- > 0;)
        {
            replay::recorder::RecordedEntity& entity = Ctx().recorder.entities[index];
            replay::skeleton::ResolvedPoseSkeleton resolved{};
            ++g_poseV2ScriptTiming.resolves;
            if (!replay::skeleton::FastResolvePedPoseSkeleton(entity.handle, resolved))
            {
                RetirePoseV2Entity(index, "skeleton_unresolved");
                continue;
            }
            if (resolved.skeleton == entity.skeleton && resolved.boneCount == entity.boneCount)
                continue;
            if (!StartPoseV2Segment(entity, resolved))
                RetirePoseV2Entity(index, "segment_failed");
        }
    }


    // The charred render bit of every recorded ped and horse
    // (_0x37B01666BAE8F7EF reads entity +0x1B0 bit 4, the bit
    // SET_ENTITY_RENDER_SCORCHED writes), twice a second, stored when it
    // changes. The first read of an entity already charred stores it at
    // that time (the state at the recording start or at its entry).
    // Vehicles too; the engine sets the same bit on a vehicle (the fire
    // shutdown of an entity fire, vehicle explosions).
    void SamplePoseV2Scorched()
    {
        REPLAY_PROFILE_ZONE("rec.scorched");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!replay::entities::Exists(entity.handle))
                continue;
            static replay::TrackIndex<replay::ScorchTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.scorched, entity.id.Packed());
            if (track != Ctx().clip.scorched.end() && track->checked && timeMs - track->lastCheckMs < 500)
                continue;
            const bool on = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(entity.handle)) & 1u) != 0;
            if (track == Ctx().clip.scorched.end())
            {
                Ctx().clip.scorched.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.scorched.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            const bool previous = track->states.empty() ? false : track->states.back().second;
            if (on == previous)
                continue;
            track->states.push_back({timeMs, on});
            if (g_poseV2ScorchLogs < 64)
            {
                ++g_poseV2ScorchLogs;
                Ctx().log << "POSE_V2_SCORCHED,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",scorched=" << (on ? 1 : 0) << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The broken windows of every recorded vehicle (IS_VEHICLE_WINDOW_INTACT,
    // windows 0..3), twice a second, stored when the mask changes. Vehicles
    // only: four natives per vehicle and sample.
    void SamplePoseV2VehicleWindows()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_windows");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            static replay::TrackIndex<replay::WindowTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.vehicleWindows, entity.id.Packed());
            if (track != Ctx().clip.vehicleWindows.end() && track->checked && timeMs - track->lastCheckMs < 500)
                continue;
            std::uint8_t mask = 0;
            for (int window = 0; window < replay::kVehicleWindows; ++window)
            {
                if (!VEHICLE::IS_VEHICLE_WINDOW_INTACT(entity.handle, window))
                    mask |= static_cast<std::uint8_t>(1u << window);
            }
            if (track == Ctx().clip.vehicleWindows.end())
            {
                Ctx().clip.vehicleWindows.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.vehicleWindows.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            // The first read is always stored: it is the vehicle's state from
            // its entry (a model without glass at an index reads broken).
            if (!track->states.empty() && mask == track->states.back().second)
                continue;
            track->states.push_back({timeMs, mask});
            if (g_poseV2WindowLogs < 64)
            {
                ++g_poseV2WindowLogs;
                Ctx().log << "POSE_V2_VEHICLE_WINDOWS,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",broken=0x" << std::hex << static_cast<unsigned>(mask) << std::dec << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The look of every recorded vehicle read again once a second, kept as its
    // latest: a vehicle whose paint, livery, cargo, lantern or extras the game
    // set after it entered the recording (an ambient wagon dressed a frame
    // later, a stable repaint) is rebuilt as it was last seen. Logged on
    // change.
    void SamplePoseV2VehicleLook()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_look");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            auto look = std::find_if(Ctx().clip.vehicleLooks.begin(), Ctx().clip.vehicleLooks.end(),
                [&](const replay::VehicleLook& entry) { return entry.id == entity.id; });
            if (look == Ctx().clip.vehicleLooks.end() || timeMs - look->lastCheckMs < 1000)
                continue;
            replay::VehicleLook now = *look;
            Hash byHash = 0;
            Hash byHandle = 0;
            ReadVehicleLook(entity.handle, now, byHash, byHandle);
            look->lastCheckMs = timeMs;
            if (now.tint == look->tint && now.livery == look->livery && now.propSet == look->propSet &&
                now.lightPropSet == look->lightPropSet && now.extrasExist == look->extrasExist &&
                now.extrasOn == look->extrasOn)
            {
                continue;
            }
            now.lastCheckMs = timeMs;
            *look = now;
            if (g_poseV2LookLogs < 64)
            {
                ++g_poseV2LookLogs;
                LogVehicleLook("change", now, byHash, byHandle, timeMs);
            }
        }
    }

    // The burnt state of every recorded vehicle, twice a
    // second, stored when it changes (VehicleBurnt track). A wagon does not
    // go black through the charred bit (+0x1B0 bit 4 never set on the
    // burnt WAGON02X in a recorded test: no POSE_V2_SCORCHED line):
    // the fire darkens it through the burn level of its shader effect
    // (draw handler +0x28, effect +0x18, burn block from vfunc +0x88, level
    // at +0x1C; written by the fire, 0x12BD608, and by the vehicle,
    // 0x15D5978, as _SET_OBJECT_BURN_LEVEL does for objects), which only
    // grows. No native reads it, so the state recorded is the natives'
    // proxy: burnt from the first sample the vehicle is on fire or charred
    // on (latched, the level does not go back).
    void SamplePoseV2VehicleBurnt()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_burnt");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            static replay::TrackIndex<replay::ScorchTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.vehicleBurnt, entity.id.Packed());
            if (track != Ctx().clip.vehicleBurnt.end() &&
                ((!track->states.empty() && track->states.back().second) ||
                    (track->checked && timeMs - track->lastCheckMs < 500)))
            {
                continue;
            }
            const bool onFire = FIRE::IS_ENTITY_ON_FIRE(entity.handle) != FALSE;
            const bool charred = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(entity.handle)) & 1u) != 0;
            if (track == Ctx().clip.vehicleBurnt.end())
            {
                Ctx().clip.vehicleBurnt.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.vehicleBurnt.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            if (!onFire && !charred)
                continue;
            track->states.push_back({timeMs, true});
            if (g_poseV2BurntLogs < 64)
            {
                ++g_poseV2BurntLogs;
                Ctx().log << "POSE_V2_VEHICLE_BURNT,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",on_fire=" << (onFire ? 1 : 0) << ",charred=" << (charred ? 1 : 0)
                    << ",fire_damage=" << (FIRE::_IS_ENTITY_BEING_DAMAGED_BY_FIRE(entity.handle) ? 1 : 0)
                    << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The fire record (burnt look, burn_level.h CharState) of
    // every recorded ped, horse and vehicle, four times a second: a search
    // of the engine's record tree per entity (a few nodes deep).
    // Stored on change (FireCharSame); the first read always (absent for
    // most: the state before the fire).
    void SamplePoseV2FireChar()
    {
        if (!replay::burn::CharEnabled())
            return;
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        if (timeMs < g_poseV2FireCharNextMs && g_poseV2FireCharNextMs - timeMs <= 250)
            return;
        g_poseV2FireCharNextMs = timeMs + 250;
        REPLAY_PROFILE_ZONE("rec.fire_char");
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            static replay::TrackIndex<replay::FireCharTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.fireChars, entity.id.Packed());
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(entity.handle));
            if (!address)
                continue;
            replay::burn::CharState read;
            {
                REPLAY_PROFILE_ZONE("rec.fire_char.read");
                if (!replay::burn::ReadChar(reinterpret_cast<const void*>(address), read))
                    continue;
            }
            replay::FireCharState state;
            state.present = read.present;
            state.flags = read.flags;
            for (const replay::burn::CharVolume& volume : read.volumes)
            {
                std::array<float, replay::kFireCharFloats> values{};
                std::copy(std::begin(volume.v), std::end(volume.v), values.begin());
                state.volumes.push_back(values);
                // Where it sits.
                replay::FireCharAnchor anchor;
                anchor.component = volume.component;
                anchor.sub = volume.sub;
                anchor.flags = volume.flags;
                anchor.mask = volume.mask;
                std::copy(std::begin(volume.dir), std::end(volume.dir), anchor.dir);
                state.anchors.push_back(anchor);
            }
            const bool first = track == Ctx().clip.fireChars.end();
            if (first)
            {
                Ctx().clip.fireChars.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.fireChars.end() - 1;
            }
            if (!track->states.empty() && replay::FireCharSame(track->states.back().second, state))
                continue;
            if (g_poseV2FireCharLogs < 64)
            {
                ++g_poseV2FireCharLogs;
                Ctx().log << "POSE_V2_FIRE_CHAR,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",present=" << (state.present ? 1 : 0) << std::hex << ",flags=0x" << int(state.flags) << std::dec
                    << ",count=" << read.count << ",volumes=" << state.volumes.size()
                    << ",char=" << (state.volumes.empty() ? 0.0f : state.volumes[0][12])
                    << ",extent=" << (state.volumes.empty() ? 0.0f : state.volumes[0][0])
                    << ",age=" << (state.volumes.empty() ? 0.0f : state.volumes[0][7])
                    << ",on_fire=" << (FIRE::IS_ENTITY_ON_FIRE(entity.handle) ? 1 : 0)
                    << ",faults=" << replay::burn::CharFaults()
                    << ",time_ms=" << timeMs << '\n';
            }
            track->states.push_back({timeMs, std::move(state)});
        }
    }

    // The wheel slot behind a wheel index (offline, build 1491.50).
    // _BREAK_OFF_VEHICLE_WHEEL (handler 0x10E2D20) takes an index (0 LF, 1 RF,
    // 2 LM, 3 RM, 4 LR, 5 RR), turns it into a hierarchy id (table 0x3B01268:
    // 19, 20, 23, 24, 21, 22) and that into the vehicle's wheel slot, u8
    // [veh+0xBB1+hid] (0x159D4A0, 0xFF: no such wheel).
    // _IS_VEHICLE_WHEEL_DESTROYED (handler 0x10ECD04) tests bit n of u32
    // [veh+0xA50] with n as given: it takes the slot, not the index. Wheel
    // count s32 [veh+0xBC0]. Returns the slot, -1 when the vehicle has no such
    // wheel, -2 unreadable or implausible.
    constexpr int kPoseV2WheelHierarchy[replay::kVehiclePartWheels] = {19, 20, 23, 24, 21, 22};
    int GuardedWheelSlot(std::uint64_t vehicle, int wheel)
    {
        __try
        {
            const std::int32_t count = *reinterpret_cast<const std::int32_t*>(vehicle + 0xBC0);
            if (count <= 0 || count > 10)
                return -2;
            const std::uint8_t slot = *reinterpret_cast<const std::uint8_t*>(vehicle + 0xBB1 + kPoseV2WheelHierarchy[wheel]);
            if (slot == 0xFF)
                return -1;
            return slot < count ? slot : -2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -2;
        }
    }

    // The wheels and doors every recorded vehicle lost
    // (VehiclePartTrack), four times a second, stored when the mask changes.
    // Natives only on the vehicle (checked to be one): _IS_VEHICLE_WHEEL_DESTROYED
    // on the slots GuardedWheelSlot maps (distinct slots, else the wheel
    // bits keep their last value) and _IS_VEHICLE_DOOR_BROKEN on doors 0..9
    // (false for a door the model lacks). The first read is always stored.
    void SamplePoseV2VehicleParts()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_parts");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle) || !ENTITY::IS_ENTITY_A_VEHICLE(entity.handle))
                continue;
            static replay::TrackIndex<replay::VehiclePartTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.vehicleParts, entity.id.Packed());
            // A vehicle that just lost groups with no fragment piece (a wheel
            // comes off as an object of its own) is sampled at once, not at its
            // 4 Hz turn.
            if (track != Ctx().clip.vehicleParts.end() && track->checked && timeMs - track->lastCheckMs < 250 &&
                !replay::objects::TakeVehiclePartsUrgent(entity.id))
                continue;
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle));
            int slots[replay::kVehiclePartWheels]{};
            bool wheelsRead = address != 0;
            std::uint32_t seen = 0;
            for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
            {
                slots[wheel] = address ? GuardedWheelSlot(address, wheel) : -2;
                if (slots[wheel] == -2 || (slots[wheel] >= 0 && (seen & (1u << slots[wheel]))))
                    wheelsRead = false;
                else if (slots[wheel] >= 0)
                    seen |= 1u << slots[wheel];
            }
            const std::uint16_t previous = track == Ctx().clip.vehicleParts.end() || track->states.empty()
                ? 0 : track->states.back().second;
            constexpr std::uint16_t kWheelBits = (1u << replay::kVehiclePartWheels) - 1;
            std::uint16_t mask = 0;
            if (wheelsRead)
            {
                for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
                {
                    if (slots[wheel] >= 0 && VEHICLE::_IS_VEHICLE_WHEEL_DESTROYED(entity.handle, slots[wheel]))
                        mask |= static_cast<std::uint16_t>(1u << wheel);
                }
            }
            else
                mask = static_cast<std::uint16_t>(previous & kWheelBits);
            for (int door = 0; door < replay::kVehiclePartDoors; ++door)
            {
                if (VEHICLE::_IS_VEHICLE_DOOR_BROKEN(entity.handle, door))
                    mask |= static_cast<std::uint16_t>(1u << (replay::kVehiclePartDoorShift + door));
            }
            const bool first = track == Ctx().clip.vehicleParts.end();
            if (first)
            {
                Ctx().clip.vehicleParts.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.vehicleParts.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            if (!track->states.empty() && mask == track->states.back().second)
                continue;
            track->states.push_back({timeMs, mask});
            // A wheel that came off now is an object of its own: recorded as a
            // piece of the vehicle with its track (replay_objects.cpp
            // NoteVehicleWheelOff).
            if (!first && wheelsRead)
                for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
                    if ((mask & ~previous & (1u << wheel)) && slots[wheel] >= 0)
                        replay::objects::NoteVehicleWheelOff(entity.id, entity.handle, wheel, slots[wheel]);
            if (g_poseV2PartLogs < 64)
            {
                ++g_poseV2PartLogs;
                std::ostream& log = Ctx().log;
                log << "POSE_V2_VEHICLE_PARTS,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",wheels_off=0x" << std::hex << static_cast<unsigned>(mask & kWheelBits)
                    << ",doors_off=0x" << static_cast<unsigned>(mask >> replay::kVehiclePartDoorShift) << std::dec
                    << ",wheels_read=" << (wheelsRead ? 1 : 0) << ",slots=";
                for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
                    log << (wheel ? ";" : "") << slots[wheel];
                log << ",first=" << (first ? 1 : 0) << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The door ratios of every recorded vehicle, read from the engine (rung 2)
    // at most every 50 ms per vehicle, stored on change. The train cars are
    // read by replay_trains.
    void SamplePoseV2VehicleDoors()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_doors");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            replay::vehdoors::RecordVehicle(Ctx().clip.vehicleDoors, entity.id.Packed(), entity.handle, false, timeMs,
                Ctx().log);
        }
    }

    // Appearance changes over time (appearance_track.h). A few recorded peds
    // and horses per tick, round robin, each at most every 250 ms: the layer
    // count (one native); the full fingerprint (1 + layers natives) when the
    // count moved, once a second, or while a change waits for its confirmation,
    // at most one per tick. A print other than what the recording already has
    // for the entity at this time (create packet, pose segment or last change,
    // as AppearanceAt picks it) must be seen on two checks in a row (the engine
    // passes through partial layer sets while it rebuilds a metaped: 10 -> 0 ->
    // 11 in the hogtie bench) and is stored at its first sighting. A change
    // that also changes the skeleton is a pose segment already: the segment
    // captures the same layers, the print then matches and nothing is stored.

    constexpr std::size_t kPoseV2AppearanceVisitsPerTick = 4;
    constexpr std::uint32_t kPoseV2AppearanceCheckMs = 250;
    constexpr std::uint32_t kPoseV2AppearanceFullMs = 1000;

    void SamplePoseV2Appearance()
    {
        REPLAY_PROFILE_ZONE("rec.appearance");
        std::vector<replay::recorder::RecordedEntity>& entities = Ctx().recorder.entities;
        if (entities.empty())
            return;
        const std::uint64_t startedAt = replay::prof::Now();
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        PoseV2AppearanceStats& stats = g_poseV2AppearanceStats;
        bool fullBudget = true;
        const std::size_t visits = (std::min)(entities.size(), kPoseV2AppearanceVisitsPerTick);
        for (std::size_t visit = 0; visit < visits; ++visit)
        {
            // Entities that went away were retired this tick already.
            const replay::recorder::RecordedEntity& entity = entities[g_poseV2AppearanceCursor++ % entities.size()];
            if (entity.vehicle)
                continue;
            const std::uint32_t packed = entity.id.Packed();
            static replay::TrackIndex<PoseV2AppearanceProbe> index;
            auto probe = FindPoseV2Track(index, g_poseV2AppearanceProbes, packed);
            if (probe == g_poseV2AppearanceProbes.end())
            {
                g_poseV2AppearanceProbes.push_back({});
                probe = g_poseV2AppearanceProbes.end() - 1;
                probe->id = packed;
            }
            if (probe->lastCount >= 0 && timeMs - probe->lastCheckMs < kPoseV2AppearanceCheckMs)
                continue;
            ++stats.visits;
            const int count = replay::entity::appearance::LayerCount(entity.handle);
            const bool full = probe->pending || count != probe->lastCount ||
                timeMs - probe->lastFullMs >= kPoseV2AppearanceFullMs;
            if (!full)
            {
                probe->lastCheckMs = timeMs;
                continue;
            }
            // Over budget: checked again on its next turn.
            if (!fullBudget)
                continue;
            fullBudget = false;
            probe->lastCheckMs = timeMs;
            probe->lastFullMs = timeMs;
            probe->lastCount = count;
            ++stats.fullReads;
            std::uint64_t live = 0;
            int liveCount = 0;
            if (!replay::entity::appearance::Fingerprint(entity.handle, live, liveCount))
            {
                ++stats.empty;
                probe->pending = false;
                continue;
            }
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(packed);
            if (!create)
                continue;
            const replay::entity::PoseSegment* segment = Ctx().clip.segments.At(entity.id, timeMs);
            const replay::AppearanceTrack* current = replay::FindAppearanceTrack(Ctx().clip.appearances, packed);
            const replay::WantedAppearance recorded = replay::AppearanceAt(current, segment, create->appearance, timeMs);
            if (replay::AppearancePrint(*recorded.appearance) == live)
            {
                stats.unconfirmed += probe->pending ? 1 : 0;
                probe->pending = false;
                continue;
            }
            if (!probe->pending || probe->pendingPrint != live)
            {
                stats.unconfirmed += probe->pending ? 1 : 0;
                probe->pending = true;
                probe->pendingPrint = live;
                probe->pendingSinceMs = timeMs;
                continue;
            }
            probe->pending = false;
            replay::AppearanceChange change;
            change.timeMs = probe->pendingSinceMs;
            // Never before the last change or the segment in force (times
            // per track ascend; AppearanceAt skips a change older than the
            // segment).
            if (current && !current->changes.empty() && change.timeMs < current->changes.back().timeMs)
                change.timeMs = current->changes.back().timeMs;
            if (segment && change.timeMs < segment->startMs)
                change.timeMs = segment->startMs;
            const std::size_t previousLayers = recorded.appearance->assets.size();
            replay::entity::appearance::Capture(entity.handle, change.appearance);
            const std::size_t layers = change.appearance.assets.size();
            const std::uint32_t outfit = change.appearance.outfit;
            replay::AppearanceTrack* track = nullptr;
            for (replay::AppearanceTrack& entry : Ctx().clip.appearances)
            {
                if (entry.id == packed)
                    track = &entry;
            }
            if (!track)
            {
                Ctx().clip.appearances.push_back({});
                track = &Ctx().clip.appearances.back();
                track->id = packed;
                ++stats.entitiesChanged;
            }
            track->changes.push_back(std::move(change));
            ++stats.changes;
            // The whole look with the new layers.
            RecordPoseV2Metaped(entity.handle, entity.id, replay::metaped::Source::Change, track->changes.back().timeMs,
                track->changes.back().appearance);
            if (g_poseV2AppearanceLogs < 64)
            {
                ++g_poseV2AppearanceLogs;
                Ctx().log << "POSE_V2_APPEARANCE_CHANGE,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",layers=" << layers << ",previous_layers=" << previousLayers
                    << ",outfit=0x" << std::hex << outfit << std::dec
                    << ",bones=" << entity.boneCount
                    << ",time_ms=" << track->changes.back().timeMs << ",confirm_ms=" << timeMs << '\n';
            }
        }
        const double us = static_cast<double>(ElapsedUs(startedAt, replay::prof::Now()));
        ++stats.ticks;
        stats.totalUs += us;
        stats.maxUs = (std::max)(stats.maxUs, us);
    }

    // The pose fidelity judge's engine reads (PoseFidelity=1, pose_fidelity.h),
    // after every script capture of the tick (the polls, then the posed
    // objects' polls in replay::objects::Sample: a read is stamped with the
    // time of its content, capture::ContentTimeMs, so the capture of this tick
    // must come first): every recorded entity's current skeleton and record, a
    // few times a second each. Posed objects (a carried pelt) count as "other".
    // replay::objects::Sample has just retired the gone ones and ended the
    // pose of one whose skeleton changed, so a posed object's armed skeleton
    // is its current one: no second resolve (that would go through a handle
    // that may have been deleted since the last tick, with no Exists check
    // first). Reads only, no natives.
    void SamplePoseV2Fidelity()
    {
        REPLAY_PROFILE_ZONE("rec.pose_fidelity");
        namespace fidelity = replay::pose::fidelity;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!fidelity::Due(entity.recordId, nowMs))
                continue;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(entity.recordId);
            const fidelity::Kind kind = entity.vehicle ? fidelity::Kind::Vehicle
                : !create ? fidelity::Kind::Other
                : create->kind == replay::entity::EntityKind::Ped ? fidelity::Kind::Ped
                : create->kind == replay::entity::EntityKind::Horse ? fidelity::Kind::Horse
                : fidelity::Kind::Other;
            fidelity::Sample(entity.recordId, entity.skeleton, entity.boneCount, kind, nowMs);
        }
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (weapon.posed && fidelity::Due(weapon.id.Packed(), nowMs))
                fidelity::Sample(weapon.id.Packed(), weapon.poseSkeleton, weapon.poseBones, fidelity::Kind::Other, nowMs);
        }
    }

    void UpdatePoseV2Capture(Ped mount)
    {
        REPLAY_PROFILE_ZONE("rec.pose_v2");
        if (!Ctx().recorder.recording)
            return;
        const std::uint64_t startedAt = replay::prof::Now();

        for (std::size_t index = Ctx().recorder.entities.size(); index-- > 0;)
        {
            if (!replay::entities::Exists(Ctx().recorder.entities[index].handle))
                RetirePoseV2Entity(index, "entity_gone");
        }
        UpdateFullAnim(); // experiment, off by default
        // The recorded horses' muscles (shader_vars_replay.h, section SHVR).
        replay::muscles::TickRecord(replay::pose::capture::NowMs());
        const std::uint64_t checkStartedAt = replay::prof::Now();
        CheckPoseV2Skeletons();
        ++g_poseV2ScriptTiming.checks;
        g_poseV2ScriptTiming.checkTotalUs += static_cast<double>(
            ElapsedUs(checkStartedAt, replay::prof::Now()));
        // The pose finalizer may not visit vehicle skeletons; sample them
        // here with the same rate gate and ring.
        // The polls and end-of-frame checks.
        static const replay::prof::ZoneId pollsZone = replay::prof::Register("rec.pose_polls");
        const std::uint64_t pollsStartedAt = replay::prof::Now();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle)
            {
                replay::pose::capture::PollFromScript(entity.recordId);
                continue;
            }
            // The end-of-frame check (and, while the engine
            // changes this skeleton after the finalizer, its capture).
            // With PollIfStalled, in one call (one lookup of the record).
            const replay::pose::capture::LateCheck late = replay::pose::capture::PollAndCheckLate(entity.recordId);
            if ((late.switchedOn || late.switchedOff || late.capped || (late.changed && g_poseV2LateLogs < 24)) &&
                g_poseV2LateLogs < 64)
            {
                ++g_poseV2LateLogs;
                std::ostream& log = Ctx().log;
                const replay::logfmt::FormatScope logFormat(log); // precision not left behind
                log << std::fixed << std::setprecision(3)
                    << "POSE_V2_LATE_POSE,id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(entity.recordId))
                    << ",state=" << (late.switchedOn ? "late_on" : late.switchedOff ? "late_off"
                                     : late.capped ? "capped" : "changed")
                    << ",local_bones=" << late.localBones
                    << ",object_bones=" << late.objectBones
                    << ",worst_bone=" << late.worstBone
                    << ",max_m=" << late.maxTranslation
                    << ",max_rad=" << late.maxRotation
                    << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
                log.unsetf(std::ios_base::floatfield);
                log.flush();
            }
        }
        replay::prof::Add(pollsZone, replay::prof::Now() - pollsStartedAt);
        // The recording gate. The clock stands still: entities
        // that went away are retired and skeleton changes open segments
        // (at the cut instant, which is where they belong), the pose polls
        // skip by themselves, the detour queues are drained and discarded
        // (events::Record takes its paused path), and nothing is sampled
        // or scanned.
        if (replay::pose::capture::IsClockPaused())
        {
            replay::objects::DiscardPendingEvents();
            replay::events::Record();
            return;
        }
        replay::objects::Sample();
        if (replay::pose::fidelity::Enabled()) // PoseFidelity=1 only, after the posed objects' polls
            SamplePoseV2Fidelity();
        replay::objects::UpdateRecordedProps();
        replay::trains::SampleRecording();
        SamplePoseV2Ropes();
        replay::objects::SearchDrops();
        SamplePoseV2Scorched();
        SamplePoseV2Appearance();
        SamplePoseV2Metaped();
        SamplePoseV2VehicleWindows();
        SamplePoseV2VehicleLook();
        SamplePoseV2VehicleParts();
        SamplePoseV2VehicleDoors();
        SamplePoseV2VehicleBurnt();
        SamplePoseV2FireChar();
        replay::events::Record();

        const ULONGLONG now = GetTickCount64();
        if (const std::uint64_t breaks = replay::frag::Total(); breaks != g_poseV2BreaksSeen)
        {
            g_poseV2BreaksSeen = breaks;
            g_poseV2BreakScanUntil = now + kPoseV2BreakScanMs;
        }
        if (now >= g_poseV2NextScanAt || now < g_poseV2BreakScanUntil)
        {
            const std::uint64_t scanStartedAt = replay::prof::Now();
            g_poseV2NextScanAt = now + kPoseV2ScanIntervalMs;
            ScanPoseV2Entities(mount);
            const double scanUs = static_cast<double>(
                ElapsedUs(scanStartedAt, replay::prof::Now()));
            ++g_poseV2ScriptTiming.scans;
            g_poseV2ScriptTiming.scanTotalUs += scanUs;
            g_poseV2ScriptTiming.scanMaxUs = (std::max)(g_poseV2ScriptTiming.scanMaxUs, scanUs);
        }
        Ctx().clip.lifecycle.Advance(replay::pose::capture::NowMs());

        const double updateUs = static_cast<double>(
            ElapsedUs(startedAt, replay::prof::Now()));
        ++g_poseV2ScriptTiming.updates;
        g_poseV2ScriptTiming.updateTotalUs += updateUs;
        g_poseV2ScriptTiming.updateMaxUs = (std::max)(g_poseV2ScriptTiming.updateMaxUs, updateUs);
    }

    // One POSE_V2_CAPTURE_STOP line's inputs, taken on the script thread at the
    // stop; the line is formatted on a worker (deferred_log.h).
    struct PoseV2StopLine
    {
        std::uint32_t recordId = 0;
        replay::entity::ReplayId id;
        const char* kind = "unknown"; // KindText's static strings
        replay::pose::capture::Stats stats;
    };

    // The stats come from the caller's one Snapshot and the line
    // is not flushed (the stop wrote 440 lines with a flush each, plus a
    // second Snapshot per record: ~25 ms of the city's 80 ms stop tick).
    // Runs on a worker thread (deferred_log.h) and writes to its stream: only
    // its arguments, never Ctx() or a global (the id and kind come resolved:
    // the clip is the script thread's). The text does not depend on the
    // stream's state: fixed and precision are set first and the first number
    // follows an explicit std::hex.
    void LogPoseV2RecordStats(std::ostream& log, std::uint32_t recordId, replay::entity::ReplayId id, const char* kind,
        ULONGLONG elapsedMs, const replay::pose::capture::Stats& stats)
    {
        const double seconds = stats.durationMs > 0
            ? static_cast<double>(stats.durationMs) / 1000.0
            : static_cast<double>(elapsedMs) / 1000.0;
        const double bytesPerSecond = seconds > 0.0
            ? static_cast<double>(stats.trackBytes) / seconds
            : 0.0;

        char engineErrorText[96]{};
        std::snprintf(engineErrorText, sizeof(engineErrorText), ",eng_max_rot=%.2e,eng_max_trans=%.2e",
            static_cast<double>(stats.engineComposeMaxRotation), static_cast<double>(stats.engineComposeMaxTranslation));
        log << std::fixed << std::setprecision(3)
            << "POSE_V2_CAPTURE_STOP"
            << ",id=" << replay::entity::IdText(id)
            << ",record=0x" << std::hex << recordId << std::dec
            << ",kind=" << kind
            << ",bones=" << stats.boneCount
            << ",recording_ms=" << elapsedMs
            << ",track_span_ms=" << stats.durationMs
            << ",captured=" << stats.captured
            << ",detour_calls=" << stats.detourCalls
            << ",dropped_ring=" << stats.droppedRingFull
            << ",bone_mismatch=" << stats.boneCountMismatch
            << ",copy_faults=" << stats.copyFaults
            << ",detour_avg_us=" << stats.detourCaptureAvgUs
            << ",detour_max_us=" << stats.detourCaptureMaxUs
            << ",layout_ready=" << (stats.layoutReady ? 1 : 0)
            << ",layout_reused=" << (stats.layoutReused ? 1 : 0)
            << ",layout_candidates=" << stats.layoutCandidates
            << ",eng_parents=" << (stats.engineParents ? 1 : 0)
            << ",eng_order_ok=" << (stats.engineParentOrderOk ? 1 : 0)
            << ",eng_root_scale=" << stats.engineRootScale
            << ",eng_groups=" << stats.engineGroups
            << ",eng_agree=" << stats.engineParentAgree
            << ",eng_disagree=" << stats.engineParentDisagree
            << ",eng_unresolved=" << stats.engineParentUnresolved
            << ",eng_checks=" << stats.engineComposeChecks
            << engineErrorText
            << ",eng_layout=" << (stats.engineLayout ? 1 : 0)
            << ",eng_layout_rejected=" << stats.engineLayoutRejected
            << ",composed_frames=" << stats.objectsComposed
            << ",frames_without_objects=" << stats.framesWithoutObjects
            << ",discovery_ms=" << stats.discoveryMs
            << ",order=" << static_cast<int>(stats.discovery.order)
            << ",inherit_scale=" << (stats.discovery.inheritScale ? 1 : 0)
            << ",demoted_bones=" << stats.demotedBones
            << ",demotion_events=" << stats.demotionEvents
            << ",reparented=" << stats.reparentedBones
            << ",reparent_jobs=" << stats.reparentJobs
            << ",reparent_timeouts=" << stats.reparentTimeouts
            << ",first_demotion_ms=" << stats.firstDemotionMs
            << ",last_demotion_ms=" << stats.lastDemotionMs
            << ",rebase_frames=" << stats.rebaseFrames << ",cut_frames=" << stats.cutFrames
            << ",roots=" << stats.discovery.roots
            << ",parented=" << stats.discovery.parented
            << ",unresolved=" << stats.discovery.unresolved
            << ",ambiguous=" << stats.discovery.ambiguous
            << ",high_precision=" << stats.discovery.highPrecision
            << ",encoded=" << stats.encodedFrames
            << ",keyframes=" << stats.keyframes
            << ",relative_keyframes=" << stats.relativeKeyframes
            << ",full_kf_bytes=" << stats.fullKeyframeBytes
            << ",relative_kf_bytes=" << stats.relativeKeyframeBytes
            << ",pending_dropped=" << stats.pendingDropped
            << ",track_bytes=" << stats.trackBytes
            << ",bytes_per_s=" << bytesPerSecond
            << ",entries_per_frame=" << stats.entriesPerFrameAvg
            << ",encode_avg_us=" << stats.encodeAvgUs
            << ",encode_max_us=" << stats.encodeMaxUs
            << ",composition_max_m=" << stats.compositionMaxTranslation
            << ",composition_max_rad=" << stats.compositionMaxRotation
            << ",world_read_faults=" << stats.worldReadFaults
            << ",script_polls=" << stats.scriptPolls
            << ",late_checks=" << stats.lateChecks
            << ",late_skipped=" << stats.lateSkipped
            << ",late_changed=" << stats.lateChanged
            << ",late_local_bones=" << stats.lateMaxLocalBones
            << ",late_object_bones=" << stats.lateMaxObjectBones
            << ",late_worst_bone=" << stats.lateWorstBone
            << ",late_max_m=" << stats.lateMaxTranslation
            << ",late_switches=" << stats.lateSwitches
            << ",late_capped=" << stats.lateCapped
            << ",late_samples=" << stats.lateSamples
            << ",engine_writes=0"
            << ",validated=" << stats.validatedFrames
            << ",validation_skipped=" << stats.validationSkipped
            << ",validation_incremental=" << stats.validationIncremental
            << ",validation_bones_checked=" << stats.validationBonesChecked
            << ",partial_composed=" << stats.partialComposed
            << ",raw_delta_channels=" << stats.rawDeltaChannels
            << ",raw_same_channels=" << stats.rawSameChannels
            << ",raw_same_pct=" << (stats.rawDeltaChannels
                   ? 100.0 * static_cast<double>(stats.rawSameChannels) / static_cast<double>(stats.rawDeltaChannels)
                   : 0.0)
            << ",raw_same_frames=" << stats.rawSameFrames
            << ",late_equal=" << stats.lateEqualChecks
            << ",late_verified=" << stats.lateVerified
            << ",late_verify_mismatch=" << stats.lateVerifyMismatch
            << ",poll_idle=" << stats.pollIdleFrames
            << ",poll_idle_local_frames=" << stats.pollIdleLocalFrames
            << ",poll_idle_object_frames=" << stats.pollIdleObjectFrames
            << ",poll_idle_root_frames=" << stats.pollIdleRootFrames
            << ",poll_idle_local_bones=" << stats.pollIdleLocalBones
            << ",poll_idle_object_bones=" << stats.pollIdleObjectBones
            << ",poll_idle_checked=" << stats.pollIdleChecked
            << ",poll_after_calls=" << stats.pollCallFrames
            << ",poll_after_calls_sum=" << stats.pollCallsSum
            << ",poll_after_same=" << stats.pollCallSameFrames
            << ",poll_after_local_bones=" << stats.pollCallLocalBones
            << ",poll_after_object_bones=" << stats.pollCallObjectBones
            << ",poll_after_checked=" << stats.pollCallChecked
            << ",poll_raced=" << stats.pollRaced
            << ",late_direct=" << stats.lateDirect
            << ",stall_polls=" << stats.stallPolls
            << ",polls_skipped_recent=" << stats.pollsSkippedRecent
            << ",ring_turn_busy=" << stats.ringTurnBusy
            << ",late_interval_max_ms=" << stats.lateIntervalMaxMs
            << '\n';
        // What the animation left unwritten in the finalizer's source
        // (pose_capture.h Stats::unwritten*).
        log << "POSE_V2_UNWRITTEN"
            << ",id=" << replay::entity::IdText(id)
            << ",record=0x" << std::hex << recordId << std::dec
            << ",kind=" << kind
            << ",bones=" << stats.boneCount
            << ",samples=" << stats.unwrittenSamples
            << ",always_t=" << stats.unwrittenAlways[0]
            << ",always_r=" << stats.unwrittenAlways[1]
            << ",always_s=" << stats.unwrittenAlways[2]
            << ",mixed_t=" << stats.unwrittenMixed[0]
            << ",mixed_r=" << stats.unwrittenMixed[1]
            << ",mixed_s=" << stats.unwrittenMixed[2]
            << ",min_rows=" << stats.unwrittenMinRows
            << ",max_rows=" << stats.unwrittenMaxRows
            << ",partial_rows=" << stats.unwrittenPartialRows
            << ",faults=" << stats.unwrittenFaults
            << ",scan_avg_us=" << stats.unwrittenScanAvgUs
            << ",scan_max_us=" << stats.unwrittenScanMaxUs
            << ",top=" << (stats.unwrittenTop.empty() ? "-" : stats.unwrittenTop)
            << ",timeline=" << (stats.unwrittenTimeline.empty() ? "-" : stats.unwrittenTimeline)
            // The census runs inside the detour; a record it never visited
            // (samples=0) whose every script copy had the same bytes never
            // moved: the skeleton as built, the bind pose (068 22:379), or a
            // corpse whose ragdoll slept before the take (008).
            << ",never_animated="
            << (replay::tpose::RecordedNeverAnimated(stats.captured, stats.detourCalls, stats.rawDeltaChannels,
                    stats.rawSameChannels) ? 1 : 0)
            << '\n';
        log.unsetf(std::ios_base::floatfield);
    }

    void StopPoseV2Capture(ULONGLONG elapsedMs)
    {
        if (!Ctx().recorder.recording)
            return;
        Ctx().recorder.recording = false;
        // The stop's lines set fixed and precision 3 and 1
        // (POSE_V2_PERF) and left them on the shared log stream.
        const replay::logfmt::FormatScope logFormat(Ctx().log);
        // The stop's parts (the stop tick of a long clip, REC_STOP_TIMING).
        const std::uint64_t stopStartedAt = replay::prof::Now();
        replay::events::EndRecording();
        const std::uint64_t eventsEndedAt = replay::prof::Now();
        replay::pose::capture::StopAll();
        const std::uint64_t captureStoppedAt = replay::prof::Now();
        // Entities still live stay created until the end of the clip.
        Ctx().clip.lifecycle.Advance(replay::pose::capture::NowMs());
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            Ctx().clip.adoptedObjects.push_back(prop.object);
        EndFullAnim(); // before the entities are forgotten
        Ctx().recorder.entities.clear();
        Ctx().recorder.weapons.clear();
        Ctx().recorder.props.clear();
        Ctx().recorder.drops.clear();
        replay::objects::EndRecording();
        replay::trains::EndRecording();
        replay::vehdoors::EndRecording(Ctx().clip.vehicleDoors, Ctx().log);

        const std::uint64_t objectsEndedAt = replay::prof::Now();
        // POSE_V2_STOP_TIMING is written at the end of the stop now, with its
        // parts.
        Ctx().log << replay::entities::SummaryLine("record") << '\n';
        double totalBytes = 0.0;
        double worstDetourUs = 0.0;
        float worstErrorM = 0.0f;
        int records = 0;
        // The script copies of every record, by kind (ped, horse, other).
        struct PollEvidence
        {
            std::uint64_t records = 0, captured = 0, detourCalls = 0, validated = 0, incremental = 0;
            std::uint64_t idle = 0, idleLocalFrames = 0, idleObjectFrames = 0, idleRootFrames = 0;
            std::uint64_t idleLocalBones = 0, idleObjectBones = 0, idleChecked = 0;
            std::uint64_t after = 0, afterCalls = 0, afterSame = 0, afterLocalBones = 0, afterObjectBones = 0;
            std::uint64_t afterChecked = 0, raced = 0, bonesChecked = 0;
            std::uint64_t stallPolls = 0, skippedRecent = 0, turnBusy = 0;
        };
        PollEvidence pollEvidence[3];
        // The POSE_V2_CAPTURE_STOP lines (~2.4 KB of iostream formatting each;
        // the tail cost a median 49 us per record, 8 to 27 ms of the stop tick
        // in town) are formatted on a worker and written on a later tick
        // (deferred_log.h); the same text.
        const std::uint64_t recordsStartedAt = replay::prof::Now();
        std::vector<PoseV2StopLine> stopLines;
        // The detector of what the engine computed only part of the time. A
        // record with more than kDegradedMixedRows mixed rows (written in some
        // samples, left at the bind pose in others: off screen or far, before
        // the anim-visible byte) is counted as degraded; with the byte on the
        // bench measured 0 to 62 mixed rows per record (the expressions' own
        // switching), 530 to 1060 without it.
        constexpr std::uint32_t kDegradedMixedRows = 150;
        struct UnwrittenSummary
        {
            std::uint32_t records = 0;
            std::uint32_t scanned = 0;
            std::uint32_t degraded = 0;
            std::uint32_t worstMixed = 0;
            std::uint32_t worstRecord = 0;
            std::uint64_t mixedTotal = 0;
            std::uint64_t samples = 0;
            double scanUs = 0.0;
            // The records the census could not scan (no detour call) and, of
            // those, the never animated bodies (tpose::RecordedNeverAnimated:
            // the whole record still; the bind pose or a sleeping corpse, the
            // playback's POSE_V2_NEVER_ANIMATED tells them apart).
            std::uint32_t unscanned = 0;
            std::uint32_t neverAnimated = 0;
            std::string neverIds;
        } unwritten;
        for (const std::uint32_t recordId : replay::pose::capture::RecordIds())
        {
            const replay::pose::capture::Stats stats =
                replay::pose::capture::Snapshot(recordId);
            ++unwritten.records;
            if (stats.unwrittenSamples > 0)
            {
                const std::uint32_t mixed = stats.unwrittenMixed[0] + stats.unwrittenMixed[1] + stats.unwrittenMixed[2];
                ++unwritten.scanned;
                unwritten.mixedTotal += mixed;
                unwritten.samples += stats.unwrittenSamples;
                unwritten.scanUs += stats.unwrittenScanAvgUs * static_cast<double>(stats.unwrittenSamples);
                if (mixed > kDegradedMixedRows)
                    ++unwritten.degraded;
                if (mixed > unwritten.worstMixed)
                {
                    unwritten.worstMixed = mixed;
                    unwritten.worstRecord = recordId;
                }
            }
            else if (stats.captured > 0)
            {
                ++unwritten.unscanned;
                // Bodies only (peds and horses); a holstered
                // weapon or a prop is just as still and is not a T-pose.
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
                const bool body = create && (create->kind == replay::entity::EntityKind::Ped ||
                    create->kind == replay::entity::EntityKind::Horse);
                if (body && replay::tpose::RecordedNeverAnimated(stats.captured, stats.detourCalls, stats.rawDeltaChannels,
                        stats.rawSameChannels))
                {
                    ++unwritten.neverAnimated;
                    if (unwritten.neverAnimated <= 16)
                        unwritten.neverIds += (unwritten.neverIds.empty() ? "" : ";") + replay::entity::IdText(create->id);
                }
            }
            {
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
                stopLines.push_back({recordId, create ? create->id : replay::entity::ReplayId::FromPacked(recordId),
                    create ? replay::entity::KindText(create->kind) : "unknown", stats});
            }
            {
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
                const char* kind = create ? replay::entity::KindText(create->kind) : "unknown";
                PollEvidence& e = pollEvidence[std::strcmp(kind, "ped") == 0 ? 0 : std::strcmp(kind, "horse") == 0 ? 1 : 2];
                ++e.records;
                e.captured += stats.captured;
                e.detourCalls += stats.detourCalls;
                e.validated += stats.validatedFrames;
                e.incremental += stats.validationIncremental;
                e.bonesChecked += stats.validationBonesChecked;
                e.idle += stats.pollIdleFrames;
                e.idleLocalFrames += stats.pollIdleLocalFrames;
                e.idleObjectFrames += stats.pollIdleObjectFrames;
                e.idleRootFrames += stats.pollIdleRootFrames;
                e.idleLocalBones += stats.pollIdleLocalBones;
                e.idleObjectBones += stats.pollIdleObjectBones;
                e.idleChecked += stats.pollIdleChecked;
                e.after += stats.pollCallFrames;
                e.afterCalls += stats.pollCallsSum;
                e.afterSame += stats.pollCallSameFrames;
                e.afterLocalBones += stats.pollCallLocalBones;
                e.afterObjectBones += stats.pollCallObjectBones;
                e.afterChecked += stats.pollCallChecked;
                e.raced += stats.pollRaced;
                e.stallPolls += stats.stallPolls;
                e.skippedRecent += stats.pollsSkippedRecent;
                e.turnBusy += stats.ringTurnBusy;
            }
            totalBytes += static_cast<double>(stats.trackBytes);
            worstDetourUs = std::max(worstDetourUs, stats.detourCaptureAvgUs);
            worstErrorM = std::max(worstErrorM, stats.compositionMaxTranslation);
            ++records;
        }
        Ctx().log << "POSE_V2_UNWRITTEN_SUMMARY,records=" << unwritten.records << ",scanned=" << unwritten.scanned
                  << ",degraded=" << unwritten.degraded << ",degraded_over_rows=" << kDegradedMixedRows
                  << ",worst_mixed_rows=" << unwritten.worstMixed << ",worst_record=0x" << std::hex
                  << unwritten.worstRecord << std::dec << ",mixed_rows_total=" << unwritten.mixedTotal
                  << ",samples=" << unwritten.samples << ",scan_avg_us="
                  << (unwritten.samples ? unwritten.scanUs / static_cast<double>(unwritten.samples) : 0.0)
                  << ",anim_visible=" << (g_fullAnim.animVisible ? 1 : 0) << ",unscanned=" << unwritten.unscanned
                  << ",never_animated=" << unwritten.neverAnimated
                  << ",never_animated_ids=" << (unwritten.neverIds.empty() ? "-" : unwritten.neverIds.c_str()) << '\n';
        if (!stopLines.empty())
        {
            // This stream as the lines left it
            // (the lines below print with its state).
            Ctx().log.unsetf(std::ios_base::floatfield);
            Ctx().log << std::dec << std::setprecision(3);
            replay::deferredlog::Log().Submit([lines = std::move(stopLines), elapsedMs]() {
                std::ostringstream text;
                for (const PoseV2StopLine& line : lines)
                    LogPoseV2RecordStats(text, line.recordId, line.id, line.kind, elapsedMs, line.stats);
                return text.str();
            });
        }
        const std::uint64_t recordsEndedAt = replay::prof::Now();
        {
            static const char* const kPollKinds[3] = {"ped", "horse", "other"};
            for (int kind = 0; kind < 3; ++kind)
            {
                const PollEvidence& e = pollEvidence[kind];
                if (e.records == 0)
                    continue;
                Ctx().log << "POSE_V2_POLL_EVIDENCE,kind=" << kPollKinds[kind] << ",records=" << e.records
                          << ",captured=" << e.captured << ",detour_calls=" << e.detourCalls
                          << ",validated=" << e.validated << ",incremental=" << e.incremental
                          << ",bones_checked=" << e.bonesChecked
                          << ",idle=" << e.idle << ",idle_local_frames=" << e.idleLocalFrames
                          << ",idle_object_frames=" << e.idleObjectFrames << ",idle_root_frames=" << e.idleRootFrames
                          << ",idle_local_bones=" << e.idleLocalBones << ",idle_object_bones=" << e.idleObjectBones
                          << ",idle_checked=" << e.idleChecked
                          << ",after_calls=" << e.after << ",after_calls_sum=" << e.afterCalls
                          << ",after_same=" << e.afterSame << ",after_local_bones=" << e.afterLocalBones
                          << ",after_object_bones=" << e.afterObjectBones << ",after_checked=" << e.afterChecked
                          << ",raced=" << e.raced
                          << ",stall_polls=" << e.stallPolls << ",polls_skipped_recent=" << e.skippedRecent
                          << ",ring_turn_busy=" << e.turnBusy << '\n';
            }
        }

        const replay::entity::LifecycleStats& lifecycle = Ctx().clip.lifecycle.Stats();
        std::ostream& log = Ctx().log;
        log << "POSE_V2_LIFECYCLE"
            << ",entities=" << records
            << ",blocks=" << lifecycle.blocks
            << ",creates=" << lifecycle.creates
            << ",deletes=" << lifecycle.deletes
            << ",full_creates=" << lifecycle.fullCreates
            << ",attaches=" << lifecycle.attaches
            << ",detaches=" << lifecycle.detaches
            << ",full_attachments=" << lifecycle.fullAttachments
            << ",live_at_end=" << Ctx().clip.lifecycle.Live().size()
            << ",rejected=" << Ctx().recorder.rejected.size()
            << ",evicted=" << g_poseV2Selection.evicted
            << ",full_scans=" << g_poseV2Selection.skippedScans
            << ",full_skipped_peak=" << g_poseV2Selection.skippedPeak
            << ",reentries=" << g_poseV2Selection.reentries
            << ",weapons=" << Ctx().clip.weaponTracks.size()
            << ",shots=" << Ctx().clip.shots.Shots().size()
            << ",shot_impacts=" << replay::events::ShotImpacts()
            << '\n';
        // How the entities were found (entity_discovery.h).
        log << "POSE_V2_DISCOVERY,phase=summary"
            << ",scans=" << g_poseV2Discovery.scans
            << ",ped_listing_empty=" << g_poseV2Discovery.pedListingEmpty
            << ",vehicle_listing_empty=" << g_poseV2Discovery.vehicleListingEmpty
            << ",nearby_calls=" << g_poseV2Discovery.nearbyCalls
            << ",nearby_peds=" << g_poseV2Discovery.nearbyPeds
            << ",nearby_vehicles=" << g_poseV2Discovery.nearbyVehicles
            << ",budget_spent=" << g_poseV2Discovery.budgetSpent
            << ",kept_recorded=" << g_poseV2Discovery.keptRecorded
            << ",dependents_added=" << g_poseV2Discovery.dependentsAdded
            << ",late_entries=" << g_poseV2Discovery.lateEntries;
        {
            const PoseV2DiscoveryStats& d = g_poseV2Discovery;
            const double scans = d.scans ? static_cast<double>(d.scans) : 1.0;
            log << ",vehicle_capacity=" << replay::capacity::VehicleCapacity() << std::fixed << std::setprecision(1)
                << ",recorded_peds_avg=" << static_cast<double>(d.recordedPedsSum) / scans
                << ",recorded_vehicles_avg=" << static_cast<double>(d.recordedVehiclesSum) / scans
                << ",recorded_vehicles_max=" << d.recordedVehiclesMax << std::defaultfloat << std::setprecision(6);
        }
        // The pool reader (entity_pools.h).
        {
            const PoseV2DiscoveryStats& d = g_poseV2Discovery;
            const double scans = d.poolScans ? static_cast<double>(d.poolScans) : 1.0;
            log << ",source=" << (d.poolScans ? (d.poolFallbacks ? "mixed" : "pool") : "listing")
                << ",pool_scans=" << d.poolScans << ",pool_fallbacks=" << d.poolFallbacks
                << ",pool_peds_avg=" << std::fixed << std::setprecision(1) << static_cast<double>(d.poolPedsLive) / scans
                << ",pool_peds_max=" << d.poolPedsLiveMax
                << ",ped_handled_avg=" << static_cast<double>(d.poolPedCandidates) / scans
                << ",pool_vehicles_avg=" << static_cast<double>(d.poolVehiclesLive) / scans
                << ",vehicle_handled_avg=" << static_cast<double>(d.poolVehicleCandidates) / scans
                << ",handles_created=" << d.poolCreated << ",handle_failed=" << d.poolFailed
                << ",budget_skipped=" << d.poolSkippedBudget
                << ",guid_free_min=" << (d.poolScans ? d.guidFreeMin : 0u) << ",guid_size=" << d.guidSize
                << ",pool_us_avg=" << d.poolUsTotal / scans << ",pool_us_max=" << d.poolUsMax
                << std::defaultfloat << std::setprecision(6);
        }
        log << '\n';
        for (const replay::WeaponTrack& entry : Ctx().clip.weaponTracks)
        {
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            entry.track.Span(firstMs, lastMs);
            log << "POSE_V2_WEAPON_STOP"
                << ",id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(entry.recordId))
                << ",samples=" << entry.track.Size()
                << ",dropped=" << entry.track.Dropped()
                << ",span_ms=" << (lastMs - firstMs)
                << '\n';
        }
        const PoseV2ScriptTiming& timing = g_poseV2ScriptTiming;
        const replay::pose::capture::EngineTiming engine =
            replay::pose::capture::EngineTimingSnapshot();
        log << std::fixed << std::setprecision(1)
            << "POSE_V2_PERF"
            << ",script_updates=" << timing.updates
            << ",script_avg_us=" << (timing.updates ? timing.updateTotalUs / timing.updates : 0.0)
            << ",script_max_us=" << timing.updateMaxUs
            << ",scans=" << timing.scans
            << ",scan_avg_us=" << (timing.scans ? timing.scanTotalUs / timing.scans : 0.0)
            << ",scan_max_us=" << timing.scanMaxUs
            << ",skeleton_resolves=" << timing.resolves
            << ",check_avg_us=" << (timing.checks ? timing.checkTotalUs / timing.checks : 0.0)
            << ",pool_avg_us=" << (timing.scans ? timing.poolTotalUs / timing.scans : 0.0)
            << ",pool_avg_peds=" << (timing.scans ? static_cast<double>(timing.poolPeds) / timing.scans : 0.0)
            << ",register_avg_us=" << (timing.scans ? timing.registerTotalUs / timing.scans : 0.0)
            << ",detour_calls_all=" << engine.detourCalls
            << ",detour_us_per_s=" << engine.detourUsPerSecond
            << ",pressure_wakes=" << engine.pressureWakes
            << ",worker_busy_ms=" << engine.workerBusyMs
            << ",worker_discovery_ms=" << engine.workerDiscoveryMs
            << ",pending_peak_frames=" << engine.pendingPeakFrames
            << '\n';
        {
            // The capture worker's time, which PROFILE_TOTAL_FRAME does not see
            // (another thread): per script tick of this recording and as a
            // share of one core, split by phase, and the g_mutex waits.
            const double ticks = static_cast<double>((std::max)(std::uint64_t{1}, replay::prof::SessionFrameCount()));
            const double spanMs = (std::max)(1.0, static_cast<double>(elapsedMs));
            const double busyMs = engine.workerBusyMs;
            const double otherMs = (std::max)(0.0, busyMs - engine.workerDrainMs - engine.workerComposeMs -
                engine.workerValidateMs - engine.workerEncodeMs);
            // The records left, those the worker still visited, those the
            // buffer removed.
            const replay::pose::capture::RecordCounts counts = replay::pose::capture::RecordCountsSnapshot();
            log << "POSE_V2_WORKER"
                << ",shortcuts=1,copy_plain=0" // release 1.0: always (the A/B keys are gone)
                << ",fidelity_hz=" << (replay::pose::fidelity::Enabled() ? replay::pose::fidelity::Hz() : 0u)
                << ",fidelity_async=" << (replay::pose::fidelity::Async() ? 1 : 0)
                << ",stall_poll_ms=" << replay::pose::capture::StallPollMs()
                << ",late_backoff=" << (replay::pose::capture::LateBackoff() ? 1 : 0)
                << ",worker_poll_ms=" << replay::pose::capture::WorkerPollMs()
                << ",spawned_objects=" << (replay::objects::SpawnedObjects() ? 1 : 0)
                << ",object_burn_level=" << (replay::objects::ObjectBurnLevel() ? 1 : 0)
                << ",still_pose_engine=" << (replay::objects::StillPoseEngine() ? 1 : 0)
                << ",object_drawn_check=" << (replay::objects::ObjectDrawnCheck() ? 1 : 0)
                << ",object_look=" << (replay::objlook::RecordAndPlay() ? 1 : 0) // object texture and tint
                << ",object_look_evidence=" << (replay::objlook::Evidence() ? 1 : 0)
                << ",scenery_handoff_ticks=" << replay::objects::SceneryHandoffTicks() // object texture handoff
                << ",regfx_owner_check=" << (replay::events::RegisteredFxOwnerCheck() ? 1 : 0)
                << ",ped_audio_keep_length=" << (replay::events::PedAudioKeepRecordedLength() ? 1 : 0)
                << ",animal_vocal_retry=" << (replay::events::AnimalVocalRetry() ? 1 : 0)
                << ",ped_capacity=" << replay::capacity::PedCapacity()
                << ",records=" << counts.records
                << ",drain=" << counts.drain
                << ",pruned=" << counts.pruned
                << ",ticks=" << static_cast<std::uint64_t>(ticks)
                << ",frames=" << engine.workerFrames
                << ",busy_ms=" << busyMs
                << ",cpu_ms=" << engine.workerCpuMs
                << ",core_pct=" << 100.0 * busyMs / spanMs
                << ",cpu_core_pct=" << 100.0 * engine.workerCpuMs / spanMs
                << ",busy_us_per_tick=" << busyMs * 1000.0 / ticks
                << ",cpu_us_per_tick=" << engine.workerCpuMs * 1000.0 / ticks
                << ",drain_ms=" << engine.workerDrainMs
                << ",compose_ms=" << engine.workerComposeMs
                << ",validate_ms=" << engine.workerValidateMs
                << ",encode_ms=" << engine.workerEncodeMs
                << ",other_ms=" << otherMs
                << ",lock_wait_ms=" << engine.workerLockWaitMs
                << ",lock_wait_max_us=" << engine.workerLockWaitMaxUs
                << ",script_lock_waits=" << engine.scriptLockWaits
                << ",script_lock_wait_ms=" << engine.scriptLockWaitMs
                << ",script_lock_wait_max_us=" << engine.scriptLockWaitMaxUs
                << ",ring_pool_hits=" << engine.ringPoolHits
                << ",ring_pool_misses=" << engine.ringPoolMisses
                << ",ring_pool_mb=" << engine.ringPoolMb
                << ",start_avg_us=" << engine.startAvgUs
                << ",start_max_us=" << engine.startMaxUs
                << ",reserve_hits=" << engine.reserveHits
                << ",reserve_misses=" << engine.reserveMisses
                << ",discovery_hold_max_us=" << engine.discoveryHoldMaxUs;
            for (std::size_t site = 0; site < replay::pose::capture::kScriptLockSites; ++site)
                log << ",script_lock_max_us_" << replay::pose::capture::kScriptLockSiteNames[site] << '='
                    << engine.scriptLockSiteMaxUs[site];
            log << '\n';
        }
        log.unsetf(std::ios_base::floatfield);
        {
            // The appearance change sampler's summary.
            const PoseV2AppearanceStats& appearance = g_poseV2AppearanceStats;
            char text[256]{};
            std::snprintf(text, sizeof(text),
                "POSE_V2_APPEARANCE_SUMMARY,entities_changed=%llu,changes=%llu,visits=%llu,full_reads=%llu,"
                "empty=%llu,unconfirmed=%llu,ticks=%llu,avg_us=%.1f,max_us=%.1f\n",
                static_cast<unsigned long long>(appearance.entitiesChanged),
                static_cast<unsigned long long>(appearance.changes),
                static_cast<unsigned long long>(appearance.visits),
                static_cast<unsigned long long>(appearance.fullReads),
                static_cast<unsigned long long>(appearance.empty),
                static_cast<unsigned long long>(appearance.unconfirmed),
                static_cast<unsigned long long>(appearance.ticks),
                appearance.ticks ? appearance.totalUs / static_cast<double>(appearance.ticks) : 0.0,
                appearance.maxUs);
            log << text;
        }
        {
            // The metaped sampler's summary.
            const PoseV2MetapedStats& m = g_poseV2MetapedStats;
            const replay::metaped::engine::Stats& reader = replay::metaped::engine::Counters();
            std::size_t tracks = 0, withExpressions = 0;
            std::size_t withShopItems = 0, shopStates = 0, shopItems = 0, shopWithState = 0;
            for (const replay::metaped::Track& track : Ctx().clip.metapeds)
            {
                ++tracks;
                for (const replay::metaped::State& state : track.states)
                    if (!state.expressions.empty())
                    {
                        ++withExpressions;
                        break;
                    }
                bool anyItems = false;
                for (const replay::metaped::State& state : track.states)
                {
                    if (state.shopItems.empty())
                        continue;
                    anyItems = true;
                    ++shopStates;
                    shopItems += state.shopItems.size();
                    for (const replay::metaped::ShopItem& item : state.shopItems)
                        shopWithState += item.state ? 1u : 0u;
                }
                withShopItems += anyItems ? 1u : 0u;
            }
            log << "POSE_V2_METAPED_SUMMARY,phase=record,entities=" << tracks << ",with_expressions=" << withExpressions
                << ",states=" << m.states << ",confirms=" << m.confirms << ",late=" << m.late << ",changes=" << m.changes
                << ",unstable_dropped=" << m.unstableDropped << ",read_failures=" << m.readFailures
                << ",no_reader=" << m.noExpressions << ",reader_reads=" << reader.reads
                << ",cross_checks=" << reader.crossChecks << ",cross_mismatches=" << reader.crossMismatches
                << ",with_shop_items=" << withShopItems << ",shop_item_states=" << shopStates
                << ",shop_items=" << shopItems << ",shop_items_with_state=" << shopWithState
                << ",reader=" << (replay::metaped::engine::ReaderOn() ? "on" : "off") << '\n';
        }
        log.flush();
        // The pose fidelity judge (PoseFidelity=1): this recording's engine
        // reads against the tracks, after StopAll and before any playback
        // samples them; last, so the lines above do not count its track reads.
        // With PoseFidelityAsync=1 only the export and the hand-over to a
        // worker happen here (fidelity_us).
        const std::uint64_t fidelityStartedAt = replay::prof::Now();
        replay::pose::fidelity::Evaluate(log);
        {
            // The stop's parts. ev_*: replay::events::EndRecording by part
            // (events_end_us is their sum); records: the per-record loop
            // (snapshots, the lines handed to a worker); summary: the lines
            // from POSE_V2_POLL_EVIDENCE to POSE_V2_APPEARANCE_SUMMARY.
            const replay::events::EndTiming ev = replay::events::LastEndTiming();
            const std::uint64_t stopEndedAt = replay::prof::Now();
            log << "POSE_V2_STOP_TIMING,events_end_us=" << ElapsedUs(stopStartedAt, eventsEndedAt)
                << ",capture_stop_all_us=" << ElapsedUs(eventsEndedAt, captureStoppedAt)
                << ",objects_end_us=" << ElapsedUs(captureStoppedAt, objectsEndedAt)
                << ",ev_totals_us=" << ev.totalsUs << ",ev_flush_us=" << ev.flushUs << ",ev_decals_us=" << ev.decalsUs
                << ",ev_fires_us=" << ev.firesUs << ",ev_audit_us=" << ev.auditUs << ",ev_sounds_us=" << ev.soundsUs
                << ",ev_melee_us=" << ev.meleeUs << ",ev_tracks_us=" << ev.tracksUs << ",ev_speech_us=" << ev.speechUs
                << ",ev_tail_us=" << ev.tailUs
                << ",records=" << records << ",records_us=" << ElapsedUs(recordsStartedAt, recordsEndedAt)
                << ",summary_us=" << ElapsedUs(recordsEndedAt, fidelityStartedAt)
                << ",fidelity_us=" << ElapsedUs(fidelityStartedAt, stopEndedAt)
                << ",stop_us=" << ElapsedUs(stopStartedAt, stopEndedAt) << '\n';
        }

        const double seconds = static_cast<double>(elapsedMs) / 1000.0;
        char message[192]{};
        std::snprintf(
            message,
            sizeof(message),
            "PoseTrack v2 | %d entities | %.1f KB/s | hook %.0f us | error %.1f mm",
            records,
            seconds > 0.0 ? totalBytes / seconds / 1024.0 : 0.0,
            worstDetourUs,
            worstErrorM * 1000.0f);
        replay::hud::ShowObjective(message);
    }
    }

    void Attach(Context& context) { g_context = &context; }
    void SetFullAnim(const FullAnimConfig& config) { g_fullAnim = config; }
    void Start() { StartPoseV2Capture(); }
    void Update(Ped mount) { UpdatePoseV2Capture(mount); }
    void Stop(std::uint64_t elapsedMs) { StopPoseV2Capture(elapsedMs); }
}
