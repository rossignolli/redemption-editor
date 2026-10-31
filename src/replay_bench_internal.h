// replay_bench_internal.h: the state and helpers the replay_bench*.cpp files share (developer build only).
// Internal to those files, not a public header.
#pragma once

#include "replay_bench.h"
#include "bench_paths.h"
#include "guarded_memory.h"
#include "entity_cache.h"
#include "object_check.h" // the owner field of world_visible's object
#include "burn_level.h"
#include "engine_sigs.h" // the component table
#include "game_sig.h"
#include "object_look.h" // object_texture, object_tint
#include "replay_objects.h"
#include "weapon_observer.h" // strap_test
#include "replay_trains.h"

#include "bench_script.h"
#include "bench_shot.h"
#include "editor_keys.h" // the keys the hand-over names
#include "engine_hud.h"
#include "export_channel.h"
#include "export_probe.h"
#include "fire_observer.h"
#include "fx_observer.h"
#include "ped_gore.h"
#include "ped_blood.h"
#include "skeleton_resolve.h"
#include "replay_events.h"
#include "replay_playback.h"
#include "pose_capture.h"
#include "profiler.h"
#include "rope_observer.h"
#include "replay_world.h"
#include "timecycle_state.h"
#include "sun_control.h" // moon_log, moon_angle
#include "moon_track.h"
#include "render_target.h" // op render_target

#include "main.h"
#include "natives.h"

