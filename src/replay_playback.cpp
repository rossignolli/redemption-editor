// replay_playback.cpp: the playback start, tick (UpdatePoseV2Playback) and stop, the hat and T-pose totals, the puppet delete, the trail focus, the restart forget, the markers, the public entry points.
// The other parts are in replay_playback_*.cpp; shared internals in replay_playback_internal.h.

#include "replay_playback_internal.h"

namespace replay::playback
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
    namespace
    {

    // Preparation: the playhead holds at the start until the puppets alive
    // there are built, as a replay editor holds the image while it loads.
    constexpr ULONGLONG kPoseV2PrepareTimeoutMs = 8000;
    bool g_poseV2Preparing = false;
    ULONGLONG g_poseV2PrepareDeadline = 0;
    }

    // Normal play: the playhead moves forward at +1x. One-shot events play only
    // then, never paused, preparing or scrubbing (as in a replay editor: sounds
    // and shots are skipped while the cursor rewinds, fast forwards or jumps).
    bool PoseV2NormalPlay()
    {
        return !Ctx().transport.Paused() && !g_poseV2Preparing && !Ctx().transport.Scrubbing();
    }

    std::vector<replay::entity::CreatePacket> g_poseV2Alive;
    namespace
    {
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
    }
    // The seek settle's side of the playback (seek_settle.h). Cheap scrub: the
    // vehicle rebuilds and the event state wait for the release; the create
    // budget per frame; what the settle waits for.
    bool g_poseV2CheapScrub = false;
    namespace
    {
    std::uint32_t g_poseV2CreateBudget = replay::playback::kMaxCreatesPerFrame;
    }
    replay::playback::SettleState g_poseV2SettleState;
    replay::playback::Counters g_poseV2Counters;
    namespace
    {
    replay::settle::ModelPreload g_poseV2Preload;
    std::uint32_t g_poseV2PreloadLogs = 0;
    }
    std::uint32_t g_poseV2UnposedHideLogs = 0; // POSE_V2_UNPOSED_HIDDEN lines
    // Unposed bodies as episodes (puppet_watch.h); a line (and a puppet_unposed
    // anomaly) only past the rule.
    replay::watch::UnposedTracker g_poseV2Unposed;
    bool g_poseV2OverrideInUse[replay::pose::capture::kMaxOverrides] = {};
    namespace
    {
    bool g_poseV2PlaybackActive = false;
    }
    std::uint32_t g_poseV2ScorchPlayLogs = 0; // POSE_V2_SCORCHED play lines
    std::uint32_t g_poseV2AppearancePlayLogs = 0; // POSE_V2_APPEARANCE_CHANGE play lines
    PoseV2MetapedPlayStats g_poseV2MetapedPlay;
    PoseV2ShopPlayStats g_poseV2ShopPlay;
    // Skeleton changes re-armed in their tick (segment_switch.h) and the ticks
    // bodies went without a pose around them.
    PoseV2SwitchTotals g_poseV2Switch;
    // A posed body whose skeleton changed to a bone count no record has
    // (no_record_fallback.h) is armed through the map at once and the recorded
    // layers tried again behind it; the layer diff of each step logged with the
    // categories learned from the puppets.
    NoRecordTotals g_noRecord;
    std::uint32_t g_poseV2WindowPlayLogs = 0; // POSE_V2_VEHICLE_WINDOWS play lines
    std::uint32_t g_poseV2LookVerifyLogs = 0; // POSE_V2_VEHICLE_LOOK verify lines
    std::uint32_t g_poseV2PartPlayLogs = 0;   // POSE_V2_VEHICLE_PARTS play lines
    std::unordered_map<std::uint32_t, std::uint16_t> g_poseV2PartKeptLogged; // vehicle -> wheels kept, last logged
    // Vehicle fragment groups in playback, per result
    // (replay::frag::VehicleGroupResult) and the POSE_V2_VEHICLE_FRAG lines.
    std::uint32_t g_poseV2VehicleFragResults[replay::frag::kVehicleGroupResults] = {};
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
    namespace
    {
    std::uint32_t g_poseV2PlaceDriftLogs = 0; // POSE_V2_PLACE_DRIFT lines
    }
    // Rider and mount pairs from the recorded transforms, the jerk detector
    // (pose_jerk.h), and the puppets the engine pushed off their placement
    // while they were physics bodies: out of physics until the time kept here
    // (GetTickCount64), so the next frames render where we place them.
    // The puppets' LOD settings (puppet_lod.h) and the playback's totals
    // (POSE_V2_PUPPET_LOD_SUMMARY, Debug=0 too).
    replay::lod::Config g_poseV2LodConfig;
    namespace
    {
    // The hat fix (hat_chain.h) and the models' fitted values seen during this
    // playback.
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
    }

    // The hat fix of the record armed on the puppet, for the layout at the
    // playhead (built again when that layout changes: a hat knocked off,
    // another layer), from the sample just taken. Peds in exact mode only;
    // what the record's chains learned is kept across rebuilds.
    void PreparePoseV2HatFix(replay::playback::Puppet& puppet, std::uint32_t recordId, std::uint32_t timeMs,
        const replay::pose::PoseSample& sample)
    {
        if (puppet.packet.kind != replay::entity::EntityKind::Ped || puppet.mapped)
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
    namespace
    {
    std::uint32_t g_poseV2LowLodLogs = 0;
    std::uint32_t g_poseV2AnimSkipLogs = 0;
    std::uint32_t g_poseV2LodLastPlayhead = ~0u; // the playhead of the last tick
    std::uint32_t g_poseV2LodPrevPlayhead = ~0u; // this tick's view of it
    constexpr std::uint32_t kPoseV2LowLodLogCap = 64;
    }
    replay::jerk::MountPairs g_poseV2Mounts;
    namespace
    {
    // Recorded visibility per ped and horse.
    std::unordered_map<std::uint32_t, const replay::PedVisibility*> g_poseV2PedVisibility;
    std::uint32_t g_poseV2RecordedVisibilityLogs = 0;
    }
    // The recorded player's id (player_hold.h PickPlayer, 0: none): his puppet
    // is never hidden, it holds its nearest pose (QA 224). A line per change
    // of what the hold covers (held, kept against PVIS), capped.
    std::uint32_t g_poseV2PlayerPacked = 0;
    namespace
    {
    std::uint32_t g_poseV2PlayerHoldLogs = 0;
    // The horse he rides at his record's first frame (0: none) with both
    // recorded transforms of that frame: before it he is carried by that horse
    // (player_hold.h Carry).
    std::uint32_t g_poseV2PlayerFirstMount = 0;
    replay::pose::WorldTransform g_poseV2PlayerRiderFirst{};
    replay::pose::WorldTransform g_poseV2PlayerMountFirst{};
    constexpr std::uint32_t kPoseV2PlayerHoldLogCap = 32;
    }
    bool PoseV2RecordedVisible(std::uint32_t packed, std::uint32_t timeMs)
    {
        const auto found = g_poseV2PedVisibility.find(packed);
        return found == g_poseV2PedVisibility.end() ||
            replay::objcheck::VisibleAt(found->second->visible != 0, found->second->flipsMs, timeMs);
    }
    namespace
    {
    // The never animated hide (bind proven) and the T-pose watch
    // (tpose_check.h). The signature of each record, read once per
    // playback; the finalizer code that proves the bind pose layout (0 not
    // checked yet, 1 proven, -1 refused: no bind read, no watch); the counts of
    // POSE_V2_TPOSE_SUMMARY.
    constexpr replay::tpose::HideMode kTPoseHideMode = replay::tpose::HideMode::BindProven;
    std::unordered_map<std::uint32_t, replay::tpose::TrackSignature> g_tposeSignatures;
    int g_tposeBindLayout = 0;
    replay::tpose::TPoseWatch g_tposeWatch;
    // When the watch judges (TPoseSettleMs, a body on screen only; tpose_check.h).
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
            g_tposeBindLayout = replay::tpose::BindLayoutProven(
                [](const replay::sig::Signature& site) { return replay::sig::Find(site) != 0; }) ? 1 : -1;
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
        const replay::tpose::HideVerdict verdict = replay::tpose::Decide(kTPoseHideMode, signature, compared ? &match : nullptr);
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
    }

    void LogPoseV2TPoseSummary()
    {
        const replay::tpose::WatchSummary watch = g_tposeWatch.Summary();
        const TPoseCounts& c = g_tposeCounts;
        using replay::tpose::HideVerdict;
        Ctx().log << "POSE_V2_TPOSE_SUMMARY,checks=" << watch.checks
                  << ",bind_checks=" << watch.bindChecks << ",unread=" << watch.unread << ",episodes=" << watch.episodes
                  << ",entities=" << watch.entities << ",open=" << watch.open
                  << ",hide_mode=" << static_cast<int>(kTPoseHideMode) << ",never_judged=" << c.judged
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
    namespace
    {
    std::unordered_map<std::uint32_t, ULONGLONG> g_poseV2PushedUntil;
    }
    std::uint32_t g_poseV2JerkLogs = 0;  // POSE_JERK lines this playback
    std::uint32_t g_poseV2MountLogs = 0; // POSE_V2_MOUNT_PAIR lines this playback
    std::uint64_t g_poseV2CarriedTicks = 0; // puppet ticks kept out of physics as riders or seated
    std::uint64_t g_poseV2PushedHolds = 0;  // physics bodies taken out after a push
    namespace
    {
    constexpr ULONGLONG kPoseV2PushedHoldMs = 1000;
    }
    // End-of-frame check of the written pose, per override slot: next check
    // (GetTickCount64) and whether the last one differed.
    std::uint32_t g_poseV2WriteCheckLogs = 0;
    std::uint64_t g_poseV2WriteCheckAt[replay::pose::capture::kMaxOverrides] = {};
    bool g_poseV2WriteChanged[replay::pose::capture::kMaxOverrides] = {};
    replay::pose::PoseSample g_poseV2Sample;
    namespace
    {
    replay::pose::PoseSample g_poseV2AltSample;
    }
    std::uint32_t g_poseV2MappedArms = 0;
    std::uint32_t g_poseV2RefusedArms = 0;
    std::uint32_t g_poseV2RefusedHiddenLogs = 0;
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
    namespace
    {

    // Playback world: every ped that is not a puppet (the player, its horse,
    // NPCs, animals) is hidden while the editor plays, and ambient
    // population is suppressed, so everything visible belongs to the replay.
    constexpr ULONGLONG kPoseV2WorldScanIntervalMs = 250;
    }
    std::vector<replay::pose::playback::HiddenPedState> g_poseV2HiddenWorld;
    ULONGLONG g_poseV2NextWorldScanAt = 0;

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
    // The world's ropes hidden while the replay plays (rope_world.h).
    replay::ropes::WorldRopeHider g_poseV2WorldRopes;
    std::uint32_t g_poseV2WorldRopeScans = 0;
    std::uint32_t g_poseV2WorldRopeSnapshotFails = 0;
    std::uint32_t g_poseV2WorldRopeNoHandle = 0; // DOES_ROPE_EXIST false for a listed rope
    std::uint32_t g_poseV2CreateRefused = 0;     // creates the engine refused this playback
    std::uint32_t g_poseV2CreateRefusedLogs = 0;
    namespace
    {

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
    }

    // The playback's hat fixes (after the puppets' deletes put theirs in).
    void LogPoseV2HatSummary()
    {
        const PoseV2HatTotals& t = g_poseV2HatTotals;
        if (!t.puppets)
            return;
        Ctx().log << "POSE_V2_HAT_FIX_SUMMARY,puppets=" << t.puppets << ",fixed=" << t.fixed
                  << ",learned=" << t.learned << ",handled=" << t.handled << ",kept_objects=" << t.kept
                  << ",puppets_on_default=" << t.defaults << ",models_fitted=" << g_poseV2HatFitted.size() << '\n';
    }
    namespace
    {

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

    // The snow and mud trail map follows the replay, not the player. The engine
    // centres the trail map (the texture the terrain reads its snow and mud
    // marks from) on the local player ped, or on an entity set by
    // _0x9F158A49B0D84C3C (the focus getter: the override slot, else the
    // player, a mounted ped's mount instead), and stamps only inside that
    // window (the stamp's in-bounds gate). The player stays hidden where the
    // clip was opened, possibly across the map: no puppet would be inside the
    // window, so the snow would never deform. The recording had the map on the
    // player: the main character's puppet takes that place while the camera is
    // near it, else the ped or horse puppet nearest the camera; none near: the
    // override is cleared (back to the player). A target far from the last one
    // clears the map, as a teleport does in the game. Diagnostics read the
    // engine globals the signatures below reach, never written.
    namespace trailmap
    {
        // _0x9F158A49B0D84C3C's body: push rbx; sub rsp,20h; call; mov rbx,rax;
        // test rax,rax; je; cmp [rip+X],rax (X = the override slot).
        const sig::Signature kSetOverride{"playback_trail_set_override",
            "40 53 48 83 EC 20 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 4F 48 39 05 ?? ?? ?? ??", 0xF9ED0C};
        const sig::Signature kOverrideSlot{"playback_trail_override_slot", kSetOverride, sig::Rel32{0x16, 0x1A}, "",
            0x4302528};
        // _0xEC3F7F24EEEB3BA3's body: sub rsp,28h; xor ecx,ecx; call; then
        // mov rcx,[rip+X] (X = the trail manager pointer).
        const sig::Signature kResetNative{"playback_trail_reset_native",
            "48 83 EC 28 33 C9 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9", 0xF9374C};
        const sig::Signature kManagerSlot{"playback_trail_manager_slot", kResetNative, sig::Rel32{0xE, 0x12}, "", 0x4A1DE70};
        // The snow depth update (the player's in-snow depth or a probe at his
        // position, smoothed): movss [rip+X],xmm0 (target), movss xmm1,[rip+X]
        // (smoothed), mov r8,[rip+X] (config).
        const sig::Signature kDepthUpdate{"playback_trail_depth_update",
            "48 8B C4 48 89 58 18 48 89 70 20 55 57 41 57 48 8D 68 A1 48 81 EC A0 00 00 00", 0x126ADA4};
        const sig::Signature kDepthTargetStore{"playback_trail_depth_target_store", kDepthUpdate, sig::Offset{0x167},
            "F3 0F 11 05 ?? ?? ?? ??", 0x126AF0B};
        const sig::Signature kDepthTarget{"playback_trail_depth_target", kDepthTargetStore, sig::Rel32{4, 8}, "", 0x4C38A10};
        const sig::Signature kDepthSmoothedLoad{"playback_trail_depth_smoothed_load", kDepthUpdate, sig::Offset{0x1C8},
            "F3 0F 10 0D ?? ?? ?? ??", 0x126AF6C};
        const sig::Signature kDepthSmoothed{"playback_trail_depth_smoothed", kDepthSmoothedLoad, sig::Rel32{4, 8}, "",
            0x4C389EC};
        const sig::Signature kDepthConfigLoad{"playback_trail_depth_config_load", kDepthUpdate, sig::Offset{0x3C},
            "4C 8B 05 ?? ?? ?? ??", 0x126ADE0};
        const sig::Signature kDepthConfig{"playback_trail_depth_config", kDepthConfigLoad, sig::Rel32{3, 7}, "", 0x4C389F8};
        constexpr std::uint32_t kMapObjectFromOverride = 0x538; // override slot = map object + 0x538
        constexpr std::uint32_t kMapCentreOffset = 0x223DE0;    // map 0 centre (x, y, z)
        constexpr std::uint32_t kManagerCount = 0x0C;           // records in use (of 128)
        constexpr std::uint32_t kManagerPending = 0x29890;      // entities waiting for a record
        constexpr std::uint32_t kEntityTrailFlags = 0xB4;       // bit 22: the entity has a trail record
        constexpr std::uint32_t kManagerCapacity = 128;         // the record add: cmp [mgr+0xC], 0x80
        constexpr std::uint64_t kRecordBytes = 0x470;
        constexpr float kKeepMeters = 80.0f;
        constexpr float kNearestMeters = 250.0f;

        struct Globals
        {
            bool resolved = false;
            std::uint64_t overrideSlot = 0; // address of the override entity pointer
            std::uint64_t managerSlot = 0;  // address of the trail manager pointer
            // The snow depth the trail map's look follows, taken at the PLAYER
            // (kDepthUpdate: his in-snow depth or a probe at his position,
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

        void Resolve()
        {
            if (g_globals.resolved)
                return;
            g_globals.resolved = true;
            g_globals.overrideSlot = sig::Find(kOverrideSlot);
            g_globals.managerSlot = sig::Find(kManagerSlot);
            g_globals.depthTarget = sig::Find(kDepthTarget);
            g_globals.depthSmoothed = sig::Find(kDepthSmoothed);
            g_globals.depthConfig = sig::Find(kDepthConfig);
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
                // The settle's budget.
                if (AdvancePoseV2Puppet(puppet, timeMs, creates < g_poseV2CreateBudget))
                    ++creates;
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
            if (body)
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
                    // as relocated (the relocation mark, through the coords native); the
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
