#include "replay_bench_internal.h"

namespace replay::bench
{

    namespace
    {

        constexpr const char* kInboxDir = REPLAY_BENCH_DIR "\\inbox\\";
        // The host drops this file to end the running scenario at once.
        constexpr const char* kAbortFile = REPLAY_BENCH_DIR "\\abort";
        constexpr std::uint64_t kReadyPollMs = 500;
        constexpr std::uint64_t kInboxPollMs = 1000;
        // A scenario is only accepted fresh from the host; one left behind by
        // a crashed session must never run by itself in a later game.
        constexpr std::uint64_t kMaxScenarioAgeMs = 30000;
    }
        ShotShared* g_shot = nullptr;

    namespace
    {
        LONG64 g_shotPending = 0;
    }
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
        std::vector<RecordedMark> g_recMarks;
        std::vector<HeldControl> g_held;
        std::vector<ProbeRope> g_probeRopes;

    namespace
    {

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
    }
        std::vector<std::string> g_camArgs;
        std::size_t g_playMark = 0;
        bool g_playMarkShooting = false;

        Context* g_context = nullptr;
        // Frozen puppets have no physical speed, and the vegetation modifier
        // only grows with speed.
        bool g_puppetVelocity = false;
        int g_puppetCollision = 0; // 0 off, 1 collision, 2 collision and not frozen

    namespace
    {
        std::uint64_t g_puppetCollisionLogAt = 0;
    }
        std::map<Entity, PuppetMotion> g_puppetMotion;

    namespace
    {
        std::uint64_t g_puppetVelocityLogAt = 0;
    }
        Hooks* g_hooks = nullptr;

    namespace
    {

        bool g_ready = false;
        std::uint64_t g_nextReadyPoll = 0;
        std::uint64_t g_nextInboxPoll = 0;
    }
        Run g_run;
        MoonSeek g_moonSeek;

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

    namespace
    {
        constexpr int kSurveyPointsPerTick = 150;
    }
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

    namespace
    {
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
    }

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
            // (burn::RecordTree, the root at +0; node +0x08 right (tagged),
            // +0x10 left, +0x20 entity, +0x28 int key; the record lives in the
            // node: +0x141 flags read by _IS_ENTITY_CONSUMED_BY_FIRE,
            // RESURRECT_PED removes it through the tree's removal). Named
            // "FR0", "FR1"...
            {
                std::uint8_t raw[8]{};
                std::uintptr_t root = 0;
                const std::uintptr_t tree = replay::burn::RecordTree();
                if (tree && GuardedCopyBytes(raw, reinterpret_cast<const void*>(tree), 8))
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
            // & 0x1FFFF minus the index base, then the table: engine_sigs.h;
            // stride 0x148, tagged pointers; ped_blood.cpp): damage, surface
            // and the rest.
            {
                std::uint8_t raw[8]{};
                std::uint32_t handle = 0;
                std::uint32_t indexBase = 0;
                std::uintptr_t table = 0;
                const std::uintptr_t baseAt = replay::sig::Find(replay::enginesig::kComponentBaseIndex);
                const std::uintptr_t tableAt = replay::sig::Find(replay::enginesig::kComponentTable);
                if (GuardedCopyBytes(raw, static_cast<const std::uint8_t*>(entity) + 0x9C, 4))
                    std::memcpy(&handle, raw, 4);
                if (baseAt && GuardedCopyBytes(raw, reinterpret_cast<const void*>(baseAt), 4))
                    std::memcpy(&indexBase, raw, 4);
                if (tableAt && GuardedCopyBytes(raw, reinterpret_cast<const void*>(tableAt), 8))
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
        std::map<std::string, StageSpec> g_stageSpecs; // to rebuild the stage (restage)

        std::ostream& Log() { return g_context->log; }

    namespace
    {

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

    namespace
    {

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
        std::vector<DoorWatch> g_doorWatches;
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

    namespace
    {

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
    }
        DofProbe g_dofProbe;

    namespace
    {

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

    namespace
    {

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
    }

        bool g_envLocked = false;

    namespace
    {
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
