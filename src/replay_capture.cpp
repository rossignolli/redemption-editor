#include "replay_capture_internal.h"

namespace replay::recorder
{

    namespace
    {

        Context* g_context = nullptr;
    }
        Context& Ctx() { return *g_context; }

    namespace
    {

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;
    }

        std::uint64_t ElapsedUs(std::uint64_t start, std::uint64_t end)
        {
            return (end - start) * 1000000ull / replay::prof::TicksPerSecond();
        }

        // What the recording does so the engine computes a recorded ped as if
        // the game camera saw it, and what to give back. The anim-visible byte
        // (anim_visible.h) goes to every recorded ped and animal.
        FullAnimConfig g_fullAnim;

    namespace
    {
        struct FullAnimState
        {
            bool visibleSet = false;       // the anim-visible byte was 0 and is ours to clear
            bool visibleForeign = false;   // it was set already (a pedshot): never touched
            std::uintptr_t ped = 0;        // the entity's address and its graphics component,
            std::uintptr_t component = 0;  // resolved once (owner checked every tick)
        };
        std::unordered_map<Entity, FullAnimState> g_fullAnimPeds;
        struct FullAnimTotals
        {
            std::uint32_t peds = 0;
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
        // The LOD setter's detour: tried once per load (installed on the
        // first recording that wants it), the error kept for the log.
        bool g_animLodTried = false;
        std::string g_animLodError = "not_tried";
        std::vector<std::uintptr_t> g_animLodDirectors;

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
            // No byte without the engine's own read of it (the offset written).
            static const bool proven = replay::sig::Find(replay::animvisible::kAnimVisibleTest) != 0;
            static const replay::enginesig::Pool pool{replay::sig::Find(replay::enginesig::kComponentBaseIndex),
                replay::sig::Find(replay::enginesig::kComponentTable)};
            state.ped = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
            if (!proven)
            {
                state.component = 0;
                return 0;
            }
            state.component = replay::animvisible::GraphicsComponent(pool, state.ped,
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

        void GiveBackFullAnim(Entity ped, FullAnimState state)
        {
            if (!replay::entities::Exists(ped))
                return;
            GiveBackAnimVisible(ped, state);
        }
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
            if (g_fullAnim.animVisible)
                Ctx().log << "RECORD_FULL_ANIM,anim_visible=1,peds=" << g_fullAnimTotals.peds
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
            if (!g_fullAnim.animVisible && !g_fullAnim.animLod0)
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
            static const replay::enginesig::Pool creatures{replay::sig::Find(replay::enginesig::kCreatureBaseIndex),
                replay::sig::Find(replay::enginesig::kCreatureTable)};
            g_animLodDirectors.clear();
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
                    const std::uintptr_t director = replay::animlod::DirectorOf(creatures, address,
                        [](std::uintptr_t at, void* out, std::size_t size) { return replay::mem::Copy(out, at, size); });
                    if (director)
                        g_animLodDirectors.push_back(director);
                    else if (first)
                        ++g_fullAnimTotals.lodUnresolved;
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

    namespace
    {

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
    }
    std::vector<std::pair<Entity, std::uint32_t>> g_unresolvedTries;
    std::uint32_t g_unresolvedRetryLogs = 0;
    std::vector<PoseV2MetapedProbe> g_poseV2MetapedProbes;
    PoseV2MetapedStats g_poseV2MetapedStats;
    std::size_t g_poseV2MetapedCursor = 0;
    std::uint32_t g_poseV2MetapedLogs = 0;

    namespace
    {
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

    namespace
    {

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
    }

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
    PoseV2ScriptTiming g_poseV2ScriptTiming;
    ULONGLONG g_poseV2NextScanAt = 0;
    std::uint64_t g_poseV2BreaksSeen = 0;
    ULONGLONG g_poseV2BreakScanUntil = 0;

    namespace
    {

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
    }
    PoseV2DiscoveryStats g_poseV2Discovery;

    namespace
    {
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
    // available (signature mismatch): the listing and
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
            Ctx().log << "POSE_V2_DISCOVERY,phase=pool_unavailable,error=" << (error.empty() ? "none" : error.c_str()) << ",fallback=listing,"
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
    }
    PoseV2SelectionStats g_poseV2Selection;
    // Handles the selection retired, for the re-entry evidence.
    replay::entity::RecentRetirements g_poseV2Retired;

    namespace
    {
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
        // The ped class keeps every place (object_pose.h ShouldPoseShared).
        SelectPoseV2Class(candidates.peds, false, replay::recorder::kPedCapacity);
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

    void Attach(Context& context) { g_context = &context; }
    void SetFullAnim(const FullAnimConfig& config) { g_fullAnim = config; }
    void Start() { StartPoseV2Capture(); }
    void Update(Ped mount) { UpdatePoseV2Capture(mount); }
    void Stop(std::uint64_t elapsedMs) { StopPoseV2Capture(elapsedMs); }
}
