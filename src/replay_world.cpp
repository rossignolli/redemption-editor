#include "replay_world.h"
#include "ipl_candidates.h"

#include "main.h"
#include "natives.h"

#include <windows.h>

#include "storage_paths.h" // the base folder of the state file

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace replay::world
{
    namespace
    {
        // The current camp, Global_40.f_4283 in the decompiled
        // scripts (build 1436): long_update's func_439 returns it and passes
        // it as the camp index of its 9 x 12 camp IPL table (func_1660);
        // the camp scripts name 0 to 3, 5 and 6 (CAMP_COLTER ... ). Global
        // slot 40 + 4283. Not proven on 1491.50 (no .data in the dump): the
        // read is guarded and checked against the camp IPLs (JudgeCamp).
        constexpr int kCampGlobal = 40 + 4283;

        constexpr std::uint32_t kPassIntervalMs = 2000;
        constexpr std::size_t kSlice = 64;          // candidates per tick while a pass runs
        constexpr std::size_t kSetSlice = 32;       // entity set names validated per tick
        constexpr std::size_t kMaxInteriors = 16;
        constexpr std::size_t kMaxIplEvents = 20000;
        constexpr std::size_t kMaxSetEvents = 4096;
        constexpr std::size_t kLogHashes = 12;      // hashes per log line

        std::uint32_t CandidateListHash()
        {
            static const std::uint32_t hash = Fnv32(kIplCandidates, kIplCandidateCount);
            return hash;
        }

        double NowUs()
        {
            static LARGE_INTEGER frequency = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return f;
            }();
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<double>(now.QuadPart) * 1000000.0 / static_cast<double>(frequency.QuadPart);
        }

        // Guarded read of the camp global (rung 2): false when the pointer
        // is null or the read faults.
        bool ReadCampGlobal(int& value)
        {
            __try
            {
                UINT64* slot = getGlobalPtr(kCampGlobal);
                if (!slot)
                    return false;
                value = static_cast<int>(*reinterpret_cast<volatile std::int32_t*>(slot));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        struct CampReading
        {
            bool globalRead = false;
            int globalValue = -1;
            int activeByCamp[9] = {};
            CampVerdict verdict;
        };

        CampReading ReadCamp()
        {
            CampReading reading;
            reading.globalRead = ReadCampGlobal(reading.globalValue);
            for (const CampSignature& signature : kCampSignatures)
                if (STREAMING::IS_IPL_ACTIVE_HASH(signature.hash))
                    ++reading.activeByCamp[signature.camp];
            reading.verdict = JudgeCamp(reading.globalRead, reading.globalValue, reading.activeByCamp);
            return reading;
        }

        void LogCamp(std::ostream& log, const CampReading& reading)
        {
            log << "global_read=" << (reading.globalRead ? 1 : 0) << ",global=" << reading.globalValue << ",camp_ipls=";
            for (int c = 0; c < 9; ++c)
                log << (c ? ";" : "") << reading.activeByCamp[c];
            char chapter[24]{};
            ChapterName(reading.verdict.chapter, chapter, sizeof(chapter));
            log << ",camp=" << static_cast<int>(reading.verdict.camp) << ",camp_name=" << CampName(reading.verdict.camp)
                << ",chapter=" << static_cast<int>(reading.verdict.chapter) << ",chapter_name=" << (chapter[0] ? chapter : "unknown")
                << ",flags=0x" << std::hex << static_cast<int>(reading.verdict.flags) << std::dec;
        }

        std::string HashList(const std::vector<std::uint32_t>& hashes)
        {
            std::ostringstream text;
            text << std::hex;
            for (std::size_t n = 0; n < hashes.size() && n < kLogHashes; ++n)
                text << (n ? ";" : "") << "0x" << hashes[n];
            if (hashes.size() > kLogHashes)
                text << ";...";
            return text.str();
        }

        // ---------------- Recording state ----------------

        struct RecInterior
        {
            Interior handle = 0;
            std::uint16_t index = 0;   // into WorldState::interiors
            std::size_t validateCursor = 0;
            bool validated = false;
            std::vector<std::uint16_t> names; // indices into kEntitySetNames
            std::vector<std::uint8_t> state;
        };

        struct RecCost
        {
            double startScanUs = 0.0;
            double tickMaxUs = 0.0;
            double tickTotalUs = 0.0;
            std::uint64_t ticks = 0;
            std::uint32_t passes = 0;
            std::uint32_t iplChanges = 0;
            std::uint32_t setChanges = 0;
            bool iplCapped = false;
            bool setCapped = false;
        };

        std::vector<std::uint8_t> g_known; // per candidate: on at the last check
        std::size_t g_cursor = 0;
        bool g_passActive = false;
        std::uint32_t g_nextPassMs = 0;
        std::vector<RecInterior> g_recInteriors;
        RecCost g_cost;
        bool g_recording = false;

        void AddSphereOf(WorldState& world, std::uint32_t hash)
        {
            if (FindSphere(world, hash))
                return;
            Vector3 center{};
            float radius = 0.0f;
            if (!STREAMING::_GET_IPL_BOUNDING_SPHERE(static_cast<Hash>(hash), &center, &radius) || radius <= 0.0f)
                return;
            AddSphere(world, {hash, {center.x, center.y, center.z}, radius});
        }

        void AddIplChange(Recording& clip, std::uint32_t nowMs, std::uint32_t hash, bool active)
        {
            if (clip.world.ipls.size() >= kMaxIplEvents)
            {
                g_cost.iplCapped = true;
                return;
            }
            clip.world.ipls.push_back({nowMs, hash, active});
            if (active)
                AddSphereOf(clip.world, hash);
        }

        void AddSetChange(Recording& clip, std::uint32_t nowMs, std::uint16_t interior, const char* name, bool active)
        {
            if (clip.world.entitySets.size() >= kMaxSetEvents)
            {
                g_cost.setCapped = true;
                return;
            }
            EntitySetChange change;
            change.timeMs = nowMs;
            change.interior = interior;
            change.active = active;
            std::snprintf(change.name, sizeof(change.name), "%s", name);
            clip.world.entitySets.push_back(change);
        }

        // The interior at a point, added once (identity: name hash and
        // position, since a streamed-in interior gets a new handle).
        void FindInterior(Recording& clip, const float (&at)[3], std::ostream& log)
        {
            const Interior interior = INTERIOR::GET_INTERIOR_AT_COORDS(at[0], at[1], at[2]);
            if (!interior || !INTERIOR::IS_VALID_INTERIOR(interior))
                return;
            for (const RecInterior& known : g_recInteriors)
                if (known.handle == interior)
                    return;
            Vector3 position{};
            Hash nameHash = 0;
            INTERIOR::GET_INTERIOR_LOCATION_AND_NAMEHASH(interior, &position, &nameHash);
            for (RecInterior& known : g_recInteriors)
            {
                const WorldInterior& seen = clip.world.interiors[known.index];
                const float dx = seen.position[0] - position.x, dy = seen.position[1] - position.y,
                            dz = seen.position[2] - position.z;
                if (seen.nameHash == nameHash && dx * dx + dy * dy + dz * dz < 4.0f)
                {
                    known.handle = interior;
                    return;
                }
            }
            if (g_recInteriors.size() >= kMaxInteriors || clip.world.interiors.size() >= 0xFFFF)
                return;
            RecInterior entry;
            entry.handle = interior;
            entry.index = static_cast<std::uint16_t>(clip.world.interiors.size());
            clip.world.interiors.push_back({static_cast<std::uint32_t>(nameHash), {position.x, position.y, position.z}});
            g_recInteriors.push_back(entry);
            log << "WORLD_STATE,phase=record_interior,index=" << entry.index << ",name_hash=0x" << std::hex << nameHash
                << std::dec << ",pos=" << position.x << ";" << position.y << ";" << position.z << '\n';
        }

        // A slice of the name validation of the first interior not yet
        // validated: a valid name's state is recorded when it is found.
        void ValidateSlice(Recording& clip, std::uint32_t nowMs, std::ostream& log)
        {
            for (RecInterior& interior : g_recInteriors)
            {
                if (interior.validated)
                    continue;
                if (!INTERIOR::IS_VALID_INTERIOR(interior.handle))
                    return;
                const std::size_t end = (std::min)(kEntitySetNameCount, interior.validateCursor + kSetSlice);
                for (std::size_t n = interior.validateCursor; n < end; ++n)
                {
                    const char* name = kEntitySetNames[n];
                    if (!INTERIOR::_IS_INTERIOR_ENTITY_SET_VALID(interior.handle, name))
                        continue;
                    const bool active = INTERIOR::IS_INTERIOR_ENTITY_SET_ACTIVE(interior.handle, name) != FALSE;
                    interior.names.push_back(static_cast<std::uint16_t>(n));
                    interior.state.push_back(active ? 1 : 0);
                    AddSetChange(clip, nowMs, interior.index, name, active);
                }
                interior.validateCursor = end;
                if (end >= kEntitySetNameCount)
                {
                    interior.validated = true;
                    std::ostringstream active;
                    int on = 0;
                    for (std::size_t k = 0; k < interior.names.size(); ++k)
                    {
                        if (!interior.state[k])
                            continue;
                        active << (on ? ";" : "") << kEntitySetNames[interior.names[k]];
                        if (++on >= static_cast<int>(kLogHashes))
                            break;
                    }
                    log << "WORLD_STATE,phase=record_sets,interior=" << interior.index << ",valid=" << interior.names.size()
                        << ",active=" << active.str() << '\n';
                }
                return; // one interior per tick
            }
        }

        void CheckSets(Recording& clip, std::uint32_t nowMs)
        {
            for (RecInterior& interior : g_recInteriors)
            {
                if (!interior.validated || !INTERIOR::IS_VALID_INTERIOR(interior.handle))
                    continue;
                for (std::size_t k = 0; k < interior.names.size(); ++k)
                {
                    const char* name = kEntitySetNames[interior.names[k]];
                    const std::uint8_t active = INTERIOR::IS_INTERIOR_ENTITY_SET_ACTIVE(interior.handle, name) ? 1 : 0;
                    if (active == interior.state[k])
                        continue;
                    interior.state[k] = active;
                    ++g_cost.setChanges;
                    AddSetChange(clip, nowMs, interior.index, name, active != 0);
                }
            }
        }

        // ---------------- Playback state ----------------

        constexpr const char* kBackupFile = "RedemptionEditor.world-ipl";
        // In the base folder (storage_paths.h), for the next load.
        std::wstring BackupFile() { return replay::storage::Path(L"RedemptionEditor.world-ipl"); }
        constexpr std::uint64_t kVerifyMs = 1000;
        constexpr std::uint64_t kInteriorRetryMs = 500;
        constexpr int kMaxReapplies = 3;

        struct Play
        {
            bool active = false;
            bool dry = false; // differences counted, nothing applied
            const char* reason = "none";
            bool listMatch = false;
            Box area;
            std::vector<IplBackup> ipls;
            std::vector<SetBackup> sets;
            std::map<std::uint32_t, bool> inArea; // per IPL hash
            std::vector<std::uint32_t> timed;
            std::size_t timelineIndex = 0;
            std::vector<Interior> interiors;     // per clip interior, 0 unresolved
            std::vector<std::uint8_t> interiorChanged;
            std::uint64_t nextVerifyAt = 0;
            std::uint64_t nextInteriorRetryAt = 0;
            std::map<std::uint32_t, int> reapplies;
            std::uint32_t noSphere = 0;
            std::uint32_t outside = 0;
            std::uint32_t unresolvedSets = 0;
            std::uint32_t timelineApplies = 0;
            std::uint32_t reverts = 0;
        };
        Play g_play;
        // The IPL churn around a clip start, for STREAM_PRESSURE (Pressure()).
        Pressure g_pressure;
        Notice g_notice;
        bool g_restoreLeftPending = true;

        void WriteBackupFile()
        {
            FILE* file = _wfopen(BackupFile().c_str(), L"w");
            if (!file)
                return;
            std::fprintf(file, "WORLDIPL 1\n");
            for (const IplBackup& b : g_play.ipls)
                std::fprintf(file, "I %u %d %d\n", b.hash, b.was ? 1 : 0, b.applied ? 1 : 0);
            for (const SetBackup& b : g_play.sets)
                std::fprintf(file, "S %u %.3f %.3f %.3f %d %d %s\n", b.interiorHash, b.position[0], b.position[1],
                    b.position[2], b.was ? 1 : 0, b.applied ? 1 : 0, b.name);
            std::fclose(file);
        }

        bool IplInArea(const WorldState& world, std::uint32_t hash)
        {
            const auto cached = g_play.inArea.find(hash);
            if (cached != g_play.inArea.end())
                return cached->second;
            bool inside = false;
            if (const IplSphere* sphere = FindSphere(world, hash))
                inside = SphereTouches(g_play.area, sphere->center, sphere->radius);
            else
            {
                Vector3 center{};
                float radius = 0.0f;
                if (STREAMING::_GET_IPL_BOUNDING_SPHERE(static_cast<Hash>(hash), &center, &radius) && radius > 0.0f)
                {
                    const float c[3] = {center.x, center.y, center.z};
                    inside = SphereTouches(g_play.area, c, radius);
                }
                else
                    ++g_play.noSphere;
            }
            if (!inside)
                ++g_play.outside;
            g_play.inArea.emplace(hash, inside);
            return inside;
        }

        IplBackup& BackupIpl(std::uint32_t hash, bool was)
        {
            for (IplBackup& b : g_play.ipls)
                if (b.hash == hash)
                    return b;
            g_play.ipls.push_back({hash, was, was});
            return g_play.ipls.back();
        }

        SetBackup& BackupSet(const WorldInterior& interior, const char* name, bool was)
        {
            for (SetBackup& b : g_play.sets)
                if (b.interiorHash == interior.nameHash && _stricmp(b.name, name) == 0 &&
                    std::fabs(b.position[0] - interior.position[0]) < 1.0f && std::fabs(b.position[1] - interior.position[1]) < 1.0f)
                    return b;
            SetBackup b;
            b.interiorHash = interior.nameHash;
            std::memcpy(b.position, interior.position, sizeof(b.position));
            std::snprintf(b.name, sizeof(b.name), "%s", name);
            b.was = b.applied = was;
            g_play.sets.push_back(b);
            return g_play.sets.back();
        }

        Interior ResolveInterior(std::uint32_t nameHash, const float (&position)[3])
        {
            Interior interior = INTERIOR::GET_INTERIOR_AT_COORDS_WITH_TYPEHASH(position[0], position[1], position[2],
                static_cast<Hash>(nameHash));
            if (interior && INTERIOR::IS_VALID_INTERIOR(interior))
                return interior;
            interior = INTERIOR::GET_INTERIOR_AT_COORDS(position[0], position[1], position[2]);
            if (!interior || !INTERIOR::IS_VALID_INTERIOR(interior))
                return 0;
            Vector3 at{};
            Hash hash = 0;
            INTERIOR::GET_INTERIOR_LOCATION_AND_NAMEHASH(interior, &at, &hash);
            return static_cast<std::uint32_t>(hash) == nameHash ? interior : 0;
        }

        void SetIpl(std::uint32_t hash, bool on)
        {
            ++g_pressure.iplToggles;
            if (on)
                STREAMING::REQUEST_IPL_HASH(static_cast<Hash>(hash));
            else
                STREAMING::REMOVE_IPL_HASH(static_cast<Hash>(hash));
        }

        void SetEntitySet(Interior interior, const char* name, bool on)
        {
            if (on)
                INTERIOR::ACTIVATE_INTERIOR_ENTITY_SET(interior, name, 0);
            else
                INTERIOR::DEACTIVATE_INTERIOR_ENTITY_SET(interior, name, TRUE); // as the game scripts call it
        }

        struct ApplyCount
        {
            std::uint32_t differences = 0;
            std::uint32_t applied = 0;
            std::vector<std::uint32_t> on;
            std::vector<std::uint32_t> off;
            std::uint32_t setsOn = 0;
            std::uint32_t setsOff = 0;
        };

        // The clip's entity sets at timeMs on the interiors that resolve now.
        void ApplySets(const WorldState& world, std::uint32_t timeMs, ApplyCount& count)
        {
            g_play.unresolvedSets = 0;
            struct Pending
            {
                Interior interior;
                const char* name;
                bool on;
            };
            std::vector<Pending> pending;
            for (const SetState& state : EntitySetsAt(world, timeMs))
            {
                if (state.interior >= world.interiors.size())
                    continue;
                const WorldInterior& interior = world.interiors[state.interior];
                if (!PointInside(g_play.area, interior.position))
                    continue;
                Interior& handle = g_play.interiors[state.interior];
                if (!handle || !INTERIOR::IS_VALID_INTERIOR(handle))
                    handle = ResolveInterior(interior.nameHash, interior.position);
                if (!handle)
                {
                    ++g_play.unresolvedSets;
                    continue;
                }
                const bool current = INTERIOR::IS_INTERIOR_ENTITY_SET_ACTIVE(handle, state.name) != FALSE;
                if (current == state.active)
                    continue;
                ++count.differences;
                if (g_play.dry)
                    continue;
                BackupSet(interior, state.name, current).applied = state.active;
                g_play.interiorChanged[state.interior] = 1;
                pending.push_back({handle, state.name, state.active});
            }
            if (pending.empty())
                return;
            WriteBackupFile(); // before touching the world: a crash finds what to give back
            for (const Pending& p : pending)
            {
                SetEntitySet(p.interior, p.name, p.on);
                ++count.applied;
                ++(p.on ? count.setsOn : count.setsOff);
            }
        }

        // The clip's IPL state at timeMs inside the area: `full` checks
        // every candidate and every hash of the clip (the start); otherwise
        // only the hashes that move along the clip and the ones touched.
        void ApplyIpls(const WorldState& world, std::uint32_t timeMs, bool full, ApplyCount& count)
        {
            const std::vector<std::uint32_t> on = IplsOnAt(world, timeMs);
            std::vector<std::uint32_t> hashes;
            if (full)
            {
                if (g_play.listMatch)
                    hashes.assign(kIplCandidates, kIplCandidates + kIplCandidateCount);
                for (const IplChange& change : world.ipls)
                    hashes.push_back(change.hash);
            }
            else
            {
                hashes = g_play.timed;
                for (const IplBackup& b : g_play.ipls)
                    hashes.push_back(b.hash);
            }
            std::sort(hashes.begin(), hashes.end());
            hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
            std::vector<std::pair<std::uint32_t, bool>> pending;
            for (const std::uint32_t hash : hashes)
            {
                if (!IplInArea(world, hash))
                    continue;
                const bool desired = std::binary_search(on.begin(), on.end(), hash);
                const bool current = STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(hash)) != FALSE;
                if (current == desired)
                    continue;
                ++count.differences;
                if (g_play.dry)
                    continue;
                BackupIpl(hash, current).applied = desired;
                pending.emplace_back(hash, desired);
            }
            if (pending.empty())
                return;
            WriteBackupFile();
            for (const auto& [hash, desired] : pending)
            {
                SetIpl(hash, desired);
                ++count.applied;
                (desired ? count.on : count.off).push_back(hash);
            }
        }

        void LogApply(std::ostream& log, const char* phase, std::uint32_t timeMs, const ApplyCount& count)
        {
            log << "WORLD_STATE,phase=" << phase << ",t_ms=" << timeMs << ",dry=" << (g_play.dry ? 1 : 0)
                << ",reason=" << g_play.reason << ",differences=" << count.differences << ",applied=" << count.applied
                << ",ipl_on=" << count.on.size() << ",ipl_off=" << count.off.size() << ",sets_on=" << count.setsOn
                << ",sets_off=" << count.setsOff << ",on=" << HashList(count.on) << ",off=" << HashList(count.off)
                << ",outside_area=" << g_play.outside << ",no_sphere=" << g_play.noSphere
                << ",unresolved_sets=" << g_play.unresolvedSets << '\n';
        }

        // Gives back what the backup holds. `onlyIfUnchanged`: after a
        // reload, only what still stands as the replay left it.
        void RestoreBackup(const std::vector<IplBackup>& ipls, const std::vector<SetBackup>& sets, bool onlyIfUnchanged,
            std::ostream& log, const char* phase)
        {
            std::vector<std::uint32_t> restored, movedOn;
            for (auto it = ipls.rbegin(); it != ipls.rend(); ++it)
            {
                const bool current = STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(it->hash)) != FALSE;
                if (current == it->was)
                    continue;
                if (onlyIfUnchanged && !ShouldRestore(current, it->applied, it->was))
                {
                    movedOn.push_back(it->hash);
                    continue;
                }
                SetIpl(it->hash, it->was);
                restored.push_back(it->hash);
            }
            g_pressure.lastRestoreIpls = static_cast<std::uint32_t>(restored.size());
            int setsRestored = 0, setsUnresolved = 0, setsMovedOn = 0;
            for (auto it = sets.rbegin(); it != sets.rend(); ++it)
            {
                const Interior interior = ResolveInterior(it->interiorHash, it->position);
                if (!interior)
                {
                    ++setsUnresolved;
                    continue;
                }
                const bool current = INTERIOR::IS_INTERIOR_ENTITY_SET_ACTIVE(interior, it->name) != FALSE;
                if (current == it->was)
                    continue;
                if (onlyIfUnchanged && !ShouldRestore(current, it->applied, it->was))
                {
                    ++setsMovedOn;
                    continue;
                }
                SetEntitySet(interior, it->name, it->was);
                ++setsRestored;
            }
            log << "WORLD_STATE,phase=" << phase << ",ipls=" << ipls.size() << ",ipl_restored=" << restored.size()
                << ",ipl_moved_on=" << movedOn.size() << ",sets=" << sets.size() << ",sets_restored=" << setsRestored
                << ",sets_moved_on=" << setsMovedOn << ",sets_unresolved=" << setsUnresolved
                << ",restored=" << HashList(restored) << ",moved_on=" << HashList(movedOn) << '\n';
        }
    }

    // ---------------- Recording ----------------

    void BeginRecording(Recording& clip, std::ostream& log)
    {
        WorldState& world = clip.world;
        world = WorldState{};
        g_known.assign(kIplCandidateCount, 0);
        g_cursor = 0;
        g_passActive = false;
        g_nextPassMs = kPassIntervalMs;
        g_recInteriors.clear();
        g_cost = RecCost{};
        g_recording = true;

        const double started = NowUs();
        std::size_t active = 0;
        for (std::size_t n = 0; n < kIplCandidateCount; ++n)
        {
            const bool on = STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(kIplCandidates[n])) != FALSE;
            g_known[n] = on ? 1 : 0;
            if (on)
            {
                ++active;
                world.ipls.push_back({0, kIplCandidates[n], true});
            }
        }
        const double scanned = NowUs();
        for (const IplChange& change : world.ipls)
            AddSphereOf(world, change.hash);
        const double spheres = NowUs();
        g_cost.startScanUs = scanned - started;

        const CampReading camp = ReadCamp();
        world.camp = camp.verdict.camp;
        world.chapter = camp.verdict.chapter;
        world.flags = camp.verdict.flags | (MISC::GET_MISSION_FLAG() ? kFlagMission : 0);
        world.candidateListHash = CandidateListHash();
        world.candidateCount = static_cast<std::uint32_t>(kIplCandidateCount);
        world.recorded = true;

        log << "WORLD_STATE,phase=record_start,candidates=" << kIplCandidateCount << ",active=" << active
            << ",spheres=" << world.spheres.size() << ",scan_us=" << static_cast<int>(g_cost.startScanUs)
            << ",sphere_us=" << static_cast<int>(spheres - scanned) << ",fingerprint=0x" << std::hex << Fingerprint(world)
            << ",list=0x" << CandidateListHash() << std::dec << ",mission=" << ((world.flags & kFlagMission) ? 1 : 0) << ",";
        LogCamp(log, camp);
        log << '\n';
    }

    void UpdateRecording(Recording& clip, std::uint32_t nowMs, const float (&player)[3], const float (&camera)[3],
        std::ostream& log)
    {
        if (!g_recording || !clip.world.recorded)
            return;
        const double started = NowUs();
        if (!g_passActive && nowMs >= g_nextPassMs)
        {
            g_passActive = true;
            g_cursor = 0;
            g_nextPassMs = nowMs + kPassIntervalMs;
            ++g_cost.passes;
            FindInterior(clip, player, log);
            FindInterior(clip, camera, log);
            CheckSets(clip, nowMs);
            // The camp moves with the story (a long buffer session).
            int value = -1;
            if (ReadCampGlobal(value) && value >= 0 && value <= 8 && value != clip.world.camp)
            {
                const CampReading camp = ReadCamp();
                if (camp.verdict.camp != clip.world.camp)
                {
                    clip.world.camp = camp.verdict.camp;
                    clip.world.chapter = camp.verdict.chapter;
                    clip.world.flags = static_cast<std::uint8_t>((clip.world.flags & kFlagMission) | camp.verdict.flags);
                    log << "WORLD_STATE,phase=record_camp,t_ms=" << nowMs << ",";
                    LogCamp(log, camp);
                    log << '\n';
                }
            }
        }
        if (g_passActive)
        {
            const std::size_t end = (std::min)(kIplCandidateCount, g_cursor + kSlice);
            for (std::size_t n = g_cursor; n < end; ++n)
            {
                const std::uint8_t on = STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(kIplCandidates[n])) ? 1 : 0;
                if (on == g_known[n])
                    continue;
                g_known[n] = on;
                ++g_cost.iplChanges;
                AddIplChange(clip, nowMs, kIplCandidates[n], on != 0);
                if (g_cost.iplChanges <= 64)
                    log << "WORLD_STATE,phase=record_change,t_ms=" << nowMs << ",hash=0x" << std::hex << kIplCandidates[n]
                        << std::dec << ",active=" << static_cast<int>(on) << '\n';
            }
            g_cursor = end;
            if (g_cursor >= kIplCandidateCount)
                g_passActive = false;
        }
        ValidateSlice(clip, nowMs, log);
        const double spent = NowUs() - started;
        g_cost.tickMaxUs = (std::max)(g_cost.tickMaxUs, spent);
        g_cost.tickTotalUs += spent;
        ++g_cost.ticks;
    }

    void EndRecording(const Recording& clip, std::ostream& log)
    {
        if (!g_recording)
            return;
        g_recording = false;
        log << "WORLD_STATE,phase=record_end,ipl_events=" << clip.world.ipls.size() << ",ipl_changes=" << g_cost.iplChanges
            << ",set_events=" << clip.world.entitySets.size() << ",set_changes=" << g_cost.setChanges
            << ",interiors=" << clip.world.interiors.size() << ",spheres=" << clip.world.spheres.size()
            << ",passes=" << g_cost.passes << ",start_scan_us=" << static_cast<int>(g_cost.startScanUs)
            << ",tick_max_us=" << static_cast<int>(g_cost.tickMaxUs) << ",tick_avg_us="
            << (g_cost.ticks ? static_cast<int>(g_cost.tickTotalUs / static_cast<double>(g_cost.ticks)) : 0)
            << ",ticks=" << g_cost.ticks << ",ipl_capped=" << (g_cost.iplCapped ? 1 : 0)
            << ",set_capped=" << (g_cost.setCapped ? 1 : 0) << ",camp=" << static_cast<int>(clip.world.camp)
            << ",chapter=" << static_cast<int>(clip.world.chapter) << '\n';
        g_recInteriors.clear();
    }

    // ---------------- Playback ----------------

    void BeginPlayback(const Recording& clip, const PlaybackConfig& config, std::ostream& log)
    {
        if (g_play.active)
            EndPlayback(log);
        g_play = Play{};
        const WorldState& world = clip.world;
        const CampReading now = ReadCamp();
        Notice notice;
        notice.sequence = g_notice.sequence;
        notice.recordedChapter = world.recorded ? world.chapter : kChapterUnknown;
        notice.currentChapter = now.verdict.chapter;
        if (!world.recorded)
        {
            g_notice = notice;
            log << "WORLD_STATE,phase=apply,recorded=0\n";
            return;
        }
        g_play.active = true;
        g_play.area = ClipArea(clip);
        g_play.listMatch = world.candidateListHash == CandidateListHash();
        g_play.timed = TimedIpls(world);
        g_play.interiors.assign(world.interiors.size(), 0);
        g_play.interiorChanged.assign(world.interiors.size(), 0);
        const bool guarmaMismatch = (world.camp == kCampGuarma) != (now.verdict.camp == kCampGuarma) &&
            world.camp != kCampUnknown && now.verdict.camp != kCampUnknown;
        if (!config.restore)
            g_play.reason = "ini_off";
        else if (MISC::GET_MISSION_FLAG() && !config.inMission)
            g_play.reason = "mission";
        else if (guarmaMismatch)
            g_play.reason = "guarma";
        else if (!g_play.area.valid)
            g_play.reason = "no_area";
        g_play.dry = std::strcmp(g_play.reason, "none") != 0;

        const double started = NowUs();
        ApplyCount count;
        ApplyIpls(world, 0, true, count);
        ApplySets(world, 0, count);
        g_pressure.lastApplyOn = static_cast<std::uint32_t>(count.on.size());
        g_pressure.lastApplyOff = static_cast<std::uint32_t>(count.off.size());
        g_pressure.lastApplySets = count.setsOn + count.setsOff;
        g_pressure.lastApplyAtMs = GetTickCount64();
        ++g_pressure.applies;
        g_play.timelineIndex = TimelineIndex(world, 0);
        g_play.nextVerifyAt = 0;

        notice.differences = count.differences;
        notice.applied = count.applied;
        char text[160]{};
        if (NoticeText(notice.recordedChapter, notice.currentChapter, notice.differences, count.applied > 0, text, sizeof(text)))
        {
            notice.kind = count.applied > 0 ? kNoticeRestored : kNoticeMayDiffer;
            ++notice.sequence;
        }
        g_notice = notice;
        LogApply(log, "apply", 0, count);
        log << "WORLD_STATE,phase=apply_info,us=" << static_cast<int>(NowUs() - started) << ",list_match="
            << (g_play.listMatch ? 1 : 0) << ",timed=" << g_play.timed.size() << ",recorded_camp="
            << static_cast<int>(world.camp) << ",recorded_chapter=" << static_cast<int>(world.chapter)
            << ",recorded_fingerprint=0x" << std::hex << Fingerprint(world) << std::dec << ",notice=" << notice.kind
            << ",text=" << (text[0] ? text : "none") << ",now_";
        LogCamp(log, now);
        log << '\n';
        if (guarmaMismatch)
            log << "WORLD_STATE,phase=guarma,recorded_camp=" << static_cast<int>(world.camp)
                << ",current_camp=" << static_cast<int>(now.verdict.camp) << ",action=warn_only\n";
    }

    void UpdatePlayback(const Recording& clip, std::uint32_t playheadMs, std::uint64_t nowMs, std::ostream& log)
    {
        if (!g_play.active || g_play.dry)
            return;
        const WorldState& world = clip.world;
        const std::size_t index = TimelineIndex(world, playheadMs);
        if (index != g_play.timelineIndex)
        {
            g_play.timelineIndex = index;
            ApplyCount count;
            ApplyIpls(world, playheadMs, false, count);
            ApplySets(world, playheadMs, count);
            if (++g_play.timelineApplies <= 32)
                LogApply(log, "timeline", playheadMs, count);
        }
        if (nowMs >= g_play.nextVerifyAt)
        {
            g_play.nextVerifyAt = nowMs + kVerifyMs;
            for (const IplBackup& b : g_play.ipls)
            {
                const bool current = STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(b.hash)) != FALSE;
                if (current == b.applied)
                    continue;
                int& count = g_play.reapplies[b.hash];
                ++g_play.reverts;
                log << "WORLD_STATE,phase=reverted,hash=0x" << std::hex << b.hash << std::dec << ",applied="
                    << (b.applied ? 1 : 0) << ",now=" << (current ? 1 : 0) << ",reapply=" << (count < kMaxReapplies ? 1 : 0)
                    << ",t_ms=" << playheadMs << '\n';
                if (count < kMaxReapplies)
                {
                    ++count;
                    SetIpl(b.hash, b.applied);
                }
            }
        }
        if (g_play.unresolvedSets && nowMs >= g_play.nextInteriorRetryAt)
        {
            g_play.nextInteriorRetryAt = nowMs + kInteriorRetryMs;
            ApplyCount count;
            ApplySets(world, playheadMs, count);
            if (count.applied)
                LogApply(log, "interior_late", playheadMs, count);
        }
    }

    Pressure CurrentPressure() { return g_pressure; }

    bool PlaybackApplied() { return g_play.active; }

    bool Ready()
    {
        if (!g_play.active || g_play.dry)
            return true;
        for (std::size_t n = 0; n < g_play.interiors.size(); ++n)
        {
            if (!g_play.interiorChanged[n] || !g_play.interiors[n])
                continue;
            if (!INTERIOR::IS_INTERIOR_READY(g_play.interiors[n]))
                return false;
        }
        return true;
    }

    void EndPlayback(std::ostream& log)
    {
        if (!g_play.active)
            return;
        if (!g_play.ipls.empty() || !g_play.sets.empty())
            RestoreBackup(g_play.ipls, g_play.sets, false, log, "restore");
        else
            log << "WORLD_STATE,phase=restore,ipls=0,sets=0,reverts=" << g_play.reverts << '\n';
        _wremove(BackupFile().c_str());
        g_play = Play{};
    }

    bool ForgetPlaybackAfterRestart(std::ostream& log)
    {
        if (!g_play.active)
            return false;
        log << "WORLD_STATE,phase=forget_after_restart,ipls=" << g_play.ipls.size() << ",sets=" << g_play.sets.size()
            << ",backup_left=1\n";
        g_play = Play{};
        g_restoreLeftPending = true; // the backup file is given back after the load (ServiceRestoreLeft)
        return true;
    }

    const Notice& CurrentNotice() { return g_notice; }

    void ServiceRestoreLeft(std::ostream& log)
    {
        if (!g_restoreLeftPending)
            return;
        if (DLC::GET_IS_LOADING_SCREEN_ACTIVE() || SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
            return;
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return;
        g_restoreLeftPending = false;
        FILE* file = _wfopen(BackupFile().c_str(), L"r");
        if (!file)
            return;
        std::vector<IplBackup> ipls;
        std::vector<SetBackup> sets;
        char line[256]{};
        bool header = false;
        while (std::fgets(line, sizeof(line), file))
        {
            if (!header)
            {
                header = std::strncmp(line, "WORLDIPL 1", 10) == 0;
                if (!header)
                    break;
                continue;
            }
            int was = 0, applied = 0;
            if (line[0] == 'I')
            {
                unsigned hash = 0;
                if (std::sscanf(line, "I %u %d %d", &hash, &was, &applied) == 3)
                    ipls.push_back({hash, was != 0, applied != 0});
            }
            else if (line[0] == 'S')
            {
                SetBackup b;
                unsigned hash = 0;
                char name[64]{};
                if (std::sscanf(line, "S %u %f %f %f %d %d %63s", &hash, &b.position[0], &b.position[1], &b.position[2], &was,
                        &applied, name) == 7)
                {
                    b.interiorHash = hash;
                    b.was = was != 0;
                    b.applied = applied != 0;
                    std::snprintf(b.name, sizeof(b.name), "%s", name);
                    sets.push_back(b);
                }
            }
        }
        std::fclose(file);
        _wremove(BackupFile().c_str());
        if (!header)
        {
            log << "WORLD_STATE,phase=restore_after_reload,error=bad_file\n";
            return;
        }
        RestoreBackup(ipls, sets, true, log, "restore_after_reload");
    }

    // ---------------- Bench ----------------

    void Probe(std::ostream& log)
    {
        const double started = NowUs();
        std::vector<std::uint32_t> on;
        for (std::size_t n = 0; n < kIplCandidateCount; ++n)
            if (STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(kIplCandidates[n])))
                on.push_back(kIplCandidates[n]);
        const double scanned = NowUs();
        const CampReading camp = ReadCamp();
        log << "WORLD_PROBE,candidates=" << kIplCandidateCount << ",direct=" << kIplDirectLiteralCount
            << ",active=" << on.size() << ",scan_us=" << static_cast<int>(scanned - started)
            << ",per_call_ns=" << (kIplCandidateCount ? static_cast<int>((scanned - started) * 1000.0 / kIplCandidateCount) : 0)
            << ",fingerprint=0x" << std::hex << Fnv32(on.data(), on.size()) << ",list=0x" << CandidateListHash() << std::dec
            << ",mission=" << (MISC::GET_MISSION_FLAG() ? 1 : 0) << ",camp_global_index=" << kCampGlobal << ",";
        LogCamp(log, camp);
        log << ",active_hashes=" << HashList(on) << '\n';
        // The full active list, 16 per line, for comparing two probes.
        for (std::size_t n = 0; n < on.size(); n += 16)
        {
            log << "WORLD_PROBE_ACTIVE,from=" << n << ",hashes=" << std::hex;
            for (std::size_t k = n; k < on.size() && k < n + 16; ++k)
                log << (k > n ? ";" : "") << on[k];
            log << std::dec << '\n';
        }
        const Ped player = PLAYER::PLAYER_PED_ID();
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        const Interior interior = INTERIOR::GET_INTERIOR_AT_COORDS(at.x, at.y, at.z);
        if (!interior || !INTERIOR::IS_VALID_INTERIOR(interior))
        {
            log << "WORLD_PROBE_INTERIOR,found=0\n";
            return;
        }
        Vector3 position{};
        Hash nameHash = 0;
        INTERIOR::GET_INTERIOR_LOCATION_AND_NAMEHASH(interior, &position, &nameHash);
        const double setsStarted = NowUs();
        std::ostringstream valid;
        int validCount = 0, activeCount = 0;
        for (std::size_t n = 0; n < kEntitySetNameCount; ++n)
        {
            if (!INTERIOR::_IS_INTERIOR_ENTITY_SET_VALID(interior, kEntitySetNames[n]))
                continue;
            const bool active = INTERIOR::IS_INTERIOR_ENTITY_SET_ACTIVE(interior, kEntitySetNames[n]) != FALSE;
            valid << (validCount ? ";" : "") << kEntitySetNames[n] << "=" << (active ? 1 : 0);
            ++validCount;
            activeCount += active ? 1 : 0;
        }
        log << "WORLD_PROBE_INTERIOR,found=1,handle=" << interior << ",name_hash=0x" << std::hex << nameHash << std::dec
            << ",pos=" << position.x << ";" << position.y << ";" << position.z << ",ready="
            << (INTERIOR::IS_INTERIOR_READY(interior) ? 1 : 0) << ",names_tested=" << kEntitySetNameCount
            << ",valid=" << validCount << ",active=" << activeCount << ",sets_us=" << static_cast<int>(NowUs() - setsStarted)
            << ",sets=" << valid.str() << '\n';
    }
}