#include <set>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace replay::bench
{
        constexpr std::uint64_t kModelTimeoutMs = 5000;
        constexpr Hash kAddReasonDefault = 0x2CD419DC;
        // A mark holds the scene still this long so the host's screenshot,
        // taken when it reads the log line, shows the marked moment.
        constexpr int kDefaultMarkHoldMs = 800;
        // How long a mark waits for the ReShade addon to save its screenshot.
        constexpr std::uint64_t kShotTimeoutMs = 2000;
        extern ShotShared* g_shot;
        extern bool g_censusOn;
        extern float g_censusCenter[3];
        extern float g_censusRadius;
        extern float g_lootWatchCenter[3];
        extern float g_lootWatchRadius;
        extern std::uint64_t g_lootWatchUntil;
        extern bool g_lootUseSeen;
        extern std::string g_lootUseDetail;
        extern Object g_lootTarget;
        extern std::uint64_t g_lootUseNextLog;
        extern std::uint64_t g_lootUseUntil;
        extern std::uint64_t g_lootUseStarted;
        extern std::string g_lootUseLast;

        // Marks taken while recording, repeated by play_marks at the same
        // timeline time with the camera that was active.
        struct RecordedMark
        {
            std::uint64_t recMs = 0;
            std::string tag;
            std::vector<std::string> cam; // cam step arguments, empty = none
        };
        extern std::vector<RecordedMark> g_recMarks;

        // Player controls held by `hold`, pressed again every tick until due.
        struct HeldControl
        {
            Hash action = 0;
            std::uint64_t until = 0;
        };
        extern std::vector<HeldControl> g_held;

        // rope_probe: script ropes whose vertices are placed on a fixed sag
        // curve every tick (the mechanism rope playback will use). Deleted
        // by the scenario hygiene.
        struct ProbeRope
        {
            int id = 0;
            Vector3 from{};
            Vector3 to{};
            float sag = 0.0f;
            float force = 0.0f; // rope_probe2: ROPE_FORCE_LENGTH after the first report
            int phase = 0;
        };
        extern std::vector<ProbeRope> g_probeRopes;
        extern std::vector<std::string> g_camArgs;
        extern std::size_t g_playMark;
        extern bool g_playMarkShooting;
        extern Context* g_context;
        extern bool g_puppetVelocity;
        extern int g_puppetCollision;
        struct PuppetMotion
        {
            Vector3 last{};
            std::uint64_t at = 0;
        };
        extern std::map<Entity, PuppetMotion> g_puppetMotion;
        extern Hooks* g_hooks;

        struct Run
        {
            bool active = false;
            std::string name;     // file name without extension
            std::string running;  // the renamed .running path
            Script script;
            std::size_t next = 0;
            std::uint64_t startedAt = 0;
            std::uint64_t waitUntil = 0;
            // A step that spans frames (model load, scrub) keeps its index.
            bool stepPending = false;
            std::uint64_t stepStartedAt = 0;
            std::uint64_t exportTimeoutMs = 900000; // render export: the "export" op's limit
            int scrubDirection = 0;
            bool scrubFine = false;
            std::uint64_t scrubUntil = 0;
            std::uint64_t recordStartedAt = 0;
            int handOverPhase = 0; // 0 wait rec, 1 recording, 2 wait play, 3 playing
            bool lassoFallback = false; // lasso_target: TASK_COMBAT_PED already issued
            bool keepPlayback = false;  // keep_play: the clip keeps playing after the run (editor tests)
        };
        extern Run g_run;

        // moon_angle up: one candidate turn angle per 100 ms (the sky update
        // has drawn the last one).
        struct MoonSeek
        {
            std::uint64_t stepStartedAt = 0;
            int next = 0;
            std::uint64_t at = 0;
            double orbit = 0.0;
            double bestAngle = 0.0;
            float bestElevation = -1000.0f;
        };
        extern MoonSeek g_moonSeek;
        constexpr int kMoonSeekCandidates = 24;
        std::string MoonLine(const std::string& tag);

        // Survey: a grid of ground heights and the highest thing above the
        // ground at each point (trees, rocks, props), written as CSV.
        constexpr const char* kSurveyDir = REPLAY_BENCH_DIR "\\survey\\";
        struct Survey
        {
            bool active = false;
            Vector3 origin{};
            float radius = 0.0f;
            float step = 0.0f;
            int cells = 0;  // per axis
            int next = 0;   // cell index
            int written = 0;
            int obstacles = 0;
            std::ofstream csv;
            std::string path;
        };
        extern Survey g_survey;
        extern Cam g_topCam;
        extern std::map<std::string, Entity> g_spawned;
        extern std::map<std::string, Vehicle> g_benchTrains;
        extern std::map<std::string, int> g_benchPickups;
        extern std::map<std::string, int> g_benchPropSets;
        extern Object g_worldHidden;
        extern Object g_worldPicked;
        int MemSnap(const void* entity, const std::string& file, std::size_t& total, bool deep);
        extern std::map<std::string, Entity> g_stage;
        struct StageSpec
        {
            Hash model = 0;
            Vector3 at{};
            float heading = 0.0f;
        };
        extern std::map<std::string, StageSpec> g_stageSpecs;
        std::ostream& Log();
        std::size_t DeleteAll(std::map<std::string, Entity>& entities);
        std::size_t DeleteSpawned();
        int LoadModel(Hash model, std::uint64_t now, std::string& detail);
        void StopTopCam();
        bool SurveySlice(Ped player);
        void RequestShot(const std::string& tag);
        bool ShotSaved();

        // Door watches: rotation and position of a door by its door hash,
        // logged when they change, in the background of the run.
        struct DoorWatch
        {
            Hash hash = 0;
            std::uint64_t until = 0;
            std::uint64_t started = 0;
            bool have = false;
            Vector3 lastRot{};
            Vector3 lastPos{};
            float firstYaw = 0.0f;
            float maxOpen = 0.0f;
            int samples = 0;
        };
        extern std::vector<DoorWatch> g_doorWatches;

        // IPL watches: does a game script undo a toggled IPL?
        struct IplWatch
        {
            Hash hash = 0;
            std::uint64_t until = 0;
            std::uint64_t started = 0;
            std::uint64_t nextAt = 0;
            int last = -1;
            int flips = 0;
        };
        extern std::vector<IplWatch> g_iplWatches;
        Hash IplHashArg(const std::string& text);

        // Depth of field research, applied every tick to the bench camera.
        // `params`: the 10 x 8-byte argument block of 0xE4B7945EF4F1BFB2
        // (dof strength, near, far, focal length, min, max, 4 flags; layout
        // from a public reverse engineering write-up).
        struct DofProbe
        {
            int mode = 0; // 0 off, 1 params, 2 hidof, 3 focus
            float values[11] = {}; // params: [10] = focus distance (0: not set)
        };
        extern DofProbe g_dofProbe;
        bool StartCam(const Vector3& at, const Vector3& look, float fov);
        extern bool g_envLocked;
        void UnlockEnvironment();
        Entity ResolveSpawned(const std::string& id);
        Ped ResolvePed(const std::string& id);
        Vector3 Coords(const Step& step, std::size_t first);
        void SetHostile(Ped ped, Ped target);
        std::string ThrowProjectile(Ped ped, const Vector3& at);
        Object HeldWeaponObject(Ped ped);

        enum class StepResult
        {
            Done,
            Pending,
            Failed,
        };
        StepResult Execute(const Step& step, std::uint64_t now, std::string& detail);
}
