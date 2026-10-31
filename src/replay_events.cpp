// replay_events.cpp: the recording side: attach, warm-up, the recording's begin, tick and end, the shots and gore recorded per tick, the settings.
// The other families are in replay_events_*.cpp; shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {

        Context* g_context = nullptr;
    }
        Context& Ctx() { return *g_context; }
    namespace
    {

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;

    // Audio evidence around events. Each window logs the sound creation
    // paths (callers of the sound init params) and the speech Say calls
    // within it, so the engine audio entry points of shots, hits and speech
    // can be identified.
    struct PoseV2AudioWindow
    {
        std::string label;
        std::uint64_t centerUs = 0;
        std::uint64_t beforeUs = 0;
        std::uint64_t afterUs = 0;
    };
    std::vector<PoseV2AudioWindow> g_poseV2AudioWindows;
    }
    // The research audio windows (sound paths around each shot, hit and
    // speech line) and the per-effect POSE_V2_FX_OBS lines are off unless a
    // scenario turns them on: in a fight they wrote ~86,000 log lines in
    // 25 s and cost the script thread 239 us per frame. The fx windows
    // themselves always run.
    bool g_poseV2ResearchWindows = false;
    namespace
    {
    bool g_poseV2AudioObserverTried = false;
    }
    bool g_poseV2AudioObserverReady = false;
    void OpenPoseV2AudioWindow(const std::string& label, std::uint64_t beforeUs, std::uint64_t afterUs)
    {
        if (!g_poseV2ResearchWindows || !g_poseV2AudioObserverReady || g_poseV2AudioWindows.size() > 64)
            return;
        g_poseV2AudioWindows.push_back({label, replay::audio::NowUs(), beforeUs, afterUs});
    }
    namespace
    {

    void FlushPoseV2AudioWindows(bool all)
    {
        REPLAY_PROFILE_ZONE("rec.audio_windows");
        if (g_poseV2AudioWindows.empty())
            return;
        const std::uint64_t now = replay::audio::NowUs();
        static std::vector<replay::audio::SoundCall> sounds;
        static std::vector<replay::audio::SpeechCall> speech;
        std::ostream& log = Ctx().log;
        for (std::size_t index = 0; index < g_poseV2AudioWindows.size();)
        {
            const PoseV2AudioWindow window = g_poseV2AudioWindows[index];
            if (!all && now < window.centerUs + window.afterUs)
            {
                ++index;
                continue;
            }
            const std::uint64_t from = window.centerUs - window.beforeUs;
            const std::uint64_t to = window.centerUs + window.afterUs;
            replay::audio::CollectSounds(from, to, sounds);
            replay::audio::CollectSpeech(from, to, speech);
            struct Path
            {
                std::uint32_t callers[replay::audio::kCallerDepth];
                std::uint32_t count;
                std::int64_t firstDtUs;
            };
            std::vector<Path> paths;
            for (const replay::audio::SoundCall& call : sounds)
            {
                const std::int64_t dt = static_cast<std::int64_t>(call.timeUs) - static_cast<std::int64_t>(window.centerUs);
                auto it = std::find_if(paths.begin(), paths.end(), [&](const Path& path) {
                    return std::equal(std::begin(path.callers), std::end(path.callers), call.callers);
                });
                if (it == paths.end())
                {
                    Path path{};
                    std::copy(std::begin(call.callers), std::end(call.callers), path.callers);
                    path.count = 1;
                    path.firstDtUs = dt;
                    paths.push_back(path);
                }
                else
                {
                    ++it->count;
                }
            }
            log << "POSE_V2_AUDIO_WINDOW,event=" << window.label
                << ",sounds=" << sounds.size()
                << ",paths=" << paths.size()
                << ",speech=" << speech.size()
                << '\n';
            for (const Path& path : paths)
            {
                log << "POSE_V2_AUDIO_PATH,event=" << window.label
                    << ",count=" << path.count
                    << ",first_dt_us=" << path.firstDtUs
                    << ",callers=" << std::hex;
                for (std::uint32_t depth = 0; depth < replay::audio::kCallerDepth; ++depth)
                    log << (depth ? "/0x" : "0x") << path.callers[depth];
                log << std::dec << '\n';
            }
            for (const replay::audio::SpeechCall& call : speech)
            {
                log << "POSE_V2_AUDIO_SPEECH,event=" << window.label
                    << ",dt_us=" << (static_cast<std::int64_t>(call.timeUs) - static_cast<std::int64_t>(window.centerUs))
                    << ",voice=0x" << std::hex << call.voiceHash
                    << ",context=0x" << replay::audio::FinalizeHash(call.contextPartialHash)
                    << ",variation=" << std::dec << call.variation
                    << ",result=" << call.result
                    << ",entity=0x" << std::hex << call.speechEntity
                    << ",callers=0x" << call.callers[0] << "/0x" << call.callers[1] << "/0x" << call.callers[2]
                    << std::dec << '\n';
            }
            g_poseV2AudioWindows.erase(g_poseV2AudioWindows.begin() + static_cast<std::ptrdiff_t>(index));
        }
        log.flush();
    }

    // Shots (the weapon fire event): recorded at the frame the game fires,
    // per recorded ped. A shot is a drop of the clip ammo of the same weapon,
    // or a rising IS_PED_SHOOTING edge for weapons without a clip. The muzzle
    // is the hand weapon object's Gun_Muzzle bone and the direction its +X
    // axis (as the game scripts use it); the impact is the ped's next new
    // weapon impact coordinate, if one comes shortly after.
    constexpr std::uint32_t kPoseV2ImpactWindowMs = 300;
    constexpr int kPoseV2MaxShotsPerFrame = 4;

    struct PoseV2ShooterState
    {
        Ped ped = 0;
        Hash weapon = 0;
        int clip = -1;
        bool shooting = false;
        Vector3 lastImpact{};
        bool hasLastImpact = false;
        bool pendingImpact = false;
        std::size_t pendingEvent = 0;
        std::uint32_t pendingSinceMs = 0;
    };
    std::vector<PoseV2ShooterState> g_poseV2Shooters;
    bool g_poseV2FireShotsSource = false;
    std::uint64_t g_poseV2FireCursor = 0;
    std::vector<replay::audio::FireCall> g_poseV2FrameFires;
    std::uint64_t g_poseV2ShotImpacts = 0;
    }
    std::vector<PoseV2FxWindow> g_poseV2FxWindows;
    namespace
    {
    bool g_poseV2FxObserverTried = false;
    }
    bool g_poseV2FxObserverReady = false;
    bool g_poseV2FxReplayEnabled = true; // bench control switch
    namespace
    {
    bool g_poseV2DecalObserverTried = false;
    }
    bool g_poseV2DecalObserverReady = false;
    std::uint64_t g_poseV2DecalCursor = 0;
    std::uint64_t g_poseV2DecalsRefused = 0;   // engine refused (result 0)
    std::uint64_t g_poseV2DecalsInternal = 0;  // internal re-add path
    std::uint64_t g_poseV2DecalsAttached = 0;
    std::uint32_t g_poseV2DecalSamples = 0;
    replay::entity::ForwardEventCursor g_poseV2DecalCursorPlay;
    std::uint64_t g_poseV2DecalsPlayed = 0;
    std::uint64_t g_poseV2DecalsSkippedInternal = 0;
    std::uint64_t g_poseV2DecalsNoEntity = 0;
    std::uint64_t g_poseV2DecalsInfoUnknown = 0; // recorded info pointer not in the info table
    std::uint64_t g_poseV2DecalsNoInfo = 0;      // playback: the info lookup refused the key
    // A decal without an info record (recorded unknown, or the record has no
    // data) is refused, never added with info 0: the ring worker reads
    // [info+0x72] and faults.
    std::uint64_t g_poseV2DecalsNullInfo = 0;
    std::uint32_t g_poseV2DecalNullInfoLogs = 0;
    std::uint64_t g_poseV2DecalsDeadIdReadd = 0;  // a replace whose id had died: added fresh
    std::uint32_t g_poseV2DecalGuardLogs = 0;
    std::uint64_t g_poseV2DecalsFailed = 0;
    std::string g_poseV2DecalLastError;
    std::uint64_t g_poseV2DecalRemoveCursor = 0;
    replay::entity::ForwardEventCursor g_poseV2DecalRemoveCursorPlay;
    std::map<std::uint32_t, std::uint32_t> g_poseV2DecalIds; // recorded engine id -> ours
    std::uint64_t g_poseV2DecalsRemoved = 0;
    std::uint64_t g_poseV2DecalsReplaced = 0;
    namespace
    {
    // Gore: the limb mask each recorded ped had at the last poll (replay id
    // packed -> mask), and the playback side.
    std::map<std::uint32_t, std::uint64_t> g_poseV2GoreMasks;
    }
    bool g_poseV2BloodObserverReady = false;
    // Fires.
    bool g_poseV2FireReady = false;
    namespace
    {
    std::uint64_t g_poseV2EngineFireCursor = 0;
    }
    std::map<std::int32_t, std::uint32_t> g_poseV2FireActiveSlots; // slot -> start index
    namespace
    {
    std::uint64_t g_poseV2FireFar = 0;
    std::uint64_t g_poseV2FireRefused = 0;
    }
    std::uint64_t g_poseV2FireCharShielded = 0; // puppet fire records kept through a rebuild clear
    namespace
    {
    // Detour events drained and discarded while the recording gate held
    // (RECORD_GATE_TOTAL).
    std::uint64_t g_poseV2GateDiscarded = 0;
    std::uint64_t g_poseV2FireRemnantAdds = 0;
    std::uint64_t g_poseV2FireRemnantRemoves = 0;
    }
    std::map<std::uint64_t, std::uint32_t> g_poseV2FireOwners; // fire or remnant address -> start index
    std::uint32_t g_poseV2FireItemSampleAt = 0;
    std::uint64_t g_poseV2FireRestored = 0;
    std::uint64_t g_poseV2FireRestoreMissed = 0;
    std::vector<OurFire> g_poseV2FiresOurs;
    replay::entity::ForwardEventCursor g_poseV2FireStartCursorPlay;
    replay::entity::ForwardEventCursor g_poseV2FireStopCursorPlay;
    std::uint32_t g_poseV2FireLastPlayhead = 0;
    namespace
    {
    bool g_poseV2FireInitialPending = false;
    }
    // Fires the start detour never saw, read from the engine's active list
    // (fire_initial.h): counted and logged, at record start and, once a
    // second, when one comes within 150 m.
    // The playback clears the live fires (STOP_FIRE_IN_RANGE) and relights the
    // recorded ones.
    namespace
    {
    std::uint32_t g_poseV2FireUntrackedNextMs = 0;
    }
    std::uint64_t g_poseV2FireRebuilt = 0;       // initial fires rebuilt into the clip
    std::uint64_t g_poseV2FireUntrackedSkipped = 0; // owned-able untracked fires left out (key off)
    std::uint64_t g_poseV2FirePtfxOwned = 0;     // untracked fires owned by a particle effect (left out)
    std::uint32_t g_poseV2FireUntrackedLogs = 0;
    std::uint8_t g_poseV2FireUntrackedSeen[replay::fire::kFireCount] = {}; // logged once per slot
    // Peds whose held state (blood, head gore, loose pieces) was taken when
    // they entered the recording.
    std::set<std::uint32_t> g_poseV2HeldStateTaken;
    std::uint64_t g_poseV2HeldBlood = 0;
    std::uint64_t g_poseV2HeldGoreBits = 0;
    std::uint64_t g_poseV2HeldPieces = 0;
    std::uint64_t g_poseV2FirePlayCursor = 0;
    std::uint64_t g_poseV2FirePlayed = 0;
    std::uint64_t g_poseV2FirePlayRefused = 0;
    std::uint64_t g_poseV2FireStopped = 0;
    std::uint64_t g_poseV2FireEngineEnded = 0;
    // Evidence: whether the puppet a recorded fire burns on is on fire for
    // the engine (IS_ENTITY_ON_FIRE), once a second, capped.
    std::uint32_t g_poseV2FireEntityLogs = 0;
    std::uint32_t g_poseV2FireEntityNextMs = 0;
    std::uint64_t g_poseV2FireReconciles = 0;
    std::uint64_t g_poseV2FireRemnantsCleared = 0;
    std::string g_poseV2FireLastError;
    // Bounded retry of recorded fires that did not start (engine refusal, or
    // a recorded entity whose puppet is not there yet), per start index; the
    // starts waiting for one; and the evidence counters.
    std::vector<replay::fire::StartRetry> g_poseV2FireRetry;
    std::vector<std::uint32_t> g_poseV2FirePending;
    std::uint64_t g_poseV2FireEntityWaits = 0;  // not started: entity not resolved
    std::uint64_t g_poseV2FireRetries = 0;      // attempts after a failure
    std::uint64_t g_poseV2FireGaveUp = 0;       // starts left out for this playback
    std::uint64_t g_poseV2FireParentCleared = 0; // desc +0xF0 parent pointers not replayed
    std::uint64_t g_poseV2FireIncremental = 0;  // reconciles that only started the missing ones
    // Fires the playhead has ended, started again only for their ground
    // state (start, restore, shut down) and paced by the engine's 128 fire
    // slots. Starting every due fire at once would overflow the 128 slots
    // and refuse the newest ones (the ones still burning at the end of the
    // clip).
    std::vector<std::uint32_t> g_poseV2FireEndedQueue;
    std::uint64_t g_poseV2FireDuplicates = 0; // starts refused because a live fire of ours had them
    std::uint32_t g_poseV2FireHoldLastMs = 0xFFFFFFFFu; // playhead of the last hold
    std::uint64_t g_poseV2FireEndedQueued = 0;  // ended starts sent to the queue
    // Per start, the time of its first recorded ground sample (none: max),
    // built once per clip.
    std::vector<std::uint32_t> g_poseV2FireFirstSampleMs;
    std::uint32_t g_poseV2FireSlotLogs = 0;
    std::uint32_t g_poseV2FireSkipLogs = 0;
    std::uint32_t g_poseV2FireReconcileLogs = 0;
    bool g_poseV2FirePlaying = false;
    float g_poseV2FireCenter[3] = {};
    float g_poseV2FireRadius = 0.0f;
    // The survivors are read on a later tick (a stopped fire can stay in
    // the list until the manager's next update): a pending check.
    ULONGLONG g_poseV2FireCheckAt = 0;
    // The fire fx sites and the two weapon fx sites (kFireAreaFxCall...,
    // kWeaponFxACall, kWeaponFxBCall) are in clip_sites.h: a clip names them.
    // The return RVA of a 5-byte call found by its signature (0: not found).
    std::uint32_t CallReturnRva(const sig::Signature& call)
    {
        const std::uintptr_t at = sig::Find(call);
        const std::uintptr_t base = sig::ImageBase();
        return at > base ? static_cast<std::uint32_t>(at + 5 - base) : 0;
    }
    std::uint64_t g_poseV2BloodCursor = 0;
    std::uint64_t g_poseV2BloodUnmapped = 0;
    std::uint64_t g_poseV2BloodNotCopied = 0;
    // Records kept through the owner cache, records on a recorded entity
    // that is not a ped (a burning wagon), capped evidence lines of unmapped
    // owners, and the owner addresses of the last tick.
    std::uint64_t g_poseV2BloodFromCache = 0;
    std::uint64_t g_poseV2BloodNotPed = 0;
    std::uint32_t g_poseV2BloodUnmappedLogs = 0;
    std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> g_poseV2BloodOwnerCache;
    std::map<std::uint32_t, std::uint32_t> g_poseV2BloodByCaller;
    std::uint32_t g_poseV2BloodLogged = 0;
    std::uint64_t g_poseV2ProjFromBlood = 0;
    replay::entity::ForwardEventCursor g_poseV2BloodCursorPlay;
    std::uint32_t g_poseV2BloodLastPlayhead = 0;
    std::map<std::uint32_t, std::uint32_t> g_poseV2BloodAppliedUpTo; // entity -> time of the last applied event
    // Entity -> the clone those records went on (a puppet created again has
    // none of them: its reconcile rebuilds, never forward only).
    std::map<std::uint32_t, int> g_poseV2BloodAppliedClone;
    std::vector<std::size_t> g_poseV2BloodPending;
    std::uint64_t g_poseV2BloodPlayed = 0;
    std::uint64_t g_poseV2BloodFailed = 0;
    std::uint64_t g_poseV2BloodPresent = 0;
    std::uint64_t g_poseV2BloodBusy = 0;
    std::uint64_t g_poseV2BloodReconciles = 0;
    std::string g_poseV2BloodLastError;
    // The rebuild after a jump is a queue run under a time budget per tick
    // (src/state_budget.h); the seek settle waits for it (StatePending), so
    // the state the wait ends on is the one a one-tick rebuild would make.
    // Diagnostics of the queue and of slow blood ticks.
    replay::budget::OpQueue g_poseV2BloodQueue;
    std::uint32_t g_poseV2StateBudgetUs = replay::budget::kPlayUs;
    std::uint32_t g_poseV2BloodQueueLogs = 0;
    std::uint32_t g_poseV2BloodSlowLogs = 0;       // POSE_V2_BLOOD_SLOW, per playback
    std::uint32_t g_poseV2BloodTickLogs = 0;       // POSE_V2_BLOOD_TICK, per playback
    namespace
    {

    bool g_poseV2GoreObserverReady = false;
    }
    std::uint64_t g_poseV2GoreHitCursor = 0;
    std::uint64_t g_poseV2GoreHitsOther = 0; // events on peds outside the recording
    replay::entity::ForwardEventCursor g_poseV2GoreHitCursorPlay;
    std::uint64_t g_poseV2GoreHitsPlayed = 0;
    std::uint64_t g_poseV2GoreHitsFailed = 0;
    std::uint64_t g_poseV2GoreHitsNoPuppet = 0;
    std::uint64_t g_poseV2GoreMaskSkipped = 0;
    namespace
    {
    // Evidence: a severed limb becomes a loose world entity; for 2 s after
    // each gore event every new object or ped within 8 m of the ped is
    // logged with its model, to learn what the engine creates.
    std::set<int> g_poseV2GorePieceSeen;
    }
    std::uint32_t g_poseV2GorePieceUntilMs = 0;
    Vector3 g_poseV2GorePieceCenter{};
    std::uint32_t g_poseV2GorePieceModel = 0;
    Entity g_poseV2GorePieceOwner = 0;
    namespace
    {

    // With the pool reader, the pieces scan walks the object and ped pools
    // in memory and remembers entity addresses; a handle is made only for an
    // entity within 8 m. Keyed by address and slot generation (the ref's
    // flag byte), so a new entity in a reused slot is new.
    std::unordered_set<std::uint64_t> g_poseV2GorePieceSeenAt;
    bool g_poseV2GorePiecePools = false;
    std::uint64_t PoseV2PieceKey(const replay::pools::Entry& entry)
    {
        return entry.address ^ (static_cast<std::uint64_t>(entry.ref & 0x7F) << 56);
    }

    void SeedPoseV2GorePieces()
    {
        g_poseV2GorePieceSeen.clear();
        g_poseV2GorePieceSeenAt.clear();
        g_poseV2GorePieceUntilMs = 0;
        {
            static std::vector<replay::pools::Entry> entries;
            const int objects = replay::entities::ReadPool(replay::pools::Kind::Object, entries);
            for (const replay::pools::Entry& entry : entries)
                g_poseV2GorePieceSeenAt.insert(PoseV2PieceKey(entry));
            const int peds = objects < 0 ? -1 : replay::entities::ReadPool(replay::pools::Kind::Ped, entries);
            for (const replay::pools::Entry& entry : entries)
                g_poseV2GorePieceSeenAt.insert(PoseV2PieceKey(entry));
            g_poseV2GorePiecePools = objects >= 0 && peds >= 0;
            if (g_poseV2GorePiecePools)
                return;
            g_poseV2GorePieceSeenAt.clear();
        }
        static int handles[2048];
        int count = replay::objects::ListObjects(handles, 2048);
        g_poseV2GorePieceSeen.insert(handles, handles + count);
        count = worldGetAllPeds(handles, 2048);
        g_poseV2GorePieceSeen.insert(handles, handles + count);
    }

    void ScanPoseV2GorePieces(std::uint32_t nowMs)
    {
        if (!g_poseV2GorePieceUntilMs || nowMs > g_poseV2GorePieceUntilMs)
            return;
        static int handles[2048];
        // The ScriptHook pool listing returns 0 in a large fight (<= 256 free
        // script handles), so a severed piece can go unseen. Probe the known
        // piece models by closest-of-type instead.
        if (replay::objects::ListObjects(handles, 1) == 0)
        {
            for (std::uint32_t model : {0x5595880bu, 0x4f6fa8e4u})
            {
                if (!replay::objects::IsGorePieceModel(model))
                    continue;
                // The three flags of the native filter by owner; the pieces are
                // not found with all FALSE: try each combination and log which
                // one sees the piece (evidence).
                Object piece = 0;
                int combo = -1;
                for (int bits = 0; bits < 8 && !piece; ++bits)
                {
                    piece = OBJECT::GET_CLOSEST_OBJECT_OF_TYPE(g_poseV2GorePieceCenter.x, g_poseV2GorePieceCenter.y,
                        g_poseV2GorePieceCenter.z, 2.0f, model, (bits & 1) ? TRUE : FALSE, (bits & 2) ? TRUE : FALSE,
                        (bits & 4) ? TRUE : FALSE);
                    if (piece)
                        combo = bits;
                }
                static std::uint32_t probeLogs = 0;
                if (probeLogs++ < 40)
                    Ctx().log << "POSE_V2_GORE_PIECE_PROBE,model=0x" << std::hex << model << std::dec << ",found=" << piece
                        << ",flags=" << combo << ",time_ms=" << nowMs << '\n';
                if (!piece || g_poseV2GorePieceSeen.count(piece) || !replay::entities::Exists(piece) ||
                    ENTITY::GET_ENTITY_ATTACHED_TO(piece) || ENTITY::IS_ENTITY_A_MISSION_ENTITY(piece) ||
                    nowMs + 1000 > g_poseV2GorePieceUntilMs)
                    continue;
                g_poseV2GorePieceSeen.insert(piece);
                const bool registered = replay::objects::RegisterGorePiece(piece, g_poseV2GorePieceOwner);
                Ctx().log << "POSE_V2_GORE_PIECE_CANDIDATE,kind=object,how=closest_of_type,handle=" << piece << ",model=0x"
                    << std::hex << model << std::dec << ",registered=" << registered << ",time_ms=" << nowMs << '\n';
            }
        }
        REPLAY_PROFILE_ZONE("rec.gore.pieces");
        for (int pass = 0; pass < 2; ++pass)
        {
            // The pools (see g_poseV2GorePieceSeenAt); the listings only when
            // the reader is not available.
            static std::vector<replay::pools::Entry> entries;
            int count = -1;
            if (g_poseV2GorePiecePools)
                count = replay::entities::ReadPool(pass == 0 ? replay::pools::Kind::Object : replay::pools::Kind::Ped, entries);
            const bool fromPool = count >= 0;
            if (!fromPool)
                count = pass == 0 ? replay::objects::ListObjects(handles, 2048) : worldGetAllPeds(handles, 2048);
            for (int n = 0; n < count; ++n)
            {
                int handle = 0;
                Vector3 p{};
                if (fromPool)
                {
                    const replay::pools::Entry& entry = entries[static_cast<std::size_t>(n)];
                    if (!g_poseV2GorePieceSeenAt.insert(PoseV2PieceKey(entry)).second)
                        continue;
                    const float ex = entry.position.x - g_poseV2GorePieceCenter.x;
                    const float ey = entry.position.y - g_poseV2GorePieceCenter.y;
                    const float ez = entry.position.z - g_poseV2GorePieceCenter.z;
                    if (ex * ex + ey * ey + ez * ez > 8.0f * 8.0f)
                        continue;
                    handle = replay::pools::HandleOf(pass == 0 ? replay::pools::Kind::Object : replay::pools::Kind::Ped, entry, true);
                    if (!handle || g_poseV2GorePieceSeen.count(handle))
                        continue;
                    g_poseV2GorePieceSeen.insert(handle);
                    p.x = entry.position.x;
                    p.y = entry.position.y;
                    p.z = entry.position.z;
                }
                else
                {
                    handle = handles[n];
                    if (!handle || g_poseV2GorePieceSeen.count(handle) || !replay::entities::Exists(handle))
                        continue;
                    g_poseV2GorePieceSeen.insert(handle);
                    p = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
                }
                const float dx = p.x - g_poseV2GorePieceCenter.x;
                const float dy = p.y - g_poseV2GorePieceCenter.y;
                const float dz = p.z - g_poseV2GorePieceCenter.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > 8.0f)
                    continue;
                const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(handle));
                // A loose object that appears next to the ped right after a
                // gore event is the severed piece: recorded as a prop.
                bool registered = false;
                if (pass == 0 && distance <= 1.5f && !ENTITY::GET_ENTITY_ATTACHED_TO(handle) &&
                    !ENTITY::IS_ENTITY_A_MISSION_ENTITY(handle) && nowMs + 1000 <= g_poseV2GorePieceUntilMs)
                {
                    registered = replay::objects::RegisterGorePiece(handle, g_poseV2GorePieceOwner);
                }
                Ctx().log << "POSE_V2_GORE_PIECE_CANDIDATE,kind=" << (pass == 0 ? "object" : "ped") << ",handle=" << handle
                    << ",model=0x" << std::hex << model << ",ped_model=0x" << g_poseV2GorePieceModel << std::dec
                    << ",same_model=" << (model == g_poseV2GorePieceModel) << ",distance=" << distance
                    << ",attached_to=" << ENTITY::GET_ENTITY_ATTACHED_TO(handle)
                    << ",mission=" << ENTITY::IS_ENTITY_A_MISSION_ENTITY(handle)
                    << ",address=0x" << std::hex << reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(handle)) << std::dec
                    << ",registered=" << registered << ",time_ms=" << nowMs << '\n';
            }
        }
    }
    }
    replay::entity::ForwardEventCursor g_poseV2GoreCursorPlay;
    std::uint64_t g_poseV2GorePlayed = 0;
    std::uint64_t g_poseV2GoreFailed = 0;
    std::uint64_t g_poseV2GoreNoPuppet = 0;
    std::string g_poseV2GoreLastError;
    std::uint64_t g_poseV2ProjCursor = 0;
    std::uint32_t g_poseV2ProjSamples = 0;
    std::uint64_t g_poseV2ProjNoEntity = 0;
    replay::entity::ForwardEventCursor g_poseV2ProjCursorPlay;
    std::uint64_t g_poseV2ProjPlayed = 0;
    std::uint64_t g_poseV2ProjFailed = 0;
    std::uint64_t g_poseV2ProjMissing = 0;
    std::uint64_t g_poseV2ProjNoInfo = 0;
    std::string g_poseV2ProjLastError;
    // Projected decals.
    std::uint64_t g_poseV2ProjFromRage = 0;      // record: rage mirrors skipped (the engine makes them again)
    std::uint32_t g_poseV2ProjFromRageLogs = 0;
    std::uint64_t g_poseV2ProjNullOwner = 0;     // playback: old clip events played with a null owner
    std::uint64_t g_poseV2ProjEngineCursor = 0;  // playback: the engine's own projected adds
    std::uint64_t g_poseV2ProjEngineSeen = 0;
    std::uint64_t g_poseV2ProjEngineMirrors = 0;
    std::uint32_t g_poseV2ProjEngineLogs = 0;
    // Rage decals with an entity but no attach (the mirror's owner).
    std::uint64_t g_poseV2DecalsWithEntity = 0;       // record
    std::uint64_t g_poseV2DecalsEntityPassed = 0;     // playback: entity found and passed
    std::uint64_t g_poseV2DecalsEntityMissing = 0;    // playback: entity not found, played without
    std::uint32_t g_poseV2DecalEntityLogs = 0;
    // Pools and decals aged to the playhead (decal_state.h).
    std::uint32_t g_poseV2PoolSamples = 0;
    std::uint64_t g_poseV2DecalsAgedInitial = 0;
    std::uint64_t g_poseV2DecalsExpiredInitial = 0;
    std::uint64_t g_poseV2DecalsAgedPlay = 0;
    std::uint64_t g_poseV2DecalsExpiredPlay = 0;
    std::uint32_t g_poseV2DecalAgeLogs = 0;
    // The recorded decals the engine let expire become removals.
    std::vector<std::uint32_t> g_poseV2RecordedAliveIds;
    std::set<std::uint32_t> g_poseV2RecordedAliveSet;
    std::size_t g_poseV2RecordedAliveAt = 0;
    std::uint64_t g_poseV2DecalsExpired = 0;
    std::uint32_t g_poseV2DecalExpiredLogs = 0;
    std::size_t g_poseV2EngineFxOwnerCount = 0;
    // Evolution of a blood record on the body (the blood evolve's return).
    std::uint64_t g_poseV2BloodEvolved = 0;
    std::uint64_t g_poseV2BloodEvolveFailed = 0;
    std::uint64_t g_poseV2BloodEvolvePresent = 0;
    std::uint32_t g_poseV2BloodEvolveLogs = 0;
    std::uint64_t g_poseV2DecalRemovesUnmapped = 0;
    // Evidence: every particle trigger the observer saw in a phase, by effect
    // hash (recording versus playback, the particle battery baseline).
    std::uint64_t g_poseV2FxPhaseStartUs = 0;

    // The fx check (fx_check_core.h) and the one-shot owner rule.
    replay::fxcheck::Tally g_fxCheck;
    std::size_t g_fxCheckRecordedEvents = 0;   // the recorded clip's one-shot events and tracks at its stop
    std::size_t g_fxCheckRecordedTracks = 0;
    std::uint64_t g_speechOnPlayer = 0;
    std::uint64_t g_speechPlayerFar = 0;       // lines of the recorded player kept on the puppet
    std::uint32_t g_speechPlayerFarLogs = 0;
    // The hold of the hidden player at the recorded player's puppet for a line
    // of his that names its voice (speech_player_route.h HoldAtPuppet): the
    // follow's last report (script.cpp UpdatePlayerFollow), the held puppet and
    // player, when the hold began (GetTickCount64, 0 = none), whether a held
    // line is still alive, the held lines, those heard (audible start) and the
    // log cap.
    bool g_playerFollowMoving = false;
    ULONGLONG g_playerFollowAtMs = 0;
    Ped g_speechHoldPuppet = 0;
    Ped g_speechHoldPlayer = 0;
    std::uint64_t g_speechHoldSinceMs = 0;
    bool g_speechHoldLineAlive = false;
    std::uint64_t g_speechHeld = 0;
    std::uint64_t g_speechHeldStarted = 0;
    std::uint32_t g_speechHeldLogs = 0;
    // The recorded player's lines given to his puppet because the live player
    // is another character (speech_player_route.h SameCharacter).
    std::uint64_t g_speechCharacterMismatch = 0;
    std::uint32_t g_speechCharacterMismatchLogs = 0;
    void ReleasePlayerSpeechHold()
    {
        g_speechHoldPuppet = 0;
        g_speechHoldPlayer = 0;
        g_speechHoldSinceMs = 0;
        g_speechHoldLineAlive = false;
    }
    // Lines the engine did not create on the hidden player, asked again on
    // the puppet (speech_player_route.h RetryOnPuppet), and how many of
    // those the puppet got.
    std::uint64_t g_speechPlayerRefused = 0;
    std::uint64_t g_speechPlayerRefusedPuppetCreated = 0;
    std::uint32_t g_speechPlayerRefusedLogs = 0;
    // The recorded player's lines that named their voice live, asked on the
    // hidden player with that voice in the request (speech_player_route.h
    // NamedVoiceOnPlayer), and how many the engine created there.
    std::uint64_t g_speechPlayerNamedVoice = 0;
    std::uint64_t g_speechPlayerNamedVoiceCreated = 0;
    std::uint32_t g_speechPlayerNamedVoiceLogs = 0;
    // Human lines refused with the conversation fields, asked again at once
    // without them, and created.
    std::uint64_t g_speechConversationPlainRetries = 0;
    std::uint64_t g_speechConversationPlainCreated = 0;
    std::uint32_t g_speechConversationPlainLogs = 0;
    std::uint32_t g_speechPlayerPacked = 0xFFFFFFFFu; // the recorded player's id (SGEN player-class owner)
    // The scripted lines on the script path (SpeechScriptPath,
    // speech_script_path.h) and the recorded player's scripted lines said by
    // the puppet, frontend at the game camera (SpeechPlayerScriptNear);
    // counters for the playback's summary.
    // Every replayed human line asked with its recorded duck byte and sound
    // set (SpeechConversationLive, default 1, speech_conversation.h); the
    // playback's counts.
    bool g_speechConversationLive = true;
    replay::speech::conversation::Tally g_speechConversationTally;
    std::uint32_t g_speechConversationLogs = 0;

    // Particle fix switches and their evidence.
    replay::events::FxParticlesConfig g_fxCfg;
    std::uint32_t g_fxSpanId = 1;                 // a stretch of play: a jump, a scrub or a new playback starts another
    std::vector<std::uint32_t> g_fxTrackCreatedSpan; // per registered track: the stretch of its last creation (0 never)
    // Per registered track, the stretch its play was last counted in the fx
    // check (0 never). The clip basis counts a track once per traversed
    // stretch it was alive in (EndPoseV2FxCheck); the play side must count
    // the same way, or a jump back or a step past the cursor's limit leaves
    // the later stretches as missing.
    std::vector<std::uint32_t> g_fxTrackPlaySpan;
    // How each script looped track was played (script_fx_loop.h,
    // FX_SCRIPT_LOOP_CHECK at the end of the playback).
    std::vector<replay::scriptfx::LoopTrackPlay> g_scriptLoopPlay;
    bool ScriptLoopSite(std::uint32_t site)
    {
        return site == replay::fx::ScriptLoopStartSite() || site == replay::fx::ScriptLoopKeepAliveSite();
    }
    std::uint32_t g_fxBlinkLogsByHash[4] = {};    // FX_BLINK lines of the first hashes (with g_fxBlinkHashes)
    // Per registered track, per stretch without a call, what the playback did
    // there (fx_motion_rule.h GapSeen); the creations right after a stretch
    // counted as its restart; the stretches missed.
    std::vector<std::vector<std::uint8_t>> g_fxGapSeen;
    std::uint64_t g_fxGapCreates = 0;
    // WeaponFxIntent: the weapon system's requests for a shot's muzzle flash
    // and shell, drained from the detours' ring while recording (section
    // WFXI). The clip's shots as the audit sees them (built at the
    // playback's start, judged per traversed span at its stop).
    std::vector<replay::weaponfx::ShotRow> g_fxShotRows;
    namespace
    {
    bool g_fxWeaponIntentTried = false;
    }
    bool g_fxWeaponIntentReady = false;
    namespace
    {
    std::string g_fxWeaponIntentError;
    std::uint64_t g_fxWeaponIntentCursor = 0;
    struct FxWeaponIntentStats
    {
        std::uint64_t muzzle = 0;
        std::uint64_t shell = 0;
        std::uint64_t stored = 0;
        std::uint64_t shooterUnknown = 0; // the shooter is not a recorded entity (kept with id 0)
        std::uint64_t weaponUnknown = 0;  // the weapon object is not a recorded weapon (kept with id 0)
        std::uint64_t infoUnread = 0;
        std::uint64_t paused = 0;         // drained while the recording clock was paused (dropped)
        std::uint64_t refused = 0;        // invalid or over the cap
        std::uint32_t logs = 0;
    } g_fxWeaponIntent;
    }
    // The requests played back (PlayWeaponFxIntents) and the recorded
    // effects they replace, per clip fx event and per registered track
    // (built in BeginFxParticles; a clip without WFXI drops nothing).
    bool g_wfxPlayOn = false;
    std::vector<std::uint8_t> g_wfxDropTrack;
    std::uint64_t g_wfxDroppedTracksPlayed = 0; // shell tracks not played
    std::uint32_t g_fxBlinkHashes[4] = {};
    std::vector<std::vector<replay::entity::RegisteredFxGap>> g_fxDerivedGaps; // per track, clips without RGSS
    std::uint32_t g_fxRecreateLogs = 0;
    std::uint64_t g_fxRecreateInSpan = 0, g_fxRecreateReentered = 0, g_fxRecreateNotReady = 0;
    std::uint64_t g_fxOwnerNotReadyTicks = 0, g_fxBackoffTicks = 0;
    std::uint64_t g_fxGapTicks = 0, g_fxGapsEntered = 0;
    std::uint64_t g_fxSettingsApplied = 0, g_fxSettingsFailed = 0, g_fxSettingsLegacyHoof = 0;
    std::string g_fxSettingsError;
    std::uint32_t g_fxSettingsDiffLogs = 0;
    std::uint64_t g_fxParentOnceSkipped = 0;
    std::uint64_t g_fxBlinks = 0;
    std::uint32_t g_fxBlinkLogs = 0;
    std::uint64_t g_fxPlayRestarts = 0;
    std::uint32_t g_fxRelocateProbeLogs = 0;
    std::uint32_t g_fxVeladdProbeLogs = 0;
    std::uint64_t g_fxRelocateCleared = 0, g_fxRelocateKept = 0, g_fxRelocateCalls = 0, g_fxRelocateFailed = 0;
    std::uint64_t g_fxRelocateStale = 0;  // recycled instances left alone (other effect or not playing)
    std::uint64_t g_fxGapEndSkipped = 0;  // entries ended inside a call gap, not stopped by pointer
    std::string g_fxRelocateError;
    bool g_fxVelocityReady = false;
    bool g_fxPauseFreezeReady = false;
    std::uint64_t g_fxWorldOwnerRefusals = 0;
    std::uint64_t g_fxOwnerListImmediate = 0;
    std::size_t g_fxLastPuppetClones = 0;
    std::vector<std::uint64_t> g_fxWorldOwnersLive; // world objects our live tracks ride
    std::vector<FxOwned> g_fxOwned;
    std::unordered_map<std::uint64_t, FxOwnerAt> g_fxOwnerLastAt;
    std::uint64_t g_fxRelocateClearedStill = 0, g_fxRelocateKeptUnplaced = 0;
    std::uint32_t g_fxUnplacedLogs = 0;
    std::map<std::uint32_t, std::uint32_t> g_fxUnplacedLogsByHash;
    std::vector<std::pair<std::uint64_t, float>> g_fxPlacedThisTick;
    std::vector<FxProbeBefore> g_fxProbeBefore;
    std::vector<FxPendingCheck> g_fxPendingChecks;
    // The shot track's weapon effects.
    bool g_fxClipWeaponEvents = false;
    std::vector<replay::shotfx::Decision> g_fxShotDecisions;
    std::uint32_t g_fxImpactOnBodyLogs = 0;
    std::uint64_t g_fxShotResolveShots = 0, g_fxShotMuzzleStarts = 0, g_fxShotImpactStarts = 0, g_fxShotBodyStarts = 0;
    std::uint64_t g_fxShotClaimedMulti = 0, g_fxShotNoMuzzle = 0, g_fxShotClaimed = 0;
    std::map<std::uint64_t, std::uint32_t> g_fxShotTriggerClaims; // trigger (time ^ hash) -> shots that took it
    namespace
    {
    std::uint32_t g_fxShotFxLogs = 0;
    }
    std::uint32_t g_poseV2RegisteredPrevTickMs = 0; // the recording's previous tick
    std::vector<RegNotCreated> g_fxRegReason;
    std::uint64_t g_fxRegRecreated = 0; // our registered instances made again for a track already created

    void LogPoseV2FxHistogram(const char* phase)
    {
        std::vector<replay::fx::TriggerCall> calls;
        replay::fx::Collect(g_poseV2FxPhaseStartUs, replay::fx::NowUs(), calls);
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> histogram; // calls, instances
        for (const replay::fx::TriggerCall& call : calls)
        {
            std::pair<std::uint32_t, std::uint32_t>& entry = histogram[call.effectHash];
            ++entry.first;
            if (call.instance)
                ++entry.second;
        }
        std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>> sorted(
            histogram.begin(), histogram.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        std::ostream& log = Ctx().log;
        log << "POSE_V2_FX_HISTOGRAM,phase=" << phase << ",calls=" << calls.size()
            << ",effects=" << histogram.size() << ",top=";
        std::size_t shown = 0;
        for (const auto& entry : sorted)
        {
            if (shown++ == 24)
                break;
            log << (shown > 1 ? ";" : "") << "0x" << std::hex << entry.first << std::dec << ":"
                << entry.second.first << "/" << entry.second.second;
        }
        log << '\n';
        log.flush();
    }
    bool g_poseV2FxPlayerReady = false;

    // Playback side of the shot events.
    replay::entity::ForwardEventCursor g_poseV2SpeechCursorPlay;
    std::uint64_t g_poseV2SpeechPlayed = 0;
    std::vector<PoseV2QueuedLine> g_poseV2QueuedLines;
    int g_poseV2QueuedLineBudget = 0;
    std::uint64_t g_poseV2CoreCursor = 0;
    std::uint64_t g_poseV2SpeechCancelled = 0;
    replay::entity::ForwardEventCursor g_poseV2ShotCursor;
    std::uint64_t g_poseV2ShotsPlayed = 0;
    std::uint64_t g_poseV2ShotsCancelled = 0;

    replay::pose::Vec3 ToPoseVec(const Vector3& v)
    {
        return {v.x, v.y, v.z};
    }
    namespace
    {

    void RecordPoseV2Shot(const replay::recorder::RecordedEntity& owner, PoseV2ShooterState& state, const char* source,
        int hand = 0, const replay::audio::FireCall* fire = nullptr)
    {
        replay::entity::ShotEvent shot;
        shot.timeMs = replay::pose::capture::NowMs();
        shot.shooter = owner.id;
        shot.weaponHash = state.weapon;
        Hash handWeapon = 0;
        if (hand != 0 && WEAPON::GET_CURRENT_PED_WEAPON(owner.handle, &handWeapon, FALSE, hand, FALSE) && handWeapon)
            shot.weaponHash = handWeapon;
        const Entity object = WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(owner.handle, hand);
        bool muzzleBone = false;
        if (object && replay::entities::Exists(object))
        {
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            {
                if (weapon.object == object)
                    shot.weapon = weapon.id;
            }
            Vector3 muzzle{};
            muzzleBone = replay::pose::playback::WeaponMuzzle(object, muzzle);
            if (!muzzleBone)
                muzzle = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            shot.muzzle = ToPoseVec(muzzle);
            const Vector3 origin = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(object, 0.0f, 0.0f, 0.0f);
            const Vector3 axis = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(object, 1.0f, 0.0f, 0.0f);
            shot.direction = {axis.x - origin.x, axis.y - origin.y, axis.z - origin.z};
        }
        else
        {
            shot.muzzle = ToPoseVec(PED::GET_PED_BONE_COORDS(owner.handle, 0, 0.0f, 0.0f, 0.0f));
            shot.direction = ToPoseVec(ENTITY::GET_ENTITY_FORWARD_VECTOR(owner.handle));
        }
        // The local player's shots play the player weapon sound (see
        // replay::audio::InstallPlayerWeaponAudio). audio_flag is the
        // engine's own answer for this ped (read-only replica), evidence
        // that the weapon audio took the player variant live.
        shot.shooterPlayer = owner.handle == PLAYER::PLAYER_PED_ID() ? 2 : 1;
        const int audioPlayerFlag = replay::audio::PedAudioPlayerFlag(replay::entities::AddressOf(owner.handle));
        state.pendingEvent = Ctx().clip.shots.Add(shot);
        if (fire) // the game's fire event
            NoteShotAudioFire(shot.shooter, fire->info, fire->timeUs, shot.timeMs, shot.muzzle, shot.shooterPlayer == 2);
        if (g_poseV2FxObserverReady)
        {
            PoseV2FxWindow window;
            window.shot = state.pendingEvent;
            window.shotUs = replay::fx::NowUs();
            BYTE* shooterAddress = replay::entities::AddressOf(owner.handle);
            window.shooter = reinterpret_cast<std::uint64_t>(shooterAddress);
            window.shooterAudio = replay::audio::PedAudio(shooterAddress);
            window.weapon = object && replay::entities::Exists(object)
                ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object))
                : 0;
            if (fire)
            {
                window.fire = *fire;
                window.hasFire = true;
            }
            // The window is part of the shot record: its flush resolves the
            // muzzle and impact effects, the muzzle flags and the fire audio
            // of the shot. Gating it off leaves every shot without muzzle
            // flash and fire audio.
            g_poseV2FxWindows.push_back(window);
        }
        OpenPoseV2AudioWindow("shot:" + std::to_string(state.pendingEvent), 100000, 150000);
        state.pendingImpact = true;
        state.pendingSinceMs = shot.timeMs;

        std::ostream& log = Ctx().log;
        log << "POSE_V2_SHOT"
            << ",shooter=" << replay::entity::IdText(shot.shooter)
            << ",weapon_entity=" << (shot.weapon.Valid() ? replay::entity::IdText(shot.weapon) : std::string("none"))
            << ",weapon=0x" << std::hex << shot.weaponHash << std::dec
            << ",source=" << source
            << ",muzzle_bone=" << (muzzleBone ? 1 : 0)
            << ",shooter_player=" << static_cast<int>(shot.shooterPlayer)
            << ",audio_flag=" << audioPlayerFlag
            << ",clip=" << state.clip
            << ",time_ms=" << shot.timeMs
            << '\n';
        LogShotGunfight("record", shot.shooter, replay::entities::AddressOf(owner.handle), shot.timeMs);
        log.flush();
    }

    // record=false while the recording gate holds: the engine fire queue is
    // drained and the per-ped baselines (weapon, clip, shooting, last
    // impact) follow the game, but no shot is recorded.
    void RecordPoseV2Shots(bool record = true)
    {
        REPLAY_PROFILE_ZONE("rec.shots");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        // With the fire event observer the shots of every hand come from
        // the engine fire queue (a shot is recorded at its fire event).
        g_poseV2FireShotsSource = g_poseV2AudioObserverReady;
        if (g_poseV2FireShotsSource)
            replay::audio::CollectFiresSince(g_poseV2FireCursor, g_poseV2FrameFires);
        g_poseV2Shooters.erase(std::remove_if(g_poseV2Shooters.begin(), g_poseV2Shooters.end(),
            [](const PoseV2ShooterState& state) {
                return std::none_of(Ctx().recorder.entities.begin(), Ctx().recorder.entities.end(),
                    [&](const replay::recorder::RecordedEntity& entity) { return entity.handle == state.ped; });
            }), g_poseV2Shooters.end());
        for (const replay::recorder::RecordedEntity& owner : Ctx().recorder.entities)
        {
            if (owner.vehicle)
                continue;
            auto state = std::find_if(g_poseV2Shooters.begin(), g_poseV2Shooters.end(),
                [&](const PoseV2ShooterState& candidate) { return candidate.ped == owner.handle; });
            const bool known = state != g_poseV2Shooters.end();
            if (!known)
            {
                g_poseV2Shooters.push_back({owner.handle});
                state = g_poseV2Shooters.end() - 1;
            }

            Hash weapon = 0;
            WEAPON::GET_CURRENT_PED_WEAPON(owner.handle, &weapon, FALSE, 0, FALSE);
            int clip = -1;
            if (!WEAPON::GET_AMMO_IN_CLIP(owner.handle, &clip, weapon))
                clip = -1;
            const bool shooting = PED::IS_PED_SHOOTING(owner.handle) != FALSE;
            Vector3 impact{};
            const bool hasImpact = WEAPON::GET_PED_LAST_WEAPON_IMPACT_COORD(owner.handle, &impact) != FALSE;

            // Compared before this frame updates the baseline.
            const bool newImpact = hasImpact && (!state->hasLastImpact ||
                std::fabs(impact.x - state->lastImpact.x) + std::fabs(impact.y - state->lastImpact.y) +
                    std::fabs(impact.z - state->lastImpact.z) > 0.01f);
            int shots = 0;
            const char* source = "none";
            if (known && weapon == state->weapon && clip >= 0 && state->clip >= 0)
            {
                if (clip < state->clip)
                {
                    shots = (std::min)(state->clip - clip, kPoseV2MaxShotsPerFrame);
                    source = "clip";
                }
            }
            else if (known && shooting && !state->shooting)
            {
                shots = 1;
                source = "edge";
            }
            state->weapon = weapon;
            state->clip = clip;
            state->shooting = shooting;
            if (hasImpact)
            {
                state->lastImpact = impact;
                state->hasLastImpact = true;
            }
            if (!record)
            {
                state->pendingImpact = false;
                continue;
            }
            if (g_poseV2FireShotsSource)
            {
                // Shots come from the engine fire events (either hand); the
                // clip only keeps the impact bookkeeping above.
                shots = 0;
                const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(owner.handle));
                for (const replay::audio::FireCall& fire : g_poseV2FrameFires)
                {
                    std::uint64_t shooterRef = 0;
                    std::memcpy(&shooterRef, fire.info + 0x10, 8);
                    if (!address || shooterRef != address)
                        continue;
                    // Which hand's weapon object holds the event's weapon
                    // reference (+0x00)?
                    std::uint64_t weaponRef = 0;
                    std::memcpy(&weaponRef, fire.info, 8);
                    int hand = 0;
                    for (int point = 0; point < 2 && weaponRef; ++point)
                    {
                        const Entity object = WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(owner.handle, point);
                        if (!object || !replay::entities::Exists(object))
                            continue;
                        static std::uint64_t words[0x200];
                        if (!replay::audio::ReadBytes(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(object)),
                                words, sizeof(words)))
                        {
                            continue;
                        }
                        if (std::find(std::begin(words), std::end(words), weaponRef) != std::end(words))
                        {
                            hand = point;
                            break;
                        }
                    }
                    RecordPoseV2Shot(owner, *state, hand ? "fire_event_left" : "fire_event", hand, &fire);
                }
            }
            for (int shot = 0; shot < shots; ++shot)
                RecordPoseV2Shot(owner, *state, source);
            // A new impact coordinate (same frame or shortly after) belongs
            // to the latest shot.
            if (state->pendingImpact)
            {
                if (newImpact && known)
                {
                    replay::entity::ShotEvent& shot = Ctx().clip.shots.At(state->pendingEvent);
                    shot.impact = ToPoseVec(impact);
                    shot.hasImpact = true;
                    MeasurePoseV2ImpactNormal(shot, owner.handle);
                    state->pendingImpact = false;
                    ++g_poseV2ShotImpacts;
                }
                else if (timeMs - state->pendingSinceMs > kPoseV2ImpactWindowMs)
                {
                    state->pendingImpact = false;
                }
            }
        }
    }

    // discard=true while the recording gate holds: a fire that starts in the
    // pause is not recorded (and its slot is not tracked); a recorded fire that
    // goes out in it stops at the cut instant (the frozen clock), so it does not
    // burn to the end of the clip.
    void RecordPoseV2Fires(bool discard = false)
    {
        if (g_poseV2FireInitialPending && !discard)
        {
            g_poseV2FireInitialPending = false;
            RecordPoseV2InitialFires();
            g_poseV2FireUntrackedNextMs = replay::pose::capture::NowMs() + 1000;
        }
        if (!discard)
            SamplePoseV2FireItems(replay::pose::capture::NowMs());
        // Late entry, once a second.
        if (!discard && g_poseV2FireReady && replay::pose::capture::NowMs() >= g_poseV2FireUntrackedNextMs)
        {
            g_poseV2FireUntrackedNextMs = replay::pose::capture::NowMs() + 1000;
            RecordPoseV2UntrackedFires(replay::pose::capture::NowMs(),
                ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE));
        }
        static std::vector<replay::fire::Call> calls;
        replay::fire::CollectSince(g_poseV2EngineFireCursor, calls);
        if (calls.empty())
            return;
        const std::uint64_t now = replay::fire::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        for (const replay::fire::Call& call : calls)
        {
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            const std::uint32_t timeMs = discard ? nowMs : replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            if (discard && call.kind == replay::fire::CallKind::Start)
            {
                g_poseV2FireActiveSlots.erase(call.slot);
                ++g_poseV2GateDiscarded;
                continue;
            }
            switch (call.kind)
            {
            case replay::fire::CallKind::Refused:
                ++g_poseV2FireRefused;
                break;
            case replay::fire::CallKind::Start:
            {
                replay::entity::FireStartEvent event;
                std::memcpy(event.position, call.state + 0x30, sizeof(event.position));
                const float dx = event.position[0] - player.x;
                const float dy = event.position[1] - player.y;
                if (dx * dx + dy * dy > 150.0f * 150.0f)
                {
                    ++g_poseV2FireFar;
                    g_poseV2FireActiveSlots.erase(call.slot);
                    break;
                }
                event.timeMs = timeMs;
                event.slot = call.slot;
                std::memcpy(&event.type, call.state + 0x9C, sizeof(event.type));
                std::memcpy(&event.group, call.state + 0x5C, sizeof(event.group));
                std::memcpy(&event.rng, call.state + 0x1C8, sizeof(event.rng));
                std::memcpy(event.desc, call.desc, sizeof(event.desc));
                ZeroFireDescPointers(event);
                std::uint64_t a = 0;
                std::uint64_t b = 0;
                std::memcpy(&a, call.desc + 0x40, sizeof(a));
                std::memcpy(&b, call.desc + 0x48, sizeof(b));
                event.entityA = RecordedIdAtAddress(a);
                event.entityB = RecordedIdAtAddress(b);
                TakeFireWorldEntity(a, event.entityA, event.entityAModel, event.entityAPos);
                TakeFireWorldEntity(b, event.entityB, event.entityBModel, event.entityBPos);
                event.caller = call.caller;
                const std::size_t index = Ctx().clip.fireStarts.Add(event);
                g_poseV2FireActiveSlots[call.slot] = static_cast<std::uint32_t>(index);
                g_poseV2FireOwners[call.fire] = static_cast<std::uint32_t>(index);
                if (index < 12)
                    Ctx().log << "POSE_V2_FIRE_START,index=" << index << ",slot=" << call.slot << ",type=" << event.type
                        << ",group=" << event.group << ",pos=" << event.position[0] << ";" << event.position[1] << ";" << event.position[2]
                        << ",entity_a=" << (a ? 1 : 0) << ",entity_a_id=" << (event.entityA.Valid() ? replay::entity::IdText(event.entityA) : std::string("none"))
                        << std::hex << ",entity_a_model=0x" << event.entityAModel << std::dec
                        << ",caller=0x" << std::hex << call.caller << std::dec << ",time_ms=" << timeMs << '\n';
                break;
            }
            case replay::fire::CallKind::Stop:
            {
                const auto found = g_poseV2FireActiveSlots.find(call.slot);
                if (found == g_poseV2FireActiveSlots.end())
                    break;
                replay::entity::FireStopEvent event;
                event.timeMs = timeMs;
                event.start = found->second;
                event.instant = call.instant;
                event.arg3 = call.arg3;
                Ctx().clip.fireStops.Add(event);
                g_poseV2FireActiveSlots.erase(found);
                break;
            }
            case replay::fire::CallKind::RemnantAdd:
            {
                ++g_poseV2FireRemnantAdds;
                const auto source = g_poseV2FireOwners.find(call.fire);
                if (source != g_poseV2FireOwners.end() && call.remnant >= 0)
                    g_poseV2FireOwners[replay::fire::RemnantAddress(call.remnant)] = source->second;
                break;
            }
            case replay::fire::CallKind::RemnantRemove:
                ++g_poseV2FireRemnantRemoves;
                break;
            }
        }
    }

    void RecordPoseV2Gore()
    {
        REPLAY_PROFILE_ZONE("rec.gore");
        RecordPoseV2HeldState(replay::pose::capture::NowMs());
        RecordPoseV2GoreHits();
        RecordPoseV2Blood();
        RecordPoseV2Fires();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        ScanPoseV2GorePieces(nowMs);
        static const replay::prof::ZoneId pollZone = replay::prof::Register("rec.gore.poll");
        const replay::prof::Scope pollScope(pollZone);
        constexpr int kLimbCount = static_cast<int>(sizeof(replay::gore::kLimbs) / sizeof(int));
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !replay::entities::IsPed(entity.handle))
                continue;
            // The engine's gore entry read from memory (one table read per frame, see
            // entity_cache.h) gives what the nine _HAS_PED_TAKEN_GORE_DAMAGE calls give,
            // bit for bit; a sample is still compared with the native, and the native
            // poll comes back when the table is not usable.
            std::uint64_t mask = 0;
            std::uint32_t engineMask = 0;
            bool memoryGore = false;
            const bool hasEntry =
                replay::entities::GoreMask(replay::entities::AddressOf(entity.handle), engineMask, memoryGore);
            if (memoryGore)
            {
                for (int n = 0; n < kLimbCount; ++n)
                    if (hasEntry && replay::entities::GoreHas(engineMask, replay::gore::kLimbs[n]))
                        mask |= 1ull << n;
                if (replay::entities::GoreCheckDue())
                {
                    std::uint64_t native = 0;
                    for (int n = 0; n < kLimbCount; ++n)
                        if (PED::_HAS_PED_TAKEN_GORE_DAMAGE(entity.handle, replay::gore::kLimbs[n]))
                            native |= 1ull << n;
                    replay::entities::GoreNote(native == mask, entity.handle, static_cast<int>(native ^ mask));
                    mask = native;
                }
            }
            else
            {
                for (int n = 0; n < kLimbCount; ++n)
                    if (PED::_HAS_PED_TAKEN_GORE_DAMAGE(entity.handle, replay::gore::kLimbs[n]))
                        mask |= 1ull << n;
            }
            if (!mask)
            {
                // No map entry for a ped that never had gore.
                const auto seen = g_poseV2GoreMasks.find(entity.id.Packed());
                if (seen == g_poseV2GoreMasks.end() || !seen->second)
                    continue;
            }
            std::uint64_t& known = g_poseV2GoreMasks[entity.id.Packed()];
            const std::uint64_t added = mask & ~known;
            known = mask;
            for (int n = 0; n < static_cast<int>(sizeof(replay::gore::kLimbs) / sizeof(int)); ++n)
            {
                if (!(added & (1ull << n)))
                    continue;
                replay::entity::GoreEvent event;
                event.timeMs = nowMs;
                event.entity = entity.id;
                event.limb = replay::gore::kLimbs[n];
                Ctx().clip.gore.Add(event);
                Ctx().log << "POSE_V2_GORE,id=" << replay::entity::IdText(entity.id) << ",limb=" << event.limb
                    << ",time_ms=" << nowMs << '\n';
            }
        }
    }
    }

    void Attach(Context& context)
    {
        g_context = &context;
        // The fire observer follows the live fires from mod load: a
        // recording that starts next to a fire lit earlier needs its
        // descriptor (the other observers stay lazy).
        std::string fireError;
        g_poseV2FireReady = replay::fire::Install(fireError);
        Ctx().log << "POSE_V2_FIRE_OBSERVER,status=" << (g_poseV2FireReady ? "installed" : "failed")
            << ",error=" << (fireError.empty() ? "none" : fireError.c_str()) << ",at=attach" << '\n';
        // The decal observer too: the monitor keeps the live decals.
        std::string decalError;
        g_poseV2DecalObserverTried = true;
        g_poseV2DecalObserverReady = replay::decal::Install(decalError);
        replay::decal::SetEnabled(g_poseV2DecalObserverReady);
        g_poseV2MonitorAddCursor = replay::decal::TotalCalls();
        g_poseV2MonitorRemoveCursor = replay::decal::TotalRemoves();
        Ctx().log << "POSE_V2_DECAL_OBSERVER,status=" << (g_poseV2DecalObserverReady ? "installed" : "failed")
            << ",error=" << (decalError.empty() ? "none" : decalError.c_str()) << ",at=attach,monitor=1"
            << ",drain_gate=" << replay::decal::DrainGateInstalled() << '\n';
    }

    void Monitor()
    {
        VerifyPoseV2PlayerWeaponBank(); // the loadout after a playback's restore
        replay::census::Idle(Ctx());
        replay::mixprobe::Idle(Ctx());
        replay::volumeprobe::Idle(Ctx());
        replay::worldsoundprobe::Idle(Ctx());
        // The render target detours' ring, every tick (the live state); filed on
        // the clip while a recording runs.
        replay::rendertarget::Monitor(true, replay::pose::capture::NowMs(), Ctx().clip.renderTargets, Ctx().log);
        replay::mixscene::Idle(Ctx());
        REPLAY_PROFILE_ZONE("monitor.decals");
        // Adds made by a playback are the replay's own, not world state.
        MonitorPoseV2Decals(!Ctx().transport.Playing());
        // Never refuse the engine's effects outside a playback (a playback that
        // ended without EndPlayback leaves no stale owners).
        if (g_poseV2EngineFxOwnerCount && !Ctx().transport.Playing())
        {
            replay::fx::SetSuppressedOwners({});
            g_poseV2EngineFxOwnerCount = 0;
        }
    }

    // Called at mod load inside the hook batch (one freeze for every detour)
    // and again, as a no-op, when a recording starts.
    void WarmUp()
    {
        // The burn level reads and writes (burn_level.h) are checked once; a
        // mismatch disables the burn level track.
        static bool burnLevelChecked = false;
        if (!burnLevelChecked)
        {
            burnLevelChecked = true;
            std::string burnError;
            const bool burnReady = replay::burn::Check(burnError);
            Ctx().log << "POSE_V2_BURN_LEVEL,status=" << (burnReady ? "enabled" : "disabled")
                << ",error=" << (burnError.empty() ? "none" : burnError.c_str()) << ",engine_writes=playback" << '\n';
            // The fire record (the burnt look).
            std::string charError;
            const bool charReady = replay::burn::CheckChar(charError);
            Ctx().log << "POSE_V2_FIRE_CHAR,status=" << (charReady ? "enabled" : "disabled")
                << ",error=" << (charError.empty() ? "none" : charError.c_str())
                << ",create=" << (replay::burn::CharCreateEnabled() ? 1 : 0) << '\n';
        }
        if (!g_poseV2FxObserverTried)
        {
            g_poseV2FxObserverTried = true;
            std::string error;
            g_poseV2FxObserverReady = replay::fx::Install(error);
            {
                std::string decalError;
                g_poseV2DecalObserverTried = true;
                g_poseV2DecalObserverReady = replay::decal::Install(decalError);
                std::string goreError;
                g_poseV2GoreObserverReady = replay::gore::InstallObserver(goreError);
                std::string fireError;
                g_poseV2FireReady = replay::fire::Install(fireError);
                Ctx().log << "POSE_V2_FIRE_OBSERVER,status=" << (g_poseV2FireReady ? "installed" : "failed")
                    << ",error=" << (fireError.empty() ? "none" : fireError.c_str()) << ",engine_writes=0" << '\n';
                std::string bloodError;
                g_poseV2BloodObserverReady = replay::blood::InstallObserver(bloodError);
                Ctx().log << "POSE_V2_BLOOD_OBSERVER,status=" << (g_poseV2BloodObserverReady ? "installed" : "failed")
                    << ",error=" << (bloodError.empty() ? "none" : bloodError.c_str()) << ",engine_writes=0" << '\n';
                Ctx().log << "POSE_V2_GORE_OBSERVER,status=" << (g_poseV2GoreObserverReady ? "installed" : "failed")
                    << ",error=" << (goreError.empty() ? "none" : goreError.c_str()) << ",engine_writes=0" << '\n';
                Ctx().log << "POSE_V2_DECAL_OBSERVER,status=" << (g_poseV2DecalObserverReady ? "installed" : "failed")
                    << ",error=" << (decalError.empty() ? "none" : decalError.c_str()) << ",engine_writes=0"
                    << ",drain_gate=" << replay::decal::DrainGateInstalled() << '\n';
            }
            std::ostream& log = Ctx().log;
            // The weapon fx requests, record only (both prologues checked first).
            if (g_fxCfg.weaponFxIntent && !g_fxWeaponIntentTried)
            {
                g_fxWeaponIntentTried = true;
                g_fxWeaponIntentReady = replay::fx::InstallWeaponFxIntent(g_fxWeaponIntentError);
                log << "WEAPON_FX_INTENT,status=" << (g_fxWeaponIntentReady ? "installed" : "failed")
                    << ",error=" << (g_fxWeaponIntentError.empty() ? "none" : g_fxWeaponIntentError.c_str())
                    << ",detours=0x14309a8;0x14308f0,engine_writes=0,replay=off" << '\n';
            }
            log << "POSE_V2_FX_OBSERVER"
                << ",status=" << (g_poseV2FxObserverReady ? "installed" : "failed")
                << ",error=" << (error.empty() ? "none" : error.c_str())
                << ",engine_writes=0"
                << '\n';
            log.flush();
        }
        // Only the requests of this recording.
        g_fxWeaponIntentCursor = replay::fx::TotalWeaponFxCalls();
        g_fxWeaponIntent = FxWeaponIntentStats{};
        if (!g_poseV2AudioObserverTried)
        {
            g_poseV2AudioObserverTried = true;
            std::string error;
            g_poseV2AudioObserverReady = replay::audio::Install(error);
            std::ostream& log = Ctx().log;
            log << "POSE_V2_AUDIO_OBSERVER"
                << ",status=" << (g_poseV2AudioObserverReady ? "installed" : "failed")
                << ",error=" << (error.empty() ? "none" : error.c_str())
                << ",engine_writes=0"
                << '\n';
            log.flush();
        }
        // The ped audio entity's sound query (the mount relation of the puppets,
        // and its evidence) (entity_query.h).
        static bool entityQueryTried = false;
        if (!entityQueryTried)
        {
            entityQueryTried = true;
            std::string error;
            const bool ready = replay::entityquery::Install(error);
            replay::entityquery::SetScriptThread(GetCurrentThreadId());
            Ctx().log << "ENTITY_QUERY_HOOK,status=" << (ready ? "installed" : "failed")
                << ",error=" << (error.empty() ? "none" : error.c_str())
                << ",relation=" << (replay::entityquery::RelationOn() ? 1 : 0) << '\n';
        }
        // The engine sound census (hooked for SoundCapture; the check and its
        // report with SoundCheck, default Debug).
        static bool censusTried = false;
        if (!censusTried && replay::census::Enabled())
        {
            censusTried = true;
            std::string error;
            const bool ready = replay::census::Install(error);
            Ctx().log << "SOUND_CENSUS_OBSERVER,status=" << (ready ? "installed" : "failed")
                      << ",error=" << (error.empty() ? "none" : error.c_str()) << ",engine_writes=0"
                      << ",mode=" << (replay::census::FullLog() ? "full" : "cheap")
                      << ",check=" << (replay::census::CheckEnabled() ? 1 : 0)
                      << ",capture=" << replay::census::CaptureMode() << '\n';
        }
        // The read probes of the sound and particle ports, only with
        // SoundFxProbe=1. After the observers above: the probe rides their detours
        // and hooks only ports they do not. The generic sound recorder shares those
        // hooks: with SoundGeneric=1 the ports it needs are installed even with the
        // probe off (its rings first).
        static bool soundFxProbeTried = false;
        if (!soundFxProbeTried && (replay::soundfx::Enabled() || replay::soundgeneric::Enabled()))
        {
            soundFxProbeTried = true;
            std::string generic;
            const bool genericReady = replay::soundgeneric::Install(generic);
            std::string summary;
            const bool ready = replay::soundfx::Install(summary);
            Ctx().log << "SOUNDFX_PROBE,status=" << (ready ? "installed" : "failed") << ',' << summary
                      << ",engine_writes=0" << '\n';
            if (replay::soundgeneric::Enabled())
                Ctx().log << "SOUND_GENERIC_RECORDER,status=" << (genericReady && ready ? "installed" : "failed") << ','
                          << generic << ",hooks=" << summary << ",engine_writes=0" << '\n';
            // Read at load.
            if (replay::soundgeneric::Enabled())
            {
                const auto mode = replay::soundgeneric::EntityMode::GameQueue;
                Ctx().log << "SOUND_GENERIC_ENTITY_MODE,mode=" << replay::soundgeneric::EntityModeLetter(mode)
                          << ",name=" << replay::soundgeneric::EntityModeName(mode)
                          << ",families=entity_sound;sound_request,owner=entity,engine_writes=0\n";
            }
        }
        // The render target and UI app detours, before the capture probe (its C29
        // and C30 ride them) (render_target.h).
        static bool renderTargetTried = false;
        if (!renderTargetTried)
        {
            renderTargetTried = true;
            std::string summary;
            const bool ready = replay::rendertarget::Install(summary);
            Ctx().log << "RT_HOOKS,status=" << (ready ? "installed" : "off") << ',' << summary
                      << ",mode=" << replay::rendertarget::ModeName(replay::rendertarget::Configured().mode)
                      << ",engine_writes=playback_natives" << '\n';
        }
        // The read probes of the capture panorama, only with CaptureProbe=1.
        static bool captureProbeTried = false;
        if (!captureProbeTried && replay::captureprobe::Enabled())
        {
            captureProbeTried = true;
            std::string summary;
            const bool ready = replay::captureprobe::Install(summary);
            Ctx().log << "CAPTURE_PROBE,status=" << (ready ? "installed" : "failed") << ',' << summary
                      << ",engine_writes=0" << '\n';
        }
    }

    void BeginRecording()
    {
        replay::log::NewSession(); // every LogLimit counts again
        WarmUp();
        g_poseV2PlayerAudioBase = replay::audio::PlayerAudioSnapshot();
        replay::capture::BeginRecording(Ctx());
        replay::soundgeneric::BeginRecording(Ctx());
        replay::rendertarget::BeginRecording(Ctx().clip.renderTargets, Ctx().log);
        g_poseV2PlayerAudioSource = "live";
        g_poseV2MountAudioBase = replay::audio::MountAudioSnapshot();
        g_poseV2MountProbeLogged = false;
        replay::horseaudio::Begin("record", Ctx().log);
        BeginEntityQuery(true);
        BeginPedAudioState(true);
        BeginFootEvents(true);
        BeginShotAudioDelay("rec");
        g_poseV2AudioWindows.clear();
        g_poseV2AudioWatches.clear();
        {
            std::vector<replay::audio::SoundRequest> stale;
            replay::audio::CollectSoundRequestsSince(g_poseV2SoundCursor, stale);
            replay::audio::CollectPlayRequestsSince(g_poseV2PlaySoundCursor, stale);
            g_poseV2PlaySoundsCovered = 0;
            g_poseV2PersistentUnattributedLogged = 0;
            g_poseV2SharedHelperLogged = 0;
            g_poseV2ImpactLogged = 0;
            g_poseV2WeaponSoundLogged = 0;
            g_poseV2LiveCollisionSounds.clear();
            std::vector<replay::audio::SoundStop> staleStops;
            replay::audio::CollectSoundStopsSince(g_poseV2SoundStopCursor, staleStops);
            BeginPoseV2EntitySounds();
            ResetPoseV2ObjectSounds();
            g_poseV2WeaponMemo.clear();
            g_poseV2FireRefFromMemo = 0;
        }
        {
            std::vector<replay::audio::ImpactCall> stale;
            replay::audio::CollectImpactsSince(g_poseV2BodyImpactCursor, stale);
        }
        {
            std::vector<replay::audio::MeleeCall> stale;
            replay::audio::CollectMeleeSince(g_poseV2MeleeCursor, stale);
        }
        g_poseV2FootstepsUnrecorded = 0;
        {
            std::vector<replay::footmaterial::Sample> stale;
            replay::audio::CollectFootMaterialSamplesSince(g_poseV2FootMaterialCursor, stale);
            g_poseV2FootMaterialPairer.Clear();
            g_poseV2FootMaterialPaired = 0;
            g_poseV2FootWetnessRecorded = 0;
            g_poseV2FootWetRecorded = 0;
            g_poseV2FootLayersRecorded = 0;
            g_poseV2FootLayersNonZero = 0;
            g_poseV2FootLodRecorded = 0;
            g_poseV2FootMaterialInvalid = 0;
            g_poseV2FootMaterialLost = 0;
        }
        g_poseV2FxStartCursor = replay::fx::TotalStarts();
        g_fxCheck.BeginRecord();
        g_poseV2RegisteredPrevTickMs = 0;
        g_fxShotResolveShots = g_fxShotMuzzleStarts = g_fxShotImpactStarts = g_fxShotBodyStarts = 0;
        g_fxShotClaimedMulti = g_fxShotNoMuzzle = g_fxShotClaimed = 0;
        g_fxShotTriggerClaims.clear();
        g_fxShotFxLogs = 0;
        g_poseV2RegisteredSplits = 0;
        g_poseV2FxSkippedShot = 0;
        g_poseV2FxSkippedEntity = 0;
        g_poseV2FxWorldOwner = 0;
        g_poseV2FxUnresolvedParent = 0;
        g_poseV2FxInvalidWorld = 0;
        g_poseV2FxUnresolvedLogs = 0;
        g_poseV2FxImpactNoShot = 0;
        g_poseV2FxNoHash = 0;
        g_poseV2FxRecordedByHash.clear();
        g_poseV2FxSkippedByHash.clear();
        g_poseV2FxNoHashByCaller.clear();
        g_poseV2FxStartsByCaller.clear();
        g_poseV2FxSamples = 0;
        g_poseV2FxAttachesAtStart = replay::fx::TotalAttaches();
        g_poseV2FxRegisteredCursor = replay::fx::TotalRegistered();
        g_poseV2FxRegistered.clear();
        g_poseV2FxRegisteredSamples = 0;
        g_poseV2RegisteredLive.clear();
        g_poseV2FxInstCreatedUs.clear();
        // Late attach resolution and attach changes.
        g_poseV2FxAttachTotalSeen = replay::fx::TotalAttaches();
        g_poseV2FxLateOwnerLogs = 0;
        g_poseV2FxLateLostLogs = 0;
        g_poseV2FxLateResolved = 0;
        g_poseV2FxLateLost = 0;
        g_poseV2FxOwnerLostEnded = 0;
        g_poseV2FxTenantAttaches = 0;
        g_poseV2FxTenantLogs = 0;
        g_poseV2FxAttachChanges = 0;
        g_poseV2FxPendingOneShot.clear();
        g_poseV2ObjectAtAddressLogs = 0;
        g_poseV2DecalCursor = replay::decal::TotalCalls();
        g_poseV2DecalInitialPending = g_poseV2DecalObserverReady;
        g_poseV2DecalInitial = 0;
        g_poseV2DecalRemoveCursor = replay::decal::TotalRemoves();
        g_poseV2ProjCursor = replay::decal::TotalProj();
        // Projected, entity, aging and expiry state of this recording.
        g_poseV2ProjFromRage = 0;
        g_poseV2ProjFromRageLogs = 0;
        g_poseV2DecalsWithEntity = 0;
        g_poseV2DecalEntityLogs = 0;
        g_poseV2PoolSamples = 0;
        g_poseV2DecalsAgedInitial = 0;
        g_poseV2DecalsExpiredInitial = 0;
        g_poseV2DecalAgeLogs = 0;
        g_poseV2RecordedAliveIds.clear();
        g_poseV2RecordedAliveSet.clear();
        g_poseV2RecordedAliveAt = 0;
        g_poseV2DecalsExpired = 0;
        g_poseV2DecalExpiredLogs = 0;
        g_poseV2GoreMasks.clear();
        g_poseV2GoreHitCursor = replay::gore::TotalEvents();
        g_poseV2HeldStateTaken.clear();
        g_poseV2HeldBlood = 0;
        g_poseV2HeldGoreBits = 0;
        g_poseV2HeldPieces = 0;
        g_poseV2BloodCursor = replay::blood::TotalCalls();
        g_poseV2EngineFireCursor = replay::fire::TotalCalls();
        g_poseV2FireActiveSlots.clear();
        g_poseV2FireOwners.clear();
        g_poseV2FireItemSampleAt = 0;
        g_poseV2FireFar = 0;
        g_poseV2FireWorldEntities = 0;
        // The initial fire state is taken on the first recording tick: the
        // clip's tracks are reset after this begin.
        g_poseV2FireInitialPending = g_poseV2FireReady;
        g_poseV2FireUntrackedNextMs = 0xFFFFFFFFu; // the initial pass first (RecordPoseV2InitialFires)
        g_poseV2FireRebuilt = 0;
        g_poseV2FireUntrackedSkipped = 0;
        g_poseV2FirePtfxOwned = 0;
        g_poseV2FireUntrackedLogs = 0;
        std::memset(g_poseV2FireUntrackedSeen, 0, sizeof(g_poseV2FireUntrackedSeen));
        g_poseV2FireRefused = 0;
        g_poseV2FireRemnantAdds = 0;
        g_poseV2FireRemnantRemoves = 0;
        g_poseV2BloodUnmapped = 0;
        g_poseV2BloodNotCopied = 0;
        g_poseV2BloodFromCache = 0;
        g_poseV2BloodNotPed = 0;
        g_poseV2BloodUnmappedLogs = 0;
        g_poseV2BloodOwnerCache.clear();
        g_poseV2BloodRef78[0] = g_poseV2BloodRef78[1] = g_poseV2BloodRef78[2] = 0;
        g_poseV2BloodByCaller.clear();
        g_poseV2BloodLogged = 0;
        g_poseV2ProjFromBlood = 0;
        g_poseV2GoreHitsOther = 0;
        SeedPoseV2GorePieces();
        replay::decal::ClearInfoKeys();
        g_poseV2SurfaceNextMs = 0;
        g_poseV2AnimLogged = 0;
        g_poseV2AnimBatches = 0;
        g_poseV2AnimRecorded = 0;
        g_poseV2AnimSkippedDuplicate = 0;
        g_poseV2AnimSkippedSpeechHash = 0;
        g_poseV2AnimUnrecordedOwner = 0;
        g_poseV2AnimTailNonZero = 0;
        g_poseV2AnimRecordLines = 0;
        {
            std::vector<replay::audio::AnimAudioBatch> staleAnim;
            replay::audio::CollectAnimBatchesSince(g_poseV2AnimCursor, staleAnim);
        }
        g_poseV2SurfaceLast.clear();
        g_poseV2SurfacesRecorded = 0;
        g_poseV2SurfacesUnreadable = 0;
        g_poseV2DecalsInfoUnknown = 0;
        g_poseV2ProjSamples = 0;
        g_poseV2ProjNoEntity = 0;
        g_poseV2DecalsRefused = 0;
        g_poseV2DecalsInternal = 0;
        g_poseV2DecalsAttached = 0;
        g_poseV2DecalSamples = 0;
        replay::decal::SetEnabled(g_poseV2DecalObserverReady);
        replay::gore::SetObserving(g_poseV2GoreObserverReady);
        replay::blood::SetObserving(g_poseV2BloodObserverReady);
        replay::fire::SetObserving(g_poseV2FireReady);
        g_poseV2RegisteredFramesStored = 0;
        g_poseV2RegisteredSkippedOld = 0;
        g_poseV2CameraAmbientSkipped = 0;
        g_poseV2RegisteredHeld = 0;
        g_poseV2RegisteredDropped = 0;
        g_poseV2RegisteredTrimmed = 0;
        g_poseV2RegisteredMaxFrames = 0;
        g_poseV2RegisteredCapLogged = false;
        g_poseV2RegisteredCapLogMs = 0;
        g_poseV2RegisteredAttachStale = 0;
        g_scriptFxHashless = {};
        {
            std::vector<replay::audio::FootstepCall> stale;
            replay::audio::CollectFootstepsSince(g_poseV2FootstepCursor, stale);
            std::vector<replay::audio::ExplosionAudioCall> staleExplosions;
            replay::audio::CollectExplosionsSince(g_poseV2ExplosionCursor, staleExplosions);
        }
        g_poseV2FireCursor = replay::audio::TotalFires();
        replay::audio::SetFullSoundCallers(g_poseV2ResearchWindows);
        replay::audio::SetEnabled(g_poseV2AudioObserverReady);
        g_poseV2FxWindows.clear();
        replay::fx::SetEnabled(g_poseV2FxObserverReady);
        g_poseV2FxPhaseStartUs = replay::fx::NowUs();
        {
            std::vector<replay::audio::SpeechCoreCall> stale;
            replay::audio::CollectSpeechCoreSince(g_poseV2CoreCursor, stale);
            std::vector<replay::audio::SpeechLineStart> staleStarts;
            replay::audio::CollectSpeechStartsSince(g_poseV2SpeechStartCursor, staleStarts);
            std::vector<replay::audio::SpeechLineStop> staleStops;
            replay::audio::CollectSpeechStopsSince(g_poseV2SpeechStopCursor, staleStops);
            // The lines already audible when the recording begins (the start detour
            // records always) are candidates, adopted once their speaker is recorded.
            CollectPoseV2AdoptCandidates(staleStarts, staleStops);
            // And the lines created before it that wait for their audible start (a
            // conversation that began before the recording), adopted when they start.
            CollectPoseV2EarlyLines(stale, staleStarts, staleStops);
        }
        g_poseV2SpeechMatcher.Clear();
        g_poseV2SpeechDroppedUnheard = 0;
        g_poseV2QueuedLines.clear();
        g_poseV2QueuedLineBudget = 16;
        g_poseV2Shooters.clear();
        g_poseV2ShotImpacts = 0;
        g_poseV2GateDiscarded = 0;
    }

    // The recording gate (pose::capture::IsClockPaused: a cutscene, fade,
    // loading screen or the pause menu). Every detour queue is drained so
    // nothing piles up for the resume (the queue times are ages against the
    // clock, so a paused event would land in the past or on the cut instant),
    // and its contents are discarded. State that ends in the pause ends at the
    // cut instant (the frozen clock): a recorded fire that goes out, a recorded
    // decal that is removed, a speech line that stops, a collision sound.
    // Baselines that turn into one-shot events (a ped's ammo clip, its last
    // impact) follow the game.
    void DiscardPoseV2WhilePaused()
    {
        REPLAY_PROFILE_ZONE("rec.gate_discard");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        std::uint64_t discarded = 0;
        RecordPoseV2Shots(false);
        {
            std::vector<replay::audio::SoundRequest> sounds;
            replay::audio::CollectSoundRequestsSince(g_poseV2SoundCursor, sounds);
            replay::audio::CollectPlayRequestsSince(g_poseV2PlaySoundCursor, sounds);
            discarded += sounds.size();
            std::vector<replay::audio::ImpactCall> impacts;
            replay::audio::CollectImpactsSince(g_poseV2BodyImpactCursor, impacts);
            std::vector<replay::audio::MeleeCall> melee;
            replay::audio::CollectMeleeSince(g_poseV2MeleeCursor, melee);
            std::vector<replay::audio::FootstepCall> footsteps;
            replay::audio::CollectFootstepsSince(g_poseV2FootstepCursor, footsteps);
            std::vector<replay::audio::ExplosionAudioCall> explosions;
            replay::audio::CollectExplosionsSince(g_poseV2ExplosionCursor, explosions);
            std::vector<replay::audio::AnimAudioBatch> anim;
            replay::audio::CollectAnimBatchesSince(g_poseV2AnimCursor, anim);
            discarded += impacts.size() + melee.size() + footsteps.size() + explosions.size() + anim.size();
        }
        // Collision sounds do not go on across a cut: they end at it.
        TrackPoseV2CollisionSounds(true);
        {
            std::vector<replay::audio::SpeechCoreCall> core;
            replay::audio::CollectSpeechCoreSince(g_poseV2CoreCursor, core);
            discarded += core.size();
            std::vector<replay::audio::SpeechLineStart> starts;
            replay::audio::CollectSpeechStartsSince(g_poseV2SpeechStartCursor, starts);
            std::vector<replay::audio::SpeechLineStop> stops;
            replay::audio::CollectSpeechStopsSince(g_poseV2SpeechStopCursor, stops);
            std::vector<replay::speech::rec::TimedRef> timeline;
            for (std::size_t n = 0; n < core.size(); ++n)
                timeline.push_back({core[n].timeUs, replay::speech::rec::EventKind::Create, n});
            for (std::size_t n = 0; n < starts.size(); ++n)
                timeline.push_back({starts[n].timeUs, replay::speech::rec::EventKind::Start, n});
            for (std::size_t n = 0; n < stops.size(); ++n)
                timeline.push_back({stops[n].timeUs, replay::speech::rec::EventKind::Stop, n});
            replay::speech::rec::SortTimeline(timeline);
            for (const replay::speech::rec::TimedRef& ref : timeline)
            {
                if (ref.kind == replay::speech::rec::EventKind::Create)
                {
                    // Rule 1 holds in the pause too (a line created there releases the
                    // address).
                    const replay::audio::SpeechCoreCall& call = core[ref.index];
                    if (call.queued && call.line)
                        DropPoseV2EarlyLine(call.line);
                    if (!call.queued || !call.line || !replay::audio::SpeechStartObserved())
                        continue;
                    std::vector<PoseV2SpeechMatcher::Drop> drops;
                    std::vector<PoseV2SpeechMatcher::StartedDrop> startedDrops;
                    g_poseV2SpeechMatcher.Created(call.line, drops, startedDrops);
                    LogPoseV2SpeechDrops(drops, nowMs);
                    LogPoseV2SpeechStartedReused(startedDrops, nowMs);
                }
                else if (ref.kind == replay::speech::rec::EventKind::Start)
                {
                    // A line heard only in the pause is not in the clip.
                    const replay::audio::SpeechLineStart& start = starts[ref.index];
                    DropPoseV2EarlyLine(start.line); // an early line too
                    replay::speech::rec::LineKey key;
                    key.voice = start.voice;
                    key.context = start.context;
                    key.variation = start.variation;
                    PoseV2SpeechMatcher::Entry entry;
                    std::vector<PoseV2SpeechMatcher::Drop> drops;
                    if (g_poseV2SpeechMatcher.Start(start.line, start.identityRead, key, entry, drops) ==
                        replay::speech::rec::StartResult::Matched)
                    {
                        ++g_poseV2SpeechDroppedUnheard;
                        ++discarded;
                    }
                    LogPoseV2SpeechDrops(drops, nowMs);
                }
                else
                {
                    const replay::audio::SpeechLineStop& stop = stops[ref.index];
                    DropPoseV2EarlyLine(stop.line);
                    std::size_t eventIndex = 0;
                    if (!g_poseV2SpeechMatcher.StopStarted(stop.line, eventIndex))
                        continue;
                    if (stop.reason != 1 && eventIndex < Ctx().clip.speechLines.Events().size())
                    {
                        replay::entity::SpeechEvent& line = Ctx().clip.speechLines.At(eventIndex);
                        line.stopMs = std::max(nowMs, line.timeMs);
                        line.stopReason = stop.reason;
                    }
                    else if (stop.reason == 1 && eventIndex < Ctx().clip.speechLines.Events().size())
                    {
                        replay::entity::SpeechEvent& line = Ctx().clip.speechLines.At(eventIndex);
                        line.endMs = std::max(nowMs, line.timeMs);
                    }
                }
            }
        }
        // Particles: starts discarded; registered effects drained without
        // recording (any phase but "record" skips the clip).
        discarded += replay::fx::TotalStarts() - g_poseV2FxStartCursor;
        g_poseV2FxStartCursor = replay::fx::TotalStarts();
        DrainPoseV2FxRegistered("paused");
        // Decals: adds and projections discarded; a recorded decal removed
        // in the pause is removed at the cut instant.
        discarded += replay::decal::TotalCalls() - g_poseV2DecalCursor;
        g_poseV2DecalCursor = replay::decal::TotalCalls();
        g_poseV2ProjCursor = replay::decal::TotalProj();
        {
            std::vector<replay::decal::RemoveCall> removes;
            replay::decal::CollectRemovesSince(g_poseV2DecalRemoveCursor, removes);
            for (const replay::decal::RemoveCall& call : removes)
            {
                if (!g_poseV2RecordedAliveSet.count(call.id))
                    continue;
                replay::entity::DecalRemoveEvent event;
                event.timeMs = nowMs;
                event.engineId = call.id;
                Ctx().clip.decalRemoves.Add(event);
                UntrackPoseV2RecordedDecal(call.id);
            }
        }
        // Damage: gore hits and blood records discarded (the ped's state
        // after the pause is what the next records build on).
        discarded += replay::gore::TotalEvents() - g_poseV2GoreHitCursor;
        g_poseV2GoreHitCursor = replay::gore::TotalEvents();
        discarded += replay::blood::TotalCalls() - g_poseV2BloodCursor;
        g_poseV2BloodCursor = replay::blood::TotalCalls();
        RecordPoseV2Fires(true);
        g_poseV2GateDiscarded += discarded;
    }

    // The requests of this tick, with the shooter and the weapon object as
    // recorded ids.
    void RecordWeaponFxIntents(bool paused)
    {
        if (!g_fxWeaponIntentReady)
            return;
        static std::vector<replay::fx::WeaponFxCall> calls;
        replay::fx::CollectWeaponFxSince(g_fxWeaponIntentCursor, calls);
        if (calls.empty())
            return;
        if (paused)
        {
            g_fxWeaponIntent.paused += calls.size();
            return;
        }
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::fx::WeaponFxCall& call : calls)
        {
            ++(call.kind == 1 ? g_fxWeaponIntent.muzzle : g_fxWeaponIntent.shell);
            replay::weaponfx::Intent intent;
            intent.timeMs = nowMs;
            intent.shooter = replay::weaponfx::kNoId;
            intent.weapon = replay::weaponfx::kNoId;
            intent.kind = call.kind;
            intent.bone = call.bone;
            intent.argA = call.argA;
            intent.fxHash = call.fxHash;
            intent.fxHashAlt = call.fxHashAlt;
            intent.range = std::isfinite(call.range) ? call.range : 0.0f;
            for (int n = 0; n < 4; ++n)
                intent.vec[n] = std::isfinite(call.vec[n]) ? call.vec[n] : 0.0f;
            if (call.kind == 1 && !call.infoRead)
                ++g_fxWeaponIntent.infoUnread;
            // The shooter by the address of a recorded entity, not HandleAtAddress
            // (that one gives a script handle to every NPC that fires, recorded or
            // not, from the guid pool).
            if (call.shooter)
                for (const replay::recorder::RecordedEntity& recorded : Ctx().recorder.entities)
                    if (recorded.handle &&
                        reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(recorded.handle)) == call.shooter)
                    {
                        intent.shooter = recorded.id.Packed();
                        break;
                    }
            if (intent.shooter == replay::weaponfx::kNoId)
                ++g_fxWeaponIntent.shooterUnknown;
            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                if (weapon.object && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)) == call.object)
                {
                    intent.weapon = weapon.id.Packed();
                    break;
                }
            if (intent.weapon == replay::weaponfx::kNoId)
                ++g_fxWeaponIntent.weaponUnknown;
            if (Ctx().clip.weaponFxIntents.Add(intent))
                ++g_fxWeaponIntent.stored;
            else
                ++g_fxWeaponIntent.refused;
            if (g_fxWeaponIntent.logs < 24)
            {
                ++g_fxWeaponIntent.logs;
                Ctx().log << "WEAPON_FX_INTENT_REC,kind=" << (call.kind == 1 ? "muzzle" : "shell")
                    << ",shooter=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(intent.shooter))
                    << ",weapon=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(intent.weapon))
                    << ",bone=" << call.bone << ",arg_a=" << call.argA << std::hex << ",fx=0x" << call.fxHash << ",fx_alt=0x"
                    << call.fxHashAlt << std::dec << ",range=" << call.range << ",vec=" << call.vec[0] << ";" << call.vec[1]
                    << ";" << call.vec[2] << ";" << call.vec[3] << ",thread=" << call.threadId << ",time_ms=" << nowMs << '\n';
            }
        }
    }

    void Record()
    {
        // Before the gate (the census logs the pause too, np=0).
        replay::census::Tick(Ctx(), "rec", replay::pose::capture::NowMs(), !replay::pose::capture::IsClockPaused());
        // The census rows of this tick, read only.
        replay::mixprobe::Tick(Ctx(), "rec", replay::pose::capture::NowMs(), !replay::pose::capture::IsClockPaused());
        // The live engine volume listener (LSTN).
        replay::gamelistener::Record(Ctx().clip, replay::pose::capture::NowMs(), !replay::pose::capture::IsClockPaused(), Ctx().log);
        // The audio wind strength (WNDA, evidence).
        replay::windreplay::Record(Ctx().clip, static_cast<std::uint32_t>(replay::pose::capture::NowMs()), !replay::pose::capture::IsClockPaused(), Ctx().log);
        // The mixer scenes and mix groups (MIXS), then the speech level probe (it
        // reads the live scenes).
        replay::mixscene::Record(Ctx(), static_cast<std::uint32_t>(replay::pose::capture::NowMs()));
        // The categories' controller values (CATS) and the speech duck's evidence.
        replay::catstate::Record(Ctx().clip, static_cast<std::uint32_t>(replay::pose::capture::NowMs()), !replay::pose::capture::IsClockPaused(), Ctx().log);
        replay::speechduck::Record(Ctx().clip, static_cast<std::uint32_t>(replay::pose::capture::NowMs()), !replay::pose::capture::IsClockPaused(), Ctx().log);
        replay::speechlevel::Tick(Ctx(), false, static_cast<std::uint32_t>(replay::pose::capture::NowMs()));
        // Every voice's gain by stage, read only.
        replay::volumeprobe::Tick(Ctx(), "rec", replay::pose::capture::NowMs(), !replay::pose::capture::IsClockPaused());
        // The particle audio slots and the horse groups, read only.
        replay::worldsoundprobe::Tick(Ctx(), "rec", replay::pose::capture::NowMs(), !replay::pose::capture::IsClockPaused());
        // After the census (its owner table is fresh), before the gate (a paused
        // clock discards the events).
        replay::soundgeneric::Record(Ctx());
        TickEntityQuery(true, replay::pose::capture::NowMs());
        TickPedAudioState(true, replay::pose::capture::NowMs());
        TickMotionState(true, replay::pose::capture::NowMs());
        ProbePoseV2PlayerMountAudio(replay::pose::capture::NowMs());
        TickPoseV2HorseAudioEvidence(true, replay::pose::capture::NowMs());
        // One gate check for every event consumer.
        if (replay::pose::capture::IsClockPaused())
        {
            FlushPoseV2FxWindows(false);
            FlushPoseV2AudioWindows(false);
            FollowPoseV2QueuedLines();
            DiscardPoseV2WhilePaused();
            DiscardFootEventsWhilePaused();
            TickShotAudioDelay();
            RecordWeaponFxIntents(true); // dropped while paused
            return;
        }
        RecordPoseV2Shots();
        TickShotAudioDelay(); // after the tick's fires
        FlushPoseV2FxWindows(false);
        WatchPoseV2AudioEvents();
        FlushPoseV2AudioWindows(false);
        FollowPoseV2QueuedLines();
        RecordPoseV2SpeechEvents();
        RecordPoseV2Footsteps();
        RecordFootEvents();
        TickMovementDetail(true, replay::pose::capture::NowMs()); // evidence
        RecordPoseV2Explosions();
        RecordPoseV2Surfaces();
        RecordPoseV2AnimEvents();
        {
            static std::uint32_t nextAreaMs = 0;
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            if (nowMs < 100)
                nextAreaMs = 0;
            if (nowMs >= nextAreaMs)
            {
                nextAreaMs = nowMs + 1000;
                for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                {
                    if (!replay::entities::Exists(entity.handle))
                        continue;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, FALSE);
                    Ctx().clip.ExtendArea(at.x, at.y, at.z);
                }
            }
        }
        RecordPoseV2Fx();
        RecordWeaponFxIntents(false);
        DrainPoseV2FxRegistered("record");
        SamplePoseV2FxRegisteredState("record");
        RecordPoseV2Decals();
        RecordPoseV2Gore();
        RecordPoseV2TimeScale();
        RecordPoseV2MeleeHits();
        RecordPoseV2BodyImpacts();
        RecordPoseV2Sounds();
        RecordPoseV2PlaySounds();
        TrackPoseV2CollisionSounds(false);
        RememberPoseV2Weapons();
        // After the sound tracks of this tick (they win a sound).
        replay::capture::Record(Ctx());
        if (g_poseV2ResearchWindows)
            TracePoseV2Emitters("record", replay::pose::capture::NowMs()); // research
    }

    // The last EndRecording's parts (LastEndTiming).
    EndTiming g_poseV2EndTiming;

    void EndRecording()
    {
        // The time of each part, for POSE_V2_STOP_TIMING (LastEndTiming).
        g_poseV2EndTiming = {};
        std::uint64_t lapAt = replay::prof::Now();
        const auto lap = [&lapAt](std::uint64_t& us) {
            const std::uint64_t now = replay::prof::Now();
            us += replay::clock::TicksToUs(now - lapAt, replay::prof::TicksPerSecond());
            lapAt = now;
        };
        // Stopped while the gate holds: what the queues got in the pause is not
        // recorded at the frozen instant.
        if (replay::pose::capture::IsClockPaused())
            DiscardPoseV2WhilePaused();
        LogPoseV2AnimAudioRecordTotal();
        {
            // engine_true = the weapon audio's player answers for the real player while
            // recording (compare with the playback overrides at the same sites).
            std::size_t playerShots = 0;
            for (const replay::entity::ShotEvent& shot : Ctx().clip.shots.Shots())
                playerShots += shot.shooterPlayer == 2 ? 1 : 0;
            LogPoseV2PlayerAudioTotal("record_total", playerShots);
            EndShotAudioDelay(nullptr);
            LogPoseV2PlayerMountAudioTotal("record_total");
            EndEntityQuery("record_total");
            EndPedAudioState("record_total");
            EndFootEvents("record_total");
            replay::horseaudio::End(Ctx().log);
        }
        replay::rendertarget::EndRecording(Ctx().clip.renderTargets, Ctx().log);
        Ctx().log << "RECORD_GATE_TOTAL,discarded=" << g_poseV2GateDiscarded << ",cuts=" << Ctx().clip.cuts.size()
            << ",spans=" << Ctx().clip.spans.size() << ",paused_ms=" << replay::pose::capture::PausedMsTotal() << '\n';
        lap(g_poseV2EndTiming.totalsUs);
        FlushPoseV2PendingSpeech();
        FlushPoseV2FxWindows(true);
        if (g_poseV2FxObserverReady)
            LogPoseV2FxHistogram("record");
        if (g_fxWeaponIntentReady)
        {
            RecordWeaponFxIntents(false);
            Ctx().log << "WEAPON_FX_INTENT_TOTAL,muzzle=" << g_fxWeaponIntent.muzzle << ",shell=" << g_fxWeaponIntent.shell
                << ",stored=" << g_fxWeaponIntent.stored << ",shooter_unknown=" << g_fxWeaponIntent.shooterUnknown
                << ",weapon_unknown=" << g_fxWeaponIntent.weaponUnknown << ",info_unread=" << g_fxWeaponIntent.infoUnread
                << ",paused=" << g_fxWeaponIntent.paused << ",refused=" << g_fxWeaponIntent.refused << '\n';
        }
        {
            // How the shots took their weapon effects while recording (the match by
            // time).
            const std::uint64_t starts = g_fxShotMuzzleStarts + g_fxShotImpactStarts;
            const std::uint64_t claimed = g_fxShotTriggerClaims.size();
            Ctx().log << "FX_SHOT_RESOLVE_TOTAL,shots=" << g_fxShotResolveShots << ",muzzle_starts=" << g_fxShotMuzzleStarts
                << ",impact_starts=" << g_fxShotImpactStarts << ",body_starts=" << g_fxShotBodyStarts
                << ",claims=" << g_fxShotClaimed << ",claimed_multi=" << g_fxShotClaimedMulti
                << ",orphans=" << (starts > claimed ? starts - claimed : 0) << ",shots_no_muzzle_live=" << g_fxShotNoMuzzle << '\n';
        }

        replay::fx::SetEnabled(false);
        lap(g_poseV2EndTiming.flushUs);
        RecordPoseV2Decals();
        lap(g_poseV2EndTiming.decalsUs);
        replay::gore::SetObserving(false);
        replay::blood::SetObserving(false);
        RecordPoseV2Fires();
        replay::fire::SetObserving(false);
        Ctx().log << "POSE_V2_FIRE_TOTAL,starts=" << Ctx().clip.fireStarts.Events().size() << ",stops=" << Ctx().clip.fireStops.Events().size()
            << ",item_samples=" << Ctx().clip.fireItems.Events().size()
            << ",still_burning=" << g_poseV2FireActiveSlots.size() << ",far=" << g_poseV2FireFar << ",refused=" << g_poseV2FireRefused
            << ",remnant_adds=" << g_poseV2FireRemnantAdds << ",remnant_removes=" << g_poseV2FireRemnantRemoves
            << ",world_entities=" << g_poseV2FireWorldEntities << ",rebuilt=" << g_poseV2FireRebuilt
            << ",untracked_left_out=" << g_poseV2FireUntrackedSkipped << ",ptfx_owned=" << g_poseV2FirePtfxOwned << '\n';
        {
            std::ostringstream callers;
            for (const auto& [caller, count] : g_poseV2BloodByCaller)
                callers << std::hex << "0x" << caller << std::dec << ":" << count << ";";
            Ctx().log << "POSE_V2_TIME_SCALE_TOTAL,changes=" << Ctx().clip.timeScale.Samples().size() << '\n';
            Ctx().log << "POSE_V2_HELD_TOTAL,blood=" << g_poseV2HeldBlood << ",gore_bits=" << g_poseV2HeldGoreBits
                << ",pieces=" << g_poseV2HeldPieces << '\n';
            Ctx().log << "POSE_V2_BLOOD_TOTAL,recorded=" << Ctx().clip.blood.Events().size() << ",proj_skipped=" << g_poseV2ProjFromBlood
                << ",unmapped=" << g_poseV2BloodUnmapped << ",from_cache=" << g_poseV2BloodFromCache << ",not_ped=" << g_poseV2BloodNotPed
                << ",not_copied=" << g_poseV2BloodNotCopied << ",ref78=" << g_poseV2BloodRef78[0] << "/" << g_poseV2BloodRef78[1]
                << "/" << g_poseV2BloodRef78[2] << ",by_caller=" << callers.str() << '\n';
        }
        lap(g_poseV2EndTiming.firesUs);
        AuditPoseV2ClipPointers();
        lap(g_poseV2EndTiming.auditUs);
        Ctx().log << "POSE_V2_GORE_HITS,recorded=" << Ctx().clip.goreHits.Events().size() << ",other_peds=" << g_poseV2GoreHitsOther
            << ",limbs=" << Ctx().clip.gore.Events().size() << '\n';
        Ctx().log << "POSE_V2_DECALS,recorded=" << Ctx().clip.decals.Events().size() << ",attached=" << g_poseV2DecalsAttached
            << ",internal=" << g_poseV2DecalsInternal << ",refused=" << g_poseV2DecalsRefused
            << ",removes=" << Ctx().clip.decalRemoves.Events().size() << ",projected=" << Ctx().clip.projDecals.Events().size()
            << ",projected_no_entity=" << g_poseV2ProjNoEntity << ",proj_from_rage=" << g_poseV2ProjFromRage
            << ",with_entity=" << g_poseV2DecalsWithEntity << ",expired=" << g_poseV2DecalsExpired
            << ",initial_aged=" << g_poseV2DecalsAgedInitial << ",initial_expired=" << g_poseV2DecalsExpiredInitial << '\n';
        FlushPoseV2AudioWindows(true);
        replay::audio::SetEnabled(false);
        {
            // Every speech line of the recording, and the most frequent
            // sound creation paths overall (the last ring's worth).
            std::vector<replay::audio::SpeechCall> speech;
            replay::audio::CollectSpeech(0, ~0ull, speech);
            std::vector<replay::audio::SoundCall> sounds;
            replay::audio::CollectSounds(0, ~0ull, sounds);
            std::vector<std::pair<std::uint64_t, std::uint32_t>> histogram;
            for (const replay::audio::SoundCall& call : sounds)
            {
                const std::uint64_t key = (static_cast<std::uint64_t>(call.callers[1]) << 32) | call.callers[2];
                auto it = std::find_if(histogram.begin(), histogram.end(),
                    [&](const auto& entry) { return entry.first == key; });
                if (it == histogram.end())
                    histogram.push_back({key, 1});
                else
                    ++it->second;
            }
            std::sort(histogram.begin(), histogram.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
            std::ostream& log = Ctx().log;
            log << "POSE_V2_AUDIO_OBSERVER_STOP,total_sounds=" << replay::audio::TotalSounds()
                << ",total_speech=" << replay::audio::TotalSpeech()
                << ",ring_sounds=" << sounds.size()
                << ",paths=" << histogram.size()
                << '\n';
            for (std::size_t index = 0; index < histogram.size() && index < 60; ++index)
            {
                log << "POSE_V2_AUDIO_TOP,count=" << histogram[index].second
                    << ",callers=0x" << std::hex << static_cast<std::uint32_t>(histogram[index].first >> 32)
                    << "/0x" << static_cast<std::uint32_t>(histogram[index].first) << std::dec
                    << '\n';
            }
            // Every sound creation of a rare path (fewer than 40 in the ring), with its
            // recording time and deeper callers, to tell projectile, explosion and fire
            // sounds apart from the ambient ones (research_windows only).
            if (g_poseV2ResearchWindows)
            {
                const std::uint64_t nowUs = replay::audio::NowUs();
                const std::uint32_t nowMs = replay::pose::capture::NowMs();
                std::uint32_t rareLogged = 0;
                for (const replay::audio::SoundCall& call : sounds)
                {
                    const std::uint64_t key = (static_cast<std::uint64_t>(call.callers[1]) << 32) | call.callers[2];
                    auto it = std::find_if(histogram.begin(), histogram.end(),
                        [&](const auto& entry) { return entry.first == key; });
                    if (it == histogram.end() || it->second >= 40 || rareLogged >= 400)
                        continue;
                    ++rareLogged;
                    const std::uint64_t ageMs = nowUs > call.timeUs ? (nowUs - call.timeUs) / 1000 : 0;
                    log << "POSE_V2_AUDIO_RARE,age_ms=" << ageMs << ",stop_ms=" << nowMs << std::hex
                        << ",callers=0x" << call.callers[1] << "/0x" << call.callers[2] << "/0x" << call.callers[3]
                        << "/0x" << call.callers[4] << "/0x" << call.callers[5] << "/0x" << call.callers[6] << std::dec << '\n';
                }
            }
            lap(g_poseV2EndTiming.soundsUs);
            {
                // Melee impact audio calls, arguments located against the
                // recorded peds (entity, ped audio).
                std::vector<replay::audio::MeleeCall> melee;
                replay::audio::CollectMelee(0, ~0ull, melee);
                const auto classify = [&](std::uint64_t value) -> std::string {
                    if (!value)
                        return "0";
                    for (const PoseV2AudioWatch& watch : g_poseV2AudioWatches)
                    {
                        if (value == watch.address)
                            return "ped:" + replay::entity::IdText(watch.id);
                        if (value == watch.audio)
                            return "audio:" + replay::entity::IdText(watch.id);
                        if (watch.audio && value > watch.audio && value < watch.audio + 0x6000)
                        {
                            char text[48]{};
                            std::snprintf(text, sizeof(text), "audio:%s+0x%llX", replay::entity::IdText(watch.id).c_str(),
                                static_cast<unsigned long long>(value - watch.audio));
                            return text;
                        }
                    }
                    return value >= 0x10000000ull && value < 0x800000000000ull ? "ptr" : "val";
                };
                log << "POSE_V2_MELEE_CALLS,count=" << melee.size() << '\n';
                for (std::size_t index = 0; index < melee.size() && index < 24; ++index)
                {
                    const replay::audio::MeleeCall& call = melee[index];
                    log << "POSE_V2_MELEE_CALL,entry=" << call.entry
                        << ",a0=" << classify(call.args[0])
                        << ",a1=" << classify(call.args[1])
                        << ",a2=" << classify(call.args[2])
                        << ",a3=" << classify(call.args[3])
                        << ",callers=0x" << std::hex << call.callers[0] << "/0x" << call.callers[1]
                        << "/0x" << call.callers[2] << std::dec;
                    std::string refs;
                    for (std::size_t offset = 0; offset + 8 <= sizeof(call.info); offset += 8)
                    {
                        std::uint64_t value = 0;
                        std::memcpy(&value, call.info + offset, 8);
                        const std::string kind = classify(value);
                        if (kind != "0" && kind != "val")
                        {
                            char text[16]{};
                            std::snprintf(text, sizeof(text), "%s%zX=", refs.empty() ? "" : "|", offset);
                            refs += text + kind;
                        }
                    }
                    log << ",info_refs=" << (refs.empty() ? "none" : refs.c_str()) << '\n';
                    if (index < 4)
                        LogPoseV2FxSnapshot("melee_info",
                            std::vector<std::uint8_t>(std::begin(call.info), std::end(call.info)));
                }
                static std::vector<replay::audio::ImpactCall> impacts;
                replay::audio::CollectImpacts(0, ~0ull, impacts);
                log << "POSE_V2_BODY_IMPACTS,count=" << impacts.size() << '\n';
                for (std::size_t index = 0; index < impacts.size() && index < 24; ++index)
                {
                    const replay::audio::ImpactCall& call = impacts[index];
                    float position[4] = {};
                    std::memcpy(position, call.position, sizeof(position));
                    float strength = 0.0f;
                    std::memcpy(&strength, &call.args[6], sizeof(strength));
                    log << "POSE_V2_BODY_IMPACT";
                    for (std::size_t arg = 0; arg < 10; ++arg)
                        log << ",a" << (arg + 1) << "=" << classify(call.args[arg]);
                    log << ",a7_float=" << strength
                        << ",pos=" << position[0] << "/" << position[1] << "/" << position[2]
                        << ",callers=0x" << std::hex << call.callers[0] << "/0x" << call.callers[1]
                        << "/0x" << call.callers[2] << std::dec;
                    for (const auto* object : {call.object8, call.object9})
                    {
                        std::string refs;
                        for (std::size_t offset = 0; offset + 8 <= 0x40; offset += 8)
                        {
                            std::uint64_t value = 0;
                            std::memcpy(&value, object + offset, 8);
                            const std::string kind = classify(value);
                            if (kind != "0" && kind != "val")
                            {
                                char text[16]{};
                                std::snprintf(text, sizeof(text), "%s%zX=", refs.empty() ? "" : "|", offset);
                                refs += text + kind;
                            }
                        }
                        log << (object == call.object8 ? ",a3_refs=" : ",a4_refs=") << (refs.empty() ? "none" : refs.c_str());
                    }
                    log << '\n';
                    if (index < 4)
                    {
                        LogPoseV2FxSnapshot("impact_a3", std::vector<std::uint8_t>(std::begin(call.object8), std::end(call.object8)));
                        LogPoseV2FxSnapshot("impact_a4", std::vector<std::uint8_t>(std::begin(call.object9), std::end(call.object9)));
                    }
                }
                lap(g_poseV2EndTiming.meleeUs);
                log << "POSE_V2_SPEECH_LINES,count=" << Ctx().clip.speechLines.Events().size() << '\n';
                TrackPoseV2CollisionSounds(true);
                log << "POSE_V2_COLLISION_SOUNDS,recorded=" << Ctx().clip.collisionSounds.size() << '\n';
                log << "POSE_V2_ENTITY_SOUNDS,recorded=" << Ctx().clip.entitySounds.size()
                    << ",unowned=" << g_poseV2EntitySoundsUnowned << ",audio_other=" << g_poseV2EntitySoundsAudioOther
                    << ",fire_refs_from_memo=" << g_poseV2FireRefFromMemo << ",object_late=" << g_poseV2ObjectSoundsLate
                    << ",object_one_shots=" << g_poseV2ObjectSoundsOneShot << ",object_expired=" << g_poseV2ObjectSoundsExpired
                    << ",ped_audio=" << g_poseV2PedAudioRecorded << ",ped_audio_unowned=" << g_poseV2PedAudioUnowned << '\n';
                for (std::size_t index = 0; index < Ctx().clip.collisionSounds.size() && index < 20; ++index)
                {
                    const replay::CollisionSound& sound = Ctx().clip.collisionSounds[index];
                    log << "POSE_V2_COLLISION_SOUND,ped=" << replay::entity::IdText(sound.entity)
                        << ",ref=0x" << std::hex << sound.soundRef << std::dec
                        << ",start_ms=" << sound.startMs << ",end_ms=" << sound.endMs
                        << ",samples=" << sound.intensity.size() << '\n';
                }
                log << "POSE_V2_SOUNDS,recorded=" << Ctx().clip.sounds.Events().size()
                    << ",covered_by_own_replay=" << g_poseV2PlaySoundsCovered
                    << ",engine_refused_total=" << replay::audio::RefusedPlayRequests() << '\n';
                log << "POSE_V2_MELEE_HITS,recorded=" << Ctx().clip.meleeHits.Events().size()
                    << ",body_impacts=" << Ctx().clip.bodyImpacts.Events().size() << '\n';
                log << "POSE_V2_FOOTSTEPS,recorded=" << Ctx().clip.footsteps.Events().size()
                    << ",unrecorded_peds=" << g_poseV2FootstepsUnrecorded
                    << ",material=" << g_poseV2FootMaterialPaired
                    << ",material_invalid=" << g_poseV2FootMaterialInvalid
                    << ",material_lost=" << (g_poseV2FootMaterialLost + g_poseV2FootMaterialPairer.PendingCount())
                    << ",material_samples=" << replay::audio::GetFootMaterialStats().samples
                    << ",wetness=" << g_poseV2FootWetnessRecorded << ",wet=" << g_poseV2FootWetRecorded
                    << ",layers=" << g_poseV2FootLayersRecorded << ",layers_nonzero=" << g_poseV2FootLayersNonZero
                    << ",audio_lod=" << g_poseV2FootLodRecorded << '\n';
                log << "POSE_V2_FX_EVENTS,recorded=" << Ctx().clip.fx.Events().size()
                    << ",skipped_shot=" << g_poseV2FxSkippedShot
                    << ",skipped_entity=" << g_poseV2FxSkippedEntity << ",world_owner=" << g_poseV2FxWorldOwner << ",no_world_position=" << g_poseV2FxUnresolvedParent << "/" << g_poseV2FxInvalidWorld
                    << ",area_rejected=" << Ctx().clip.areaRejected << ",impact_no_shot=" << g_poseV2FxImpactNoShot
                    << ",no_hash=" << g_poseV2FxNoHash << '\n';
                LogPoseV2FxCounts("POSE_V2_FX_RECORDED_BY_HASH", g_poseV2FxRecordedByHash);
                LogPoseV2FxCounts("POSE_V2_FX_SKIPPED_ENTITY_BY_HASH", g_poseV2FxSkippedByHash);
                LogPoseV2FxCounts("POSE_V2_FX_NO_HASH_BY_CALLER", g_poseV2FxNoHashByCaller);
                LogPoseV2FxCounts("POSE_V2_FX_STARTS_BY_CALLER", g_poseV2FxStartsByCaller);
                LogPoseV2FxRegistered("record");
                g_fxCheckRecordedEvents = Ctx().clip.fx.Events().size();
                g_fxCheckRecordedTracks = Ctx().clip.registeredFx.size();
                for (const replay::entity::RegisteredFxTrack& track : Ctx().clip.registeredFx)
                {
                    if (!track.frames.empty() && track.frames.front().planeCount)
                        log << "POSE_V2_REGISTERED_FX_PLANES,hash=0x" << std::hex << track.effectHash << std::dec
                            << ",planes=" << track.frames.front().planeCount << ",p0=" << track.frames.front().planes[0] << ";"
                            << track.frames.front().planes[1] << ";" << track.frames.front().planes[2] << ";" << track.frames.front().planes[3]
                            << ",aux=" << track.frames.front().aux350[0] << ";" << track.frames.front().aux350[1] << ";"
                            << track.frames.front().aux350[2] << ";" << track.frames.front().aux350[3] << '\n';
                }
                // The caps of the registered fx, with Debug=0 too (_SUMMARY): tracks kept,
                // refused at the guard, removed by the buffer's trim, and the longest track.
                log << "POSE_V2_REGISTERED_FX_CAP_SUMMARY,event=end,tracks=" << Ctx().clip.registeredFx.size()
                    << ",cap=" << replay::entity::kMaxRegisteredFxTracks << ",refused_tracks=" << g_poseV2RegisteredDropped
                    << ",trimmed_tracks=" << g_poseV2RegisteredTrimmed << ",frames=" << g_poseV2RegisteredFramesStored
                    << ",max_track_frames=" << g_poseV2RegisteredMaxFrames << ",frame_cap=none\n";
                log << "POSE_V2_REGISTERED_FX,tracks=" << Ctx().clip.registeredFx.size() << ",frames=" << g_poseV2RegisteredFramesStored
                    << ",skipped_old_keys=" << g_poseV2RegisteredSkippedOld << ",held_keys=" << g_poseV2RegisteredHeld
                    << ",dropped_tracks=" << g_poseV2RegisteredDropped << ",stale_attaches=" << g_poseV2RegisteredAttachStale
                    << ",late_owner_resolved=" << g_poseV2FxLateResolved << ",late_owner_lost=" << g_poseV2FxLateLost << ",owner_lost_ended=" << g_poseV2FxOwnerLostEnded
                    << ",attach_changes=" << g_poseV2FxAttachChanges
                    << ",tenant_attaches=" << g_poseV2FxTenantAttaches
                    << ",camera_ambient_skipped=" << g_poseV2CameraAmbientSkipped
                    << ",splits=" << g_poseV2RegisteredSplits << ",list=";
                for (const replay::entity::RegisteredFxTrack& track : Ctx().clip.registeredFx)
                    log << "0x" << std::hex << track.effectHash << std::dec << ":" << track.startMs << "-" << track.endMs << "/"
                        << track.frames.size() << "/f" << int(track.flag) << int(track.arg8) << int(track.arg9) << (track.attached ? "a" : "w") << ";";
                log << '\n';
                {
                    // The hashless registered calls and the script looped tracks they kept
                    // (script_fx_loop.h).
                    replay::scriptfx::LoopTally tally;
                    for (const replay::entity::RegisteredFxTrack& track : Ctx().clip.registeredFx)
                    {
                        if (track.callerSite != replay::fx::ScriptLoopStartSite() &&
                            track.callerSite != replay::fx::ScriptLoopKeepAliveSite())
                            continue;
                        replay::scriptfx::LoopTrackPlay row;
                        row.recStartMs = track.startMs;
                        row.recEndMs = track.endMs;
                        tally.Add(row);
                    }
                    log << "POSE_V2_SCRIPT_FX_REC,hashless_calls=" << g_scriptFxHashless.calls
                        << ",keep_alive_site=" << g_scriptFxHashless.keepAliveSite << ",live_key=" << g_scriptFxHashless.liveKey
                        << ",identity=" << g_scriptFxHashless.identity << ",mismatch=" << g_scriptFxHashless.mismatch
                        << ",unresolved=" << g_scriptFxHashless.unresolved << ",script_tracks=" << tally.tracks
                        << ",one_tick=" << tally.oneTick << ",rec_ms=" << tally.recMs << '\n';
                }
            }
            lap(g_poseV2EndTiming.tracksUs);
            // The ring's speech lines (up to 1024) are formatted on a worker and written
            // on a later tick (deferred_log.h), the same text; every number in them
            // follows an explicit base, so the text does not depend on the stream, and
            // this stream is left in dec as they left it.
            if (!speech.empty())
            {
                log << std::dec;
                replay::deferredlog::Log().Submit([calls = std::move(speech)]() {
                    std::ostringstream text;
                    for (const replay::audio::SpeechCall& call : calls)
                    {
                        text << "POSE_V2_AUDIO_SPEECH_ALL"
                             << ",voice=0x" << std::hex << call.voiceHash
                             << ",context=0x" << replay::audio::FinalizeHash(call.contextPartialHash)
                             << ",variation=" << std::dec << call.variation
                             << ",result=" << call.result
                             << ",entity=0x" << std::hex << call.speechEntity
                             << ",callers=0x" << call.callers[0] << "/0x" << call.callers[1] << "/0x" << call.callers[2]
                             << std::dec << '\n';
                    }
                    return text.str();
                });
            }
            log.flush();
            lap(g_poseV2EndTiming.speechUs);
        }
        {
            std::ostream& log = Ctx().log;
            log << "POSE_V2_FX_OBSERVER_STOP,total_calls=" << replay::fx::TotalCalls() << '\n';
        }
        {
            std::vector<std::uint8_t> snapshot;
            if (replay::fx::TakeGameplaySnapshot(snapshot))
                LogPoseV2FxSnapshot("gameplay", snapshot);
        }
        replay::capture::EndRecording(Ctx());
        replay::soundgeneric::EndRecording(Ctx());
        lap(g_poseV2EndTiming.tailUs);
    }

    EndTiming LastEndTiming() { return g_poseV2EndTiming; }

    std::uint32_t ShotImpacts() { return static_cast<std::uint32_t>(g_poseV2ShotImpacts); }

    void SetWatchEffect(std::uint32_t effectHash) { g_poseV2FxWatchHash = effectHash; }

    void SetFxReplay(bool enabled) { g_poseV2FxReplayEnabled = enabled; }
    void SetResearchWindows(bool enabled)
    {
        g_poseV2ResearchWindows = enabled;
        replay::audio::SetFullSoundCallers(enabled);
    }

    // Cheap scrub and the counters of the SCRUB_COST line.
    void SetStateDeferred(bool deferred)
    {
        g_poseV2StateDeferred = deferred;
        if (deferred)
            ++g_poseV2DeferredTicks;
    }
    bool StateDeferred() { return g_poseV2StateDeferred; }
    void NotePlayerFollow(bool moving)
    {
        g_playerFollowMoving = moving;
        g_playerFollowAtMs = GetTickCount64();
        if (!moving)
            ReleasePlayerSpeechHold();
    }
    bool PlayerSpeechHoldAt(float& x, float& y, float& z)
    {
        if (!g_speechHoldSinceMs)
            return false;
        if (!replay::speech::route::HoldActive(g_speechHoldSinceMs, GetTickCount64(), g_speechHoldLineAlive) ||
            !g_speechHoldPuppet || !replay::entities::Exists(g_speechHoldPuppet) ||
            g_speechHoldPlayer != PLAYER::PLAYER_PED_ID())
        {
            ReleasePlayerSpeechHold();
            return false;
        }
        float at[3]{};
        if (!replay::entities::Position(g_speechHoldPuppet, at))
            return false;
        x = at[0];
        y = at[1];
        z = at[2];
        return true;
    }

    std::size_t DropTrimmedRegisteredFx(std::uint32_t keepFrom)
    {
        std::vector<replay::entity::RegisteredFxTrack>& tracks = Ctx().clip.registeredFx;
        std::vector<std::uint8_t> held(tracks.size(), 0);
        for (const LiveRegistered& live : g_poseV2RegisteredLive)
            if (live.track < held.size())
                held[live.track] = 1;
        std::vector<std::size_t> remap;
        const std::size_t dropped = replay::entity::DropEmptiedRegisteredFx(tracks, keepFrom, held, remap);
        if (!dropped)
            return 0;
        // The live keys follow their tracks (a held track is never removed).
        for (LiveRegistered& live : g_poseV2RegisteredLive)
            if (live.track < remap.size())
                live.track = remap[live.track];
        g_poseV2RegisteredTrimmed += dropped;
        return dropped;
    }
    void SetFootEvents(bool replayOn)
    {
        g_footEventReplay = replayOn;
        if (!replayOn)
            replay::audio::SetFootEventRefuseOwn(false);
    }
    void SetShotEnvGroup(bool on) { g_shotEnvGroupReplay = on; }
    void SetFootMovement(bool replayOn)
    {
        g_footMovementReplay = replayOn;
        if (!replayOn)
            replay::audio::SetMovementReplay(false);
    }
    void SetMotionSpeed(bool replayOn)
    {
        g_motionSpeedReplay = replayOn;
        if (!replayOn)
            replay::pedaudiostate::SetMotionTargets({});
    }
    void SetPedSpeed(bool replayOn)
    {
        g_pedSpeedReplay = replayOn;
        if (!replayOn)
            replay::pedaudiostate::SetSpeedTargets({});
    }
    void SetStateBudget(bool settling, bool frozen)
    {
        g_poseV2StateBudgetUs = replay::budget::StateBudgetUs(settling, frozen);
    }
    bool StatePending()
    {
        // Bounded work only: blood and fire retries may wait on the engine
        // for seconds and have their own give-up rules.
        // The blood queue is bounded work too (it drains under its budget; a
        // record that must wait goes to the pending list).
        return g_poseV2DecalReconcileIncomplete || g_poseV2GoreRetryTicks > 0 || !g_poseV2BloodQueue.Empty();
    }
    StateCounters Counters()
    {
        StateCounters counters;
        counters.decalReconciles = g_poseV2DecalReconciles;
        counters.bloodReconciles = g_poseV2BloodReconciles;
        counters.goreReconciles = g_poseV2GoreReconciles;
        counters.fireReconciles = g_poseV2FireReconciles;
        counters.deferredTicks = g_poseV2DeferredTicks;
        return counters;
    }
}
