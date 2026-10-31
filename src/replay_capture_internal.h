// replay_capture_internal.h: the state and helpers the replay_capture*.cpp files share.
// Internal to those files, not a public header.
#pragma once

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
#include "game_sig.h"
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
        Context& Ctx();
        std::uint64_t ElapsedUs(std::uint64_t start, std::uint64_t end);
        extern FullAnimConfig g_fullAnim;
        void EndFullAnim();
        void UpdateFullAnim();
    void ReadVehicleLook(Vehicle vehicle, replay::VehicleLook& look, Hash& byHash, Hash& byHandle);
    void LogVehicleLook(const char* phase, const replay::VehicleLook& look, Hash byHash, Hash byHandle,
        std::uint32_t timeMs);
    extern std::vector<std::pair<Entity, std::uint32_t>> g_unresolvedTries;
    extern std::uint32_t g_unresolvedRetryLogs;

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
    extern std::vector<PoseV2MetapedProbe> g_poseV2MetapedProbes;
    extern PoseV2MetapedStats g_poseV2MetapedStats;
    extern std::size_t g_poseV2MetapedCursor;
    extern std::uint32_t g_poseV2MetapedLogs;
    void RecordPoseV2Metaped(Entity handle, replay::entity::ReplayId id, replay::metaped::Source source,
        std::uint32_t timeMs, const replay::entity::Appearance& layers);
    void SamplePoseV2Metaped();
    void SamplePoseV2Ropes();
    void RetirePoseV2Entity(std::size_t index, const char* reason);

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
    extern PoseV2ScriptTiming g_poseV2ScriptTiming;

    // Candidates come from a scan of the ped pool (people and animals) at
    // kPoseV2ScanIntervalMs; the player and its current mount are pinned.
    // replay::entity::SelectEntities decides who enters and leaves.
    constexpr ULONGLONG kPoseV2ScanIntervalMs = 200;
    extern ULONGLONG g_poseV2NextScanAt;
    // A fragment break (a blast breaking a wheel, a crate) makes its pieces in
    // that frame: the scan runs every tick for a while after one, so the
    // pieces are recorded from the frame they appear (at 200 ms they began up
    // to 200 ms late, the first frames of the blast without them).
    constexpr ULONGLONG kPoseV2BreakScanMs = 500;
    extern std::uint64_t g_poseV2BreaksSeen;
    extern ULONGLONG g_poseV2BreakScanUntil;

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
    extern PoseV2DiscoveryStats g_poseV2Discovery;

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
    extern PoseV2SelectionStats g_poseV2Selection;
    extern replay::entity::RecentRetirements g_poseV2Retired;
    void ScanPoseV2Entities(Ped mount);
    void StartPoseV2Capture();
    void UpdatePoseV2Capture(Ped mount);
    void StopPoseV2Capture(ULONGLONG elapsedMs);
}
