// replay_playback_puppets.cpp: the puppets: LOD, dress and metaped, the pose override arm (mapped, exact), the skeleton signature gate, the prefix hold.
// Split from replay_playback.cpp (moved code); shared internals in replay_playback_internal.h.

#include "replay_playback_internal.h"

namespace replay::playback
{
    namespace
    {
    replay::log::LogLimit g_poseV2ShopPlayLogs;
    replay::log::LogLimit g_poseV2MetapedPlayLogs;
    replay::log::LogLimit g_poseV2NotRecordedLogs;
    replay::log::LogLimit g_poseV2SwitchLogs;
    replay::norecord::CategoryCache g_noRecordCategories;
    replay::log::LogLimit g_noRecordLogs;
    // A mapped puppet's pose (its own bone count), built from the record's
    // sample through its skeleton map.
    replay::pose::PoseSample g_poseV2MappedSample;
    }

    bool PoseV2LodBody(const replay::playback::Puppet& puppet)
    {
        return puppet.packet.kind == replay::entity::EntityKind::Ped || puppet.packet.kind == replay::entity::EntityKind::Horse;
    }

    // Once per clone of a ped or horse puppet, before it is armed
    // (puppet_lod.h): full detail far away and the pre-render off screen. The
    // state of a former clone goes with it.
    void ApplyPoseV2Lod(replay::playback::Puppet& puppet)
    {
        if (!PoseV2LodBody(puppet) || !puppet.clone || puppet.lod.clone == puppet.clone)
            return;
        if (puppet.lod.clone)
            g_poseV2LodSummary.Add(puppet.packet.id.Packed(), puppet.lod); // a former clone's counts
        puppet.lod = replay::lod::PuppetState{};
        puppet.lod.clone = puppet.clone;
        PED::SET_PED_LOD_MULTIPLIER(puppet.clone, static_cast<float>(replay::lod::kLodMultiplier));
        ENTITY::SET_ENTITY_ALWAYS_PRERENDER(puppet.clone, TRUE);
        ++g_poseV2LodSummary.puppets;
    }

    // Before the entity is deleted: off the pre-render list, multiplier 1.
    void UndoPoseV2Lod(replay::playback::Puppet& puppet)
    {
        if (!PoseV2LodBody(puppet) || !puppet.lod.clone)
            return;
        if (puppet.lod.clone == puppet.clone && replay::entities::Exists(puppet.clone))
        {
            ENTITY::SET_ENTITY_ALWAYS_PRERENDER(puppet.clone, FALSE);
            PED::SET_PED_LOD_MULTIPLIER(puppet.clone, 1.0f);
            ++g_poseV2LodSummary.undone;
        }
        g_poseV2LodSummary.Add(puppet.packet.id.Packed(), puppet.lod);
        puppet.lod = replay::lod::PuppetState{};
    }
    namespace
    {

    // The puppets' anim directors at LOD 0 and visible (RecordPedAnimLod0,
    // puppet_lod.h, anim_lod_hook.h), as the recording had the originals.
    // Without it the engine's (lod, visible) of a puppet decides each frame
    // which expressions run: those that write only bones do not matter (the
    // pose is the replay's), those that write the animated normal maps of a
    // horse's muscles (track 0x17) switch on and off with it.
    struct PoseV2AnimLodState
    {
        bool tried = false;
        std::string error = "not_tried";
        bool active = false;          // a list was published this playback
        replay::animlod::Stats start; // the hook's counters when it began
        std::uint32_t directorsMax = 0;
        std::uint64_t unresolved = 0; // ticks x puppets without a director
    } g_poseV2AnimLod;
    std::vector<std::uintptr_t> g_poseV2AnimLodDirectors;
    }

