#include "replay_playback.h"
#include "camera_bones.h" // CameraRecordTime
#include "object_check.h"
#include "guarded_memory.h"
#include "tpose_check.h"

#include "burn_level.h"
#include "entity_appearance.h"
#include "hat_look.h" // the knock wait
#include "metaped_reader.h"
#include "anim_lod_hook.h" // the puppets' anim LOD (PuppetAnimLod0)
#include "shader_vars_replay.h" // the horses' muscles (MuscleReplay)
#include "entity_pools.h"
#include "log_format.h"
#include "log_policy.h"
#include "mount_placement.h"
#include "playback_cache.h"
#include "pose_capture.h"
#include "pose_jerk.h"
#include "puppet_collision.h"
#include "pose_playback.h"
#include "pose_segments.h"
#include "profiler.h"
#include "entity_cache.h"
#include "replay_events.h"
#include "fx_observer.h" // PlaceScope
#include "fx_motion_rule.h" // StepMetres
#include "editor_keys.h" // the keys a message names
#include "entity_selection.h"
#include "replay_markers.h"
#include "replay_objects.h"
#include "object_look.h"
#include "replay_trains.h"
#include "vehicle_doors_engine.h"
#include "rope_observer.h"
#include "reins_guard.h"
#include "rope_models.h"   // a rope's entity is the rope track's
#include "rope_world.h"
#include "world_landing.h"
#include "puppet_blink.h"
#include "puppet_watch.h"
#include "player_hold.h"
#include "seek_settle.h"
#include "skeleton_map.h"
#include "skeleton_signature.h"
#include "skeleton_resolve.h"
#include "engine_hud.h"
#include "frag_observer.h"
#include "vegetation_speed.h"
#include "audio_observer.h"
#include "world_theatre.h" // kScriptHookThreadHash: the other ASIs' threads

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace replay::playback
{
    namespace
    {
        Context* g_context = nullptr;
        Context& Ctx() { return *g_context; }

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;

    // PoseTrack v2 playback: puppets follow the lifecycle track. At every
    // playhead time the covering block says which entities exist; a puppet
    // (inert clone) is created for each one, at most a few per frame, and
    // deleted when its entity is not alive, so seek and rewind need no
    // special case. Each puppet gets the engine world transform and the pose
    // recorded in the same snapshot; there is no other source of actor state.


    std::vector<Hash> g_poseV2RequestedModels;
    std::vector<Hash> g_poseV2RequestedWeapons;
    // Entities whose create waits for a later pose segment
    // (POSE_V2_PLAYBACK_PUPPET status=wait_segment logged once each).
    std::vector<std::uint32_t> g_poseV2WaitSegmentLogged;

    // Preparation: the playhead holds at the start until the puppets alive
    // there are built, as a replay editor holds the image while it loads.
    constexpr ULONGLONG kPoseV2PrepareTimeoutMs = 8000;
    bool g_poseV2Preparing = false;
    ULONGLONG g_poseV2PrepareDeadline = 0;

    // Normal play: the playhead moves forward at +1x. One-shot events play only
    // then, never paused, preparing or scrubbing (as in a replay editor: sounds
    // and shots are skipped while the cursor rewinds, fast forwards or jumps).
    bool PoseV2NormalPlay()
    {
        return !Ctx().transport.Paused() && !g_poseV2Preparing && !Ctx().transport.Scrubbing();
    }

    std::vector<replay::entity::CreatePacket> g_poseV2Alive;
    // AliveAt only when its stamp moves.
    std::uint64_t g_poseV2AliveStamp = replay::entity::LifecycleTrack::kNoAliveStamp - 1;
    std::size_t g_poseV2AliveBlocks = 0;
    std::vector<std::uint32_t> g_poseV2AliveIds; // sorted packed ids of g_poseV2Alive
    std::uint64_t g_poseV2AliveCopies = 0;
    std::uint64_t g_poseV2NoCollisionCalls = 0; // SET_ENTITY_NO_COLLISION_ENTITY calls
    std::uint64_t g_poseV2HeadBoneCalls = 0;    // head bone natives
    // Body puppet pose samples taken, and the ticks that reused the last one
    // (the playhead had not moved).
    std::uint64_t g_poseV2PoseSamples = 0;
    std::uint64_t g_poseV2PoseSampleReuses = 0;
    // The seek settle's side of the playback (seek_settle.h). Cheap scrub: the
    // vehicle rebuilds and the event state wait for the release; the create
    // budget per frame; what the settle waits for.
    bool g_poseV2CheapScrub = false;
    std::uint32_t g_poseV2CreateBudget = replay::playback::kMaxCreatesPerFrame;
    replay::playback::SettleState g_poseV2SettleState;
    replay::playback::Counters g_poseV2Counters;
    replay::settle::ModelPreload g_poseV2Preload;
    std::uint32_t g_poseV2PreloadLogs = 0;
    std::uint32_t g_poseV2UnposedHideLogs = 0; // POSE_V2_UNPOSED_HIDDEN lines
    // Unposed bodies as episodes (puppet_watch.h); a line (and a puppet_unposed
    // anomaly) only past the rule.
    replay::watch::UnposedTracker g_poseV2Unposed;
    bool g_poseV2OverrideInUse[replay::pose::capture::kMaxOverrides] = {};
    bool g_poseV2PlaybackActive = false;
    std::uint32_t g_poseV2ScorchPlayLogs = 0; // POSE_V2_SCORCHED play lines
    std::uint32_t g_poseV2AppearancePlayLogs = 0; // POSE_V2_APPEARANCE_CHANGE play lines
    // The metaped look at the dressing.
    struct PoseV2MetapedPlayStats
    {
        std::uint64_t dressed = 0;      // bodies that left Dressing
        std::uint64_t withState = 0;    // ... with a recorded metaped state
        std::uint64_t exact = 0;        // ... whose expressions read back as recorded
        std::uint64_t notExact = 0;
        std::uint64_t unverified = 0;   // the puppet could not be read
        std::uint64_t applies = 0;      // Apply calls (dressing, segment, change)
        std::uint64_t expressionsSet = 0;
        std::uint64_t retries = 0;
        std::uint64_t notRecorded = 0;  // appearance_not_recorded
        std::uint64_t lookahead = 0;    // dressed from a later segment
    };
    PoseV2MetapedPlayStats g_poseV2MetapedPlay;
    // Bodies dressed by shop items or layers (metaped_state.h DressPath), why
    // the layers, and the read back of the shop items after the dress
    // (POSE_V2_PLAYBACK_SHOP_ITEMS, _SUMMARY).
    struct PoseV2ShopPlayStats
    {
        std::uint64_t dressShop = 0;      // DressPoseV2Body calls by shop items
        std::uint64_t dressLayers = 0;    // ... by layers
        std::uint64_t whyNoState = 0;
        std::uint64_t whyNoShopItems = 0;
        std::uint64_t whyPrintDiffers = 0;
        std::uint64_t whyShopRefused = 0;
        std::uint64_t whyNoLayers = 0;
        std::uint64_t checks = 0;         // read backs (dressing and shown puppets)
        std::uint64_t checksShown = 0;    // ... of a dress on a shown puppet
        std::uint64_t layersExactAfterShop = 0;
        std::uint64_t layersNotExactAfterShop = 0;
        std::uint64_t fallbacks = 0;      // shop path refused: dressed again by layers
        std::uint64_t itemsRecorded = 0;
        std::uint64_t matched = 0;
        std::uint64_t stateDiffer = 0;
        std::uint64_t missing = 0;
        std::uint64_t extra = 0;
        std::uint64_t exactBodies = 0;    // checks whose shop items read back as recorded
    };
    PoseV2ShopPlayStats g_poseV2ShopPlay;
    replay::log::LogLimit g_poseV2ShopPlayLogs;
    replay::log::LogLimit g_poseV2MetapedPlayLogs;
    replay::log::LogLimit g_poseV2NotRecordedLogs;
    // Skeleton changes re-armed in their tick (segment_switch.h; ini
    // SegmentSwitchInTick, default 1) and the ticks bodies went without a pose
    // around them.
    bool g_poseV2SwitchInTick = true;
    constexpr std::uint64_t kPoseV2HoldMaxMs = 600;
    struct PoseV2SwitchTotals
    {
        std::uint64_t switches = 0, rearmedInTick = 0, released = 0, holeTicks = 0, worstHole = 0;
        std::uint64_t kept = 0, held = 0, holdTimeouts = 0, rearmFailed = 0;
        // prefix_hold.h: holds begun, verified, given up (differ, unread,
        // timeout) and ended by a record fitting the skeleton again.
        std::uint64_t prefixHolds = 0, prefixVerified = 0, prefixReleased = 0, prefixEnded = 0;
    };
    PoseV2SwitchTotals g_poseV2Switch;
    replay::log::LogLimit g_poseV2SwitchLogs;
    void LogPoseV2SwitchSummary();
    // A posed body whose skeleton changed to a bone count no record has
    // (no_record_fallback.h) is armed through the map at once (ini
    // NoRecordMapNow, default 1), the recorded layers tried again behind it,
    // then (ini NoRecordAltAppearance, default 0) the entity's other recorded
    // appearances of the segment; the layer diff of each step logged with the
    // categories learned from the puppets.
    bool g_noRecordMapNow = true;
    bool g_noRecordAltAppearance = false;
    replay::norecord::CategoryCache g_noRecordCategories;
    struct NoRecordTotals
    {
        std::uint64_t mapNow = 0, mapRefused = 0, retries = 0, sameAnswer = 0, alternates = 0, alternateFits = 0;
        std::uint64_t restores = 0, exactAfter = 0, diffs = 0;
    };
    NoRecordTotals g_noRecord;
    replay::log::LogLimit g_noRecordLogs;
    std::uint32_t g_poseV2WindowPlayLogs = 0; // POSE_V2_VEHICLE_WINDOWS play lines
    std::uint32_t g_poseV2LookVerifyLogs = 0; // POSE_V2_VEHICLE_LOOK verify lines
    std::uint32_t g_poseV2PartPlayLogs = 0;   // POSE_V2_VEHICLE_PARTS play lines
    std::unordered_map<std::uint32_t, std::uint16_t> g_poseV2PartKeptLogged; // vehicle -> wheels kept, last logged
    // Vehicle fragment groups in playback, per result
    // (replay::frag::VehicleGroupResult) and the POSE_V2_VEHICLE_FRAG lines.
    std::uint32_t g_poseV2VehicleFragResults[replay::frag::kVehicleGroupResults] = {};
    // The clip's vehicle pieces and their puppets' state.
    std::vector<replay::vehicle_pieces::PieceState> g_poseV2PieceStates;
    std::uint32_t g_poseV2VehicleFragRebuilds = 0;
    std::uint32_t g_poseV2VehicleFragLogs = 0;
    std::uint32_t g_poseV2BurntPlayLogs = 0;  // POSE_V2_VEHICLE_BURNT play lines
    std::uint32_t g_poseV2BurnLevelPlayLogs = 0;  // POSE_V2_BURN_LEVEL play lines
    std::uint64_t g_poseV2BurnLevelWrites = 0;    // level writes this playback
    std::uint64_t g_poseV2BurnLevelEngineChanged = 0; // written level found changed by the engine
    std::uint32_t g_poseV2FireCharPlayLogs = 0; // POSE_V2_FIRE_CHAR play lines
    std::uint64_t g_poseV2FireCharWrites = 0;   // fire record writes this playback
    std::uint64_t g_poseV2FireCharRemoves = 0;  // fire record removals this playback
    std::uint64_t g_poseV2FireCharCreated = 0;      // volumes made on puppets this playback
    std::uint64_t g_poseV2FireCharCreateFailed = 0;
    std::uint64_t g_poseV2FireCharShapeDiffers = 0; // checks whose flags or volume count WriteChar cannot restore
    std::uint32_t g_poseV2PlaceDriftLogs = 0; // POSE_V2_PLACE_DRIFT lines
    // Rider and mount pairs from the recorded transforms, the jerk detector
    // (pose_jerk.h), and the puppets the engine pushed off their placement
    // while they were physics bodies: out of physics until the time kept here
    // (GetTickCount64), so the next frames render where we place them.
    // The puppets' LOD settings (puppet_lod.h) and the playback's totals
    // (POSE_V2_PUPPET_LOD_SUMMARY, Debug=0 too).
    replay::lod::Config g_poseV2LodConfig;
    // The hat fix (hat_chain.h) and the models' fitted values seen during this
    // playback.
    bool g_poseV2HatFix = true;
    replay::hatchain::Fitted g_poseV2HatFitted;
    struct PoseV2HatTotals
    {
        std::uint32_t puppets = 0;
        std::uint64_t fixed = 0;
        std::uint64_t learned = 0;
        std::uint64_t handled = 0;
        std::uint64_t kept = 0;
        std::uint32_t defaults = 0; // puppets whose last fix used kDefaultFitX
        std::uint32_t logs = 0;
    } g_poseV2HatTotals;

    // The hat fix of the record armed on the puppet, for the layout at the
    // playhead (built again when that layout changes: a hat knocked off,
    // another layer), from the sample just taken. Peds in exact mode only;
    // what the record's chains learned is kept across rebuilds.
    void PreparePoseV2HatFix(replay::playback::Puppet& puppet, std::uint32_t recordId, std::uint32_t timeMs,
        const replay::pose::PoseSample& sample)
    {
        if (!g_poseV2HatFix || puppet.packet.kind != replay::entity::EntityKind::Ped || puppet.mapped)
        {
            puppet.hatFix.armed = false;
            return;
        }
        // The layout at the playhead looked at every 250 ms (the capture's
        // lock); a new arm (another bone count: a hat knocked off) clears
        // hatLayout and builds at once.
        const std::uint64_t now = GetTickCount64();
        if (puppet.hatLayout != 0 && now < puppet.hatCheckAt)
            return;
        puppet.hatCheckAt = now + 250;
        std::uintptr_t layoutKey = 0;
        if (!replay::pose::capture::TrackLayoutSignatureAt(recordId, timeMs, nullptr, nullptr, layoutKey))
        {
            puppet.hatFix.armed = false;
            return;
        }
        if (layoutKey == puppet.hatLayout && puppet.hatLayout != 0)
            return;
        static std::vector<std::int32_t> parents;
        if (!replay::pose::capture::TrackLayoutSignatureAt(recordId, timeMs, &parents, nullptr, layoutKey))
            return;
        std::vector<std::array<float, 3>> translations(sample.locals.size());
        for (std::size_t n = 0; n < sample.locals.size(); ++n)
            translations[n] = {sample.locals[n].translation[0], sample.locals[n].translation[1], sample.locals[n].translation[2]};
        const std::vector<replay::hatchain::PuppetFix::Item> learned = puppet.hatFix.items;
        const std::uint64_t fixed = puppet.hatFix.fixed, learnedSamples = puppet.hatFix.learned;
        const std::uint64_t handled = puppet.hatFix.handled, kept = puppet.hatFix.kept;
        // The ped skeleton's rule, the finalizer's own.
        puppet.hatFix = replay::hatchain::BuildFix(puppet.packet.modelHash, parents, translations,
            replay::pose::CompositionOrder::ParentThenLocal, true);
        puppet.hatFix.fixed = fixed;
        puppet.hatFix.learned = learnedSamples;
        puppet.hatFix.handled = handled;
        puppet.hatFix.kept = kept;
        for (replay::hatchain::PuppetFix::Item& item : puppet.hatFix.items)
            for (const replay::hatchain::PuppetFix::Item& before : learned)
                if (before.top == item.top)
                    item.fitted = before.fitted;
        puppet.hatLayout = layoutKey;
        if (puppet.hatFix.armed && g_poseV2HatTotals.logs < 48)
        {
            ++g_poseV2HatTotals.logs;
            Ctx().log << "POSE_V2_HAT_CHAIN,id=" << replay::entity::IdText(puppet.packet.id) << ",record=0x" << std::hex << recordId
                      << std::dec << ",chains=" << puppet.hatFix.items.size() << ",top=" << puppet.hatFix.items[0].top
                      << ",head=" << puppet.hatFix.items[0].head << ",hat_root=" << puppet.hatFix.items[0].hatRoot
                      << ",subtree=" << puppet.hatFix.items[0].subtree.size() << ",playhead_ms=" << timeMs << '\n';
        }
    }

    void FixPoseV2Hat(replay::playback::Puppet& puppet, replay::pose::PoseSample& sample)
    {
        if (puppet.hatFix.armed)
            replay::hatchain::FixSample(puppet.hatFix, g_poseV2HatFitted, sample.locals, sample.objects);
    }
    replay::lod::Summary g_poseV2LodSummary;
    std::uint32_t g_poseV2LowLodLogs = 0;
    std::uint32_t g_poseV2AnimSkipLogs = 0;
    std::uint32_t g_poseV2LodLastPlayhead = ~0u; // the playhead of the last tick
    std::uint32_t g_poseV2LodPrevPlayhead = ~0u; // this tick's view of it
    constexpr std::uint32_t kPoseV2LowLodLogCap = 64;
    replay::jerk::MountPairs g_poseV2Mounts;
    // Recorded visibility per ped and horse.
    std::unordered_map<std::uint32_t, const replay::PedVisibility*> g_poseV2PedVisibility;
    std::uint32_t g_poseV2RecordedVisibilityLogs = 0;
    // The recorded player's id (player_hold.h PickPlayer, 0: none): his puppet
    // is never hidden, it holds its nearest pose (QA 224). A line per change
    // of what the hold covers (held, kept against PVIS), capped.
    std::uint32_t g_poseV2PlayerPacked = 0;
    std::uint32_t g_poseV2PlayerHoldLogs = 0;
    // The horse he rides at his record's first frame (0: none) with both
    // recorded transforms of that frame: before it he is carried by that horse
    // (player_hold.h Carry).
    std::uint32_t g_poseV2PlayerFirstMount = 0;
    replay::pose::WorldTransform g_poseV2PlayerRiderFirst{};
    replay::pose::WorldTransform g_poseV2PlayerMountFirst{};
    constexpr std::uint32_t kPoseV2PlayerHoldLogCap = 32;
    bool PoseV2RecordedVisible(std::uint32_t packed, std::uint32_t timeMs)
    {
        const auto found = g_poseV2PedVisibility.find(packed);
        return found == g_poseV2PedVisibility.end() ||
            replay::objcheck::VisibleAt(found->second->visible != 0, found->second->flipsMs, timeMs);
    }
    // The never animated hide (HideNeverAnimated) and the T-pose watch
    // (TPoseWatch, tpose_check.h). The signature of each record, read once per
    // playback; the finalizer code that proves the bind pose layout (0 not
    // checked yet, 1 proven, -1 refused: no bind read, no watch); the counts of
    // POSE_V2_TPOSE_SUMMARY.
    replay::tpose::HideMode g_tposeHideMode = replay::tpose::HideMode::BindProven;
    bool g_tposeWatchOn = true;
    std::unordered_map<std::uint32_t, replay::tpose::TrackSignature> g_tposeSignatures;
    int g_tposeBindLayout = 0;
    replay::tpose::TPoseWatch g_tposeWatch;
    // When the watch judges (TPoseSettleMs, TPoseOnScreenOnly; tpose_check.h).
    replay::tpose::WatchGate g_tposeGate;
    struct TPoseCounts
    {
        std::uint32_t judged = 0; // never animated records judged on a puppet
        std::uint32_t perVerdict[static_cast<int>(replay::tpose::HideVerdict::Off) + 1] = {};
        std::uint32_t bindReads = 0;
        std::uint32_t bindFailed = 0;
        std::uint32_t neverLines = 0;
        std::uint32_t tposeLines = 0;
    } g_tposeCounts;
    constexpr std::uint32_t kTPoseLineCap = 64;
    replay::pose::PoseSample g_tposeSample;
    std::vector<replay::pose::BoneQts> g_tposeDrawn;

    bool TPoseRead(std::uintptr_t address, void* out, std::size_t size)
    {
        return replay::mem::Copy(out, address, size);
    }

    bool TPoseBindLayoutProven()
    {
        if (g_tposeBindLayout == 0)
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
            g_tposeBindLayout = replay::tpose::BindLayoutProven([&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
                return replay::mem::Matches(base + rva, bytes, size);
            }) ? 1 : -1;
            Ctx().log << "POSE_V2_TPOSE_BIND_LAYOUT,status=" << (g_tposeBindLayout > 0 ? "proven" : "signature_mismatch")
                      << ",engine_writes=0\n";
        }
        return g_tposeBindLayout > 0;
    }

    // The bind rotations of the puppet's armed skeleton, read once per skeleton.
    bool EnsurePoseV2Bind(replay::playback::Puppet& puppet)
    {
        if (!puppet.armedSkeleton)
            return false;
        if (puppet.bindSkeleton == puppet.armedSkeleton)
            return !puppet.bindRotations.empty();
        puppet.bindSkeleton = puppet.armedSkeleton;
        puppet.bindRotations.clear();
        if (!TPoseBindLayoutProven())
            puppet.bindError = "layout_signature";
        else
            puppet.bindError = replay::tpose::ReadBindRotations(TPoseRead,
                reinterpret_cast<std::uintptr_t>(puppet.armedSkeleton), puppet.bindRotations);
        if (puppet.bindError[0] == 0)
            ++g_tposeCounts.bindReads;
        else
            ++g_tposeCounts.bindFailed;
        return !puppet.bindRotations.empty();
    }

    // True when the record the puppet plays was never animated (no pose change
    // over the record) and, as HideNeverAnimated asks, recorded in its model's
    // bind pose: a mission's hidden copies of a ped in a clip without PVIS.
    // Judged once per record and armed skeleton.
    bool PoseV2NeverAnimatedHidden(replay::playback::Puppet& puppet, std::uint32_t poseRecord, std::uint32_t timeMs)
    {
        if (puppet.neverRecord == poseRecord && puppet.neverSkeleton == puppet.armedSkeleton)
            return puppet.neverHidden;
        puppet.neverRecord = poseRecord;
        puppet.neverSkeleton = puppet.armedSkeleton;
        puppet.neverHidden = false;
        auto found = g_tposeSignatures.find(poseRecord);
        if (found == g_tposeSignatures.end())
        {
            replay::tpose::TrackSignature signature;
            if (!replay::pose::capture::ReadTrackSignature(poseRecord, signature))
                signature = {};
            found = g_tposeSignatures.emplace(poseRecord, signature).first;
        }
        const replay::tpose::TrackSignature& signature = found->second;
        if (!replay::tpose::NeverAnimated(signature))
            return false;
        // Every sample of the record is the same pose: its first one.
        replay::tpose::BindMatch match;
        bool compared = false;
        if (!puppet.mapped && EnsurePoseV2Bind(puppet) &&
            replay::pose::capture::SampleTrack(poseRecord, signature.firstMs, g_tposeSample) &&
            g_tposeSample.locals.size() == puppet.bindRotations.size())
        {
            match = replay::tpose::CompareLocalsWithBind(g_tposeSample.locals, puppet.bindRotations);
            compared = true;
        }
        const replay::tpose::HideVerdict verdict = replay::tpose::Decide(g_tposeHideMode, signature, compared ? &match : nullptr);
        puppet.neverHidden = replay::tpose::Hides(verdict);
        ++g_tposeCounts.judged;
        ++g_tposeCounts.perVerdict[static_cast<int>(verdict)];
        if (g_tposeCounts.neverLines < kTPoseLineCap)
        {
            ++g_tposeCounts.neverLines;
            const auto visibility = g_poseV2PedVisibility.find(puppet.packet.id.Packed());
            Ctx().log << "POSE_V2_NEVER_ANIMATED,id=" << replay::entity::IdText(puppet.packet.id)
                      << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex
                      << puppet.packet.modelHash << ",record=0x" << poseRecord << std::dec << ",frames=" << signature.frames
                      << ",span_ms=" << replay::tpose::SpanMs(signature) << ",world_frames=" << signature.worldFrames
                      << ",verdict=" << replay::tpose::HideVerdictText(verdict) << ",bind_compared=" << match.compared
                      << ",bind_posed=" << match.posed << ",bind_max_deg=" << static_cast<int>(match.maxRad * 57.29578f)
                      << ",bind_error=" << (puppet.mapped ? "mapped" : puppet.bindError[0] ? puppet.bindError
                                                          : compared ? "none" : "bones_differ")
                      << ",pvis=" << (visibility == g_poseV2PedVisibility.end() ? "none"
                                         : visibility->second->visible ? "1" : "0")
                      << ",playhead_ms=" << timeMs << '\n';
        }
        return puppet.neverHidden;
    }

    // The T-pose watch: what the shown puppet's skeleton draws, compared with
    // its model's bind pose every kCheckEveryMs (POSE_V2_TPOSE per episode).
    void WatchPoseV2TPose(replay::playback::Puppet& puppet, bool shown, std::uint32_t timeMs)
    {
        const std::uint32_t id = puppet.packet.id.Packed();
        if (!shown || puppet.overrideSlot < 0 || !puppet.armedSkeleton)
        {
            g_tposeWatch.Forget(id);
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < puppet.tposeCheckAt)
            return;
        // Not while the replay prepares, the screen is faded, within the settle
        // window of a seek, pause or resume, nor a body the engine does not
        // draw. The run starts again; the next check comes at the cadence, so
        // an episode needs two judged checks.
        const replay::tpose::WatchSkip skip =
            g_tposeGate.Judge(now, !g_tposeGate.OnScreenOnly() || ENTITY::IS_ENTITY_ON_SCREEN(puppet.clone) != FALSE);
        puppet.tposeCheckAt = now + replay::tpose::kCheckEveryMs;
        if (skip != replay::tpose::WatchSkip::None)
        {
            g_tposeWatch.Hold(id, skip);
            return;
        }
        if (!EnsurePoseV2Bind(puppet) ||
            replay::tpose::ReadDrawnLocals(TPoseRead, reinterpret_cast<std::uintptr_t>(puppet.armedSkeleton), g_tposeDrawn)[0] != 0 ||
            g_tposeDrawn.size() != puppet.bindRotations.size())
        {
            g_tposeWatch.NoteUnread();
            return;
        }
        const replay::tpose::BindMatch match = replay::tpose::CompareLocalsWithBind(g_tposeDrawn, puppet.bindRotations);
        replay::tpose::WatchReport report;
        if (!g_tposeWatch.Note(id, replay::tpose::IsBindPose(match), report) || g_tposeCounts.tposeLines >= kTPoseLineCap)
            return;
        ++g_tposeCounts.tposeLines;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        float sx = 0.0f, sy = 0.0f;
        const bool onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
        const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
        const auto signature = g_tposeSignatures.find(puppet.armedRecordId);
        Ctx().log << "POSE_V2_TPOSE,id=" << replay::entity::IdText(puppet.packet.id)
                  << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",model=0x" << std::hex
                  << puppet.packet.modelHash << ",record=0x" << puppet.armedRecordId << std::dec << ",posed=" << match.posed
                  << ",compared=" << match.compared << ",max_deg=" << static_cast<int>(match.maxRad * 57.29578f)
                  << ",checks=" << report.checks << ",never_animated="
                  << (signature != g_tposeSignatures.end() && replay::tpose::NeverAnimated(signature->second) ? 1 : 0)
                  << ",mapped=" << (puppet.mapped ? 1 : 0)
                  << ",camera_m=" << static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz))
                  << ",on_screen=" << (onScreen ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
    }

    void LogPoseV2TPoseSummary()
    {
        const replay::tpose::WatchSummary watch = g_tposeWatch.Summary();
        const TPoseCounts& c = g_tposeCounts;
        using replay::tpose::HideVerdict;
        Ctx().log << "POSE_V2_TPOSE_SUMMARY,watch=" << (g_tposeWatchOn ? 1 : 0) << ",checks=" << watch.checks
                  << ",bind_checks=" << watch.bindChecks << ",unread=" << watch.unread << ",episodes=" << watch.episodes
                  << ",entities=" << watch.entities << ",open=" << watch.open
                  << ",hide_mode=" << static_cast<int>(g_tposeHideMode) << ",never_judged=" << c.judged
                  << ",hide_bind=" << c.perVerdict[static_cast<int>(HideVerdict::HideBind)]
                  << ",show_posed=" << c.perVerdict[static_cast<int>(HideVerdict::ShowPosed)]
                  << ",hide_unproven=" << c.perVerdict[static_cast<int>(HideVerdict::HideUnproven)]
                  << ",show_unproven=" << c.perVerdict[static_cast<int>(HideVerdict::ShowUnproven)]
                  << ",off=" << c.perVerdict[static_cast<int>(HideVerdict::Off)]
                  << ",bind_reads=" << c.bindReads << ",bind_failed=" << c.bindFailed
                  << ",bind_layout=" << (g_tposeBindLayout > 0 ? "proven" : g_tposeBindLayout < 0 ? "signature_mismatch" : "unchecked")
                  << ",lines=" << c.tposeLines << ",never_lines=" << c.neverLines
                  << ",settle_ms=" << g_tposeGate.SettleMs() << ",on_screen_only=" << (g_tposeGate.OnScreenOnly() ? 1 : 0)
                  << ",transport_events=" << g_tposeGate.events
                  << ",skip_preparing=" << watch.skipped[static_cast<int>(replay::tpose::WatchSkip::Preparing)]
                  << ",skip_hidden=" << watch.skipped[static_cast<int>(replay::tpose::WatchSkip::Hidden)]
                  << ",skip_settle=" << watch.skipped[static_cast<int>(replay::tpose::WatchSkip::Settle)]
                  << ",skip_off_screen=" << watch.skipped[static_cast<int>(replay::tpose::WatchSkip::OffScreen)] << '\n';
    }

    replay::jerk::Detector g_poseV2Jerk;
    std::vector<replay::jerk::Finding> g_poseV2JerkFindings;
    std::vector<replay::jerk::Body> g_poseV2JerkBodies;
    std::unordered_map<std::uint32_t, ULONGLONG> g_poseV2PushedUntil;
    std::uint32_t g_poseV2JerkLogs = 0;  // POSE_JERK lines this playback
    std::uint32_t g_poseV2MountLogs = 0; // POSE_V2_MOUNT_PAIR lines this playback
    std::uint64_t g_poseV2CarriedTicks = 0; // puppet ticks kept out of physics as riders or seated
    std::uint64_t g_poseV2PushedHolds = 0;  // physics bodies taken out after a push
    constexpr ULONGLONG kPoseV2PushedHoldMs = 1000;
    constexpr std::uint32_t kPoseV2JerkLogCap = 200;
    // End-of-frame check of the written pose, per override slot: next check
    // (GetTickCount64) and whether the last one differed.
    std::uint32_t g_poseV2WriteCheckLogs = 0;
    std::uint64_t g_poseV2WriteCheckAt[replay::pose::capture::kMaxOverrides] = {};
    bool g_poseV2WriteChanged[replay::pose::capture::kMaxOverrides] = {};
    replay::pose::PoseSample g_poseV2Sample;
    replay::pose::PoseSample g_poseV2AltSample;
    // A mapped puppet's pose (its own bone count), built from the record's
    // sample through its skeleton map.
    replay::pose::PoseSample g_poseV2MappedSample;
    std::uint32_t g_poseV2MappedArms = 0;
    std::uint32_t g_poseV2RefusedArms = 0;
    std::uint32_t g_poseV2RefusedHiddenLogs = 0;
    // Signature checks of the playback (skeleton_signature.h), by verdict, and
    // what a mismatch led to.
    struct PoseV2SignatureCounts
    {
        std::uint32_t same = 0;
        std::uint32_t differ = 0;
        std::uint32_t unknown = 0;
        std::uint32_t mapped = 0;   // a mismatch armed through the skeleton map
        std::uint32_t refused = 0;  // a mismatch the map refused: hidden
        std::uint32_t rechecks = 0; // armed skeletons compared again
        std::uint32_t rearms = 0;   // a recheck that found a mismatch
        std::uint32_t timeouts = 0; // the puppet's table not read in time (armed unchecked)
        std::uint32_t rebuilt = 0;  // the record's own skeleton rebuilt mid-recording
    };
    PoseV2SignatureCounts g_poseV2Signature;

    // World transform at the sample time: both recorded engine matrices are
    // decoded and blended with the same factor as the pose.
    bool SampleWorldTransform(
        const replay::pose::PoseSample& sample,
        replay::pose::WorldTransform& world)
    {
        replay::pose::WorldTransform a{};
        if (!replay::pose::DecodeWorldMatrix(sample.worldA, a))
            return false;
        replay::pose::WorldTransform b{};
        world = replay::pose::DecodeWorldMatrix(sample.worldB, b)
            ? replay::pose::InterpolateWorld(a, b, sample.t)
            : a;
        return true;
    }

    // The entity's recorded world transform at timeMs, from the record of
    // its segment there (declared later with the segment helpers).
    bool PoseV2RecordedWorldAt(
        replay::entity::ReplayId id, std::uint32_t timeMs, replay::pose::WorldTransform& world);
    const replay::entity::PoseSegment* PoseV2SegmentAt(replay::entity::ReplayId id, std::uint32_t timeMs);

    // Playback world: every ped that is not a puppet (the player, its horse,
    // NPCs, animals) is hidden while the editor plays, and ambient
    // population is suppressed, so everything visible belongs to the replay.
    constexpr ULONGLONG kPoseV2WorldScanIntervalMs = 250;
    std::vector<replay::pose::playback::HiddenPedState> g_poseV2HiddenWorld;
    ULONGLONG g_poseV2NextWorldScanAt = 0;

    // The world clear's two membership tests per listed entity (a puppet's
    // clone? already hidden? ~750 entities against ~100 puppets and ~120 hidden
    // in town, every 250 ms) use sorted handles, built at the start of each
    // pass (HidePoseV2World). The puppets do not change during a pass; every
    // entity hidden goes into the hidden list and its set together.
    // tests/playback_perf_tests.cpp keeps the linear search as the reference.
    replay::playback::HandleSet g_poseV2ClearClones;
    replay::playback::HandleSet g_poseV2ClearHidden;

    // Entities that are never deleted, only hidden: the player's horses and its
    // vehicle. The player itself is left alone (visible, not frozen): the world
    // is not yet prepared for the clip as a whole, and the player keeps control
    // of the character while the editor plays.
    struct PoseV2PlayerOwned
    {
        Entity entities[5] = {};

        bool IsPlayer(Entity entity) const { return entity && entity == entities[0]; }

        void Refresh()
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            entities[0] = player;
            entities[1] = PED::IS_PED_ON_MOUNT(player) ? PED::GET_MOUNT(player) : 0;
            entities[2] = PED::_GET_LAST_MOUNT(player);
            entities[3] = PLAYER::_GET_ACTIVE_HORSE_FOR_PLAYER(PLAYER::PLAYER_ID());
            entities[4] = PED::GET_VEHICLE_PED_IS_IN(player, TRUE);
        }

        bool Contains(Entity entity) const
        {
            return entity &&
                std::find(std::begin(entities), std::end(entities), entity) != std::end(entities);
        }
    };

    std::uint64_t g_poseV2WorldDeleted = 0;
    std::uint64_t g_poseV2WorldRehidden = 0;
    std::uint64_t g_poseV2WorldOwnerChanges = 0;
    // A hidden entity a script unfroze or gave collision back to, put back by
    // the re-hide.
    std::uint64_t g_poseV2WorldRefrozen = 0;
    std::uint64_t g_poseV2WorldRecollided = 0;
    // The hidden entities given back after the playback (world_landing.h), each
    // once the map collision around it is in.
    replay::landing::Queue<replay::pose::playback::HiddenPedState> g_poseV2Landing;
    std::uint32_t g_poseV2LandingImmediate = 0; // restored at once (frozen before the replay)
    Hash g_poseV2LandingTimeoutModel = 0;        // the first that timed out (log)
    Vector3 g_poseV2LandingTimeoutAt{};
    bool g_poseV2LandingPlayerHorse = false;     // the player's horse was in the queue
    bool g_poseV2LandingPlayerHorseTimedOut = false;
    unsigned long long g_poseV2LandingPlayerHorseMs = 0;
    Entity g_poseV2LandingPlayerHorseHandle = 0;
    // The world's ropes hidden while the replay plays (rope_world.h).
    replay::ropes::WorldRopeHider g_poseV2WorldRopes;
    std::uint32_t g_poseV2WorldRopeScans = 0;
    std::uint32_t g_poseV2WorldRopeSnapshotFails = 0;
    std::uint32_t g_poseV2WorldRopeNoHandle = 0; // DOES_ROPE_EXIST false for a listed rope
    replay::log::LogLimit g_poseV2WorldRopeLogs;
    std::uint32_t g_poseV2CreateRefused = 0;     // creates the engine refused this playback
    std::uint32_t g_poseV2CreateRefusedLogs = 0;
    // What the scan leaves around the camera (live peds that are not puppets,
    // visible, within 60 m), logged once every 3 s, at most 30 lines per
    // playback.
    ULONGLONG g_poseV2WorldCensusAt = 0;
    std::uint32_t g_poseV2WorldCensusLogs = 0;
    // Per scan, for the log: what keeps coming back.
    std::uint32_t g_poseV2ClearPeds = 0;
    std::uint32_t g_poseV2ClearVehicles = 0;
    Hash g_poseV2ClearSampleModel = 0;

    void ClearPoseV2WorldPool(
        int* handles,
        int count,
        bool vehicles,
        const PoseV2PlayerOwned& owned,
        std::size_t& hidden)
    {
        for (int index = 0; index < count; ++index)
        {
            const Entity entity = handles[index];
            if (!entity || g_poseV2ClearClones.Contains(entity) || owned.IsPlayer(entity))
                continue;
            // A train car is never deleted one by one (trains go as a whole):
            // hidden, its ambient train halted, and given back at the end
            // (replay_trains).
            if (vehicles && replay::trains::ClearWorldCar(entity))
                continue;
            const bool known = g_poseV2ClearHidden.Contains(entity);
            if (known)
            {
                // A game script may show its ped again (town scripts, the
                // camp): hidden again at once, and frozen and without collision
                // again, so it cannot fall through the ground unseen while the
                // replay plays.
                const std::uint32_t fixed = replay::pose::playback::RehideWorldPed(entity);
                if (fixed)
                {
                    // The owner's change is what goes back at the end
                    // (NoteOwnerChange).
                    for (replay::pose::playback::HiddenPedState& state : g_poseV2HiddenWorld)
                    {
                        if (state.ped != entity)
                            continue;
                        if (replay::pose::playback::NoteOwnerChange(state, fixed))
                            ++g_poseV2WorldOwnerChanges;
                        break;
                    }
                }
                if (fixed & replay::pose::playback::kRehideVisible)
                    ++g_poseV2WorldRehidden;
                if (fixed & replay::pose::playback::kRehideFrozen)
                    ++g_poseV2WorldRefrozen;
                if (fixed & replay::pose::playback::kRehideCollision)
                    ++g_poseV2WorldRecollided;
                continue;
            }
            if (!owned.Contains(entity))
            {
                const Hash model = ENTITY::GET_ENTITY_MODEL(entity);
                const bool deleted = vehicles
                    ? replay::pose::playback::DeleteAmbientVehicle(entity)
                    : replay::pose::playback::DeleteAmbientPed(entity);
                if (deleted)
                {
                    ++g_poseV2WorldDeleted;
                    ++(vehicles ? g_poseV2ClearVehicles : g_poseV2ClearPeds);
                    g_poseV2ClearSampleModel = model;
                    continue;
                }
            }
            replay::pose::playback::HiddenPedState state;
            if (replay::pose::playback::HideWorldPed(entity, state))
            {
                g_poseV2HiddenWorld.push_back(state);
                g_poseV2ClearHidden.Add(state.ped);
                ++hidden;
            }
        }
    }

    // The objects listed at the playback's first pass: a script object that
    // is not among them was made while the game's scripts were paused, by
    // another ASI (objcheck::IsOtherModObject).
    std::unordered_set<Object> g_poseV2LiveObjectsAtStart;
    bool g_poseV2LiveObjectsBaseline = false;

    // Another mod's object (objcheck::IsOtherModObject): a script object made
    // by an ASI's thread (ScriptHookRDR2 runs every ASI in one with the script
    // hash joaat("audiotest")), or one born after the playback's first pass.
    // The replay's own objects are told apart before (ReplayOwnObjects, the
    // puppets).
    // Only a script object (a mission entity, or the CREATE_OBJECT owner)
    // asks the engine for its script.
    void PoseV2ScriptFacts(Object object, bool missionEntity, std::uint32_t owner, bool& otherScript, bool& born)
    {
        otherScript = false;
        born = false;
        if (!missionEntity && owner != replay::objcheck::kOwnerMission)
            return;
        born = g_poseV2LiveObjectsBaseline && g_poseV2LiveObjectsAtStart.count(object) == 0;
        Any scriptArgs[8]{};
        otherScript =
            static_cast<std::uint32_t>(ENTITY::_GET_ENTITY_SCRIPT(object, scriptArgs)) == replay::theatre::kScriptHookThreadHash;
    }

    bool PoseV2OtherModObject(Object object, bool missionEntity, std::uint32_t owner)
    {
        bool otherScript = false, born = false;
        PoseV2ScriptFacts(object, missionEntity, owner, otherScript, born);
        return replay::objcheck::IsOtherModObject(otherScript, born, missionEntity, owner);
    }

    // The live world's vehicle debris (wheels, doors, boards an explosion or a
    // fire knocked off: objects of a vehicle's model) is not the replay's: the
    // world's originals are hidden while it plays. Ambient debris is deleted
    // like the ambient vehicles, a mission one hidden and given back. The
    // replay's own pieces are puppets.
    std::uint32_t g_poseV2ClearDebris = 0;
    std::unordered_map<std::uint32_t, bool> g_poseV2VehicleModels;

    void ClearPoseV2VehicleDebris(std::size_t& hidden)
    {
        static int handles[replay::recorder::kPoolCapacity]{};
        const int count = (std::min)(
            (std::max)(0, replay::objects::ListObjects(handles, replay::recorder::kPoolCapacity)),
            replay::recorder::kPoolCapacity);
        for (int index = 0; index < count; ++index)
        {
            Object object = handles[index];
            if (!object || !replay::entities::Exists(object) || g_poseV2ClearClones.Contains(object))
                continue;
            // The model (cached per model) before the attach test, both pure
            // reads: the ~675 objects of a town (a few of a vehicle model) do
            // not each ask IS_ENTITY_ATTACHED.
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            auto known = g_poseV2VehicleModels.find(model);
            if (known == g_poseV2VehicleModels.end())
                known = g_poseV2VehicleModels.emplace(model, STREAMING::IS_MODEL_A_VEHICLE(model) != FALSE).first;
            if (!known->second || ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            if (g_poseV2ClearHidden.Contains(object))
                continue;
            const bool mission = replay::pose::playback::IsMissionEntity(object);
            if (PoseV2OtherModObject(object, mission, replay::objects::EntityOwnerOf(object)))
                continue; // another mod's (objcheck::IsOtherModObject)
            if (!mission)
            {
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                Object handle = object;
                OBJECT::DELETE_OBJECT(&handle);
                if (!replay::entities::Exists(object))
                {
                    ++g_poseV2WorldDeleted;
                    ++g_poseV2ClearDebris;
                    continue;
                }
            }
            replay::pose::playback::HiddenPedState state;
            if (replay::pose::playback::HideWorldPed(object, state))
            {
                g_poseV2HiddenWorld.push_back(state);
                g_poseV2ClearHidden.Add(state.ped);
                ++hidden;
            }
        }
    }

    // The live objects that are not the map's (a script's, a temporary one, a
    // prop set's, a pickup; object_check.h HideLiveObject) in the clip area are
    // hidden while the replay plays, like the mission peds, and given back with
    // them (RestorePoseV2World): otherwise a mission's prop is drawn next to
    // the replay's copy. An object decided not to hide is looked at again after
    // kPoseV2LiveObjectRecheckMs (its owner or attachment may change).
    struct PoseV2LiveObjectStats
    {
        std::uint32_t passes = 0;
        std::uint32_t hidden = 0;
        std::uint32_t reshown = 0; // a script drew a hidden one again: hidden again
        std::uint32_t verdicts[replay::objcheck::kLiveHideCount] = {};
        std::uint32_t logs = 0;
        std::uint32_t otherModLogs = 0;
        std::uint32_t baselineObjects = 0; // listed at the playback's first pass
    } g_poseV2LiveObjects;
    std::unordered_map<Object, ULONGLONG> g_poseV2LiveObjectKept;
    constexpr ULONGLONG kPoseV2LiveObjectRecheckMs = 2000;

    void ResetPoseV2LiveObjects()
    {
        g_poseV2LiveObjects = {};
        g_poseV2LiveObjectKept.clear();
        g_poseV2LiveObjectsAtStart.clear();
        g_poseV2LiveObjectsBaseline = false;
    }

    void ClearPoseV2LiveObjects(const PoseV2PlayerOwned& owned, std::size_t& hidden)
    {
        REPLAY_PROFILE_ZONE("play.hide_world.objects");
        static int handles[replay::recorder::kPoolCapacity]{};
        const int count = (std::min)(
            (std::max)(0, replay::objects::ListObjects(handles, replay::recorder::kPoolCapacity)),
            replay::recorder::kPoolCapacity);
        static std::vector<Object> own;
        replay::objects::ReplayOwnObjects(own);
        const ULONGLONG now = GetTickCount64();
        ++g_poseV2LiveObjects.passes;
        if (!g_poseV2LiveObjectsBaseline && count > 0)
        {
            // The first pass with a listing: what the world had when the
            // playback began (the game's scripts are paused from here on).
            g_poseV2LiveObjectsAtStart.insert(handles, handles + count);
            g_poseV2LiveObjectsBaseline = true;
            g_poseV2LiveObjects.baselineObjects = static_cast<std::uint32_t>(count);
        }
        if (g_poseV2LiveObjectKept.size() > 16384)
            g_poseV2LiveObjectKept.clear();
        const auto ownObject = [&](Entity entity) { return entity && std::binary_search(own.begin(), own.end(), entity); };
        for (int index = 0; index < count; ++index)
        {
            const Object object = handles[index];
            if (!object || !replay::entities::Exists(object) || g_poseV2ClearClones.Contains(object))
                continue;
            if (g_poseV2ClearHidden.Contains(object))
            {
                // A script drew it again (or gave its collision back): hidden again.
                const std::uint32_t fixed = replay::pose::playback::RehideWorldPed(object);
                if (fixed & replay::pose::playback::kRehideVisible)
                    ++g_poseV2LiveObjects.reshown;
                continue;
            }
            const auto kept = g_poseV2LiveObjectKept.find(object);
            if (kept != g_poseV2LiveObjectKept.end() && now < kept->second)
                continue;
            replay::objcheck::LiveObject live;
            live.replayOwn = ownObject(object);
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            auto vehicleModel = g_poseV2VehicleModels.find(model);
            if (vehicleModel == g_poseV2VehicleModels.end())
                vehicleModel = g_poseV2VehicleModels.emplace(model, STREAMING::IS_MODEL_A_VEHICLE(model) != FALSE).first;
            live.vehicleModel = vehicleModel->second;
            live.rope = replay::ropemodels::IsRopeModel(model);
            bool otherMod = false;
            if (!live.replayOwn)
            {
                // Another mod's object (a lighting mod's lantern) is never
                // hidden, wherever it stands and whatever holds it.
                live.owner = replay::objects::EntityOwnerOf(object);
                live.missionEntity = replay::pose::playback::IsMissionEntity(object);
                PoseV2ScriptFacts(object, live.missionEntity, live.owner, live.otherScript, live.bornInPlayback);
                otherMod = replay::objcheck::IsOtherModObject(
                    live.otherScript, live.bornInPlayback, live.missionEntity, live.owner);
                if (otherMod && g_poseV2LiveObjects.otherModLogs < 24)
                {
                    ++g_poseV2LiveObjects.otherModLogs;
                    float at[3]{};
                    replay::entities::Position(object, at);
                    char line[224];
                    std::snprintf(line, sizeof(line),
                        "POSE_V2_WORLD_LIVE_OBJECT,action=other_mod,handle=%d,model=0x%x,owner=%u,mission=%d,"
                        "asi_thread=%d,born_in_playback=%d,pos=%.2f;%.2f;%.2f,playhead_ms=%u\n",
                        object, model, live.owner, live.missionEntity ? 1 : 0, live.otherScript ? 1 : 0,
                        live.bornInPlayback ? 1 : 0, at[0], at[1], at[2],
                        static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()));
                    Ctx().log << line;
                }
            }
            if (!live.replayOwn && !otherMod && !live.vehicleModel && !live.rope)
            {
                // Held by the player, what he owns or a puppet (two levels:
                // the item on the holster on the saddle).
                const Entity parent = ENTITY::GET_ENTITY_ATTACHED_TO(object);
                if (parent)
                {
                    live.onKept = owned.Contains(parent) || g_poseV2ClearClones.Contains(parent) || ownObject(parent);
                    if (!live.onKept)
                    {
                        const Entity grand = ENTITY::GET_ENTITY_ATTACHED_TO(parent);
                        live.onKept = grand && (owned.Contains(grand) || g_poseV2ClearClones.Contains(grand) || ownObject(grand));
                    }
                }
                if (!live.onKept)
                {
                    float at[3]{};
                    live.inArea = !replay::entities::Position(object, at) ||
                        replay::objcheck::InClipArea(Ctx().clip.hasArea, at[0], at[1], Ctx().clip.areaMin[0],
                            Ctx().clip.areaMin[1], Ctx().clip.areaMax[0], Ctx().clip.areaMax[1]);
                    if (live.inArea)
                    {
                        live.pickup = OBJECT::DOES_PICKUP_OBJECT_EXIST(object) || OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object);
                    }
                }
            }
            const replay::objcheck::LiveHide verdict = replay::objcheck::HideLiveObject(live);
            ++g_poseV2LiveObjects.verdicts[static_cast<std::size_t>(verdict)];
            if (verdict != replay::objcheck::LiveHide::Hide)
            {
                g_poseV2LiveObjectKept[object] = now + kPoseV2LiveObjectRecheckMs;
                continue;
            }
            replay::pose::playback::HiddenPedState state;
            if (!replay::pose::playback::HideWorldPed(object, state))
                continue;
            g_poseV2HiddenWorld.push_back(state);
            g_poseV2ClearHidden.Add(state.ped);
            ++hidden;
            ++g_poseV2LiveObjects.hidden;
            if (g_poseV2LiveObjects.logs < 48)
            {
                ++g_poseV2LiveObjects.logs;
                float at[3]{};
                replay::entities::Position(object, at);
                char line[256];
                std::snprintf(line, sizeof(line),
                    "POSE_V2_WORLD_LIVE_OBJECT,action=hide,handle=%d,model=0x%x,owner=%u,mission=%d,pickup=%d,attached=%d,"
                    "was_visible=%d,pos=%.2f;%.2f;%.2f,playhead_ms=%u\n",
                    object, model, live.owner, live.missionEntity ? 1 : 0, live.pickup ? 1 : 0,
                    ENTITY::IS_ENTITY_ATTACHED(object) ? 1 : 0, state.visible ? 1 : 0, at[0], at[1], at[2],
                    static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()));
                Ctx().log << line;
            }
        }
    }

    // The ScriptHook pool listing can come back empty in town (worldGetAllPeds
    // and the object listing return 0 even into an 8192 entry array), which
    // would leave the town's NPCs walking through the replay. The engine's own
    // list of the peds and vehicles near a ped (GET_PED_NEARBY_PEDS and
    // _VEHICLES, the size in the first slot) fills the list then, around the
    // hidden player (he follows the replay's focus) and every ped puppet.
    std::uint32_t g_poseV2NearFallbacks = 0;
    int g_poseV2NearLastCount = -1;
    int ListPoseV2NearEntities(int* handles, int capacity, bool wantVehicles)
    {
        int count = 0;
        const auto add = [&](Ped around) {
            if (!around || !replay::entities::Exists(around) || !replay::entities::IsPed(around))
                return;
            constexpr int kSlots = 64;
            Any list[1 + kSlots]{};
            list[0] = kSlots;
            const int found = wantVehicles ? PED::GET_PED_NEARBY_VEHICLES(around, list)
                                           : PED::GET_PED_NEARBY_PEDS(around, list, -1, -1);
            for (int n = 0; n < found && n < kSlots && count < capacity; ++n)
            {
                const Entity entity = static_cast<Entity>(list[1 + n]);
                if (!entity || !replay::entities::Exists(entity))
                    continue;
                if (std::find(handles, handles + count, entity) == handles + count)
                    handles[count++] = entity;
            }
        };
        add(PLAYER::PLAYER_PED_ID());
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone && (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                                    puppet.packet.kind == replay::entity::EntityKind::Horse))
                add(puppet.clone);
        g_poseV2NearLastCount = count;
        return count;
    }

    // The world clear reads the engine's pools (entity_pools.h), as a replay
    // editor's world clear walks its pools: every live ped or vehicle, wherever
    // it is. A handle is made for those without one (at most
    // kPoseV2ClearHandleBudget a pass; the ambient ones are deleted with their
    // handle). -1 when the reader is unavailable (the listing and the nearby
    // lists take over, logged once per playback).
    constexpr int kPoseV2ClearHandleBudget = 256;
    std::uint32_t g_poseV2ClearPoolPasses = 0;
    std::uint32_t g_poseV2ClearPoolCreated = 0;
    std::uint32_t g_poseV2ClearPoolFallbacks = 0;
    replay::pools::ListStats g_poseV2ClearPoolPeds;
    int ListPoseV2WorldPool(replay::pools::Kind kind, int* handles, int capacity, replay::pools::ListStats& stats)
    {
        REPLAY_PROFILE_ZONE("play.hide_world.pools");
        const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const replay::pools::Position center{cam.x, cam.y, cam.z};
        constexpr float kEverywhere = 1.0e7f;
        const int count = replay::pools::ListHandles(
            kind, &center, 1, kEverywhere, kEverywhere, kPoseV2ClearHandleBudget, handles, capacity, &stats);
        if (count >= 0)
            g_poseV2ClearPoolCreated += stats.created;
        return count;
    }

    // The world's ropes are hidden while the replay plays (rope_world.h). Rung
    // 1 (the public _ROPE_CHANGE_VISIBILITY) on ropes listed by the rope
    // manager view (guarded reads, rope_observer.h); nothing is written.
    void HidePoseV2WorldRopes(const PoseV2PlayerOwned& owned)
    {
        REPLAY_PROFILE_ZONE("play.hide_world.ropes");
        static std::vector<replay::rope::RopeInfo> ropes;
        std::uint32_t managerCount = 0;
        if (!replay::rope::Snapshot(ropes, managerCount))
        {
            ++g_poseV2WorldRopeSnapshotFails;
            return;
        }
        ++g_poseV2WorldRopeScans;
        static std::vector<std::uint64_t> puppets;
        puppets.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone && replay::entities::Exists(puppet.clone))
                puppets.push_back(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)));
        std::sort(puppets.begin(), puppets.end());
        const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const auto distance = [&](const float at[3]) {
            const float dx = at[0] - cam.x, dy = at[1] - cam.y, dz = at[2] - cam.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const unsigned long long now = GetTickCount64();
        g_poseV2WorldRopes.BeginScan();
        for (const replay::rope::RopeInfo& rope : ropes)
        {
            replay::ropes::WorldRopeView view;
            view.id = rope.id;
            view.replay = Ctx().clip.ropes.Plays(rope.id);
            std::uint64_t worldEntity = 0;
            const std::uint32_t count = (std::min)(rope.attachmentCount, replay::rope::kMaxAttachments);
            for (std::uint32_t slot = 0; slot < count && !view.replay; ++slot)
            {
                const std::uint64_t entity = rope.attachments[slot].entity;
                if (!entity)
                    continue;
                if (std::binary_search(puppets.begin(), puppets.end(), entity))
                    view.replay = true;
                else if (!worldEntity)
                    worldEntity = entity;
            }
            view.onWorldEntity = worldEntity != 0;
            view.visible = (rope.flags & replay::ropes::kRopeVisibleFlag) != 0;
            view.cameraM = (std::min)(distance(rope.first), distance(rope.last));
            const replay::ropes::WorldRopeStep step = g_poseV2WorldRopes.Observe(view, now);
            if (step.action != replay::ropes::WorldRopeAction::Leave)
            {
                int handle = static_cast<int>(rope.id);
                const bool exists = PHYSICS::DOES_ROPE_EXIST(handle) != FALSE;
                const bool show = step.action == replay::ropes::WorldRopeAction::Show;
                if (exists)
                    PHYSICS::_ROPE_CHANGE_VISIBILITY(&handle, show ? TRUE : FALSE);
                else
                    ++g_poseV2WorldRopeNoHandle;
                if (replay::log::Verbose() && g_poseV2WorldRopeLogs.Take(40))
                    Ctx().log << "POSE_V2_WORLD_ROPE,action=" << (show ? "show" : "hide") << ",engine_id=" << rope.id
                              << ",type=" << rope.type << ",flags=0x" << std::hex << rope.flags << std::dec
                              << ",exists=" << (exists ? 1 : 0) << ",replay=" << (view.replay ? 1 : 0)
                              << ",camera_m=" << static_cast<int>(view.cameraM)
                              << ",visible_bit=" << replay::ropes::VisibleBitText(g_poseV2WorldRopes.Bit()) << '\n';
            }
            if (step.stray)
            {
                // rope_stray (qa_health.h): drawn near the camera after it
                // was hidden. Who holds it: the player, a hidden world
                // entity, or another entity of the world.
                const char* holder = "world";
                const std::uint64_t player = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(owned.entities[0]));
                for (std::uint32_t slot = 0; slot < count; ++slot)
                {
                    const std::uint64_t entity = rope.attachments[slot].entity;
                    if (!entity)
                        continue;
                    if (entity == player)
                    {
                        holder = "player";
                        break;
                    }
                    for (const Entity own : owned.entities)
                        if (own && entity == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(own)))
                            holder = "player_owned";
                    if (std::strcmp(holder, "world") == 0)
                        for (const replay::pose::playback::HiddenPedState& state : g_poseV2HiddenWorld)
                            if (entity == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(state.ped)))
                            {
                                holder = "hidden";
                                break;
                            }
                }
                Ctx().log << "POSE_V2_ROPE_STRAY,engine_id=" << rope.id << ",type=" << rope.type
                          << ",camera_m=" << static_cast<int>(view.cameraM) << ",holder=" << holder
                          << ",entity=0x" << std::hex << worldEntity << std::dec
                          << ",playhead_ms=" << static_cast<std::uint32_t>(Ctx().transport.PlayheadMs()) << '\n';
            }
        }
        g_poseV2WorldRopes.EndScan();
    }

    // World clear: ambient population is deleted (it respawns after playback
    // and frees pool room for puppets); mission and player entities are hidden
    // and restored.
    void HidePoseV2World()
    {
        REPLAY_PROFILE_ZONE("play.hide_world");
        // Room for a whole pool. A listing into a smaller array may come back
        // empty (the town pools; see ListPoseV2NearEntities).
        constexpr int kListCapacity = 8192;
        static int handles[kListCapacity]{};
        const std::uint64_t deletedBefore = g_poseV2WorldDeleted;
        std::size_t hidden = 0;
        PoseV2PlayerOwned owned;
        owned.Refresh();
        g_poseV2ClearPeds = 0;
        g_poseV2ClearVehicles = 0;
        g_poseV2ClearSampleModel = 0;
        // This pass's membership sets.
        g_poseV2ClearClones.Assign(Ctx().puppets.list, [](const replay::playback::Puppet& puppet) { return puppet.clone; });
        g_poseV2ClearHidden.Assign(g_poseV2HiddenWorld,
            [](const replay::pose::playback::HiddenPedState& state) { return state.ped; });
        // The pools first (ListPoseV2WorldPool).
        replay::pools::ListStats poolStats;
        int count = ListPoseV2WorldPool(replay::pools::Kind::Vehicle, handles, kListCapacity, poolStats);
        const bool fromPool = count >= 0;
        if (fromPool)
        {
            ++g_poseV2ClearPoolPasses;
        }
        else
        {
            ++g_poseV2ClearPoolFallbacks;
            count = (std::min)((std::max)(0, worldGetAllVehicles(handles, kListCapacity)), kListCapacity);
            if (count == 0)
                count = ListPoseV2NearEntities(handles, kListCapacity, true);
        }
        ClearPoseV2WorldPool(handles, count, true, owned, hidden);
        count = fromPool ? ListPoseV2WorldPool(replay::pools::Kind::Ped, handles, kListCapacity, g_poseV2ClearPoolPeds) : -1;
        if (count < 0)
            count = (std::min)((std::max)(0, worldGetAllPeds(handles, kListCapacity)), kListCapacity);
        const int poolPeds = count;
        if (count == 0 && !fromPool)
        {
            // The pool listing failed (see ListPoseV2NearEntities).
            count = ListPoseV2NearEntities(handles, kListCapacity, false);
            ++g_poseV2NearFallbacks;
        }
        ClearPoseV2WorldPool(handles, count, false, owned, hidden);
        g_poseV2ClearDebris = 0;
        ClearPoseV2VehicleDebris(hidden);
        ClearPoseV2LiveObjects(owned, hidden);
        HidePoseV2WorldRopes(owned);
        // The census after the clear (see g_poseV2WorldCensusAt).
        const ULONGLONG censusNow = GetTickCount64();
        // The census reads the world only for its line: Debug=1 only.
        if (replay::log::Verbose() && censusNow >= g_poseV2WorldCensusAt && g_poseV2WorldCensusLogs < 30)
        {
            g_poseV2WorldCensusAt = censusNow + 3000;
            ++g_poseV2WorldCensusLogs;
            const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
            int nearCount = 0, nearVisible = 0, puppets = 0;
            Hash sample = 0;
            int samplePopulation = -1;
            bool sampleMission = false;
            for (int index = 0; index < count; ++index)
            {
                const Entity entity = handles[index];
                if (!entity || !replay::entities::Exists(entity))
                    continue;
                if (g_poseV2ClearClones.Contains(entity))
                {
                    ++puppets;
                    continue;
                }
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
                const float dx = at.x - cam.x, dy = at.y - cam.y, dz = at.z - cam.z;
                if (dx * dx + dy * dy + dz * dz > 60.0f * 60.0f)
                    continue;
                ++nearCount;
                if (ENTITY::IS_ENTITY_VISIBLE(entity))
                {
                    ++nearVisible;
                    sample = ENTITY::GET_ENTITY_MODEL(entity);
                    samplePopulation = ENTITY::GET_ENTITY_POPULATION_TYPE(entity);
                    sampleMission = replay::pose::playback::IsMissionEntity(entity);
                }
            }
            Ctx().log << "POSE_V2_WORLD_CENSUS,source=" << (fromPool ? "pool" : "listing") << ",pool_peds=" << poolPeds
                      << ",pool_peds_live=" << (fromPool ? static_cast<int>(g_poseV2ClearPoolPeds.live) : -1)
                      << ",pool_passes=" << g_poseV2ClearPoolPasses << ",pool_fallbacks=" << g_poseV2ClearPoolFallbacks
                      << ",pool_handles_created=" << g_poseV2ClearPoolCreated
                      << ",guid_free=" << (fromPool ? static_cast<int>(g_poseV2ClearPoolPeds.guidFree) : -1)
                      << ",pool_us=" << (fromPool ? static_cast<int>(g_poseV2ClearPoolPeds.us + poolStats.us) : -1)
                      << ",near_fallbacks=" << g_poseV2NearFallbacks << ",near_count=" << g_poseV2NearLastCount
                      << ",peds_listed=" << count << ",puppets=" << puppets << ",near_60m=" << nearCount
                      << ",near_visible=" << nearVisible << ",sample_model=0x" << std::hex << sample << std::dec
                      << ",sample_population=" << samplePopulation << ",sample_mission=" << (sampleMission ? 1 : 0)
                      << ",hidden_total=" << g_poseV2HiddenWorld.size() << ",rehidden_total=" << g_poseV2WorldRehidden << '\n';
        }
        const std::uint64_t deleted = g_poseV2WorldDeleted - deletedBefore;
        if ((hidden > 0 || deleted > 0) && replay::log::Verbose()) // every clearing tick, Debug=1 only
        {
            std::ostream& log = Ctx().log;
            log << "POSE_V2_WORLD_CLEAR"
                << ",deleted=" << deleted
                << ",peds=" << g_poseV2ClearPeds
                << ",vehicles=" << g_poseV2ClearVehicles
                << ",vehicle_debris=" << g_poseV2ClearDebris
                << ",sample_model=0x" << std::hex << g_poseV2ClearSampleModel << std::dec
                << ",hidden=" << hidden
                << ",hidden_total=" << g_poseV2HiddenWorld.size()
                << ",deleted_total=" << g_poseV2WorldDeleted
                << '\n';
            log.flush();
        }
    }

    // Leftovers of the recording, deleted before any puppet exists (nothing
    // comes back): every pickup that appeared during the recording (a dead
    // NPC's weapon), and every loose object that appeared during it with the
    // model of a weapon or prop the recording registered (knocked hats, dropped
    // weapons). Map objects that streamed in meanwhile are left alone: deleting
    // every new object takes hundreds of map props with it.
    void DeletePoseV2RecordingLeftovers()
    {
        // A clip from a file has no objects-before list of this session:
        // everything would look like a leftover of it.
        if (Ctx().clip.fromDisk)
        {
            Ctx().log << "POSE_V2_WORLD_LEFTOVERS,skipped=from_disk\n";
            return;
        }
        static int handles[replay::recorder::kPoolCapacity]{};
        const std::vector<int>& objectsBefore = Ctx().clip.objectsBefore;
        const std::vector<int>& pickupsBefore = Ctx().clip.pickupsBefore;
        std::vector<std::uint32_t> models;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (packet.kind != replay::entity::EntityKind::Ped && packet.kind != replay::entity::EntityKind::Horse &&
                packet.kind != replay::entity::EntityKind::Vehicle && packet.kind != replay::entity::EntityKind::TrainCar)
                models.push_back(packet.modelHash);
        std::sort(models.begin(), models.end());
        std::size_t pickupsRemoved = 0;
        const int pickups = (std::min)((std::max)(0, worldGetAllPickups(handles, replay::recorder::kPoolCapacity)),
            replay::recorder::kPoolCapacity);
        std::size_t pickupsKeptBefore = 0;
        std::size_t pickupsNotExisting = 0;
        std::size_t pickupsDeletedAsObject = 0;
        for (int n = 0; n < pickups; ++n)
        {
            if (std::binary_search(pickupsBefore.begin(), pickupsBefore.end(), handles[n]))
            {
                ++pickupsKeptBefore;
                continue;
            }
            if (!OBJECT::DOES_PICKUP_EXIST(handles[n]))
            {
                // The ScriptHook pickup listing hands out entity handles of
                // pickup objects (DOES_PICKUP_EXIST is false for them): delete
                // the object when it was born during the recording, inside its
                // area.
                Object object = handles[n];
                bool inArea = !Ctx().clip.hasArea;
                if (replay::entities::Exists(object) && !ENTITY::IS_ENTITY_ATTACHED(object) && !inArea)
                {
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    inArea = p.x >= Ctx().clip.areaMin[0] - 30.0f && p.x <= Ctx().clip.areaMax[0] + 30.0f &&
                        p.y >= Ctx().clip.areaMin[1] - 30.0f && p.y <= Ctx().clip.areaMax[1] + 30.0f;
                }
                if (replay::entities::Exists(object) && !ENTITY::IS_ENTITY_ATTACHED(object) && inArea &&
                    !replay::pose::playback::IsMissionEntity(object))
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                    OBJECT::DELETE_OBJECT(&object);
                    ++pickupsDeletedAsObject;
                }
                else
                    ++pickupsNotExisting;
                continue;
            }
            OBJECT::REMOVE_PICKUP(handles[n]);
            ++pickupsRemoved;
        }
        std::size_t objectsDeleted = 0;
        std::size_t pickupObjectsDeleted = 0;
        std::size_t adoptedDeleted = 0;
        std::size_t scriptsKept = 0; // a script's (a mod's) object: objcheck::LeftoverDeletable
        for (int handle : Ctx().clip.adoptedObjects)
        {
            Object object = handle;
            if (!object || !replay::entities::Exists(object) || ENTITY::IS_ENTITY_ATTACHED(object) ||
                std::binary_search(objectsBefore.begin(), objectsBefore.end(), object))
                continue;
            if (!replay::objcheck::LeftoverDeletable(replay::pose::playback::IsMissionEntity(object)))
            {
                ++scriptsKept;
                continue;
            }
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            OBJECT::DELETE_OBJECT(&object);
            ++adoptedDeleted;
        }
        Ctx().clip.adoptedObjects.clear();
        // Guarded like the recording's objects-before list (with the pool
        // short, only objects that already have a handle are looked at).
        const char* objectsMode = "listing";
        const int objects = (std::min)((std::max)(0, replay::objects::ListAllObjectsGuarded(handles,
            replay::recorder::kPoolCapacity, objectsMode)), replay::recorder::kPoolCapacity);
        const int listedObjects = objects;
        for (int n = 0; n < objects; ++n)
        {
            Object object = handles[n];
            if (!object || !replay::entities::Exists(object) || std::binary_search(objectsBefore.begin(), objectsBefore.end(), object))
                continue;
            if (ENTITY::IS_ENTITY_ATTACHED(object))
                continue;
            // RDR2 pickups (a dead NPC's dropped weapon, loot) are objects with
            // a pickup record; worldGetAllPickups never lists them. Pickup
            // objects born during the recording near its area go.
            const bool pickup = OBJECT::DOES_PICKUP_OBJECT_EXIST(object) || OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object);
            if (pickup && !replay::objcheck::LeftoverDeletable(replay::pose::playback::IsMissionEntity(object)))
            {
                ++scriptsKept;
                continue;
            }
            if (pickup)
            {
                bool inArea = !Ctx().clip.hasArea;
                if (!inArea)
                {
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    inArea = p.x >= Ctx().clip.areaMin[0] - 30.0f && p.x <= Ctx().clip.areaMax[0] + 30.0f &&
                        p.y >= Ctx().clip.areaMin[1] - 30.0f && p.y <= Ctx().clip.areaMax[1] + 30.0f;
                }
                if (inArea)
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                    OBJECT::DELETE_OBJECT(&object);
                    ++pickupObjectsDeleted;
                }
                continue;
            }
            if (replay::pose::playback::IsMissionEntity(object))
                continue;
            if (!std::binary_search(models.begin(), models.end(), static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object))))
                continue;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            OBJECT::DELETE_OBJECT(&object);
            ++objectsDeleted;
        }
        Ctx().log << "POSE_V2_WORLD_LEFTOVERS,objects_mode=" << objectsMode << ",pickups_before=" << pickupsBefore.size() << ",pickups_removed=" << pickupsRemoved
            << ",pickups_kept_before=" << pickupsKeptBefore << ",pickups_not_existing=" << pickupsNotExisting
            << ",pickups_deleted_as_object=" << pickupsDeletedAsObject
            << ",pickup_objects_deleted=" << pickupObjectsDeleted << ",leftover_models=" << models.size()
            << ",objects_deleted=" << objectsDeleted << ",adopted_deleted=" << adoptedDeleted
            << ",scripts_kept=" << scriptsKept
            << ",listed_objects=" << listedObjects << ",listed_pickups=" << pickups << '\n';
    }

    // Every hidden entity is shown at once (world_landing.h); one that was not
    // frozen before the replay stays frozen without collision until the map
    // collision around it is in (UpdateWorldLanding), the player's horse
    // included. One frozen before the replay has nothing to fall from: restored
    // at once.
    void RestorePoseV2World()
    {
        REPLAY_PROFILE_ZONE("play.restore_world");
        const unsigned long long now = GetTickCount64();
        if (g_poseV2Landing.Empty())
        {
            g_poseV2Landing.ResetStats();
            g_poseV2LandingImmediate = 0;
            g_poseV2LandingTimeoutModel = 0;
            g_poseV2LandingPlayerHorse = false;
            g_poseV2LandingPlayerHorseTimedOut = false;
            g_poseV2LandingPlayerHorseMs = 0;
        }
        PoseV2PlayerOwned owned;
        owned.Refresh();
        const Entity playerHorse = owned.entities[1] ? owned.entities[1] : owned.entities[3];
        std::size_t immediate = 0, queued = 0, gone = 0, invisible = 0;
        std::uint32_t invisibleLogs = 0;
        for (const replay::pose::playback::HiddenPedState& state : g_poseV2HiddenWorld)
        {
            // Gone, or its handle now another entity's (its owner deleted it).
            if (!replay::pose::playback::StillHiddenEntity(state))
            {
                ++gone;
                continue;
            }
            // The detector: an entity given back invisible was invisible when
            // the replay first hid it and nobody showed it meanwhile; a mission
            // ped among them would be an invisible enemy after the replay. One
            // line each (16).
            if (!state.visible)
            {
                ++invisible;
                if (invisibleLogs < 16)
                {
                    ++invisibleLogs;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(state.ped, TRUE, FALSE);
                    Ctx().log << "POSE_V2_WORLD_RESTORE_INVISIBLE,handle=" << state.ped << ",model=0x" << std::hex
                           << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(state.ped)) << std::dec
                           << ",ped=" << (ENTITY::IS_ENTITY_A_PED(state.ped) ? 1 : 0)
                           << ",mission=" << (replay::pose::playback::IsMissionEntity(state.ped) ? 1 : 0)
                           << ",dead=" << (ENTITY::IS_ENTITY_DEAD(state.ped) ? 1 : 0)
                           << ",alpha=" << ENTITY::GET_ENTITY_ALPHA(state.ped) << ",pos=" << at.x << ";" << at.y << ";"
                           << at.z << '\n';
                }
            }
            if (state.frozen)
            {
                replay::pose::playback::RestoreWorldPed(state);
                ++immediate;
                continue;
            }
            replay::pose::playback::BeginWorldPedLanding(state);
            g_poseV2Landing.Add(state.ped, replay::pose::playback::MountOfWorldPed(state.ped), now, state);
            if (state.ped == playerHorse || state.ped == owned.entities[2])
            {
                g_poseV2LandingPlayerHorse = true;
                g_poseV2LandingPlayerHorseHandle = state.ped;
            }
            ++queued;
        }
        g_poseV2LandingImmediate += static_cast<std::uint32_t>(immediate);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_WORLD_RESTORE_SUMMARY" // kept with Debug=0
            << ",restored=" << g_poseV2HiddenWorld.size()
            << ",immediate=" << immediate
            << ",landing=" << queued
            << ",gone=" << gone
            << ",player_horse=" << (g_poseV2LandingPlayerHorse ? 1 : 0)
            << ",refrozen=" << g_poseV2WorldRefrozen
            << ",recollided=" << g_poseV2WorldRecollided
            << ",reshown=" << g_poseV2WorldRehidden
            << ",owner_changes=" << g_poseV2WorldOwnerChanges
            << ",restored_invisible=" << invisible
            << '\n';
        // The live objects that are not the map's.
        {
            const std::uint32_t* v = g_poseV2LiveObjects.verdicts;
            using LH = replay::objcheck::LiveHide;
            log << "POSE_V2_WORLD_LIVE_OBJECTS,passes=" << g_poseV2LiveObjects.passes << ",hidden=" << g_poseV2LiveObjects.hidden
                << ",reshown=" << g_poseV2LiveObjects.reshown
                << ",looked_hide=" << v[static_cast<int>(LH::Hide)] << ",looked_replay=" << v[static_cast<int>(LH::Replay)]
                << ",looked_kept=" << v[static_cast<int>(LH::OnKept)] << ",looked_outside=" << v[static_cast<int>(LH::Outside)]
                << ",looked_map=" << v[static_cast<int>(LH::Map)] << ",looked_rope=" << v[static_cast<int>(LH::Rope)]
                << ",looked_debris=" << v[static_cast<int>(LH::Debris)]
                << ",looked_other_mod=" << v[static_cast<int>(LH::OtherMod)]
                << ",baseline_objects=" << g_poseV2LiveObjects.baselineObjects << '\n';
            ResetPoseV2LiveObjects();
        }
        log.flush();
        g_poseV2HiddenWorld.clear();
    }

    // The world's ropes shown again when the playback stops.
    void RestorePoseV2WorldRopes()
    {
        const replay::ropes::WorldRopeHider::Counters stats = g_poseV2WorldRopes.Stats();
        const replay::ropes::VisibleBit bit = g_poseV2WorldRopes.Bit();
        const std::size_t held = g_poseV2WorldRopes.Size();
        std::uint32_t shown = 0, missing = 0;
        for (const std::uint32_t id : g_poseV2WorldRopes.TakeRestore())
        {
            int handle = static_cast<int>(id);
            if (!PHYSICS::DOES_ROPE_EXIST(handle))
            {
                ++missing;
                continue;
            }
            PHYSICS::_ROPE_CHANGE_VISIBILITY(&handle, TRUE);
            ++shown;
        }
        if (!g_poseV2WorldRopeScans && !stats.hidden)
            return;
        Ctx().log << "POSE_V2_WORLD_ROPES_SUMMARY,scans=" << g_poseV2WorldRopeScans
                  << ",snapshot_fails=" << g_poseV2WorldRopeSnapshotFails << ",hidden=" << stats.hidden
                  << ",rehidden=" << stats.rehidden << ",released=" << stats.released << ",gone=" << stats.gone
                  << ",strays=" << stats.strays << ",held_at_stop=" << held << ",shown=" << shown
                  << ",missing=" << missing << ",no_handle=" << g_poseV2WorldRopeNoHandle
                  << ",hide_held=" << stats.hideHeld << ",hide_missed=" << stats.hideMissed
                  << ",visible_bit=" << replay::ropes::VisibleBitText(bit) << '\n';
    }

    // One tick of the landing queue (any time, also after the playback
    // stopped).
    void UpdatePoseV2WorldLanding()
    {
        if (g_poseV2Landing.Empty())
            return;
        REPLAY_PROFILE_ZONE("tick.world_landing");
        const unsigned long long now = GetTickCount64();
        static std::vector<std::int32_t> requested;
        requested.clear();
        g_poseV2Landing.ForRequests(replay::landing::kRequestsPerTick,
            [&](const auto& entry) { requested.push_back(entry.handle); });
        g_poseV2Landing.Tick(
            now, [](std::int32_t handle) { return replay::entities::Exists(handle); },
            [&](const auto& entry) {
                const bool request = std::find(requested.begin(), requested.end(), entry.handle) != requested.end();
                return replay::pose::playback::WorldPedCollisionLoaded(entry.handle, request);
            },
            [&](const auto& entry, replay::landing::Action action, unsigned long long waitedMs) {
                if (action == replay::landing::Action::CollisionOn)
                {
                    replay::pose::playback::LandWorldPedCollision(entry.payload);
                    if (entry.timedOut && !g_poseV2LandingTimeoutModel)
                    {
                        g_poseV2LandingTimeoutModel = ENTITY::GET_ENTITY_MODEL(entry.handle);
                        g_poseV2LandingTimeoutAt = ENTITY::GET_ENTITY_COORDS(entry.handle, TRUE, FALSE);
                    }
                    if (entry.handle == g_poseV2LandingPlayerHorseHandle)
                    {
                        g_poseV2LandingPlayerHorseMs = waitedMs;
                        g_poseV2LandingPlayerHorseTimedOut = entry.timedOut;
                    }
                    return;
                }
                replay::pose::playback::RestoreWorldPed(entry.payload);
            });
        if (!g_poseV2Landing.Empty())
            return;
        const replay::landing::Stats& stats = g_poseV2Landing.Totals();
        Ctx().log << "POSE_V2_WORLD_LANDING_SUMMARY,queued=" << stats.queued << ",immediate=" << g_poseV2LandingImmediate
                  << ",waited=" << stats.waited << ",landed=" << stats.landed << ",timed_out=" << stats.timedOut
                  << ",gone=" << stats.gone << ",max_wait_ms=" << stats.maxWaitMs
                  << ",avg_wait_ms=" << (stats.waited ? stats.totalWaitMs / stats.waited : 0)
                  << ",player_horse=" << (g_poseV2LandingPlayerHorse ? 1 : 0)
                  << ",player_horse_ms=" << g_poseV2LandingPlayerHorseMs
                  << ",player_horse_timed_out=" << (g_poseV2LandingPlayerHorseTimedOut ? 1 : 0)
                  << ",timeout_model=0x" << std::hex << g_poseV2LandingTimeoutModel << std::dec
                  << ",timeout_at=" << g_poseV2LandingTimeoutAt.x << ';' << g_poseV2LandingTimeoutAt.y << ';'
                  << g_poseV2LandingTimeoutAt.z << '\n';
        g_poseV2LandingPlayerHorseHandle = 0;
    }

    void ResetPoseV2Watch(std::uint32_t startMs);

    void StartPoseV2Playback()
    {
        replay::objects::EnsureStrapVisibility();
        Ctx().recorder.ignored.clear();
        // A skeleton map request left by a puppet not deleted.
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.mapRequest >= 0)
                replay::pose::capture::CancelSkeletonInfo(puppet.mapRequest);
        Ctx().puppets.list.clear();
        g_poseV2HiddenWorld.clear();
        ResetPoseV2LiveObjects(); // the first pass takes the baseline again
        // Entities of the last playback still waiting for the map collision are
        // hidden by this one (the scan re-hides them).
        for (const replay::pose::playback::HiddenPedState& state : g_poseV2Landing.TakeAll())
            if (state.ped && replay::entities::Exists(state.ped))
                g_poseV2HiddenWorld.push_back(state);
        g_poseV2WorldRefrozen = 0;
        g_poseV2WorldRecollided = 0;
        g_poseV2WorldRehidden = 0;
        g_poseV2WorldOwnerChanges = 0;
        g_poseV2WorldRopes.Reset();
        g_poseV2WorldRopeScans = 0;
        g_poseV2WorldRopeSnapshotFails = 0;
        g_poseV2WorldRopeNoHandle = 0;
        DeletePoseV2RecordingLeftovers();
        g_poseV2WorldDeleted = 0;
        g_poseV2NextWorldScanAt = 0;
        g_poseV2ScorchPlayLogs = 0;
        g_poseV2AppearancePlayLogs = 0;
        g_poseV2WindowPlayLogs = 0;
        g_poseV2LookVerifyLogs = 0;
        g_poseV2PartPlayLogs = 0;
        g_poseV2PartKeptLogged.clear();
        for (std::uint32_t& count : g_poseV2VehicleFragResults)
            count = 0;
        g_poseV2VehicleFragRebuilds = 0;
        g_poseV2VehicleFragLogs = 0;
        g_poseV2BurntPlayLogs = 0;
        g_poseV2BurnLevelPlayLogs = 0;
        g_poseV2BurnLevelWrites = 0;
        g_poseV2BurnLevelEngineChanged = 0;
        g_poseV2FireCharPlayLogs = 0;
        g_poseV2FireCharWrites = 0;
        g_poseV2FireCharRemoves = 0;
        g_poseV2FireCharCreated = 0;
        g_poseV2FireCharCreateFailed = 0;
        g_poseV2FireCharShapeDiffers = 0;
        g_poseV2PlaceDriftLogs = 0;
        g_poseV2UnposedHideLogs = 0;
        g_poseV2Unposed.Reset();
        g_poseV2Mounts.Clear();
        g_poseV2Mounts.ResetShared();
        // The clip's recorded ped visibility by id.
        g_poseV2PedVisibility.clear();
        g_poseV2RecordedVisibilityLogs = 0;
        for (const replay::PedVisibility& row : Ctx().clip.pedVisibility)
            g_poseV2PedVisibility[row.id.Packed()] = &row;
        // The recorded player (player_hold.h).
        {
            std::vector<replay::playerhold::Candidate> candidates;
            candidates.reserve(Ctx().clip.created.size());
            for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
                candidates.push_back({packet.id.Packed(), packet.kind == replay::entity::EntityKind::Ped,
                    packet.modelHash, packet.timeMs});
            g_poseV2PlayerPacked = replay::playerhold::PickPlayer(candidates, Ctx().clip.place.playerModel);
            g_poseV2PlayerHoldLogs = 0;
            g_poseV2PlayerFirstMount = 0;
            std::uint32_t firstMs = 0, lastMs = 0;
            const bool track = g_poseV2PlayerPacked &&
                replay::pose::capture::TrackSpan(g_poseV2PlayerPacked, firstMs, lastMs);
            // The horse under him at his first frame: the playback's own
            // rider-to-mount rule (pose_jerk.h MountPairs) over the recorded
            // transforms of that frame, the horses recorded at it only.
            std::uint32_t horsesAtFirst = 0;
            if (track)
            {
                const replay::entity::ReplayId playerId = replay::entity::ReplayId::FromPacked(g_poseV2PlayerPacked);
                std::vector<replay::jerk::Body> bodies;
                std::vector<replay::pose::WorldTransform> horseWorlds;
                replay::jerk::Body rider;
                rider.packed = g_poseV2PlayerPacked;
                if (PoseV2RecordedWorldAt(playerId, firstMs, rider.world))
                {
                    bodies.push_back(rider);
                    for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
                    {
                        if (packet.kind != replay::entity::EntityKind::Horse)
                            continue;
                        const replay::entity::PoseSegment* segment = PoseV2SegmentAt(packet.id, firstMs);
                        const std::uint32_t recordId = segment ? segment->recordId : packet.id.Packed();
                        std::uint32_t horseFirst = 0, horseLast = 0;
                        replay::jerk::Body horse;
                        horse.packed = packet.id.Packed();
                        horse.horse = true;
                        if (!replay::pose::capture::TrackSpan(recordId, horseFirst, horseLast) || firstMs < horseFirst ||
                            firstMs > horseLast || !PoseV2RecordedWorldAt(packet.id, firstMs, horse.world))
                            continue;
                        bodies.push_back(horse);
                        ++horsesAtFirst;
                    }
                    replay::jerk::MountPairs pairs;
                    pairs.Update(bodies);
                    const std::uint32_t mount = pairs.MountOf(g_poseV2PlayerPacked);
                    for (const replay::jerk::Body& body : bodies)
                    {
                        if (mount && body.packed == mount)
                        {
                            g_poseV2PlayerFirstMount = mount;
                            g_poseV2PlayerRiderFirst = rider.world;
                            g_poseV2PlayerMountFirst = body.world;
                        }
                    }
                }
            }
            Ctx().log << "POSE_V2_PLAYER,id="
                      << (g_poseV2PlayerPacked ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(g_poseV2PlayerPacked))
                                               : std::string("none"))
                      << ",model=0x" << std::hex << Ctx().clip.place.playerModel << std::dec
                      << ",track=" << (track ? 1 : 0) << ",first_ms=" << firstMs << ",last_ms=" << lastMs
                      << ",first_mount="
                      << (g_poseV2PlayerFirstMount
                                 ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(g_poseV2PlayerFirstMount))
                                 : std::string("none"))
                      << ",horses_at_first=" << horsesAtFirst << '\n';
        }
        // The never animated hide and the T-pose watch.
        g_tposeSignatures.clear();
        g_tposeWatch.Reset();
        g_tposeGate.Reset();
        g_tposeCounts = {};
        g_poseV2Jerk.Reset();
        g_poseV2PushedUntil.clear();
        g_poseV2JerkLogs = 0;
        g_poseV2LodSummary = {};
        g_poseV2MetapedPlay = {};
        g_poseV2ShopPlay = {};
        g_poseV2Switch = {};
        g_noRecord = {}; // the categories are kept (a property of the drawable)
        g_poseV2HatFitted.clear();
        g_poseV2HatTotals = {};
        Ctx().log << replay::metaped::engine::ReaderStatus() << ",clip_metaped_entities=" << Ctx().clip.metapeds.size()
                  << '\n';
        g_poseV2LowLodLogs = 0;
        g_poseV2AnimSkipLogs = 0;
        g_poseV2LodLastPlayhead = g_poseV2LodPrevPlayhead = ~0u;
        g_poseV2MountLogs = 0;
        g_poseV2CarriedTicks = 0;
        g_poseV2PushedHolds = 0;
        g_poseV2WriteCheckLogs = 0;
        for (std::uint32_t slot = 0; slot < replay::pose::capture::kMaxOverrides; ++slot)
        {
            g_poseV2WriteCheckAt[slot] = 0;
            g_poseV2WriteChanged[slot] = false;
        }
        g_poseV2Preparing = true;
        g_poseV2PrepareDeadline = GetTickCount64() + kPoseV2PrepareTimeoutMs;
        // Hold where every entity of the recording's opening census is alive:
        // the recorder lists the scene over its first ticks (the player at 59
        // ms, two peds at 60 ms: a hold at the first create would release with
        // one puppet and run the clock before the others are built).
        constexpr std::uint32_t kOpeningCensusMs = 100; // a few ticks: one-shot events before the hold are skipped
        std::uint32_t firstCreateMs = 0;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
        {
            if (&packet == &Ctx().clip.created.front() || packet.timeMs < firstCreateMs)
                firstCreateMs = packet.timeMs;
        }
        // ... and where each of them has its first pose sample: a puppet is
        // hidden before it (a still ragdoll is sampled a little after the
        // census).
        constexpr std::uint32_t kOpeningPoseMs = 1000;
        std::uint32_t holdMs = firstCreateMs;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
        {
            if (packet.timeMs > firstCreateMs + kOpeningCensusMs)
                continue;
            holdMs = (std::max)(holdMs, packet.timeMs);
            std::uint32_t firstPoseMs = 0;
            std::uint32_t lastPoseMs = 0;
            if (replay::pose::capture::TrackSpan(packet.id.Packed(), firstPoseMs, lastPoseMs) &&
                firstPoseMs <= firstCreateMs + kOpeningPoseMs)
            {
                holdMs = (std::max)(holdMs, firstPoseMs);
            }
        }
        Ctx().transport.Seek(holdMs);
        // A scrub back never goes before it: at 0 nothing of the scene is alive
        // yet (every puppet would be deleted).
        Ctx().transport.SetStartMs(holdMs);
        ResetPoseV2Watch(holdMs);
        replay::events::BeginPlayback(holdMs);
        Ctx().clip.ropes.BeginPlayback();
        {
            // The reins guard, installed once at the first playback
            // (reins_guard.h).
            static bool tried = false;
            if (!tried)
            {
                tried = true;
                std::string reinsError;
                const bool reinsReady = replay::reins::Install(reinsError);
                Ctx().log << "POSE_V2_REINS_GUARD,status=" << (reinsReady ? "installed" : "failed")
                          << ",hold=" << (replay::reins::HoldInstalled() ? "installed" : "off")
                          << ",error=" << (reinsError.empty() ? "none" : reinsError.c_str()) << ",engine_writes=0\n";
            }
        }
        replay::objects::BeginPlayback();
        replay::trains::BeginPlayback();
        replay::vehdoors::BeginPlayback(Ctx().clip.vehicleDoors);
        g_poseV2PlaybackActive = !Ctx().clip.created.empty();
        g_poseV2AliveStamp = replay::entity::LifecycleTrack::kNoAliveStamp - 1; // a new clip
        g_poseV2AliveCopies = 0;
        g_poseV2NoCollisionCalls = g_poseV2HeadBoneCalls = 0;
        g_poseV2PoseSamples = g_poseV2PoseSampleReuses = 0;
        std::ostream& log = Ctx().log;
        {
            std::string vegetationError;
            const bool vegetation = replay::vegetation::Install(vegetationError);
            log << "POSE_V2_VEGETATION,status=" << (vegetation ? "installed" : "failed") << ",error="
                << (vegetation ? "none" : vegetationError.c_str()) << ",overrides_so_far=" << replay::vegetation::Overrides() << '\n';
        }
        log << "POSE_V2_PLAYBACK_START"
            << ",entities=" << Ctx().clip.created.size()
            << ",hold_ms=" << holdMs
            << ",blocks=" << Ctx().clip.lifecycle.BlockCount()
            << '\n';
        log.flush();
    }

    // keepSignature keeps the puppet's parent table read for its skeleton (a
    // recheck that found a mismatch arms again through the map with it); every
    // other release forgets it (a new skeleton, a segment switch, a deleted
    // puppet: compared again at the next arm).
    void ReleasePoseV2Override(replay::playback::Puppet& puppet, bool keepSignature = false);

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
        if (g_poseV2LodConfig.multiplier)
            PED::SET_PED_LOD_MULTIPLIER(puppet.clone, static_cast<float>(g_poseV2LodConfig.multiplier));
        if (g_poseV2LodConfig.prerender)
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
            if (g_poseV2LodConfig.prerender)
                ENTITY::SET_ENTITY_ALWAYS_PRERENDER(puppet.clone, FALSE);
            if (g_poseV2LodConfig.multiplier)
                PED::SET_PED_LOD_MULTIPLIER(puppet.clone, 1.0f);
            ++g_poseV2LodSummary.undone;
        }
        g_poseV2LodSummary.Add(puppet.packet.id.Packed(), puppet.lod);
        puppet.lod = replay::lod::PuppetState{};
    }

    // The puppets' anim directors at LOD 0 and visible (PuppetAnimLod0,
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
        static const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        g_poseV2AnimLodDirectors.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (!PoseV2LodBody(puppet) || !puppet.clone || puppet.lod.clone != puppet.clone)
                continue;
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(puppet.clone));
            const std::uintptr_t director = replay::animlod::DirectorOf(moduleBase, address,
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
                  << ",multiplier=" << g_poseV2LodConfig.multiplier << ",prerender=" << (g_poseV2LodConfig.prerender ? 1 : 0)
                  << ",force_update=" << (g_poseV2LodConfig.forceUpdate ? 1 : 0) << ",stale_ticks=" << total.staleTicks
                  << ",forced_updates=" << total.forcedTicks << ",puppets_stale=" << total.puppetsStale
                  << ",low_lod_episodes=" << total.episodes << ",worst_id="
                  << (total.worstPacked ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(total.worstPacked)) : std::string("none"))
                  << ",worst_stale_ticks=" << total.worstStale << ",script_pose=" << (g_poseV2LodConfig.scriptPose ? 1 : 0)
                  << ",script_pose_ticks=" << total.scriptTicks << ",puppets_script_pose=" << total.puppetsScript << '\n';
    }

    void DeletePoseV2Puppet(replay::playback::Puppet& puppet, const char* reason)
    {
        // The hat fix of this puppet into the totals.
        if (puppet.hatFix.armed || puppet.hatFix.fixed || puppet.hatFix.learned)
        {
            ++g_poseV2HatTotals.puppets;
            g_poseV2HatTotals.fixed += puppet.hatFix.fixed;
            g_poseV2HatTotals.learned += puppet.hatFix.learned;
            g_poseV2HatTotals.handled += puppet.hatFix.handled;
            g_poseV2HatTotals.kept += puppet.hatFix.kept;
            if (puppet.hatFix.fixed && puppet.hatFix.lastSource == replay::hatchain::FitSource::Default)
                ++g_poseV2HatTotals.defaults;
            if (puppet.hatFix.fixed)
                Ctx().log << "POSE_V2_HAT_FIX,id=" << replay::entity::IdText(puppet.packet.id) << ",model=0x" << std::hex
                          << puppet.packet.modelHash << std::dec << ",fixed=" << puppet.hatFix.fixed
                          << ",learned=" << puppet.hatFix.learned << ",handled=" << puppet.hatFix.handled
                          << ",kept_objects=" << puppet.hatFix.kept
                          << ",last_source=" << replay::hatchain::FitSourceName(puppet.hatFix.lastSource)
                          << ",last_fit=" << puppet.hatFix.lastFit << '\n';
        }
        puppet.hatFix = replay::hatchain::PuppetFix{};
        puppet.hatLayout = 0;
        // The body's ticks without a pose around skeleton changes into the
        // playback's totals.
        g_poseV2Switch.holeTicks += puppet.switchGap.holeTicks;
        if (puppet.switchGap.worstHole > g_poseV2Switch.worstHole)
            g_poseV2Switch.worstHole = puppet.switchGap.worstHole;
        puppet.switchGap = replay::segswitch::Gap{};
        std::ostream& log = Ctx().log;
        // The object's drawn check and still rule (still_pose_core.h), one line
        // per object that had either.
        if (puppet.stillSkip || puppet.drawn.checks)
            log << "POSE_V2_OBJECT_DRAWN_SUMMARY,id=" << replay::entity::IdText(puppet.packet.id) << ",model=0x"
                << std::hex << puppet.packet.modelHash << std::dec << ",still=" << (puppet.stillSkip ? 1 : 0)
                << ",armed=" << (puppet.overrideSlot >= 0 ? 1 : 0) << ','
                << replay::stillpose::FormatTotals(puppet.drawn) << '\n';
        log << "POSE_V2_PLAYBACK_DELETE"
            << ",id=" << replay::entity::IdText(puppet.packet.id)
            << ",reason=" << reason
            << ",armed=" << (puppet.overrideSlot >= 0 ? 1 : 0);
        if (puppet.overrideSlot >= 0)
        {
            const replay::pose::capture::OverrideStats stats =
                replay::pose::capture::OverrideSnapshot(puppet.overrideSlot);
            log << ",publishes=" << stats.publishes
                << ",applies=" << stats.applies
                << ",alt_applies=" << stats.altApplies
                << ",bone_mismatch=" << stats.boneCountMismatch
                << ",write_faults=" << stats.writeFaults
                << ",script_writes=" << stats.scriptWrites
                << ",script_survived=" << stats.scriptSurvived
                << ",script_overwritten=" << stats.scriptOverwritten
                << ",late_writes=" << stats.lateWrites
                << ",prerender_writes=" << stats.preRenderWrites
                << ",weapon_pass_writes=" << stats.weaponPassWrites
                << ",skipped_original=" << stats.skippedOriginal
                << ",write_checks=" << stats.writeChecks
                << ",write_changed=" << stats.writeChanged;
        }
        if (PoseV2LodBody(puppet) && puppet.lod.clone)
            log << ",lod_stale_ticks=" << puppet.lod.staleTicks << ",lod_forced=" << puppet.lod.forcedTicks
                << ",lod_episodes=" << puppet.lod.episodes;
        ReleasePoseV2Override(puppet);
        // The audio LOD override back to 4 (none) before the ped goes, in case
        // the entity outlives the puppet; "already" when it never had one.
        if (puppet.clone && (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                puppet.packet.kind == replay::entity::EntityKind::Horse) &&
            replay::entities::Exists(puppet.clone) && replay::entities::IsPed(puppet.clone))
        {
            std::string lodStatus;
            replay::audio::SetPuppetAudioLodOverride(replay::entities::AddressOf(puppet.clone), false, lodStatus);
            if (lodStatus != "already")
                log << ",audio_lod=" << lodStatus;
        }
        log << '\n';
        log.flush();
        UndoPoseV2Lod(puppet); // before the entity goes
        if (puppet.clone)
        {
            replay::vegetation::Remove(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)));
            replay::collision::Forget(replay::entities::AddressOf(puppet.clone));
        }
        if (puppet.clone)
            Ctx().recorder.ignored.push_back({puppet.clone, GetTickCount64() + 2000});
        if (puppet.packet.kind == replay::entity::EntityKind::Vehicle)
            replay::pose::playback::DeleteVehiclePuppet(puppet.clone);
        else if (puppet.packet.kind == replay::entity::EntityKind::TrainCar)
            replay::trains::DeletePuppet(puppet, reason); // a vehicle or an object
        else if (puppet.packet.kind == replay::entity::EntityKind::Weapon ||
            puppet.packet.kind == replay::entity::EntityKind::Prop)
        {
            // A wheel object a vehicle puppet gave goes back free at the end of
            // the puppet's span (the next puppet of the object, the held wheel
            // after the fallen one, takes it); the playback's stop deletes it.
            if (std::strcmp(reason, "lifecycle") == 0 && replay::objects::ReturnWheelObject(puppet.clone))
                puppet.clone = 0;
            else
                replay::pose::playback::DeleteObjectPuppet(puppet.clone);
        }
        else
            replay::pose::playback::DeleteClone(puppet.clone);
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


    bool PoseV2Body(const replay::playback::Puppet& puppet)
    {
        return puppet.packet.kind == replay::entity::EntityKind::Ped || puppet.packet.kind == replay::entity::EntityKind::Horse;
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

    // Arms the override on the puppet's current skeleton and writes the
    // pose at the playhead at once, from the script thread, so the skeleton
    // never renders a frame without it (the finalizer keeps it afterwards).
    // A skeleton with another segment's bones (the engine has not rebuilt
    // it yet, or already has) takes that segment's pose meanwhile.
    bool PoseV2KnockPending(replay::entity::ReplayId owner);

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
                << ",multiplier=" << (g_poseV2LodConfig.multiplier ? PED::_GET_PED_LOD_MULTIPLIER(puppet.clone) : 0.0f)
                << ",prerender=" << (g_poseV2LodConfig.prerender && puppet.lod.clone == puppet.clone ? 1 : 0)
                << ",force_update=" << (g_poseV2LodConfig.forceUpdate ? 1 : 0)
                << ",stale_ticks=" << puppet.lod.staleTicks << ",forced=" << puppet.lod.forcedTicks
                << ",playhead_ms=" << timeMs << '\n';
        log.flush();
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

    std::vector<replay::norecord::Alternate> PoseV2Alternates(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        return replay::norecord::Alternates(replay::FindAppearanceTrack(Ctx().clip.appearances, puppet.packet.id.Packed()),
            Ctx().clip.segments.Segments(), puppet.packet.id, PoseV2SegmentAt(puppet.packet.id, timeMs), puppet.packet.appearance,
            timeMs, replay::AppearancePrint(PoseV2Wear(puppet)), replay::norecord::kMaxAlternates);
    }

    // A body armed exact: the no-record steps for its record end (an
    // alternate that fit stays on).
    void NotePoseV2ExactArm(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        if (puppet.noRecord.active)
        {
            const bool alternate = puppet.noRecord.wearingAlternate;
            ++(alternate ? g_noRecord.alternateFits : g_noRecord.exactAfter);
            if (g_noRecordLogs.Take(96))
                Ctx().log << "POSE_V2_NO_RECORD,id=" << replay::entity::IdText(puppet.packet.id)
                          << ",action=" << (alternate ? "alternate_fits" : "exact_after_retry")
                          << ",key=" << (alternate ? puppet.noRecord.AlternateKey() : puppet.appearanceKey)
                          << ",retries=" << puppet.noRecord.retries << ",alternates_tried=" << puppet.noRecord.alternatesTried
                          << ",bones=" << puppet.armedBones << ",record=0x" << std::hex << puppet.armedRecordId << std::dec
                          << ",playhead_ms=" << timeMs << '\n';
        }
        puppet.noRecord.Finish();
        puppet.lostToChange = false;
        puppet.noRecordMapRefused = false;
    }

    // Once a tick while a body is mapped for want of a record: every
    // kStepMs the recorded layers again (until an attempt answers as the
    // one before), then the other recorded appearances of the segment, then
    // the recorded layers back. An attempt that builds a skeleton a record
    // has is armed exact by RefreshPoseV2Override (the mapped arm released).
    void StepPoseV2NoRecord(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < puppet.noRecord.dueAt || PoseV2KnockPending(puppet.packet.id))
            return;
        const std::uint32_t present = PoseV2LayerDiff(puppet, PoseV2Wear(puppet),
            puppet.noRecord.haveLast || puppet.noRecord.wearingAlternate ? "after_step" : nullptr, puppet.armedBones, timeMs);
        const bool wasSame = puppet.noRecord.sameAnswer;
        const replay::norecord::Step step = puppet.noRecord.Next(puppet.armedBones, present, now, g_noRecordAltAppearance);
        if (puppet.noRecord.sameAnswer && !wasSame)
            ++g_noRecord.sameAnswer;
        if (step == replay::norecord::Step::Wait)
            return;
        const replay::entity::Appearance* apply = nullptr;
        std::uint32_t key = puppet.appearanceKey;
        const replay::WantedAppearance wanted = PoseV2AppearanceAt(puppet, timeMs);
        if (step == replay::norecord::Step::Retry || step == replay::norecord::Step::Restore)
        {
            apply = wanted.appearance;
            key = wanted.key;
            puppet.appearanceKey = wanted.key; // an alternate leaves it (the playhead's change still re-applies)
            ++(step == replay::norecord::Step::Retry ? g_noRecord.retries : g_noRecord.restores);
        }
        else if (step == replay::norecord::Step::Alternate)
        {
            key = puppet.noRecord.AlternateKey();
            apply = replay::norecord::AppearanceForKey(
                replay::FindAppearanceTrack(Ctx().clip.appearances, puppet.packet.id.Packed()),
                PoseV2SegmentAt(puppet.packet.id, timeMs), puppet.packet.appearance, key);
            ++g_noRecord.alternates;
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
                      << ",alternates=" << puppet.noRecord.alternates << ",alternates_tried=" << puppet.noRecord.alternatesTried
                      << ",alt_on=" << (g_noRecordAltAppearance ? 1 : 0) << ",clone_bones=" << puppet.armedBones
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
            replay::norecord::MapNow(g_noRecordMapNow, puppet.lostToChange, puppet.noRecordMapRefused))
        {
            std::string mapError;
            const PoseV2MapArm result = ArmPoseV2Mapped(puppet, resolved, slot, timeMs, mapError);
            if (result == PoseV2MapArm::Pending)
                return; // the detour reads the puppet's table (a frame or two)
            if (result == PoseV2MapArm::Armed)
            {
                ++g_noRecord.mapNow;
                const bool began = puppet.noRecord.Begin(puppet.recordId, puppet.boneCount, PoseV2Alternates(puppet, timeMs),
                    GetTickCount64());
                PoseV2LayerDiff(puppet, PoseV2Wear(puppet), began ? "map_now" : nullptr, resolved.boneCount, timeMs);
                if (g_noRecordLogs.Take(96))
                    Ctx().log << "POSE_V2_NO_RECORD,id=" << replay::entity::IdText(puppet.packet.id) << ",action=map_now"
                              << ",began=" << (began ? 1 : 0) << ",clone_bones=" << resolved.boneCount
                              << ",track_bones=" << puppet.boneCount << ",alternates=" << puppet.noRecord.alternates
                              << ",record=0x" << std::hex << puppet.recordId << std::dec << ",playhead_ms=" << timeMs << '\n';
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

    // The playback's hat fixes (after the puppets' deletes put theirs in).
    void LogPoseV2HatSummary()
    {
        const PoseV2HatTotals& t = g_poseV2HatTotals;
        if (!t.puppets)
            return;
        Ctx().log << "POSE_V2_HAT_FIX_SUMMARY,on=" << (g_poseV2HatFix ? 1 : 0) << ",puppets=" << t.puppets << ",fixed=" << t.fixed
                  << ",learned=" << t.learned << ",handled=" << t.handled << ",kept_objects=" << t.kept
                  << ",puppets_on_default=" << t.defaults << ",models_fitted=" << g_poseV2HatFitted.size() << '\n';
    }

    void LogPoseV2SwitchSummary()
    {
        LogPoseV2HatSummary();
        const PoseV2SwitchTotals& t = g_poseV2Switch;
        if (!t.switches && !t.kept && !t.held)
            return;
        Ctx().log << "POSE_V2_SEGMENT_SWITCH_SUMMARY,in_tick=" << (g_poseV2SwitchInTick ? 1 : 0) << ",switches=" << t.switches
                  << ",rearmed_in_tick=" << t.rearmedInTick << ",released=" << t.released << ",rearm_failed=" << t.rearmFailed
                  << ",segment_kept_arm=" << t.kept << ",segment_held_arm=" << t.held << ",hold_timeouts=" << t.holdTimeouts
                  << ",hole_ticks=" << t.holeTicks << ",worst_hole_ticks=" << t.worstHole
                  << ",prefix_holds=" << t.prefixHolds << ",prefix_verified=" << t.prefixVerified
                  << ",prefix_released=" << t.prefixReleased << ",prefix_ended=" << t.prefixEnded << '\n';
        // The no-record fallback's totals (no_record_fallback.h).
        const NoRecordTotals& n = g_noRecord;
        if (n.mapNow || n.mapRefused || n.diffs)
            Ctx().log << "POSE_V2_NO_RECORD_SUMMARY,map_now_on=" << (g_noRecordMapNow ? 1 : 0)
                      << ",alt_on=" << (g_noRecordAltAppearance ? 1 : 0) << ",map_now=" << n.mapNow
                      << ",map_refused=" << n.mapRefused << ",retries=" << n.retries << ",same_answer=" << n.sameAnswer
                      << ",alternates=" << n.alternates << ",alternate_fits=" << n.alternateFits << ",restores=" << n.restores
                      << ",exact_after_retry=" << n.exactAfter << ",diff_lines=" << n.diffs
                      << ",categories_known=" << g_noRecordCategories.Size() << '\n';
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
            if ((skeletonChanged || segmentArrived) && !puppet.mapped && g_poseV2SwitchInTick)
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
        if (std::find(g_poseV2RequestedModels.begin(), g_poseV2RequestedModels.end(),
                packet.modelHash) == g_poseV2RequestedModels.end())
        {
            g_poseV2RequestedModels.push_back(packet.modelHash);
        }
        replay::pose::playback::RequestModel(packet.modelHash);
        Ctx().puppets.list.push_back(puppet);
    }

    const replay::VehicleLook* PoseV2VehicleLook(replay::entity::ReplayId id)
    {
        for (const replay::VehicleLook& look : Ctx().clip.vehicleLooks)
        {
            if (look.id == id)
                return &look;
        }
        return nullptr;
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
                if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                {
                    puppet.state = replay::playback::PuppetState::Failed;
                    LogPoseV2Puppet(puppet, "model_timeout", timeMs);
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

    // The pose segment at the playhead decides the puppet's record, bone
    // count and appearance. On a change (either direction, so rewinding
    // restores the earlier look) the override is released, the segment's
    // layers are applied and arming waits for the puppet's skeleton to take
    // the segment's bone count.
    // A recorded dropped hat of `owner` still to be knocked off its puppet
    // by the engine (bounded: the knock has a deadline).
    constexpr ULONGLONG kPoseV2KnockPendingMaxMs = 2000;
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
        const bool keepArm = g_poseV2SwitchInTick &&
            replay::segswitch::SwitchKeepsArm(armedNow, puppet.mapped, puppet.armedRecordId, puppet.armedBones, recordId, newBones);
        const bool holdArm = g_poseV2SwitchInTick && !keepArm && replay::segswitch::SwitchHoldsArm(armedNow, puppet.mapped);
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

    // Applies the recorded attachments at the playhead between ready
    // puppets, and releases the ones that no longer hold.
    constexpr std::uint32_t kPoseV2SeatFollowFrames = 10;
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

    void PlayPoseV2Ropes(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.ropes");
        if (Ctx().clip.ropes.Records().empty())
            return;
        const PoseV2RopePuppetIndex index;
        Ctx().clip.ropes.Play(index, timeMs, Ctx().log);
    }

    struct Hooks final : replay::PlaybackHooks
    {
        void LogPuppet(const replay::playback::Puppet& puppet, const char* status, std::uint32_t timeMs) override
        {
            LogPoseV2Puppet(puppet, status, timeMs);
        }
        void RefreshOverride(replay::playback::Puppet& puppet, std::uint32_t timeMs) override
        {
            RefreshPoseV2Override(puppet, timeMs);
        }
        void ReleaseOverride(replay::playback::Puppet& puppet) override { ReleasePoseV2Override(puppet); }
        bool NormalPlay() const override { return PoseV2NormalPlay(); }
    } g_hooks;

    // A shop dress of a shown puppet, checked once the engine rebuilt it (ready
    // to render, at most 2 s after the dress).
    void StepPoseV2ShopCheck(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        if (!puppet.shopCheckAt || now < puppet.shopCheckAt || !puppet.clone)
            return;
        if (!replay::pose::playback::IsReadyToRender(puppet.clone) &&
            now - puppet.shopCheckSince < replay::playback::kDressTimeoutMs)
            return;
        puppet.shopCheckAt = 0;
        const replay::entity::appearance::Match match =
            replay::entity::appearance::Compare(puppet.clone, PoseV2Wear(puppet));
        if (!CheckPoseV2ShopDress(puppet, timeMs, "shown", match))
            return;
        // Dressed again by layers: the skeleton may change, as after an
        // appearance change (ApplyPoseV2Appearance).
        puppet.layerRetries = 0;
        puppet.nextLayerRetryAt = now + 600;
        puppet.noRecord.Reset();
        puppet.noRecordMapRefused = false;
        if (puppet.overrideSlot >= 0 && !puppet.mapped)
            ScheduleSignatureRecheck(puppet);
    }

    // The recorded appearance changes at the playhead on a ped or horse puppet
    // (appearance_track.h), in both directions: a scrub back before a change
    // puts the earlier layers back, a seek past one wears it. A pose segment
    // switch applies its own (SwitchPoseV2Segment, which also picks the change
    // in force); this acts only when the change at the playhead is not the one
    // worn. Layers are applied only when they differ from the puppet's:
    // applying them rebuilds the skeleton, which RefreshPoseV2Override re-arms,
    // and a bone count no record has retries the layers through
    // ArmPoseV2Puppet. A dropped hat still to be knocked off waits, like the
    // segment switch.
    void ApplyPoseV2Appearance(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        // The metaped look follows the playhead (a change, a seek back before
        // one).
        if (!PoseV2KnockPending(puppet.packet.id))
            ApplyPoseV2Metaped(puppet, timeMs, false);
        if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                puppet.packet.kind != replay::entity::EntityKind::Horse) ||
            Ctx().clip.appearances.empty())
            return;
        const replay::WantedAppearance wanted = PoseV2AppearanceAt(puppet, timeMs);
        if (wanted.key == puppet.appearanceKey || PoseV2KnockPending(puppet.packet.id))
            return;
        SetPoseV2Wear(puppet, *wanted.appearance);
        const bool layersApplied =
            !replay::entity::appearance::Compare(puppet.clone, *wanted.appearance).Exact();
        if (layersApplied)
        {
            LearnPoseV2Categories(puppet, *wanted.appearance);
            DressPoseV2Body(puppet, *wanted.appearance, timeMs, true); // shop items or layers
            ApplyPoseV2Metaped(puppet, timeMs, true);
            puppet.layerRetries = 0;
            puppet.nextLayerRetryAt = GetTickCount64() + 600;
            puppet.noRecord.Reset();
            puppet.noRecordMapRefused = false;
            if (puppet.overrideSlot >= 0 && !puppet.mapped)
                ScheduleSignatureRecheck(puppet);
        }
        const std::uint32_t previousKey = puppet.appearanceKey;
        puppet.appearanceKey = wanted.key;
        if (g_poseV2AppearancePlayLogs >= 64)
            return;
        ++g_poseV2AppearancePlayLogs;
        Ctx().log << "POSE_V2_APPEARANCE_CHANGE,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
            << ",key=" << wanted.key << ",previous_key=" << previousKey
            << ",layers=" << wanted.appearance->assets.size() << ",layers_applied=" << (layersApplied ? 1 : 0)
            << ",playhead_ms=" << timeMs << '\n';
    }

    // The recorded charred state at the playhead on a ped or horse puppet,
    // written when it changes (both directions: a scrub back before the change
    // takes the black off). The fire replayed on the puppet can make the engine
    // set the bit itself: once a second the bit is read back and written again
    // when it differs (engine_set=1).
    void ApplyPoseV2Scorched(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        // Vehicle puppets too (a burnt wagon).
        if (puppet.packet.kind != replay::entity::EntityKind::Ped && puppet.packet.kind != replay::entity::EntityKind::Horse &&
            puppet.packet.kind != replay::entity::EntityKind::Vehicle)
            return;
        const auto track = std::find_if(Ctx().clip.scorched.begin(), Ctx().clip.scorched.end(),
            [&](const replay::ScorchTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
        const bool wanted = track != Ctx().clip.scorched.end() && replay::ScorchedAt(*track, timeMs);
        bool engineSet = false;
        const ULONGLONG now = GetTickCount64();
        if (puppet.scorchedApplied == (wanted ? 1 : 0))
        {
            if (now < puppet.scorchCheckAt)
                return;
            puppet.scorchCheckAt = now + 1000;
            const bool actual = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(puppet.clone)) & 1u) != 0;
            if (actual == wanted)
                return;
            engineSet = true;
        }
        ENTITY::SET_ENTITY_RENDER_SCORCHED(puppet.clone, wanted ? TRUE : FALSE);
        const bool first = puppet.scorchedApplied < 0;
        puppet.scorchedApplied = wanted ? 1 : 0;
        puppet.scorchCheckAt = now + 1000;
        // A new puppet without the bit needs no line.
        if ((first && !wanted) || g_poseV2ScorchPlayLogs >= 64)
            return;
        ++g_poseV2ScorchPlayLogs;
        Ctx().log << "POSE_V2_SCORCHED,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
            << ",scorched=" << (wanted ? 1 : 0) << ",engine_set=" << (engineSet ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
    }

    // A vehicle puppet's paint and livery compared with the recorded ones after
    // it has rendered (500 ms and 2 s after the create) and set again when the
    // engine changed them, the variation data reapplied until it is verified.
    // Evidence in POSE_V2_VEHICLE_LOOK,phase=verify.
    void VerifyPoseV2VehicleLook(std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle || !puppet.lookCheckAt ||
                now < puppet.lookCheckAt)
                continue;
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                puppet.clone != puppet.lookClone || !replay::entities::Exists(puppet.clone))
            {
                puppet.lookCheckAt = 0;
                continue;
            }
            const int tint = VEHICLE::_GET_VEHICLE_TINT(puppet.clone);
            const int livery = VEHICLE::_GET_VEHICLE_LIVERY(puppet.clone);
            const bool tintOff = puppet.lookTint >= 0 && tint != puppet.lookTint;
            const bool liveryOff = puppet.lookLivery >= 0 && livery != puppet.lookLivery;
            if (tintOff)
                VEHICLE::_SET_VEHICLE_TINT(puppet.clone, puppet.lookTint);
            if (liveryOff)
                VEHICLE::_SET_VEHICLE_LIVERY(puppet.clone, puppet.lookLivery);
            ++puppet.lookChecks;
            puppet.lookCheckAt = puppet.lookChecks < 2 ? now + 1500 : 0;
            if (g_poseV2LookVerifyLogs < 64 && (tintOff || liveryOff || puppet.lookChecks == 1))
            {
                ++g_poseV2LookVerifyLogs;
                Ctx().log << "POSE_V2_VEHICLE_LOOK,phase=verify,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",check=" << puppet.lookChecks
                    << ",tint=" << puppet.lookTint << ",tint_seen=" << tint
                    << ",livery=" << puppet.lookLivery << ",livery_seen=" << livery
                    << ",reapplied=" << ((tintOff || liveryOff) ? 1 : 0)
                    << ",tint_after=" << VEHICLE::_GET_VEHICLE_TINT(puppet.clone)
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The recorded broken windows at the playhead on a vehicle puppet. A window
    // broken since is removed (REMOVE_VEHICLE_WINDOW); back before a break the
    // puppet is rebuilt whole through the Loading path (no native puts the
    // glass back, as the frag rewind does) and the windows up to the playhead
    // come again.
    void PlayPoseV2VehicleWindows(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_windows");
        if (Ctx().clip.vehicleWindows.empty())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            const auto track = std::find_if(Ctx().clip.vehicleWindows.begin(), Ctx().clip.vehicleWindows.end(),
                [&](const replay::WindowTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.vehicleWindows.end())
                continue;
            if (puppet.windowsClone != puppet.clone)
            {
                puppet.windowsClone = puppet.clone;
                puppet.windowsApplied = 0;
            }
            const std::uint8_t wanted = replay::BrokenWindowsAt(*track, timeMs);
            if (wanted == puppet.windowsApplied)
                continue;
            if (puppet.windowsApplied & ~wanted)
            {
                Ctx().log << "POSE_V2_VEHICLE_WINDOWS,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",applied=0x" << std::hex << static_cast<unsigned>(puppet.windowsApplied)
                    << ",wanted=0x" << static_cast<unsigned>(wanted) << std::dec << ",playhead_ms=" << timeMs << '\n';
                ReleaseVehicleForRebuild(puppet);
                puppet.clone = 0;
                puppet.windowsClone = 0;
                puppet.windowsApplied = 0;
                puppet.scorchedApplied = -1;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                continue;
            }
            const std::uint8_t added = static_cast<std::uint8_t>(wanted & ~puppet.windowsApplied);
            for (int window = 0; window < replay::kVehicleWindows; ++window)
            {
                if (added & (1u << window))
                    VEHICLE::REMOVE_VEHICLE_WINDOW(puppet.clone, window);
            }
            puppet.windowsApplied = wanted;
            if (g_poseV2WindowPlayLogs < 64)
            {
                ++g_poseV2WindowPlayLogs;
                std::uint8_t intact = 0;
                for (int window = 0; window < replay::kVehicleWindows; ++window)
                {
                    if (VEHICLE::IS_VEHICLE_WINDOW_INTACT(puppet.clone, window))
                        intact |= static_cast<std::uint8_t>(1u << window);
                }
                Ctx().log << "POSE_V2_VEHICLE_WINDOWS,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",broken=0x" << std::hex << static_cast<unsigned>(wanted)
                    << ",intact_after=0x" << static_cast<unsigned>(intact) << std::dec << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The recorded door ratios at the playhead on every vehicle puppet (the
    // train cars in replay_trains), driven and read back
    // (vehicle_doors_engine.h).
    void PlayPoseV2VehicleDoors(std::uint32_t timeMs)
    {
        if (Ctx().clip.vehicleDoors.empty())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone)
                continue;
            if (const replay::vehdoors::DoorTrack* track =
                    replay::vehdoors::TrackOf(Ctx().clip.vehicleDoors, puppet.packet.id.Packed()))
                replay::vehdoors::Apply(puppet.clone, *track, timeMs, Ctx().log);
        }
    }

    // The recorded lost wheels and doors at the playhead on a vehicle puppet
    // (VehiclePartTrack), with the natives the game's own scripts use on
    // wagons: _BREAK_OFF_VEHICLE_WHEEL (bounds-checked by the engine: an index
    // the model lacks does nothing) and SET_VEHICLE_DOOR_BROKEN with deletion.
    // The wheel piece the native returns is deleted (the recorded piece is not
    // part of the clip). Back before a loss the puppet is rebuilt whole through
    // the Loading path, as the windows do. The object-only fragment native is
    // never used on a vehicle.
    void PlayPoseV2VehicleParts(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_parts");
        if (Ctx().clip.vehicleParts.empty())
            return;
        bool pieceStatesRead = false;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone) ||
                !ENTITY::IS_ENTITY_A_VEHICLE(puppet.clone))
                continue;
            const auto track = std::find_if(Ctx().clip.vehicleParts.begin(), Ctx().clip.vehicleParts.end(),
                [&](const replay::VehiclePartTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.vehicleParts.end())
                continue;
            if (puppet.partsClone != puppet.clone)
            {
                puppet.partsClone = puppet.clone;
                puppet.partsApplied = 0;
            }
            // The wheels the clip's wheel pieces took are off too (a piece may
            // be born a sample before the 4 Hz part track; vehicle_pieces.h),
            // and the wheel models of the puppet are known to the object
            // puppets.
            replay::objects::NoteVehiclePuppetWheels(puppet.clone);
            std::uint16_t pieceWheels = 0;
            if (replay::objects::ClipHasWheelPieces())
            {
                if (!pieceStatesRead)
                {
                    replay::objects::VehiclePieceStates(g_poseV2PieceStates);
                    pieceStatesRead = true;
                }
                pieceWheels = replay::vehicle_pieces::WheelPieceBits(g_poseV2PieceStates, puppet.packet.id, timeMs);
            }
            // A recorded wheel child attached at the playhead keeps its wheel on
            // the puppet (vehicle_pieces.h wheel children).
            const std::uint16_t recorded = replay::BrokenPartsAt(*track, timeMs);
            const std::uint16_t keptWheels = replay::objects::KeptVehicleWheels(puppet, timeMs);
            // A wheel whose record left the vehicle came off as it was
            // (LeftVehicleWheels): never through the part native.
            const std::uint16_t leftWheels = replay::objects::LeftVehicleWheels(puppet, timeMs);
            const std::uint16_t wanted = replay::vehicle_pieces::PartsWanted(recorded, pieceWheels, keptWheels, leftWheels);
            std::uint16_t& keptLogged = g_poseV2PartKeptLogged[puppet.packet.id.Packed()];
            if ((recorded & keptWheels) && keptLogged != (recorded & keptWheels))
            {
                keptLogged = static_cast<std::uint16_t>(recorded & keptWheels);
                Ctx().log << "POSE_V2_VEHICLE_PARTS,phase=kept,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",recorded_wheels_off=0x" << std::hex << (recorded & ((1u << replay::kVehiclePartWheels) - 1))
                    << ",wheels_kept=0x" << (recorded & keptWheels) << std::dec << ",playhead_ms=" << timeMs << '\n';
            }
            // Back before a piece broke off one of its wheels: built whole again.
            const bool piecesAhead = replay::objects::VehicleWheelPiecesAhead(puppet.packet.id, timeMs);
            if (wanted == puppet.partsApplied && !piecesAhead)
                continue;
            if ((puppet.partsApplied & ~wanted) || piecesAhead)
            {
                Ctx().log << "POSE_V2_VEHICLE_PARTS,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wheel_pieces_ahead=" << (piecesAhead ? 1 : 0)
                    << ",applied=0x" << std::hex << puppet.partsApplied << ",wanted=0x" << wanted << std::dec
                    << ",playhead_ms=" << timeMs << '\n';
                ReleaseVehicleForRebuild(puppet);
                puppet.clone = 0;
                puppet.partsClone = 0;
                puppet.partsApplied = 0;
                puppet.windowsClone = 0;
                puppet.windowsApplied = 0;
                puppet.scorchedApplied = -1;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                continue;
            }
            // A recorded piece of this vehicle due now and not made yet breaks
            // its wheel or door off first.
            replay::objects::VehiclePieceStates(g_poseV2PieceStates);
            if (replay::vehicle_pieces::DeferPartNatives(g_poseV2PieceStates, puppet.packet.id, timeMs))
                continue;
            const std::uint16_t added = static_cast<std::uint16_t>(wanted & ~puppet.partsApplied);
            int pieces = 0;
            int kept = 0;
            for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
            {
                if (!(added & (1u << wheel)))
                    continue;
                Entity piece = VEHICLE::_BREAK_OFF_VEHICLE_WHEEL(puppet.clone, wheel);
                if (piece && piece != puppet.clone && replay::entities::Exists(piece) && ENTITY::IS_ENTITY_AN_OBJECT(piece))
                {
                    // Kept (hidden) when the clip has a use for it: its wheel
                    // piece, or an object puppet of its model (the carried
                    // wheel).
                    if (replay::objects::KeepVehicleWheel(puppet.packet.id, wheel, piece, timeMs))
                    {
                        ++kept;
                        continue;
                    }
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
                    ENTITY::DELETE_ENTITY(&piece);
                    ++pieces;
                }
            }
            for (int door = 0; door < replay::kVehiclePartDoors; ++door)
            {
                if (added & (1u << (replay::kVehiclePartDoorShift + door)))
                    VEHICLE::SET_VEHICLE_DOOR_BROKEN(puppet.clone, door, TRUE);
            }
            puppet.partsApplied = wanted;
            if (g_poseV2PartPlayLogs < 64)
            {
                ++g_poseV2PartPlayLogs;
                unsigned doorsAfter = 0;
                for (int door = 0; door < replay::kVehiclePartDoors; ++door)
                {
                    if (VEHICLE::_IS_VEHICLE_DOOR_BROKEN(puppet.clone, door))
                        doorsAfter |= 1u << door;
                }
                Ctx().log << "POSE_V2_VEHICLE_PARTS,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wheels_off=0x" << std::hex << (wanted & ((1u << replay::kVehiclePartWheels) - 1))
                    << ",doors_off=0x" << (static_cast<unsigned>(wanted) >> replay::kVehiclePartDoorShift)
                    << ",doors_off_after=0x" << doorsAfter << std::dec
                    << ",any_wheel_off_after=" << (VEHICLE::_ARE_ANY_VEHICLE_WHEELS_DESTROYED(puppet.clone) ? 1 : 0)
                    << ",pieces_deleted=" << pieces << ",pieces_kept=" << kept << ",piece_wheels=0x" << std::hex << pieceWheels
                    << std::dec << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The fragment groups a recorded vehicle lost (boards, canvas, cargo, the
    // wreck an explosion leaves: FragBreak events of the vehicle,
    // RecordVehicleFragBreak) taken off its puppet up to the playhead through
    // the engine's DeleteAboveGroup (replay::frag::DeleteVehicleGroup:
    // signature, fragInst owned by the vehicle, group in range, never the
    // root). Wheel and door groups are left to PlayPoseV2VehicleParts' natives.
    // Back before a break the puppet is rebuilt whole through the Loading path,
    // as the windows and parts do. BREAK_OBJECT_FRAGMENT_CHILD is never used on
    // a vehicle (its getter is object-only).
    void PlayPoseV2VehicleFrags(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_frags");
        if (Ctx().clip.fragBreaks.empty() || !replay::frag::VehicleGroupsSupported())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone) ||
                !ENTITY::IS_ENTITY_A_VEHICLE(puppet.clone))
                continue;
            if (puppet.fragClone != puppet.clone)
            {
                puppet.fragClone = puppet.clone;
                puppet.fragApplied = 0;
            }
            std::uint32_t due = 0;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
                if (e.id == puppet.packet.id && e.timeMs <= timeMs)
                    ++due;
            if (due == puppet.fragApplied)
                continue;
            if (due < puppet.fragApplied)
            {
                ++g_poseV2VehicleFragRebuilds;
                Ctx().log << "POSE_V2_VEHICLE_FRAG,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",applied=" << puppet.fragApplied << ",due=" << due << ",playhead_ms=" << timeMs << '\n';
                ReleaseVehicleForRebuild(puppet);
                puppet.clone = 0;
                puppet.fragClone = 0;
                puppet.fragApplied = 0;
                puppet.partsClone = 0;
                puppet.partsApplied = 0;
                puppet.windowsClone = 0;
                puppet.windowsApplied = 0;
                puppet.scorchedApplied = -1;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                continue;
            }
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
            // The groups of the vehicle's recorded pieces are theirs to break
            // off (vehicle_pieces.h).
            replay::objects::VehiclePieceStates(g_poseV2PieceStates);
            std::uint16_t reserved[64];
            const int reservedCount = replay::vehicle_pieces::ReservedRoots(
                g_poseV2PieceStates, puppet.packet.id, timeMs, reserved, 64);
            // A recorded piece's group comes off as that piece at its own break,
            // before the groups above it go in the same tick (deleting a parent
            // takes the children with it; the recorder found the piece's object
            // a few ticks after the break).
            std::uint32_t pieceIndex = 0;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
            {
                if (!(e.id == puppet.packet.id) || e.timeMs > timeMs || pieceIndex++ < puppet.fragApplied ||
                    !replay::vehicle_pieces::IsUnmadePieceGroup(g_poseV2PieceStates, puppet.packet.id, e.group))
                    continue;
                std::uint64_t entity = 0;
                if (replay::frag::BreakOffVehicleGroup(address, e.group, entity) == replay::frag::VehicleGroupResult::Deleted)
                    replay::objects::KeepVehicleFragPiece(puppet.packet.id, e.group, entity, timeMs);
            }
            std::uint32_t index = 0;
            std::uint32_t counts[replay::frag::kVehicleGroupResults] = {};
            int firstGroup = -1;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
            {
                if (!(e.id == puppet.packet.id) || e.timeMs > timeMs)
                    continue;
                if (index++ < puppet.fragApplied)
                    continue;
                const replay::frag::VehicleGroupResult result = replay::frag::DeleteVehicleGroup(address, e.group, reserved, reservedCount);
                const int slot = static_cast<int>(result);
                if (slot >= 0 && slot < replay::frag::kVehicleGroupResults)
                {
                    ++counts[slot];
                    ++g_poseV2VehicleFragResults[slot];
                }
                if (firstGroup < 0)
                    firstGroup = e.group;
                // A vehicle without a usable fragInst now will not have one
                // later in this tick either.
                if (result == replay::frag::VehicleGroupResult::NoInst || result == replay::frag::VehicleGroupResult::Fault ||
                    result == replay::frag::VehicleGroupResult::Unsupported)
                    break;
            }
            puppet.fragApplied = due;
            if (g_poseV2VehicleFragLogs < 64)
            {
                ++g_poseV2VehicleFragLogs;
                Ctx().log << "POSE_V2_VEHICLE_FRAG,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",events=" << due << ",first_group=" << firstGroup;
                for (int slot = 0; slot < replay::frag::kVehicleGroupResults; ++slot)
                    if (counts[slot])
                        Ctx().log << ',' << replay::frag::VehicleGroupResultText(static_cast<replay::frag::VehicleGroupResult>(slot))
                            << '=' << counts[slot];
                Ctx().log << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The burnt state of a vehicle puppet. The fire replayed on the puppet
    // darkens it by itself in normal play (the engine's burn level,
    // SamplePoseV2VehicleBurnt), and that level never goes back: a scrub back
    // to before the burn would leave the wagon black. The playhead where the
    // puppet was first seen on fire or charred is kept; back before it, when
    // the recorded track does not have the vehicle burnt there, the puppet is
    // rebuilt clean through the Loading path (no native lowers the burn level),
    // as the windows and parts do. Old clips without the track rebuild the same
    // way.
    void PlayPoseV2VehicleBurnt(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_burnt");
        constexpr std::uint32_t kNotSeen = 0xFFFFFFFFu;
        constexpr std::uint32_t kSlackMs = 250;
        const ULONGLONG now = GetTickCount64();
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            if (puppet.burntClone != puppet.clone)
            {
                puppet.burntClone = puppet.clone;
                puppet.burntSeenMs = kNotSeen;
                puppet.burntCheckAt = 0;
            }
            if (puppet.burntSeenMs == kNotSeen)
            {
                if (now < puppet.burntCheckAt)
                    continue;
                puppet.burntCheckAt = now + 500;
                const bool onFire = FIRE::IS_ENTITY_ON_FIRE(puppet.clone) != FALSE;
                const bool charred = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(puppet.clone)) & 1u) != 0;
                if (!onFire && !charred)
                    continue;
                puppet.burntSeenMs = timeMs;
                if (g_poseV2BurntPlayLogs < 64)
                {
                    ++g_poseV2BurntPlayLogs;
                    Ctx().log << "POSE_V2_VEHICLE_BURNT,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",on_fire=" << (onFire ? 1 : 0) << ",charred=" << (charred ? 1 : 0)
                        << ",playhead_ms=" << timeMs << '\n';
                }
                continue;
            }
            if (timeMs + kSlackMs >= puppet.burntSeenMs)
                continue;
            // A vehicle with a FREC track needs no rebuild: the black body is
            // the fire record (the vehicle branch of the fire renderer,
            // 0x127A0D8 -> 0x1290DCC, looks the record up the same way as for
            // peds) and PlayPoseV2FireChar removes it back before the recorded
            // fire.
            if (replay::burn::CharEnabled() && replay::FireCharKept(Ctx().clip.fireChars, puppet.packet.id.Packed()))
                continue;
            const auto track = std::find_if(Ctx().clip.vehicleBurnt.begin(), Ctx().clip.vehicleBurnt.end(),
                [&](const replay::ScorchTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track != Ctx().clip.vehicleBurnt.end() && replay::ScorchedAt(*track, timeMs))
                continue;
            Ctx().log << "POSE_V2_VEHICLE_BURNT,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                << ",seen_ms=" << puppet.burntSeenMs
                << ",recorded=" << (track != Ctx().clip.vehicleBurnt.end() ? 1 : 0)
                << ",playhead_ms=" << timeMs << '\n';
            ReleaseVehicleForRebuild(puppet);
            puppet.clone = 0;
            puppet.burntClone = 0;
            puppet.burntSeenMs = kNotSeen;
            puppet.partsClone = 0;
            puppet.partsApplied = 0;
            puppet.windowsClone = 0;
            puppet.windowsApplied = 0;
            puppet.scorchedApplied = -1;
            puppet.state = replay::playback::PuppetState::Loading;
            puppet.stateSince = now;
        }
    }

    // The recorded burn level at the playhead on every vehicle, ped and horse
    // puppet with a track (BurnLevelTrack), written straight into the puppet's
    // effect blocks (burn_level.h) whenever the puppet's own value differs.
    // Back before a burn it lowers the level, forward past one it darkens a
    // clean puppet (a rebuilt one included), and a ped or horse puppet,
    // invincible, gets the level the replayed fire cannot give it. The fire
    // replayed on a vehicle puppet writes the level too while it burns; the
    // track wins on the next frame (engine_changed). Also on the weapon object
    // and prop puppets (BurnLevelPlayable; ini ObjectBurnLevel): the cooked
    // meat, the campfire's logs. Their objects are the replay's own (created,
    // or taken from the engine during the playback and deleted at its end), so
    // nothing is given back.
    void PlayPoseV2BurnLevel(std::uint32_t timeMs)
    {
        if (Ctx().clip.burnLevels.empty() || !replay::burn::Enabled())
            return;
        constexpr float kSame = 0.001f;
        const bool objects = replay::objects::ObjectBurnLevel();
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            const replay::entity::EntityKind kind = puppet.packet.kind;
            const bool object = kind == replay::entity::EntityKind::Weapon || kind == replay::entity::EntityKind::Prop;
            if (!replay::BurnLevelPlayable(kind) || (object && !objects) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            const auto track = std::find_if(Ctx().clip.burnLevels.begin(), Ctx().clip.burnLevels.end(),
                [&](const replay::BurnLevelTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.burnLevels.end() || track->states.empty())
                continue;
            if (puppet.burnLevelClone != puppet.clone)
            {
                puppet.burnLevelClone = puppet.clone;
                puppet.burnLevelHas = false;
            }
            // A file's value never reaches the engine unbounded.
            const float level = replay::BurnLevelAt(*track, timeMs);
            const float wanted = level == level ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
            const void* address = replay::entities::AddressOf(puppet.clone);
            replay::burn::State state;
            if (!replay::burn::Read(address, state))
                continue;
            const bool fireSame = !state.fireBlock || std::fabs(state.level - wanted) < kSame;
            const bool pedSame = !state.pedBlock || std::fabs(state.pedLevel - wanted) < kSame;
            if (fireSame && pedSame)
                continue;
            const bool engineChanged = puppet.burnLevelHas && std::fabs(wanted - puppet.burnLevelWritten) < kSame;
            if (engineChanged)
                ++g_poseV2BurnLevelEngineChanged;
            const float before = state.fireBlock ? state.level : state.pedLevel;
            const int blocks = replay::burn::Write(address, wanted);
            puppet.burnLevelWritten = wanted;
            puppet.burnLevelHas = blocks > 0;
            ++g_poseV2BurnLevelWrites;
            if (g_poseV2BurnLevelPlayLogs < 64 && !engineChanged)
            {
                ++g_poseV2BurnLevelPlayLogs;
                Ctx().log << "POSE_V2_BURN_LEVEL,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",kind=" << static_cast<int>(kind) // 3 weapon object, 4 prop
                    << ",from=" << before << ",to=" << wanted
                    << ",blocks=" << blocks
                    << ",fire_block=" << (state.fireBlock ? 1 : 0) << ",ped_block=" << (state.pedBlock ? 1 : 0)
                    << std::hex << ",fire_vt=0x" << state.fireVtableRva << ",ped_vt=0x" << state.pedVtableRva << std::dec
                    << ",on_fire=" << (FIRE::IS_ENTITY_ON_FIRE(puppet.clone) ? 1 : 0)
                    << ",writes=" << g_poseV2BurnLevelWrites
                    << ",engine_changed=" << g_poseV2BurnLevelEngineChanged
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The recorded fire record (FireCharTrack, the burnt look) on every puppet
    // with a track, checked 6 times a second (wall clock, so pause and scrub
    // too). No record at the playhead: the puppet's own is removed (not while
    // it burns). A record: the recorded volumes are written into the puppet's
    // own (made by the replayed fire, or by a short fire of ours put out on the
    // next check).
    void PlayPoseV2FireChar(std::uint32_t timeMs)
    {
        if (Ctx().clip.fireChars.empty() || !replay::burn::CharEnabled())
            return;
        REPLAY_PROFILE_ZONE("play.fire_char");
        const std::uint64_t now = GetTickCount64();
        // A scrub (the transport scrubbing, the playhead going back or jumping
        // more than wall time allows) makes the
        // fire track rebuild its fires and clear the area often: no fire of
        // ours then, and the per puppet budget starts again once it settles.
        static std::uint32_t lastPlayhead = 0;
        static std::uint64_t lastWall = 0;
        static std::uint64_t settledAt = 0;
        const std::uint64_t wallStep = now - lastWall;
        const std::uint32_t headStep = timeMs > lastPlayhead ? timeMs - lastPlayhead : lastPlayhead - timeMs;
        if (lastWall == 0 || headStep > wallStep * 2 + 100 || Ctx().transport.Scrubbing() || timeMs < lastPlayhead)
            settledAt = now + 400;
        lastPlayhead = timeMs;
        lastWall = now;
        const bool settled = now >= settledAt;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            if (puppet.fireCharClone != puppet.clone)
            {
                puppet.fireCharClone = puppet.clone;
                puppet.fireCharCheckAt = 0;
                puppet.fireCharIgnitedAt = 0;
                puppet.fireCharIgnites = 0;
                puppet.fireCharWrites = 0;
                puppet.fireCharHad = false;
            }
            if (!settled)
                puppet.fireCharIgnites = 0;
            // A short fire of ours is put out as soon as normal play stops
            // (pause, scrub), not at the next check.
            const bool normalPlay = PoseV2NormalPlay();
            if (now < puppet.fireCharCheckAt && !(puppet.fireCharIgnitedAt && !normalPlay))
                continue;
            const auto track = std::find_if(Ctx().clip.fireChars.begin(), Ctx().clip.fireChars.end(),
                [&](const replay::FireCharTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.fireChars.end())
                continue;
            const replay::FireCharState* wanted = replay::FireCharAt(*track, timeMs);
            if (!wanted)
                continue;
            puppet.fireCharCheckAt = now + 160;
            void* address = replay::entities::AddressOf(puppet.clone);
            replay::burn::CharState actual;
            if (!replay::burn::ReadChar(address, actual))
                continue;
            const bool onFire = FIRE::IS_ENTITY_ON_FIRE(puppet.clone) != FALSE;
            const char* action = nullptr;
            if (puppet.fireCharHad && !actual.present && g_poseV2FireCharPlayLogs < 96)
            {
                // Evidence: the engine or a fire clear took the record.
                ++g_poseV2FireCharPlayLogs;
                Ctx().log << "POSE_V2_FIRE_CHAR,phase=lost,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wanted=" << (wanted->present ? 1 : 0) << std::hex << ",wanted_flags=0x" << int(wanted->flags) << std::dec
                    << ",on_fire=" << (onFire ? 1 : 0) << ",dead=" << (ENTITY::IS_ENTITY_DEAD(puppet.clone) ? 1 : 0)
                    << ",faults=" << replay::burn::CharFaults() << ",playhead_ms=" << timeMs << '\n';
            }
            puppet.fireCharHad = actual.present;
            // Our short fire: out on the next check (at least 250 ms later),
            // whatever the record did (never left burning: a fire left on a
            // puppet leaks into later recordings).
            if (puppet.fireCharIgnitedAt && (now - puppet.fireCharIgnitedAt >= 250 || !normalPlay))
            {
                FIRE::STOP_ENTITY_FIRE(puppet.clone, 0);
                puppet.fireCharIgnitedAt = 0;
                if (g_poseV2FireCharPlayLogs < 96)
                {
                    ++g_poseV2FireCharPlayLogs;
                    Ctx().log << "POSE_V2_FIRE_CHAR,phase=ignite_done,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",record=" << (actual.present ? 1 : 0) << ",on_fire=" << (onFire ? 1 : 0)
                        << ",dead=" << (ENTITY::IS_ENTITY_DEAD(puppet.clone) ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
                }
            }
            else if (puppet.fireCharIgnitedAt)
                continue;
            if (!wanted->present)
            {
                if (actual.present && !onFire)
                {
                    std::uint32_t code = 0;
                    const bool removed = replay::burn::RemoveChar(address, code);
                    ++g_poseV2FireCharRemoves;
                    action = removed ? "remove" : "remove_failed";
                }
            }
            else if (actual.present)
            {
                // Part by part (FREC v2 anchors): each recorded volume goes
                // into the puppet's volume on the same part of the entity, a
                // missing part gets a volume made by the engine's own copy, the
                // puppet's other parts are neutralized. Written every pass (the
                // engine moves a live record every frame), so the same playhead
                // always gives the same look. v1 clips (no anchors):
                // index-wise, no creation.
                const bool anchored = !wanted->anchors.empty() && wanted->anchors.size() == wanted->volumes.size();
                replay::burn::CharState recorded;
                recorded.present = true;
                recorded.flags = wanted->flags;
                for (std::size_t n = 0; n < wanted->volumes.size(); ++n)
                {
                    replay::burn::CharVolume volume;
                    std::copy(wanted->volumes[n].begin(), wanted->volumes[n].end(), volume.v);
                    if (anchored)
                    {
                        const replay::FireCharAnchor& anchor = wanted->anchors[n];
                        volume.component = anchor.component;
                        volume.sub = anchor.sub;
                        volume.flags = anchor.flags;
                        volume.mask = anchor.mask;
                        std::copy(std::begin(anchor.dir), std::end(anchor.dir), volume.dir);
                    }
                    recorded.volumes.push_back(volume);
                }
                std::vector<int> match(wanted->volumes.size(), -1);
                if (anchored)
                {
                    std::vector<replay::FireCharAnchor> parts;
                    for (const replay::burn::CharVolume& volume : actual.volumes)
                    {
                        replay::FireCharAnchor part;
                        part.component = volume.component;
                        part.sub = volume.sub;
                        parts.push_back(part);
                    }
                    match = replay::MatchFireCharVolumes(wanted->anchors, parts);
                }
                else
                    for (std::size_t n = 0; n < match.size() && n < actual.volumes.size(); ++n)
                        match[n] = static_cast<int>(n);
                const int missing = static_cast<int>(std::count(match.begin(), match.end(), -1));
                if (missing || actual.volumes.size() != wanted->volumes.size())
                {
                    ++g_poseV2FireCharShapeDiffers;
                    if (g_poseV2FireCharPlayLogs < 96 && puppet.fireCharWrites % 64 == 0)
                    {
                        ++g_poseV2FireCharPlayLogs;
                        Ctx().log << "POSE_V2_FIRE_CHAR,phase=shape_differs,id=" << replay::entity::IdText(puppet.packet.id)
                            << std::hex << ",flags=0x" << int(actual.flags) << ",wanted_flags=0x" << int(wanted->flags) << std::dec
                            << ",volumes=" << actual.volumes.size() << ",wanted_volumes=" << wanted->volumes.size()
                            << ",parts_missing=" << missing << ",anchored=" << (anchored ? 1 : 0)
                            << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                const replay::burn::CharWriteReport report = replay::burn::WriteChar(address, recorded, match, anchored);
                g_poseV2FireCharCreated += static_cast<std::uint64_t>(report.created);
                g_poseV2FireCharCreateFailed += static_cast<std::uint64_t>(report.createFailed);
                if (report.written > 0)
                {
                    ++g_poseV2FireCharWrites;
                    // A line for the first write per puppet, every 64th and
                    // every one that made volumes.
                    if (puppet.fireCharWrites++ % 64 == 0 || report.created || report.createFailed)
                        action = report.created ? "write_created" : (report.createFailed ? "write_create_failed" : "write");
                }
            }
            else if (!onFire && !wanted->anchors.empty() && wanted->anchors.size() == wanted->volumes.size() &&
                replay::burn::CharCreateEnabled() && puppet.fireCharIgnites < 8)
            {
                // No record on the puppet (recreated by a frag or parts
                // rebuild, or scrubbed past a fire that ended): made without a
                // fire (CreateChar), then the recorded parts are written into
                // it in the same pass. No flame, so in pause and scrub too (the
                // normal-play rule is about fire particles).
                std::uint32_t code = 0;
                ++puppet.fireCharIgnites;
                if (replay::burn::CreateChar(address, code))
                {
                    replay::burn::CharState recorded;
                    recorded.present = true;
                    recorded.flags = wanted->flags;
                    for (std::size_t n = 0; n < wanted->volumes.size(); ++n)
                    {
                        replay::burn::CharVolume volume;
                        std::copy(wanted->volumes[n].begin(), wanted->volumes[n].end(), volume.v);
                        const replay::FireCharAnchor& anchor = wanted->anchors[n];
                        volume.component = anchor.component;
                        volume.sub = anchor.sub;
                        volume.flags = anchor.flags;
                        volume.mask = anchor.mask;
                        std::copy(std::begin(anchor.dir), std::end(anchor.dir), volume.dir);
                        recorded.volumes.push_back(volume);
                    }
                    const std::vector<int> none(recorded.volumes.size(), -1);
                    const replay::burn::CharWriteReport report = replay::burn::WriteChar(address, recorded, none, true);
                    g_poseV2FireCharCreated += static_cast<std::uint64_t>(report.created);
                    g_poseV2FireCharCreateFailed += static_cast<std::uint64_t>(report.createFailed);
                    action = report.created ? "record_created" : "record_created_empty";
                    if (!report.created)
                    {
                        // An empty record would linger: taken off again.
                        std::uint32_t removeCode = 0;
                        replay::burn::RemoveChar(address, removeCode);
                    }
                }
                else
                    action = code == 2 ? "record_no_source" : "record_create_failed";
            }
            else if (!onFire && !puppet.fireCharIgnitedAt && settled && normalPlay && puppet.fireCharIgnites < 8)
            {
                // Only in normal play (effects are built on play): paused after
                // a scrub the puppet waits without a record instead of burning
                // on a still playhead. The engine path that makes a record
                // without a fire (0x126E5DC) is not validated. No record on the
                // puppet (a scrub past a fire that ended, or the fire rebuild
                // took it): a short fire of our own makes the engine create
                // one; it is put out on the next check and the recorded volumes
                // are written into it (a dead, frozen, invincible horse without
                // collision gets a record from a 300 ms fire, and a grown
                // volume chars it and stays). Invincible and collision-less
                // puppets refuse START_ENTITY_FIRE: both lifted for the call.
                ENTITY::SET_ENTITY_INVINCIBLE(puppet.clone, FALSE);
                ENTITY::SET_ENTITY_COLLISION(puppet.clone, TRUE, FALSE);
                FIRE::START_ENTITY_FIRE(puppet.clone, 1.0f, -1, 0);
                ENTITY::SET_ENTITY_COLLISION(puppet.clone, FALSE, FALSE);
                ENTITY::SET_ENTITY_INVINCIBLE(puppet.clone, TRUE);
                puppet.fireCharIgnitedAt = now;
                ++puppet.fireCharIgnites;
                action = FIRE::IS_ENTITY_ON_FIRE(puppet.clone) ? "ignite" : "ignite_refused";
            }
            if (action && g_poseV2FireCharPlayLogs < 96)
            {
                ++g_poseV2FireCharPlayLogs;
                Ctx().log << "POSE_V2_FIRE_CHAR,phase=" << action << ",id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wanted=" << (wanted->present ? 1 : 0) << ",had=" << (actual.present ? 1 : 0)
                    << ",volumes=" << wanted->volumes.size() << ",puppet_volumes=" << actual.volumes.size()
                    << ",char=" << (wanted->volumes.empty() ? 0.0f : wanted->volumes[0][12])
                    << ",on_fire=" << (onFire ? 1 : 0) << ",writes=" << g_poseV2FireCharWrites
                    << ",removes=" << g_poseV2FireCharRemoves << ",shape_differs=" << g_poseV2FireCharShapeDiffers
                    << ",created=" << g_poseV2FireCharCreated << ",create_failed=" << g_poseV2FireCharCreateFailed
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The snow and mud trail map follows the replay, not the player. The engine
    // centres the trail map (the texture the terrain reads its snow and mud
    // marks from) on the local player ped, or on an entity set by
    // _0x9F158A49B0D84C3C (0x692F04: override [0x4302528], else the player, a
    // mounted ped's mount instead), and stamps only inside that window
    // (0xC43D64, in-bounds gate at 0xC442BB). The player stays hidden where the
    // clip was opened, possibly across the map: no puppet would be inside the
    // window, so the snow would never deform. The recording had the map on the
    // player: the main character's puppet takes that place while the camera is
    // near it, else the ped or horse puppet nearest the camera; none near: the
    // override is cleared (back to the player). A target far from the last one
    // clears the map, as a teleport does in the game. Diagnostics read the
    // engine globals resolved from code with their instruction bytes checked
    // (build 1491.50), never written.
    namespace trailmap
    {
        constexpr std::uint32_t kSetOverrideRva = 0xF9ED0C;   // _0x9F158A49B0D84C3C body
        constexpr std::uint32_t kResetNativeRva = 0xF9374C;   // _0xEC3F7F24EEEB3BA3 body
        constexpr std::uint32_t kMapObjectFromOverride = 0x538; // override slot = map object + 0x538
        constexpr std::uint32_t kMapCentreOffset = 0x223DE0;    // map 0 centre (x, y, z)
        constexpr std::uint32_t kManagerCount = 0x0C;           // records in use (of 128)
        constexpr std::uint32_t kManagerPending = 0x29890;      // entities waiting for a record
        constexpr std::uint32_t kEntityTrailFlags = 0xB4;       // bit 22: the entity has a trail record
        constexpr std::uint32_t kManagerCapacity = 128;         // 0xC26100: cmp [mgr+0xC], 0x80
        constexpr std::uint64_t kRecordBytes = 0x470;
        constexpr float kKeepMeters = 80.0f;
        constexpr float kNearestMeters = 250.0f;

        struct Globals
        {
            bool resolved = false;
            std::uint64_t overrideSlot = 0; // address of the override entity pointer
            std::uint64_t managerSlot = 0;  // address of the trail manager pointer
            // The snow depth the trail map's look follows, taken at the PLAYER
            // (0x126ADA4: his in-snow depth or a probe at his position,
            // smoothed; the factor at [cfg+0x174] scales depth, shader
            // constants and refill): target, smoothed, config.
            std::uint64_t depthTarget = 0;
            std::uint64_t depthSmoothed = 0;
            std::uint64_t depthConfig = 0;
        };
        Globals g_globals;
        replay::entity::ReplayId g_target{};
        Ped g_targetClone = 0;
        bool g_overrideSet = false;
        ULONGLONG g_nextChoiceAt = 0;
        ULONGLONG g_nextProbeAt = 0;
        ULONGLONG g_nextRefreshAt = 0;
        std::uint32_t g_focusLogs = 0;
        std::uint32_t g_probeLogs = 0;
        std::uint32_t g_readds = 0;
        std::uint32_t g_recordLogs = 0; // TRAIL_RECORD lines

        // A rip-relative disp32 at `at`, checked against the bytes before it.
        bool RipTarget(std::uint64_t base, std::uint32_t at, const std::uint8_t* expect, std::size_t expectSize,
            std::uint32_t instructionEnd, std::uint64_t& target)
        {
            std::uint8_t bytes[16] = {};
            if (expectSize > sizeof(bytes) - 4 || !replay::audio::ReadBytes(base + at, bytes, expectSize + 4) ||
                std::memcmp(bytes, expect, expectSize) != 0)
                return false;
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + expectSize, sizeof(displacement));
            target = base + instructionEnd + static_cast<std::int64_t>(displacement);
            return true;
        }

        void Resolve()
        {
            if (g_globals.resolved)
                return;
            g_globals.resolved = true;
            const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
            // 0xF9ED0C: push rbx; sub rsp,20h; call; mov rbx,rax; test rax,rax;
            // je; then cmp [rip+X],rax at +0x13 (X = the override slot).
            static const std::uint8_t kOverrideCmp[] = {0x48, 0x39, 0x05};
            static const std::uint8_t kSetPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8};
            std::uint8_t prologue[sizeof(kSetPrologue)] = {};
            std::uint64_t slot = 0;
            if (replay::audio::ReadBytes(base + kSetOverrideRva, prologue, sizeof(prologue)) &&
                std::memcmp(prologue, kSetPrologue, sizeof(prologue)) == 0 &&
                RipTarget(base, kSetOverrideRva + 0x13, kOverrideCmp, sizeof(kOverrideCmp), kSetOverrideRva + 0x1A, slot))
                g_globals.overrideSlot = slot;
            // 0xF9374C: sub rsp,28h; xor ecx,ecx; call 0x682988; then
            // mov rcx,[rip+X] at +0xB (X = the trail manager pointer).
            static const std::uint8_t kResetPrologue[] = {0x48, 0x83, 0xEC, 0x28, 0x33, 0xC9, 0xE8};
            static const std::uint8_t kManagerLoad[] = {0x48, 0x8B, 0x0D};
            std::uint8_t reset[sizeof(kResetPrologue)] = {};
            std::uint64_t manager = 0;
            if (replay::audio::ReadBytes(base + kResetNativeRva, reset, sizeof(reset)) &&
                std::memcmp(reset, kResetPrologue, sizeof(reset)) == 0 &&
                RipTarget(base, kResetNativeRva + 0x0B, kManagerLoad, sizeof(kManagerLoad), kResetNativeRva + 0x12, manager))
                g_globals.managerSlot = manager;
            // 0x126ADA4 (build 1491.50): movss [rip+X],xmm0 at 0x126AF0B
            // (target), movss xmm1,[rip+X] at 0x126AF6C (smoothed), mov
            // r8,[rip+X] at 0x126ADE0 (config).
            static const std::uint8_t kStoreXmm0[] = {0xF3, 0x0F, 0x11, 0x05};
            static const std::uint8_t kLoadXmm1[] = {0xF3, 0x0F, 0x10, 0x0D};
            static const std::uint8_t kLoadR8[] = {0x4C, 0x8B, 0x05};
            std::uint64_t address = 0;
            if (RipTarget(base, 0x126AF0B, kStoreXmm0, sizeof(kStoreXmm0), 0x126AF13, address))
                g_globals.depthTarget = address;
            if (RipTarget(base, 0x126AF6C, kLoadXmm1, sizeof(kLoadXmm1), 0x126AF74, address))
                g_globals.depthSmoothed = address;
            if (RipTarget(base, 0x126ADE0, kLoadR8, sizeof(kLoadR8), 0x126ADE7, address))
                g_globals.depthConfig = address;
            Ctx().log << "TRAIL_MAP,phase=resolve,override=" << (g_globals.overrideSlot ? 1 : 0)
                << ",depth=" << (g_globals.depthTarget && g_globals.depthSmoothed && g_globals.depthConfig ? 1 : 0)
                << ",manager=" << (g_globals.managerSlot ? 1 : 0) << '\n';
        }

        bool EntityHasTrail(Entity entity, bool& has)
        {
            const auto address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity));
            std::uint32_t flags = 0;
            if (!address || !replay::audio::ReadBytes(address + kEntityTrailFlags, &flags, sizeof(flags)))
                return false;
            has = ((flags >> 22) & 1u) != 0;
            return true;
        }

        float DistanceSq(const Vector3& a, const Vector3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        void Clear(const char* why)
        {
            if (g_overrideSet)
                GRAPHICS::_0x910E260AEAD855DE();
            if ((g_overrideSet || g_targetClone) && g_focusLogs < 32)
            {
                ++g_focusLogs;
                Ctx().log << "TRAIL_FOCUS,phase=clear,why=" << why << '\n';
            }
            g_overrideSet = false;
            g_target = {};
            g_targetClone = 0;
        }

        void Reset()
        {
            g_overrideSet = false;
            g_target = {};
            g_targetClone = 0;
            g_nextChoiceAt = 0;
            g_nextProbeAt = 0;
            g_nextRefreshAt = 0;
            g_focusLogs = 0;
            g_probeLogs = 0;
            g_readds = 0;
            g_recordLogs = 0;
        }
    }

    bool PoseV2PuppetShown(const replay::playback::Puppet& puppet)
    {
        return (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                   puppet.packet.kind == replay::entity::EntityKind::Horse) &&
            puppet.state == replay::playback::PuppetState::Ready && puppet.clone && puppet.visibleState == 1 &&
            replay::entities::Exists(puppet.clone);
    }

    void UpdatePoseV2TrailFocus(std::uint32_t timeMs)
    {
        using namespace trailmap;
        const ULONGLONG now = GetTickCount64();
        if (now < g_nextChoiceAt)
            return;
        g_nextChoiceAt = now + 250;
        Resolve();
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        const replay::playback::Puppet* keep = nullptr;
        const replay::playback::Puppet* main = nullptr;
        const replay::playback::Puppet* nearest = nullptr;
        float nearestSq = kNearestMeters * kNearestMeters;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (!PoseV2PuppetShown(puppet))
                continue;
            const float distanceSq = DistanceSq(ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE), camera);
            if (puppet.packet.id == g_target && puppet.clone == g_targetClone && distanceSq < kKeepMeters * kKeepMeters)
                keep = &puppet;
            if (puppet.packet.kind == replay::entity::EntityKind::Ped &&
                (puppet.packet.modelHash == arthur || puppet.packet.modelHash == john) &&
                distanceSq < kKeepMeters * kKeepMeters)
                main = &puppet;
            if (distanceSq < nearestSq)
            {
                nearestSq = distanceSq;
                nearest = &puppet;
            }
        }
        const replay::playback::Puppet* chosen = keep ? keep : main ? main : nearest;
        if (!chosen)
        {
            Clear("no_puppet_near_camera");
        }
        else if (chosen->packet.id != g_target || chosen->clone != g_targetClone || now >= g_nextRefreshAt)
        {
            const bool changed = chosen->packet.id != g_target || chosen->clone != g_targetClone;
            // The engine keeps a self-clearing reference: set again now and
            // then (a clone deleted and rebuilt, an engine clear).
            GRAPHICS::_0x9F158A49B0D84C3C(chosen->clone);
            g_overrideSet = true;
            g_target = chosen->packet.id;
            g_targetClone = chosen->clone;
            g_nextRefreshAt = now + 2000;
            if (changed && g_focusLogs < 32)
            {
                ++g_focusLogs;
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(chosen->clone, TRUE, FALSE);
                Ctx().log << "TRAIL_FOCUS,phase=set,id=" << replay::entity::IdText(chosen->packet.id)
                    << ",why=" << (keep ? "kept" : main ? "main_character" : "nearest")
                    << ",camera_m=" << std::sqrt(DistanceSq(at, camera)) << ",playhead_ms=" << timeMs << '\n';
            }
        }

        // Evidence every 2 s (capped): the map centre against the camera and
        // the player, the override in the engine, the records in use, and the
        // shown puppets without a trail record (given one again, bounded).
        if (now < g_nextProbeAt || g_probeLogs >= 60)
            return;
        g_nextProbeAt = now + 2000;
        ++g_probeLogs;
        std::uint64_t overrideEntity = 0;
        float centre[3] = {};
        std::uint64_t manager = 0;
        std::uint32_t records = 0;
        std::uint32_t pending = 0;
        const bool overrideRead = g_globals.overrideSlot && replay::audio::ReadU64(g_globals.overrideSlot, overrideEntity);
        const bool centreRead = g_globals.overrideSlot &&
            replay::audio::ReadBytes(g_globals.overrideSlot - kMapObjectFromOverride + kMapCentreOffset, centre, sizeof(centre));
        const bool managerRead = g_globals.managerSlot && replay::audio::ReadU64(g_globals.managerSlot, manager) && manager &&
            replay::audio::ReadBytes(manager + kManagerCount, &records, sizeof(records)) &&
            replay::audio::ReadBytes(manager + kManagerPending, &pending, sizeof(pending));
        std::uint32_t shown = 0;
        std::uint32_t noTrail = 0;
        std::uint32_t readdsNow = 0;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (!PoseV2PuppetShown(puppet))
                continue;
            ++shown;
            bool has = true;
            if (EntityHasTrail(puppet.clone, has) && !has)
            {
                ++noTrail;
                // A full manager refused the create-time call (128 records, no
                // queue for the forced add): again later.
                if (readdsNow < 4 && g_readds < 400)
                {
                    ENTITY::_ADD_ENTITY_TRACKING_TRAILS(puppet.clone);
                    ++readdsNow;
                    ++g_readds;
                }
            }
        }
        // The trail records of the target and a few shown puppets (manager
        // record: entity at +0x48, 0x470 bytes each from +0x10): flags, ring
        // count, last processed frame and time, speed, the eight limb heights,
        // and the entity's +0xB0 bit 7.
        if (managerRead && g_recordLogs < 40)
        {
            std::uint32_t logged = 0;
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if (!PoseV2PuppetShown(puppet) || logged >= 3)
                    continue;
                const bool isTarget = puppet.clone == g_targetClone;
                if (!isTarget && logged >= 2)
                    continue;
                const auto entity = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
                if (!entity)
                    continue;
                std::uint64_t record = 0;
                for (std::uint32_t index = 0; index < kManagerCapacity && !record; ++index)
                {
                    std::uint64_t owner = 0;
                    const std::uint64_t at = manager + 0x10 + static_cast<std::uint64_t>(index) * kRecordBytes;
                    if (replay::audio::ReadU64(at + 0x48, owner) && owner == entity)
                        record = at;
                }
                std::uint32_t flags = 0, frame = 0, entityB0 = 0;
                std::uint16_t ring = 0;
                float time = 0.0f, speed = 0.0f;
                float limbs[8] = {};
                std::uint8_t head[0x60] = {};
                if (record)
                {
                    replay::audio::ReadBytes(record, &flags, sizeof(flags));
                    replay::audio::ReadBytes(record + 0x0E, &ring, sizeof(ring));
                    replay::audio::ReadBytes(record + 0x20, &frame, sizeof(frame));
                    replay::audio::ReadBytes(record + 0x24, &time, sizeof(time));
                    replay::audio::ReadBytes(record + 0x3FC, &speed, sizeof(speed));
                    replay::audio::ReadBytes(record + 0x340, limbs, sizeof(limbs));
                    replay::audio::ReadBytes(record, head, sizeof(head));
                }
                replay::audio::ReadBytes(entity + 0xB0, &entityB0, sizeof(entityB0));
                ++logged;
                ++g_recordLogs;
                char text[900]{};
                int used = std::snprintf(text, sizeof(text),
                    "TRAIL_RECORD,id=%s,target=%d,found=%d,flags=0x%x,ring=%u,frame=%u,time=%.3f,speed=%.3f,"
                    "limbs=%.2f;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f;%.2f,off_world=%u,frozen=%d,collision=%d,head=",
                    replay::entity::IdText(puppet.packet.id).c_str(), isTarget ? 1 : 0, record ? 1 : 0, flags, ring, frame,
                    time, speed, limbs[0], limbs[1], limbs[2], limbs[3], limbs[4], limbs[5], limbs[6], limbs[7],
                    (entityB0 >> 7) & 1u, ENTITY::_IS_ENTITY_FROZEN(puppet.clone) ? 1 : 0,
                    ENTITY::GET_ENTITY_COLLISION_DISABLED(puppet.clone) ? 0 : 1);
                for (std::size_t byte = 0; byte < sizeof(head) && used > 0 && used < static_cast<int>(sizeof(text)) - 3; ++byte)
                    used += std::snprintf(text + used, sizeof(text) - static_cast<std::size_t>(used), "%02x", head[byte]);
                Ctx().log << text << ",playhead_ms=" << timeMs << '\n';
            }
        }
        float depthTarget = -1.0f, depthSmoothed = -1.0f, depthFactor = -1.0f;
        if (g_globals.depthTarget)
            replay::audio::ReadBytes(g_globals.depthTarget, &depthTarget, sizeof(depthTarget));
        if (g_globals.depthSmoothed)
            replay::audio::ReadBytes(g_globals.depthSmoothed, &depthSmoothed, sizeof(depthSmoothed));
        std::uint64_t config = 0;
        if (g_globals.depthConfig && replay::audio::ReadU64(g_globals.depthConfig, config) && config)
            replay::audio::ReadBytes(config + 0x174, &depthFactor, sizeof(depthFactor));
        Ctx().log << "TRAIL_DEPTH,target=" << depthTarget << ",smoothed=" << depthSmoothed << ",factor=" << depthFactor
            << ",playhead_ms=" << timeMs << '\n';
        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        Vector3 centreAt{};
        centreAt.x = centre[0];
        centreAt.y = centre[1];
        centreAt.z = centre[2];
        const auto target = reinterpret_cast<std::uint64_t>(g_targetClone ? replay::entities::AddressOf(g_targetClone) : nullptr);
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "TRAIL_MAP,phase=probe,override_read=%d,override_is_target=%d,override_set=%d,centre=%.1f;%.1f;%.1f,"
            "centre_camera_m=%.1f,centre_player_m=%.1f,records=%u,pending=%u,manager_read=%d,shown=%u,no_trail=%u,readds=%u,"
            "playhead_ms=%u\n",
            overrideRead ? 1 : 0, overrideRead && target && overrideEntity == target ? 1 : 0, g_overrideSet ? 1 : 0,
            centre[0], centre[1], centre[2], centreRead ? std::sqrt(DistanceSq(centreAt, camera)) : -1.0f,
            centreRead ? std::sqrt(DistanceSq(centreAt, player)) : -1.0f, records, pending, managerRead ? 1 : 0, shown, noTrail,
            readdsNow, timeMs);
        Ctx().log << line;
    }

    // What the seek settle waits for, after this tick's work: bodies alive at
    // the playhead still being built (Loading, Dressing, or created and not
    // placed yet), those whose model is not loaded, and those the create budget
    // left waiting with their model loaded (the hold in normal play).
    void UpdatePoseV2SettleState(std::uint32_t timeMs)
    {
        (void)timeMs;
        replay::playback::SettleState state;
        const ULONGLONG now = GetTickCount64();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind == replay::entity::EntityKind::Weapon ||
                puppet.packet.kind == replay::entity::EntityKind::Prop)
                continue;
            ++state.puppets;
            const bool loading = puppet.state == replay::playback::PuppetState::Loading && !puppet.waitingForParent &&
                STREAMING::IS_MODEL_VALID(puppet.packet.modelHash); // an unknown model only fails, never builds
            const bool building = loading || puppet.state == replay::playback::PuppetState::Dressing ||
                (puppet.state == replay::playback::PuppetState::Ready && puppet.visibleState < 0);
            if (!building)
                continue;
            ++state.building;
            if (!loading)
                continue;
            if (STREAMING::HAS_MODEL_LOADED(puppet.packet.modelHash))
            {
                // Only bodies the budget alone keeps back: not a vehicle
                // waiting for its prop sets, not an entity waiting for a
                // later pose segment (wait_segment).
                const bool body = puppet.packet.kind == replay::entity::EntityKind::Ped ||
                    puppet.packet.kind == replay::entity::EntityKind::Horse;
                const bool waitsSegment = std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(),
                    puppet.packet.id.Packed()) != g_poseV2WaitSegmentLogged.end();
                if (body && !waitsSegment && !puppet.clone && now >= puppet.nextCreateAt)
                    ++state.dueWithModel;
            }
            else
                ++state.modelsMissing;
        }
        g_poseV2SettleState = state;
    }

    // The missing puppet detector (puppet_watch.h). In normal play, every
    // checkMs: the bodies alive at the playhead within radiusM of the camera
    // whose puppet is not shown, and why. PUPPET_MISSING when one stays so for
    // thresholdMs, PUPPET_MISSING_END when it shows, and PUPPET_WATCH_SUMMARY
    // at the end. Also: PUPPET_POP_IN, a body shown for the first time close to
    // the camera right after it entered the recording (the recording found it
    // late: entity_discovery.h), and PUPPET_INTERIOR, a shown body whose engine
    // interior is not the one at its position (a suspect for bodies unseen from
    // inside a building; evidence only).
    replay::watch::PuppetWatch g_poseV2Watch;
    bool g_poseV2WatchHeld = false;     // the seek settle or the load wait (script.cpp)
    bool g_poseV2WatchActive = false;
    ULONGLONG g_poseV2WatchNextAt = 0;
    ULONGLONG g_poseV2InteriorNextAt = 0;
    std::uint32_t g_poseV2WatchLastPlayhead = 0;
    std::uint32_t g_poseV2WatchStartMs = 0;
    std::uint32_t g_poseV2WatchLogs = 0;
    std::uint32_t g_poseV2WatchSuspends = 0;
    std::uint32_t g_poseV2PopIns = 0;
    std::uint32_t g_poseV2PopInsOnScreen = 0;
    std::uint32_t g_poseV2PopInLogs = 0;
    std::vector<std::uint32_t> g_poseV2PopInJudged;
    // Ready puppets whose record gave no sample at a playhead where the
    // entity's recording has one (the record the puppet plays is not the one
    // the clip has, e.g. a wagon kept on its empty create record). One line per
    // entity, counted in the summary.
    std::vector<std::uint32_t> g_poseV2RecordUnsampled;
    // Interior: consecutive mismatching checks per id, and the ids logged.
    std::unordered_map<std::uint32_t, std::uint32_t> g_poseV2InteriorStreak;
    std::vector<std::uint32_t> g_poseV2InteriorLogged;
    std::vector<replay::watch::Event> g_poseV2WatchEvents;

    void LogPoseV2WatchEvents(std::uint32_t timeMs)
    {
        for (const replay::watch::Event& event : g_poseV2WatchEvents)
        {
            if (g_poseV2WatchLogs >= 200)
                break;
            ++g_poseV2WatchLogs;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(event.id);
            const replay::entity::ReplayId id = replay::entity::ReplayId::FromPacked(event.id);
            std::ostream& log = Ctx().log;
            log << (event.begin ? "PUPPET_MISSING" : "PUPPET_MISSING_END") << ",id=" << replay::entity::IdText(id)
                << ",kind=" << (create ? replay::entity::KindText(create->kind) : "?") << ",model=0x" << std::hex
                << (create ? create->modelHash : 0u) << std::dec << ",reason=" << replay::watch::ReasonText(event.reason)
                << ",camera_m=" << static_cast<int>(event.distanceM) << ",missing_ms=" << event.missingMs;
            if (!event.begin)
                log << ",suspended=" << (event.suspended ? 1 : 0);
            if (create)
                log << ",entry_ms=" << create->timeMs;
            log << ",playhead_ms=" << timeMs << '\n';
        }
        g_poseV2WatchEvents.clear();
    }

    void SuspendPoseV2Watch(std::uint32_t timeMs)
    {
        g_poseV2Watch.Suspend(GetTickCount64(), g_poseV2WatchEvents);
        LogPoseV2WatchEvents(timeMs);
        g_poseV2InteriorStreak.clear();
        ++g_poseV2WatchSuspends;
    }

    replay::watch::Reason PoseV2MissingReason(const replay::playback::Puppet* puppet)
    {
        using replay::watch::Reason;
        if (!puppet)
            return Reason::NoPuppet;
        switch (puppet->state)
        {
        case replay::playback::PuppetState::Loading:
            if (!STREAMING::HAS_MODEL_LOADED(puppet->packet.modelHash))
                return Reason::ModelLoading;
            if (std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(), puppet->packet.id.Packed()) !=
                g_poseV2WaitSegmentLogged.end())
                return Reason::WaitSegment;
            return Reason::CreateWait;
        case replay::playback::PuppetState::Dressing:
            return Reason::Dressing;
        case replay::playback::PuppetState::Failed:
            return Reason::Failed;
        default:
            break;
        }
        if (!puppet->clone || !replay::entities::Exists(puppet->clone))
            return Reason::NoPuppet;
        if (puppet->visibleState == 1)
            return Reason::Invisible;
        const bool body = puppet->packet.kind == replay::entity::EntityKind::Ped ||
            puppet->packet.kind == replay::entity::EntityKind::Horse;
        // A body that could not be armed stays hidden (arm_refused).
        if (body && puppet->overrideSlot < 0)
            return puppet->armFailureLogged ? Reason::ArmRefused : Reason::Unposed;
        return Reason::NoPose;
    }

    void UpdatePoseV2PuppetWatch(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.puppet_watch");
        const bool active = PoseV2NormalPlay() && !g_poseV2WatchHeld && !g_poseV2CheapScrub;
        // A jump inside normal play (a marker, a loop) is a new start too.
        const bool jumped = timeMs < g_poseV2WatchLastPlayhead || timeMs > g_poseV2WatchLastPlayhead + 1500;
        g_poseV2WatchLastPlayhead = timeMs;
        if (!active || jumped)
        {
            if (g_poseV2WatchActive)
                SuspendPoseV2Watch(timeMs);
            g_poseV2WatchActive = active && !jumped;
            if (!active)
                return;
        }
        g_poseV2WatchActive = true;
        const ULONGLONG now = GetTickCount64();
        const replay::watch::Config& config = g_poseV2Watch.Settings();
        if (now < g_poseV2WatchNextAt)
            return;
        g_poseV2WatchNextAt = now + config.checkMs;
        const bool interiorCheck = now >= g_poseV2InteriorNextAt;
        if (interiorCheck)
            g_poseV2InteriorNextAt = now + 1000;
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        static std::vector<replay::watch::Observation> missing;
        missing.clear();
        std::uint32_t recordedSamples = 0;
        constexpr std::uint32_t kMaxRecordedSamples = 16; // a full pose decode each
        for (const replay::entity::CreatePacket& packet : g_poseV2Alive)
        {
            if (packet.kind != replay::entity::EntityKind::Ped && packet.kind != replay::entity::EntityKind::Horse &&
                packet.kind != replay::entity::EntityKind::Vehicle && packet.kind != replay::entity::EntityKind::TrainCar)
                continue;
            const replay::playback::Puppet* puppet = FindPoseV2Puppet(packet.id);
            const bool exists = puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                replay::entities::Exists(puppet->clone);
            const bool shown = exists && puppet->visibleState == 1 && ENTITY::IS_ENTITY_VISIBLE(puppet->clone);
            Vector3 at{};
            bool haveAt = false;
            if (exists && puppet->visibleState == 1)
            {
                at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
                haveAt = true;
            }
            else if (puppet && puppet->hasWorld)
            {
                at.x = puppet->lastWorld.position.x;
                at.y = puppet->lastWorld.position.y;
                at.z = puppet->lastWorld.position.z;
                haveAt = true;
            }
            else if (recordedSamples < kMaxRecordedSamples)
            {
                ++recordedSamples;
                replay::pose::WorldTransform world{};
                float track[3]{};
                if (packet.kind == replay::entity::EntityKind::TrainCar) // its rigid track
                {
                    if (replay::trains::RecordedWorldAt(packet.id, timeMs, track))
                    {
                        at.x = track[0];
                        at.y = track[1];
                        at.z = track[2];
                        haveAt = true;
                    }
                }
                else if (PoseV2RecordedWorldAt(packet.id, timeMs, world))
                {
                    at.x = world.position.x;
                    at.y = world.position.y;
                    at.z = world.position.z;
                    haveAt = true;
                }
            }
            if (!haveAt)
                continue; // no pose anywhere near the playhead: nothing to place, nothing to miss
            const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > config.radiusM)
                continue;
            if (!shown)
            {
                missing.push_back({packet.id.Packed(), PoseV2MissingReason(puppet), distance});
                continue;
            }
            const std::uint32_t packed = packet.id.Packed();
            if (std::find(g_poseV2PopInJudged.begin(), g_poseV2PopInJudged.end(), packed) == g_poseV2PopInJudged.end())
            {
                g_poseV2PopInJudged.push_back(packed);
                if (replay::watch::IsPopIn(packet.timeMs, timeMs, g_poseV2WatchStartMs, distance))
                {
                    float sx = 0.0f, sy = 0.0f;
                    const bool onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
                    ++g_poseV2PopIns;
                    g_poseV2PopInsOnScreen += onScreen ? 1 : 0;
                    if (g_poseV2PopInLogs < 64)
                    {
                        ++g_poseV2PopInLogs;
                        Ctx().log << "PUPPET_POP_IN,id=" << replay::entity::IdText(packet.id)
                                  << ",kind=" << replay::entity::KindText(packet.kind) << ",model=0x" << std::hex
                                  << packet.modelHash << std::dec << ",camera_m=" << static_cast<int>(distance)
                                  << ",on_screen=" << (onScreen ? 1 : 0) << ",entry_ms=" << packet.timeMs
                                  << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            if (interiorCheck)
            {
                const Interior mine = INTERIOR::GET_INTERIOR_FROM_ENTITY(puppet->clone);
                Interior there = INTERIOR::GET_INTERIOR_AT_COORDS(at.x, at.y, at.z);
                if (there && !INTERIOR::IS_VALID_INTERIOR(there))
                    there = 0;
                std::uint32_t& streak = g_poseV2InteriorStreak[packed];
                streak = mine != there ? streak + 1 : 0;
                // Two checks in a row (a doorway flips for a moment).
                if (streak == 2 && std::find(g_poseV2InteriorLogged.begin(), g_poseV2InteriorLogged.end(), packed) ==
                                       g_poseV2InteriorLogged.end())
                {
                    g_poseV2InteriorLogged.push_back(packed);
                    if (g_poseV2InteriorLogged.size() <= 32)
                        Ctx().log << "PUPPET_INTERIOR,id=" << replay::entity::IdText(packet.id)
                                  << ",kind=" << replay::entity::KindText(packet.kind) << ",entity_interior=" << mine
                                  << ",position_interior=" << there << ",camera_m=" << static_cast<int>(distance)
                                  << ",camera_interior=" << INTERIOR::GET_INTERIOR_AT_COORDS(camera.x, camera.y, camera.z)
                                  << ",frozen=" << (ENTITY::_IS_ENTITY_FROZEN(puppet->clone) ? 1 : 0)
                                  << ",playhead_ms=" << timeMs << '\n';
                }
            }
        }
        g_poseV2Watch.Update(now, missing, g_poseV2WatchEvents);
        LogPoseV2WatchEvents(timeMs);
    }

    // The puppet blink detector (puppet_blink.h). Every tick of normal play,
    // every body (ped, horse) the recording says is visible at the playhead is
    // classified: drawn, hidden, unposed, bind or missing. Each episode closes
    // into a PUPPET_BLINK line (kBlinkLogCap a playback); PUPPET_BLINK_SUMMARY
    // at the end of the playback has the totals, the counted episodes (on
    // screen and near: the bench fails the run on them) and the worst ones.
    // The engine is read (camera, screen) only on a blink tick, on the
    // episode's first and every Rule::whereEveryFrames: a drawn body costs a
    // few compares and the override slot's counters (play.blink).
    replay::blink::Tracker g_blink;
    std::vector<replay::blink::Episode> g_blinkClosed;
    std::uint32_t g_blinkLastPlayhead = 0;
    std::uint32_t g_blinkLogs = 0;
    std::uint32_t g_blinkDropped = 0;
    std::uint32_t g_blinkPlaybacks = 0;
    constexpr std::uint32_t kBlinkLogCap = 96;

    void LogPoseV2Blinks()
    {
        for (const replay::blink::Episode& e : g_blinkClosed)
        {
            // Every counted episode before the far ones: past the cap only
            // the counted keep a line until twice the cap.
            if (g_blinkLogs >= (e.counts ? 2 * kBlinkLogCap : kBlinkLogCap))
            {
                ++g_blinkDropped;
                continue;
            }
            ++g_blinkLogs;
            Ctx().log << "PUPPET_BLINK,id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(e.id))
                      << ",model=0x" << std::hex << e.model << std::dec << ",kind=" << replay::blink::KindText(e.kind)
                      << ",from_ms=" << e.fromMs << ",frames=" << e.frames << ",ms=" << e.wallMs
                      << ",cause=" << replay::blink::CauseText(e.cause)
                      << ",camera_dist_m=" << (e.cameraM < 0.0f ? -1 : static_cast<int>(e.cameraM + 0.5f))
                      << ",on_screen=" << (e.onScreen ? 1 : 0) << ",counts=" << (e.counts ? 1 : 0)
                      << ",to_ms=" << e.toMs << ",cut=" << (e.cut ? 1 : 0) << '\n';
        }
        g_blinkClosed.clear();
    }

    void ResetPoseV2Blink(std::uint32_t startMs)
    {
        g_blink.Reset();
        g_blinkClosed.clear();
        g_blinkLastPlayhead = startMs;
        g_blinkLogs = 0;
        g_blinkDropped = 0;
    }

    // The tick's gate: normal play outside the seek settle and the cheap
    // scrub. A jump (a marker, a loop) ends the stretch like a pause.
    bool BeginPoseV2BlinkTick(std::uint32_t timeMs)
    {
        const bool active = PoseV2NormalPlay() && !g_poseV2WatchHeld && !g_poseV2CheapScrub;
        const bool jumped = timeMs < g_blinkLastPlayhead || timeMs > g_blinkLastPlayhead + 1500;
        g_blinkLastPlayhead = timeMs;
        if ((!active || jumped) && g_blink.Active())
        {
            g_blink.Suspend(GetTickCount64(), g_blinkClosed);
            LogPoseV2Blinks();
        }
        if (!active)
            return false;
        g_blink.BeginTick(timeMs);
        return true;
    }

    void EndPoseV2BlinkTick()
    {
        REPLAY_PROFILE_ZONE("play.blink.end");
        g_blink.EndTick(GetTickCount64(), g_blinkClosed);
        if (!g_blinkClosed.empty())
            LogPoseV2Blinks();
    }

    // Where a blinking body is: its clone, its last placement or its recorded
    // pose; the distance to the rendered camera and whether that point
    // projects on screen.
    replay::blink::Where PoseV2BlinkWhere(const replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        replay::blink::Where where;
        Vector3 at{};
        bool haveAt = false;
        if (puppet.clone && replay::entities::Exists(puppet.clone))
        {
            at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
            haveAt = true;
        }
        else if (puppet.hasWorld)
        {
            at.x = puppet.lastWorld.position.x;
            at.y = puppet.lastWorld.position.y;
            at.z = puppet.lastWorld.position.z;
            haveAt = true;
        }
        else
        {
            replay::pose::WorldTransform world{};
            if (PoseV2RecordedWorldAt(puppet.packet.id, timeMs, world))
            {
                at.x = world.position.x;
                at.y = world.position.y;
                at.z = world.position.z;
                haveAt = true;
            }
        }
        if (!haveAt)
            return where;
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
        where.cameraM = std::sqrt(dx * dx + dy * dy + dz * dz);
        float sx = 0.0f, sy = 0.0f;
        where.onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
        return where;
    }

    void NotePoseV2Blink(const replay::playback::Puppet& puppet, replay::blink::Kind kind, replay::blink::Cause cause,
        std::uint32_t timeMs)
    {
        const std::uint32_t packed = puppet.packet.id.Packed();
        const ULONGLONG now = GetTickCount64();
        if (kind == replay::blink::Kind::None)
        {
            g_blink.Clear(packed, now, g_blinkClosed);
            return;
        }
        replay::blink::Where where;
        const bool read = g_blink.WantsWhere(packed, kind);
        if (read)
            where = PoseV2BlinkWhere(puppet, timeMs);
        g_blink.Note(packed, puppet.packet.modelHash, kind, cause, timeMs, now, read ? &where : nullptr, g_blinkClosed);
    }

    // A body with no Ready puppet this tick (loading, dressing, failed, its
    // clone gone): missing when the recording shows it.
    void JudgePoseV2BlinkAbsent(const replay::playback::Puppet& puppet, bool alive, std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.blink");
        const std::uint32_t packed = puppet.packet.id.Packed();
        replay::blink::Frame frame;
        frame.alive = alive;
        frame.recordedVisible = alive && PoseV2RecordedVisible(packed, timeMs);
        frame.intentional = puppet.waitingForParent ||
            std::find(g_poseV2WaitSegmentLogged.begin(), g_poseV2WaitSegmentLogged.end(), packed) !=
                g_poseV2WaitSegmentLogged.end();
        if (frame.recordedVisible && !frame.intentional &&
            (g_blink.ShownBefore(packed) || timeMs >= puppet.packet.timeMs + g_blink.Settings().graceMs))
        {
            // Inside the pose track of the segment at the playhead (the
            // Ready path's rule): before its first sample, or after its last
            // with no later record, the body is not drawn by design.
            const replay::entity::PoseSegment* segment = PoseV2SegmentAt(puppet.packet.id, timeMs);
            const std::uint32_t recordId = segment ? segment->recordId : packed;
            std::uint32_t firstMs = 0, lastMs = 0;
            const bool span = replay::pose::capture::TrackSpan(recordId, firstMs, lastMs);
            // The recorded player holds his nearest pose outside it (player_hold.h).
            frame.inTrack = span && ((g_poseV2PlayerPacked && packed == g_poseV2PlayerPacked) ||
                (timeMs >= firstMs && (timeMs <= lastMs + 100 || PoseV2RecordHasSuccessor(puppet.packet.id, recordId))));
        }
        const replay::blink::Kind kind = g_blink.Judge(packed, frame, timeMs, puppet.packet.timeMs);
        replay::blink::Cause cause = replay::blink::Cause::NoPuppet;
        if (kind != replay::blink::Kind::None && g_blink.WantsWhere(packed, kind))
        {
            switch (PoseV2MissingReason(&puppet))
            {
            case replay::watch::Reason::ModelLoading:
                cause = replay::blink::Cause::ModelLoading;
                break;
            case replay::watch::Reason::CreateWait:
                cause = replay::blink::Cause::CreateWait;
                break;
            case replay::watch::Reason::Dressing:
                cause = replay::blink::Cause::Dressing;
                break;
            case replay::watch::Reason::Failed:
                cause = replay::blink::Cause::Failed;
                break;
            default:
                break;
            }
        }
        NotePoseV2Blink(puppet, kind, cause, timeMs);
    }

    // A Ready body after this tick's visibility decision.
    void JudgePoseV2BlinkReady(replay::playback::Puppet& puppet, bool alive, bool recordedVisible, bool inTrack,
        bool sampled, std::uint32_t poseRecord, bool recordChanged, bool moved, std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.blink");
        const std::uint32_t packed = puppet.packet.id.Packed();
        replay::blink::Frame frame;
        frame.alive = alive;
        frame.recordedVisible = recordedVisible;
        frame.inTrack = inTrack;
        // The never animated hide judged for this record (a bind pose recording
        // is hidden on purpose).
        frame.intentional = puppet.neverRecord == poseRecord && puppet.neverHidden;
        frame.ready = true;
        frame.shown = puppet.visibleState == 1;
        frame.armed = puppet.overrideSlot >= 0;
        // The frame since the last tick: the same slot, shown then, the
        // playhead moving (a paused frame is not judged).
        std::uint64_t writes = 0, mismatches = 0;
        if (frame.armed)
        {
            const replay::pose::capture::OverrideStats stats =
                replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(puppet.overrideSlot));
            writes = stats.applies + stats.altApplies + stats.guardApplies + stats.prefixApplies + stats.staleApplies +
                stats.lateWrites + stats.weaponPassWrites;
            mismatches = stats.boneCountMismatch;
            frame.writesKnown = moved &&
                puppet.blinkWrites.Delta(puppet.overrideSlot, writes, mismatches, frame.writes, frame.mismatches);
        }
        puppet.blinkWrites.Keep(puppet.overrideSlot, frame.shown, writes, mismatches);
        const replay::blink::Kind kind = g_blink.Judge(packed, frame, timeMs, puppet.packet.timeMs);
        replay::blink::Cause cause = replay::blink::Cause::Other;
        if (kind == replay::blink::Kind::Bind)
            cause = replay::blink::BindCause(frame);
        else if (kind != replay::blink::Kind::None)
        {
            replay::blink::ArmState state;
            state.sampled = sampled;
            state.armRefused = puppet.armFailureLogged;
            state.prefixHold = puppet.prefixHold.Active();
            state.layerRetry = puppet.layerRetries > 0;
            state.segmentHole = puppet.switchGap.inHole;
            state.recordChanged = recordChanged;
            state.lostToChange = puppet.lostToChange;
            state.shownBefore = g_blink.ShownBefore(packed);
            cause = replay::blink::ArmCause(state);
        }
        NotePoseV2Blink(puppet, kind, cause, timeMs);
    }

    // PUPPET_BLINK_SUMMARY: one line per playback (Debug=0 too), read by the
    // bench (puppet_blink check) and tools/qa_triage.py.
    void LogPoseV2BlinkSummary()
    {
        g_blink.Suspend(GetTickCount64(), g_blinkClosed);
        LogPoseV2Blinks();
        using replay::blink::Kind;
        const replay::blink::Summary& s = g_blink.Stats();
        const replay::blink::Rule& rule = g_blink.Settings();
        const auto kindCount = [&](const std::uint32_t* table, Kind kind) { return table[static_cast<int>(kind)]; };
        ++g_blinkPlaybacks;
        Ctx().log << "PUPPET_BLINK_SUMMARY,playback=" << g_blinkPlaybacks << ",ticks=" << s.ticks
                  << ",body_ticks=" << s.judged << ",episodes=" << s.episodes << ",frames=" << s.frames
                  << ",on_screen_episodes=" << s.onScreenEpisodes << ",counted=" << s.counted
                  << ",on_screen_frames=" << s.countedFrames
                  << ",hidden=" << kindCount(s.perKind, Kind::Hidden) << ",unposed=" << kindCount(s.perKind, Kind::Unposed)
                  << ",bind=" << kindCount(s.perKind, Kind::Bind) << ",missing=" << kindCount(s.perKind, Kind::Missing)
                  << ",counted_hidden=" << kindCount(s.countedPerKind, Kind::Hidden)
                  << ",counted_unposed=" << kindCount(s.countedPerKind, Kind::Unposed)
                  << ",counted_bind=" << kindCount(s.countedPerKind, Kind::Bind)
                  << ",counted_missing=" << kindCount(s.countedPerKind, Kind::Missing)
                  << ",lines=" << g_blinkLogs << ",dropped=" << g_blinkDropped << ",suspends=" << s.suspends
                  << ",radius_m=" << static_cast<int>(rule.radiusM) << ",min_frames=" << rule.minFrames
                  << ",no_write_min_frames=" << rule.noWriteMinFrames << ",grace_ms=" << rule.graceMs << ",worst="
                  << replay::blink::WorstText(s.worst, [](std::uint32_t id) {
                         return replay::entity::IdText(replay::entity::ReplayId::FromPacked(id));
                     })
                  << '\n';
    }

    void LogPoseV2WatchSummary()
    {
        g_poseV2Watch.Finish(GetTickCount64(), g_poseV2WatchEvents);
        LogPoseV2WatchEvents(g_poseV2WatchLastPlayhead);
        const replay::watch::Summary& summary = g_poseV2Watch.Stats();
        const replay::watch::Config& config = g_poseV2Watch.Settings();
        std::ostream& log = Ctx().log;
        log << "PUPPET_WATCH_SUMMARY,checks=" << summary.checks << ",episodes=" << summary.episodes
            << ",entities=" << summary.entities << ",worst_ms=" << summary.worstMs << ",total_ms=" << summary.totalMs;
        for (int reason = 0; reason < static_cast<int>(replay::watch::Reason::Count); ++reason)
            log << ',' << replay::watch::ReasonText(static_cast<replay::watch::Reason>(reason)) << '='
                << summary.perReason[reason];
        log << ",pop_ins=" << g_poseV2PopIns << ",pop_ins_on_screen=" << g_poseV2PopInsOnScreen
            << ",interior_mismatch=" << g_poseV2InteriorLogged.size() << ",create_refused=" << g_poseV2CreateRefused
            << ",record_unsampled=" << g_poseV2RecordUnsampled.size()
            << ",suspends=" << g_poseV2WatchSuspends << ",radius_m=" << config.radiusM
            << ",threshold_ms=" << config.thresholdMs << '\n';
        // Arms through the skeleton map, arms given up (hidden since, never a
        // T-pose) and their lines.
        log << "POSE_V2_ARM_SUMMARY,mapped=" << g_poseV2MappedArms << ",refused=" << g_poseV2RefusedArms
            << ",refused_hidden_lines=" << g_poseV2RefusedHiddenLogs << '\n';
        // The skeleton signature checks (Debug=0 too).
        log << "POSE_V2_SKELETON_SIGNATURE_SUMMARY,same=" << g_poseV2Signature.same
            << ",differ=" << g_poseV2Signature.differ << ",unknown=" << g_poseV2Signature.unknown
            << ",mapped=" << g_poseV2Signature.mapped << ",refused=" << g_poseV2Signature.refused
            << ",rechecks=" << g_poseV2Signature.rechecks << ",rearms=" << g_poseV2Signature.rearms
            << ",timeouts=" << g_poseV2Signature.timeouts << ",rebuilt=" << g_poseV2Signature.rebuilt << '\n';
        // The unposed episodes, Debug=0 too (_SUMMARY).
        g_poseV2Unposed.Finish();
        {
            const replay::watch::UnposedSummary& unposed = g_poseV2Unposed.Stats();
            const replay::watch::UnposedRule& rule = g_poseV2Unposed.Rule();
            log << "POSE_V2_UNPOSED_SUMMARY,episodes=" << unposed.episodes << ",transient=" << unposed.transient
                << ",persistent=" << unposed.persistent
                << ",create=" << unposed.perCause[static_cast<int>(replay::watch::UnposedCause::Create)]
                << ",rearm=" << unposed.perCause[static_cast<int>(replay::watch::UnposedCause::Rearm)]
                << ",persistent_create=" << unposed.persistentPerCause[static_cast<int>(replay::watch::UnposedCause::Create)]
                << ",persistent_rearm=" << unposed.persistentPerCause[static_cast<int>(replay::watch::UnposedCause::Rearm)]
                << ",worst_transient_ms=" << unposed.worstTransientMs << ",worst_transient_ticks=" << unposed.worstTransientTicks
                << ",worst_ms=" << unposed.worstMs << ",lines=" << g_poseV2UnposedHideLogs
                << ",rule_ms=" << rule.persistMs << ",rule_ticks=" << rule.persistTicks << '\n';
        }
        LogPoseV2TPoseSummary(); // Debug=0 too (_SUMMARY)
        LogPoseV2BlinkSummary(); // Debug=0 too (_SUMMARY)
        log.flush();
    }

    void ResetPoseV2Watch(std::uint32_t startMs)
    {
        g_poseV2Watch.Reset();
        ResetPoseV2Blink(startMs);
        g_poseV2WatchActive = false;
        g_poseV2WatchNextAt = 0;
        g_poseV2InteriorNextAt = 0;
        g_poseV2WatchLastPlayhead = startMs;
        g_poseV2WatchStartMs = startMs;
        g_poseV2WatchLogs = 0;
        g_poseV2WatchSuspends = 0;
        g_poseV2PopIns = 0;
        g_poseV2PopInsOnScreen = 0;
        g_poseV2PopInLogs = 0;
        g_poseV2PopInJudged.clear();
        g_poseV2RecordUnsampled.clear();
        g_poseV2InteriorStreak.clear();
        g_poseV2InteriorLogged.clear();
        g_poseV2WatchEvents.clear();
        g_poseV2CreateRefused = 0;
        g_poseV2CreateRefusedLogs = 0;
        g_poseV2MappedArms = 0;
        g_poseV2RefusedArms = 0;
        g_poseV2RefusedHiddenLogs = 0;
        g_poseV2Signature = {};
    }

    // After every puppet of the tick: the rider and mount pairs for the next
    // tick (from the recorded transforms just placed), the pair check and the
    // time step of the detector, and the POSE_JERK lines (pose_jerk.h).
    void UpdatePoseV2Jerks(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.jerk");
        g_poseV2JerkBodies.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            const bool horse = puppet.packet.kind == replay::entity::EntityKind::Horse;
            if ((!horse && puppet.packet.kind != replay::entity::EntityKind::Ped) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.hasWorld || puppet.visibleState != 1)
                continue;
            replay::jerk::Body body;
            body.packed = puppet.packet.id.Packed();
            body.horse = horse;
            body.world = puppet.lastWorld;
            g_poseV2JerkBodies.push_back(body);
        }
        const std::unordered_map<std::uint32_t, std::uint32_t> before = g_poseV2Mounts.Map();
        g_poseV2Mounts.Update(g_poseV2JerkBodies);
        const auto logPair = [&](std::uint32_t rider, std::uint32_t mount, const char* phase, int seat) {
            if (g_poseV2MountLogs >= 32)
                return;
            ++g_poseV2MountLogs;
            // The seat (-1 the saddle, 0 behind it) and how many riders the
            // mount carries after this tick's update.
            Ctx().log << "POSE_V2_MOUNT_PAIR,phase=" << phase
                << ",rider=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(rider))
                << ",mount=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(mount))
                << ",playhead_ms=" << timeMs << ",seat=" << seat
                << ",riders=" << g_poseV2Mounts.RidersOf(mount) << '\n';
        };
        for (const auto& [rider, mount] : g_poseV2Mounts.Map())
        {
            const auto was = before.find(rider);
            if (was == before.end() || was->second != mount)
                logPair(rider, mount, "enter", g_poseV2Mounts.SeatOf(rider));
        }
        // A leave shows the seat the rider had; a rider who keeps his mount but
        // changes seat (the first one on is alone in front, -1, until a second
        // gets on in front of him) gets a seat line, so the log never keeps a
        // stale seat.
        for (const auto& [rider, mount] : before)
        {
            if (g_poseV2Mounts.MountOf(rider) != mount)
                logPair(rider, mount, "leave", g_poseV2Mounts.PreviousSeatOf(rider));
        }
        for (const std::uint32_t rider : g_poseV2Mounts.SeatChanges())
            logPair(rider, g_poseV2Mounts.MountOf(rider), "seat", g_poseV2Mounts.SeatOf(rider));

        g_poseV2Jerk.EndTick(g_poseV2JerkFindings);
        const replay::jerk::TickInfo& tick = g_poseV2Jerk.LastTick();
        for (const replay::jerk::Finding& finding : g_poseV2JerkFindings)
        {
            if (g_poseV2JerkLogs >= kPoseV2JerkLogCap)
                break;
            ++g_poseV2JerkLogs;
            const replay::entity::ReplayId id = replay::entity::ReplayId::FromPacked(finding.packed);
            const replay::playback::Puppet* puppet = FindPoseV2Puppet(id);
            float velocity = -1.0f;
            if (puppet && puppet->clone && (finding.kind == replay::jerk::Kind::EngineMoved ||
                    finding.kind == replay::jerk::Kind::MountOffset))
            {
                const Vector3 v = ENTITY::GET_ENTITY_VELOCITY(puppet->clone, 0);
                velocity = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            }
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "POSE_JERK,id=%s,kind=%s,playhead_ms=%u,magnitude_m=%.3f,engine_m=%.3f,mount=%s,mount_m=%.3f,"
                "speed_mps=%.1f,engine_velocity_mps=%.2f,wall_step_ms=%.1f,play_step_ms=%.0f,lost_ms=%.1f,causes=%s,"
                "riders=%u\n", // riders a mount_offset shows on
                replay::entity::IdText(id).c_str(), replay::jerk::KindText(finding.kind), finding.playheadMs,
                finding.magnitudeM, finding.engineM,
                finding.mount ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(finding.mount)).c_str() : "none",
                finding.mountM, finding.speedMps, velocity, tick.wallStepMs, tick.playStepMs, tick.lostMs,
                replay::jerk::CauseText(finding.causes).c_str(), finding.riders);
            Ctx().log << line;
        }
    }

    // The detector's totals for the playback (and the worst puppets); the bench
    // regression reads POSE_JERK_SUMMARY.
    void LogPoseV2JerkSummary()
    {
        const replay::jerk::Summary& total = g_poseV2Jerk.Totals();
        if (total.ticks == 0)
            return;
        std::ostream& log = Ctx().log;
        char line[1024]{}; // room for every field
        std::snprintf(line, sizeof(line),
            "POSE_JERK_SUMMARY,ticks=%llu,puppet_ticks=%llu,jerks=%llu,engine_moved=%llu,mount_offset=%llu,time_step=%llu,"
            "stall=%llu,pose_stale=%llu,worst_m=%.3f,worst_id=%s,worst_kind=%s,worst_ms=%u,held_ticks=%llu,capped_ticks=%llu,"
            "slow_ticks=%llu,mount_pairs_entered=%llu,mount_pairs_left=%llu,carried_ticks=%llu,pushed_holds=%llu,logged=%u,carried_physics=%d,"
            "frame_end=%d,physics_filter=%d,engine_moved_placed_back=%llu," // placed back
            "mount_offset_placed_back=%llu,mount_offset_riders=%llu,max_riders_per_mount=%u,shared_mount_ticks=%llu\n",
            static_cast<unsigned long long>(total.ticks), static_cast<unsigned long long>(total.observations),
            static_cast<unsigned long long>(total.jerks),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::EngineMoved)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::MountOffset)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::TimeStep)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::Stall)]),
            static_cast<unsigned long long>(total.byKind[static_cast<int>(replay::jerk::Kind::PoseStale)]),
            total.worstM,
            total.worstPacked ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(total.worstPacked)).c_str() : "none",
            replay::jerk::KindText(total.worstKind), total.worstMs, static_cast<unsigned long long>(total.heldTicks),
            static_cast<unsigned long long>(total.cappedTicks), static_cast<unsigned long long>(total.slowTicks),
            static_cast<unsigned long long>(g_poseV2Mounts.Enters()), static_cast<unsigned long long>(g_poseV2Mounts.Leaves()),
            static_cast<unsigned long long>(g_poseV2CarriedTicks), static_cast<unsigned long long>(g_poseV2PushedHolds),
            g_poseV2JerkLogs, 0, replay::mount::FrameEnd() ? 1 : 0,
            replay::collision::Enabled() ? 1 : 0, static_cast<unsigned long long>(total.placedBack),
            static_cast<unsigned long long>(total.pairPlacedBack), static_cast<unsigned long long>(total.mountOffsetRiders),
            g_poseV2Mounts.MaxRiders(), static_cast<unsigned long long>(g_poseV2Mounts.SharedUpdates()));
        log << line;
        log << "POSE_JERK_CAUSES";
        static const char* const kCauseNames[] = {"slow_tick", "capped", "held", "physics", "not_written", "segment",
            "created", "mounted", "frozen", "attached", "prerender"}; // prerender
        static_assert(sizeof(kCauseNames) / sizeof(kCauseNames[0]) == replay::jerk::kCauseBits, "one name per cause");
        for (int bit = 0; bit < replay::jerk::kCauseBits; ++bit)
            log << ',' << kCauseNames[bit] << '=' << total.byCause[bit];
        log << '\n';
        for (const replay::jerk::PuppetSummary& puppet : g_poseV2Jerk.Worst(8))
        {
            log << "POSE_JERK_PUPPET,id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(puppet.packed))
                << ",jerks=" << puppet.jerks << ",worst_m=" << puppet.worstM << ",worst_ms=" << puppet.worstMs
                << ",worst_kind=" << replay::jerk::KindText(puppet.worstKind);
            for (int kind = 0; kind < static_cast<int>(replay::jerk::Kind::Count); ++kind)
                log << ',' << replay::jerk::KindText(static_cast<replay::jerk::Kind>(kind)) << '=' << puppet.byKind[kind];
            log << '\n';
        }
        log.flush();
    }

    void UpdatePoseV2Playback()
    {
        REPLAY_PROFILE_ZONE("play.pose_v2");
        if (!g_poseV2PlaybackActive || !Ctx().transport.Playing())
            return;
        // The pre-render hook: the rider and mount pairs published for the last
        // pre-render are withdrawn (and its records read back) before this tick
        // can delete a puppet. A drain that timed out (a pre-render detour may
        // still hold a rider or mount pair) skips this tick's lifecycle
        // deletes. EndTick when the tick ends (every return below).
        const bool mountDrained = replay::mount::BeginTick(Ctx().log);
        struct MountTickEnd
        {
            ~MountTickEnd() { replay::mount::EndTick(); }
        } mountTickEnd;

        replay::pose::playback::SuppressPopulationThisFrame();
        replay::trains::SuppressThisFrame();
        const ULONGLONG now = GetTickCount64();
        // Cheap scrub: the event state waits too.
        replay::events::SetStateDeferred(g_poseV2CheapScrub);
        if (now >= g_poseV2NextWorldScanAt)
        {
            g_poseV2NextWorldScanAt = now + kPoseV2WorldScanIntervalMs;
            HidePoseV2World();
        }

        const std::uint32_t timeMs = static_cast<std::uint32_t>(Ctx().transport.PlayheadMs());
        // The T-pose watch's transport gate.
        if (g_tposeWatchOn)
            g_tposeGate.Tick(GetTickCount64(), timeMs, Ctx().transport.Paused(), Ctx().transport.Scrubbing(),
                g_poseV2Preparing, CAM::IS_SCREEN_FADED_IN() == FALSE, Ctx().transport.Rate());
        // The jerk detector's tick (the playhead against the wall clock of the
        // frame; paused, scrub or preparing: nothing checked).
        {
            static const double msPerTick = 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
            g_poseV2Jerk.BeginTick(timeMs, static_cast<double>(replay::prof::Now()) * msPerTick,
                Ctx().transport.Rate(), PoseV2NormalPlay());
            g_poseV2JerkFindings.clear();
        }
        {
            // The alive packets (two vectors each) are copied only when the
            // playhead crosses a lifecycle event or a block (AliveStamp); the
            // membership tests below are binary searches over sorted ids
            // instead of any_of in any_of.
            REPLAY_PROFILE_ZONE("play.lifecycle.alive");
            const std::uint64_t stamp = Ctx().clip.lifecycle.AliveStamp(timeMs);
            if (stamp != g_poseV2AliveStamp || Ctx().clip.lifecycle.BlockCount() != g_poseV2AliveBlocks)
            {
                g_poseV2AliveStamp = stamp;
                g_poseV2AliveBlocks = Ctx().clip.lifecycle.BlockCount();
                Ctx().clip.lifecycle.AliveAt(timeMs, g_poseV2Alive);
                g_poseV2AliveIds.clear();
                for (const replay::entity::CreatePacket& packet : g_poseV2Alive)
                    g_poseV2AliveIds.push_back(packet.id.Packed());
                std::sort(g_poseV2AliveIds.begin(), g_poseV2AliveIds.end());
                ++g_poseV2AliveCopies;
            }
        }
        const auto isAlive = [&](replay::entity::ReplayId id) {
            return std::binary_search(g_poseV2AliveIds.begin(), g_poseV2AliveIds.end(), id.Packed());
        };

        {
            // Deletes and queues get a zone of their own (outside every zone
            // they would only show in play.pose_v2).
            REPLAY_PROFILE_ZONE("play.lifecycle");
            for (std::size_t index = mountDrained ? Ctx().puppets.list.size() : 0; index-- > 0;)
            {
                if (isAlive(Ctx().puppets.list[index].packet.id))
                    continue;
                DeletePoseV2Puppet(Ctx().puppets.list[index], "lifecycle");
                Ctx().puppets.list.erase(Ctx().puppets.list.begin() + static_cast<std::ptrdiff_t>(index));
            }

            static std::vector<std::uint32_t> puppetIds;
            puppetIds.clear();
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                puppetIds.push_back(puppet.packet.id.Packed());
            std::sort(puppetIds.begin(), puppetIds.end());
            for (const replay::entity::CreatePacket& packet : g_poseV2Alive)
            {
                const std::uint32_t packed = packet.id.Packed();
                const auto at = std::lower_bound(puppetIds.begin(), puppetIds.end(), packed);
                if (at != puppetIds.end() && *at == packed)
                    continue;
                QueuePoseV2Puppet(packet);
                puppetIds.insert(at, packed);
            }
        }

        std::uint32_t creates = 0;
        g_poseV2Unposed.BeginTick();
        const bool blinkJudge = BeginPoseV2BlinkTick(timeMs); // puppet_blink.h
        g_poseV2LodPrevPlayhead = g_poseV2LodLastPlayhead;
        g_poseV2LodLastPlayhead = timeMs;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind == replay::entity::EntityKind::Weapon ||
                puppet.packet.kind == replay::entity::EntityKind::Prop ||
                puppet.packet.kind == replay::entity::EntityKind::TrainCar) // replay_trains
            {
                continue;
            }
            // The blink detector judges the bodies (puppet_blink.h).
            const bool blinkBody = blinkJudge &&
                (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                    puppet.packet.kind == replay::entity::EntityKind::Horse);
            if (puppet.state == replay::playback::PuppetState::Loading ||
                puppet.state == replay::playback::PuppetState::Dressing)
            {
                // The settle's budget; the start batch (ClipStartStagger)
                // shared by every kind.
                if (AdvancePoseV2Puppet(puppet, timeMs, creates < g_poseV2CreateBudget && replay::playback::StartBatchLeft()))
                {
                    ++creates;
                    replay::playback::StartBatchUse();
                }
                if (blinkBody)
                    JudgePoseV2BlinkAbsent(puppet, isAlive(puppet.packet.id), timeMs);
                continue;
            }
            if (puppet.state != replay::playback::PuppetState::Ready ||
                !puppet.clone || !replay::entities::Exists(puppet.clone))
            {
                if (blinkBody)
                    JudgePoseV2BlinkAbsent(puppet, isAlive(puppet.packet.id), timeMs);
                continue;
            }
            const std::uint32_t recordBeforeSwitch = puppet.recordId; // jerk cause hint
            ApplyPoseV2Lod(puppet); // before the arm
            {
                REPLAY_PROFILE_ZONE("play.pose.segment");
                SwitchPoseV2Segment(puppet, timeMs);
                ApplyPoseV2Appearance(puppet, timeMs);
                StepPoseV2ShopCheck(puppet, timeMs);
                RefreshPoseV2Override(puppet, timeMs);
            }

            const std::uint32_t recordId = puppet.recordId;
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            replay::pose::capture::TrackSpan(recordId, firstMs, lastMs);
            // Hidden only before the entity's first record and after its
            // last: between two segments the pose holds.
            const bool beforeFirst = timeMs < firstMs && recordId == puppet.packet.id.Packed();
            // One capture frame of slack: the timeline ends a frame after the
            // last sample, and the puppet stays up to the very end.
            constexpr std::uint32_t kTrackEndSlackMs = 100;
            const bool afterLast = timeMs > lastMs + kTrackEndSlackMs &&
                !PoseV2RecordHasSuccessor(puppet.packet.id, recordId);
            // The recorded player's puppet is never hidden: outside his
            // record's span it holds the nearest pose (player_hold.h, QA 224).
            const bool isPlayer = g_poseV2PlayerPacked && puppet.packet.id.Packed() == g_poseV2PlayerPacked;
            const bool inTrack = isPlayer || (!beforeFirst && !afterLast);
            // The pose comes from the record matching the armed skeleton
            // (the segment's, once the skeleton has its bones).
            const std::uint32_t poseRecord =
                puppet.overrideSlot >= 0 ? puppet.armedRecordId : recordId;
            replay::pose::WorldTransform world{};
            bool sampled = false;
            // The same record at the same playhead into the same slot as this
            // puppet's last sample (pause, the start hold, the clip end, slow
            // motion under 1 ms a tick): the sample would give the same bits
            // and leave the track as it is, and the slot holds them already
            // (playback_cache.h; tests/playback_perf_tests.cpp). The tick takes
            // what it took from that sample and publishes nothing.
            const bool reused = puppet.sampleReuse.Matches(poseRecord, timeMs, puppet.overrideSlot);
            if (reused)
            {
                sampled = puppet.sampleReuse.sampled;
                world = puppet.sampleReuse.world;
                ++g_poseV2PoseSampleReuses;
            }
            else
            {
                REPLAY_PROFILE_ZONE("play.pose.sample");
                sampled = replay::pose::capture::SampleTrack(poseRecord, timeMs, g_poseV2Sample) &&
                    SampleWorldTransform(g_poseV2Sample, world);
                puppet.sampleReuse.Begin(poseRecord, timeMs, puppet.overrideSlot, sampled, world,
                    sampled && !g_poseV2Sample.objects.empty() ? g_poseV2Sample.objects[0].translation : nullptr);
                ++g_poseV2PoseSamples;
            }

            // A ped or horse whose skeleton the override cannot write
            // (re-arming after a rebuild, no record for its bones yet) shows
            // its bind pose, a T-pose: hidden until armed. Also once arming
            // gave up (no record and no skeleton map for its bones): it stays
            // hidden instead of showing the T-pose, counted by the puppet watch
            // as arm_refused, with a line per entity every 5 s (no session
            // cap).
            const bool body = puppet.packet.kind == replay::entity::EntityKind::Ped ||
                puppet.packet.kind == replay::entity::EntityKind::Horse;
            const bool unposed = body && puppet.overrideSlot < 0;
            // One line per episode that lasts past the rule (puppet_watch.h
            // UnposedRule), not one per tick: holes of 1 to 20 ticks at
            // creation and at a segment switch are only counted in
            // POSE_V2_UNPOSED_SUMMARY. An arm given up is not an unposed
            // episode (POSE_V2_ARM_REFUSED_HIDDEN below).
            if (body)
            {
                replay::watch::UnposedReport report;
                if (g_poseV2Unposed.Note(puppet.packet.id.Packed(), unposed && !puppet.armFailureLogged, GetTickCount64(),
                        timeMs, report) &&
                    g_poseV2UnposedHideLogs < 64)
                {
                    ++g_poseV2UnposedHideLogs;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
                    const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
                    float sx = 0.0f, sy = 0.0f;
                    const bool onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
                    Ctx().log << "POSE_V2_UNPOSED_HIDDEN,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",kind=" << replay::entity::KindText(puppet.packet.kind)
                        << ",cause=" << replay::watch::UnposedCauseText(report.cause) << ",ms=" << report.hiddenMs
                        << ",ticks=" << report.ticks << ",since_playhead_ms=" << report.sincePlayheadMs
                        << ",camera_m="
                        << static_cast<int>(std::sqrt((at.x - camera.x) * (at.x - camera.x) +
                               (at.y - camera.y) * (at.y - camera.y) + (at.z - camera.z) * (at.z - camera.z)))
                        << ",on_screen=" << (onScreen ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
                }
            }
            if (unposed && puppet.armFailureLogged && GetTickCount64() >= puppet.refusedLogAt)
            {
                puppet.refusedLogAt = GetTickCount64() + 5000;
                ++g_poseV2RefusedHiddenLogs;
                Ctx().log << "POSE_V2_ARM_REFUSED_HIDDEN,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",kind=" << replay::entity::KindText(puppet.packet.kind)
                    << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec
                    << ",error=" << puppet.armError << ",clone_bones=" << puppet.failedBones
                    << ",track_bones=" << puppet.boneCount << ",in_track=" << (inTrack ? 1 : 0)
                    << ",playhead_ms=" << timeMs << '\n';
            }
            // And visible in the recording (a mission's hidden ped stays
            // hidden), and not a body never animated in the recording and
            // recorded in its bind pose (clips without PVIS).
            const bool neverHidden = body && sampled && inTrack && !unposed &&
                PoseV2NeverAnimatedHidden(puppet, poseRecord, timeMs);
            replay::playerhold::Gate holdGate;
            holdGate.player = isPlayer && body;
            holdGate.sampled = sampled;
            holdGate.beforeFirst = beforeFirst;
            holdGate.afterLast = afterLast;
            holdGate.unposed = unposed;
            holdGate.recordedVisible = PoseV2RecordedVisible(puppet.packet.id.Packed(), timeMs);
            holdGate.neverAnimated = neverHidden;
            const replay::playerhold::Verdict holdVerdict = replay::playerhold::Decide(holdGate);
            const bool recordedVisible = holdGate.player ? true : (holdGate.recordedVisible && !neverHidden);
            const int wantVisible = holdVerdict.visible ? 1 : 0;
            if (holdGate.player)
            {
                // A line when what keeps him drawn changes (held at an end of
                // his record, or shown against the recorded visibility).
                const int holdState = (holdVerdict.held ? 1 : 0) | (holdVerdict.keptVisible ? 2 : 0) |
                    (unposed ? 4 : 0) | (sampled ? 0 : 8);
                if (holdState != puppet.playerHoldState && g_poseV2PlayerHoldLogs < kPoseV2PlayerHoldLogCap)
                {
                    ++g_poseV2PlayerHoldLogs;
                    Ctx().log << "POSE_V2_PLAYER_HOLD,id=" << replay::entity::IdText(puppet.packet.id)
                              << ",held=" << (holdVerdict.held ? (beforeFirst ? "first" : "last") : "none")
                              << ",carried_by="
                              << (holdVerdict.held && beforeFirst && g_poseV2PlayerFirstMount
                                         ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(g_poseV2PlayerFirstMount))
                                         : std::string("none"))
                              << ",kept_visible=" << (holdVerdict.keptVisible ? 1 : 0)
                              << ",recorded_visible=" << (holdGate.recordedVisible ? 1 : 0)
                              << ",never_animated=" << (neverHidden ? 1 : 0) << ",unposed=" << (unposed ? 1 : 0)
                              << ",sampled=" << (sampled ? 1 : 0) << ",visible=" << wantVisible
                              << ",first_ms=" << firstMs << ",last_ms=" << lastMs << ",playhead_ms=" << timeMs << '\n';
                }
                puppet.playerHoldState = holdState;
            }
            if (body && g_tposeWatchOn)
                WatchPoseV2TPose(puppet, wantVisible != 0, timeMs);
            if (puppet.visibleState != wantVisible && sampled && inTrack && !unposed &&
                g_poseV2RecordedVisibilityLogs < 40)
            {
                ++g_poseV2RecordedVisibilityLogs;
                Ctx().log << "POSE_V2_RECORDED_VISIBILITY,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",visible=" << (recordedVisible ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
            }
            if (puppet.visibleState != wantVisible)
            {
                replay::pose::playback::SetCloneVisible(puppet.clone, wantVisible != 0);
                puppet.visibleState = wantVisible;
            }
            if (blinkBody)
                JudgePoseV2BlinkReady(puppet, isAlive(puppet.packet.id),
                    recordedVisible, inTrack, sampled, poseRecord,
                    recordBeforeSwitch != puppet.recordId, timeMs != g_poseV2LodPrevPlayhead, timeMs);
            bool lodScriptPose = false; // the pose written from here this tick
            if (body && puppet.lod.clone == puppet.clone)
            {
                // A tick that finds no pose write since the last one (the
                // finalizer skipped the puppet: the frame in between showed an
                // old pose, the low LOD case; puppet_lod.h) forces the ped's
                // update for the next. The engine's own writes (the
                // finalizer's) apart from the script thread's (ApplyOverrideNow
                // also counts in applies): a puppet the engine skips gets the
                // pose from the script thread every tick (lodScriptPose, after
                // the publish below).
                std::uint64_t writes = 0;
                std::uint64_t own = 0;
                if (puppet.overrideSlot >= 0)
                {
                    const replay::pose::capture::OverrideStats stats =
                        replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(puppet.overrideSlot));
                    writes = stats.applies + stats.altApplies + stats.guardApplies + stats.prefixApplies;
                    own = writes >= stats.scriptWrites ? writes - stats.scriptWrites : 0;
                }
                const bool moved = PoseV2NormalPlay() && timeMs != g_poseV2LodPrevPlayhead;
                const replay::lod::Verdict lodVerdict = replay::lod::Observe(puppet.lod, g_poseV2LodConfig,
                    puppet.overrideSlot >= 0, own, writes, moved, wantVisible != 0);
                if (lodVerdict.force)
                    PED::FORCE_PED_AI_AND_ANIMATION_UPDATE(puppet.clone, TRUE, FALSE);
                lodScriptPose = lodVerdict.scriptPose && puppet.overrideSlot >= 0;
                if (lodVerdict.skipStart && g_poseV2AnimSkipLogs < kPoseV2LowLodLogCap)
                {
                    ++g_poseV2AnimSkipLogs;
                    LogPoseV2AnimSkip(puppet, timeMs);
                }
                if (lodVerdict.episode && g_poseV2LowLodLogs < kPoseV2LowLodLogCap)
                {
                    const BOOL onScreen = ENTITY::IS_ENTITY_ON_SCREEN(puppet.clone);
                    if (onScreen)
                    {
                        ++g_poseV2LowLodLogs;
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
                        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
                        const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
                        Ctx().log << "POSE_V2_PUPPET_LOW_LOD,id=" << replay::entity::IdText(puppet.packet.id)
                                  << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",on_screen=1"
                                  << ",camera_m=" << static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz))
                                  << ",stale_ticks=" << puppet.lod.staleTicks << ",forced=" << (lodVerdict.force ? 1 : 0)
                                  << ",lod_multiplier=" << PED::_GET_PED_LOD_MULTIPLIER(puppet.clone)
                                  << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            if (!sampled)
            {
                puppet.sampleReuse.Keep(); // nothing to publish; the same sample fails again
                // The detector: a puppet left without a sample while its entity
                // has one recorded at the playhead plays the wrong record.
                const std::uint32_t packed = puppet.packet.id.Packed();
                if (inTrack && std::find(g_poseV2RecordUnsampled.begin(), g_poseV2RecordUnsampled.end(), packed) ==
                        g_poseV2RecordUnsampled.end())
                {
                    replay::pose::WorldTransform recorded{};
                    if (PoseV2RecordedWorldAt(puppet.packet.id, timeMs, recorded))
                    {
                        g_poseV2RecordUnsampled.push_back(packed);
                        if (g_poseV2RecordUnsampled.size() <= 32)
                            Ctx().log << "POSE_V2_RECORD_UNSAMPLED,id=" << replay::entity::IdText(puppet.packet.id)
                                      << ",kind=" << replay::entity::KindText(puppet.packet.kind) << ",record=0x" << std::hex
                                      << poseRecord << ",segment_record=0x"
                                      << replay::entity::PlaybackRecordAt(Ctx().clip.segments, puppet.packet.id, timeMs,
                                             PoseV2RecordSampled)
                                      << std::dec << ",armed=" << (puppet.overrideSlot >= 0 ? 1 : 0)
                                      << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                continue;
            }
            // The player before his record's first frame, on the horse he
            // rides at it: carried by that horse (player_hold.h Carry).
            if (isPlayer && beforeFirst && g_poseV2PlayerFirstMount)
            {
                replay::pose::WorldTransform mountNow{};
                if (PoseV2RecordedWorldAt(replay::entity::ReplayId::FromPacked(g_poseV2PlayerFirstMount), timeMs, mountNow))
                    world = replay::playerhold::Carry(g_poseV2PlayerRiderFirst, g_poseV2PlayerMountFirst, mountNow);
            }
            puppet.lastWorld = world;
            puppet.hasWorld = true;
            // Seat evidence: where the engine left the puppet since the last
            // tick, against the recorded pose and the vehicle puppet.
            if (puppet.seatFollowLeft > 0)
            {
                --puppet.seatFollowLeft;
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
                const replay::playback::Puppet* vehicle = FindPoseV2Puppet(puppet.attachment.parent);
                const Vector3 vehicleAt = vehicle && vehicle->clone
                    ? ENTITY::GET_ENTITY_COORDS(vehicle->clone, TRUE, FALSE)
                    : Vector3{};
                const auto distance = [](const Vector3& a, float bx, float by, float bz) {
                    return std::sqrt((a.x - bx) * (a.x - bx) + (a.y - by) * (a.y - by) + (a.z - bz) * (a.z - bz));
                };
                char line[200]{};
                std::snprintf(line, sizeof(line),
                    "POSE_V2_SEAT_FOLLOW,child=%s,left=%u,drift_m=%.3f,vehicle_m=%.3f,attached=%d,playhead_ms=%u\n",
                    replay::entity::IdText(puppet.packet.id).c_str(), puppet.seatFollowLeft,
                    distance(at, world.position.x, world.position.y, world.position.z),
                    vehicle && vehicle->clone ? distance(at, vehicleAt.x, vehicleAt.y, vehicleAt.z) : -1.0f,
                    ENTITY::IS_ENTITY_ATTACHED(puppet.clone) ? 1 : 0, timeMs);
                std::ostream& log = Ctx().log;
                log << line;
                log.flush();
            }
            // Rider of a horse puppet (last tick's pairs) or seated in a
            // vehicle: carried. Its standing capsule is not what the engine had
            // while it rode (see pose_jerk.h), so it never becomes a physics
            // body; the horse or the vehicle touches the world.
            const std::uint32_t packedId = puppet.packet.id.Packed();
            const std::uint32_t mountId = puppet.packet.kind == replay::entity::EntityKind::Ped
                ? g_poseV2Mounts.MountOf(packedId) : 0u;
            const bool carried = mountId != 0 ||
                (puppet.attached && puppet.attachment.kind == replay::entity::AttachmentKind::VehicleSeat);
            const bool harnessed = puppet.attached && puppet.attachment.kind == replay::entity::AttachmentKind::DraftHarness;
            if (body && PoseV2NormalPlay() && wantVisible && !harnessed)
            {
                // Where the engine left the clone since our last placement, the
                // pose writes since the last tick, against the recording
                // (pose_jerk.h). One coords read per shown body per tick.
                REPLAY_PROFILE_ZONE("play.jerk");
                replay::jerk::Observation observation;
                observation.packed = packedId;
                observation.mount = mountId;
                observation.playheadMs = timeMs;
                observation.recorded = world.position;
                observation.placed = puppet.placed;
                observation.placedAt = puppet.placedWorld.position;
                float engineAt[3]{};
                // The entity's position read from memory (GET_ENTITY_COORDS'
                // own reader, entity_cache.h).
                if (puppet.placed && replay::entities::Position(puppet.clone, engineAt))
                {
                    observation.engine = {engineAt[0], engineAt[1], engineAt[2]};
                    observation.hasEngine = true;
                }
                // The root bone of this puppet's sample, as kept with it (the
                // shared sample buffer holds another puppet's after a reused
                // tick).
                if (puppet.sampleReuse.hasRoot)
                {
                    const float* root = puppet.sampleReuse.root;
                    observation.rootObject = {root[0], root[1], root[2]};
                    observation.hasRoot = true;
                }
                if (puppet.overrideSlot >= 0)
                {
                    const replay::pose::capture::OverrideStats stats =
                        replay::pose::capture::OverrideSnapshot(static_cast<std::uint32_t>(puppet.overrideSlot));
                    observation.overrideWrites = stats.applies + stats.altApplies + stats.guardApplies + stats.prefixApplies;
                    observation.hasOverride = true;
                }
                const bool frozen = replay::entities::Frozen(puppet.clone); // one native per frame, shared with the place
                observation.causes = (frozen ? replay::jerk::kCauseFrozen : replay::jerk::kCausePhysics) |
                    (recordBeforeSwitch != puppet.recordId ? replay::jerk::kCauseSegment : 0u);
                if (mountId && replay::mount::CarriedLastFrame(packedId)) // the pre-render hook
                    observation.causes |= replay::jerk::kCausePreRender;
                if (replay::mount::FrameEnd() && replay::mount::Installed())
                    observation.frameEndRefuseM = replay::mount::PlaceThresholds{}.refuseM;
                const std::size_t before = g_poseV2JerkFindings.size();
                g_poseV2Jerk.Observe(observation, g_poseV2JerkFindings);
                for (std::size_t n = before; n < g_poseV2JerkFindings.size(); ++n)
                {
                    // A physics body the engine pushed off its placement goes
                    // out of physics for a while: the next frames render where
                    // the recording is. Not under the physics filter: the
                    // filtered body touches only plants, the end-of-frame write
                    // puts it back, and the hold's freeze and unfreeze would
                    // push the body about once a second (coming back inside the
                    // ground).
                    if (g_poseV2JerkFindings[n].kind == replay::jerk::Kind::EngineMoved && !frozen &&
                        !replay::collision::Enabled())
                    {
                        g_poseV2PushedUntil[packedId] = now + kPoseV2PushedHoldMs;
                        ++g_poseV2PushedHolds;
                    }
                }
            }
            {
                REPLAY_PROFILE_ZONE("play.pose.place");
                const bool same = puppet.placed &&
                    std::memcmp(&world, &puppet.placedWorld, sizeof(world)) == 0;
                bool physicsBody = false; // placed every tick (the engine may move it)
                // Vegetation: bushes and grass react only to an active physical
                // body; a frozen puppet, with or without collision, is a ghost
                // to them, and an unfrozen one with collision fights the ground
                // and the other puppets. So physics only while the puppet walks
                // or runs upright; then no gravity (the pose still places it
                // every frame) and no collision with the other puppets, so it
                // touches the world and its plants only. Standing, down or dead
                // it is frozen without collision.
                if (puppet.packet.kind == replay::entity::EntityKind::Ped || puppet.packet.kind == replay::entity::EntityKind::Horse)
                {
                    const auto engine = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
                    const float speedSquared = replay::vegetation::PublishedSpeedSquared(engine);
                    // Never a carried ped, and not a body the engine pushed off
                    // its placement a moment ago.
                    const auto pushed = g_poseV2PushedUntil.find(packedId);
                    const bool pushedHold = pushed != g_poseV2PushedUntil.end() && now < pushed->second;
                    if (carried && PoseV2NormalPlay())
                        ++g_poseV2CarriedTicks;
                    // The head bone native only when it decides (a moving, not
                    // carried, not pushed ped), not for every standing ped
                    // every tick.
                    const bool moving = PoseV2NormalPlay() && speedSquared > 0.25f && !carried && !pushedHold;
                    bool upright = true;
                    if (moving && puppet.packet.kind == replay::entity::EntityKind::Ped)
                    {
                        ++g_poseV2HeadBoneCalls;
                        const Vector3 head = PED::GET_PED_BONE_COORDS(puppet.clone, 21030, 0.0f, 0.0f, 0.0f); // SKEL_Head
                        upright = head.z - world.position.z > 0.45f; // origin at the pelvis, head ~0.7 m above standing
                    }
                    const bool active = moving && upright;
                    physicsBody = active;
                    const bool wasActive = !replay::entities::Frozen(puppet.clone);
                    if (active != wasActive)
                    {
                        // The original include back before the collision
                        // natives see the instance (no-op off).
                        if (!active)
                            replay::collision::Release(replay::entities::AddressOf(puppet.clone), packedId, Ctx().log);
                        ENTITY::SET_ENTITY_HAS_GRAVITY(puppet.clone, FALSE);
                        ENTITY::SET_ENTITY_COLLISION(puppet.clone, active ? TRUE : FALSE, active ? TRUE : FALSE);
                        ENTITY::FREEZE_ENTITY_POSITION(puppet.clone, active ? FALSE : TRUE);
                        replay::entities::NoteFrozen(puppet.clone, !active);
                    }
                    // The active body collides only with plants
                    // (puppet_collision.h; checked every tick: a reinsert or a
                    // native may put the include back). While it holds, no pair
                    // with another puppet can form either, so the pair-by-pair
                    // natives below are not needed.
                    const bool filtered = active &&
                        replay::collision::Apply(replay::entities::AddressOf(puppet.clone), packedId, Ctx().log);
                    if (active && !filtered)
                    {
                        // Every other ped and horse puppet when the body turns
                        // active, then only those close enough to touch it
                        // (placements known here, no native): the flag is
                        // dropped by the engine once the two no longer overlap,
                        // and a far one cannot be reached this tick. Every
                        // puppet every tick would be ~765 natives with 15
                        // walking in town.
                        constexpr float kNoCollisionReachM = 5.0f;
                        const bool turnedOn = !wasActive;
                        for (const replay::playback::Puppet& other : Ctx().puppets.list)
                        {
                            if (&other == &puppet || !other.clone || (other.packet.kind != replay::entity::EntityKind::Ped &&
                                    other.packet.kind != replay::entity::EntityKind::Horse))
                                continue;
                            if (!turnedOn && (other.placed || other.hasWorld))
                            {
                                const auto& at = other.placed ? other.placedWorld.position : other.lastWorld.position;
                                const float dx = at.x - world.position.x;
                                const float dy = at.y - world.position.y;
                                const float dz = at.z - world.position.z;
                                if (dx * dx + dy * dy + dz * dz > kNoCollisionReachM * kNoCollisionReachM)
                                    continue;
                            }
                            ++g_poseV2NoCollisionCalls;
                            ENTITY::SET_ENTITY_NO_COLLISION_ENTITY(puppet.clone, other.clone, TRUE);
                        }
                    }
                }
                const auto vegEntity = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
                replay::vegetation::Publish(vegEntity,
                    world.position.x, world.position.y, world.position.z, GetTickCount64(), PoseV2NormalPlay());
                // Paused (or held by the editor), the transform does not change
                // and the placement would wait up to kPlaceRefreshFrames,
                // showing whatever the engine does to the clone meanwhile (a
                // rider paused while mounting slides away and snaps back). Out
                // of normal play a clone the engine moved off its placement is
                // put back at once (one coords read per held puppet, only
                // then).
                bool drifted = false;
                if (same && !PoseV2NormalPlay())
                {
                    float at[3]{}; // memory (entity_cache.h)
                    replay::entities::Position(puppet.clone, at);
                    const float dx = at[0] - world.position.x;
                    const float dy = at[1] - world.position.y;
                    const float dz = at[2] - world.position.z;
                    const float drift = std::sqrt(dx * dx + dy * dy + dz * dz);
                    drifted = drift > 0.02f;
                    if (drifted && g_poseV2PlaceDriftLogs < 24)
                    {
                        ++g_poseV2PlaceDriftLogs;
                        Ctx().log << "POSE_V2_PLACE_DRIFT,id=" << replay::entity::IdText(puppet.packet.id) << ",drift_m=" << drift
                            << ",frozen=" << (ENTITY::_IS_ENTITY_FROZEN(puppet.clone) ? 1 : 0)
                            << ",attached=" << (ENTITY::IS_ENTITY_ATTACHED(puppet.clone) ? 1 : 0)
                            << ",held_frames=" << puppet.placeHeldFrames << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                if (!same || drifted || physicsBody || ++puppet.placeHeldFrames >= replay::playback::kPlaceRefreshFrames)
                {
                    // The placement marks every effect attached to the puppet
                    // as relocated (0x143C180 through the coords native); the
                    // step tells the end of the tick whether that was a jump.
                    const float placedFrom[3] = {puppet.placedWorld.position.x, puppet.placedWorld.position.y,
                        puppet.placedWorld.position.z};
                    const float placedTo[3] = {world.position.x, world.position.y, world.position.z};
                    const float stepM = puppet.placed ? replay::fxmotion::StepMetres(placedFrom, placedTo) : -1.0f;
                    replay::events::FxBeforePlace(vegEntity);
                    {
                        replay::fx::PlaceScope placeScope(reinterpret_cast<const void*>(vegEntity)); // FxPuppetNoWarpDetour
                        replay::pose::playback::PlaceClone(puppet.clone, world);
                    }
                    replay::events::FxAfterPlace(vegEntity, stepM);
                    puppet.placedWorld = world;
                    puppet.placed = true;
                    puppet.placeHeldFrames = 0;
                }
            }
            {
                // Zones of their own, so the profile splits play.pose_v2.
                REPLAY_PROFILE_ZONE("play.pose.scorched");
                ApplyPoseV2Scorched(puppet, timeMs);
            }
            if (puppet.overrideSlot >= 0 && puppet.packet.kind != replay::entity::EntityKind::Vehicle)
            {
                REPLAY_PROFILE_ZONE("play.pose.write_check");
                CheckPoseV2Write(puppet, timeMs); // before the publish
            }
            if (puppet.overrideSlot < 0 && !reused)
                puppet.sampleReuse.Keep(); // nothing published, the transform alone
            if (puppet.overrideSlot >= 0 && !reused) // a reused tick's slot holds its pose
            {
                REPLAY_PROFILE_ZONE("play.pose.publish");
                // Swapped into the slot, not copied (the sample is not read
                // again this tick). A mapped puppet gets the record's pose
                // through its skeleton map (its own bone count), and no
                // alternate: no record has its bones. The hat fitted again
                // where the recording has the bind pose (hat_chain.h).
                if (!puppet.mapped && puppet.packet.kind == replay::entity::EntityKind::Ped)
                {
                    PreparePoseV2HatFix(puppet, puppet.armedRecordId, timeMs, g_poseV2Sample);
                    FixPoseV2Hat(puppet, g_poseV2Sample);
                }
                const bool published = puppet.mapped
                    ? PublishPoseV2Mapped(puppet, g_poseV2Sample)
                    : replay::pose::capture::PublishOverrideSample(
                          puppet.overrideSlot, g_poseV2Sample, puppet.armedBones);
                // Near a skeleton change the neighbouring record's pose is
                // kept ready for the frame the engine rebuilds the skeleton
                // after this tick (in place, same pointer, new bone count).
                std::uint32_t neighbourBones = 0;
                const std::uint32_t neighbour = puppet.mapped ? 0u : PoseV2NeighbourRecord(
                    puppet.packet.id, timeMs, puppet.armedBones, neighbourBones);
                const bool alternate = neighbour &&
                    replay::pose::capture::SampleTrack(neighbour, timeMs, g_poseV2AltSample) &&
                    replay::pose::capture::PublishOverrideAlternate(
                        puppet.overrideSlot,
                        g_poseV2AltSample.locals.data(),
                        g_poseV2AltSample.objects.data(),
                        neighbourBones);
                if (!alternate)
                    replay::pose::capture::ClearOverrideAlternate(puppet.overrideSlot);
                // While the alternate is ready, any finalizer call writes it
                // into this skeleton once the engine rebuilt it.
                if (puppet.packet.kind != replay::entity::EntityKind::Vehicle)
                    replay::pose::capture::SetOverrideRebuildGuard(
                        static_cast<std::uint32_t>(puppet.overrideSlot), alternate);
                // The finalizer never visits vehicle skeletons (live:
                // detour_calls=0, script_polls only).
                if (puppet.packet.kind == replay::entity::EntityKind::Vehicle)
                    replay::pose::capture::ApplyOverrideNow(puppet.overrideSlot);
                // Only the pose went to the slot (a vehicle's late writes, its
                // script-thread write and an alternate pose are done again
                // every tick).
                if (published && !alternate && puppet.packet.kind != replay::entity::EntityKind::Vehicle)
                    puppet.sampleReuse.Keep();
                // A body the engine skipped lately (puppet_lod.h): the pose
                // just published, written now (the frame shows it even when the
                // engine does not update the puppet; when it does, its
                // finalizer writes the same pose).
                if (lodScriptPose)
                    replay::pose::capture::ApplyOverrideNow(puppet.overrideSlot);
            }
        }
        if (blinkJudge)
            EndPoseV2BlinkTick(); // the episodes of bodies not judged this tick end

#if defined(REDEMPTION_DEV)
        replay::objects::BenchWatchStands("tick_start", timeMs);
#endif
        UpdatePoseV2Jerks(timeMs);
        UpdatePoseV2AnimLod();
        // The horses' muscles: the recorded shader variables published for the
        // detour that writes them before the shader reads them (SHVR).
        replay::muscles::TickPlay(timeMs);
        // The pre-render hook: every puppet is placed and the pairs are this
        // tick's, published for the frame's pre-render.
        replay::mount::Publish(Ctx().puppets, timeMs, true);
        UpdatePoseV2TrailFocus(timeMs);
        UpdatePoseV2Attachments(timeMs);
        replay::objects::UpdateWeaponPuppets(timeMs, creates);
        replay::objects::PublishStrapOwned();
        replay::objects::UpdatePropPuppets(timeMs, creates);
        replay::trains::UpdatePuppets(timeMs, creates);
        {
            REPLAY_PROFILE_ZONE("play.vehicle_state");
            PlayPoseV2VehicleDoors(timeMs); // cheap, also while scrubbing
            // Cheap scrub: a vehicle keeps the damage it had (the rebuilds
            // delete and create it again); the release applies the state at the
            // playhead once (these are all states at a time).
            if (!g_poseV2CheapScrub)
            {
                PlayPoseV2VehicleWindows(timeMs);
                VerifyPoseV2VehicleLook(timeMs);
                PlayPoseV2VehicleParts(timeMs);
                PlayPoseV2VehicleFrags(timeMs);
                PlayPoseV2VehicleBurnt(timeMs);
            }
            {
                REPLAY_PROFILE_ZONE("play.burn_level");
                PlayPoseV2BurnLevel(timeMs);
            }
            {
                REPLAY_PROFILE_ZONE("play.object_look");
                replay::objlook::PlayPuppets(timeMs);
            }
        }
        PlayPoseV2FireChar(timeMs);
        PlayPoseV2Ropes(timeMs);
        replay::events::Play(timeMs);
        // The WFXI requests, after Play and outside its audio scope
        // (ReplayEmitScope).
        replay::events::PlayWeaponFxIntents(timeMs);
        // After every placement and the events.
        replay::events::FxEndOfPlaybackTick(PoseV2NormalPlay());
#if defined(REDEMPTION_DEV)
        replay::objects::BenchWatchStands("tick_end", timeMs);
#endif
        g_poseV2Counters.creates += creates;
        UpdatePoseV2SettleState(timeMs);
        UpdatePoseV2PuppetWatch(timeMs);

        if (g_poseV2Preparing)
        {
            // A ped, horse or vehicle built in this tick is still hidden
            // (visibility and placement come on its first Ready pass, the next
            // tick). A vehicle skips Dressing, so without this the hold would
            // release in the very tick the vehicles were created and the first
            // frame would play without them.
            const bool building = std::any_of(Ctx().puppets.list.begin(), Ctx().puppets.list.end(),
                [](const replay::playback::Puppet& puppet) {
                    const bool body = puppet.packet.kind == replay::entity::EntityKind::Ped ||
                        puppet.packet.kind == replay::entity::EntityKind::Horse ||
                        puppet.packet.kind == replay::entity::EntityKind::Vehicle;
                    return (puppet.state == replay::playback::PuppetState::Loading && !puppet.waitingForParent) ||
                        puppet.state == replay::playback::PuppetState::Dressing ||
                        (body && puppet.state == replay::playback::PuppetState::Ready && puppet.visibleState < 0) ||
                        (puppet.packet.kind == replay::entity::EntityKind::Vehicle && replay::objects::VehicleWheelsPending(puppet));
                });
            if (!building || GetTickCount64() > g_poseV2PrepareDeadline)
            {
                g_poseV2Preparing = false;
                std::ostream& log = Ctx().log;
                log << "POSE_V2_PLAYBACK_READY"
                    << ",puppets=" << Ctx().puppets.list.size()
                    << ",timed_out=" << (building ? 1 : 0)
                    << ",prepare_ms=" << (GetTickCount64() + kPoseV2PrepareTimeoutMs - g_poseV2PrepareDeadline)
                    << '\n';
                log.flush();
                replay::hud::ShowObjective(replay::keys::Fill("Redemption Editor: PoseTrack v2 +1x ({key_play_pause} pause, {key_frame_back} "
                                                              "{key_frame_forward} scrub, {key_free_cam} free camera)",
                    replay::keys::Current()).c_str());
            }
        }
    }

    void StopPoseV2Playback()
    {
        LogPoseV2JerkSummary();
        // The pre-render hook: withdrawn before the puppets go;
        // POSE_V2_MOUNT_PRERENDER_SUMMARY.
        replay::mount::EndPlayback(Ctx().log);
        EndPoseV2AnimLod(); // POSE_V2_PUPPET_ANIM_LOD, the directors withdrawn
        replay::muscles::EndPlay(); // MUSCLE_REPLAY_END, the puppets withdrawn
        g_poseV2Jerk.Reset();
        if (g_poseV2PlaybackActive)
            Ctx().log << replay::entities::SummaryLine("play") << ",alive_copies=" << g_poseV2AliveCopies
                      << ",no_collision_calls=" << g_poseV2NoCollisionCalls << ",head_bone_calls=" << g_poseV2HeadBoneCalls
                      << ",pose_samples=" << g_poseV2PoseSamples << ",pose_sample_reuses=" << g_poseV2PoseSampleReuses
                      << '\n';
        trailmap::Clear("playback_stop"); // the map goes back to the player
        trailmap::Reset();
        replay::vegetation::Clear();
        Ctx().log << "POSE_V2_VEGETATION_TOTAL,overrides=" << replay::vegetation::Overrides() << '\n';
        replay::collision::EndPlayback(Ctx().log); // POSE_V2_COLLISION_FILTER_SUMMARY, states forgotten
        if (!g_poseV2PlaybackActive && Ctx().puppets.list.empty() && g_poseV2HiddenWorld.empty() && g_poseV2WorldRopes.Empty())
        {
            replay::trains::EndPlayback(); // random trains and the world's trains given back
            replay::vehdoors::EndPlayback(Ctx().log);
            return;
        }
        LogPoseV2WatchSummary(); // before the puppets go
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
            DeletePoseV2Puppet(puppet, "playback_stop");
        LogPoseV2LodSummary(); // after every puppet gave its counts
        {
            // The look applied, and the segment switches re-armed in their tick
            // (segment_switch.h).
            const PoseV2MetapedPlayStats& m = g_poseV2MetapedPlay;
            Ctx().log << "POSE_V2_METAPED_SUMMARY,phase=play,clip_entities=" << Ctx().clip.metapeds.size()
                      << ",dressed=" << m.dressed << ",with_state=" << m.withState << ",exact=" << m.exact
                      << ",not_exact=" << m.notExact << ",unverified=" << m.unverified << ",applies=" << m.applies
                      << ",expressions_set=" << m.expressionsSet << ",retries=" << m.retries
                      << ",appearance_not_recorded=" << m.notRecorded << ",lookahead=" << m.lookahead
                      << ",reader=" << (replay::metaped::engine::ReaderOn() ? "on" : "off") << '\n';
            // The dressing by shop items.
            const PoseV2ShopPlayStats& sp = g_poseV2ShopPlay;
            Ctx().log << "POSE_V2_PLAYBACK_SHOP_ITEMS_SUMMARY,dressed_shop=" << sp.dressShop
                      << ",dressed_layers=" << sp.dressLayers << ",why_no_state=" << sp.whyNoState
                      << ",why_no_shop_items=" << sp.whyNoShopItems << ",why_print_differs=" << sp.whyPrintDiffers
                      << ",why_shop_refused=" << sp.whyShopRefused << ",why_no_layers=" << sp.whyNoLayers
                      << ",checks=" << sp.checks << ",checks_shown=" << sp.checksShown
                      << ",layers_exact_after_shop=" << sp.layersExactAfterShop
                      << ",layers_not_exact_after_shop=" << sp.layersNotExactAfterShop << ",fallbacks=" << sp.fallbacks
                      << ",items_recorded=" << sp.itemsRecorded << ",matched=" << sp.matched
                      << ",state_differ=" << sp.stateDiffer << ",missing=" << sp.missing << ",extra=" << sp.extra
                      << ",shop_exact_bodies=" << sp.exactBodies << '\n';
            LogPoseV2SwitchSummary();
        }
        replay::objects::DeleteAttachHelper();
        replay::objects::EndPlayback();
        {
            std::uint32_t total = 0;
            for (const std::uint32_t count : g_poseV2VehicleFragResults)
                total += count;
            if (total || g_poseV2VehicleFragRebuilds)
            {
                Ctx().log << "POSE_V2_VEHICLE_FRAG_TOTAL";
                for (int slot = 0; slot < replay::frag::kVehicleGroupResults; ++slot)
                    Ctx().log << ',' << replay::frag::VehicleGroupResultText(static_cast<replay::frag::VehicleGroupResult>(slot))
                        << '=' << g_poseV2VehicleFragResults[slot];
                Ctx().log << ",rebuilds=" << g_poseV2VehicleFragRebuilds << '\n';
            }
        }
        replay::weapon::SetStrapOwnedObjects({});
        replay::weapon::SetWeaponPoseSlots({});
        {
            std::ostream& log = Ctx().log;
            const replay::weapon::StrapVisibilityStats strapStats = replay::weapon::StrapVisibilitySnapshot();
            log << "POSE_V2_PLAYBACK_STRAP_VISIBILITY"
                << ",installed=" << (replay::objects::StrapVisibilityReady() ? 1 : 0)
                << ",calls=" << strapStats.calls
                << ",skipped=" << strapStats.skipped
                << ",faults=" << strapStats.faults
                << '\n';
            log.flush();
        }
        replay::events::EndPlayback();
        Ctx().clip.ropes.LogPinCheck(Ctx().log);
        Ctx().clip.ropes.EndPlayback();
        {
            const replay::reins::Stats reins = replay::reins::Snapshot(); // Debug=0 too
            Ctx().log << "POSE_V2_PLAYBACK_REINS_GUARD,installed=" << (replay::reins::Installed() ? 1 : 0)
                      << ",calls=" << reins.calls << ",skipped=" << reins.skipped << ",faults=" << reins.faults
                      << ",hold_installed=" << (replay::reins::HoldInstalled() ? 1 : 0)
                      << ",hold_calls=" << reins.holdCalls << ",hold_skipped=" << reins.holdSkipped
                      << ",hold_skipped_rest=" << reins.holdSkippedRest << ",hold_faults=" << reins.holdFaults << '\n';
        }
        RestorePoseV2WorldRopes();
        RestorePoseV2World();
        replay::trains::EndPlayback(); // after its puppets went
        replay::vehdoors::EndPlayback(Ctx().log); // VEHICLE_DOOR_SUMMARY
        Ctx().puppets.list.clear();
        for (const Hash model : g_poseV2RequestedModels)
            replay::pose::playback::ReleaseModel(model);
        g_poseV2RequestedModels.clear();
        for (const Hash weapon : g_poseV2RequestedWeapons)
            replay::pose::playback::ReleaseWeaponAsset(weapon);
        g_poseV2RequestedWeapons.clear();
        g_poseV2WaitSegmentLogged.clear();
        {
            // The model window lets go of what it held.
            std::vector<std::uint32_t> release;
            g_poseV2Preload.Clear(release);
            for (const std::uint32_t model : release)
                replay::pose::playback::ReleaseModel(model);
            if (!release.empty() || g_poseV2Preload.Requested())
                Ctx().log << "PRELOAD,phase=stop,released=" << release.size() << ",requested_total=" << g_poseV2Preload.Requested()
                    << ",released_total=" << g_poseV2Preload.Released() << ",capped=" << g_poseV2Preload.Capped() << '\n';
            g_poseV2Preload = replay::settle::ModelPreload{};
            g_poseV2PreloadLogs = 0;
            g_poseV2CheapScrub = false;
            g_poseV2CreateBudget = replay::playback::kMaxCreatesPerFrame;
            g_poseV2SettleState = {};
            replay::events::SetStateDeferred(false);
        }
        replay::pose::capture::DisarmOverrides();
        for (bool& inUse : g_poseV2OverrideInUse)
            inUse = false;
        g_poseV2PlaybackActive = false;
        g_poseV2Preparing = false;
    }

    // The game restarted its scripts with a playback open (a save loaded with
    // the replay paused, a death, the mod's own reload). Every handle the stop
    // above would use belongs to the old world (the load freed it, the engine
    // hands the number out again): the tables are emptied without a native on
    // them. Only mod memory (overrides, detour tables) and the global trail map
    // override are touched.
    replay::playback::ForgetReport ForgetPoseV2PlaybackAfterRestart()
    {
        replay::playback::ForgetReport report;
        report.active = g_poseV2PlaybackActive;
        report.puppets = static_cast<std::uint32_t>(Ctx().puppets.list.size());
        report.hiddenWorld = static_cast<std::uint32_t>(g_poseV2HiddenWorld.size());
        report.models = static_cast<std::uint32_t>(g_poseV2RequestedModels.size() + g_poseV2RequestedWeapons.size());
        // The pose detour writes into the armed skeletons: disarmed first
        // (mod memory; each disarm waits out the detours inside its slot).
        replay::pose::capture::DisarmOverrides();
        for (bool& inUse : g_poseV2OverrideInUse)
            inUse = false;
        replay::mount::EndPlayback(Ctx().log); // the pre-render detour withdrawn (mod state)
        g_poseV2Jerk.Reset();
        trailmap::Clear("session_reset"); // a global override, no handle
        trailmap::Reset();
        replay::vegetation::Clear(); // the detour's entity slots
        replay::collision::EndPlayback(Ctx().log); // old handles' states
        replay::weapon::SetStrapOwnedObjects({});
        replay::weapon::SetWeaponPoseSlots({});
        report.objects = replay::objects::ForgetPlaybackAfterRestart();
        report.events = replay::events::ForgetPlaybackAfterRestart();
        Ctx().clip.ropes.ForgetPlayback();
        report.trains = replay::trains::ForgetPlaybackAfterRestart();
        Ctx().puppets.list.clear(); // not deleted: their handles are the old world's
        g_poseV2HiddenWorld.clear(); // not restored: the same
        g_poseV2Landing.Clear();     // the landing queue, old handles
        g_poseV2WorldRopes.Reset();  // the hidden world ropes, old ids
        // The streaming requests were the old script's (the reset released
        // them with the thread); only the lists go.
        g_poseV2RequestedModels.clear();
        g_poseV2RequestedWeapons.clear();
        g_poseV2WaitSegmentLogged.clear();
        {
            std::vector<std::uint32_t> released;
            g_poseV2Preload.Clear(released);
            g_poseV2Preload = replay::settle::ModelPreload{};
            g_poseV2PreloadLogs = 0;
            g_poseV2CheapScrub = false;
            g_poseV2CreateBudget = replay::playback::kMaxCreatesPerFrame;
            g_poseV2SettleState = {};
            replay::events::SetStateDeferred(false);
        }
        g_poseV2PlaybackActive = false;
        g_poseV2Preparing = false;
        Ctx().log << "PLAYBACK_FORGET,why=session_reset,active=" << (report.active ? 1 : 0) << ",puppets=" << report.puppets
                  << ",hidden_world=" << report.hiddenWorld << ",models=" << report.models << ",objects=" << report.objects
                  << ",events=" << report.events << ",trains=" << report.trains << '\n';
        return report;
    }

    // Editor markers: during REC the capture range (enter and exit radius)
    // and a disc under every recorded entity; during PLAY a disc and the
    // ReplayId over every visible puppet.
    replay::markers::GroundCircle g_poseV2EnterCircle;
    replay::markers::GroundCircle g_poseV2ExitCircle;

    void DrawPoseV2Markers()
    {
        // A zone of its own (debug mode, every tick).
        REPLAY_PROFILE_ZONE("tick.markers");
        const ULONGLONG now = GetTickCount64();
        const replay::markers::Color pedColor{70, 190, 255, 170};
        const replay::markers::Color horseColor{255, 150, 40, 170};
        const replay::markers::Color vehicleColor{190, 90, 255, 150};
        if (Ctx().recorder.recording)
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            const Vector3 origin = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
            const replay::entity::SelectionConfig config;
            {
            static const replay::prof::ZoneId circlesZone = replay::prof::Register("tick.markers.circles");
            const replay::prof::Scope circlesScope(circlesZone);
            g_poseV2EnterCircle.Draw(origin.x, origin.y, origin.z, config.enterRadius,
                {80, 230, 110, 150}, now);
            g_poseV2ExitCircle.Draw(origin.x, origin.y, origin.z, config.exitRadius,
                {235, 70, 60, 110}, now);
            }
            static const replay::prof::ZoneId discsZone = replay::prof::Register("tick.markers.discs");
            const replay::prof::Scope discsScope(discsZone);
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                // The position from the entity cache (memory path, checked
                // against the native on a schedule) instead of
                // DOES_ENTITY_EXIST + GET_ENTITY_COORDS: two natives less per
                // recorded entity per tick, same disc.
                float at[3]{};
                if (!entity.handle || !replay::entities::Position(entity.handle, at))
                    continue;
                Vector3 position{};
                position.x = at[0];
                position.y = at[1];
                position.z = at[2];
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(entity.id.Packed());
                const bool horse = create && create->kind == replay::entity::EntityKind::Horse;
                replay::markers::DrawEntityDiscAt(entity.handle, position,
                    entity.vehicle ? 2.2f : horse ? 1.1f : 0.6f,
                    {255, 220, 60, 170});
            }
        }
        if (g_poseV2PlaybackActive && Ctx().transport.Playing())
        {
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if (puppet.packet.kind == replay::entity::EntityKind::Weapon ||
                    puppet.packet.kind == replay::entity::EntityKind::Prop ||
                    !puppet.clone || !replay::entities::Exists(puppet.clone) ||
                    !ENTITY::IS_ENTITY_VISIBLE(puppet.clone))
                {
                    continue;
                }
                const bool horse = puppet.packet.kind == replay::entity::EntityKind::Horse;
                const bool vehicle = puppet.packet.kind == replay::entity::EntityKind::Vehicle;
                const replay::markers::Color& color =
                    vehicle ? vehicleColor : horse ? horseColor : pedColor;
                // One coordinates read for the disc and the label (the disc
                // read them again, nothing moves the clone in between) and no
                // second existence check (IS_ENTITY_VISIBLE above is false for
                // a handle without an entity).
                const Vector3 position = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
                replay::markers::DrawEntityDiscAt(puppet.clone, position,
                    vehicle ? 2.2f : horse ? 1.1f : 0.6f, color);
                float screenX = 0.0f;
                float screenY = 0.0f;
                if (GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                        position.x, position.y, position.z + (horse ? 1.6f : 1.1f),
                        &screenX, &screenY))
                {
                    replay::hud::DrawText(replay::entity::IdText(puppet.packet.id).c_str(), screenX, screenY, 0.3f,
                        color.r, color.g, color.b, 230);
                }
            }
        }
    }
    }

    void Attach(Context& context)
    {
        g_context = &context;
        context.hooks = &g_hooks;
    }
    void Start() { StartPoseV2Playback(); }
    void Update() { UpdatePoseV2Playback(); }
    void Stop() { StopPoseV2Playback(); }
    void UpdateWorldLanding() { UpdatePoseV2WorldLanding(); }
    ForgetReport ForgetAfterRestart() { return ForgetPoseV2PlaybackAfterRestart(); }
    void DrawMarkers() { DrawPoseV2Markers(); }
    bool NormalPlay() { return PoseV2NormalPlay(); }

    std::uint32_t MountOf(std::uint32_t packedRider)
    {
        return g_poseV2Mounts.MountOf(packedRider);
    }

    bool TrailFocus(float& x, float& y, float& z)
    {
        if (!trailmap::g_overrideSet || !trailmap::g_targetClone || !replay::entities::Exists(trailmap::g_targetClone))
            return false;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(trailmap::g_targetClone, TRUE, FALSE);
        x = at.x;
        y = at.y;
        z = at.z;
        return true;
    }
    bool Preparing() { return g_poseV2Preparing; }

    void SetCheapScrub(bool cheap) { g_poseV2CheapScrub = cheap; }
    bool CheapScrub() { return g_poseV2CheapScrub; }
    void SetCreateBudget(std::uint32_t budget) { g_poseV2CreateBudget = budget ? budget : kMaxCreatesPerFrame; }
    void SetWatchHeld(bool held) { g_poseV2WatchHeld = held; }
    void SetPuppetLod(const lod::Config& config) { g_poseV2LodConfig = config; }
    void SetHatFix(bool on) { g_poseV2HatFix = on; }
    void SetTPoseGate(std::uint32_t settleMs, bool onScreenOnly)
    {
        g_tposeGate.Configure(settleMs, onScreenOnly);
    }
    void SetTPose(int hideNeverAnimated, bool watch)
    {
        g_tposeHideMode = hideNeverAnimated <= 0 ? replay::tpose::HideMode::Off
            : hideNeverAnimated == 1          ? replay::tpose::HideMode::BindProven
                                              : replay::tpose::HideMode::Signature;
        g_tposeWatchOn = watch;
    }
    void SetSegmentSwitchInTick(bool on) { g_poseV2SwitchInTick = on; }
    void SetNoRecordFallback(bool mapNow, bool alternates)
    {
        g_noRecordMapNow = mapNow;
        g_noRecordAltAppearance = alternates;
    }
    SettleState GetSettleState() { return g_poseV2SettleState; }
    Counters GetCounters() { return g_poseV2Counters; }

    std::uint32_t AliveWithoutModel(std::uint32_t timeMs)
    {
        if (!g_poseV2PlaybackActive)
            return 0;
        static std::vector<replay::entity::CreatePacket> alive;
        Ctx().clip.lifecycle.AliveAt(timeMs, alive);
        std::uint32_t missing = 0;
        for (const replay::entity::CreatePacket& packet : alive)
        {
            if (packet.weaponHash || !packet.modelHash)
                continue;
            // A model the game does not know never loads (the puppet fails
            // after kModelTimeoutMs): not a reason to freeze every jump.
            if (STREAMING::IS_MODEL_VALID(packet.modelHash) && !STREAMING::HAS_MODEL_LOADED(packet.modelHash))
                ++missing;
        }
        return missing;
    }

    void PreloadModels(std::uint32_t playheadMs, std::uint64_t now, std::uint32_t windowMs)
    {
        if (!g_poseV2PlaybackActive)
            return;
        REPLAY_PROFILE_ZONE("play.preload");
        static std::vector<std::uint32_t> wanted;
        static std::vector<std::uint32_t> owned;
        static std::vector<std::uint32_t> request;
        static std::vector<std::uint32_t> release;
        const std::uint32_t from = playheadMs > windowMs ? playheadMs - windowMs : 0;
        Ctx().clip.lifecycle.ModelsBetween(from, playheadMs + windowMs, wanted);
        owned.assign(g_poseV2RequestedModels.begin(), g_poseV2RequestedModels.end());
        const std::size_t before = g_poseV2Preload.Count();
        g_poseV2Preload.Update(now, wanted, owned, request, release);
        std::uint32_t loaded = 0;
        for (const std::uint32_t model : request)
            loaded += replay::pose::playback::RequestModel(model) ? 1 : 0;
        for (const std::uint32_t model : release)
            replay::pose::playback::ReleaseModel(model);
        if ((g_poseV2Preload.Count() != before || !release.empty()) && g_poseV2PreloadLogs < 100)
        {
            ++g_poseV2PreloadLogs;
            Ctx().log << "PRELOAD,phase=window,playhead_ms=" << playheadMs << ",window_ms=" << windowMs << ",wanted=" << wanted.size()
                << ",owned=" << owned.size() << ",held=" << g_poseV2Preload.Count() << ",requested=" << request.size()
                << ",loaded=" << loaded << ",released=" << release.size() << ",capped_total=" << g_poseV2Preload.Capped() << '\n';
        }
    }

    bool RecordedBodyAt(std::uint32_t packedId, std::uint32_t timeMs, int headBone, RecordedBody& out, bool holdFirst)
    {
        out = RecordedBody{};
        if (!packedId)
            return false;
        const replay::entity::ReplayId id = replay::entity::ReplayId::FromPacked(packedId);
        const replay::entity::PoseSegment* segment = PoseV2SegmentAt(id, timeMs);
        const std::uint32_t recordId = segment ? segment->recordId : packedId;
        std::uint32_t firstMs = 0, lastMs = 0;
        constexpr std::uint32_t kTrackEndSlackMs = 100; // the puppet's own slack (Update)
        if (!replay::pose::capture::TrackSpan(recordId, firstMs, lastMs) || (timeMs < firstMs && !holdFirst) ||
            timeMs > lastMs + kTrackEndSlackMs)
            return false;
        if (holdFirst)
        {
            std::uint32_t sampleAt = timeMs;
            if (!replay::camera::bones::CameraRecordTime(timeMs, firstMs, lastMs, kTrackEndSlackMs, sampleAt))
                return false;
            out.heldFirst = sampleAt != timeMs;
            timeMs = sampleAt;
        }
        // Its own sample (the puppets' g_poseV2Sample and the detector's stay untouched).
        static replay::pose::PoseSample sample;
        replay::pose::WorldTransform world{};
        if (!replay::pose::capture::SampleTrack(recordId, timeMs, sample) || !SampleWorldTransform(sample, world))
            return false;
        out.position[0] = world.position.x;
        out.position[1] = world.position.y;
        out.position[2] = world.position.z;
        out.rotation[0] = world.rotation.x;
        out.rotation[1] = world.rotation.y;
        out.rotation[2] = world.rotation.z;
        out.rotation[3] = world.rotation.w;
        out.recordId = recordId;
        out.boneCount = static_cast<std::uint32_t>(sample.objects.size());
        if (headBone >= 0 && static_cast<std::size_t>(headBone) < sample.objects.size())
        {
            const replay::pose::BoneQts& bone = sample.objects[static_cast<std::size_t>(headBone)];
            out.boneTranslation[0] = bone.translation[0];
            out.boneTranslation[1] = bone.translation[1];
            out.boneTranslation[2] = bone.translation[2];
            out.boneRotation[0] = bone.rotation.x;
            out.boneRotation[1] = bone.rotation.y;
            out.boneRotation[2] = bone.rotation.z;
            out.boneRotation[3] = bone.rotation.w;
            const replay::pose::Vec3 local{bone.translation[0], bone.translation[1], bone.translation[2]};
            const replay::pose::Vec3 offset = replay::pose::Rotate(world.rotation, local);
            out.head[0] = world.position.x + offset.x;
            out.head[1] = world.position.y + offset.y;
            out.head[2] = world.position.z + offset.z;
            out.hasHead = std::isfinite(out.head[0]) && std::isfinite(out.head[1]) && std::isfinite(out.head[2]);
        }
        return true;
    }
}
