#include "replay_bench.h"
#include "bench_paths.h"
#include "guarded_memory.h"
#include "entity_cache.h"
#include "object_check.h" // the owner field of world_visible's object
#include "burn_level.h"
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
    namespace
    {
        constexpr const char* kInboxDir = REPLAY_BENCH_DIR "\\inbox\\";
        // The host drops this file to end the running scenario at once.
        constexpr const char* kAbortFile = REPLAY_BENCH_DIR "\\abort";
        constexpr std::uint64_t kReadyPollMs = 500;
        constexpr std::uint64_t kInboxPollMs = 1000;
        constexpr std::uint64_t kModelTimeoutMs = 5000;
        // A scenario is only accepted fresh from the host; one left behind by
        // a crashed session must never run by itself in a later game.
        constexpr std::uint64_t kMaxScenarioAgeMs = 30000;
        constexpr Hash kAddReasonDefault = 0x2CD419DC;
        // A mark holds the scene still this long so the host's screenshot,
        // taken when it reads the log line, shows the marked moment.
        constexpr int kDefaultMarkHoldMs = 800;
        // How long a mark waits for the ReShade addon to save its screenshot.
        constexpr std::uint64_t kShotTimeoutMs = 2000;
        ShotShared* g_shot = nullptr;
        LONG64 g_shotPending = 0;
        // object_census: every mark (recorded and played) also logs the objects
        // near this point (replay::objects::BenchObjectCensus).
        bool g_censusOn = false;
        float g_censusCenter[3]{};
        float g_censusRadius = 0.0f;
        // loot_watch, loot_use, loot_entity (BENCH_LOOT_*).
        float g_lootWatchCenter[3]{};
        float g_lootWatchRadius = 0.0f;
        std::uint64_t g_lootWatchUntil = 0;
        bool g_lootUseSeen = false;
        std::string g_lootUseDetail;
        Object g_lootTarget = 0;
        std::uint64_t g_lootUseNextLog = 0;
        std::uint64_t g_lootUseUntil = 0;
        std::uint64_t g_lootUseStarted = 0;
        std::string g_lootUseLast;

        // Marks taken while recording, repeated by play_marks at the same
        // timeline time with the camera that was active.
        struct RecordedMark
        {
            std::uint64_t recMs = 0;
            std::string tag;
            std::vector<std::string> cam; // cam step arguments, empty = none
        };
        std::vector<RecordedMark> g_recMarks;

        // Player controls held by `hold`, pressed again every tick until due.
        struct HeldControl
        {
            Hash action = 0;
            std::uint64_t until = 0;
        };
        std::vector<HeldControl> g_held;

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
        std::vector<ProbeRope> g_probeRopes;

        void HoldProbeRopes()
        {
            for (const ProbeRope& rope : g_probeRopes)
            {
                if (!PHYSICS::DOES_ROPE_EXIST(rope.id))
                    continue;
                const int count = PHYSICS::GET_ROPE_VERTEX_COUNT(rope.id);
                for (int v = 0; v < count; ++v)
                {
                    const float t = count > 1 ? static_cast<float>(v) / static_cast<float>(count - 1) : 0.0f;
                    const float x = rope.from.x + (rope.to.x - rope.from.x) * t;
                    const float y = rope.from.y + (rope.to.y - rope.from.y) * t;
                    const float z = rope.from.z + (rope.to.z - rope.from.z) * t - rope.sag * 4.0f * t * (1.0f - t);
                    PHYSICS::_0xE54BF2CE6C7D23A9(rope.id, v, x, y, z);
                }
            }
        }

        std::size_t DeleteProbeRopes()
        {
            std::size_t deleted = 0;
            for (ProbeRope& rope : g_probeRopes)
            {
                if (PHYSICS::DOES_ROPE_EXIST(rope.id))
                {
                    PHYSICS::DELETE_ROPE(&rope.id);
                    ++deleted;
                }
            }
            g_probeRopes.clear();
            return deleted;
        }
        std::vector<std::string> g_camArgs;
        std::size_t g_playMark = 0;
        bool g_playMarkShooting = false;

        Context* g_context = nullptr;
        // Frozen puppets have no physical speed, and the vegetation modifier
        // only grows with speed.
        bool g_puppetVelocity = false;
        int g_puppetCollision = 0; // 0 off, 1 collision, 2 collision and not frozen
        std::uint64_t g_puppetCollisionLogAt = 0;
        struct PuppetMotion
        {
            Vector3 last{};
            std::uint64_t at = 0;
        };
        std::map<Entity, PuppetMotion> g_puppetMotion;
        std::uint64_t g_puppetVelocityLogAt = 0;
        Hooks* g_hooks = nullptr;

        bool g_ready = false;
        std::uint64_t g_nextReadyPoll = 0;
        std::uint64_t g_nextInboxPoll = 0;

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
        Run g_run;

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
        MoonSeek g_moonSeek;
        constexpr int kMoonSeekCandidates = 24;

        // The sky object's moon now, one line (never fails the run).
        std::string MoonLine(const std::string& tag)
        {
            std::ostringstream line;
            const int h = CLOCK::GET_CLOCK_HOURS(), m = CLOCK::GET_CLOCK_MINUTES(), s = CLOCK::GET_CLOCK_SECONDS();
            line << "tag=" << tag << ",clock=" << h << ':' << m << ':' << s << ",clock_s=" << (h * 3600 + m * 60 + s)
                 << ",date=" << CLOCK::GET_CLOCK_YEAR() << '-' << CLOCK::GET_CLOCK_MONTH() << '-' << CLOCK::GET_CLOCK_DAY_OF_MONTH();
            replay::sunctl::MoonState state;
            if (!replay::sunctl::ReadMoon(state))
                line << ",ok=0,error=" << replay::sunctl::ResolveError();
            else
            {
                const float sun[3] = {state.sun.x, state.sun.y, state.sun.z};
                const float moon[3] = {state.moon.x, state.moon.y, state.moon.z};
                line << std::setprecision(9) << ",ok=1,angle=" << state.angle << ",orbit=" << state.orbit
                     << ",sun_el=" << replay::moontrack::ElevationDeg(sun) << ",moon_el=" << replay::moontrack::ElevationDeg(moon)
                     << ",moon=" << moon[0] << ';' << moon[1] << ';' << moon[2];
            }
            return line.str();
        }

        // Survey: a grid of ground heights and the highest thing above the
        // ground at each point (trees, rocks, props), written as CSV.
        constexpr const char* kSurveyDir = REPLAY_BENCH_DIR "\\survey\\";
        constexpr int kSurveyPointsPerTick = 150;
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
        Survey g_survey;
        Cam g_topCam = 0;

        // Peds and objects spawned by the running scenario, by scenario id.
        std::map<std::string, Entity> g_spawned;
        // Mission trains of the scenario (train_spawn), deleted as trains, not
        // entity by entity.
        std::map<std::string, Vehicle> g_benchTrains;
        // Pickups of the scenario (pickup_spawn), removed as pickups; the
        // world object world_visible hid (shown back at clear).
        std::map<std::string, int> g_benchPickups;
        // Prop sets of the scenario (propset_spawn), deleted with the spawned
        // entities.
        std::map<std::string, int> g_benchPropSets;
        Object g_worldHidden = 0;
        Object g_worldPicked = 0;
        // The stage: objects that outlive scenarios until stage_clear. The
        // host tears it down before every hot reload, since a reload loses
        // these handles.
        bool GuardedCopyBytes(std::uint8_t* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size);
        }

        // A read-only memory graph of one entity for the burn look hunt.
        // Regions are keyed by the pointer path from the entity ("E+28+18"),
        // so two snapshots of one entity diff by path offline
        // (tools/bench/memsnap_diff.py). Only committed readable pages are
        // copied (VirtualQuery; no guard pages) and every copy is guarded.
        bool SnapReadable(std::uintptr_t address, std::size_t size)
        {
            std::uintptr_t at = address;
            const std::uintptr_t end = address + size;
            while (at < end)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT ||
                    (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                    return false;
                at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            }
            return true;
        }


        struct SnapRegion
        {
            std::string path;
            std::uintptr_t address = 0;
            std::vector<std::uint8_t> bytes;
            int depth = 0;
        };

        // Breadth first from the entity: region sizes by depth, deeper under
        // the draw handler (E+28), a global byte budget.
        int MemSnap(const void* entity, const std::string& file, std::size_t& total, bool deep)
        {
            const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::uintptr_t imageEnd = module;
            {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
                imageEnd = module + nt->OptionalHeader.SizeOfImage;
            }
            constexpr std::size_t kBudget = 96u * 1024u * 1024u;
            std::vector<SnapRegion> regions;
            std::set<std::uintptr_t> seen;
            std::vector<std::size_t> queue;
            total = 0;
            auto add = [&](const std::string& path, std::uintptr_t address, std::size_t size, int depth) {
                if (total + size > kBudget || seen.count(address) || !SnapReadable(address, size))
                    return;
                SnapRegion region;
                region.path = path;
                region.address = address;
                region.depth = depth;
                region.bytes.resize(size);
                if (!GuardedCopyBytes(region.bytes.data(), reinterpret_cast<const void*>(address), size))
                    return;
                seen.insert(address);
                total += size;
                regions.push_back(std::move(region));
                queue.push_back(regions.size() - 1);
            };
            // Named regions first (their paths stay stable): draw handler,
            // its effect and block (fire path +0x18), the ped data (+0x748),
            // its effect (+0xC0) and that effect's block (+0x18).
            {
                auto ptrAt = [&](std::uintptr_t address) -> std::uintptr_t {
                    std::uint8_t raw[8]{};
                    std::uintptr_t value = 0;
                    if (address && SnapReadable(address, 8) && GuardedCopyBytes(raw, reinterpret_cast<const void*>(address), 8))
                        std::memcpy(&value, raw, 8);
                    return value;
                };
                const std::uintptr_t handlerAddress = ptrAt(reinterpret_cast<std::uintptr_t>(entity) + 0x28);
                const std::uintptr_t effect = ptrAt(handlerAddress + 0x18);
                const std::uintptr_t pedData = handlerAddress ? ptrAt(handlerAddress + 0x748) : 0;
                const std::uintptr_t pedEffect = pedData ? ptrAt(pedData + 0xC0) : 0;
                if (handlerAddress)
                    add("H", handlerAddress, 0x1000, 9);
                if (effect)
                    add("FX", effect, 0x400, 9);
                if (effect && ptrAt(effect + 0x18))
                    add("FXB", ptrAt(effect + 0x18), 0x800, 9);
                if (pedData)
                    add("PD", pedData, 0x800, 9);
                if (pedEffect)
                    add("PFX", pedEffect, 0x800, 9);
                if (pedEffect && ptrAt(pedEffect + 0x18))
                    add("PFXB", ptrAt(pedEffect + 0x18), 0x1000, 9);
            }
            // The entity's nodes in the engine's per-entity fire record tree
            // (root at module+0x4E2F610; node +0x08 right (tagged), +0x10 left,
            // +0x20 entity, +0x28 int key; the record lives in the node: +0x141
            // flags read by _IS_ENTITY_CONSUMED_BY_FIRE 0xF9A8DC, RESURRECT_PED
            // removes it through 0x12A3D58). Named "FR0", "FR1"...
            {
                std::uint8_t raw[8]{};
                std::uintptr_t root = 0;
                if (GuardedCopyBytes(raw, reinterpret_cast<const void*>(module + 0x4E2F610), 8))
                    std::memcpy(&root, raw, 8);
                std::vector<std::uintptr_t> stack;
                if (root)
                    stack.push_back(root);
                int visited = 0;
                int found = 0;
                while (!stack.empty() && visited < 2048)
                {
                    const std::uintptr_t node = stack.back();
                    stack.pop_back();
                    ++visited;
                    std::uint8_t head[0x30]{};
                    if (!SnapReadable(node, sizeof(head)) || !GuardedCopyBytes(head, reinterpret_cast<const void*>(node), sizeof(head)))
                        continue;
                    std::uintptr_t right = 0;
                    std::uintptr_t left = 0;
                    std::uintptr_t key = 0;
                    std::memcpy(&right, head + 0x08, 8);
                    std::memcpy(&left, head + 0x10, 8);
                    std::memcpy(&key, head + 0x20, 8);
                    right &= ~static_cast<std::uintptr_t>(1);
                    if (key == reinterpret_cast<std::uintptr_t>(entity))
                        add("FR" + std::to_string(found++), node, 0x200, 0);
                    if (right)
                        stack.push_back(right);
                    if (left)
                        stack.push_back(left);
                }
            }
            // The entity's row of the engine component table (index [E+0x9C]
            // & 0x1FFFF minus [0x39ECE40], table [0x39ECE48], stride 0x148,
            // tagged pointers; ped_blood.cpp): damage, surface and the rest.
            {
                std::uint8_t raw[8]{};
                std::uint32_t handle = 0;
                std::uint32_t indexBase = 0;
                std::uintptr_t table = 0;
                if (GuardedCopyBytes(raw, static_cast<const std::uint8_t*>(entity) + 0x9C, 4))
                    std::memcpy(&handle, raw, 4);
                if (GuardedCopyBytes(raw, reinterpret_cast<const void*>(module + 0x39ECE40), 4))
                    std::memcpy(&indexBase, raw, 4);
                if (GuardedCopyBytes(raw, reinterpret_cast<const void*>(module + 0x39ECE48), 8))
                    std::memcpy(&table, raw, 8);
                const std::uint32_t index = (handle & 0x1FFFF) - indexBase;
                if (table && handle && index < 0x20000)
                    add("C", table + static_cast<std::uintptr_t>(index) * 0x148, 0x148, 0);
            }
            add("E", reinterpret_cast<std::uintptr_t>(entity), 0x2000, 0);
            for (std::size_t q = 0; q < queue.size(); ++q)
            {
                const std::size_t index = queue[q];
                const int depth = regions[index].depth;
                const bool component = regions[index].path.rfind("C", 0) == 0;
                const bool fireRecord = regions[index].path.rfind("FR", 0) == 0;
                const bool handler = regions[index].path.rfind("E+28", 0) == 0 || component;
                const bool meta = regions[index].path.rfind("C+50", 0) == 0;
                const int maxDepth = fireRecord ? 3 : (meta && deep ? 4 : (component ? 2 : (handler ? 3 : 2)));
                if (depth >= maxDepth)
                    continue;
                const std::size_t childSize = depth == 0 ? (component ? 0x800 : 0x400) : (handler && depth == 1 ? 0x1000 : (handler ? 0x300 : 0x100));
                const std::size_t count = regions[index].bytes.size() / 8;
                for (std::size_t n = 0; n < count; ++n)
                {
                    std::uint64_t value = 0;
                    std::memcpy(&value, regions[index].bytes.data() + n * 8, 8);
                    if (component && depth == 0)
                        value &= ~static_cast<std::uint64_t>(1);
                    if (value < 0x10000 || value > 0x7FFFFFFFFFFFull || (value & 7) || (value >= module && value < imageEnd))
                        continue;
                    char step[24];
                    std::snprintf(step, sizeof(step), "+%zx", n * 8);
                    add(regions[index].path + step, static_cast<std::uintptr_t>(value), childSize, depth + 1);
                }
            }
            std::ofstream out(file, std::ios::binary);
            if (!out)
                return -1;
            const std::uint64_t base = module;
            out.write(reinterpret_cast<const char*>(&base), 8);
            for (const SnapRegion& region : regions)
            {
                const auto pathSize = static_cast<std::uint16_t>(region.path.size());
                const auto size = static_cast<std::uint32_t>(region.bytes.size());
                const std::uint64_t address = region.address;
                out.write(reinterpret_cast<const char*>(&pathSize), 2);
                out.write(region.path.data(), pathSize);
                out.write(reinterpret_cast<const char*>(&address), 8);
                out.write(reinterpret_cast<const char*>(&size), 4);
                out.write(reinterpret_cast<const char*>(region.bytes.data()), size);
            }
            return static_cast<int>(regions.size());
        }


        std::map<std::string, Entity> g_stage;
        struct StageSpec
        {
            Hash model = 0;
            Vector3 at{};
            float heading = 0.0f;
        };
        std::map<std::string, StageSpec> g_stageSpecs; // to rebuild the stage (restage)

        std::ostream& Log() { return g_context->log; }

        bool PlayerReady()
        {
            if (DLC::GET_IS_LOADING_SCREEN_ACTIVE())
                return false;
            const Ped player = PLAYER::PLAYER_PED_ID();
            return player && ENTITY::DOES_ENTITY_EXIST(player) &&
                !ENTITY::IS_ENTITY_DEAD(player) &&
                PLAYER::IS_PLAYER_CONTROL_ON(PLAYER::PLAYER_ID());
        }

        bool FindScenario(std::string& path, std::string& name)
        {
            WIN32_FIND_DATAA found{};
            const std::string pattern = std::string(kInboxDir) + "*.scn";
            HANDLE search = FindFirstFileA(pattern.c_str(), &found);
            if (search == INVALID_HANDLE_VALUE)
                return false;
            FindClose(search);
            path = std::string(kInboxDir) + found.cFileName;
            name = found.cFileName;
            name.resize(name.size() - 4);

            FILETIME nowFile{};
            GetSystemTimeAsFileTime(&nowFile);
            ULARGE_INTEGER nowTicks{};
            nowTicks.LowPart = nowFile.dwLowDateTime;
            nowTicks.HighPart = nowFile.dwHighDateTime;
            ULARGE_INTEGER written{};
            written.LowPart = found.ftLastWriteTime.dwLowDateTime;
            written.HighPart = found.ftLastWriteTime.dwHighDateTime;
            const std::uint64_t ageMs = nowTicks.QuadPart > written.QuadPart
                ? (nowTicks.QuadPart - written.QuadPart) / 10000
                : 0;
            if (ageMs > kMaxScenarioAgeMs)
            {
                const std::string stale = path.substr(0, path.size() - 4) + ".stale";
                MoveFileExA(path.c_str(), stale.c_str(), MOVEFILE_REPLACE_EXISTING);
                Log() << "BENCH_STALE,name=" << name << ",age_ms=" << ageMs << '\n';
                Log().flush();
                return false;
            }
            return true;
        }

        std::size_t DeleteAll(std::map<std::string, Entity>& entities)
        {
            std::size_t deleted = 0;
            for (auto& [id, entity] : entities)
            {
                if (entity && ENTITY::DOES_ENTITY_EXIST(entity))
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(entity, TRUE, TRUE);
                    ENTITY::DELETE_ENTITY(&entity);
                    ++deleted;
                }
            }
            entities.clear();
            return deleted;
        }

        std::size_t DeleteTrains()
        {
            std::size_t deleted = 0;
            for (auto& [id, train] : g_benchTrains)
            {
                deleted += train && ENTITY::DOES_ENTITY_EXIST(train) ? 1 : 0;
                replay::trains::BenchDelete(train);
            }
            g_benchTrains.clear();
            return deleted;
        }

        std::size_t DeletePickups()
        {
            std::size_t removed = 0;
            for (auto& [id, pickup] : g_benchPickups)
            {
                if (pickup && OBJECT::DOES_PICKUP_EXIST(pickup))
                {
                    OBJECT::REMOVE_PICKUP(pickup);
                    ++removed;
                }
            }
            g_benchPickups.clear();
            if (g_worldHidden && ENTITY::DOES_ENTITY_EXIST(g_worldHidden))
                ENTITY::SET_ENTITY_VISIBLE(g_worldHidden, TRUE);
            g_worldHidden = 0;
            g_worldPicked = 0;
            return removed;
        }

        std::size_t DeletePropSets()
        {
            std::size_t deleted = 0;
            for (auto& [id, propSet] : g_benchPropSets)
            {
                if (propSet && PROPSET::DOES_PROP_SET_EXIST(propSet))
                {
                    PROPSET::_DELETE_PROP_SET(propSet, TRUE, TRUE);
                    ++deleted;
                }
            }
            g_benchPropSets.clear();
            return deleted;
        }

        std::size_t DeleteSpawned()
        {
            return DeleteAll(g_spawned) + DeleteProbeRopes() + DeleteTrains() + DeletePickups() + DeletePropSets();
        }

        // Streams the model in; Pending until loaded, Failed on a bad model
        // or after kModelTimeoutMs.
        int LoadModel(Hash model, std::uint64_t now, std::string& detail)
        {
            if (!STREAMING::IS_MODEL_IN_CDIMAGE(model) || !STREAMING::IS_MODEL_VALID(model))
            {
                detail = "invalid_model";
                return -1;
            }
            if (STREAMING::HAS_MODEL_LOADED(model))
                return 1;
            STREAMING::REQUEST_MODEL(model, FALSE);
            if (now - g_run.stepStartedAt > kModelTimeoutMs)
            {
                detail = "model_timeout";
                return -1;
            }
            return 0;
        }

        void StopTopCam()
        {
            if (!g_topCam)
                return;
            CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, TRUE, 0);
            CAM::SET_CAM_ACTIVE(g_topCam, FALSE);
            CAM::DESTROY_CAM(g_topCam, FALSE);
            g_topCam = 0;
        }

        // One slice of the survey grid; true when the grid is done.
        bool SurveySlice(Ped player)
        {
            Survey& sv = g_survey;
            const int total = sv.cells * sv.cells;
            for (int n = 0; n < kSurveyPointsPerTick && sv.next < total; ++n, ++sv.next)
            {
                const float dx = -sv.radius + static_cast<float>(sv.next % sv.cells) * sv.step;
                const float dy = -sv.radius + static_cast<float>(sv.next / sv.cells) * sv.step;
                if (dx * dx + dy * dy > sv.radius * sv.radius)
                    continue;
                const float x = sv.origin.x + dx;
                const float y = sv.origin.y + dy;
                float groundZ = 0.0f;
                const bool ground = MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, sv.origin.z + 120.0f, &groundZ, FALSE) != FALSE;
                const ScrHandle probe = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(
                    x, y, sv.origin.z + 120.0f, x, y, sv.origin.z - 120.0f, -1, player, 7);
                BOOL hit = FALSE;
                Vector3 end{};
                Vector3 normal{};
                Entity entity = 0;
                SHAPETEST::GET_SHAPE_TEST_RESULT(probe, &hit, &end, &normal, &entity);
                float waterZ = 0.0f;
                const bool water = WATER::TEST_VERTICAL_PROBE_AGAINST_ALL_WATER(x, y, sv.origin.z + 120.0f, 0, &waterZ) != 0;
                const float above = hit && ground ? end.z - groundZ : 0.0f;
                if (above > 0.5f)
                    ++sv.obstacles;
                sv.csv << dx << ',' << dy << ',' << (ground ? 1 : 0) << ',' << groundZ << ','
                       << (hit ? 1 : 0) << ',' << end.z << ',' << normal.z << ','
                       << (entity ? ENTITY::GET_ENTITY_TYPE(entity) : 0) << ','
                       << (water ? 1 : 0) << ',' << waterZ << '\n';
                ++sv.written;
            }
            return sv.next >= total;
        }

        void RequestShot(const std::string& tag)
        {
            std::snprintf(g_shot->tag, kShotTagSize, "bench_%s_%s", g_run.name.c_str(), tag.c_str());
            g_shotPending = InterlockedIncrement64(&g_shot->requested);
        }

        bool ShotSaved()
        {
            return InterlockedCompareExchange64(&g_shot->saved, 0, 0) >= g_shotPending;
        }

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
        std::vector<DoorWatch> g_doorWatches;

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
        std::vector<IplWatch> g_iplWatches;

        // A number (decimal, negative as the scripts write it, or 0x hex)
        // or an IPL name (joaat).
        Hash IplHashArg(const std::string& text)
        {
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                return static_cast<Hash>(std::strtoul(text.c_str() + 2, nullptr, 16));
            bool number = !text.empty();
            for (std::size_t n = 0; n < text.size() && number; ++n)
                number = (text[n] >= '0' && text[n] <= '9') || (n == 0 && text[n] == '-' && text.size() > 1);
            if (number)
                return static_cast<Hash>(static_cast<std::uint32_t>(std::strtoll(text.c_str(), nullptr, 10)));
            return MISC::GET_HASH_KEY(text.c_str());
        }

        void UpdateIplWatches(std::uint64_t now)
        {
            for (std::size_t n = 0; n < g_iplWatches.size();)
            {
                IplWatch& watch = g_iplWatches[n];
                if (now >= watch.nextAt)
                {
                    watch.nextAt = now + 1000;
                    const int state = STREAMING::IS_IPL_ACTIVE_HASH(watch.hash) ? 1 : 0;
                    if (watch.last >= 0 && state != watch.last)
                        ++watch.flips;
                    watch.last = state;
                    Log() << "BENCH_IPL_SAMPLE,hash=" << watch.hash << ",t_ms=" << (now - watch.started) << ",active=" << state
                          << ",flips=" << watch.flips << '\n';
                }
                if (now >= watch.until)
                {
                    Log() << "BENCH_IPL_WATCH,hash=" << watch.hash << ",active=" << watch.last << ",flips=" << watch.flips << '\n';
                    g_iplWatches.erase(g_iplWatches.begin() + static_cast<std::ptrdiff_t>(n));
                    continue;
                }
                ++n;
            }
        }

        // Depth of field research, applied every tick to the bench camera.
        // `params`: the 10 x 8-byte argument block of 0xE4B7945EF4F1BFB2
        // (dof strength, near, far, focal length, min, max, 4 flags; layout
        // from a public reverse engineering write-up).
        struct DofProbe
        {
            int mode = 0; // 0 off, 1 params, 2 hidof, 3 focus
            float values[11] = {}; // params: [10] = focus distance (0: not set)
        };
        DofProbe g_dofProbe;

        void ApplyDofProbe()
        {
            if (!g_dofProbe.mode || !g_topCam || !CAM::DOES_CAM_EXIST(g_topCam))
                return;
            if (g_dofProbe.mode == 1)
            {
                alignas(8) std::uint8_t block[10 * 8]{};
                for (int n = 0; n < 6; ++n)
                    std::memcpy(block + n * 8, &g_dofProbe.values[n], 4);
                for (int n = 6; n < 10; ++n)
                {
                    const std::int32_t flag = static_cast<std::int32_t>(g_dofProbe.values[n]);
                    std::memcpy(block + n * 8, &flag, 4);
                }
                CAM::_SET_CAM_DOF_PARAMS(g_topCam, reinterpret_cast<Any*>(block));
                if (g_dofProbe.values[10] > 0.0f)
                    CAM::_SET_CAM_FOCUS_DISTANCE(g_topCam, g_dofProbe.values[10]);
            }
            else if (g_dofProbe.mode == 2)
                GRAPHICS::SET_HIDOF_OVERRIDE(TRUE, TRUE, g_dofProbe.values[0], g_dofProbe.values[1], g_dofProbe.values[2],
                    g_dofProbe.values[3]);
            else if (g_dofProbe.mode == 3)
                CAM::_SET_CAM_FOCUS_DISTANCE(g_topCam, g_dofProbe.values[0]);
        }

        float YawDelta(float a, float b)
        {
            float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
            return std::fabs(d);
        }

        void UpdateDoorWatches(std::uint64_t now)
        {
            for (std::size_t n = 0; n < g_doorWatches.size();)
            {
                DoorWatch& watch = g_doorWatches[n];
                const Entity door = ENTITY::_GET_ENTITY_BY_DOORHASH(watch.hash, 0);
                if (door && ENTITY::DOES_ENTITY_EXIST(door))
                {
                    const Vector3 rot = ENTITY::GET_ENTITY_ROTATION(door, 2);
                    const Vector3 pos = ENTITY::GET_ENTITY_COORDS(door, TRUE, FALSE);
                    if (!watch.have)
                    {
                        watch.have = true;
                        watch.firstYaw = rot.z;
                        watch.lastRot = rot;
                        watch.lastPos = pos;
                    }
                    const float moved = std::fabs(pos.x - watch.lastPos.x) + std::fabs(pos.y - watch.lastPos.y) +
                        std::fabs(pos.z - watch.lastPos.z);
                    if (YawDelta(rot.z, watch.lastRot.z) > 0.2f || moved > 0.01f)
                    {
                        ++watch.samples;
                        watch.maxOpen = (std::max)(watch.maxOpen, YawDelta(rot.z, watch.firstYaw));
                        Log() << "BENCH_DOOR_SAMPLE,hash=" << watch.hash << ",t_ms=" << (now - watch.started)
                              << ",rot_z=" << rot.z << ",pos=" << pos.x << ";" << pos.y << ";" << pos.z
                              << ",ratio=" << OBJECT::DOOR_SYSTEM_GET_OPEN_RATIO(watch.hash) << '\n';
                        watch.lastRot = rot;
                        watch.lastPos = pos;
                    }
                }
                if (now >= watch.until || !g_run.active)
                {
                    Log() << "BENCH_DOOR_WATCH,hash=" << watch.hash << ",found=" << (watch.have ? 1 : 0)
                          << ",samples=" << watch.samples << ",max_open_deg=" << watch.maxOpen << ",returned="
                          << (YawDelta(watch.lastRot.z, watch.firstYaw) < 2.0f ? 1 : 0) << '\n';
                    g_doorWatches.erase(g_doorWatches.begin() + static_cast<std::ptrdiff_t>(n));
                    continue;
                }
                ++n;
            }
        }

        bool StartCam(const Vector3& at, const Vector3& look, float fov)
        {
            StopTopCam();
            g_topCam = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", at.x, at.y, at.z, 0.0f, 0.0f, 0.0f, fov, FALSE, 2);
            if (!g_topCam)
                return false;
            CAM::POINT_CAM_AT_COORD(g_topCam, look.x, look.y, look.z);
            CAM::SET_CAM_ACTIVE(g_topCam, TRUE);
            CAM::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, TRUE, 0);
            return true;
        }

        void Finish(const char* result, const std::string& reason)
        {
            StopTopCam();
            if (g_survey.active)
            {
                g_survey.csv.close();
                g_survey.active = false;
            }
            // Hygiene: nothing of the scenario survives it, except the replay
            // buffer's ring: stopping it here would make every save of the
            // next scenario start from zero.
            if (g_hooks->Recording() && !g_hooks->BufferRecording())
                g_hooks->StopRecording();
            if (g_context->transport.Playing() && !g_run.keepPlayback)
                g_hooks->StopPlayback();
            const std::size_t deleted = DeleteSpawned();

            const std::string done = g_run.running.substr(0, g_run.running.size() - 8) +
                (std::string(result) == "passed" ? ".done" : ".failed");
            MoveFileExA(g_run.running.c_str(), done.c_str(), MOVEFILE_REPLACE_EXISTING);

            Log() << "BENCH_DONE"
                  << ",name=" << g_run.name
                  << ",result=" << result
                  << ",steps=" << g_run.next << "/" << g_run.script.steps.size()
                  << ",elapsed_ms=" << (GetTickCount64() - g_run.startedAt)
                  << ",deleted=" << deleted;
            if (!reason.empty())
                Log() << ",reason=" << reason;
            Log() << '\n';
            Log().flush();
            g_run = Run{};
        }

        void StartScenario(std::uint64_t now)
        {
            std::string path;
            std::string name;
            if (!FindScenario(path, name))
                return;
            const std::string running = path.substr(0, path.size() - 4) + ".running";
            if (!MoveFileExA(path.c_str(), running.c_str(), MOVEFILE_REPLACE_EXISTING))
                return;

            std::ifstream file(running);
            std::stringstream text;
            text << file.rdbuf();
            file.close();

            g_run = Run{};
            g_run.name = name;
            g_run.running = running;
            g_run.startedAt = now;
            std::string error;
            if (!ParseScript(text.str(), g_run.script, error))
            {
                Log() << "BENCH_PARSE_ERROR,name=" << name << ",error=" << error << '\n';
                g_run.active = true;
                Finish("failed", "parse");
                return;
            }
            g_run.active = true;
            g_censusOn = false;
            Log() << "BENCH_START,name=" << name << ",steps=" << g_run.script.steps.size() << '\n';
            Log().flush();
        }

        bool g_envLocked = false;
        std::uint64_t g_nextPurge = 0;

        // While the environment is locked the stage has no ambient life:
        // nothing new spawns (per-frame densities) and, between takes, ambient
        // peds and birds around the player are deleted. The player, the
        // player's horses and every mission entity (the bench's own) are kept.
        void KeepWorldEmpty(std::uint64_t now)
        {
            PED::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            PED::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            PED::_SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            PED::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            PED::_SET_SCENARIO_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            PED::_SET_SCENARIO_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            VEHICLE::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            VEHICLE::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);

            if (now < g_nextPurge || g_hooks->Recording() || g_context->transport.Playing())
                return;
            g_nextPurge = now + 1000;
            const Player playerId = PLAYER::PLAYER_ID();
            const Ped player = PLAYER::PLAYER_PED_ID();
            const Ped keep[] = {player, PLAYER::_GET_ACTIVE_HORSE_FOR_PLAYER(playerId),
                PLAYER::_GET_SADDLE_HORSE_FOR_PLAYER(playerId), PED::GET_MOUNT(player)};
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
            static int peds[1024];
            const int count = worldGetAllPeds(peds, 1024);
            int deleted = 0;
            int birds = 0;
            for (int n = 0; n < count; ++n)
            {
                Ped ped = peds[n];
                if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped) || PED::IS_PED_A_PLAYER(ped) ||
                    ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped))
                    continue;
                bool kept = false;
                for (Ped k : keep)
                    kept = kept || (k && k == ped);
                if (kept)
                    continue;
                const Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE);
                const float dx = p.x - at.x;
                const float dy = p.y - at.y;
                if (dx * dx + dy * dy > 250.0f * 250.0f)
                    continue;
                if (ENTITY::_GET_IS_BIRD(ped))
                    ++birds;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(ped, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&ped);
                ++deleted;
            }
            if (deleted)
            {
                Log() << "BENCH_PURGE,deleted=" << deleted << ",birds=" << birds << '\n';
                Log().flush();
            }
        }

        void UnlockEnvironment()
        {
            if (!g_envLocked)
                return;
            CLOCK::PAUSE_CLOCK(FALSE, 0);
            MISC::_SET_WEATHER_TYPE_FROZEN(FALSE);
            MISC::CLEAR_WEATHER_TYPE_PERSIST();
            ENTITY::SET_ENTITY_INVINCIBLE(PLAYER::PLAYER_PED_ID(), FALSE);
            g_envLocked = false;
        }

        Entity ResolveSpawned(const std::string& id)
        {
            const auto found = g_spawned.find(id);
            if (found == g_spawned.end())
            {
                const auto train = g_benchTrains.find(id);
                return train == g_benchTrains.end() ? 0 : train->second;
            }
            return found->second;
        }

        Ped ResolvePed(const std::string& id)
        {
            if (id == "player")
                return PLAYER::PLAYER_PED_ID();
            if (id == "player_horse")
            {
                // The player's own horse: his saddle horse, else his active one.
                const Player playerId = PLAYER::PLAYER_ID();
                Ped horse = PLAYER::_GET_SADDLE_HORSE_FOR_PLAYER(playerId);
                if (!horse || !ENTITY::DOES_ENTITY_EXIST(horse))
                    horse = PLAYER::_GET_ACTIVE_HORSE_FOR_PLAYER(playerId);
                return horse && ENTITY::DOES_ENTITY_EXIST(horse) ? horse : 0;
            }
            const auto found = g_spawned.find(id);
            return found == g_spawned.end() || !ENTITY::IS_ENTITY_A_PED(found->second) ? 0 : found->second;
        }

        Vector3 Coords(const Step& step, std::size_t first)
        {
            Vector3 v{};
            ArgFloat(step, first, v.x);
            ArgFloat(step, first + 1, v.y);
            ArgFloat(step, first + 2, v.z);
            return v;
        }

        // Two relationship groups that hate each other, as the game's scripts
        // set up a scripted fight (fight, melee, lasso_target).
        void SetHostile(Ped ped, Ped target)
        {
            static Hash fighters = 0;
            static Hash foes = 0;
            if (!fighters)
                PED::ADD_RELATIONSHIP_GROUP("BENCH_FIGHTERS", &fighters);
            if (!foes)
                PED::ADD_RELATIONSHIP_GROUP("BENCH_FOES", &foes);
            PED::SET_PED_RELATIONSHIP_GROUP_HASH(ped, fighters);
            PED::SET_PED_RELATIONSHIP_GROUP_HASH(target, foes);
            PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, fighters, foes); // hate
            PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, foes, fighters);
        }

        // TASK_THROW_PROJECTILE. Schema: (ped, Any, Any, Any); community use
        // passes the target coordinates as floats, as the GTA version does.
        // Live: an NPC throws dynamite this way; the player drops it at his
        // feet, and a lantern (melee) in an NPC's hand is not thrown.
        std::string ThrowProjectile(Ped ped, const Vector3& at)
        {
            const Hash held = WEAPON::_GET_PED_CURRENT_HELD_WEAPON(ped);
            invoke<Void>(0x7285951DBF6B5A51, ped, at.x, at.y, at.z);
            char text[64]{};
            std::snprintf(text, sizeof(text), "held=0x%X", static_cast<unsigned>(held));
            return text;
        }

        // The weapon object in the ped's hand: the primary hand attach point,
        // else the ped's weapon object.
        Object HeldWeaponObject(Ped ped)
        {
            Object object = WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(ped, 0);
            if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
                object = WEAPON::_GET_PED_WEAPON_OBJECT(ped, FALSE);
            return object && ENTITY::DOES_ENTITY_EXIST(object) ? object : 0;
        }

        enum class StepResult
        {
            Done,
            Pending,
            Failed,
        };

        StepResult Execute(const Step& step, std::uint64_t now, std::string& detail)
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (step.op == "reset")
            {
                const Vector3 at = Coords(step, 0);
                float heading = 0.0f;
                float radius = 0.0f;
                ArgFloat(step, 3, heading);
                ArgFloat(step, 4, radius);
                if (g_hooks->Recording())
                    g_hooks->StopRecording();
                if (g_context->transport.Playing())
                    g_hooks->StopPlayback();
                DeleteSpawned();
                // A mounted player is not moved by SET_ENTITY_COORDS (the
                // player stays wherever the horse is).
                if (PED::IS_PED_ON_MOUNT(player))
                    PED::_REMOVE_PED_FROM_MOUNT(player, TRUE, TRUE);
                TASK::CLEAR_PED_TASKS_IMMEDIATELY(player, FALSE, TRUE);
                ENTITY::SET_ENTITY_COORDS(player, at.x, at.y, at.z, FALSE, FALSE, FALSE, TRUE);
                ENTITY::SET_ENTITY_HEADING(player, heading);
                FIRE::STOP_FIRE_IN_RANGE(at.x, at.y, at.z, radius);
                MISC::CLEAR_AREA(at.x, at.y, at.z, radius, 0);
                PLAYER::CLEAR_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID());
                ENTITY::SET_ENTITY_HEALTH(player, ENTITY::GET_ENTITY_MAX_HEALTH(player, FALSE), 0);
                return StepResult::Done;
            }
            if (step.op == "teleport")
            {
                const Vector3 at = Coords(step, 0);
                if (PED::IS_PED_ON_MOUNT(player))
                    PED::_REMOVE_PED_FROM_MOUNT(player, TRUE, TRUE);
                ENTITY::SET_ENTITY_COORDS(player, at.x, at.y, at.z, FALSE, FALSE, FALSE, FALSE);
                float heading = 0.0f;
                if (ArgFloat(step, 3, heading))
                    ENTITY::SET_ENTITY_HEADING(player, heading);
                return StepResult::Done;
            }
            if (step.op == "time")
            {
                int hour = 12;
                int minute = 0;
                ArgInt(step, 0, hour);
                ArgInt(step, 1, minute);
                CLOCK::SET_CLOCK_TIME(hour, minute, 0);
                return StepResult::Done;
            }
            if (step.op == "weather")
            {
                // With seconds, a transition over time as winter2 makes it
                // (SET_WEATHER_TYPE(hash, false, true, true, 40f, false)): the
                // weather manager's over-time fields, which the environment
                // track v2 records.
                float seconds = 0.0f;
                if (step.args.size() >= 2 && ArgFloat(step, 1, seconds) && seconds > 0.0f)
                {
                    MISC::SET_WEATHER_TYPE(MISC::GET_HASH_KEY(step.args[0].c_str()), FALSE, TRUE, TRUE, seconds, FALSE);
                    Log() << "BENCH_WEATHER,type=" << step.args[0] << ",transition_s=" << seconds << '\n';
                }
                else
                    MISC::SET_WEATHER_TYPE(MISC::GET_HASH_KEY(step.args[0].c_str()), TRUE, TRUE, FALSE, 0.0f, FALSE);
                return StepResult::Done;
            }
            if (step.op == "wait")
            {
                int ms = 0;
                ArgInt(step, 0, ms);
                g_run.waitUntil = now + static_cast<std::uint64_t>(ms);
                return StepResult::Done;
            }
            if (step.op == "walk")
            {
                const Vector3 at = Coords(step, 0);
                float speed = 1.0f;
                ArgFloat(step, 3, speed);
                TASK::TASK_GO_STRAIGHT_TO_COORD(player, at.x, at.y, at.z, speed, -1, 40000.0f, 0.5f, 0);
                return StepResult::Done;
            }
            if (step.op == "record")
            {
                const bool start = step.args[0] == "start";
                if (start == g_hooks->Recording())
                {
                    detail = start ? "already_recording" : "not_recording";
                    return StepResult::Failed;
                }
                if (start)
                {
                    g_hooks->StartRecording();
                    g_run.recordStartedAt = now;
                    g_recMarks.clear();
                }
                else
                    g_hooks->StopRecording();
                return StepResult::Done;
            }
            // The playing clip stays after the run: the host drives the editor
            // next (tools/bench editor tests).
            // Render export: export the playing clip.
            if (step.op == "export")
            {
                namespace ec = replay::exportchannel;
                if (!g_run.stepPending)
                {
                    if (!g_context->transport.Playing())
                    {
                        detail = "not_playing";
                        return StepResult::Failed;
                    }
                    static ec::Settings settings; // big (paths): not on the fiber's stack
                    settings = ec::Settings{};
                    settings.endMs = g_context->transport.DurationMs();
                    settings.audio = ec::AudioMode::None;
                    settings.preset = ec::VideoPreset::H264Nvenc;
                    settings.thumbnail = 1;
                    settings.chapters = 0;
                    std::string out = REPLAY_BENCH_VIDEOS_DIR;
                    std::string name = g_run.name;
                    g_run.exportTimeoutMs = 900000;
                    for (const std::string& arg : step.args)
                    {
                        const std::size_t eq = arg.find('=');
                        const std::string key = arg.substr(0, eq);
                        const std::string value = arg.substr(eq + 1);
                        if (key == "preset")
                        {
                            static const std::pair<const char*, ec::VideoPreset> presets[] = {{"h264", ec::VideoPreset::H264Nvenc},
                                {"hevc", ec::VideoPreset::HevcNvenc}, {"av1", ec::VideoPreset::Av1Nvenc}, {"preview", ec::VideoPreset::PreviewH264},
                                {"prores", ec::VideoPreset::ProRes422Hq}, {"dnxhr", ec::VideoPreset::DnxhrHqx}, {"ffv1", ec::VideoPreset::LosslessFfv1},
                                {"hevc-lossless", ec::VideoPreset::LosslessNvencHevc}, {"png", ec::VideoPreset::PngSequence}};
                            bool found = false;
                            for (const auto& [label, preset] : presets)
                                if (value == label)
                                {
                                    settings.preset = preset;
                                    found = true;
                                }
                            if (!found)
                            {
                                detail = "bad_preset";
                                return StepResult::Failed;
                            }
                        }
                        else if (key == "fps")
                        {
                            const std::size_t slash = value.find('/');
                            settings.rate.num = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                            settings.rate.den = slash == std::string::npos ? 1u : static_cast<std::uint32_t>(std::strtoul(value.c_str() + slash + 1, nullptr, 10));
                        }
                        else if (key == "samples")
                            settings.samples = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                        else if (key == "shutter")
                            settings.shutterDegrees = std::strtof(value.c_str(), nullptr);
                        else if (key == "start")
                            settings.startMs = std::strtoull(value.c_str(), nullptr, 10);
                        else if (key == "end")
                            settings.endMs = std::min<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10), g_context->transport.DurationMs());
                        else if (key == "letterbox")
                            settings.letterboxAspect = std::strtof(value.c_str(), nullptr);
                        else if (key == "grain")
                            settings.grain = std::strtof(value.c_str(), nullptr);
                        else if (key == "audio")
                            settings.audio = value == "1" ? ec::AudioMode::RealtimePass : ec::AudioMode::None;
                        else if (key == "preroll")
                            settings.prerollFrames = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                        else if (key == "out")
                            out = value;
                        else if (key == "name")
                            name = value;
                        else if (key == "timeout")
                            g_run.exportTimeoutMs = std::strtoull(value.c_str(), nullptr, 10);
                    }
                    static const char* const extensions[] = {".mkv", ".mkv", ".mp4", ".mp4", ".mp4", ".mov", "", ".mp4", ".mov", ".mp4"};
                    const std::uint32_t index = static_cast<std::uint32_t>(settings.preset);
                    const std::string path = out + "\\" + name + (index < 10 ? extensions[index] : ".mkv");
                    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, settings.outputPath, ec::kPathCapacity);
                    if (!replay::exportprobe::BenchRequest(&settings, sizeof(settings), Log()))
                    {
                        detail = "export_refused";
                        return StepResult::Failed;
                    }
                    Log() << "BENCH_EXPORT,phase=requested,name=" << name << ",path=" << path << '\n';
                    return StepResult::Pending;
                }
                const int poll = replay::exportprobe::BenchPoll();
                if (poll == 0 && now - g_run.stepStartedAt < g_run.exportTimeoutMs)
                    return StepResult::Pending;
                if (poll == 0)
                {
                    replay::exportprobe::BenchCancel(Log());
                    detail = "export_timeout";
                    return StepResult::Failed;
                }
                detail = poll > 0 ? "export_done" : "export_failed";
                return poll > 0 ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "keep_play")
            {
                g_run.keepPlayback = true;
                return StepResult::Done;
            }
            if (step.op == "marker_add" || step.op == "marker_lens" || step.op == "marker_clear" || step.op == "cam_log" || step.op == "fx_add" || step.op == "fx_clear" ||
                step.op == "spline_default" || step.op == "freecam" || step.op == "focus" || step.op == "player_far")
                return g_hooks->CameraOp(step.op, step.args, detail) ? StepResult::Done : StepResult::Failed;
            if (step.op == "buffer")
            {
                g_hooks->Buffer(step.args[0]);
                return StepResult::Done;
            }
            if (step.op == "play")
            {
                if (!g_hooks->StartPlayback(now))
                {
                    detail = "no_timeline";
                    return StepResult::Failed;
                }
                return StepResult::Done;
            }
            if (step.op == "stop_play")
            {
                g_hooks->StopPlayback();
                return StepResult::Done;
            }
            if (step.op == "safe_exit")
            {
                if (!g_hooks->SafeExitTest(step.args[0]))
                {
                    detail = "unknown_fault";
                    return StepResult::Failed;
                }
                Log() << "BENCH_SAFE_EXIT,fault=" << step.args[0] << '\n';
                return StepResult::Done;
            }
            if (step.op == "pause" || step.op == "resume")
            {
                if (!g_context->transport.Playing())
                {
                    detail = "not_playing";
                    return StepResult::Failed;
                }
                g_hooks->SetPaused(step.op == "pause", now);
                return StepResult::Done;
            }
            if (step.op == "clean_view")
            {
                g_hooks->SetCleanView(step.args[0] != "0");
                return StepResult::Done;
            }
            if (step.op == "seek")
            {
                if (!g_context->transport.Playing())
                {
                    detail = "not_playing";
                    return StepResult::Failed;
                }
                g_hooks->SetPaused(true, now);
                const std::uint64_t at = std::strtoull(step.args[0].c_str(), nullptr, 10);
                g_context->transport.Seek(std::min<std::uint64_t>(at, g_context->transport.DurationMs()));
                return StepResult::Done;
            }
            if (step.op == "scrub")
            {
                if (!g_context->transport.Playing())
                {
                    detail = "not_playing";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                {
                    int ms = 0;
                    ArgInt(step, 1, ms);
                    g_run.scrubDirection = step.args[0] == "back" ? -1 : 1;
                    g_run.scrubFine = step.args.size() > 2;
                    g_run.scrubUntil = now + static_cast<std::uint64_t>(ms);
                    return StepResult::Pending;
                }
                if (now < g_run.scrubUntil)
                    return StepResult::Pending;
                g_run.scrubDirection = 0;
                detail = "playhead_ms=" + std::to_string(g_context->transport.PlayheadMs());
                return StepResult::Done;
            }
            if (step.op == "survey")
            {
                if (!g_run.stepPending)
                {
                    Survey& sv = g_survey;
                    sv = Survey{};
                    sv.origin = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                    ArgFloat(step, 0, sv.radius);
                    ArgFloat(step, 1, sv.step);
                    sv.cells = static_cast<int>(2.0f * sv.radius / sv.step) + 1;
                    CreateDirectoryA(REPLAY_BENCH_DIR, nullptr);
                    CreateDirectoryA(kSurveyDir, nullptr);
                    sv.path = std::string(kSurveyDir) + g_run.name + "-" + std::to_string(GetTickCount64()) + ".csv";
                    sv.csv.open(sv.path, std::ios::out | std::ios::trunc);
                    if (!sv.csv)
                    {
                        detail = "csv_open_failed";
                        return StepResult::Failed;
                    }
                    sv.csv << "# origin=" << sv.origin.x << ',' << sv.origin.y << ',' << sv.origin.z
                           << " radius=" << sv.radius << " step=" << sv.step << '\n'
                           << "dx,dy,ground,ground_z,hit,hit_z,normal_z,entity_type,water,water_z\n";
                    sv.active = true;
                    return StepResult::Pending;
                }
                if (!SurveySlice(player))
                    return StepResult::Pending;
                g_survey.csv.close();
                g_survey.active = false;
                std::ostringstream text;
                text << "points=" << g_survey.written << ",obstacles=" << g_survey.obstacles
                     << ",origin=" << g_survey.origin.x << ";" << g_survey.origin.y << ";" << g_survey.origin.z
                     << ",csv=" << g_survey.path;
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "topcam")
            {
                StopTopCam();
                if (step.args[0] == "off")
                    return StepResult::Done;
                float height = 120.0f;
                float fov = 60.0f;
                ArgFloat(step, 0, height);
                ArgFloat(step, 1, fov);
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                g_topCam = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA",
                    at.x, at.y, at.z + height, -90.0f, 0.0f, 0.0f, fov, FALSE, 2);
                if (!g_topCam)
                {
                    detail = "cam_failed";
                    return StepResult::Failed;
                }
                CAM::SET_CAM_ACTIVE(g_topCam, TRUE);
                CAM::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, TRUE, 0);
                return StepResult::Done;
            }
            if (step.op == "dims")
            {
                const Hash model = MISC::GET_HASH_KEY(step.args[0].c_str());
                std::string why;
                const int loaded = LoadModel(model, now, why);
                if (loaded == 0)
                    return StepResult::Pending;
                if (loaded < 0)
                {
                    detail = "model=" + step.args[0] + ",status=" + why;
                    return StepResult::Done;
                }
                Vector3 lo{};
                Vector3 hi{};
                MISC::GET_MODEL_DIMENSIONS(model, &lo, &hi);
                STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
                std::ostringstream text;
                text << "model=" << step.args[0] << ",status=ok,size=" << (hi.x - lo.x) << ";" << (hi.y - lo.y)
                     << ";" << (hi.z - lo.z) << ",min=" << lo.x << ";" << lo.y << ";" << lo.z
                     << ",max=" << hi.x << ";" << hi.y << ";" << hi.z;
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "play_marks")
            {
                if (!g_context->transport.Playing())
                {
                    detail = "not_playing";
                    return StepResult::Failed;
                }
                // A new scripted camera needs a frame or two before it renders:
                // each mark's camera is switched in ahead of time, right after
                // the previous screenshot, never at the mark itself.
                const auto preCam = [](std::size_t index) {
                    if (index >= g_recMarks.size())
                        return;
                    const std::vector<std::string>& cam = g_recMarks[index].cam;
                    if (cam.empty() || cam == g_camArgs)
                        return;
                    Step camStep;
                    camStep.args = cam;
                    float fov = 50.0f;
                    ArgFloat(camStep, 6, fov);
                    StartCam(Coords(camStep, 0), Coords(camStep, 3), fov);
                    g_camArgs = cam;
                };
                if (!g_run.stepPending)
                {
                    g_playMark = 0;
                    g_playMarkShooting = false;
                    preCam(0);
                }
                while (g_playMark < g_recMarks.size())
                {
                    const RecordedMark& mark = g_recMarks[g_playMark];
                    if (g_playMarkShooting)
                    {
                        if (g_shot && !ShotSaved() && now - g_run.stepStartedAt < kShotTimeoutMs * 20)
                            return StepResult::Pending;
                        g_playMarkShooting = false;
                        ++g_playMark;
                        preCam(g_playMark);
                        continue;
                    }
                    // Never during the prepare hold (the playhead sits at the
                    // first create while the puppets are built: a mark near 0
                    // would print before them).
                    if (g_context->transport.PlayheadMs() < mark.recMs || replay::playback::Preparing())
                        return StepResult::Pending;
                    if (g_censusOn)
                        replay::objects::BenchObjectCensus(mark.tag, true, g_context->transport.PlayheadMs(), g_censusCenter,
                            g_censusRadius);
                    Log() << "BENCH_PLAY_MARK,tag=" << mark.tag << ",rec_ms=" << mark.recMs
                          << ",playhead_ms=" << g_context->transport.PlayheadMs();
                    if (g_shot)
                    {
                        RequestShot("play_" + mark.tag);
                        Log() << ",shot=" << g_shot->tag;
                        g_playMarkShooting = true;
                    }
                    else
                    {
                        ++g_playMark;
                    }
                    Log() << '\n';
                    Log().flush();
                }
                detail = "marks=" + std::to_string(g_recMarks.size());
                return StepResult::Done;
            }
            if (step.op == "mark" && g_shot)
            {
                // First the frame-exact ReShade screenshot, then the log line.
                if (!g_run.stepPending)
                {
                    char tag[kShotTagSize];
                    std::snprintf(tag, sizeof(tag), "%02zu_%s", g_run.next, step.args[0].c_str());
                    RequestShot(tag);
                    // The recording clock, not the bench tick clock: a record
                    // start that stalls the tick (world snapshot) would put the
                    // bench clock ahead and the playback marks with it.
                    if (g_hooks->Recording())
                    {
                        g_recMarks.push_back({replay::pose::capture::NowMs(), step.args[0], g_camArgs});
                        if (g_censusOn)
                            replay::objects::BenchObjectCensus(step.args[0], false, replay::pose::capture::NowMs(), g_censusCenter,
                                g_censusRadius);
                    }
                    else if (g_censusOn && g_context->transport.Playing())
                        replay::objects::BenchObjectCensus(step.args[0], true, g_context->transport.PlayheadMs(), g_censusCenter,
                            g_censusRadius);
                    return StepResult::Pending;
                }
                const bool saved = ShotSaved();
                if (!saved && now - g_run.stepStartedAt < kShotTimeoutMs)
                    return StepResult::Pending;
                detail = std::string("shot=") + (saved ? g_shot->tag : "timeout") + ",";
            }
            if (step.op == "mark")
            {
                int hold = kDefaultMarkHoldMs;
                ArgInt(step, 1, hold);
                g_run.waitUntil = now + static_cast<std::uint64_t>(hold);
                const Vector3 pos = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                std::ostringstream where;
                where << ",pos=" << pos.x << ";" << pos.y << ";" << pos.z << ",heading=" << ENTITY::GET_ENTITY_HEADING(player);
                detail += "tag=" + step.args[0] + where.str() + ",playhead_ms=" + std::to_string(g_context->transport.PlayheadMs()) +
                    ",recording=" + (g_hooks->Recording() ? "1" : "0") +
                    (g_hooks->Recording() ? ",rec_ms=" + std::to_string(replay::pose::capture::NowMs()) +
                        ",bench_ms=" + std::to_string(now - g_run.recordStartedAt) : std::string()) +
                    ",playing=" + (g_context->transport.Playing() ? "1" : "0");
                return StepResult::Done;
            }
            if (step.op == "spawn_ped")
            {
                const Hash model = MISC::GET_HASH_KEY(step.args[1].c_str());
                if (const int loaded = LoadModel(model, now, detail); loaded <= 0)
                    return loaded < 0 ? StepResult::Failed : StepResult::Pending;
                const Vector3 at = Coords(step, 2);
                float heading = 0.0f;
                ArgFloat(step, 5, heading);
                Ped ped = PED::CREATE_PED(model, at.x, at.y, at.z, heading, FALSE, FALSE, FALSE, FALSE);
                STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
                if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
                {
                    detail = "create_failed";
                    return StepResult::Failed;
                }
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(ped, TRUE, TRUE);
                PED::_SET_RANDOM_OUTFIT_VARIATION(ped, TRUE);
                PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, TRUE);
                g_spawned[step.args[0]] = ped;
                detail = "handle=" + std::to_string(ped);
                return StepResult::Done;
            }
            if (step.op == "env_lock")
            {
                int hour = 12;
                ArgInt(step, 0, hour);
                CLOCK::SET_CLOCK_TIME(hour, 0, 0);
                CLOCK::PAUSE_CLOCK(TRUE, 0);
                // As the game's scripts pin a mission's weather.
                MISC::SET_WEATHER_TYPE(MISC::GET_HASH_KEY(step.args[1].c_str()), TRUE, TRUE, FALSE, 0.0f, FALSE);
                MISC::_SET_WEATHER_TYPE_FROZEN(TRUE);
                ENTITY::SET_ENTITY_INVINCIBLE(player, TRUE);
                g_envLocked = true;
                return StepResult::Done;
            }
            if (step.op == "env_unlock")
            {
                UnlockEnvironment();
                return StepResult::Done;
            }
            // What a mission does to the look, by the same natives (winter2.c),
            // and the engine state read back.
            if (step.op == "timecycle_modifier")
            {
                if (step.args[0] == "off")
                    GRAPHICS::CLEAR_TIMECYCLE_MODIFIER();
                else
                {
                    GRAPHICS::SET_TIMECYCLE_MODIFIER(step.args[0].c_str());
                    float strength = 1.0f;
                    if (ArgFloat(step, 1, strength))
                        GRAPHICS::SET_TIMECYCLE_MODIFIER_STRENGTH(strength);
                }
                detail = "index=" + std::to_string(GRAPHICS::GET_TIMECYCLE_MODIFIER_INDEX());
                return StepResult::Done;
            }
            if (step.op == "timecycle_transition")
            {
                float seconds = 1.0f;
                ArgFloat(step, 1, seconds);
                GRAPHICS::SET_TRANSITION_TIMECYCLE_MODIFIER(step.args[0].c_str(), seconds);
                return StepResult::Done;
            }
            if (step.op == "weather_variation")
            {
                if (step.args[1] == "off")
                    MISC::_CLEAR_WEATHER_VARIATION(step.args[0].c_str(), TRUE);
                else
                    MISC::_SET_WEATHER_VARIATION(step.args[0].c_str(), step.args[1].c_str());
                return StepResult::Done;
            }
            if (step.op == "timecycle_log")
            {
                std::ostringstream line;
                line << "tag=" << step.args[0] << ",native_index=" << GRAPHICS::GET_TIMECYCLE_MODIFIER_INDEX();
                std::string error;
                replay::TimecycleSample live;
                if (!replay::tcstate::Resolve(error))
                    line << ",ok=0,error=" << error;
                else if (!replay::tcstate::Read(live))
                    line << ",ok=0,error=read";
                else
                {
                    line << ",ok=1,mod=0x" << std::hex << live.modifierKey << std::dec << ",mod_index=" << live.modifierIndex
                         << ",strength=" << live.strength << ",trans=0x" << std::hex << live.transitionKey << std::dec
                         << ",trans_index=" << live.transitionIndex << ",progress=" << live.progress << ",speed=" << live.speed
                         << ",fade=" << static_cast<int>(live.fadeOut) << ",variations=";
                    if (!live.variationCount)
                        line << "none";
                    for (std::size_t n = 0; n < live.variationCount; ++n)
                        line << (n ? ";" : "") << "0x" << std::hex << live.variations[n].type << ":0x" << live.variations[n].variation
                             << std::dec;
                    line << ",modifiers=" << replay::tcstate::ModifierCount();
                }
                Log() << "BENCH_TIMECYCLE," << line.str() << '\n';
                detail = line.str();
                return StepResult::Done;
            }
            if (step.op == "env_edit")
                return g_hooks->EnvironmentOp(step.args[0], step.args[1], detail) ? StepResult::Done : StepResult::Failed;
            // The moon's angles (moon_track.h) of the sky object, read
            // (moon_log) and set as a trainer would leave them (moon_angle: a
            // value, or "up": the turn angle that puts the moon highest at this
            // clock, searched over 24 candidates).
            if (step.op == "render_target")
            {
                const bool ok = replay::rendertarget::BenchOp(step.args, detail);
                Log() << "BENCH_RT," << detail << ",ok=" << (ok ? 1 : 0) << '\n';
                return ok ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "moon_log")
            {
                detail = MoonLine(step.args[0]);
                Log() << "BENCH_MOON," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "moon_angle")
            {
                replay::sunctl::MoonState state;
                if (!replay::sunctl::ReadMoon(state))
                {
                    detail = std::string("error=") + replay::sunctl::ResolveError();
                    return StepResult::Failed;
                }
                if (step.args[0] != "up")
                {
                    double angle = std::atof(step.args[0].c_str());
                    const bool ok = replay::sunctl::WriteMoon(angle, state.orbit);
                    detail = "angle=" + std::to_string(angle) + ",was=" + std::to_string(state.angle);
                    Log() << "BENCH_MOON,tag=moon_angle," << detail << ",ok=" << (ok ? 1 : 0) << '\n';
                    return ok ? StepResult::Done : StepResult::Failed;
                }
                MoonSeek& seek = g_moonSeek;
                if (seek.stepStartedAt != g_run.stepStartedAt)
                {
                    seek = MoonSeek{};
                    seek.stepStartedAt = g_run.stepStartedAt;
                    seek.orbit = state.orbit;
                }
                if (now < seek.at)
                    return StepResult::Pending;
                if (seek.next > 0)
                {
                    // What the update drew from the last candidate.
                    const float moon[3] = {state.moon.x, state.moon.y, state.moon.z};
                    const float elevation = state.directions ? replay::moontrack::ElevationDeg(moon) : -1000.0f;
                    if (elevation > seek.bestElevation)
                    {
                        seek.bestElevation = elevation;
                        seek.bestAngle = state.angle;
                    }
                }
                if (seek.next < kMoonSeekCandidates)
                {
                    const double angle = replay::moontrack::kTurn * seek.next / kMoonSeekCandidates;
                    replay::sunctl::WriteMoon(angle, seek.orbit);
                    ++seek.next;
                    seek.at = now + 100;
                    return StepResult::Pending;
                }
                const bool ok = seek.bestElevation > -1000.0f && replay::sunctl::WriteMoon(seek.bestAngle, seek.orbit);
                std::ostringstream text;
                text << "up,angle=" << seek.bestAngle << ",moon_el=" << seek.bestElevation << ",ok=" << (ok ? 1 : 0);
                detail = text.str();
                Log() << "BENCH_MOON,tag=moon_angle," << detail << '\n';
                return ok ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "spawn_vehicle")
            {
                const Hash model = MISC::GET_HASH_KEY(step.args[1].c_str());
                if (const int loaded = LoadModel(model, now, detail); loaded <= 0)
                    return loaded < 0 ? StepResult::Failed : StepResult::Pending;
                const Vector3 at = Coords(step, 2);
                float heading = 0.0f;
                ArgFloat(step, 5, heading);
                // Draft animals are created with the vehicle.
                Vehicle vehicle = VEHICLE::CREATE_VEHICLE(model, at.x, at.y, at.z, heading, FALSE, FALSE, FALSE, FALSE);
                STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
                if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle))
                {
                    detail = "create_failed";
                    return StepResult::Failed;
                }
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(vehicle, TRUE, TRUE);
                g_spawned[step.args[0]] = vehicle;
                detail = "handle=" + std::to_string(vehicle);
                return StepResult::Done;
            }
            if (step.op == "mount" || step.op == "set_on_mount")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped horse = ResolvePed(step.args[1]);
                if (!ped || !horse)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                // The seat, -1 the saddle (default), 0 the passenger behind it:
                // winter2.c seats a rider with -1 and a second ped on the same
                // horse with 0; beat_inbred_kidnap.c asks
                // _IS_MOUNT_SEAT_FREE(horse, 0) before TASK_MOUNT_ANIMAL with
                // seat 0.
                int seat = -1;
                if (step.args.size() > 2)
                    ArgInt(step, 2, seat);
                if (step.op == "mount")
                    TASK::TASK_MOUNT_ANIMAL(ped, horse, 20000, seat, 2.0f, 1, 0, 0);
                else
                    PED::SET_PED_ONTO_MOUNT(ped, horse, seat, TRUE);
                detail = "seat=" + std::to_string(seat) + ",on_mount=" + std::to_string(PED::IS_PED_ON_MOUNT(ped) ? 1 : 0) +
                    ",mount_is_horse=" + std::to_string(PED::GET_MOUNT(ped) == horse ? 1 : 0) +
                    ",saddle_free=" + std::to_string(PED::_IS_MOUNT_SEAT_FREE(horse, -1) ? 1 : 0) +
                    ",passenger_free=" + std::to_string(PED::_IS_MOUNT_SEAT_FREE(horse, 0) ? 1 : 0);
                return StepResult::Done;
            }
            if (step.op == "enter_vehicle" || step.op == "into_vehicle" || step.op == "drive_to")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Entity vehicle = ResolveSpawned(step.args[1]);
                if (!ped || !vehicle || !ENTITY::IS_ENTITY_A_VEHICLE(vehicle))
                {
                    detail = "unknown_ped_or_vehicle";
                    return StepResult::Failed;
                }
                if (step.op == "drive_to")
                {
                    const Vector3 at = Coords(step, 2);
                    float speed = 5.0f;
                    ArgFloat(step, 5, speed);
                    // CART05 never moved with the coord task; the other modes
                    // are the vehicle tasks the natives list, one hypothesis
                    // each (flags 0: engine default).
                    const std::string mode = step.args.size() > 6 ? step.args[6] : "coord";
                    if (mode == "dest")
                        TASK::TASK_VEHICLE_DRIVE_TO_DESTINATION(ped, vehicle, at.x, at.y, at.z, speed, 0, 0, 5.0f, 5.0f, FALSE);
                    else if (mode == "straight")
                        TASK::TASK_VEHICLE_DRIVE_STRAIGHT_TO_POINT(ped, vehicle, at.x, at.y, at.z, speed, 5.0f, 0);
                    else if (mode == "any")
                        TASK::TASK_GO_TO_COORD_ANY_MEANS(ped, at.x, at.y, at.z, speed, 0, FALSE, 0, -1.0f);
                    else if (mode == "wander")
                        TASK::TASK_VEHICLE_DRIVE_WANDER(ped, vehicle, speed, 0);
                    else
                        TASK::TASK_VEHICLE_DRIVE_TO_COORD(ped, vehicle, at.x, at.y, at.z, speed, 0, 0, 67633207, 5.0f, 20.0f);
                    detail = "mode=" + mode + ",in_vehicle=" + (PED::IS_PED_IN_VEHICLE(ped, vehicle, FALSE) ? "1" : "0") +
                        ",driver=" + (VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, -1) == ped ? "1" : "0");
                    return StepResult::Done;
                }
                int seat = -1;
                ArgInt(step, 2, seat);
                if (step.op == "enter_vehicle")
                    TASK::TASK_ENTER_VEHICLE(ped, vehicle, 20000, seat, 1.0f, 1, 0);
                else
                    PED::SET_PED_INTO_VEHICLE(ped, vehicle, seat);
                return StepResult::Done;
            }
            if (step.op == "goto")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Vector3 at = Coords(step, 1);
                float speed = 1.0f;
                ArgFloat(step, 4, speed);
                TASK::TASK_GO_STRAIGHT_TO_COORD(ped, at.x, at.y, at.z, speed, -1, 40000.0f, 0.5f, 0);
                return StepResult::Done;
            }
            if (step.op == "hand_over")
            {
                // A manual scene: record, stop, play, stop by hand.
                int limit = 0;
                ArgInt(step, 0, limit);
                if (!g_run.stepPending)
                {
                    g_run.handOverPhase = 0;
                    replay::hud::ShowObjective(replay::keys::Fill("Bench: scene ready. {key_record} record, {key_record} stop, {key_stop} play, "
                                                                  "{key_stop} end.",
                        replay::keys::Current()).c_str());
                    Log() << "BENCH_HAND_OVER,limit_ms=" << limit << '\n';
                    Log().flush();
                }
                const bool recording = g_hooks->Recording();
                const bool playing = g_context->transport.Playing();
                int& phase = g_run.handOverPhase;
                if (phase == 0 && recording)
                    phase = 1;
                else if (phase == 1 && !recording)
                    phase = 2;
                else if (phase == 2 && playing)
                    phase = 3;
                else if (phase == 3 && !playing)
                {
                    detail = "cycle=1,after_ms=" + std::to_string(now - g_run.stepStartedAt);
                    return StepResult::Done;
                }
                if (now - g_run.stepStartedAt >= static_cast<std::uint64_t>(limit))
                {
                    detail = "cycle=0,phase=" + std::to_string(phase);
                    return StepResult::Done;
                }
                return StepResult::Pending;
            }
            if (step.op == "clip")
            {
                // The host reads this line and starts or stops an OBS
                // recording; the scenario waits for OBS to roll (start) or
                // to close the file (stop).
                const bool start = step.args[0] == "start";
                Log() << "BENCH_CLIP,action=" << step.args[0]
                      << ",tag=" << (step.args.size() > 1 ? step.args[1] : std::string("clip"))
                      << ",step=" << g_run.next << '\n';
                Log().flush();
                g_run.waitUntil = now + (start ? 1200u : 500u);
                return StepResult::Done;
            }
            if (step.op == "host_input")
            {
                // The host reads this line and sends the input with SendInput;
                // the scenario only waits.
                int ms = 0;
                ArgInt(step, 1, ms);
                Log() << "BENCH_HOST_INPUT,spec=" << step.args[0] << '\n';
                Log().flush();
                g_run.waitUntil = now + static_cast<std::uint64_t>(ms);
                return StepResult::Done;
            }
            if (step.op == "hold")
            {
                int ms = 0;
                ArgInt(step, 1, ms);
                g_held.push_back({MISC::GET_HASH_KEY(step.args[0].c_str()), now + static_cast<std::uint64_t>(ms)});
                return StepResult::Done;
            }
            if (step.op == "aim_at")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                    TASK::TASK_TURN_PED_TO_FACE_ENTITY(ped, target, 1000, 0.0f, 0.0f, 0.0f);
                // The player aims where the gameplay camera looks, even while a
                // scripted camera renders: keep it straight behind the player.
                if (ped == player)
                {
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f, 1.0f);
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(0.0f, 1.0f);
                }
                return now - g_run.stepStartedAt < 1200 ? StepResult::Pending : StepResult::Done;
            }
            if (step.op == "rope_probe" || step.op == "rope_probe2")
            {
                if (!g_run.stepPending)
                {
                    int type = 0;
                    float length = 0.0f;
                    int segments = 0;
                    ArgInt(step, 0, type);
                    ArgFloat(step, 1, length);
                    ArgInt(step, 2, segments);
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                    const Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(player);
                    ProbeRope rope;
                    ArgFloat(step, 3, rope.force);
                    rope.from = {at.x + fwd.x * 2.0f, at.y + fwd.y * 2.0f, at.z + 0.8f};
                    rope.to = {rope.from.x + fwd.x * length * 0.8f, rope.from.y + fwd.y * length * 0.8f, rope.from.z};
                    rope.sag = length * 0.15f;
                    // Arguments as the game's own scripts create a rope (act_bankrobbery01).
                    // Arguments as the game's own scripts pass them; _ADD_ROPE_2 with
                    // a type 0 rope and a small int (6, 10) in its 10th slot, never networked here.
                    rope.id = step.op == "rope_probe2"
                        ? PHYSICS::_ADD_ROPE_2(rope.from.x, rope.from.y, rope.from.z, 0.0f, 0.0f, 0.0f, length,
                              type, FALSE, segments, -1.0f)
                        : PHYSICS::ADD_ROPE(rope.from.x, rope.from.y, rope.from.z, 0.0f, 0.0f, 0.0f, length,
                              type, length, 0.5f, 0.5f, FALSE, TRUE, FALSE, 1.0f, FALSE, nullptr, TRUE);
                    if (!PHYSICS::DOES_ROPE_EXIST(rope.id))
                    {
                        detail = "add_rope_failed,returned=" + std::to_string(rope.id);
                        return StepResult::Failed;
                    }
                    g_probeRopes.push_back(rope);
                    return StepResult::Pending;
                }
                // One frame later the rope is in the manager: report what the engine made of it.
                ProbeRope& rope = g_probeRopes.back();
                std::ostringstream text;
                text << "rope=" << rope.id << ",vertex_count=" << PHYSICS::GET_ROPE_VERTEX_COUNT(rope.id)
                     << ",forced_len=" << PHYSICS::_ROPE_GET_FORCED_LENGTH(rope.id);
                static std::vector<rope::RopeInfo> ropes;
                std::uint32_t managerCount = 0;
                if (rope::Snapshot(ropes, managerCount))
                {
                    for (const rope::RopeInfo& info : ropes)
                    {
                        if (static_cast<int>(info.id) != rope.id)
                            continue;
                        text << ",engine_type=" << info.type << ",engine_vertices=" << info.vertices
                             << ",draw_start=" << info.drawStart << ",draw_unused=" << info.drawUnused
                             << ",engine_length=" << info.length << ",mode=" << info.mode
                             << ",flags=0x" << std::hex << info.flags << std::dec
                             << ",attachments=" << info.attachmentCount;
                    }
                    text << ",manager_ropes=" << managerCount;
                }
                if (rope.force > 0.0f && rope.phase == 0)
                {
                    // Evidence: does forcing the length change the script
                    // vertex count? The state before, then the call; the
                    // step reports the state one frame after.
                    Log() << "BENCH_ROPE_FORCE,phase=before,force=" << rope.force << ',' << text.str() << '\n';
                    Log().flush();
                    PHYSICS::ROPE_FORCE_LENGTH(rope.id, rope.force);
                    rope.phase = 1;
                    return StepResult::Pending;
                }
                if (rope.force > 0.0f)
                    text << ",forced=" << rope.force;
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "rope_census")
            {
                static std::vector<rope::RopeInfo> ropes;
                std::uint32_t managerCount = 0;
                if (!rope::Snapshot(ropes, managerCount))
                {
                    detail = "snapshot_failed";
                    return StepResult::Done;
                }
                std::ostringstream text;
                text << std::fixed << std::setprecision(1) << "manager=" << managerCount << ",ropes=";
                for (const rope::RopeInfo& info : ropes)
                    text << info.id << ":t" << info.type << ":v" << info.vertices << ":a" << info.attachmentCount
                         << "@" << info.first[0] << "/" << info.first[1] << ";";
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "lasso")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                {
                    TASK::TASK_LASSO_PED(ped, target);
                    return StepResult::Pending;
                }
                const bool caught = PED::IS_PED_LASSOED(target) != FALSE;
                if (!caught && now - g_run.stepStartedAt < 6000)
                    return StepResult::Pending;
                // A miss is evidence too: the run goes on and the line says so.
                detail = std::string("caught=") + (caught ? "1" : "0") +
                    ",after_ms=" + std::to_string(now - g_run.stepStartedAt);
                return StepResult::Done;
            }
            if (step.op == "timescale")
            {
                float scale = 1.0f;
                ArgFloat(step, 0, scale);
                MISC::SET_TIME_SCALE(scale);
                return StepResult::Done;
            }
            if (step.op == "kill_player")
            {
                PLAYER::SET_PLAYER_INVINCIBLE(PLAYER::PLAYER_ID(), FALSE);
                ENTITY::SET_ENTITY_INVINCIBLE(player, FALSE);
                ENTITY::SET_ENTITY_HEALTH(player, 0, 0);
                return StepResult::Done;
            }
            if (step.op == "health")
            {
                const Ped ped = ResolvePed(step.args[0]);
                int health = 0;
                ArgInt(step, 1, health);
                if (!ped || ped == player)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                ENTITY::SET_ENTITY_MAX_HEALTH(ped, health);
                ENTITY::SET_ENTITY_HEALTH(ped, health, 0);
                return StepResult::Done;
            }
            // Hogtie, carry and track ops (natives only).
            if (step.op == "knockout")
            {
                const Ped ped = ResolvePed(step.args[0]);
                int ms = 0;
                ArgInt(step, 1, ms);
                if (!ped || ped == player)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                TASK::TASK_KNOCKED_OUT(ped, 0.0f, FALSE);
                TASK::_TASK_KNOCKED_OUT_SET_DURATION(ped, static_cast<float>(ms) / 1000.0f);
                detail = std::string("ragdoll=") + (PED::IS_PED_RAGDOLL(ped) ? "1" : "0");
                return StepResult::Done;
            }
            if (step.op == "hogtie")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                {
                    TASK::TASK_HOGTIE_TARGET_PED(ped, target);
                    return StepResult::Pending;
                }
                const bool tied = PED::IS_PED_HOGTIED(target) != FALSE;
                if (!tied && now - g_run.stepStartedAt < 12000u)
                    return StepResult::Pending;
                detail = std::string("hogtied=") + (tied ? "1" : "0") + ",being=" +
                    (PED::IS_PED_BEING_HOGTIED(target) ? "1" : "0") + ",after_ms=" +
                    std::to_string(now - g_run.stepStartedAt);
                return StepResult::Done;
            }
            if (step.op == "place_on_mount" || step.op == "place_at")
            {
                const bool mount = step.op == "place_on_mount";
                const Ped ped = ResolvePed(step.args[0]);
                const Entity target = ResolveSpawned(step.args[1]);
                const Ped horse = mount ? ResolvePed(step.args[2]) : 0;
                if (!ped || !target || (mount && !horse))
                {
                    detail = "unknown_entity";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                {
                    if (mount)
                        TASK::TASK_PLACE_CARRIED_ENTITY_ON_MOUNT(ped, target, horse, 1.0f);
                    else
                    {
                        float x = 0, y = 0, z = 0;
                        ArgFloat(step, 2, x);
                        ArgFloat(step, 3, y);
                        ArgFloat(step, 4, z);
                        TASK::TASK_PLACE_CARRIED_ENTITY_AT_COORD(ped, target, x, y, z, 1.0f, 0);
                    }
                    return StepResult::Pending;
                }
                const int state = ENTITY::GET_CARRIABLE_ENTITY_STATE(target);
                const bool done = mount ? state == 7 : (state == 3 || state == 0);
                if (!done && now - g_run.stepStartedAt < (mount ? 12000u : 10000u))
                    return StepResult::Pending;
                detail = "state=" + std::to_string(state) + ",mount=" +
                    std::to_string(PED::_GET_CARRIER_AS_MOUNT(target)) + ",after_ms=" +
                    std::to_string(now - g_run.stepStartedAt);
                return StepResult::Done;
            }
            if (step.op == "carry_state")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                std::ostringstream text;
                text << "hogtied=" << (PED::IS_PED_HOGTIED(ped) ? 1 : 0)
                     << ",being_hogtied=" << (PED::IS_PED_BEING_HOGTIED(ped) ? 1 : 0)
                     << ",lassoed=" << (PED::IS_PED_LASSOED(ped) ? 1 : 0)
                     << ",carriable_state=" << ENTITY::GET_CARRIABLE_ENTITY_STATE(ped)
                     << ",carrier_ped=" << PED::_GET_CARRIER_AS_PED(ped)
                     << ",carrier_mount=" << PED::_GET_CARRIER_AS_MOUNT(ped)
                     << ",attached_to=" << ENTITY::GET_ENTITY_ATTACHED_TO(ped)
                     << ",carrying=" << (PED::IS_PED_CARRYING_SOMETHING(ped) ? 1 : 0)
                     << ",first_carried=" << PED::_GET_FIRST_ENTITY_PED_IS_CARRYING(ped)
                     << ",components=" << PED::_GET_NUM_COMPONENTS_IN_PED(ped)
                     ;
                Log() << "BENCH_CARRY_STATE,who=" << step.args[0] << "," << text.str() << '\n';
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "door_census")
            {
                // Objects near the player whose model is a door (by name
                // of the models the door table uses is not known here: any
                // object the door system knows by one of the listed hashes,
                // plus every object in the pool near the player for models).
                float radius = 40.0f;
                ArgFloat(step, 0, radius);
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                static int objects[4096];
                const int count = worldGetAllObjects(objects, 4096);
                int nearCount = 0;
                std::ostringstream models;
                for (int n = 0; n < count; ++n)
                {
                    const Object object = objects[n];
                    if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
                    if (dx * dx + dy * dy + dz * dz > radius * radius)
                        continue;
                    ++nearCount;
                    const Vector3 r = ENTITY::GET_ENTITY_ROTATION(object, 2);
                    Log() << "BENCH_DOOR_OBJECT,handle=" << object << ",model=" << ENTITY::GET_ENTITY_MODEL(object)
                          << ",pos=" << p.x << ";" << p.y << ";" << p.z << ",rot_z=" << r.z
                          << ",frozen=" << (ENTITY::_IS_ENTITY_FROZEN(object) ? 1 : 0)
                          << ",static=" << (ENTITY::IS_ENTITY_STATIC(object) ? 1 : 0) << '\n';
                }
                int byHash = 0;
                for (std::size_t a = 1; a < step.args.size(); ++a)
                {
                    const Hash hash = static_cast<Hash>(std::stoul(step.args[a]));
                    const Entity door = ENTITY::_GET_ENTITY_BY_DOORHASH(hash, 0);
                    bool inPool = false;
                    for (int n = 0; n < count && door; ++n)
                        inPool = inPool || objects[n] == door;
                    byHash += door ? 1 : 0;
                    const Vector3 r = door ? ENTITY::GET_ENTITY_ROTATION(door, 2) : Vector3{};
                    Log() << "BENCH_DOOR,hash=" << hash << ",by_hash_handle=" << door << ",in_pool=" << (inPool ? 1 : 0)
                          << ",model=" << (door ? ENTITY::GET_ENTITY_MODEL(door) : 0)
                          << ",registered=" << (OBJECT::IS_DOOR_REGISTERED_WITH_SYSTEM(hash) ? 1 : 0)
                          << ",state=" << OBJECT::DOOR_SYSTEM_GET_DOOR_STATE(hash)
                          << ",ratio=" << OBJECT::DOOR_SYSTEM_GET_OPEN_RATIO(hash) << ",rot_z=" << r.z
                          << ",frozen=" << (door && ENTITY::_IS_ENTITY_FROZEN(door) ? 1 : 0) << '\n';
                }
                detail = "near_objects=" + std::to_string(nearCount) + ",by_hash=" + std::to_string(byHash);
                return StepResult::Done;
            }
            if (step.op == "ipl")
            {
                const Hash hash = IplHashArg(step.args[0]);
                const bool before = STREAMING::IS_IPL_ACTIVE_HASH(hash) != FALSE;
                if (step.args[1] == "on" || (step.args[1] == "toggle" && !before))
                    STREAMING::REQUEST_IPL_HASH(hash);
                else if (step.args[1] == "off" || (step.args[1] == "toggle" && before))
                    STREAMING::REMOVE_IPL_HASH(hash);
                const bool after = STREAMING::IS_IPL_ACTIVE_HASH(hash) != FALSE;
                Vector3 center{};
                float radius = 0.0f;
                const bool sphere = STREAMING::_GET_IPL_BOUNDING_SPHERE(hash, &center, &radius) != FALSE;
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                const float dx = center.x - at.x, dy = center.y - at.y, dz = center.z - at.z;
                std::ostringstream text;
                text << "hash=" << hash << ",name=" << step.args[0] << ",action=" << step.args[1] << ",before=" << (before ? 1 : 0)
                     << ",after=" << (after ? 1 : 0) << ",sphere=" << (sphere ? 1 : 0) << ",center=" << center.x << ";"
                     << center.y << ";" << center.z << ",radius=" << radius
                     << ",player_dist=" << std::sqrt(dx * dx + dy * dy + dz * dz);
                Log() << "BENCH_IPL," << text.str() << '\n';
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "ipl_watch")
            {
                IplWatch watch;
                watch.hash = IplHashArg(step.args[0]);
                int ms = 0;
                ArgInt(step, 1, ms);
                watch.started = now;
                watch.until = now + static_cast<std::uint64_t>(ms);
                g_iplWatches.push_back(watch);
                return StepResult::Done;
            }
            if (step.op == "world_probe")
            {
                replay::world::Probe(Log());
                return StepResult::Done;
            }
            if (step.op == "door_watch")
            {
                DoorWatch watch;
                watch.hash = static_cast<Hash>(std::stoul(step.args[0]));
                int ms = 0;
                ArgInt(step, 1, ms);
                watch.started = now;
                watch.until = now + static_cast<std::uint64_t>(ms);
                g_doorWatches.push_back(watch);
                return StepResult::Done;
            }
            if (step.op == "through_door" || step.op == "door_cam")
            {
                const bool cam = step.op == "door_cam";
                const Hash hash = static_cast<Hash>(std::stoul(step.args[cam ? 0 : 1]));
                const Entity door = ENTITY::_GET_ENTITY_BY_DOORHASH(hash, 0);
                if (!door || !ENTITY::DOES_ENTITY_EXIST(door))
                {
                    detail = "door_not_found";
                    return StepResult::Failed;
                }
                const Vector3 d = ENTITY::GET_ENTITY_COORDS(door, TRUE, FALSE);
                const Vector3 normal = ENTITY::GET_ENTITY_FORWARD_VECTOR(door);
                if (cam)
                {
                    float side = 1, dist = 5, height = 1.6f, fov = 50;
                    ArgFloat(step, 1, side);
                    ArgFloat(step, 2, dist);
                    ArgFloat(step, 3, height);
                    ArgFloat(step, 4, fov);
                    const Vector3 at{d.x + normal.x * side * dist, d.y + normal.y * side * dist, d.z + height};
                    const Vector3 look{d.x, d.y, d.z + 1.2f};
                    if (!StartCam(at, look, fov))
                    {
                        detail = "cam_failed";
                        return StepResult::Failed;
                    }
                    return StepResult::Done;
                }
                const Ped ped = ResolvePed(step.args[0]);
                float speed = 0, dist = 3;
                ArgFloat(step, 2, speed);
                ArgFloat(step, 3, dist);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                Vector3 from{d.x + normal.x * dist, d.y + normal.y * dist, d.z + 2.0f};
                float ground = from.z;
                if (MISC::GET_GROUND_Z_FOR_3D_COORD(from.x, from.y, from.z, &ground, FALSE))
                    from.z = ground;
                ENTITY::SET_ENTITY_COORDS(ped, from.x, from.y, from.z, FALSE, FALSE, FALSE, FALSE);
                ENTITY::SET_ENTITY_HEADING(ped, MISC::GET_HEADING_FROM_VECTOR_2D(-normal.x, -normal.y));
                if (speed > 0.0f)
                    TASK::TASK_FOLLOW_NAV_MESH_TO_COORD(ped, d.x - normal.x * dist, d.y - normal.y * dist, d.z, speed,
                        -1, 0.25f, 0, 40000.0f);
                detail = "door=" + std::to_string(door);
                return StepResult::Done;
            }
            if (step.op == "dof_probe")
            {
                const std::string& mode = step.args[0];
                g_dofProbe = {};
                g_dofProbe.mode = mode == "params" ? 1 : mode == "hidof" ? 2 : mode == "focus" ? 3 : 0;
                for (std::size_t n = 1; n < step.args.size() && n <= 11; ++n)
                    ArgFloat(step, n, g_dofProbe.values[n - 1]);
                if (!g_dofProbe.mode)
                    GRAPHICS::SET_HIDOF_OVERRIDE(FALSE, FALSE, 0.0f, 0.0f, 0.0f, 0.0f);
                detail = "mode=" + mode + ",cam=" + std::to_string(g_topCam);
                return StepResult::Done;
            }
            if (step.op == "dead_eye")
            {
                // The player's Dead Eye on (the game drains it).
                PLAYER::_SPECIAL_ABILITY_SET_ACTIVATE(PLAYER::PLAYER_ID());
                detail = std::string("active=") + (PLAYER::_IS_SPECIAL_ABILITY_ACTIVE(PLAYER::PLAYER_ID()) ? "1" : "0");
                return StepResult::Done;
            }
            if (step.op == "trail_reset")
            {
                // The global track map clear (snow, mud); the same call the
                // playback makes at its start and on a jump back.
                GRAPHICS::_0xEC3F7F24EEEB3BA3();
                return StepResult::Done;
            }
            if (step.op == "carry")
            {
                // The carrier picks the entity up (a body, a dead animal);
                // the step waits until it is carried (state 5) or 10 s.
                const Ped ped = ResolvePed(step.args[0]);
                const Entity target = ResolveSpawned(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_entity";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                {
                    TASK::TASK_PICKUP_CARRIABLE_ENTITY(ped, target);
                    return StepResult::Pending;
                }
                const int state = ENTITY::GET_CARRIABLE_ENTITY_STATE(target);
                if (state != 5 && now - g_run.stepStartedAt < 10000)
                    return StepResult::Pending;
                detail = "state=" + std::to_string(state) + ",after_ms=" + std::to_string(now - g_run.stepStartedAt);
                return StepResult::Done;
            }
            if (step.op == "gore")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const auto mask = static_cast<std::uint16_t>(std::strtoul(step.args[1].c_str(), nullptr, 16));
                bool handled = false;
                std::string error;
                const bool ok = replay::gore::FireEvent(getScriptHandleBaseAddress(ped),
                    getScriptHandleBaseAddress(PLAYER::PLAYER_PED_ID()), mask, handled, error);
                std::ostringstream text;
                text << "mask=0x" << std::hex << mask << std::dec << ",called=" << ok << ",handled=" << handled
                     << ",error=" << (error.empty() ? "none" : error.c_str());
                detail = text.str();
                Log() << "BENCH_GORE,id=" << step.args[0] << "," << detail << '\n';
                return ok ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "kill")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped || ped == player)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                ENTITY::SET_ENTITY_HEALTH(ped, 0, 0);
                return StepResult::Done;
            }
            if (step.op == "skeleton")
            {
                const Ped ped = ResolvePed(step.args[0]);
                std::ostringstream text;
                text << "id=" << step.args[0];
                replay::skeleton::ResolvedPoseSkeleton resolved{};
                if (ped && replay::skeleton::FastResolvePedPoseSkeleton(ped, resolved))
                {
                    text << ",bones=" << resolved.boneCount << ",skeleton=0x" << std::hex
                         << reinterpret_cast<std::uintptr_t>(resolved.skeleton) << std::dec
                         << ",dead=" << (ENTITY::IS_ENTITY_DEAD(ped) ? 1 : 0)
                         << ",ragdoll=" << (PED::IS_PED_RAGDOLL(ped) ? 1 : 0) << ",gore=";
                    for (const int limb : {3, 4, 6, 7, 9, 10, 12, 13, 37})
                        if (PED::_HAS_PED_TAKEN_GORE_DAMAGE(ped, limb))
                            text << limb << ";";
                }
                else
                    text << ",resolved=0";
                detail = text.str();
                Log() << "BENCH_SKELETON," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "knock_hat")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                PED::KNOCK_OFF_PED_PROP(ped, FALSE, TRUE, FALSE, TRUE);
                return StepResult::Done;
            }
            if (step.op == "horse_weapon")
            {
                const Ped horse = ResolvePed(step.args[0]);
                const Ped owner = ResolvePed(step.args[1]);
                if (!horse || !owner)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash weapon = MISC::GET_HASH_KEY(step.args[2].c_str());
                WEAPON::GIVE_WEAPON_TO_PED(owner, weapon, 100, TRUE, FALSE, 0, FALSE, 0.5f, 1.0f,
                    kAddReasonDefault, TRUE, 0.0f, FALSE);
                WEAPON::_ATTACH_WEAPON_TO_HORSE_HOLSTER(horse, weapon, owner);
                char text[64]{};
                std::snprintf(text, sizeof(text), "holster=0x%X", WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(horse));
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "ped_place")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped || ped == player)
                {
                    detail = ped ? "player_use_teleport" : "unknown_ped";
                    return StepResult::Failed;
                }
                const Vector3 at = Coords(step, 1);
                ENTITY::SET_ENTITY_COORDS(ped, at.x, at.y, at.z, FALSE, FALSE, FALSE, TRUE);
                float heading = 0.0f;
                if (ArgFloat(step, 4, heading))
                    ENTITY::SET_ENTITY_HEADING(ped, heading);
                return StepResult::Done;
            }
            if (step.op == "weapon_state")
            {
                // What the PlayerWeaponBank restore must keep (player_weapon_bank_core.h
                // Loadout): the same reads as the mod's event=inventory, independent of it.
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash weapon = MISC::GET_HASH_KEY(step.args[1].c_str());
                Hash held = 0;
                WEAPON::GET_CURRENT_PED_WEAPON(ped, &held, FALSE, 0, FALSE);
                std::string attach;
                for (int ap = 2; ap < 29; ++ap)
                {
                    Hash at = 0;
                    WEAPON::GET_CURRENT_PED_WEAPON(ped, &at, FALSE, ap, FALSE);
                    if (!at)
                        continue;
                    char part[32]{};
                    std::snprintf(part, sizeof(part), "%s%d:0x%X", attach.empty() ? "" : ";", ap, static_cast<unsigned>(at));
                    attach += part;
                }
                const Ped horse = ResolvePed("player_horse");
                const Ped mount = PED::IS_PED_ON_MOUNT(ped) ? PED::GET_MOUNT(ped) : 0;
                char text[320]{};
                std::snprintf(text, sizeof(text),
                    "tag=%s,weapon=%s,weapon_hash=0x%X,held=0x%X,owned=%d,player_inventory=%d,carrying=%d,ammo=%d,mounted=%d,"
                    "horse=%d,horse_holster=0x%X,mount_holster=0x%X,attach=%s",
                    step.args[2].c_str(), step.args[1].c_str(), static_cast<unsigned>(weapon), static_cast<unsigned>(held),
                    WEAPON::HAS_PED_GOT_WEAPON(ped, weapon, 0, FALSE) ? 1 : 0,
                    WEAPON::HAS_PED_GOT_WEAPON(ped, weapon, 0, TRUE) ? 1 : 0,
                    WEAPON::IS_PED_CARRYING_WEAPON(ped, weapon) ? 1 : 0, WEAPON::GET_AMMO_IN_PED_WEAPON(ped, weapon),
                    mount ? 1 : 0, horse ? 1 : 0, horse ? static_cast<unsigned>(WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(horse)) : 0u,
                    mount ? static_cast<unsigned>(WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(mount)) : 0u,
                    attach.empty() ? "none" : attach.c_str());
                detail = text;
                Log() << "BENCH_WEAPON_STATE,id=" << step.args[0] << "," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "scenario")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                int ms = -1;
                if (step.args.size() > 2)
                    ArgInt(step, 2, ms);
                TASK::TASK_START_SCENARIO_IN_PLACE_HASH(ped, MISC::GET_HASH_KEY(step.args[1].c_str()), ms, TRUE, 0,
                    ENTITY::GET_ENTITY_HEADING(ped), FALSE);
                return StepResult::Done;
            }
            if (step.op == "item_interaction")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash item = step.args.size() > 2 && step.args[2] != "-" ? MISC::GET_HASH_KEY(step.args[2].c_str()) : 0;
                const Hash interaction = MISC::GET_HASH_KEY(step.args[1].c_str());
                const BOOL can = TASK::CAN_START_ITEM_INTERACTION(ped, item, interaction, 0);
                TASK::START_TASK_ITEM_INTERACTION(ped, item, interaction, 1, 0, -1.0f);
                detail = can ? "can_start=1" : "can_start=0";
                return StepResult::Done;
            }
            if (step.op == "attached_census")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                float radius = 2.0f;
                ArgFloat(step, 1, radius);
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE);
                static int objects[2048];
                const int count = (std::min)(worldGetAllObjects(objects, 2048), 2048);
                static std::set<int> censusSeen;
                int nearCount = 0;
                int attachedToPed = 0;
                for (int n = 0; n < count; ++n)
                {
                    const Object object = objects[n];
                    if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const Entity parent = ENTITY::GET_ENTITY_ATTACHED_TO(object);
                    // Objects new since the previous census are logged wherever they are.
                    const bool fresh = !censusSeen.empty() && !censusSeen.count(object);
                    if (distance > radius && parent != ped && !fresh)
                        continue;
                    if (fresh)
                        Log() << "BENCH_ATTACHED_CENSUS_NEW,handle=" << object << ",attached_to=" << parent << ",attached_model=0x" << std::hex
                              << (parent ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(parent)) : 0u) << std::dec
                              << ",pos=" << p.x << ";" << p.y << ";" << p.z << '\n';
                    ++nearCount;
                    if (parent == ped)
                        ++attachedToPed;
                    Log() << "BENCH_ATTACHED_CENSUS_OBJECT,of=" << step.args[0] << ",handle=" << object << ",model=0x" << std::hex
                          << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) << std::dec << ",distance=" << distance
                          << ",attached_to=" << parent << ",to_this=" << (parent == ped) << ",visible=" << (ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0)
                          << ",mission=" << (ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) ? 1 : 0) << '\n';
                }
                censusSeen.clear();
                censusSeen.insert(objects, objects + count);
                static ItemSet itemset = 0;
                if (!itemset || !ITEMSET::IS_ITEMSET_VALID(itemset))
                    itemset = ITEMSET::CREATE_ITEMSET(TRUE);
                int carriables = -1;
                if (itemset)
                {
                    ITEMSET::CLEAN_ITEMSET(itemset);
                    PED::FIND_ALL_ATTACHED_CARRIABLE_ENTITIES(ped, itemset);
                    carriables = ITEMSET::GET_ITEMSET_SIZE(itemset);
                    for (int n = 0; n < carriables && n < 16; ++n)
                    {
                        const Entity entity = MISC::_GET_ENTITY_FROM_ITEM(ITEMSET::GET_INDEXED_ITEM_IN_ITEMSET(n, itemset));
                        Log() << "BENCH_ATTACHED_CENSUS_CARRIABLE,of=" << step.args[0] << ",handle=" << entity << ",model=0x" << std::hex
                              << (entity ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(entity)) : 0u) << std::dec
                              << ",attached_to=" << (entity ? ENTITY::GET_ENTITY_ATTACHED_TO(entity) : 0) << '\n';
                    }
                }
                char text[96]{};
                std::snprintf(text, sizeof(text), "pool=%d,near=%d,attached_to_it=%d,carriables=%d", count, nearCount, attachedToPed, carriables);
                detail = text;
                return StepResult::Done;
            }
            // Loot: the ransack containers (drawers,
            // cabinets, chests) are scenario points attached to the container
            // object (RANSACK_ATTACHED_<MODEL> types in the game's scripts,
            // _GET_SCENARIO_POINT_ENTITY gives the container); the player uses
            // one as with the prompt: TASK_USE_SCENARIO_POINT. A loose item is
            // picked up with TASK_LOOT_ENTITY.
            if (step.op == "loot_points" || step.op == "loot_use")
            {
                float radius = 3.0f;
                ArgFloat(step, 0, radius);
                // The n-th container nearest first, or the nearest one of a
                // container model (0x...).
                int pick = 0;
                Hash pickModel = 0;
                if (step.args.size() > 1 && step.args[1].rfind("0x", 0) == 0)
                    pickModel = static_cast<Hash>(std::strtoul(step.args[1].c_str() + 2, nullptr, 16));
                else if (step.args.size() > 1)
                    ArgInt(step, 1, pick);
                int waitMs = 15000;
                if (step.args.size() > 2)
                    ArgInt(step, 2, waitMs);
                const bool use = step.op == "loot_use";
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                static std::int64_t buffer[1024];
                std::fill(std::begin(buffer), std::end(buffer), 0);
                const int count = (std::min)(TASK::GET_SCENARIO_POINTS_IN_AREA(at.x, at.y, at.z, radius,
                    reinterpret_cast<Any*>(buffer), 1000), 1000);
                struct Point
                {
                    int point = 0;
                    float distance = 0.0f;
                    Entity entity = 0;
                };
                std::vector<Point> points;
                for (int n = 0; n < count; ++n)
                {
                    const int point = static_cast<int>(buffer[n]);
                    if (!TASK::DOES_SCENARIO_POINT_EXIST(point))
                        continue;
                    const Vector3 p = TASK::_GET_SCENARIO_POINT_COORDS(point, TRUE);
                    const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
                    points.push_back({point, std::sqrt(dx * dx + dy * dy + dz * dz), TASK::_GET_SCENARIO_POINT_ENTITY(point)});
                }
                std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.distance < b.distance; });
                int index = 0;
                int chosen = -1;
                for (const Point& point : points)
                {
                    const Vector3 p = TASK::_GET_SCENARIO_POINT_COORDS(point.point, TRUE);
                    const bool entity = point.entity && ENTITY::DOES_ENTITY_EXIST(point.entity);
                    if (!entity)
                        continue; // a container point has its container entity
                    const bool looted = PERSISTENCE::_PERSISTENCE_IS_SCENARIO_MARKED_AS_LOOTED(point.point) != FALSE;
                    char line[360];
                    std::snprintf(line, sizeof(line),
                        "BENCH_LOOT_POINT,index=%d,point=%d,type=0x%x,pos=%.2f;%.2f;%.2f,heading=%.1f,distance_m=%.2f,entity=%d,"
                        "entity_model=0x%x,container=%d,loot_left=%d,looted=%d\n",
                        index, point.point, static_cast<unsigned>(TASK::_GET_SCENARIO_POINT_TYPE(point.point)), p.x, p.y, p.z,
                        TASK::_GET_SCENARIO_POINT_HEADING(point.point, TRUE), point.distance, entity ? point.entity : 0,
                        entity ? static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(point.entity)) : 0u,
                        entity ? TASK::_GET_RANSACK_SCENARIO_CONTAINER_NUM_COMPARTMENTS(point.entity) : -1,
                        entity ? TASK::_GET_RANSACK_SCENARIO_CONTAINER_REMAINING_LOOT_COUNT(point.entity) : -1, looted ? 1 : 0);
                    Log() << line;
                    if (entity && chosen < 0 &&
                        (pickModel ? static_cast<Hash>(ENTITY::GET_ENTITY_MODEL(point.entity)) == pickModel : index == pick))
                        chosen = point.point;
                    index += entity ? 1 : 0;
                }
                detail = "points=" + std::to_string(points.size()) + ",raw=" + std::to_string(count);
                if (!use)
                    return StepResult::Done;
                if (chosen < 0)
                {
                    detail += ",why=no_container_point";
                    return StepResult::Failed;
                }
                const Entity container = TASK::_GET_SCENARIO_POINT_ENTITY(chosen);
                g_lootTarget = container;
                g_lootUseNextLog = 0;
                TASK::TASK_USE_SCENARIO_POINT(player, chosen, "", -1, TRUE, FALSE, 0, FALSE, -1.0f, FALSE);
                g_lootUseSeen = false;
                std::ostringstream text;
                text << detail << ",point=" << chosen << ",type=0x" << std::hex
                     << static_cast<unsigned>(TASK::_GET_SCENARIO_POINT_TYPE(chosen)) << ",container=" << std::dec << container
                     << ",container_model=0x" << std::hex << static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(container)) << std::dec;
                g_lootUseDetail = text.str();
                detail = g_lootUseDetail;
                // The ransack goes on in the background (marks and waits run
                // meanwhile); BENCH_LOOT_USE_TICK lines tell its state.
                g_lootUseUntil = (std::max)(g_lootUseUntil, now + static_cast<std::uint64_t>(waitMs));
                g_lootUseLast.clear();
                Log() << "BENCH_LOOT_USE," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "loot_end")
            {
                // Out of the scenario as the player's exit does it.
                TASK::CLEAR_PED_TASKS(player, TRUE, FALSE);
                g_lootUseUntil = 0;
                return StepResult::Done;
            }
            if (step.op == "loot_entity")
            {
                float radius = 2.0f;
                ArgFloat(step, 0, radius);
                const Hash model = step.args.size() > 1 && step.args[1] != "any" ? IplHashArg(step.args[1]) : 0;
                int waitMs = 8000;
                if (step.args.size() > 2)
                    ArgInt(step, 2, waitMs);
                if (g_run.stepPending)
                {
                    const std::uint64_t elapsed = now - g_run.stepStartedAt;
                    const bool taken = !ENTITY::DOES_ENTITY_EXIST(g_lootTarget) || !ENTITY::IS_ENTITY_VISIBLE(g_lootTarget);
                    if (!taken && elapsed < static_cast<std::uint64_t>(waitMs))
                        return StepResult::Pending;
                    detail = g_lootUseDetail + ",taken=" + (taken ? "1" : "0") + ",ms=" + std::to_string(elapsed);
                    Log() << "BENCH_LOOT_ENTITY," << detail << '\n';
                    return StepResult::Done; // evidence: the take is read in the log
                }
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                static int objects[2048];
                const int listed = (std::min)(worldGetAllObjects(objects, 2048), 2048);
                Object best = 0;
                float bestM = radius;
                for (int n = 0; n < listed; ++n)
                {
                    const Object object = objects[n];
                    if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || ENTITY::IS_ENTITY_ATTACHED(object) ||
                        !ENTITY::IS_ENTITY_VISIBLE(object))
                        continue;
                    if (model && static_cast<Hash>(ENTITY::GET_ENTITY_MODEL(object)) != model)
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
                    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d < bestM)
                    {
                        bestM = d;
                        best = object;
                    }
                }
                if (!best)
                {
                    detail = "why=no_object";
                    return StepResult::Failed;
                }
                g_lootTarget = best;
                TASK::TASK_LOOT_ENTITY(player, best);
                std::ostringstream text;
                text << "object=" << best << ",model=0x" << std::hex << static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(best))
                     << std::dec << ",distance_m=" << bestM;
                g_lootUseDetail = text.str();
                return StepResult::Pending;
            }
            if (step.op == "loot_watch")
            {
                if (step.args[0] == "off")
                {
                    g_lootWatchUntil = 0;
                    return StepResult::Done;
                }
                float radius = 4.0f;
                ArgFloat(step, 0, radius);
                int ms = 60000;
                if (step.args.size() > 1)
                    ArgInt(step, 1, ms);
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                g_lootWatchCenter[0] = at.x;
                g_lootWatchCenter[1] = at.y;
                g_lootWatchCenter[2] = at.z;
                g_lootWatchRadius = radius;
                g_lootWatchUntil = now + static_cast<std::uint64_t>(ms);
                replay::objects::BenchLootWatchReset();
                // The player's scenario state too (BENCH_LOOT_USE_TICK).
                g_lootUseUntil = g_lootWatchUntil;
                g_lootUseStarted = now;
                g_lootUseLast.clear();
                g_lootTarget = 0;
                return StepResult::Done;
            }
            if (step.op == "item_prop")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const BOOL running = TASK::IS_PED_RUNNING_TASK_ITEM_INTERACTION(ped);
                const Entity first = TASK::_GET_ITEM_INTERACTION_ENTITY_FROM_PED(ped, 0);
                const Entity primary = TASK::_GET_ITEM_INTERACTION_ENTITY_FROM_PED(ped, MISC::GET_HASH_KEY("PRIMARYITEM"));
                static int objects[2048];
                const int count = (std::min)(worldGetAllObjects(objects, 2048), 2048);
                bool listed = false;
                for (int n = 0; n < count; ++n)
                    if (first && objects[n] == first)
                        listed = true;
                char text[256]{};
                std::snprintf(text, sizeof(text), "running=%d,first=%d,primary=%d,exists=%d,object=%d,model=0x%X,attached_to=%d,listed=%d,pool=%d,item_id=0x%X,state=0x%X",
                    running ? 1 : 0, first, primary, first && ENTITY::DOES_ENTITY_EXIST(first) ? 1 : 0,
                    first && ENTITY::IS_ENTITY_AN_OBJECT(first) ? 1 : 0, first ? static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(first)) : 0u,
                    first ? ENTITY::GET_ENTITY_ATTACHED_TO(first) : 0, listed ? 1 : 0, count,
                    static_cast<unsigned>(TASK::GET_ITEM_INTERACTION_ITEM_ID(ped)), static_cast<unsigned>(TASK::GET_ITEM_INTERACTION_STATE(ped)));
                detail = text;
                return StepResult::Done;
            }
            // A pickup on the ground (as a dead NPC's weapon or a mission's),
            // as the game's scripts create one (CREATE_PICKUP with flags 8|16,
            // -1, true: armadillo's revolver). The detail says whether its
            // object is there yet and how the recorder sees it (pickup natives,
            // mission entity).
            if (step.op == "pickup_spawn")
            {
                const Vector3 at = Coords(step, 2);
                if (const auto old = g_benchPickups.find(step.args[0]); old != g_benchPickups.end())
                {
                    if (old->second && OBJECT::DOES_PICKUP_EXIST(old->second))
                        OBJECT::REMOVE_PICKUP(old->second);
                    g_benchPickups.erase(old);
                }
                const int pickup = OBJECT::CREATE_PICKUP(MISC::GET_HASH_KEY(step.args[1].c_str()), at.x, at.y, at.z, 8 | 16, -1,
                    TRUE, 0, 0, 0.0f, 0);
                if (!pickup)
                {
                    detail = "create_failed";
                    return StepResult::Failed;
                }
                g_benchPickups[step.args[0]] = pickup;
                const Object object = OBJECT::GET_PICKUP_OBJECT(pickup);
                const bool exists = object && ENTITY::DOES_ENTITY_EXIST(object);
                char text[200]{};
                std::snprintf(text, sizeof(text), "pickup=%d,object=%d,exists=%d,model=0x%X,pickup_object=%d,portable=%d,mission=%d",
                    pickup, object, exists ? 1 : 0, exists ? static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(object)) : 0u,
                    exists && OBJECT::DOES_PICKUP_OBJECT_EXIST(object) ? 1 : 0, exists && OBJECT::IS_OBJECT_A_PORTABLE_PICKUP(object) ? 1 : 0,
                    exists && ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) ? 1 : 0);
                detail = text;
                return StepResult::Done;
            }
            // A prop set as player_camp creates the camp's tent and bed
            // (player_camp.c 866: placement 1, the heading, z probe 1.5, false,
            // false); its objects come from the prop set spawner with owner 23
            // (object_check.h kOwnerPropSet).
            if (step.op == "propset_spawn")
            {
                const Hash type = MISC::GET_HASH_KEY(step.args[1].c_str());
                PROPSET::_REQUEST_PROP_SET(type);
                if (!PROPSET::_HAS_PROP_SET_LOADED(type))
                {
                    if (now - g_run.stepStartedAt > kModelTimeoutMs)
                    {
                        detail = "propset_not_loaded";
                        return StepResult::Failed;
                    }
                    return StepResult::Pending;
                }
                const Vector3 at = Coords(step, 2);
                float heading = 0.0f;
                ArgFloat(step, 5, heading);
                if (const auto old = g_benchPropSets.find(step.args[0]); old != g_benchPropSets.end())
                {
                    if (old->second && PROPSET::DOES_PROP_SET_EXIST(old->second))
                        PROPSET::_DELETE_PROP_SET(old->second, TRUE, TRUE);
                    g_benchPropSets.erase(old);
                }
                // The request stays (whether _SET_PROP_SET_AS_NO_LONGER_NEEDED
                // takes the type or the instance is not proven).
                const int propSet = PROPSET::_CREATE_PROP_SET(type, at.x, at.y, at.z, 1, heading, 1.5f, FALSE, FALSE);
                if (!propSet || !PROPSET::DOES_PROP_SET_EXIST(propSet))
                {
                    detail = "create_failed";
                    return StepResult::Failed;
                }
                g_benchPropSets[step.args[0]] = propSet;
                detail = "propset=" + std::to_string(propSet);
                return StepResult::Done;
            }
            if (step.op == "propset_census")
            {
                const auto found = g_benchPropSets.find(step.args[0]);
                if (found == g_benchPropSets.end() || !PROPSET::DOES_PROP_SET_EXIST(found->second))
                {
                    detail = "unknown_propset";
                    return StepResult::Failed;
                }
                const ItemSet items = ITEMSET::CREATE_ITEMSET(TRUE);
                int count = items ? PROPSET::_GET_ENTITIES_FROM_PROP_SET(found->second, items, 0, FALSE, FALSE) : -1;
                std::map<std::uint32_t, int> owners;
                for (int n = 0; items && n < count && n < 64; ++n)
                {
                    const Object object = MISC::_GET_OBJECT_FROM_INDEXED_ITEM(ITEMSET::GET_INDEXED_ITEM_IN_ITEMSET(n, items));
                    if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
                        continue;
                    std::uint32_t field = 0;
                    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(object));
                    const std::uint32_t owner = address && replay::mem::Read(address + replay::objcheck::kOwnerFieldOffset, field)
                        ? replay::objcheck::OwnerOf(field) : replay::objcheck::kOwnerUnknown;
                    ++owners[owner];
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    Log() << "BENCH_PROPSET_OBJECT,of=" << step.args[0] << ",handle=" << object << ",model=0x" << std::hex
                          << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) << std::dec << ",owned_by=" << owner
                          << ",mission=" << (ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) ? 1 : 0)
                          << ",visible=" << (ENTITY::IS_ENTITY_VISIBLE(object) ? 1 : 0)
                          << ",attached_to=" << ENTITY::GET_ENTITY_ATTACHED_TO(object) << ",pos=" << p.x << ";" << p.y << ";"
                          << p.z << '\n';
                }
                if (items)
                    ITEMSET::DESTROY_ITEMSET(items);
                std::string text = "loaded=" + std::to_string(PROPSET::IS_PROP_SET_FULLY_LOADED(found->second) ? 1 : 0) +
                    ",objects=" + std::to_string(count) + ",owners=";
                for (const auto& [owner, n] : owners)
                    text += std::to_string(owner) + ":" + std::to_string(n) + "|";
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "propset_delete")
            {
                const auto found = g_benchPropSets.find(step.args[0]);
                if (found == g_benchPropSets.end())
                {
                    detail = "unknown_propset";
                    return StepResult::Failed;
                }
                const bool existed = found->second && PROPSET::DOES_PROP_SET_EXIST(found->second);
                if (existed)
                    PROPSET::_DELETE_PROP_SET(found->second, TRUE, TRUE);
                g_benchPropSets.erase(found);
                detail = std::string("deleted=") + (existed ? "1" : "0");
                return StepResult::Done;
            }
            if (step.op == "pickup_remove")
            {
                const auto found = g_benchPickups.find(step.args[0]);
                if (found == g_benchPickups.end())
                {
                    detail = "unknown_pickup";
                    return StepResult::Failed;
                }
                const Object object = OBJECT::GET_PICKUP_OBJECT(found->second);
                char text[96]{};
                std::snprintf(text, sizeof(text), "existed=%d,object=%d", OBJECT::DOES_PICKUP_EXIST(found->second) ? 1 : 0, object);
                if (OBJECT::DOES_PICKUP_EXIST(found->second))
                    OBJECT::REMOVE_PICKUP(found->second);
                g_benchPickups.erase(found);
                detail = text;
                return StepResult::Done;
            }
            // The engine hides a shelf's loot at the take and keeps the object;
            // here the same on a free world object that no script owns (MODEL,
            // or any with nearest): pick chooses the nearest within the radius
            // and puts the player 1.2 m from it on the ground (within reach for
            // the recorder's loot watch), off hides it, on shows it back.
            if (step.op == "world_visible")
            {
                if (step.args[2] == "on")
                {
                    const bool had = g_worldHidden && ENTITY::DOES_ENTITY_EXIST(g_worldHidden);
                    if (had)
                        ENTITY::SET_ENTITY_VISIBLE(g_worldHidden, TRUE);
                    detail = had ? "shown=" + std::to_string(g_worldHidden) : "nothing_hidden";
                    g_worldHidden = 0;
                    g_worldPicked = 0;
                    return StepResult::Done;
                }
                const Ped playerPed = PLAYER::PLAYER_PED_ID();
                const Vector3 from = ENTITY::GET_ENTITY_COORDS(playerPed, TRUE, FALSE);
                Object best = step.args[2] == "off" && g_worldPicked && ENTITY::DOES_ENTITY_EXIST(g_worldPicked) ? g_worldPicked : 0;
                float bestDistance = 0.0f;
                if (!best)
                {
                    float radius = 2.0f;
                    ArgFloat(step, 1, radius);
                    bestDistance = radius;
                    const bool any = step.args[0] == "nearest";
                    const Hash wanted = any ? 0 : MISC::GET_HASH_KEY(step.args[0].c_str());
                    static int objects[2048];
                    const int count = (std::min)(worldGetAllObjects(objects, 2048), 2048);
                    for (int n = 0; n < count; ++n)
                    {
                        const Object object = objects[n];
                        if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || ENTITY::IS_ENTITY_ATTACHED(object) ||
                            ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) || !ENTITY::IS_ENTITY_VISIBLE(object) ||
                            OBJECT::DOES_PICKUP_OBJECT_EXIST(object))
                            continue;
                        if (!any && static_cast<Hash>(ENTITY::GET_ENTITY_MODEL(object)) != wanted)
                            continue;
                        // No script's or temporary object (those are scenery already).
                        std::uint32_t field = 0;
                        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(object));
                        if (!address || !replay::mem::Read(address + replay::objcheck::kOwnerFieldOffset, field) ||
                            replay::objcheck::IsSceneryOwner(false, replay::objcheck::OwnerOf(field)))
                            continue;
                        const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                        const float dx = p.x - from.x, dy = p.y - from.y, dz = p.z - from.z;
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            best = object;
                        }
                    }
                }
                if (!best)
                {
                    detail = "none_in_radius";
                    return StepResult::Failed;
                }
                std::uint32_t field = 0;
                const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(best));
                const std::uint32_t owner = address && replay::mem::Read(address + replay::objcheck::kOwnerFieldOffset, field)
                    ? replay::objcheck::OwnerOf(field) : replay::objcheck::kOwnerUnknown;
                const Vector3 p = ENTITY::GET_ENTITY_COORDS(best, TRUE, FALSE);
                if (step.args[2] == "pick")
                {
                    // 1.2 m from it on the player's side, on the ground.
                    float dx = from.x - p.x, dy = from.y - p.y;
                    const float flat = std::sqrt(dx * dx + dy * dy);
                    dx = flat > 0.01f ? dx / flat : 1.0f;
                    dy = flat > 0.01f ? dy / flat : 0.0f;
                    const float x = p.x + dx * 1.2f, y = p.y + dy * 1.2f;
                    float z = from.z;
                    float ground = 0.0f;
                    if (MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, (std::max)(from.z, p.z) + 2.0f, &ground, FALSE))
                        z = ground;
                    ENTITY::SET_ENTITY_COORDS(playerPed, x, y, z, FALSE, FALSE, FALSE, FALSE);
                    ENTITY::SET_ENTITY_HEADING(playerPed, std::atan2(dx, -dy) * 57.29578f);
                    g_worldPicked = best;
                }
                else
                {
                    ENTITY::SET_ENTITY_VISIBLE(best, FALSE);
                    g_worldHidden = best;
                }
                char text[220]{};
                std::snprintf(text, sizeof(text), "%s=%d,model=0x%X,distance=%.2f,owner=%u,pos=%.2f;%.2f;%.2f",
                    step.args[2] == "pick" ? "picked" : "hidden", best, static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(best)),
                    bestDistance, owner, p.x, p.y, p.z);
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "weapon_component")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash weapon = MISC::GET_HASH_KEY(step.args[1].c_str());
                for (std::size_t n = 2; n < step.args.size(); ++n)
                    WEAPON::_GIVE_WEAPON_COMPONENT_TO_ENTITY(ped, MISC::GET_HASH_KEY(step.args[n].c_str()), weapon, TRUE);
                detail = "components=" + std::to_string(step.args.size() - 2);
                return StepResult::Done;
            }
            if (step.op == "strap_test")
            {
                if (step.args[0] == "clear")
                    replay::weapon::ClearStrapTestBits();
                else if (step.args[0] == "flags")
                {
                    // flags clearhex:weightA:weightB (a weight below 0 = as recorded)
                    unsigned clear = 0;
                    float a = -1.0f, b = -1.0f;
                    std::sscanf(step.args[1].c_str(), "%x:%f:%f", &clear, &a, &b);
                    replay::weapon::SetStrapTestFlags(clear, a, b);
                }
                else if (step.args[0] == "regive")
                    replay::weapon::RequestStrapTestRegive(
                        static_cast<std::uint32_t>(std::strtoul(step.args[1].c_str(), nullptr, 16)));
                else
                {
                    const auto hash = static_cast<std::uint32_t>(std::strtoul(step.args[1].c_str(), nullptr, 16));
                    replay::weapon::SetStrapTestBit(hash, step.args[0] == "hide" ? 1 : step.args[0] == "show" ? 0 : -1);
                }
                detail = step.args[0] + (step.args.size() > 1 ? "=" + step.args[1] : "");
                Log() << "BENCH_STRAP_TEST," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "ped_visible")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                ENTITY::SET_ENTITY_VISIBLE(ped, step.args[1] == "on" ? TRUE : FALSE);
                return StepResult::Done;
            }
            if (step.op == "ped_freeze")
            {
                const Ped ped = step.args[0] == "player" ? 0 : ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                ENTITY::FREEZE_ENTITY_POSITION(ped, step.args[1] == "on" ? TRUE : FALSE);
                return StepResult::Done;
            }
            if (step.op == "wearable_state")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                // The worn shop items (the cached entries, resolveSelection
                // false: story mode), each with its category and states; the
                // first in CATEGORY (or any) is the one set.
                const int type = PED::_GET_META_PED_TYPE(ped);
                const Hash category = step.args[1] == "any" ? 0u : MISC::GET_HASH_KEY(step.args[1].c_str());
                Hash target = 0;
                Hash targetState = 0;
                std::vector<Hash> targetStates;
                int items = 0;
                // The index is a component index (dump: 0xB440C8 -> 0xB45C00
                // maps it to the item's entry), so every component is asked
                // (the body and head layers come first and are no item:
                // stopping after 8 zeros could list nothing) and each item is
                // listed once.
                std::vector<Hash> seen;
                const int components = (std::min)(PED::_GET_NUM_COMPONENTS_IN_PED(ped), 128);
                for (int index = 0; index < components; ++index)
                {
                    BOOL status = FALSE;
                    Hash state = 0;
                    const Hash item = PED::_GET_SHOP_ITEM_COMPONENT_AT_INDEX(ped, index, FALSE, &status, &state);
                    if (!item || std::find(seen.begin(), seen.end(), item) != seen.end())
                        continue;
                    seen.push_back(item);
                    ++items;
                    const Hash itemCategory = PED::_GET_SHOP_ITEM_COMPONENT_CATEGORY(item, type, FALSE);
                    const int count = (std::min)(PED::_GET_SHOP_ITEM_NUM_WEARABLE_STATES(item, FALSE, TRUE), 16);
                    std::vector<Hash> states;
                    for (int n = 0; n < count; ++n)
                        states.push_back(PED::_GET_SHOP_ITEM_WEARABLE_STATE_BY_INDEX(item, n, FALSE, TRUE));
                    Log() << "BENCH_WEARABLE_ITEM,of=" << step.args[0] << ",index=" << index << ",item=0x" << std::hex << item
                          << ",category=0x" << itemCategory << ",state=0x" << state << std::dec << ",status=" << (status ? 1 : 0)
                          << ",states=" << count << ",list=";
                    for (std::size_t n = 0; n < states.size(); ++n)
                        Log() << (n ? ";" : "") << "0x" << std::hex << states[n] << std::dec;
                    Log() << '\n';
                    if (!target && (!category || itemCategory == category))
                    {
                        target = item;
                        targetState = state;
                        targetStates = states;
                    }
                }
                char text[160]{};
                const std::string& what = step.args[2];
                if (what == "list" || !target)
                {
                    std::snprintf(text, sizeof(text), "items=%d,target=0x%X,states=%u", items, static_cast<unsigned>(target),
                        static_cast<unsigned>(targetStates.size()));
                    detail = text;
                    // Diagnostic: no item in the category (story mode may keep
                    // no shop item entries) does not end the run, which may be
                    // recording (the detail says target=0x0).
                    return StepResult::Done;
                }
                Hash next = 0;
                if (what == "next")
                {
                    if (targetStates.empty())
                    {
                        detail = "no_states";
                        return StepResult::Done;
                    }
                    const auto at = std::find(targetStates.begin(), targetStates.end(), targetState);
                    next = at == targetStates.end() || at + 1 == targetStates.end() ? targetStates.front() : *(at + 1);
                }
                else if (what[0] == '@')
                {
                    const std::size_t n = static_cast<std::size_t>(std::strtol(what.c_str() + 1, nullptr, 10));
                    if (n >= targetStates.size())
                    {
                        detail = "no_such_state";
                        return StepResult::Done;
                    }
                    next = targetStates[n];
                }
                else
                    next = MISC::GET_HASH_KEY(what.c_str());
                const int layersBefore = PED::_GET_NUM_COMPONENTS_IN_PED(ped);
                PED::_UPDATE_SHOP_ITEM_WEARABLE_STATE(ped, target, next, 0, FALSE, 1);
                PED::_UPDATE_PED_VARIATION(ped, FALSE, TRUE, TRUE, TRUE, FALSE);
                std::snprintf(text, sizeof(text), "item=0x%X,from=0x%X,to=0x%X,layers_before=%d", static_cast<unsigned>(target),
                    static_cast<unsigned>(targetState), static_cast<unsigned>(next), layersBefore);
                detail = text;
                return StepResult::Done;
            }
            // A script sound by name on an entity. weapon:<id> is the ped's
            // held a script sound by name on an entity. weapon:<id> is the
            // ped's held weapon object, the case of the camp's cooking (the
            // sizzle plays on the meat, a weapon object: its sound sits on the
            // weapon's audio).
            if (step.op == "script_sound")
            {
                const std::string& target = step.args[0];
                Entity entity = 0;
                if (target.rfind("weapon:", 0) == 0)
                {
                    const Ped ped = ResolvePed(target.substr(7));
                    entity = ped ? WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(ped, 0) : 0;
                }
                else
                {
                    entity = target == "player" ? PLAYER::PLAYER_PED_ID() : ResolveSpawned(target);
                }
                if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity))
                {
                    detail = "unknown_entity";
                    return StepResult::Failed;
                }
                AUDIO::PLAY_SOUND_FROM_ENTITY(step.args[1].c_str(), entity, step.args[2].c_str(), FALSE, 0, 0);
                char text[96]{};
                std::snprintf(text, sizeof(text), "entity=%d,model=0x%X", entity, static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(entity)));
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "script_sound_var")
            {
                float value = 0.0f;
                ArgFloat(step, 1, value);
                AUDIO::_SET_VARIABLE_ON_SOUND_WITH_NAME(step.args[0].c_str(), value, step.args[2].c_str(), step.args[3].c_str());
                return StepResult::Done;
            }
            if (step.op == "script_sound_stop")
            {
                AUDIO::_STOP_SOUND_WITH_NAME(step.args[0].c_str(), step.args[1].c_str());
                return StepResult::Done;
            }
            if (step.op == "object_state")
            {
                // Another mod's prop (QA4: a lighting mod's lantern) is never
                // hidden nor deleted by the replay: what the object is now.
                const Entity entity = ResolveSpawned(step.args[0]);
                const bool exists = entity && ENTITY::DOES_ENTITY_EXIST(entity);
                const bool visible = exists && ENTITY::IS_ENTITY_VISIBLE(entity);
                const bool collisionOff = exists && ENTITY::GET_ENTITY_COLLISION_DISABLED(entity);
                const bool mission = exists && ENTITY::IS_ENTITY_A_MISSION_ENTITY(entity);
                Any scriptArgs[8]{};
                const std::uint32_t script = exists ? static_cast<std::uint32_t>(ENTITY::_GET_ENTITY_SCRIPT(entity, scriptArgs)) : 0;
                const std::string state = !exists ? "gone" : visible ? "visible" : "hidden";
                char line[256];
                std::snprintf(line, sizeof(line),
                    "BENCH_OBJECT_STATE,id=%s,handle=%d,state=%s,collision_off=%d,mission=%d,script=0x%x,playing=%d\n",
                    step.args[0].c_str(), entity, state.c_str(), collisionOff ? 1 : 0, mission ? 1 : 0, script,
                    g_context->transport.Playing() ? 1 : 0);
                Log() << line;
                detail = "state=" + state;
                if (step.args.size() == 2 && step.args[1] != state)
                    return StepResult::Failed;
                return StepResult::Done;
            }
            if (step.op == "object_census")
            {
                g_censusOn = step.args.size() == 4;
                for (int axis = 0; axis < 3 && g_censusOn; ++axis)
                    ArgFloat(step, static_cast<std::size_t>(axis), g_censusCenter[axis]);
                if (g_censusOn)
                    ArgFloat(step, 3, g_censusRadius);
                return StepResult::Done;
            }
            if (step.op == "leave_vehicle")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Vehicle vehicle = ped ? PED::GET_VEHICLE_PED_IS_IN(ped, FALSE) : 0;
                if (!ped || !vehicle)
                {
                    detail = !ped ? "unknown_ped" : "not_in_vehicle";
                    return StepResult::Failed;
                }
                TASK::TASK_LEAVE_VEHICLE(ped, vehicle, 0, 0);
                detail = "vehicle=" + std::to_string(vehicle);
                return StepResult::Done;
            }
            if (step.op == "explosion_at")
            {
                const Entity entity = ResolveSpawned(step.args[0]);
                if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity))
                {
                    detail = "unknown_entity";
                    return StepResult::Failed;
                }
                int type = 0;
                float scale = 1.0f;
                float offset[3]{};
                ArgInt(step, 1, type);
                ArgFloat(step, 2, scale);
                for (int axis = 0; axis < 3; ++axis)
                    ArgFloat(step, static_cast<std::size_t>(3 + axis), offset[axis]);
                const Vector3 at = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, offset[0], offset[1], offset[2]);
                FIRE::ADD_EXPLOSION(at.x, at.y, at.z, type, scale, TRUE, FALSE, 1.0f);
                std::ostringstream text;
                text << "at=" << at.x << ";" << at.y << ";" << at.z << ",type=" << type << ",scale=" << scale;
                detail = text.str();
                return StepResult::Done;
            }
            if (step.op == "dismount")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                TASK::TASK_DISMOUNT_ANIMAL(ped, 0, 0, 0, 0, 0);
                return StepResult::Done;
            }
            if (step.op == "pool_listing")
            {
                replay::objects::SetPoolListingDisabled(step.args[0] == "off");
                return StepResult::Done;
            }
            // Trains (replay_trains.h).
            if (step.op == "train_spawn")
            {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                ArgFloat(step, 2, x);
                ArgFloat(step, 3, y);
                ArgFloat(step, 4, z);
                Vehicle train = 0;
                const int result = replay::trains::BenchSpawn(step.args[1], x, y, z, step.args[5] == "1",
                    g_run.stepStartedAt, now, train, detail);
                if (result == 0)
                    return StepResult::Pending;
                if (result < 0)
                    return StepResult::Failed;
                auto& slot = g_benchTrains[step.args[0]];
                if (slot && slot != train)
                    replay::trains::BenchDelete(slot);
                slot = train;
                detail = "handle=" + std::to_string(train) + "," + detail;
                return StepResult::Done;
            }
            if (step.op == "train_speed")
            {
                float speed = 0.0f;
                ArgFloat(step, 1, speed);
                const auto found = g_benchTrains.find(step.args[0]);
                if (found == g_benchTrains.end())
                {
                    detail = "unknown_train";
                    return StepResult::Failed;
                }
                return replay::trains::BenchSpeed(found->second, speed, detail) ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "train_census")
            {
                const auto found = g_benchTrains.find(step.args[0]);
                if (found == g_benchTrains.end())
                {
                    detail = "unknown_train";
                    return StepResult::Failed;
                }
                replay::trains::BenchCensus(found->second, step.args[0], detail);
                return StepResult::Done; // evidence: never fails the run
            }
            if (step.op == "train_door")
            {
                const auto found = g_benchTrains.find(step.args[0]);
                if (found == g_benchTrains.end())
                {
                    detail = "unknown_train";
                    return StepResult::Failed;
                }
                return replay::trains::BenchDoor(found->second, std::atoi(step.args[1].c_str()), step.args[2], step.args[3], detail)
                    ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "no_random_trains")
            {
                replay::trains::BenchRandomTrains(step.args[0] == "on", detail);
                return StepResult::Done;
            }
            if (step.op == "throw_at")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                detail = ThrowProjectile(ped, Coords(step, 1));
                return StepResult::Done;
            }
            if (step.op == "throw_ped")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                Vector3 at = ENTITY::GET_ENTITY_COORDS(target, TRUE, FALSE);
                at.z += 0.3f;
                detail = ThrowProjectile(ped, at);
                return StepResult::Done;
            }
            if (step.op == "face")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                if (!g_run.stepPending)
                {
                    const Vector3 at = Coords(step, 1);
                    TASK::TASK_TURN_PED_TO_FACE_COORD(ped, at.x, at.y, at.z, 1000);
                }
                // As aim_at: the player's aim follows the gameplay camera.
                if (ped == player)
                {
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f, 1.0f);
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(0.0f, 1.0f);
                }
                return now - g_run.stepStartedAt < 1200 ? StepResult::Pending : StepResult::Done;
            }
            if (step.op == "ammo")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash ammo = MISC::GET_HASH_KEY(step.args[1].c_str());
                Hash weapon = step.args.size() > 2 ? MISC::GET_HASH_KEY(step.args[2].c_str()) : WEAPON::_GET_PED_CURRENT_HELD_WEAPON(ped);
                if (!weapon)
                    WEAPON::GET_CURRENT_PED_WEAPON(ped, &weapon, FALSE, 0, FALSE);
                if (!weapon)
                {
                    detail = "no_weapon";
                    return StepResult::Failed;
                }
                const BOOL ammoValid = WEAPON::_IS_AMMO_VALID(ammo);
                const BOOL validForWeapon = WEAPON::_IS_AMMO_TYPE_VALID_FOR_WEAPON(weapon, ammo);
                WEAPON::_ADD_AMMO_TO_PED_BY_TYPE(ped, ammo, 20, kAddReasonDefault);
                WEAPON::_ENABLE_AMMO_TYPE_FOR_PED_WEAPON(ped, weapon, ammo);
                WEAPON::_SET_AMMO_TYPE_FOR_PED_WEAPON(ped, weapon, ammo);
                const Object object = HeldWeaponObject(ped);
                const Hash current = object ? WEAPON::_GET_CURRENT_PED_WEAPON_AMMO_TYPE(ped, object) : 0;
                char text[192]{};
                std::snprintf(text, sizeof(text), "weapon=0x%X,ammo=0x%X,ammo_valid=%d,valid_for_weapon=%d,count=%d,current=0x%X,selected=%d",
                    static_cast<unsigned>(weapon), static_cast<unsigned>(ammo), ammoValid ? 1 : 0, validForWeapon ? 1 : 0,
                    WEAPON::GET_PED_AMMO_BY_TYPE(ped, ammo), static_cast<unsigned>(current), current == ammo ? 1 : 0);
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "weapon_condition")
            {
                // Visual condition of the held weapon object (dirt, soot, blood
                // and wear shaders). The last BOOL of the setters is undocumented;
                // community scripts that clean a weapon pass 0.
                const Ped ped = ResolvePed(step.args[0]);
                const Object object = ped ? HeldWeaponObject(ped) : 0;
                if (!object)
                {
                    detail = ped ? "no_weapon_object" : "unknown_ped";
                    return StepResult::Failed;
                }
                float level = 0.0f;
                if (step.args[1] != "-" && ArgFloat(step, 1, level))
                    WEAPON::_SET_WEAPON_DIRT(object, level, FALSE);
                if (step.args[2] != "-" && ArgFloat(step, 2, level))
                    WEAPON::_SET_WEAPON_SOOT(object, level, FALSE);
                if (step.args[3] != "-" && ArgFloat(step, 3, level))
                    WEAPON::_SET_WEAPON_DAMAGE(object, level, FALSE);
                if (step.args[4] != "-" && ArgFloat(step, 4, level))
                    WEAPON::_SET_WEAPON_DEGRADATION(object, level);
                char text[192]{};
                std::snprintf(text, sizeof(text), "object=%d,model=0x%X,dirt=%.2f,soot=%.2f,damage=%.2f,degradation=%.2f,permanent=%.2f",
                    object, static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(object)), WEAPON::_GET_WEAPON_DIRT(object),
                    WEAPON::_GET_WEAPON_SOOT(object), WEAPON::_GET_WEAPON_DAMAGE(object), WEAPON::GET_WEAPON_DEGRADATION(object),
                    WEAPON::GET_WEAPON_PERMANENT_DEGRADATION(object));
                detail = text;
                Log() << "BENCH_WEAPON_CONDITION,id=" << step.args[0] << "," << detail << '\n';
                return StepResult::Done;
            }
            // Tool only: the player's own weapons keep their components only
            // when selected, not given again; reloads need a finite clip
            // (give_weapon makes it infinite, as the game's scripts arm a
            // scripted shooter).
            if (step.op == "equip" || step.op == "clip_ammo" || step.op == "reload")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                if (step.op == "equip")
                {
                    const Hash weapon = MISC::GET_HASH_KEY(step.args[1].c_str());
                    const BOOL had = WEAPON::HAS_PED_GOT_WEAPON(ped, weapon, 0, FALSE);
                    if (!had)
                    {
                        detail = "not_owned";
                        Log() << "BENCH_EQUIP,id=" << step.args[0] << ",weapon=" << step.args[1] << ",had=0\n";
                        return StepResult::Failed;
                    }
                    WEAPON::SET_CURRENT_PED_WEAPON(ped, weapon, TRUE, 0, FALSE, FALSE);
                    WEAPON::_SET_PED_INFINITE_AMMO_CLIP(ped, FALSE);
                }
                Hash held = WEAPON::_GET_PED_CURRENT_HELD_WEAPON(ped);
                if (!held)
                    WEAPON::GET_CURRENT_PED_WEAPON(ped, &held, FALSE, 0, FALSE);
                int rounds = -1;
                BOOL setOk = FALSE;
                BOOL reloadAsked = FALSE;
                if (step.op == "clip_ammo" && ArgInt(step, 1, rounds) && held)
                {
                    WEAPON::_SET_PED_INFINITE_AMMO_CLIP(ped, FALSE);
                    WEAPON::_ADD_AMMO_TO_PED_BY_TYPE(ped, WEAPON::GET_PED_AMMO_TYPE_FROM_WEAPON(ped, held), 60, kAddReasonDefault);
                    setOk = WEAPON::SET_AMMO_IN_CLIP(ped, held, rounds);
                }
                if (step.op == "reload")
                {
                    reloadAsked = WEAPON::_MAKE_PED_RELOAD(ped) ? TRUE : FALSE;
                    if (!reloadAsked)
                        TASK::TASK_RELOAD_WEAPON(ped, TRUE);
                }
                int inClip = 0;
                if (held)
                    WEAPON::GET_AMMO_IN_CLIP(ped, &inClip, held);
                char text[160]{};
                std::snprintf(text, sizeof(text), "held=0x%X,rounds=%d,set_ok=%d,reload_asked=%d,in_clip=%d,object=%d",
                    static_cast<unsigned>(held), rounds, setOk ? 1 : 0, reloadAsked ? 1 : 0, inClip, HeldWeaponObject(ped));
                detail = text;
                Log() << "BENCH_" << (step.op == "equip" ? "EQUIP" : step.op == "reload" ? "RELOAD" : "CLIP_AMMO") << ",id=" << step.args[0]
                      << "," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "inventory")
            {
                // Research: an item interaction only makes its prop with the
                // item in the inventory.
                // The satchel is reached through a guid chain (CHARACTER, then
                // WARDROBE) as community scripts do; each link is logged and
                // two parents are tried. Guid buffers are oversized and zeroed.
                int qty = 1;
                ArgInt(step, 1, qty);
                int inventory = INVENTORY::_INVENTORY_GET_INVENTORY_ID_FROM_PED(player);
                if (inventory <= 0)
                    inventory = 1;
                const Hash item = MISC::GET_HASH_KEY(step.args[0].c_str());
                const Hash slotNone = MISC::GET_HASH_KEY("SLOTID_NONE");
                const Hash slotSatchel = MISC::GET_HASH_KEY("SLOTID_SATCHEL");
                const BOOL keyValid = ITEMDATABASE::_ITEMDATABASE_IS_KEY_VALID(item, 0);
                const int before = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(inventory, item, FALSE);
                alignas(8) std::uint64_t character[32]{};
                alignas(8) std::uint64_t wardrobe[32]{};
                const BOOL gotCharacter = INVENTORY::INVENTORY_GET_GUID_FROM_ITEMID(inventory, nullptr,
                    MISC::GET_HASH_KEY("CHARACTER"), slotNone, reinterpret_cast<Any*>(character));
                const BOOL gotWardrobe = gotCharacter && INVENTORY::INVENTORY_GET_GUID_FROM_ITEMID(inventory,
                    reinterpret_cast<Any*>(character), MISC::GET_HASH_KEY("WARDROBE"), MISC::GET_HASH_KEY("SLOTID_WARDROBE"),
                    reinterpret_cast<Any*>(wardrobe));
                const char* path = "none";
                int after = before;
                const struct
                {
                    bool usable;
                    std::uint64_t* parent;
                    const char* name;
                } parents[] = {{gotWardrobe != FALSE, wardrobe, "wardrobe"}, {gotCharacter != FALSE, character, "character"}};
                for (const auto& parent : parents)
                {
                    if (!parent.usable)
                        continue;
                    alignas(8) std::uint64_t added[32]{};
                    const BOOL ok = INVENTORY::_INVENTORY_ADD_ITEM_WITH_GUID(inventory, reinterpret_cast<Any*>(added),
                        reinterpret_cast<Any*>(parent.parent), item, slotSatchel, qty, kAddReasonDefault);
                    after = INVENTORY::_INVENTORY_GET_INVENTORY_ITEM_COUNT_WITH_ITEMID(inventory, item, FALSE);
                    Log() << "BENCH_INVENTORY_TRY,item=" << step.args[0] << ",parent=" << parent.name << ",ok=" << (ok ? 1 : 0)
                          << ",count=" << after << '\n';
                    if (ok && after > before)
                    {
                        path = parent.name;
                        break;
                    }
                }
                char text[192]{};
                std::snprintf(text, sizeof(text), "inventory=%d,key_valid=%d,character=%d,wardrobe=%d,path=%s,count_before=%d,count_after=%d",
                    inventory, keyValid ? 1 : 0, gotCharacter ? 1 : 0, gotWardrobe ? 1 : 0, path, before, after);
                detail = text;
                // A miss is evidence: the run goes on (an item interaction then shows no prop).
                return StepResult::Done;
            }
            if (step.op == "lasso_target")
            {
                // TASK_LASSO_PED alone never throws. Here the lasso is in hand
                // and the two are hostile; halfway through the limit without a
                // catch, TASK_COMBAT_PED with the lasso as the only weapon in
                // hand (bounty hunters lasso in combat).
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target || ped == player)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                int limit = 8000;
                if (step.args.size() > 2)
                    ArgInt(step, 2, limit);
                const Hash lasso = MISC::GET_HASH_KEY("WEAPON_LASSO");
                if (!g_run.stepPending)
                {
                    if (!WEAPON::HAS_PED_GOT_WEAPON(ped, lasso, 0, FALSE))
                        WEAPON::GIVE_WEAPON_TO_PED(ped, lasso, 1, TRUE, FALSE, 0, FALSE, 0.5f, 1.0f, kAddReasonDefault, TRUE, 0.0f, FALSE);
                    WEAPON::SET_CURRENT_PED_WEAPON(ped, lasso, TRUE, 0, FALSE, FALSE);
                    SetHostile(ped, target);
                    TASK::TASK_LASSO_PED(ped, target);
                    g_run.lassoFallback = false;
                    return StepResult::Pending;
                }
                const std::uint64_t elapsed = now - g_run.stepStartedAt;
                const bool caught = PED::IS_PED_LASSOED(target) != FALSE || PED::_GET_LASSO_TARGET(ped) == target;
                if (!caught && !g_run.lassoFallback && elapsed >= static_cast<std::uint64_t>(limit / 2))
                {
                    g_run.lassoFallback = true;
                    TASK::TASK_COMBAT_PED(ped, target, 0, 16);
                    Log() << "BENCH_LASSO_FALLBACK,id=" << step.args[0] << ",target=" << step.args[1] << ",after_ms=" << elapsed << '\n';
                }
                if (!caught && elapsed < static_cast<std::uint64_t>(limit))
                    return StepResult::Pending;
                detail = std::string("caught=") + (caught ? "1" : "0") + ",path=" + (g_run.lassoFallback ? "combat" : "task") +
                    ",lassoer_is_it=" + (PED::_GET_LASSOER_OF_PED(target) == ped ? "1" : "0") +
                    ",held=" + std::to_string(WEAPON::_GET_PED_CURRENT_HELD_WEAPON(ped) == lasso ? 1 : 0) +
                    ",after_ms=" + std::to_string(elapsed);
                return StepResult::Done;
            }
            if (step.op == "melee")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash action = step.args[2] == "0" ? 0 : MISC::GET_HASH_KEY(step.args[2].c_str());
                SetHostile(ped, target);
                // Only p2 is documented (the action); the rest as zeros.
                const BOOL started = TASK::TASK_MELEE(ped, target, action, 0, 0, 0.0f, 0, 0.0f);
                detail = std::string("started=") + (started ? "1" : "0");
                return StepResult::Done;
            }
            if (step.op == "animal_interaction")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped animal = ResolvePed(step.args[1]);
                if (!ped || !animal)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash model = step.args[3] == "-" ? 0 : MISC::GET_HASH_KEY(step.args[3].c_str());
                if (model)
                {
                    if (const int loaded = LoadModel(model, now, detail); loaded <= 0)
                        return loaded < 0 ? StepResult::Failed : StepResult::Pending;
                }
                TASK::TASK_ANIMAL_INTERACTION(ped, animal, MISC::GET_HASH_KEY(step.args[2].c_str()), model, FALSE);
                return StepResult::Done;
            }
            if (step.op == "drop_weapon")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                WEAPON::SET_PED_DROPS_INVENTORY_WEAPON(ped, MISC::GET_HASH_KEY(step.args[1].c_str()), 0.0f, 0.0f, 0.0f, 9);
                return StepResult::Done;
            }
            if (step.op == "cam")
            {
                g_camArgs.clear();
                if (step.args[0] == "off")
                {
                    StopTopCam();
                    return StepResult::Done;
                }
                float fov = 50.0f;
                ArgFloat(step, 6, fov);
                if (!StartCam(Coords(step, 0), Coords(step, 3), fov))
                {
                    detail = "cam_failed";
                    return StepResult::Failed;
                }
                g_camArgs = step.args;
                return StepResult::Done;
            }
            if (step.op == "purge_objects")
            {
                // Leftovers of a stage whose handles a reload lost.
                const Hash model = MISC::GET_HASH_KEY(step.args[0].c_str());
                float radius = 0.0f;
                ArgFloat(step, 1, radius);
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                static int objects[4096];
                const int count = worldGetAllObjects(objects, 4096);
                int deleted = 0;
                for (int n = 0; n < count; ++n)
                {
                    Object object = objects[n];
                    if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || ENTITY::GET_ENTITY_MODEL(object) != model)
                        continue;
                    bool ours = false;
                    for (const auto& [id, entity] : g_stage)
                        ours = ours || entity == object;
                    if (ours)
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    const float dx = p.x - at.x;
                    const float dy = p.y - at.y;
                    if (dx * dx + dy * dy > radius * radius)
                        continue;
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                    ENTITY::DELETE_ENTITY(&object);
                    ++deleted;
                }
                detail = "deleted=" + std::to_string(deleted);
                return StepResult::Done;
            }
            if (step.op == "stage_clear")
            {
                UnlockEnvironment();
                detail = "deleted=" + std::to_string(DeleteAll(g_stage));
                return StepResult::Done;
            }
            if (step.op == "require_stage")
            {
                if (g_stage.empty())
                {
                    detail = "no_stage";
                    return StepResult::Failed;
                }
                detail = "objects=" + std::to_string(g_stage.size());
                return StepResult::Done;
            }
            if (step.op == "spawn_object" || step.op == "stage_object")
            {
                const Hash model = MISC::GET_HASH_KEY(step.args[1].c_str());
                if (const int loaded = LoadModel(model, now, detail); loaded <= 0)
                    return loaded < 0 ? StepResult::Failed : StepResult::Pending;
                const Vector3 at = Coords(step, 2);
                float heading = 0.0f;
                ArgFloat(step, 5, heading);
                Object object = OBJECT::CREATE_OBJECT(model, at.x, at.y, at.z, FALSE, FALSE, FALSE, FALSE, FALSE);
                STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
                if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
                {
                    detail = "create_failed";
                    return StepResult::Failed;
                }
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                ENTITY::SET_ENTITY_HEADING(object, heading);
                ENTITY::FREEZE_ENTITY_POSITION(object, TRUE);
                // A mission's invisible object.
                if (step.args.size() == 7 && step.args[6] == "hidden")
                    ENTITY::SET_ENTITY_VISIBLE(object, FALSE);
                auto& owner = step.op == "stage_object" ? g_stage : g_spawned;
                if (const auto old = owner.find(step.args[0]); old != owner.end() && ENTITY::DOES_ENTITY_EXIST(old->second))
                {
                    Entity previous = old->second;
                    ENTITY::DELETE_ENTITY(&previous);
                }
                owner[step.args[0]] = object;
                if (step.op == "stage_object")
                    g_stageSpecs[step.args[0]] = {model, at, heading};
                detail = "handle=" + std::to_string(object);
                return StepResult::Done;
            }
            if (step.op == "give_weapon")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Hash weapon = MISC::GET_HASH_KEY(step.args[1].c_str());
                // Optional attach point (an NPC's lantern given at the primary
                // hand never shows; 11 is the lantern's).
                int attach = 0;
                if (step.args.size() > 2)
                    ArgInt(step, 2, attach);
                WEAPON::GIVE_WEAPON_TO_PED(ped, weapon, 100, TRUE, FALSE, attach, FALSE, 0.5f, 1.0f,
                    kAddReasonDefault, TRUE, 0.0f, FALSE);
                WEAPON::SET_CURRENT_PED_WEAPON(ped, weapon, TRUE, attach, FALSE, FALSE);
                // As the game's scripts arm a scripted shooter.
                WEAPON::_SET_PED_INFINITE_AMMO_CLIP(ped, TRUE);
                char text[48]{};
                std::snprintf(text, sizeof(text), "held=0x%X", static_cast<unsigned>(WEAPON::_GET_PED_CURRENT_HELD_WEAPON(ped)));
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "shoot_at")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const Vector3 at = Coords(step, 1);
                float ms = 0.0f;
                ArgFloat(step, 4, ms);
                // The firing pattern as the game's scripts pass it.
                const char* pattern = step.args.size() > 5 ? step.args[5].c_str() : "FIRING_PATTERN_BURST_FIRE";
                PED::SET_PED_ACCURACY(ped, 100);
                TASK::TASK_SHOOT_AT_COORD(ped, at.x, at.y, at.z, static_cast<int>(ms), MISC::GET_HASH_KEY(pattern), 0);
                return StepResult::Done;
            }
            if (step.op == "shoot_ped")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                float ms = 0.0f;
                ArgFloat(step, 2, ms);
                const char* pattern = step.args.size() > 3 ? step.args[3].c_str() : "FIRING_PATTERN_BURST_FIRE";
                // Bullets of a ped skip the peds of its own relationship group:
                // shooter and target go to two groups that hate each other.
                static Hash shooters = 0;
                static Hash targets = 0;
                if (!shooters)
                    PED::ADD_RELATIONSHIP_GROUP("BENCH_SHOOTERS", &shooters);
                if (!targets)
                    PED::ADD_RELATIONSHIP_GROUP("BENCH_TARGETS", &targets);
                PED::SET_PED_RELATIONSHIP_GROUP_HASH(ped, shooters);
                PED::SET_PED_RELATIONSHIP_GROUP_HASH(target, targets);
                PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, shooters, targets); // hate
                PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, targets, shooters);
                PED::SET_PED_ACCURACY(ped, 100);
                TASK::TASK_SHOOT_AT_ENTITY(ped, target, static_cast<int>(ms), MISC::GET_HASH_KEY(pattern), 0);
                return StepResult::Done;
            }
            if (step.op == "unfreeze")
            {
                const auto found = g_spawned.find(step.args[0]);
                if (found == g_spawned.end() || !ENTITY::DOES_ENTITY_EXIST(found->second))
                {
                    detail = "unknown_object";
                    return StepResult::Failed;
                }
                ENTITY::FREEZE_ENTITY_POSITION(found->second, FALSE);
                ENTITY::SET_ENTITY_DYNAMIC(found->second, TRUE);
                PHYSICS::ACTIVATE_PHYSICS(found->second);
                return StepResult::Done;
            }
            // A spawned object attached to another (the chest's lining and
            // loot), detached, or given its skeleton (what the engine does to
            // the chest when it opens). A spawned object's bones animated
            // after the recording took it (a chair tilting), with an object
            // animation the game's scripts play on that model
            // (PLAY_ENTITY_ANIM builds the skeleton when the object has none
            // yet).
            if (step.op == "object_anim")
            {
                const Entity object = ResolveSpawned(step.args[0]);
                if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || !ENTITY::IS_ENTITY_AN_OBJECT(object))
                {
                    detail = "unknown_object";
                    return StepResult::Failed;
                }
                const char* dict = step.args[1].c_str();
                // A dict the game lacks or never loads ends the step without
                // failing the run (the scenario's other objects still count).
                if (!STREAMING::DOES_ANIM_DICT_EXIST(dict))
                {
                    detail = "started=0,why=no_anim_dict";
                    return StepResult::Done;
                }
                if (!STREAMING::HAS_ANIM_DICT_LOADED(dict))
                {
                    STREAMING::REQUEST_ANIM_DICT(dict);
                    if (now - g_run.stepStartedAt > kModelTimeoutMs)
                    {
                        detail = "started=0,why=anim_dict_timeout";
                        return StepResult::Done;
                    }
                    return StepResult::Pending;
                }
                const bool loop = step.args.size() < 4 || step.args[3] == "1";
                const BOOL started =
                    ENTITY::PLAY_ENTITY_ANIM(object, step.args[2].c_str(), dict, 8.0f, loop ? TRUE : FALSE, TRUE, FALSE, 0.0f, 0);
                detail = "started=" + std::to_string(started ? 1 : 0) + ",loop=" + std::to_string(loop ? 1 : 0) +
                    ",handle=" + std::to_string(object);
                return StepResult::Done;
            }
            if (step.op == "attach_object" || step.op == "detach_object" || step.op == "object_skeleton")
            {
                const Entity object = ResolveSpawned(step.args[0]);
                if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || !ENTITY::IS_ENTITY_AN_OBJECT(object))
                {
                    detail = "unknown_object";
                    return StepResult::Failed;
                }
                if (step.op == "detach_object")
                {
                    ENTITY::DETACH_ENTITY(object, TRUE, FALSE);
                    detail = "attached=" + std::to_string(ENTITY::IS_ENTITY_ATTACHED(object) ? 1 : 0);
                    return StepResult::Done;
                }
                if (step.op == "object_skeleton")
                {
                    OBJECT::_0x491439AEF410A2FC(object);
                    detail = "handle=" + std::to_string(object);
                    return StepResult::Done;
                }
                // The player as the parent (an object in the hand, recorded as
                // an attached object like the cooked meat).
                const Entity parent = step.args[1] == "player" ? PLAYER::PLAYER_PED_ID() : ResolveSpawned(step.args[1]);
                if (!parent || !ENTITY::DOES_ENTITY_EXIST(parent))
                {
                    detail = "unknown_parent";
                    return StepResult::Failed;
                }
                const Vector3 offset = Coords(step, 2);
                float rx = 0.0f, ry = 0.0f, rz = 0.0f;
                ArgFloat(step, 5, rx);
                ArgFloat(step, 6, ry);
                ArgFloat(step, 7, rz);
                ENTITY::FREEZE_ENTITY_POSITION(object, FALSE);
                ENTITY::ATTACH_ENTITY_TO_ENTITY(object, parent, 0, offset.x, offset.y, offset.z, rx, ry, rz,
                    FALSE, FALSE, FALSE, FALSE, 2, TRUE, FALSE, FALSE);
                detail = "attached_to=" + std::to_string(ENTITY::GET_ENTITY_ATTACHED_TO(object));
                return StepResult::Done;
            }
            // A spawned object's burn level set the way the camp cooking script
            // sets the meat's; the detail is the effect block's level read back
            // (burn_level.h), -1 none.
            if (step.op == "object_burn")
            {
                const Entity object = ResolveSpawned(step.args[0]);
                if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || !ENTITY::IS_ENTITY_AN_OBJECT(object))
                {
                    detail = "unknown_object";
                    return StepResult::Failed;
                }
                float level = 0.0f;
                ArgFloat(step, 1, level);
                replay::burn::State before;
                const bool hadBlock = replay::burn::Read(replay::entities::AddressOf(object), before);
                OBJECT::_SET_OBJECT_BURN_LEVEL(object, level, TRUE);
                replay::burn::State after;
                const bool hasBlock = replay::burn::Read(replay::entities::AddressOf(object), after);
                char text[192];
                std::snprintf(text, sizeof(text), "object=%d,model=0x%X,asked=%.3f,before=%.3f,after=%.3f,fire_vt=0x%X",
                    static_cast<int>(object), static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(object)), level,
                    hadBlock ? before.level : -1.0f, hasBlock ? after.level : -1.0f, hasBlock ? after.fireVtableRva : 0u);
                detail = text;
                return StepResult::Done;
            }
            // A document's text put on a spawned object the way the scripts do
            // it (REQUEST_STREAMED_TXD, wait for HAS_STREAMED_TXD_LOADED,
            // SET_CUSTOM_TEXTURES_ON_OBJECT), and its tint; the detail is the
            // fields read back (object_look.h), so the step also proves the
            // layout live (+0x420 == the hash asked).
            if (step.op == "object_texture" || step.op == "object_tint")
            {
                const Entity object = ResolveSpawned(step.args[0]);
                if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || !ENTITY::IS_ENTITY_AN_OBJECT(object))
                {
                    detail = "unknown_object";
                    return StepResult::Failed;
                }
                replay::ObjectLook before;
                std::uint32_t wordBefore = 0;
                const bool readBefore = replay::objlook::ReadLook(object, before, wordBefore);
                Hash txd = 0, p2 = 0, p3 = 0;
                int tint = -1;
                if (step.op == "object_texture")
                {
                    txd = step.args[1] == "none" ? 0u : IplHashArg(step.args[1]);
                    p2 = step.args.size() > 2 && step.args[2] != "none" ? IplHashArg(step.args[2]) : 0u;
                    p3 = step.args.size() > 3 && step.args[3] != "none" ? IplHashArg(step.args[3]) : 0u;
                    bool loaded = true;
                    for (Hash hash : {txd, p2, p3})
                    {
                        if (!hash)
                            continue;
                        if (!TXD::DOES_STREAMED_TXD_EXIST(hash))
                        {
                            char text[64];
                            std::snprintf(text, sizeof(text), "txd_missing=0x%08X", static_cast<unsigned>(hash));
                            detail = text;
                            return StepResult::Failed;
                        }
                        TXD::REQUEST_STREAMED_TXD(hash, FALSE);
                        loaded = loaded && TXD::HAS_STREAMED_TXD_LOADED(hash);
                    }
                    if (!loaded && now - g_run.stepStartedAt < 5000)
                        return StepResult::Pending;
                    if (!loaded)
                    {
                        detail = "txd_timeout";
                        return StepResult::Failed;
                    }
                    OBJECT::SET_CUSTOM_TEXTURES_ON_OBJECT(object, txd, static_cast<Any>(p2), static_cast<Any>(p3));
                }
                else
                {
                    ArgInt(step, 1, tint);
                    OBJECT::SET_OBJECT_TINT_INDEX(object, tint);
                }
                replay::ObjectLook after;
                std::uint32_t wordAfter = 0;
                const bool readAfter = replay::objlook::ReadLook(object, after, wordAfter);
                char text[320];
                std::snprintf(text, sizeof(text),
                    "object=%d,model=0x%X,asked_txd=0x%08X,asked_p2=0x%08X,asked_p3=0x%08X,asked_tint=%d,read=%d,"
                    "before_txd=0x%08X,before_tint=%u,after_txd=0x%08X,after_p2=0x%08X,after_p3=0x%08X,after_tint=%u,"
                    "word_before=0x%08X,word_after=0x%08X,layout=%s",
                    static_cast<int>(object), static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(object)), static_cast<unsigned>(txd),
                    static_cast<unsigned>(p2), static_cast<unsigned>(p3), tint, readBefore && readAfter ? 1 : 0, before.txd,
                    static_cast<unsigned>(before.tint), after.txd, after.p2, after.p3, static_cast<unsigned>(after.tint), wordBefore,
                    wordAfter,
                    !readAfter ? "unread"
                    : step.op == "object_texture" ? (after.txd == txd && after.p2 == p2 && after.p3 == p3 ? "confirmed" : "differs")
                                                  : (after.tint == static_cast<unsigned>(tint) ? "confirmed" : "differs"));
                detail = text;
                return StepResult::Done;
            }
            if (step.op == "disk_save" || step.op == "disk_load")
            {
                std::string error;
                const bool ok = step.op == "disk_save" ? g_hooks->WriteClip(step.args[0], error) : g_hooks->ReadClip(step.args[0], error);
                if (!ok)
                {
                    detail = error;
                    return StepResult::Failed;
                }
                return StepResult::Done;
            }
            if (step.op == "puppet_velocity")
            {
                g_puppetVelocity = step.args[0] == "on";
                g_puppetMotion.clear();
                return StepResult::Done;
            }
            if (step.op == "puppet_collision")
            {
                g_puppetCollision = step.args[0] == "on" ? 1 : step.args[0] == "nofreeze" ? 2 : 0;
                return StepResult::Done;
            }
            if (step.op == "still_pose")
            {
                // The A/B of the still rule on the same clip; read when a
                // puppet is armed, so it applies from the next play.
                replay::objects::SetStillPose(step.args[0] == "on", step.args[1] == "on");
                detail = std::string("rule=") + (replay::objects::StillPoseEngine() ? "1" : "0") +
                    ",drawn_check=" + (replay::objects::ObjectDrawnCheck() ? "1" : "0");
                return StepResult::Done;
            }
            if (step.op == "music")
            {
                // The score off in the recording too, so a sound measure
                // compares the sounds alone (the replay's MusicStop uses the
                // same flag and event).
                const bool off = step.args[0] == "off";
                AUDIO::SET_AUDIO_FLAG("OpenWorldMusicDisabled", off ? TRUE : FALSE);
                if (off)
                    AUDIO::TRIGGER_MUSIC_EVENT("STOP_MUSIC_8S");
                return StepResult::Done;
            }
            if (step.op == "disarm")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                WEAPON::REMOVE_ALL_PED_WEAPONS(ped, TRUE, TRUE);
                return StepResult::Done;
            }
            if (step.op == "fight")
            {
                const Ped ped = ResolvePed(step.args[0]);
                const Ped target = ResolvePed(step.args[1]);
                if (!ped || !target)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                SetHostile(ped, target);
                TASK::TASK_COMBAT_PED(ped, target, 0, 16);
                return StepResult::Done;
            }
            if (step.op == "fx_registered")
            {
                std::string error;
                if (!replay::fx::SetRegisteredObserver(step.args[0] == "on", error))
                {
                    detail = error;
                    return StepResult::Failed;
                }
                detail = "observer=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "fx_watch")
            {
                const std::uint32_t hash = static_cast<std::uint32_t>(std::strtoul(step.args[0].c_str(), nullptr, 0));
                replay::fx::SetWatchEffect(hash);
                replay::events::SetWatchEffect(hash);
                detail = "hash=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "say" || step.op == "scripted_speech")
            {
                // PLAY_PED_AMBIENT_SPEECH_NATIVE / _CREATE_NEW_SCRIPTED_PED_
                // AMBIENT_SPEECH with the public ScriptedSpeechParams (0x40
                // bytes; natives.h). A scripted line is created at once and
                // played after the delay, as a script or conversation does.
                struct SpeechParams
                {
                    const char* speechName;
                    const char* voiceName;
                    alignas(8) int variation;
                    alignas(8) Hash speechParamHash;
                    alignas(8) Ped listenerPed;
                    alignas(8) BOOL syncOverNetwork;
                    alignas(8) int v7;
                    alignas(8) int v8;
                };
                static_assert(sizeof(SpeechParams) == 0x40, "ScriptedSpeechParams is 0x40 bytes");
                static int scriptedHandle = -1;
                static std::uint64_t scriptedPlayAt = 0;
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const bool scripted = step.op == "scripted_speech";
                if (!g_run.stepPending)
                {
                    int variation = 0;
                    ArgInt(step, 3, variation);
                    const std::string& voice = step.args[2];
                    SpeechParams params{};
                    params.speechName = step.args[1].c_str();
                    params.voiceName = voice == "-" ? nullptr : voice.c_str();
                    params.variation = variation;
                    params.speechParamHash = !scripted && step.args.size() > 4 ? MISC::GET_HASH_KEY(step.args[4].c_str()) : 0;
                    params.listenerPed = 0;
                    params.syncOverNetwork = FALSE;
                    params.v7 = 1;
                    params.v8 = 1;
                    if (!scripted)
                    {
                        const BOOL played = AUDIO::PLAY_PED_AMBIENT_SPEECH_NATIVE(ped, reinterpret_cast<Any*>(&params));
                        detail = "id=" + step.args[0] + ",context=" + step.args[1] + ",voice=" + voice +
                            ",variation=" + std::to_string(variation) + ",played=" + (played ? "1" : "0") +
                            ",last=" + std::to_string(static_cast<unsigned>(AUDIO::_GET_LAST_PLAYED_SPEECH_FOR_PED(ped)));
                        Log() << "BENCH_SAY," << detail << '\n';
                        return StepResult::Done;
                    }
                    int delayMs = 0;
                    ArgInt(step, 4, delayMs);
                    scriptedHandle = AUDIO::_CREATE_NEW_SCRIPTED_PED_AMBIENT_SPEECH(ped, reinterpret_cast<Any*>(&params));
                    scriptedPlayAt = now + static_cast<std::uint64_t>(delayMs);
                    Log() << "BENCH_SCRIPTED_SPEECH,id=" << step.args[0] << ",context=" << step.args[1]
                          << ",voice=" << voice << ",variation=" << variation << ",handle=" << scriptedHandle
                          << ",delay_ms=" << delayMs << '\n';
                    if (scriptedHandle < 0)
                    {
                        detail = "create_failed";
                        return StepResult::Failed;
                    }
                    return StepResult::Pending;
                }
                if (now < scriptedPlayAt)
                    return StepResult::Pending;
                // Poll until it plays (1) or is consumed (2); 0 = not ready.
                const int status = AUDIO::_PLAY_SOUND_FROM_SCRIPTED_PED_AMBIENT_SPEECH(scriptedHandle);
                if (status == 0 && now - scriptedPlayAt < 5000)
                    return StepResult::Pending;
                detail = "id=" + step.args[0] + ",handle=" + std::to_string(scriptedHandle) + ",play_status=" +
                    std::to_string(status) + ",waited_ms=" + std::to_string(now - scriptedPlayAt);
                Log() << "BENCH_SCRIPTED_SPEECH_PLAY," << detail << '\n';
                scriptedHandle = -1;
                return status == 0 ? StepResult::Failed : StepResult::Done;
            }
            if (step.op == "vocal")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const BOOL flag = step.args.size() < 3 || step.args[2] == "1";
                AUDIO::PLAY_ANIMAL_VOCALIZATION(ped, step.args[1].c_str(), flag);
                detail = "id=" + step.args[0] + ",vocalization=" + step.args[1] + ",flag=" + (flag ? "1" : "0");
                Log() << "BENCH_VOCAL," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "stop_speech")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const bool speaking = AUDIO::IS_ANY_SPEECH_PLAYING(ped) != FALSE;
                AUDIO::STOP_CURRENT_PLAYING_SPEECH(ped, 0);
                detail = "id=" + step.args[0] + ",was_speaking=" + (speaking ? "1" : "0");
                Log() << "BENCH_STOP_SPEECH," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "interact")
            {
                // The player's focus interaction, as gameplay does it: face
                // the ped, hold focus (INPUT_AIM, unarmed) and press the
                // lock-on prompt: greet = INPUT_INTERACT_LOCKON_POS,
                // antagonize = INPUT_INTERACT_LOCKON_NEG; defuse is the
                // positive prompt of a ped that was antagonized (run it
                // after an antagonize). Script-thread control values only;
                // which prompt the game offers is a live-test hypothesis.
                const Ped target = ResolvePed(step.args[0]);
                if (!target || target == player)
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                const std::uint64_t elapsed = now - g_run.stepStartedAt;
                if (!g_run.stepPending)
                    TASK::TASK_TURN_PED_TO_FACE_ENTITY(player, target, 1000, 0.0f, 0.0f, 0.0f);
                CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f, 1.0f);
                if (elapsed < 3500)
                    PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, MISC::GET_HASH_KEY("INPUT_AIM"), 1.0f);
                const char* prompt = step.args[1] == "antagonize" ? "INPUT_INTERACT_LOCKON_NEG" : "INPUT_INTERACT_LOCKON_POS";
                if (elapsed >= 1500 && elapsed < 1800)
                    PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, MISC::GET_HASH_KEY(prompt), 1.0f);
                if (elapsed < 4000)
                    return StepResult::Pending;
                detail = "id=" + step.args[0] + ",kind=" + step.args[1] + ",prompt=" + prompt +
                    ",player_last=" + std::to_string(static_cast<unsigned>(AUDIO::_GET_LAST_PLAYED_SPEECH_FOR_PED(player))) +
                    ",ped_last=" + std::to_string(static_cast<unsigned>(AUDIO::_GET_LAST_PLAYED_SPEECH_FOR_PED(target)));
                Log() << "BENCH_INTERACT," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "where")
            {
                Entity entity = step.args[0] == "player" ? player : 0;
                if (const auto found = g_spawned.find(step.args[0]); found != g_spawned.end())
                    entity = found->second;
                std::ostringstream where;
                where << "id=" << step.args[0] << ",handle=" << entity;
                if (entity && ENTITY::DOES_ENTITY_EXIST(entity))
                {
                    const Vector3 pos = ENTITY::GET_ENTITY_COORDS(entity, TRUE, FALSE);
                    where << ",pos=" << pos.x << ";" << pos.y << ";" << pos.z
                          << ",health=" << ENTITY::GET_ENTITY_HEALTH(entity) << "/" << ENTITY::GET_ENTITY_MAX_HEALTH(entity, FALSE)
                          << ",dead=" << (ENTITY::IS_ENTITY_DEAD(entity) ? 1 : 0)
                          << ",mission=" << (ENTITY::IS_ENTITY_A_MISSION_ENTITY(entity) ? 1 : 0)
                          << ",on_fire=" << (FIRE::IS_ENTITY_ON_FIRE(entity) ? 1 : 0)
                          << ",speed=" << ENTITY::GET_ENTITY_SPEED(entity);
                    if (ENTITY::IS_ENTITY_A_PED(entity))
                        where << ",ragdoll=" << (PED::IS_PED_RAGDOLL(entity) ? 1 : 0)
                              << ",accuracy=" << PED::GET_PED_ACCURACY(entity);
                }
                else
                    where << ",exists=0";
                Log() << "BENCH_WHERE," << where.str() << '\n';
                detail = where.str();
                return StepResult::Done;
            }
            if (step.op == "blood_pools")
            {
                // The pool natives (handlers 0xF89DD4 and 0x140C238); the
                // meaning of p1 and p3 is not known, the bench logs what it
                // passed.
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                char text[128]{};
                if (step.args.size() == 4)
                {
                    float p1 = 0.0f;
                    float size = 0.0f;
                    float p3 = 0.0f;
                    ArgFloat(step, 1, p1);
                    ArgFloat(step, 2, size);
                    ArgFloat(step, 3, p3);
                    GRAPHICS::_ADD_BLOOD_POOLS_FOR_PED_WITH_PARAMS(ped, p1, size, p3);
                    std::snprintf(text, sizeof(text), "with_params=1,p1=%.3f,size=%.3f,p3=%.3f", p1, size, p3);
                }
                else
                {
                    GRAPHICS::_ADD_BLOOD_POOLS_FOR_PED(ped);
                    std::snprintf(text, sizeof(text), "with_params=0");
                }
                detail = text;
                Log() << "BENCH_BLOOD_POOLS,ped=" << step.args[0] << "," << text << '\n';
                return StepResult::Done;
            }
            // Fire on a ped or horse, put out, and the charred bit alone. Each
            // logs the burning and charred state read back.
            if (step.op == "fire_ped" || step.op == "stop_fire" || step.op == "scorch")
            {
                const Ped ped = ResolvePed(step.args[0]);
                if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
                {
                    detail = "unknown_ped";
                    return StepResult::Failed;
                }
                char text[160]{};
                if (step.op == "fire_ped")
                {
                    float intensity = 1.0f;
                    int flags = 0;
                    if (step.args.size() > 1)
                        ArgFloat(step, 1, intensity);
                    if (step.args.size() > 2)
                        ArgInt(step, 2, flags);
                    FIRE::START_ENTITY_FIRE(ped, intensity, -1, flags);
                    std::snprintf(text, sizeof(text), "intensity=%.2f,flags=%d,", intensity, flags);
                }
                else if (step.op == "stop_fire")
                {
                    FIRE::STOP_ENTITY_FIRE(ped, 0);
                }
                else
                {
                    ENTITY::SET_ENTITY_RENDER_SCORCHED(ped, step.args[1] == "1" ? TRUE : FALSE);
                }
                const bool onFire = FIRE::IS_ENTITY_ON_FIRE(ped) != FALSE;
                const bool scorched = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(ped)) & 1u) != 0;
                char tail[96]{};
                std::snprintf(tail, sizeof(tail), "on_fire=%d,scorched=%d,health=%d", onFire ? 1 : 0, scorched ? 1 : 0,
                    ENTITY::GET_ENTITY_HEALTH(ped));
                detail = std::string(text) + tail;
                Log() << "BENCH_BODY_FIRE,op=" << step.op << ",ped=" << step.args[0] << "," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "explosion")
            {
                float type = 0.0f;
                float scale = 1.0f;
                ArgFloat(step, 0, type);
                ArgFloat(step, 4, scale);
                const Vector3 at = Coords(step, 1);
                FIRE::ADD_EXPLOSION(at.x, at.y, at.z, static_cast<int>(type), scale, TRUE, FALSE, 1.0f);
                return StepResult::Done;
            }
            if (step.op == "mem_snap")
            {
                // See MemSnap.
                const Entity entity = ResolveSpawned(step.args[0]);
                if (!entity || !ENTITY::DOES_ENTITY_EXIST(entity))
                {
                    detail = "unknown_entity";
                    return StepResult::Failed;
                }
                CreateDirectoryA(REPLAY_BURN_SNAP_DIR, nullptr);
                CreateDirectoryA(REPLAY_BURN_SNAP_DIR "\\snap", nullptr);
                const std::string file = REPLAY_BURN_SNAP_DIR "\\snap\\" + step.args[1] + "-" + step.args[0] + ".bin";
                std::size_t total = 0;
                const ULONGLONG started = GetTickCount64();
                const int regions = MemSnap(getScriptHandleBaseAddress(entity), file, total, !step.args[1].empty() && step.args[1][0] == 'd');
                const bool onFire = FIRE::IS_ENTITY_ON_FIRE(entity) != FALSE;
                const bool scorched = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(entity)) & 1u) != 0;
                Log() << "BENCH_MEM_SNAP,id=" << step.args[0] << ",tag=" << step.args[1] << ",regions=" << regions << ",bytes=" << total
                      << ",ms=" << (GetTickCount64() - started) << ",on_fire=" << (onFire ? 1 : 0) << ",scorched=" << (scorched ? 1 : 0)
                      << ",health=" << ENTITY::GET_ENTITY_HEALTH(entity) << ",dead=" << (ENTITY::IS_ENTITY_DEAD(entity) ? 1 : 0) << '\n';
                detail = "regions=" + std::to_string(regions);
                return regions > 0 ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "restage")
            {
                // New stage objects at the same places: what the recording left
                // on the old ones (projected scorch) goes with them.
                int rebuilt = 0;
                for (auto& [id, entity] : g_stage)
                {
                    const auto spec = g_stageSpecs.find(id);
                    if (spec == g_stageSpecs.end())
                        continue;
                    if (!STREAMING::HAS_MODEL_LOADED(spec->second.model))
                    {
                        STREAMING::REQUEST_MODEL(spec->second.model, FALSE);
                        return now - g_run.stepStartedAt < 10000 ? StepResult::Pending : StepResult::Failed;
                    }
                    if (ENTITY::DOES_ENTITY_EXIST(entity))
                    {
                        Entity old = entity;
                        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(old, TRUE, TRUE);
                        ENTITY::DELETE_ENTITY(&old);
                    }
                    const Vector3 at = spec->second.at;
                    Object object = OBJECT::CREATE_OBJECT(spec->second.model, at.x, at.y, at.z, FALSE, FALSE, FALSE, FALSE, FALSE);
                    if (!object)
                        continue;
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
                    ENTITY::SET_ENTITY_HEADING(object, spec->second.heading);
                    ENTITY::FREEZE_ENTITY_POSITION(object, TRUE);
                    entity = object;
                    ++rebuilt;
                }
                detail = "rebuilt=" + std::to_string(rebuilt) + ",stage=" + std::to_string(g_stage.size());
                return rebuilt ? StepResult::Done : StepResult::Failed;
            }
            if (step.op == "research_windows")
            {
                replay::events::SetResearchWindows(step.args[0] == "on");
                detail = "research_windows=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "fx_world")
            {
                replay::events::SetFxWorldSiteRule(step.args[0] == "on");
                detail = "fx_world=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "fx_split")
            {
                replay::events::SetFxRegisteredSplit(step.args[0] == "on");
                detail = "fx_split=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "fx_owner")
            {
                replay::events::SetFxOwnerRule(step.args[0] == "on");
                detail = "fx_owner=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "fx_replay")
            {
                replay::events::SetFxReplay(step.args[0] == "on");
                detail = "fx_replay=" + step.args[0];
                return StepResult::Done;
            }
            if (step.op == "clean_objects")
            {
                const Vector3 at = Coords(step, 0);
                float radius = 50.0f;
                ArgFloat(step, 3, radius);
                const bool decals = step.args[4] == "decals";
                static int handles[1024];
                const int count = worldGetAllObjects(handles, 1024);
                int touched = 0;
                for (int n = 0; n < count; ++n)
                {
                    if (!handles[n] || !ENTITY::DOES_ENTITY_EXIST(handles[n]))
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(handles[n], TRUE, FALSE);
                    const float d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y);
                    if (d > radius * radius)
                        continue;
                    if (decals)
                        GRAPHICS::REMOVE_DECALS_FROM_OBJECT(handles[n]);
                    else
                        ENTITY::SET_ENTITY_RENDER_SCORCHED(handles[n], FALSE);
                    ++touched;
                }
                detail = "objects=" + std::to_string(touched) + ",what=" + step.args[4];
                return StepResult::Done;
            }
            if (step.op == "clean_world")
            {
                const Vector3 at = Coords(step, 0);
                float radius = 50.0f;
                ArgFloat(step, 3, radius);
                std::uint32_t checked = 0;
                const std::uint32_t before = replay::events::RecordedDecalsAlive(checked);
                const int firesBefore = FIRE::GET_NUMBER_OF_FIRES_IN_RANGE(at.x, at.y, at.z, radius);
                FIRE::STOP_FIRE_IN_RANGE(at.x, at.y, at.z, radius);
                GRAPHICS::REMOVE_PARTICLE_FX_IN_RANGE(at.x, at.y, at.z, radius);
                GRAPHICS::REMOVE_DECALS_IN_RANGE(at.x, at.y, at.z, radius);
                MISC::CLEAR_AREA(at.x, at.y, at.z, radius, 0);
                const std::uint32_t afterNatives = replay::events::RecordedDecalsAlive(checked);
                const std::uint32_t removedById = replay::events::RemoveRecordedDecals();
                const std::uint32_t after = replay::events::RecordedDecalsAlive(checked);
                const int firesAfter = FIRE::GET_NUMBER_OF_FIRES_IN_RANGE(at.x, at.y, at.z, radius);
                detail = "recorded_decals=" + std::to_string(checked) + ",alive_before=" + std::to_string(before) +
                    ",alive_after_natives=" + std::to_string(afterNatives) + ",removed_by_id=" + std::to_string(removedById) +
                    ",alive_after=" + std::to_string(after) + ",fires_before=" + std::to_string(firesBefore) +
                    ",fires_after=" + std::to_string(firesAfter);
                return StepResult::Done;
            }
            if (step.op == "fire_entity")
            {
                const Hash model = MISC::GET_HASH_KEY(step.args[0].c_str());
                const Vector3 at = Coords(step, 1);
                float intensity = 1.0f;
                ArgFloat(step, 4, intensity);
                static int handles[1024];
                const int count = worldGetAllObjects(handles, 1024);
                Entity best = 0;
                float bestDistance = 25.0f;
                for (int n = 0; n < count; ++n)
                {
                    if (!handles[n] || static_cast<Hash>(ENTITY::GET_ENTITY_MODEL(handles[n])) != model)
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(handles[n], TRUE, FALSE);
                    const float d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z);
                    if (d < bestDistance)
                    {
                        bestDistance = d;
                        best = handles[n];
                    }
                }
                if (!best)
                {
                    detail = "no_object";
                    return StepResult::Failed;
                }
                FIRE::START_ENTITY_FIRE(best, intensity, -1, 0);
                detail = "handle=" + std::to_string(best) + ",on_fire=" + std::to_string(FIRE::IS_ENTITY_ON_FIRE(best) ? 1 : 0);
                return StepResult::Done;
            }
            // A wheel off or a door broken on a spawned vehicle with the
            // natives the game's scripts use (the wheel piece stays in the
            // world as a spawned entity "<veh>_wheel<n>", deleted with the
            // rest), and the lost parts read back: wheel slots
            // (_IS_VEHICLE_WHEEL_DESTROYED takes a slot) and doors.
            if (step.op == "vehicle_look")
            {
                const Entity vehicle = ResolveSpawned(step.args[0]);
                if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle) || !ENTITY::IS_ENTITY_A_VEHICLE(vehicle))
                {
                    detail = "unknown_vehicle";
                    return StepResult::Failed;
                }
                int tint = -1;
                int livery = -1;
                ArgInt(step, 1, tint);
                ArgInt(step, 2, livery);
                const int tintBefore = VEHICLE::_GET_VEHICLE_TINT(vehicle);
                const int liveryBefore = VEHICLE::_GET_VEHICLE_LIVERY(vehicle);
                if (tint >= 0)
                    VEHICLE::_SET_VEHICLE_TINT(vehicle, tint);
                if (livery >= 0)
                    VEHICLE::_SET_VEHICLE_LIVERY(vehicle, livery);
                char text[128]{};
                std::snprintf(text, sizeof(text), "tint_before=%d,tint=%d,tint_after=%d,livery_before=%d,livery=%d,livery_after=%d",
                    tintBefore, tint, VEHICLE::_GET_VEHICLE_TINT(vehicle), liveryBefore, livery, VEHICLE::_GET_VEHICLE_LIVERY(vehicle));
                detail = text;
                Log() << "BENCH_VEHICLE_LOOK,veh=" << step.args[0] << "," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "vehicle_part")
            {
                const Entity vehicle = ResolveSpawned(step.args[0]);
                if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle) || !ENTITY::IS_ENTITY_A_VEHICLE(vehicle))
                {
                    detail = "unknown_vehicle";
                    return StepResult::Failed;
                }
                int index = 0;
                ArgInt(step, 2, index);
                std::string action = "log";
                if (step.args[1] == "wheel")
                {
                    const Entity piece = VEHICLE::_BREAK_OFF_VEHICLE_WHEEL(vehicle, index);
                    if (piece && ENTITY::DOES_ENTITY_EXIST(piece))
                        g_spawned[step.args[0] + "_wheel" + std::to_string(index)] = piece;
                    action = "wheel" + std::to_string(index) + ",piece=" + std::to_string(piece);
                }
                else if (step.args[1] == "door")
                {
                    VEHICLE::SET_VEHICLE_DOOR_BROKEN(vehicle, index, FALSE);
                    action = "door" + std::to_string(index);
                }
                unsigned slotsOff = 0;
                for (int slot = 0; slot < 10; ++slot)
                {
                    if (VEHICLE::_IS_VEHICLE_WHEEL_DESTROYED(vehicle, slot))
                        slotsOff |= 1u << slot;
                }
                unsigned doorsOff = 0;
                for (int door = 0; door < 10; ++door)
                {
                    if (VEHICLE::_IS_VEHICLE_DOOR_BROKEN(vehicle, door))
                        doorsOff |= 1u << door;
                }
                char tail[96]{};
                std::snprintf(tail, sizeof(tail), ",wheel_slots_off=0x%x,doors_off=0x%x,any_wheel_off=%d", slotsOff, doorsOff,
                    VEHICLE::_ARE_ANY_VEHICLE_WHEELS_DESTROYED(vehicle) ? 1 : 0);
                detail = "action=" + action + tail;
                Log() << "BENCH_VEHICLE_PART,veh=" << step.args[0] << "," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "fire_forget")
            {
                // The fires lit so far become fires the start detour never saw
                // (the camp fires lit before the mod loaded), for the untracked
                // initial path.
                std::uint32_t engineFires = 0;
                std::uint32_t engineRemnants = 0;
                replay::fire::EngineLiveCounts(engineFires, engineRemnants);
                const int forgotten = replay::fire::ForgetLiveFires();
                detail = "forgotten=" + std::to_string(forgotten) + ",engine_fires=" + std::to_string(engineFires);
                Log() << "BENCH_FIRE_FORGET," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "fire_untracked")
            {
                // The ini key for this session, so the scenario does not depend
                // on REPLAY_INI_EXTRA.
                replay::events::SetFireUntrackedInitial(step.args[0] == "on");
                detail = "fire_untracked=" + step.args[0];
                Log() << "BENCH_FIRE_UNTRACKED," << detail << '\n';
                return StepResult::Done;
            }
            if (step.op == "fire")
            {
                const Vector3 at = Coords(step, 0);
                const FireId fire = FIRE::START_SCRIPT_FIRE(at.x, at.y, at.z, 25, 1.0f, FALSE, "", 1.0f, 0);
                detail = "fire_id=" + std::to_string(fire);
                return StepResult::Done;
            }
            if (step.op == "clear")
            {
                DeleteSpawned();
                return StepResult::Done;
            }
            detail = "unhandled_op";
            return StepResult::Failed;
        }
    }

    void Attach(Context& context, Hooks& hooks)
    {
        g_context = &context;
        g_hooks = &hooks;
        g_shot = OpenShotShared();
        g_ready = false;
        g_run = Run{};
        g_spawned.clear();
        g_stage.clear();
        g_benchPropSets.clear();
    }

    int OwnCamera() { return g_topCam; }

    int ScrubOverride(bool* fine)
    {
        if (fine)
            *fine = g_run.scrubFine;
        return g_run.active ? g_run.scrubDirection : 0;
    }

    void ApplyPuppetVelocity()
    {
        if (!g_puppetVelocity || !g_context->transport.Playing())
            return;
        const std::uint64_t now = GetTickCount64();
        const bool logNow = now >= g_puppetVelocityLogAt;
        if (logNow)
            g_puppetVelocityLogAt = now + 500;
        for (const replay::playback::Puppet& puppet : g_context->puppets.list)
        {
            if ((puppet.packet.kind != replay::entity::EntityKind::Ped && puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                !puppet.clone || !ENTITY::DOES_ENTITY_EXIST(puppet.clone))
                continue;
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
            PuppetMotion& motion = g_puppetMotion[puppet.clone];
            if (motion.at && now > motion.at)
            {
                const float dt = static_cast<float>(now - motion.at) / 1000.0f;
                const float vx = (at.x - motion.last.x) / dt;
                const float vy = (at.y - motion.last.y) / dt;
                const float vz = (at.z - motion.last.z) / dt;
                const float before = ENTITY::GET_ENTITY_SPEED(puppet.clone);
                ENTITY::SET_ENTITY_VELOCITY(puppet.clone, vx, vy, vz);
                if (logNow && std::sqrt(vx * vx + vy * vy) > 1.0f)
                    Log() << "BENCH_PUPPET_VELOCITY,id=" << replay::entity::IdText(puppet.packet.id) << ",set=" << std::sqrt(vx * vx + vy * vy + vz * vz)
                          << ",speed_before=" << before << ",speed_after=" << ENTITY::GET_ENTITY_SPEED(puppet.clone) << '\n';
            }
            motion.last = at;
            motion.at = now;
        }
    }

    // The horse puppets with their collision on (and, with nofreeze, not
    // frozen) while a clip plays, to test whether the recording's 50-100 Hz
    // stride hump is a physical contact of the hooves. Bench only.
    void ApplyPuppetCollision()
    {
        if (!g_puppetCollision || !g_context->transport.Playing())
            return;
        const std::uint64_t now = GetTickCount64();
        const bool logNow = now >= g_puppetCollisionLogAt;
        if (logNow)
            g_puppetCollisionLogAt = now + 2000;
        for (const replay::playback::Puppet& puppet : g_context->puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Horse || !puppet.clone || !ENTITY::DOES_ENTITY_EXIST(puppet.clone))
                continue;
            ENTITY::SET_ENTITY_COLLISION(puppet.clone, TRUE, TRUE);
            if (g_puppetCollision == 2)
                ENTITY::FREEZE_ENTITY_POSITION(puppet.clone, FALSE);
            if (logNow)
                Log() << "BENCH_PUPPET_COLLISION,id=" << replay::entity::IdText(puppet.packet.id) << ",mode=" << g_puppetCollision
                      << ",disabled=" << (ENTITY::GET_ENTITY_COLLISION_DISABLED(puppet.clone) ? 1 : 0)
                      << ",frozen=" << (ENTITY::_IS_ENTITY_FROZEN(puppet.clone) ? 1 : 0) << '\n';
        }
    }

    void Update(std::uint64_t now)
    {
        REPLAY_PROFILE_ZONE("tick.bench"); // the bench's own share of the tick
        if (!g_context)
            return;
        ApplyPuppetVelocity();
        ApplyPuppetCollision();
        UpdateDoorWatches(now);
        UpdateIplWatches(now);
        if (g_lootWatchUntil && now < g_lootWatchUntil)
            replay::objects::BenchLootWatch(g_lootWatchCenter, g_lootWatchRadius);
        if (g_lootUseUntil && now < g_lootUseUntil && now >= g_lootUseNextLog)
        {
            // The player's scenario and the container's ransack state, one
            // line per change.
            g_lootUseNextLog = now + 100;
            const Ped player = PLAYER::PLAYER_PED_ID();
            const Entity box = g_lootTarget;
            const bool boxed = box && ENTITY::DOES_ENTITY_EXIST(box);
            std::ostringstream state;
            state << "using=" << (PED::IS_PED_USING_ANY_SCENARIO(player) ? 1 : 0)
                  << ",point=" << TASK::_GET_SCENARIO_POINT_PED_IS_USING(player, TRUE)
                  << ",point_type=0x" << std::hex << static_cast<unsigned>(TASK::_GET_SCENARIO_POINT_TYPE_PED_IS_USING(player)) << std::dec
                  << ",opening=" << (boxed ? TASK::_GET_RANSACK_SCENARIO_CONTAINER_OPENING_STATE(box) : -1)
                  << ",open=" << (boxed ? TASK::_GET_RANSACK_SCENARIO_CONTAINER_NUM_OPEN_COMPARTMENTS(box) : -1)
                  << ",loot_left=" << (boxed ? TASK::_GET_RANSACK_SCENARIO_CONTAINER_REMAINING_LOOT_COUNT(box) : -1)
                  << ",held=0x" << std::hex << static_cast<unsigned>(WEAPON::_GET_PED_CURRENT_HELD_WEAPON(player)) << std::dec;
            if (state.str() != g_lootUseLast)
            {
                g_lootUseLast = state.str();
                Log() << "BENCH_LOOT_USE_TICK,ms=" << (now - g_lootUseStarted) << "," << g_lootUseLast
                      << ",rec_ms=" << replay::pose::capture::NowMs() << '\n';
            }
        }
        ApplyDofProbe();
        if (g_envLocked)
            KeepWorldEmpty(now);
        HoldProbeRopes();
        for (std::size_t n = 0; n < g_held.size();)
        {
            if (now >= g_held[n].until || !g_run.active)
            {
                g_held.erase(g_held.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, g_held[n].action, 1.0f);
            ++n;
        }
        if (!g_ready)
        {
            if (now < g_nextReadyPoll)
                return;
            g_nextReadyPoll = now + kReadyPollMs;
            if (!PlayerReady())
                return;
            g_ready = true;
            Log() << "BENCH_READY,inbox=" << kInboxDir << '\n';
            Log().flush();
        }

        if (!g_run.active)
        {
            if (now < g_nextInboxPoll)
                return;
            g_nextInboxPoll = now + kInboxPollMs;
            StartScenario(now);
            return;
        }

        if (GetFileAttributesA(kAbortFile) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileA(kAbortFile);
            g_held.clear();
            Finish("aborted", "abort_file");
            return;
        }
        if (now < g_run.waitUntil)
            return;
        if (g_run.next >= g_run.script.steps.size())
        {
            Finish("passed", "");
            return;
        }

        const Step& step = g_run.script.steps[g_run.next];
        if (!g_run.stepPending)
            g_run.stepStartedAt = now;
        std::string detail;
        const StepResult result = Execute(step, now, detail);
        if (result == StepResult::Pending)
        {
            g_run.stepPending = true;
            return;
        }
        g_run.stepPending = false;
        Log() << "BENCH_STEP"
              << ",i=" << g_run.next
              << ",line=" << step.line
              << ",op=" << step.op
              << ",ok=" << (result == StepResult::Done ? 1 : 0)
              << ",t_ms=" << (now - g_run.startedAt);
        if (!detail.empty())
            Log() << "," << detail;
        Log() << '\n';
        Log().flush();
        ++g_run.next;
        if (result == StepResult::Failed)
            Finish("failed", step.op);
    }
}