    void UpdatePoseV2AnimLod()
    {
        if (!g_poseV2LodConfig.animLod0)
            return;
        if (!g_poseV2AnimLod.tried)
        {
            g_poseV2AnimLod.tried = true;
            std::string error;
            g_poseV2AnimLod.error = replay::animlod::Install(error) ? "none" : error;
            Ctx().log << "POSE_V2_PUPPET_ANIM_LOD_HOOK,installed=" << (replay::animlod::Installed() ? 1 : 0)
                      << ",error=" << g_poseV2AnimLod.error << '\n';
        }
        if (!replay::animlod::Installed())
            return;
        if (!g_poseV2AnimLod.active)
        {
            g_poseV2AnimLod.active = true;
            g_poseV2AnimLod.start = replay::animlod::Snapshot();
        }
        static const replay::enginesig::Pool creatures{replay::sig::Find(replay::enginesig::kCreatureBaseIndex),
            replay::sig::Find(replay::enginesig::kCreatureTable)};
        g_poseV2AnimLodDirectors.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (!PoseV2LodBody(puppet) || !puppet.clone || puppet.lod.clone != puppet.clone)
                continue;
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(puppet.clone));
            const std::uintptr_t director = replay::animlod::DirectorOf(creatures, address,
                [](std::uintptr_t at, void* out, std::size_t size) { return replay::mem::Copy(out, at, size); });
            if (director)
                g_poseV2AnimLodDirectors.push_back(director);
            else
                ++g_poseV2AnimLod.unresolved;
        }
        g_poseV2AnimLod.directorsMax = (std::max)(g_poseV2AnimLod.directorsMax,
            static_cast<std::uint32_t>(g_poseV2AnimLodDirectors.size()));
        replay::animlod::SetForcedFor(replay::animlod::Owner::Playback, g_poseV2AnimLodDirectors);
    }

    // The list withdrawn when the playback stops, and the line: forced, the
    // calls of the puppets' directors; changed, those where the engine had
    // chosen a LOD above 0 or not visible; flips, those whose engine choice
    // differed from the previous call of the same director (the frame to
    // frame switching the detour hides).
    void EndPoseV2AnimLod()
    {
        if (!g_poseV2AnimLod.active)
            return;
        g_poseV2AnimLodDirectors.clear();
        replay::animlod::SetForcedFor(replay::animlod::Owner::Playback, g_poseV2AnimLodDirectors);
        const replay::animlod::Stats now = replay::animlod::Snapshot();
        const replay::animlod::Stats& was = g_poseV2AnimLod.start;
        const auto since = [](std::uint64_t a, std::uint64_t b) { return a >= b ? a - b : a; };
        Ctx().log << "POSE_V2_PUPPET_ANIM_LOD,installed=" << (replay::animlod::Installed() ? 1 : 0)
                  << ",error=" << g_poseV2AnimLod.error << ",forced=" << since(now.forced, was.forced)
                  << ",changed=" << since(now.changed, was.changed) << ",flips=" << since(now.flips, was.flips)
                  << ",directors_max=" << g_poseV2AnimLod.directorsMax << ",unresolved=" << g_poseV2AnimLod.unresolved
                  << '\n';
        g_poseV2AnimLod.active = false;
        g_poseV2AnimLod.directorsMax = 0;
        g_poseV2AnimLod.unresolved = 0;
    }


    // A puppet the engine stopped updating (two ticks in a row without an own
    // write), with what could decide it: the render camera (camera_m,
    // on_screen), the hidden player (player_m) and the gameplay camera behind
    // him (gameplay_m, gameplay_angle: 0 ahead, 180 behind), how many bodies
    // the replay shows and how many peds the pool holds. 64 per playback, with
    // Debug=0.
    void LogPoseV2AnimSkip(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
        const auto distance = [&](const Vector3& from) {
            const float dx = at.x - from.x, dy = at.y - from.y, dz = at.z - from.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const Ped player = PLAYER::PLAYER_PED_ID();
        const bool hasPlayer = player && ENTITY::DOES_ENTITY_EXIST(player);
        const Vector3 playerAt = hasPlayer ? ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE) : Vector3{};
        const Vector3 gameplay = CAM::GET_GAMEPLAY_CAM_COORD();
        const Vector3 gameplayRot = CAM::GET_GAMEPLAY_CAM_ROT(2);
        const float angle = replay::lod::AngleFromForwardDeg(
            gameplayRot.x, gameplayRot.z, at.x - gameplay.x, at.y - gameplay.y, at.z - gameplay.z);
        std::uint32_t bodies = 0;
        for (const replay::playback::Puppet& other : Ctx().puppets.list)
            bodies += PoseV2LodBody(other) && other.clone && other.visibleState == 1 ? 1u : 0u;
        const replay::pools::PoolState peds = replay::pools::State(replay::pools::Kind::Ped);
        const replay::logfmt::FormatScope logFormat(Ctx().log);
        Ctx().log << std::fixed << std::setprecision(1) << "POSE_V2_PUPPET_ANIM_SKIP,id=" << replay::entity::IdText(puppet.packet.id)
                  << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex << puppet.packet.modelHash
                  << std::dec << ",on_screen=" << (ENTITY::IS_ENTITY_ON_SCREEN(puppet.clone) ? 1 : 0)
                  << ",camera_m=" << distance(camera) << ",player_m=" << (hasPlayer ? distance(playerAt) : -1.0f)
                  << ",gameplay_m=" << distance(gameplay) << ",gameplay_angle=" << angle << ",bodies=" << bodies
                  << ",ped_pool=" << peds.used << ",mission=" << (MISC::GET_MISSION_FLAG() ? 1 : 0)
                  << ",stale_ticks=" << puppet.lod.staleTicks << ",script_pose=" << (g_poseV2LodConfig.scriptPose ? 1 : 0)
                  << ",playhead_ms=" << timeMs << '\n';
    }

    void LogPoseV2LodSummary()
    {
        const replay::lod::Summary& total = g_poseV2LodSummary;
        if (total.puppets == 0)
            return;
        Ctx().log << "POSE_V2_PUPPET_LOD_SUMMARY,puppets=" << total.puppets << ",undone=" << total.undone
                  << ",multiplier=" << replay::lod::kLodMultiplier << ",stale_ticks=" << total.staleTicks
                  << ",forced_updates=" << total.forcedTicks << ",puppets_stale=" << total.puppetsStale
                  << ",low_lod_episodes=" << total.episodes << ",worst_id="
                  << (total.worstPacked ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(total.worstPacked)) : std::string("none"))
                  << ",worst_stale_ticks=" << total.worstStale << ",script_pose=" << (g_poseV2LodConfig.scriptPose ? 1 : 0)
                  << ",script_pose_ticks=" << total.scriptTicks << ",puppets_script_pose=" << total.puppetsScript << '\n';
    }

    // Segment selection is pure (src/pose_segments): these wrappers bind
    // the recording's segment track, create packets and the recorder's
    // sample state.
    bool PoseV2RecordSampled(std::uint32_t recordId)
    {
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        return replay::pose::capture::TrackSpan(recordId, firstMs, lastMs);
    }

    const replay::entity::PoseSegment* PoseV2SegmentAt(
        replay::entity::ReplayId id, std::uint32_t timeMs)
    {
        return replay::entity::SegmentAt(Ctx().clip.segments, id, timeMs, PoseV2RecordSampled);
    }

    // replay_puppets.h SetWear / Wear.
    void SetPoseV2Wear(replay::playback::Puppet& puppet, const replay::entity::Appearance& appearance)
    {
        replay::playback::SetWear(puppet, appearance);
    }
    const replay::entity::Appearance& PoseV2Wear(const replay::playback::Puppet& puppet)
    {
        return replay::playback::Wear(puppet);
    }

    // The appearance a puppet wears at the playhead: the latest of its create
    // packet, pose segment and recorded appearance changes
    // (appearance_track.h).
    replay::WantedAppearance PoseV2AppearanceAt(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        return replay::AppearanceAt(replay::FindAppearanceTrack(Ctx().clip.appearances, puppet.packet.id.Packed()),
            PoseV2SegmentAt(puppet.packet.id, timeMs), puppet.packet.appearance, timeMs);
    }

    // What a new puppet is dressed with before it is shown (appearance_track.h
    // DressAt): the playhead's appearance, or for a ped recorded while the
    // engine still built it (no layers in the create packet), the first later
    // segment or change with layers.
    replay::DressAppearance PoseV2DressAt(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        return replay::DressAt(replay::FindAppearanceTrack(Ctx().clip.appearances, puppet.packet.id.Packed()),
            Ctx().clip.segments.Segments(), puppet.packet.id, PoseV2SegmentAt(puppet.packet.id, timeMs),
            puppet.packet.appearance, timeMs, replay::kDressLookaheadMs);
    }

    // The whole metaped look (metaped_state.h: expressions, scale, cleanliness)
    // worn at the playhead.
    replay::metaped::Wanted PoseV2MetapedAt(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        return replay::metaped::StateAt(replay::metaped::FindTrack(Ctx().clip.metapeds, puppet.packet.id.Packed()), timeMs,
            replay::kDressLookaheadMs);
    }
    namespace
    {


    bool PoseV2Body(const replay::playback::Puppet& puppet)
    {
        return puppet.packet.kind == replay::entity::EntityKind::Ped || puppet.packet.kind == replay::entity::EntityKind::Horse;
    }
    }

    // The metaped state at the playhead on the puppet, when its key changed
    // or `force` (the layers were just applied: an equip resets them). Only
    // what differs from the puppet's own is set (metaped_reader.cpp Apply).
    bool ApplyPoseV2Metaped(replay::playback::Puppet& puppet, std::uint32_t timeMs, bool force)
    {
        if (!PoseV2Body(puppet) || !puppet.clone || Ctx().clip.metapeds.empty())
            return false;
        const replay::metaped::Wanted wanted = PoseV2MetapedAt(puppet, timeMs);
        if (!wanted.state)
        {
            puppet.metapedKey = 0;
            return false;
        }
        if (!force && wanted.key == puppet.metapedKey)
            return false;
        const replay::metaped::engine::ApplyReport report = replay::metaped::engine::Apply(puppet.clone, *wanted.state);
        puppet.metapedKey = wanted.key;
        puppet.metapedSet += report.expressionsSet;
        ++g_poseV2MetapedPlay.applies;
        g_poseV2MetapedPlay.expressionsSet += report.expressionsSet;
        return report.expressionsSet || report.scaleSet || report.cleanlinessSet;
    }

    // Dresses a body puppet with `appearance` as the game dresses the story
    // player, by the recorded shop items and their wearable states, when the
    // metaped state at the playhead carries them for these very layers (dressed
    // by layers alone, the story player's feet render under his shoes); else by
    // the layers alone (clips before MPED v2, a body without shop items, a
    // puppet whose shop dress failed the exact check). Both end with
    // _SET_ACTIVE_META_PED_COMPONENTS_UPDATED and _UPDATE_PED_VARIATION.
    // `shown`: the puppet is on screen (a segment, a change, a retry): the
    // dress is checked once the engine rebuilt it (CheckPoseV2ShopDress).
    replay::metaped::DressPath DressPoseV2Body(replay::playback::Puppet& puppet, const replay::entity::Appearance& appearance,
        std::uint32_t timeMs, bool shown)
    {
        const replay::metaped::Wanted wanted = PoseV2MetapedAt(puppet, timeMs);
        replay::metaped::DressFacts facts;
        facts.hasState = wanted.state != nullptr;
        facts.stateHasItems = wanted.state && (wanted.state->flags & replay::metaped::kHasShopItems) &&
            !wanted.state->shopItems.empty();
        facts.printMatches = wanted.state && wanted.state->layersPrint == replay::AppearancePrint(appearance);
        facts.shopRefused = puppet.shopRefused;
        facts.wearEmpty = appearance.assets.empty();
        const replay::metaped::DressChoice choice = replay::metaped::ChooseDress(facts);
        PoseV2ShopPlayStats& stats = g_poseV2ShopPlay;
        if (choice.path == replay::metaped::DressPath::ShopItems)
        {
            replay::entity::appearance::DressByShopItems(puppet.clone, appearance, wanted.state->shopItems);
            puppet.shopItemsApplied = static_cast<std::uint32_t>(wanted.state->shopItems.size());
            ++stats.dressShop;
        }
        else
        {
            replay::entity::appearance::ApplyLayers(puppet.clone, appearance);
            puppet.shopItemsApplied = 0;
            ++stats.dressLayers;
            const std::string why = choice.why;
            stats.whyNoState += why == "no_state" ? 1u : 0u;
            stats.whyNoShopItems += why == "no_shop_items" ? 1u : 0u;
            stats.whyPrintDiffers += why == "print_differs" ? 1u : 0u;
            stats.whyShopRefused += why == "shop_refused" ? 1u : 0u;
            stats.whyNoLayers += why == "no_layers" ? 1u : 0u;
        }
        puppet.dressedBy = choice.path;
        puppet.dressWhy = choice.why;
        const bool check = shown && choice.path == replay::metaped::DressPath::ShopItems;
        puppet.shopCheckAt = check ? GetTickCount64() + 300 : 0;
        puppet.shopCheckSince = check ? GetTickCount64() : 0;
        return choice.path;
    }

    // The check of a dress. The layers against the recording (the exact check
    // of the Dressing), the shop items read back against the metaped state's. A
    // shop dress whose layers are not the recorded ones is refused for this
    // puppet and dressed again by layers (returns true: the caller waits for
    // the new dress). One line per body (capped) and the counts of the summary.
    bool CheckPoseV2ShopDress(replay::playback::Puppet& puppet, std::uint32_t timeMs, const char* phase,
        const replay::entity::appearance::Match& match)
    {
        const replay::metaped::Wanted wanted = PoseV2MetapedAt(puppet, timeMs);
        const bool shopDress = puppet.dressedBy == replay::metaped::DressPath::ShopItems;
        const bool recorded = wanted.state && (wanted.state->flags & replay::metaped::kHasShopItems) &&
            !wanted.state->shopItems.empty();
        if (!shopDress && !recorded)
            return false;
        PoseV2ShopPlayStats& stats = g_poseV2ShopPlay;
        ++stats.checks;
        if (std::string(phase) != "dress")
            ++stats.checksShown;
        static std::vector<replay::metaped::ShopItem> have;
        replay::entity::appearance::ReadShopItems(puppet.clone, have);
        static const std::vector<replay::metaped::ShopItem> kNone;
        const replay::metaped::ShopDiff diff =
            replay::metaped::CompareShopItems(recorded ? wanted.state->shopItems : kNone, have);
        stats.itemsRecorded += diff.recorded;
        stats.matched += diff.matched;
        stats.stateDiffer += diff.stateDiffer;
        stats.missing += diff.missing;
        stats.extra += diff.extra;
        stats.exactBodies += diff.Exact() ? 1u : 0u;
        bool fallback = false;
        if (shopDress)
        {
            if (match.Exact())
                ++stats.layersExactAfterShop;
            else
            {
                ++stats.layersNotExactAfterShop;
                fallback = !PoseV2Wear(puppet).assets.empty();
            }
        }
        if (g_poseV2ShopPlayLogs.Take(200))
            Ctx().log << "POSE_V2_PLAYBACK_SHOP_ITEMS,phase=" << phase << ",id=" << replay::entity::IdText(puppet.packet.id)
                      << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex
                      << puppet.packet.modelHash << std::dec
                      << ",dressed_by=" << replay::metaped::DressPathText(puppet.dressedBy) << ",why=" << puppet.dressWhy
                      << ",applied=" << puppet.shopItemsApplied << ",recorded=" << diff.recorded
                      << ",puppet=" << diff.puppet << ",matched=" << diff.matched << ",state_differ=" << diff.stateDiffer
                      << ",missing=" << diff.missing << ",extra=" << diff.extra << ",first_missing=0x" << std::hex
                      << diff.firstMissing << ",first_differ=0x" << diff.firstDiffer << std::dec
                      << ",layers_recorded=" << match.recorded << ",layers_present=" << match.present
                      << ",layers_puppet=" << match.pedLayers << ",layers_exact=" << (match.Exact() ? 1 : 0)
                      << ",shop_exact=" << (diff.Exact() ? 1 : 0) << ",fallback=" << (fallback ? 1 : 0)
                      << ",playhead_ms=" << timeMs << '\n';
        if (!fallback)
            return false;
        ++stats.fallbacks;
        puppet.shopRefused = true;
        DressPoseV2Body(puppet, PoseV2Wear(puppet), timeMs, false);
        ApplyPoseV2Metaped(puppet, timeMs, true); // the layers reset the look
        return true;
    }

    // One line per body leaving Dressing: recorded against applied, and the
    // QA rule appearance_not_recorded.
    void LogPoseV2Dressed(const replay::playback::Puppet& puppet, const replay::entity::appearance::Match& match,
        bool timedOut, std::uint32_t timeMs)
    {
        ++g_poseV2MetapedPlay.dressed;
        g_poseV2MetapedPlay.lookahead += puppet.dressLookaheadMs ? 1u : 0u;
        const replay::metaped::Track* track = replay::metaped::FindTrack(Ctx().clip.metapeds, puppet.packet.id.Packed());
        const replay::metaped::Wanted wanted = PoseV2MetapedAt(puppet, timeMs);
        replay::metaped::engine::Check check;
        if (wanted.state)
        {
            ++g_poseV2MetapedPlay.withState;
            check = replay::metaped::engine::Verify(puppet.clone, *wanted.state);
            if (!(wanted.state->flags & replay::metaped::kHasExpressions) || !check.read)
                ++g_poseV2MetapedPlay.unverified;
            else if (check.expressions.Exact())
                ++g_poseV2MetapedPlay.exact;
            else
                ++g_poseV2MetapedPlay.notExact;
        }
        replay::metaped::NotRecordedFacts facts;
        const replay::entity::Appearance& wear = PoseV2Wear(puppet);
        facts.layers = wear.assets.size();
        facts.outfit = wear.outfit;
        facts.clipHasMetaped = !Ctx().clip.metapeds.empty();
        facts.entityHasMetaped = track && !track->states.empty();
        const char* why = replay::metaped::NotRecordedWhy(facts);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_APPEARANCE"
            << ",id=" << replay::entity::IdText(puppet.packet.id)
            << ",outfit=0x" << std::hex << facts.outfit << std::dec
            << ",outfit_applied=" << (puppet.outfitApplied ? 1 : 0)
            << ",layers_applied=" << (puppet.layersApplied ? 1 : 0)
            << ",recorded_layers=" << match.recorded
            << ",present=" << match.present
            << ",puppet_layers=" << match.pedLayers
            << ",exact=" << (match.Exact() ? 1 : 0)
            << ",ready_to_render=" << (timedOut ? 0 : 1)
            << ",dressed_from=" << puppet.dressFrom
            << ",lookahead_ms=" << puppet.dressLookaheadMs
            // Shop_items|layers (outfit: the equip alone), why the layers, the
            // shop items applied, the shop path refused after a dress whose
            // layers were not the recorded ones.
            << ",dressed_by=" << (puppet.outfitApplied && !puppet.layersApplied
                                       ? "outfit" : replay::metaped::DressPathText(puppet.dressedBy))
            << ",dress_why=" << puppet.dressWhy
            << ",shop_items=" << puppet.shopItemsApplied
            << ",shop_refused=" << (puppet.shopRefused ? 1 : 0)
            << '\n';
        if (g_poseV2MetapedPlayLogs.Take(200))
        {
            const replay::logfmt::FormatScope logFormat(log);
            log << std::fixed << std::setprecision(3) << "POSE_V2_PLAYBACK_METAPED,id=" << replay::entity::IdText(puppet.packet.id)
                << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex << puppet.packet.modelHash
                << std::dec << ",state=" << (wanted.state ? replay::metaped::SourceText(wanted.state->source) : "none")
                << ",state_ms=" << (wanted.state ? wanted.state->timeMs : 0u) << ",state_lookahead_ms=" << wanted.lookaheadMs
                << ",layers_recorded=" << match.recorded << ",layers_present=" << match.present
                << ",layers_puppet=" << match.pedLayers;
            if (wanted.state)
            {
                const replay::metaped::State& st = *wanted.state;
                log << ",expr_recorded=" << st.expressions.size() << ",expr_set=" << puppet.metapedSet
                    << ",expr_read=" << (check.read ? 1 : 0) << ",expr_differ=" << check.expressions.differ
                    << ",expr_missing=" << check.expressions.missing << ",expr_max_delta=" << check.expressions.maxDelta
                    << ",expr_worst=0x" << std::hex << check.expressions.worstId << std::dec
                    << ",scale_recorded=" << ((st.flags & replay::metaped::kHasScale) ? st.scale : 0.0f)
                    << ",scale_puppet=" << (check.scaleRead ? check.scale : 0.0f)
                    << ",clean_recorded=" << st.cleanliness << ",clean_puppet=" << check.cleanliness
                    << ",retried=" << (puppet.metapedRetried ? 1 : 0);
            }
            log << ",verdict=" << (why ? "appearance_not_recorded" : !wanted.state ? "layers_only"
                                       : check.read && !check.expressions.Exact() ? "expressions_differ" : "ok")
                << ",playhead_ms=" << timeMs << '\n';
        }
        if (why)
        {
            ++g_poseV2MetapedPlay.notRecorded;
            if (g_poseV2NotRecordedLogs.Take(64))
                log << "POSE_V2_APPEARANCE_NOT_RECORDED,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex
                    << puppet.packet.modelHash << std::dec << ",why=" << why
                    << ",create_layers=" << puppet.packet.appearance.assets.size() << ",dressed_from=" << puppet.dressFrom
                    << ",metaped_states=" << (track ? track->states.size() : 0u) << ",playhead_ms=" << timeMs << '\n';
        }
        log.flush();
    }

    bool PoseV2RecordHasSuccessor(replay::entity::ReplayId id, std::uint32_t recordId)
    {
        return replay::entity::RecordHasSuccessor(Ctx().clip.segments, id, recordId);
    }

    std::uint32_t PoseV2RecordForBones(
        replay::entity::ReplayId id, std::uint32_t timeMs, std::uint32_t bones)
    {
        return replay::entity::RecordForBones(Ctx().clip.segments, Ctx().clip.FindCreate(id.Packed()),
            id, timeMs, bones, PoseV2RecordSampled);
    }

    std::uint32_t PoseV2NeighbourRecord(
        replay::entity::ReplayId id, std::uint32_t timeMs, std::uint32_t bones,
        std::uint32_t& neighbourBones)
    {
        return replay::entity::NeighbourRecord(Ctx().clip.segments, Ctx().clip.FindCreate(id.Packed()),
            id, timeMs, bones, PoseV2RecordSampled, neighbourBones);
    }

    bool PoseV2RecordedWorldAt(
        replay::entity::ReplayId id, std::uint32_t timeMs, replay::pose::WorldTransform& world)
    {
        static replay::pose::PoseSample sample;
        const replay::entity::PoseSegment* segment = PoseV2SegmentAt(id, timeMs);
        const std::uint32_t recordId = segment ? segment->recordId : id.Packed();
        return replay::pose::capture::SampleTrack(recordId, timeMs, sample) &&
            SampleWorldTransform(sample, world);
    }

    // A vehicle puppet deleted to be created again (its windows, parts or burn
    // going back on a scrub): the pose override and the vegetation slot are
    // released first, as when a puppet is deleted for good (otherwise the pose
    // detour goes on writing into the freed skeleton until the new one comes).
    void ReleaseVehicleForRebuild(replay::playback::Puppet& puppet)
    {
        ++g_poseV2Counters.vehicleRebuilds;
        ReleasePoseV2Override(puppet);
        // Its pieces break off the new puppet again.
        replay::objects::ReleaseVehiclePieces(puppet.packet.id);
        if (!puppet.clone)
            return;
        // Its draft animals come off the harness first: a horse still on the
        // deleted wagon's harness refuses the new one (every attach fails).
        for (replay::playback::Puppet& child : Ctx().puppets.list)
            if (child.attached && child.clone && child.attachment.parent == puppet.packet.id &&
                child.attachment.kind != replay::entity::AttachmentKind::VehicleSeat)
            {
                replay::pose::playback::DetachDraftHarness(puppet.clone, child.clone);
                child.attached = false;
                child.attachFailures = 0;
            }
        replay::vegetation::Remove(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)));
        replay::collision::Forget(replay::entities::AddressOf(puppet.clone));
        Ctx().recorder.ignored.push_back({puppet.clone, GetTickCount64() + 2000});
        replay::pose::playback::DeleteVehiclePuppet(puppet.clone);
    }
    namespace
    {

    void ForgetPoseV2Signature(replay::playback::Puppet& puppet)
    {
        if (puppet.sigRequest >= 0)
            replay::pose::capture::CancelSkeletonInfo(puppet.sigRequest);
        puppet.sigRequest = -1;
        puppet.sigSkeleton = nullptr;
        puppet.sigBones = 0;
        puppet.sigDeadline = 0;
        puppet.sigHave = false;
        puppet.sigFailed = false;
        puppet.sigParents.clear();
        puppet.sigLocals.clear();
        puppet.sigGroups = 0;
        puppet.sigVerifiedKey = 0;
        puppet.sigRecheckAt = 0;
        puppet.sigRechecks = 0;
    }
    }

    void ReleasePoseV2Override(replay::playback::Puppet& puppet, bool keepSignature)
    {
        if (!keepSignature)
            ForgetPoseV2Signature(puppet);
        else
        {
            puppet.sigVerifiedKey = 0;
            puppet.sigRecheckAt = 0;
            puppet.sigRechecks = 0;
        }
        if (puppet.overrideSlot >= 0)
        {
            replay::pose::capture::DisarmOverride(puppet.overrideSlot);
            g_poseV2OverrideInUse[puppet.overrideSlot] = false;
        }
        puppet.overrideSlot = -1;
        puppet.armedSkeleton = nullptr;
        puppet.armedBones = 0;
        puppet.armedRecordId = 0;
        puppet.bodyBones = 0;
        puppet.prefixHold.End();
        // The skeleton map goes with the arm (a request not taken yet is given
        // back).
        if (puppet.mapRequest >= 0)
            replay::pose::capture::CancelSkeletonInfo(puppet.mapRequest);
        puppet.mapRequest = -1;
        puppet.mapSkeleton = nullptr;
        puppet.mapBones = 0;
        puppet.mapDeadline = 0;
        puppet.mapped = false;
        puppet.sigMapped = false;
        puppet.sampleReuse.Invalidate();
    }
    namespace
    {

    // One POSE_V2_PLAYBACK_ARM line per arm: mode exact (a record with the
    // skeleton's bones), mapped (the skeleton map) or refused (neither: the
    // puppet stays hidden), with the bones posed from the record, left at rest
    // and dropped.
    void LogPoseV2Arm(const replay::playback::Puppet& puppet, bool armed, const char* mode, const std::string& error,
        const replay::skeleton::ResolvedPoseSkeleton& resolved, std::uint32_t recordId,
        const replay::pose::SkeletonMap* map, std::uint32_t engineGroups, std::uint32_t timeMs)
    {
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_ARM"
            << ",id=" << replay::entity::IdText(puppet.packet.id)
            << ",status=" << (armed ? "armed" : "failed")
            << ",mode=" << mode
            << ",error=" << (armed ? "none" : error.c_str())
            << ",slot=" << puppet.overrideSlot
            << ",clone_bones=" << resolved.boneCount
            << ",track_bones=" << puppet.boneCount;
        if (map)
        {
            log << ",matched=" << map->matched << ",rest=" << map->rest << ",dropped=" << map->dropped
                << ",groups=" << map->targetGroups << '/' << map->sourceGroups
                << ",matched_groups=" << map->matchedGroups << ",engine_groups=" << engineGroups;
        }
        else
        {
            const std::uint32_t matched = armed ? resolved.boneCount : 0u;
            log << ",matched=" << matched << ",rest=" << (armed ? 0u : resolved.boneCount)
                << ",dropped=" << (armed ? 0u : puppet.boneCount);
        }
        log << ",record=0x" << std::hex << recordId
            << ",segment_record=0x" << puppet.recordId
            << ",skeleton=0x" << reinterpret_cast<std::uintptr_t>(resolved.skeleton)
            << ",fragment_owner=0x" << reinterpret_cast<std::uintptr_t>(resolved.fragmentOwner)
            << std::dec
            << ",playhead_ms=" << timeMs
            << '\n';
        // One line per arm, the LOD settings the puppet carries (the multiplier
        // read back; puppet_lod.h).
        if (armed && PoseV2LodBody(puppet) && puppet.clone)
            log << "POSE_V2_PLAYBACK_LOD,id=" << replay::entity::IdText(puppet.packet.id)
                << ",multiplier=" << PED::_GET_PED_LOD_MULTIPLIER(puppet.clone)
                << ",prerender=" << (puppet.lod.clone == puppet.clone ? 1 : 0)
                << ",stale_ticks=" << puppet.lod.staleTicks << ",forced=" << puppet.lod.forcedTicks
                << ",playhead_ms=" << timeMs << '\n';
        log.flush();
    }
    }

    // A mapped puppet's pose for the slot, from the record's sample.
    bool PublishPoseV2Mapped(replay::playback::Puppet& puppet, const replay::pose::PoseSample& sample)
    {
        const replay::pose::SkeletonMap& map = puppet.map;
        if (puppet.overrideSlot < 0 || sample.locals.size() != map.sourceBones ||
            sample.objects.size() != map.sourceBones || puppet.mapRest.size() != map.targetBones)
            return false;
        g_poseV2MappedSample.locals.resize(map.targetBones);
        g_poseV2MappedSample.objects.resize(map.targetBones);
        if (!replay::pose::ApplySkeletonMap(map, sample.locals.data(), sample.objects.data(), map.sourceBones,
                puppet.mapRest.data(), g_poseV2MappedSample.locals.data(), g_poseV2MappedSample.objects.data()))
            return false;
        return replay::pose::capture::PublishOverrideSample(
            static_cast<std::uint32_t>(puppet.overrideSlot), g_poseV2MappedSample, map.targetBones);
    }
    namespace
    {

    enum class PoseV2MapArm { Pending, Armed, Refused };
    constexpr ULONGLONG kPoseV2MapWaitMs = 1500; // the detour reads the puppet's table within a frame or two

    // Arms `puppet.map` (built, not refused) on the slot: the record's pose
    // through the map, the puppet-only bones in `rest` (the puppet's own
    // locals, swapped in). Also the arm of a skeleton with the record's bone
    // count and another signature.
    bool ArmPoseV2WithMap(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved, int slot,
        std::uint32_t sourceRecord, std::vector<replay::pose::BoneQts>& rest, std::uint32_t engineGroups, std::uint32_t timeMs,
        std::string& error)
    {
        if (slot < 0)
        {
            error = "no_override_slot";
            return false;
        }
        if (!replay::pose::capture::ArmOverride(slot, resolved.skeleton, resolved.boneCount, error))
            return false;
        puppet.overrideSlot = slot;
        puppet.armedSkeleton = resolved.skeleton;
        puppet.armedBones = resolved.boneCount;
        puppet.armedRecordId = sourceRecord;
        puppet.sampleReuse.Invalidate();
        puppet.mapped = true;
        puppet.mapSegmentRecord = puppet.recordId; // the segment the map was armed in
        puppet.mapRest.swap(rest);
        g_poseV2OverrideInUse[slot] = true;
        g_poseV2WriteCheckAt[slot] = GetTickCount64() + 250u + 20u * static_cast<std::uint64_t>(slot);
        g_poseV2WriteChanged[slot] = false;
        ++g_poseV2MappedArms;
        if (replay::pose::capture::SampleTrack(sourceRecord, timeMs, g_poseV2Sample) &&
            PublishPoseV2Mapped(puppet, g_poseV2Sample))
        {
            replay::pose::capture::ApplyOverrideNow(static_cast<std::uint32_t>(slot));
        }
        LogPoseV2Arm(puppet, true, "mapped", error, resolved, sourceRecord, &puppet.map, engineGroups, timeMs);
        return true;
    }

    // No record has the skeleton's bone count and the layer retries are spent.
    // As a replay editor poses the bones both sides have and keeps the ped, the
    // record's pose goes on the groups both skeletons share (skeleton_map.h):
    // the detour reads the puppet's parent table and locals, then the map is
    // built and armed.
    PoseV2MapArm ArmPoseV2Mapped(replay::playback::Puppet& puppet,
        const replay::skeleton::ResolvedPoseSkeleton& resolved, int slot, std::uint32_t timeMs, std::string& error)
    {
        const ULONGLONG now = GetTickCount64();
        const bool sameTarget = puppet.mapDeadline != 0 && puppet.mapSkeleton == resolved.skeleton &&
            puppet.mapBones == resolved.boneCount;
        if (!sameTarget)
        {
            if (puppet.mapRequest >= 0)
                replay::pose::capture::CancelSkeletonInfo(puppet.mapRequest);
            puppet.mapRequest = -1;
            puppet.mapSkeleton = resolved.skeleton;
            puppet.mapBones = resolved.boneCount;
            puppet.mapDeadline = now + kPoseV2MapWaitMs;
        }
        if (puppet.mapRequest < 0)
        {
            puppet.mapRequest = replay::pose::capture::RequestSkeletonInfo(resolved.skeleton, resolved.boneCount);
            if (puppet.mapRequest < 0 && now > puppet.mapDeadline)
            {
                error = "map_no_info_slot";
                return PoseV2MapArm::Refused;
            }
            return PoseV2MapArm::Pending;
        }
        replay::pose::capture::SkeletonInfo info;
        const replay::pose::capture::SkeletonInfoState state =
            replay::pose::capture::PollSkeletonInfo(puppet.mapRequest, info);
        if (state == replay::pose::capture::SkeletonInfoState::Pending)
        {
            if (now <= puppet.mapDeadline)
                return PoseV2MapArm::Pending;
            replay::pose::capture::CancelSkeletonInfo(puppet.mapRequest);
            puppet.mapRequest = -1;
            error = "map_parents_timeout"; // no finalizer call for this skeleton
            return PoseV2MapArm::Refused;
        }
        puppet.mapRequest = -1;
        if (state == replay::pose::capture::SkeletonInfoState::Failed)
        {
            error = std::string("map_") + info.error;
            return PoseV2MapArm::Refused;
        }
        const std::uint32_t sourceRecord = puppet.recordId ? puppet.recordId : puppet.packet.id.Packed();
        static std::vector<std::int32_t> sourceParents;
        static std::vector<std::int32_t> targetParents;
        // The layout that plays at the playhead, not the record's last one: a
        // record with a layout change mid-clip would map the earlier frames
        // with the later parents.
        std::uintptr_t layoutKey = 0;
        if (!replay::pose::capture::TrackLayoutSignatureAt(sourceRecord, timeMs, &sourceParents, nullptr, layoutKey))
        {
            error = "map_no_layout";
            return PoseV2MapArm::Refused;
        }
        targetParents.assign(info.parents.begin(), info.parents.end());
        puppet.map = replay::pose::BuildSkeletonMap(sourceParents.data(), static_cast<std::uint32_t>(sourceParents.size()),
            targetParents.data(), static_cast<std::uint32_t>(targetParents.size()));
        if (puppet.map.mode == replay::pose::SkeletonMapMode::Refused)
        {
            error = std::string("map_") + puppet.map.refuseReason;
            LogPoseV2Arm(puppet, false, "refused", error, resolved, sourceRecord, &puppet.map, info.groups, timeMs);
            puppet.armFailureLogged = true; // logged here, with the map's counts
            return PoseV2MapArm::Refused;
        }
        return ArmPoseV2WithMap(puppet, resolved, slot, sourceRecord, info.locals, info.groups, timeMs, error)
            ? PoseV2MapArm::Armed
            : PoseV2MapArm::Refused;
    }

    // The override armed on the skeleton for the record with its bones,
    // and the pose at the playhead written at once.
    bool ArmPoseV2Exact(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved, int slot,
        std::uint32_t recordId, std::uint32_t timeMs, std::string& error)
    {
        if (!replay::pose::capture::ArmOverride(slot, resolved.skeleton, resolved.boneCount, error))
            return false;
        puppet.overrideSlot = slot;
        puppet.armedSkeleton = resolved.skeleton;
        puppet.armedBones = resolved.boneCount;
        puppet.armedRecordId = recordId;
        puppet.prefixHold.End();
        // A body: the record's body group, for a rebuild in place to fewer
        // bones (prefix_hold.h).
        puppet.bodyBones = 0;
        if (PoseV2Body(puppet))
        {
            static std::vector<std::int32_t> layoutParents;
            if (replay::pose::capture::TrackLayoutParents(recordId, layoutParents) &&
                layoutParents.size() == resolved.boneCount)
                puppet.bodyBones = replay::prefixhold::BodyBones(layoutParents.data(), resolved.boneCount);
        }
        replay::pose::capture::SetOverridePrefixBones(static_cast<std::uint32_t>(slot), puppet.bodyBones);
        puppet.sampleReuse.Invalidate(); // a new arm samples again
        g_poseV2OverrideInUse[slot] = true;
        // Staggered by slot, so the checks of a full scene do not land in the
        // same tick.
        g_poseV2WriteCheckAt[slot] = GetTickCount64() + 250u + 20u * static_cast<std::uint64_t>(slot);
        g_poseV2WriteChanged[slot] = false;
        if (puppet.packet.kind == replay::entity::EntityKind::Vehicle)
            replay::pose::capture::SetOverrideLate(static_cast<std::uint32_t>(slot), true);
        if (replay::pose::capture::SampleTrack(recordId, timeMs, g_poseV2Sample))
        {
            puppet.hatLayout = 0; // a new arm: the hat fix for this record's layout
            PreparePoseV2HatFix(puppet, recordId, timeMs, g_poseV2Sample);
            FixPoseV2Hat(puppet, g_poseV2Sample);
            if (replay::pose::capture::PublishOverride(slot, g_poseV2Sample.locals.data(),
                    g_poseV2Sample.objects.data(), resolved.boneCount))
                replay::pose::capture::ApplyOverrideNow(static_cast<std::uint32_t>(slot));
        }
        // The layout at the playhead is looked at again every half second (a
        // layout change compares the skeleton again).
        puppet.sigRecheckAt = GetTickCount64() + 500;
        return true;
    }

    // The puppet's engine parent table for its current skeleton
    // (skeleton_signature.h), read by the finalizer detour after a request (the
    // skeleton map's path, rung 2, read only), kept while the skeleton and its
    // bone count stay. Pending until the detour read it, within
    // kPoseV2MapWaitMs; Failed (with the reason) when it could not be read: not
    // asked again for that skeleton.
    enum class PoseV2SignatureInfo { Pending, Ready, Failed };
    PoseV2SignatureInfo PoseV2PuppetParents(replay::playback::Puppet& puppet,
        const replay::skeleton::ResolvedPoseSkeleton& resolved, const char*& error)
    {
        const ULONGLONG now = GetTickCount64();
        const bool sameSkeleton = puppet.sigSkeleton == resolved.skeleton && puppet.sigBones == resolved.boneCount;
        if (sameSkeleton && puppet.sigHave)
            return PoseV2SignatureInfo::Ready;
        if (sameSkeleton && puppet.sigFailed)
        {
            error = "info_failed";
            return PoseV2SignatureInfo::Failed;
        }
        if (!sameSkeleton || puppet.sigDeadline == 0)
        {
            if (puppet.sigRequest >= 0)
                replay::pose::capture::CancelSkeletonInfo(puppet.sigRequest);
            puppet.sigRequest = -1;
            puppet.sigSkeleton = resolved.skeleton;
            puppet.sigBones = resolved.boneCount;
            puppet.sigHave = false;
            puppet.sigFailed = false;
            puppet.sigDeadline = now + kPoseV2MapWaitMs;
        }
        if (puppet.sigRequest < 0)
        {
            puppet.sigRequest = replay::pose::capture::RequestSkeletonInfo(resolved.skeleton, resolved.boneCount);
            if (puppet.sigRequest < 0 && now > puppet.sigDeadline)
            {
                puppet.sigFailed = true;
                error = "no_info_slot";
                return PoseV2SignatureInfo::Failed;
            }
            return PoseV2SignatureInfo::Pending;
        }
        replay::pose::capture::SkeletonInfo info;
        const replay::pose::capture::SkeletonInfoState state = replay::pose::capture::PollSkeletonInfo(puppet.sigRequest, info);
        if (state == replay::pose::capture::SkeletonInfoState::Pending)
        {
            if (now <= puppet.sigDeadline)
                return PoseV2SignatureInfo::Pending;
            replay::pose::capture::CancelSkeletonInfo(puppet.sigRequest);
            puppet.sigRequest = -1;
            puppet.sigFailed = true;
            ++g_poseV2Signature.timeouts;
            error = "info_timeout";
            return PoseV2SignatureInfo::Failed;
        }
        puppet.sigRequest = -1;
        if (state == replay::pose::capture::SkeletonInfoState::Failed || info.parents.size() != resolved.boneCount)
        {
            puppet.sigFailed = true;
            error = state == replay::pose::capture::SkeletonInfoState::Failed ? info.error : "info_count";
            return PoseV2SignatureInfo::Failed;
        }
        puppet.sigParents.swap(info.parents);
        puppet.sigLocals.swap(info.locals);
        puppet.sigGroups = info.groups;
        puppet.sigHave = true;
        return PoseV2SignatureInfo::Ready;
    }

    // One line per verdict and layout/skeleton (a mismatch always): with
    // Debug=0 only the mismatch stays (its anomaly text is a failure marker,
    // log_policy.h), and the QA kit makes it skeleton_signature_mismatch.
    void LogPoseV2Signature(replay::playback::Puppet& puppet, const char* phase, const char* verdict, const char* why,
        const char* action, const replay::pose::SignatureCompare& compare, std::uint32_t recordId, std::uintptr_t layoutKey,
        std::uint8_t* skeleton, std::uint32_t timeMs)
    {
        const bool mismatch = std::strcmp(verdict, "differ") == 0;
        if (!mismatch && puppet.sigLoggedKey == layoutKey && puppet.sigLoggedSkeleton == skeleton)
            return;
        puppet.sigLoggedKey = layoutKey;
        puppet.sigLoggedSkeleton = skeleton;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SKELETON_SIGNATURE,id=" << replay::entity::IdText(puppet.packet.id)
            << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex << puppet.packet.modelHash
            << std::dec << ",phase=" << phase << ",verdict=" << verdict;
        if (mismatch)
            log << ",anomaly=skeleton_signature_mismatch";
        log << ",why=" << (why ? why : "none") << ",action=" << action << ",bones=" << compare.bones
            << ",compared=" << compare.compared << ",differ=" << compare.differ << ",first_bone=" << compare.firstDiff
            << ",last_bone=" << compare.lastDiff << ",ambiguous=" << compare.ambiguous << ",skipped=" << compare.skipped;
        if (mismatch)
            log << ",matched=" << puppet.map.matched << ",rest=" << puppet.map.rest << ",dropped=" << puppet.map.dropped
                << ",groups=" << puppet.map.targetGroups << '/' << puppet.map.sourceGroups
                << ",matched_groups=" << puppet.map.matchedGroups << ",map=" << replay::pose::SkeletonMapModeText(puppet.map.mode)
                << ",map_reason=" << puppet.map.refuseReason;
        log << ",record=0x" << std::hex << recordId << ",skeleton=0x" << reinterpret_cast<std::uintptr_t>(skeleton) << std::dec
            << ",playhead_ms=" << timeMs << '\n';
        if (mismatch)
            log.flush();
    }

    // The record layout at the playhead against the puppet's table. Pending
    // while the detour reads the table; Same also when the layout cannot vouch
    // or the table could not be read (`why` says which: armed as if unchecked).
    // Rebuilt: the parents agree (or cannot say) but the record demoted more
    // than kSignatureDifferLimit bones since an earlier layout (`rebuilt` flags
    // them): those bones are left to the puppet.
    enum class PoseV2SignatureResult { Pending, Same, Unknown, Differ, Rebuilt };
    PoseV2SignatureResult ComparePoseV2Signature(replay::playback::Puppet& puppet,
        const replay::skeleton::ResolvedPoseSkeleton& resolved, std::uint32_t recordId, std::uint32_t timeMs,
        std::vector<std::int32_t>& parents, replay::pose::SignatureCompare& compare, std::uintptr_t& layoutKey,
        const char*& why, std::vector<std::uint8_t>& rebuilt)
    {
        static std::vector<std::uint8_t> ambiguous;
        why = nullptr;
        compare = {};
        compare.bones = resolved.boneCount;
        std::uint32_t rebuiltCount = 0;
        if (!replay::pose::capture::TrackLayoutSignatureAt(recordId, timeMs, &parents, &ambiguous, layoutKey, &rebuilt))
            why = "no_layout";
        else
        {
            why = replay::pose::SignatureUnusable(parents, ambiguous);
            rebuiltCount = replay::pose::CountFlags(rebuilt);
        }
        const bool rebuiltBlock = rebuiltCount > replay::pose::kSignatureDifferLimit && rebuilt.size() == resolved.boneCount;
        if (!why || rebuiltBlock)
        {
            const char* infoError = "none";
            const PoseV2SignatureInfo info = PoseV2PuppetParents(puppet, resolved, infoError);
            if (info == PoseV2SignatureInfo::Pending)
                return PoseV2SignatureResult::Pending;
            if (info == PoseV2SignatureInfo::Failed)
                why = infoError;
            else if (!why)
            {
                compare = replay::pose::CompareSkeletonSignature(parents, ambiguous, puppet.sigParents.data(),
                    static_cast<std::uint32_t>(puppet.sigParents.size()));
                if (compare.verdict == replay::pose::SignatureVerdict::Unknown)
                    why = compare.reason;
            }
            if (info == PoseV2SignatureInfo::Ready && rebuiltBlock &&
                compare.verdict != replay::pose::SignatureVerdict::Differ)
            {
                why = "record_rebuilt";
                compare.differ = rebuiltCount;
                compare.firstDiff = compare.lastDiff = -1;
                for (std::size_t bone = 0; bone < rebuilt.size(); ++bone)
                {
                    if (!rebuilt[bone])
                        continue;
                    if (compare.firstDiff < 0)
                        compare.firstDiff = static_cast<std::int32_t>(bone);
                    compare.lastDiff = static_cast<std::int32_t>(bone);
                }
                return PoseV2SignatureResult::Rebuilt;
            }
        }
        if (why)
            return PoseV2SignatureResult::Unknown;
        return compare.verdict == replay::pose::SignatureVerdict::Differ ? PoseV2SignatureResult::Differ
                                                                          : PoseV2SignatureResult::Same;
    }

    // The arm of a body whose skeleton has the record's bone count.
    enum class PoseV2SignatureGate { Exact, Pending, Mapped, Refused };
    PoseV2SignatureGate GatePoseV2Signature(replay::playback::Puppet& puppet,
        const replay::skeleton::ResolvedPoseSkeleton& resolved, std::uint32_t recordId, int slot, std::uint32_t timeMs,
        std::string& error)
    {
        if (puppet.packet.kind != replay::entity::EntityKind::Ped && puppet.packet.kind != replay::entity::EntityKind::Horse)
            return PoseV2SignatureGate::Exact;
        static std::vector<std::int32_t> parents;
        static std::vector<std::uint8_t> rebuilt;
        replay::pose::SignatureCompare compare;
        std::uintptr_t layoutKey = 0;
        const char* why = nullptr;
        const PoseV2SignatureResult result =
            ComparePoseV2Signature(puppet, resolved, recordId, timeMs, parents, compare, layoutKey, why, rebuilt);
        if (result == PoseV2SignatureResult::Pending)
            return PoseV2SignatureGate::Pending;
        if (result == PoseV2SignatureResult::Rebuilt)
        {
            // The original's skeleton was rebuilt mid-recording under the same
            // bone count (the capture demoted the block): the block's recorded
            // objects are another order's bones. Every other bone 1:1, the
            // block in the puppet's own pose.
            ++g_poseV2Signature.rebuilt;
            puppet.map = replay::pose::BuildRebuiltMap(puppet.sigParents.data(),
                static_cast<std::uint32_t>(puppet.sigParents.size()), rebuilt);
            std::vector<replay::pose::BoneQts> rest = puppet.sigLocals;
            if (puppet.map.mode != replay::pose::SkeletonMapMode::Mapped || rest.size() != resolved.boneCount ||
                !ArmPoseV2WithMap(puppet, resolved, slot, recordId, rest, puppet.sigGroups, timeMs, error))
            {
                ++g_poseV2Signature.refused;
                if (puppet.map.mode != replay::pose::SkeletonMapMode::Mapped)
                    error = std::string("signature_map_") + puppet.map.refuseReason;
                puppet.sigRefusedKey = layoutKey;
                LogPoseV2Signature(puppet, "arm", "differ", why, "refused", compare, recordId, layoutKey, resolved.skeleton, timeMs);
                return PoseV2SignatureGate::Refused;
            }
            ++g_poseV2Signature.mapped;
            puppet.sigMapped = true;
            puppet.sigVerifiedKey = layoutKey;
            puppet.sigRecheckAt = GetTickCount64() + 500;
            LogPoseV2Signature(puppet, "arm", "differ", why, "mapped", compare, recordId, layoutKey, resolved.skeleton, timeMs);
            return PoseV2SignatureGate::Mapped;
        }
        if (result != PoseV2SignatureResult::Differ)
        {
            const bool same = result == PoseV2SignatureResult::Same;
            ++(same ? g_poseV2Signature.same : g_poseV2Signature.unknown);
            LogPoseV2Signature(puppet, "arm", same ? "same" : "unknown", why, "exact", compare, recordId, layoutKey,
                resolved.skeleton, timeMs);
            puppet.sigVerifiedKey = layoutKey;
            return PoseV2SignatureGate::Exact;
        }
        // Another skeleton with the record's bone count: the record's pose
        // goes through the skeleton map (groups matched by size and shape,
        // skeleton_map.h), the mod's bone remap; there is no bone name or
        // tag table to remap by. Refused by the map: not armed, hidden.
        static std::vector<std::int32_t> targetParents;
        targetParents.assign(puppet.sigParents.begin(), puppet.sigParents.end());
        puppet.map = replay::pose::BuildSkeletonMap(parents.data(), static_cast<std::uint32_t>(parents.size()),
            targetParents.data(), static_cast<std::uint32_t>(targetParents.size()));
        if (puppet.map.mode == replay::pose::SkeletonMapMode::Exact)
        {
            // Bone for bone the same groups (the differing parents were in
            // bones the map does not look at): exact.
            ++g_poseV2Signature.same;
            LogPoseV2Signature(puppet, "arm", "same", "map_identity", "exact", compare, recordId, layoutKey,
                resolved.skeleton, timeMs);
            puppet.sigVerifiedKey = layoutKey;
            return PoseV2SignatureGate::Exact;
        }
        ++g_poseV2Signature.differ;
        if (puppet.map.mode == replay::pose::SkeletonMapMode::Refused)
        {
            ++g_poseV2Signature.refused;
            error = std::string("signature_map_") + puppet.map.refuseReason;
            puppet.sigRefusedKey = layoutKey;
            LogPoseV2Signature(puppet, "arm", "differ", nullptr, "refused", compare, recordId, layoutKey,
                resolved.skeleton, timeMs);
            return PoseV2SignatureGate::Refused;
        }
        std::vector<replay::pose::BoneQts> rest = puppet.sigLocals; // the cache stays for a recheck
        if (rest.size() != resolved.boneCount ||
            !ArmPoseV2WithMap(puppet, resolved, slot, recordId, rest, puppet.sigGroups, timeMs, error))
        {
            ++g_poseV2Signature.refused;
            if (rest.size() != resolved.boneCount)
                error = "signature_map_no_rest";
            puppet.sigRefusedKey = layoutKey;
            LogPoseV2Signature(puppet, "arm", "differ", error.c_str(), "refused", compare, recordId, layoutKey,
                resolved.skeleton, timeMs);
            return PoseV2SignatureGate::Refused;
        }
        ++g_poseV2Signature.mapped;
        puppet.sigMapped = true;
        puppet.sigVerifiedKey = layoutKey;
        puppet.sigRecheckAt = GetTickCount64() + 500;
        LogPoseV2Signature(puppet, "arm", "differ", nullptr, "mapped", compare, recordId, layoutKey, resolved.skeleton, timeMs);
        return PoseV2SignatureGate::Mapped;
    }
    }

    // An exact-armed body compared again, without dropping its pose, when the
    // record's layout at the playhead changes (the engine rebuilt a group
    // mid-recording) and after layers were applied on it (a rebuild in place
    // keeps the pointer and the bone count). A mismatch releases the arm
    // keeping the table, and the arm in the same tick goes through the map.
    // True when it released the arm.
    bool RecheckPoseV2Signature(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        std::uintptr_t layoutKey = 0;
        if (!replay::pose::capture::TrackLayoutSignatureAt(puppet.armedRecordId, timeMs, nullptr, nullptr, layoutKey))
        {
            puppet.sigRecheckAt = now + 1000;
            return false;
        }
        if (layoutKey == puppet.sigVerifiedKey && puppet.sigRechecks == 0)
        {
            puppet.sigRecheckAt = now + 500;
            return false;
        }
        if (puppet.sigMapped)
        {
            // Armed through a signature map: built for the layout it was
            // armed on. Another layout at the playhead (a seek back before a
            // rebuild, or past one) arms again from the kept table.
            if (layoutKey == puppet.sigVerifiedKey)
            {
                puppet.sigRecheckAt = now + 500;
                return false;
            }
            ++g_poseV2Signature.rechecks;
            Ctx().log << "POSE_V2_PLAYBACK_REARM,id=" << replay::entity::IdText(puppet.packet.id)
                      << ",reason=signature_layout,clone_bones=" << puppet.armedBones << ",playhead_ms=" << timeMs << '\n';
            ReleasePoseV2Override(puppet, true);
            puppet.armFailureLogged = false;
            puppet.armDeadline = now + 3000;
            return true;
        }
        replay::skeleton::ResolvedPoseSkeleton resolved{};
        if (!replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved) || resolved.skeleton != puppet.armedSkeleton ||
            resolved.boneCount != puppet.armedBones)
        {
            puppet.sigRecheckAt = now + 100; // RefreshPoseV2Override re-arms a changed skeleton
            return false;
        }
        static std::vector<std::int32_t> parents;
        static std::vector<std::uint8_t> rebuilt;
        replay::pose::SignatureCompare compare;
        const char* why = nullptr;
        const PoseV2SignatureResult result =
            ComparePoseV2Signature(puppet, resolved, puppet.armedRecordId, timeMs, parents, compare, layoutKey, why, rebuilt);
        if (result == PoseV2SignatureResult::Pending)
        {
            puppet.sigRecheckAt = now; // next tick
            return false;
        }
        ++g_poseV2Signature.rechecks;
        if (puppet.sigRechecks)
            --puppet.sigRechecks;
        puppet.sigRecheckAt = now + (puppet.sigRechecks ? 1250u : 500u);
        if (result != PoseV2SignatureResult::Differ && result != PoseV2SignatureResult::Rebuilt)
        {
            LogPoseV2Signature(puppet, "recheck", result == PoseV2SignatureResult::Same ? "same" : "unknown", why, "keep",
                compare, puppet.armedRecordId, layoutKey, resolved.skeleton, timeMs);
            puppet.sigVerifiedKey = layoutKey;
            return false;
        }
        ++g_poseV2Signature.rearms;
        Ctx().log << "POSE_V2_PLAYBACK_REARM,id=" << replay::entity::IdText(puppet.packet.id)
                  << ",reason=skeleton_signature,why=" << (why ? why : "none") << ",differ=" << compare.differ << ",first_bone=" << compare.firstDiff
                  << ",last_bone=" << compare.lastDiff << ",clone_bones=" << resolved.boneCount
                  << ",playhead_ms=" << timeMs << '\n';
        ReleasePoseV2Override(puppet, true);
        puppet.armFailureLogged = false;
        puppet.armDeadline = now + 3000;
        return true;
    }

    // Layers applied on an armed body may rebuild its skeleton in place (same
    // pointer, same bone count, other group order): its table is read again and
    // compared twice, 250 ms and 1.5 s later.
    void ScheduleSignatureRecheck(replay::playback::Puppet& puppet)
    {
        if (puppet.sigRequest >= 0)
            replay::pose::capture::CancelSkeletonInfo(puppet.sigRequest);
        puppet.sigRequest = -1;
        puppet.sigHave = false;
        puppet.sigFailed = false;
        puppet.sigDeadline = 0;
        puppet.sigRechecks = 2;
        puppet.sigRecheckAt = GetTickCount64() + 250;
    }
    namespace
    {

    // The puppet's layers with their categories (learned into the cache;
    // no_record_fallback.h), compared with `wanted`; a POSE_V2_LAYER_DIFF line
    // when `phase` is given (bounded). The recorded layers present on the
    // puppet.
    std::uint32_t PoseV2LayerDiff(const replay::playback::Puppet& puppet, const replay::entity::Appearance& wanted,
        const char* phase, std::uint32_t cloneBones, std::uint32_t timeMs)
    {
        static std::vector<replay::entity::MetaPedAsset> layers;
        static std::vector<Hash> categories;
        static std::vector<replay::norecord::PuppetLayer> puppetLayers;
        replay::entity::appearance::ReadWithCategories(puppet.clone, layers, categories);
        puppetLayers.clear();
        for (std::size_t index = 0; index < layers.size(); ++index)
        {
            const std::uint32_t category = index < categories.size() ? static_cast<std::uint32_t>(categories[index]) : 0u;
            g_noRecordCategories.Learn(layers[index].drawable, category);
            puppetLayers.push_back({layers[index], category});
        }
        const replay::norecord::LayerDiff diff = replay::norecord::DiffLayers(wanted.assets, puppetLayers, g_noRecordCategories);
        if (phase && g_noRecordLogs.Take(96))
        {
            ++g_noRecord.diffs;
            Ctx().log << "POSE_V2_LAYER_DIFF,id=" << replay::entity::IdText(puppet.packet.id) << ",phase=" << phase
                      << ",record=0x" << std::hex << puppet.recordId << std::dec << ",recorded=" << diff.recorded
                      << ",present=" << diff.present << ",puppet=" << diff.puppet << ",shared_category=" << diff.sharedCategory
                      << ",clone_bones=" << cloneBones << ",track_bones=" << puppet.boneCount
                      << ",missing=" << replay::norecord::FormatMissing(diff, 8) << ",extra=" << replay::norecord::FormatExtra(diff, 8)
                      << ",categories_known=" << g_noRecordCategories.Size() << ",playhead_ms=" << timeMs << '\n';
        }
        return diff.present;
    }
    }

    // Before the layers of a segment or change go on a body: the categories
    // of the drawables it wears now (the old neckwear the new list still
    // names is learned here, while it is on the puppet).
    void LearnPoseV2Categories(const replay::playback::Puppet& puppet, const replay::entity::Appearance& next)
    {
        const bool unknown = std::any_of(next.assets.begin(), next.assets.end(),
            [](const replay::entity::MetaPedAsset& asset) { return !g_noRecordCategories.Knows(asset.drawable); });
        if (!unknown || !PoseV2Body(puppet))
            return;
        static std::vector<replay::entity::MetaPedAsset> layers;
        static std::vector<Hash> categories;
        replay::entity::appearance::ReadWithCategories(puppet.clone, layers, categories);
        for (std::size_t index = 0; index < layers.size() && index < categories.size(); ++index)
            g_noRecordCategories.Learn(layers[index].drawable, static_cast<std::uint32_t>(categories[index]));
    }
    namespace
    {

    // A body armed exact: the no-record steps for its record end.
    void NotePoseV2ExactArm(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        if (puppet.noRecord.active)
        {
            ++g_noRecord.exactAfter;
            if (g_noRecordLogs.Take(96))
                Ctx().log << "POSE_V2_NO_RECORD,id=" << replay::entity::IdText(puppet.packet.id)
                          << ",action=exact_after_retry,key=" << puppet.appearanceKey
                          << ",retries=" << puppet.noRecord.retries << ",bones=" << puppet.armedBones << ",record=0x" << std::hex << puppet.armedRecordId << std::dec
                          << ",playhead_ms=" << timeMs << '\n';
        }
        puppet.noRecord.Finish();
        puppet.lostToChange = false;
        puppet.noRecordMapRefused = false;
    }
    }

    // Once a tick while a body is mapped for want of a record: every
    // kStepMs the recorded layers again (until an attempt answers as the
    // one before), then the mapped pose stays. An attempt that builds a skeleton a record
    // has is armed exact by RefreshPoseV2Override (the mapped arm released).
    void StepPoseV2NoRecord(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < puppet.noRecord.dueAt || PoseV2KnockPending(puppet.packet.id))
            return;
        const std::uint32_t present = PoseV2LayerDiff(puppet, PoseV2Wear(puppet),
            puppet.noRecord.haveLast ? "after_step" : nullptr, puppet.armedBones, timeMs);
        const bool wasSame = puppet.noRecord.sameAnswer;
        const replay::norecord::Step step = puppet.noRecord.Next(puppet.armedBones, present, now);
        if (puppet.noRecord.sameAnswer && !wasSame)
            ++g_noRecord.sameAnswer;
        if (step == replay::norecord::Step::Wait)
            return;
        const replay::entity::Appearance* apply = nullptr;
        std::uint32_t key = puppet.appearanceKey;
        const replay::WantedAppearance wanted = PoseV2AppearanceAt(puppet, timeMs);
        if (step == replay::norecord::Step::Retry)
        {
            apply = wanted.appearance;
            key = wanted.key;
            puppet.appearanceKey = wanted.key;
            ++g_noRecord.retries;
        }
        if (apply)
        {
            SetPoseV2Wear(puppet, *apply); // what the puppet must wear now (the QA kit compares with it)
            DressPoseV2Body(puppet, *apply, timeMs, true); // shop items or layers
            ApplyPoseV2Metaped(puppet, timeMs, true);
        }
        if (g_noRecordLogs.Take(96))
            Ctx().log << "POSE_V2_NO_RECORD,id=" << replay::entity::IdText(puppet.packet.id)
                      << ",action=" << replay::norecord::StepText(step) << ",key=" << key
                      << ",layers=" << (apply ? apply->assets.size() : 0u) << ",retries=" << puppet.noRecord.retries
                      << ",same_answer=" << (puppet.noRecord.sameAnswer ? 1 : 0)
                      << ",clone_bones=" << puppet.armedBones
                      << ",track_bones=" << puppet.boneCount << ",present=" << present << ",playhead_ms=" << timeMs << '\n';
    }

    void ArmPoseV2Puppet(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        bool signatureRefused = false;
        replay::skeleton::ResolvedPoseSkeleton resolved{};
        std::string error = "skeleton_unresolved";
        int slot = -1;
        std::uint32_t recordId = puppet.recordId;
        if (replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved))
        {
            for (std::uint32_t index = 0; index < replay::pose::capture::kMaxOverrides; ++index)
            {
                if (!g_poseV2OverrideInUse[index])
                {
                    slot = static_cast<int>(index);
                    break;
                }
            }
            if (resolved.boneCount != puppet.boneCount || !recordId)
            {
                recordId = PoseV2RecordForBones(puppet.packet.id, timeMs, resolved.boneCount);
                if (!recordId)
                    error = "no_record_for_bones";
            }
            if (!recordId)
            {
            }
            else if (slot < 0)
                error = "no_override_slot";
            else
            {
                // The same bone count is not the same skeleton. The record's
                // parents at the playhead against the puppet's
                // (skeleton_signature.h): the same, or a layout that cannot
                // vouch, arms exact; another signature arms through the
                // skeleton map, or is refused (hidden).
                switch (GatePoseV2Signature(puppet, resolved, recordId, slot, timeMs, error))
                {
                case PoseV2SignatureGate::Pending:
                case PoseV2SignatureGate::Mapped:
                    return; // the detour reads the puppet's table / armed and logged
                case PoseV2SignatureGate::Refused:
                    signatureRefused = true;
                    break;
                default:
                    ArmPoseV2Exact(puppet, resolved, slot, recordId, timeMs, error);
                    break;
                }
            }
        }
        const bool armed = puppet.overrideSlot >= 0;
        if (armed)
            NotePoseV2ExactArm(puppet, timeMs);
        // A body that was posed and lost its arm to a skeleton change no record
        // has (a bandana put on: 985 bones for a 1066 record;
        // no_record_fallback.h) is armed through the skeleton map in this tick
        // instead of hidden through the retries below; the retries and the
        // other recorded appearances go on behind the mapped pose
        // (StepPoseV2NoRecord).
        if (!armed && error == "no_record_for_bones" && slot >= 0 &&
            replay::norecord::MapNow(true, puppet.lostToChange, puppet.noRecordMapRefused))
        {
            std::string mapError;
            const PoseV2MapArm result = ArmPoseV2Mapped(puppet, resolved, slot, timeMs, mapError);
            if (result == PoseV2MapArm::Pending)
                return; // the detour reads the puppet's table (a frame or two)
            if (result == PoseV2MapArm::Armed)
            {
                ++g_noRecord.mapNow;
                const bool began = puppet.noRecord.Begin(puppet.recordId, puppet.boneCount, GetTickCount64());
                PoseV2LayerDiff(puppet, PoseV2Wear(puppet), began ? "map_now" : nullptr, resolved.boneCount, timeMs);
                if (g_noRecordLogs.Take(96))
                    Ctx().log << "POSE_V2_NO_RECORD,id=" << replay::entity::IdText(puppet.packet.id) << ",action=map_now"
                              << ",began=" << (began ? 1 : 0) << ",clone_bones=" << resolved.boneCount
                              << ",track_bones=" << puppet.boneCount << ",record=0x" << std::hex << puppet.recordId << std::dec << ",playhead_ms=" << timeMs << '\n';
                return;
            }
            // Refused (logged by the map when it built one): the old path,
            // the retries hidden, for this change.
            ++g_noRecord.mapRefused;
            puppet.noRecordMapRefused = true;
            puppet.armFailureLogged = false;
            // The old path maps again after its retries: a fresh wait, not
            // this attempt's spent deadline (an instant map_parents_timeout).
            puppet.mapDeadline = 0;
            if (g_noRecordLogs.Take(96))
                Ctx().log << "POSE_V2_NO_RECORD,id=" << replay::entity::IdText(puppet.packet.id) << ",action=map_refused"
                          << ",error=" << mapError << ",clone_bones=" << resolved.boneCount << ",track_bones=" << puppet.boneCount
                          << ",playhead_ms=" << timeMs << '\n';
        }
        constexpr std::uint32_t kMaxLayerRetries = 6;
        if (!armed && error == "no_record_for_bones" && puppet.layerRetries < kMaxLayerRetries &&
            GetTickCount64() >= puppet.nextLayerRetryAt && !PoseV2KnockPending(puppet.packet.id))
        {
            // The skeleton the engine built matches no record: apply the
            // segment's layers again (or the appearance change worn at the
            // playhead) and let the next ticks re-arm.
            const replay::WantedAppearance wanted = PoseV2AppearanceAt(puppet, timeMs);
            const replay::entity::Appearance& appearance = *wanted.appearance;
            SetPoseV2Wear(puppet, appearance);
            DressPoseV2Body(puppet, appearance, timeMs, true); // shop items or layers
            ApplyPoseV2Metaped(puppet, timeMs, true);
            puppet.appearanceKey = wanted.key;
            ++puppet.layerRetries;
            puppet.nextLayerRetryAt = GetTickCount64() + 400;
            puppet.armDeadline = GetTickCount64() + 3000;
            puppet.armFailureLogged = false;
            Ctx().log << "POSE_V2_PLAYBACK_LAYER_RETRY,id=" << replay::entity::IdText(puppet.packet.id)
                << ",retry=" << puppet.layerRetries << ",clone_bones=" << resolved.boneCount << ",track_bones=" << puppet.boneCount
                << ",layers=" << appearance.assets.size() << ",playhead_ms=" << timeMs << '\n';
            return;
        }
        // The retries are spent (and the last one had its 400 ms): the skeleton
        // map instead of giving up.
        bool refusedNow = false;
        if (!armed && error == "no_record_for_bones" && puppet.layerRetries >= kMaxLayerRetries &&
            GetTickCount64() >= puppet.nextLayerRetryAt)
        {
            const PoseV2MapArm result = ArmPoseV2Mapped(puppet, resolved, slot, timeMs, error);
            if (result == PoseV2MapArm::Armed) // which layers did not take
                PoseV2LayerDiff(puppet, PoseV2Wear(puppet), "mapped_after_retries", resolved.boneCount, timeMs);
            if (result != PoseV2MapArm::Refused)
                return; // armed (logged) or waiting for the detour
            refusedNow = true;
        }
        refusedNow = refusedNow || signatureRefused;
        if (armed || refusedNow || (GetTickCount64() > puppet.armDeadline && !puppet.armFailureLogged))
        {
            // A map refusal logged its own line (with the map's counts).
            if (!(refusedNow && puppet.armFailureLogged))
                LogPoseV2Arm(puppet, armed, armed ? "exact" : "refused", error, resolved, recordId, nullptr, 0, timeMs);
            puppet.armFailureLogged = !armed;
            if (!armed)
            {
                // Hidden from now on (never the bind pose); a line every 5 s
                // while it stays so, and the skeleton watched for a rebuild
                // that a record or the map could pose.
                ++g_poseV2RefusedArms;
                puppet.armError = error;
                puppet.failedSkeleton = resolved.skeleton;
                puppet.failedBones = resolved.boneCount;
                puppet.rearmProbeAt = GetTickCount64() + 1000;
                puppet.refusedLogAt = 0;
            }
        }
    }
    namespace
    {

    // One POSE_V2_SEGMENT_SWITCH line per step of a prefix hold
    // (prefix_hold.h): begin, verified, release (given up: the old path) and
    // end (a record fits the skeleton again, or it grew back).
    void LogPoseV2PrefixHold(const replay::playback::Puppet& puppet, const char* phase, const char* why,
        std::uint32_t cloneBones, std::uint32_t timeMs)
    {
        if (!g_poseV2SwitchLogs.Take(96))
            return;
        std::uint64_t prefixApplies = 0;
        if (puppet.overrideSlot >= 0)
            prefixApplies = replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(puppet.overrideSlot)).prefixApplies;
        const std::uint64_t now = GetTickCount64();
        Ctx().log << "POSE_V2_SEGMENT_SWITCH,id=" << replay::entity::IdText(puppet.packet.id) << ",action=prefix_hold"
                  << ",phase=" << phase << ",why=" << (why ? why : "none") << ",bones=" << cloneBones
                  << ",armed_bones=" << puppet.armedBones << ",body_bones=" << puppet.bodyBones
                  << ",record=0x" << std::hex << puppet.armedRecordId << ",segment_record=0x" << puppet.recordId << std::dec
                  << ",verdict=" << replay::prefixhold::VerdictText(puppet.prefixHold.verdict)
                  << ",ticks=" << puppet.prefixHold.ticks
                  << ",held_ms=" << (puppet.prefixHold.Active() && now > puppet.prefixHold.since ? now - puppet.prefixHold.since : 0u)
                  << ",prefix_applies=" << prefixApplies << ",playhead_ms=" << timeMs << '\n';
    }
    }

    void EndPoseV2PrefixHold(replay::playback::Puppet& puppet, const char* why, std::uint32_t cloneBones, std::uint32_t timeMs)
    {
        if (!puppet.prefixHold.Active())
            return;
        ++g_poseV2Switch.prefixEnded;
        LogPoseV2PrefixHold(puppet, "end", why, cloneBones, timeMs);
        puppet.prefixHold.End();
    }

    // segment_switch.h HoldPrefix: the arm kept on a skeleton the engine
    // rebuilt in place with fewer bones than the armed record, within its body
    // group (a gore rebuild in progress; prefix_hold.h). The detour writes the
    // published pose's first bones into it, so the body stays shown and posed
    // instead of hidden. The puppet's parent table, read by the detour within
    // a frame or two, is compared with the record's first bones. False: given
    // up (another skeleton, the table unread, past kMaxMs); the caller
    // releases (the old path).
    bool HoldPoseV2Prefix(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved,
        std::uint32_t timeMs)
    {
        using replay::prefixhold::Verdict;
        const ULONGLONG now = GetTickCount64();
        if (puppet.prefixHold.Note(resolved.boneCount, now))
        {
            ++g_poseV2Switch.prefixHolds;
            LogPoseV2PrefixHold(puppet, "begin", "skeleton_shrank", resolved.boneCount, timeMs);
        }
        const char* why = "none";
        if (puppet.prefixHold.verdict == Verdict::Pending)
        {
            const char* error = "none";
            switch (PoseV2PuppetParents(puppet, resolved, error))
            {
            case PoseV2SignatureInfo::Pending:
                break;
            case PoseV2SignatureInfo::Failed:
                puppet.prefixHold.verdict = Verdict::Unread;
                why = error;
                break;
            case PoseV2SignatureInfo::Ready:
            {
                static std::vector<std::int32_t> recordParents;
                const bool same = replay::pose::capture::TrackLayoutParents(puppet.armedRecordId, recordParents) &&
                    puppet.sigParents.size() == resolved.boneCount &&
                    replay::prefixhold::SameFront(recordParents.data(), static_cast<std::uint32_t>(recordParents.size()),
                        puppet.sigParents.data(), resolved.boneCount);
                puppet.prefixHold.verdict = same ? Verdict::Same : Verdict::Differ;
                why = same ? "front_same" : "front_differ";
                if (same)
                {
                    ++g_poseV2Switch.prefixVerified;
                    LogPoseV2PrefixHold(puppet, "verified", why, resolved.boneCount, timeMs);
                }
                break;
            }
            }
        }
        if (puppet.prefixHold.Holds(now))
            return true;
        if (puppet.prefixHold.Expired(now))
            why = "timeout";
        else if (std::strcmp(why, "none") == 0)
            why = replay::prefixhold::VerdictText(puppet.prefixHold.verdict);
        ++g_poseV2Switch.prefixReleased;
        LogPoseV2PrefixHold(puppet, "release", why, resolved.boneCount, timeMs);
        puppet.prefixHold.End();
        return false;
    }

    // An exact arm (segment_switch.h) moved to the puppet's current skeleton
    // and `record` in the tick the change is seen, the pose written at once
    // (ArmPoseV2Exact), the slot kept; the signature is compared after, on the
    // armed body. False: nothing armed (the caller releases).
    bool RearmPoseV2InTick(replay::playback::Puppet& puppet, const replay::skeleton::ResolvedPoseSkeleton& resolved,
        std::uint32_t record, std::uint32_t timeMs, const char* why)
    {
        const int slot = puppet.overrideSlot;
        if (slot < 0 || puppet.mapped)
            return false;
        const replay::pose::capture::OverrideStats released =
            replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(slot));
        const std::uint32_t previousBones = puppet.armedBones;
        const std::uint32_t previousRecord = puppet.armedRecordId;
        replay::pose::capture::DisarmOverride(slot);
        ForgetPoseV2Signature(puppet);
        puppet.overrideSlot = -1;
        puppet.armedSkeleton = nullptr;
        puppet.armedBones = 0;
        puppet.armedRecordId = 0;
        std::string error;
        if (!ArmPoseV2Exact(puppet, resolved, slot, record, timeMs, error))
        {
            g_poseV2OverrideInUse[slot] = false;
            ++g_poseV2Switch.rearmFailed;
            return false;
        }
        ScheduleSignatureRecheck(puppet); // compared on the armed body
        puppet.armFailureLogged = false;
        puppet.holdSince = 0;
        NotePoseV2ExactArm(puppet, timeMs);
        ++g_poseV2Switch.rearmedInTick;
        if (g_poseV2SwitchLogs.Take(96))
            Ctx().log << "POSE_V2_SEGMENT_SWITCH,id=" << replay::entity::IdText(puppet.packet.id) << ",action=rearm_in_tick"
                      << ",why=" << why << ",record=0x" << std::hex << record << ",previous_record=0x" << previousRecord
                      << std::dec << ",bones=" << resolved.boneCount << ",previous_bones=" << previousBones
                      << ",segment_record=0x" << std::hex << puppet.recordId << std::dec
                      << ",released_applies=" << released.applies << ",released_alt_applies=" << released.altApplies
                      << ",released_guard_applies=" << released.guardApplies << ",playhead_ms=" << timeMs << '\n';
        return true;
    }

    void LogPoseV2SwitchSummary()
    {
        LogPoseV2HatSummary();
        const PoseV2SwitchTotals& t = g_poseV2Switch;
        if (!t.switches && !t.kept && !t.held)
            return;
        Ctx().log << "POSE_V2_SEGMENT_SWITCH_SUMMARY,switches=" << t.switches
                  << ",rearmed_in_tick=" << t.rearmedInTick << ",released=" << t.released << ",rearm_failed=" << t.rearmFailed
                  << ",segment_kept_arm=" << t.kept << ",segment_held_arm=" << t.held << ",hold_timeouts=" << t.holdTimeouts
                  << ",hole_ticks=" << t.holeTicks << ",worst_hole_ticks=" << t.worstHole
                  << ",prefix_holds=" << t.prefixHolds << ",prefix_verified=" << t.prefixVerified
                  << ",prefix_released=" << t.prefixReleased << ",prefix_ended=" << t.prefixEnded << '\n';
        // The no-record fallback's totals (no_record_fallback.h).
        const NoRecordTotals& n = g_noRecord;
        if (n.mapNow || n.mapRefused || n.diffs)
            Ctx().log << "POSE_V2_NO_RECORD_SUMMARY,map_now=" << n.mapNow
                      << ",map_refused=" << n.mapRefused << ",retries=" << n.retries << ",same_answer=" << n.sameAnswer
                      << ",exact_after_retry=" << n.exactAfter << ",diff_lines=" << n.diffs
                      << ",categories_known=" << g_noRecordCategories.Size() << '\n';
    }
}
