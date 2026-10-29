#pragma once

// The rules of the clip's map state (IPLs and interior entity sets), pure so
// the core tests cover them. The recording keeps the enabled IPLs (from the
// candidates of ipl_candidates.h) and each interior's entity sets; playback
// applies them and restores what it found on exit. The natives live in
// replay_world.cpp.

#include "replay_recording.h"
#include "world_notice.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace replay::world
{
    enum WorldFlags : std::uint8_t
    {
        kFlagCampRead = 1u << 0,      // the camp global was read and held 0..8
        kFlagCampConfirmed = 1u << 1, // the camp's own IPLs were on and no other camp's
        kFlagCampFromIpls = 1u << 2,  // the global failed: the camp is the one whose IPLs were on
        kFlagMission = 1u << 3,       // a mission ran at the recording start (GET_MISSION_FLAG)
        kFlagGuarma = 1u << 4,        // recorded in Guarma (camp 4): another world, warn only
    };

    constexpr std::uint32_t Joaat(const char* text)
    {
        std::uint32_t h = 0;
        for (; *text; ++text)
        {
            char c = *text;
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            h += static_cast<std::uint8_t>(c);
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }

    // FNV-1a over a list of hashes (the candidate list, a fingerprint).
    inline std::uint32_t Fnv32(const std::uint32_t* values, std::size_t count)
    {
        std::uint32_t h = 0x811C9DC5u;
        for (std::size_t n = 0; n < count; ++n)
        {
            for (int b = 0; b < 4; ++b)
            {
                h ^= (values[n] >> (b * 8)) & 0xFFu;
                h *= 0x01000193u;
            }
        }
        return h;
    }

    // The camp IPLs the game's long_update sets for the current camp (the
    // unconditional literals of its 9 x 12 camp table): on
    // for the camp in Global_40.f_4283 and off for the others. Camps 4
    // (Guarma), 5 (Lakay) and 8 (Beecher's Hope) have none there.
    struct CampSignature
    {
        std::uint8_t camp;
        std::uint32_t hash;
    };
    inline constexpr CampSignature kCampSignatures[] = {
        {0, Joaat("COL_01_CHIMNEY_SMOKE_EFFECT")}, {0, Joaat("COL_02_CHIMNEY_SMOKE_EFFECT")},
        {1, 288413571u}, {1, static_cast<std::uint32_t>(-441619793)}, {1, Joaat("HSO_NORMAL_SEATS")},
        {1, static_cast<std::uint32_t>(-80564929)}, {1, static_cast<std::uint32_t>(-676881895)},
        {1, static_cast<std::uint32_t>(-1445186253)}, {1, static_cast<std::uint32_t>(-1893724593)},
        {2, 1706275010u}, {2, Joaat("DEWCLM_NORMAL_SEATS")}, {2, 1734859244u}, {2, 1309652195u},
        {2, static_cast<std::uint32_t>(-1291679096)}, {2, 1676971154u}, {2, static_cast<std::uint32_t>(-1841279810)},
        {3, 923572416u}, {3, Joaat("SHB_NORMAL_SEATS")}, {3, static_cast<std::uint32_t>(-1828030290)},
        {3, static_cast<std::uint32_t>(-960337247)}, {3, static_cast<std::uint32_t>(-299265919)},
        {6, 1561231200u}, {6, static_cast<std::uint32_t>(-407402757)}, {6, static_cast<std::uint32_t>(-1849681615)},
        {6, static_cast<std::uint32_t>(-981684452)}, {6, 1547347496u}, {6, static_cast<std::uint32_t>(-1966238128)},
        {6, static_cast<std::uint32_t>(-149795084)}, {6, static_cast<std::uint32_t>(-419935200)},
        {6, static_cast<std::uint32_t>(-1496155572)},
        {7, 764025611u},
    };

    struct CampVerdict
    {
        std::uint8_t camp = kCampUnknown;
        std::uint8_t chapter = kChapterUnknown;
        std::uint8_t flags = 0;
        int campsWithIpls = 0; // how many camps had one of their IPLs on
    };

    // The camp from the global (when it could be read) checked against the
    // camp IPLs that are on (`activeByCamp[c]`: how many of camp c's
    // signature IPLs are on). An earlier camp's IPLs stay on after the gang
    // moved on (seen in the second epilogue: global 8 with Shady Belle's
    // camp IPLs still on), so only a LATER
    // camp's IPLs contradict the global (unknown: better no chapter than a
    // wrong one). The global's own IPLs on confirm it; none on (a camp
    // turned off by a mission, or Beecher's Hope, which has no signature)
    // keeps it, unconfirmed.
    inline CampVerdict JudgeCamp(bool globalRead, int globalValue, const int (&activeByCamp)[9])
    {
        CampVerdict verdict;
        int only = -1;
        for (int c = 0; c < 9; ++c)
        {
            if (activeByCamp[c] > 0)
            {
                ++verdict.campsWithIpls;
                only = c;
            }
        }
        if (globalRead && globalValue >= 0 && globalValue <= 8)
        {
            verdict.flags |= kFlagCampRead;
            bool later = false;
            for (int c = globalValue + 1; c < 9; ++c)
                later = later || activeByCamp[c] > 0;
            if (!later)
            {
                verdict.camp = static_cast<std::uint8_t>(globalValue);
                if (activeByCamp[globalValue] > 0)
                    verdict.flags |= kFlagCampConfirmed;
            }
        }
        else if (verdict.campsWithIpls == 1)
        {
            verdict.camp = static_cast<std::uint8_t>(only);
            verdict.flags |= kFlagCampFromIpls;
        }
        verdict.chapter = ChapterFromCamp(verdict.camp);
        if (verdict.camp == kCampGuarma)
            verdict.flags |= kFlagGuarma;
        return verdict;
    }

    // ---------------- State at a time ----------------

    // The IPLs on at timeMs (sorted): every hash whose last event at or
    // before timeMs is "on".
    inline std::vector<std::uint32_t> IplsOnAt(const WorldState& world, std::uint32_t timeMs)
    {
        std::map<std::uint32_t, bool> state;
        for (const IplChange& change : world.ipls)
        {
            if (change.timeMs > timeMs)
                break;
            state[change.hash] = change.active;
        }
        std::vector<std::uint32_t> on;
        for (const auto& [hash, active] : state)
            if (active)
                on.push_back(hash);
        return on;
    }

    inline bool IplOnAt(const WorldState& world, std::uint32_t hash, std::uint32_t timeMs)
    {
        bool active = false;
        for (const IplChange& change : world.ipls)
        {
            if (change.timeMs > timeMs)
                break;
            if (change.hash == hash)
                active = change.active;
        }
        return active;
    }

    // An entity set's key: the interior and the name, case folded (the
    // engine hashes the name).
    inline std::string SetKey(std::uint16_t interior, const char* name)
    {
        std::string key = std::to_string(interior) + ":";
        for (const char* c = name; *c; ++c)
            key += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        return key;
    }

    // Every entity set of the clip with its state at timeMs: the last event
    // at or before it, else its first event (found later in the clip).
    struct SetState
    {
        std::uint16_t interior = 0;
        const char* name = "";
        bool active = false;
    };
    inline std::vector<SetState> EntitySetsAt(const WorldState& world, std::uint32_t timeMs)
    {
        std::map<std::string, std::size_t> index;
        std::vector<SetState> out;
        for (const EntitySetChange& change : world.entitySets)
        {
            const std::string key = SetKey(change.interior, change.name);
            auto found = index.find(key);
            if (found == index.end())
            {
                index.emplace(key, out.size());
                out.push_back({change.interior, change.name, change.active});
                continue;
            }
            if (change.timeMs <= timeMs)
                out[found->second].active = change.active;
        }
        return out;
    }

    // How many state changes happen at or before timeMs after the clip
    // start (both kinds): playback applies again when this moves (play
    // across an event, a seek either way).
    inline std::size_t TimelineIndex(const WorldState& world, std::uint32_t timeMs)
    {
        std::size_t count = 0;
        for (const IplChange& change : world.ipls)
            count += change.timeMs > 0 && change.timeMs <= timeMs ? 1 : 0;
        for (const EntitySetChange& change : world.entitySets)
            count += change.timeMs > 0 && change.timeMs <= timeMs ? 1 : 0;
        return count;
    }

    // The IPL hashes whose state moves along the clip (an event after 0).
    inline std::vector<std::uint32_t> TimedIpls(const WorldState& world)
    {
        std::vector<std::uint32_t> hashes;
        for (const IplChange& change : world.ipls)
            if (change.timeMs > 0)
                hashes.push_back(change.hash);
        std::sort(hashes.begin(), hashes.end());
        hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        return hashes;
    }

    // A fingerprint of the map state at the clip start: the IPLs on at 0.
    inline std::uint32_t Fingerprint(const WorldState& world)
    {
        const std::vector<std::uint32_t> on = IplsOnAt(world, 0);
        return Fnv32(on.data(), on.size());
    }

    inline const IplSphere* FindSphere(const WorldState& world, std::uint32_t hash)
    {
        const auto at = std::lower_bound(world.spheres.begin(), world.spheres.end(), hash,
            [](const IplSphere& s, std::uint32_t h) { return s.hash < h; });
        return at != world.spheres.end() && at->hash == hash ? &*at : nullptr;
    }

    inline void AddSphere(WorldState& world, const IplSphere& sphere)
    {
        const auto at = std::lower_bound(world.spheres.begin(), world.spheres.end(), sphere.hash,
            [](const IplSphere& s, std::uint32_t h) { return s.hash < h; });
        if (at != world.spheres.end() && at->hash == sphere.hash)
            *at = sphere;
        else
            world.spheres.insert(at, sphere);
    }

    // ---------------- The clip's area ----------------

    struct Box
    {
        bool valid = false;
        float min[3] = {};
        float max[3] = {};
        void Extend(float x, float y, float z)
        {
            const float p[3] = {x, y, z};
            for (int n = 0; n < 3; ++n)
            {
                min[n] = valid ? (std::min)(min[n], p[n]) : p[n];
                max[n] = valid ? (std::max)(max[n], p[n]) : p[n];
            }
            valid = true;
        }
    };

    // Metres around the recorded area (entities, effects, the game camera)
    // where the clip's map state is applied (300 m; the load scene streams
    // 500 m in detail around the camera, script.cpp).
    constexpr float kAreaMarginM = 300.0f;

    // The area of the clip: its recorded area and every game camera sample
    // (a camera can stand away from the entities), grown by `margin`.
    inline Box ClipArea(const Recording& clip, float margin = kAreaMarginM)
    {
        Box box;
        if (clip.hasArea)
        {
            box.Extend(clip.areaMin[0], clip.areaMin[1], clip.areaMin[2]);
            box.Extend(clip.areaMax[0], clip.areaMax[1], clip.areaMax[2]);
        }
        for (const camera::GameSample& sample : clip.gameCamera)
            box.Extend(sample.position.x, sample.position.y, sample.position.z);
        if (!box.valid)
            return box;
        for (int n = 0; n < 3; ++n)
        {
            box.min[n] -= margin;
            box.max[n] += margin;
        }
        return box;
    }

    // A sphere touches the box (closest point within the radius).
    inline bool SphereTouches(const Box& box, const float (&center)[3], float radius)
    {
        if (!box.valid)
            return false;
        float d2 = 0.0f;
        for (int n = 0; n < 3; ++n)
        {
            const float c = (std::max)(box.min[n], (std::min)(center[n], box.max[n]));
            d2 += (center[n] - c) * (center[n] - c);
        }
        return d2 <= radius * radius;
    }

    inline bool PointInside(const Box& box, const float (&point)[3])
    {
        return SphereTouches(box, point, 0.0f);
    }

    // ---------------- Cut and trim ----------------

    // Last-wins per key: the last event at or before `base` of each key is
    // kept (at `foldTo`), the later ones up to `end` as they are (shifted by
    // `shift`). IPL "off" events folded to the base are dropped: before its
    // first event an IPL is off anyway.
    inline void FoldWorld(const WorldState& in, std::uint32_t base, std::uint32_t end, bool rebase, WorldState& out)
    {
        const auto placed = [&](std::uint32_t timeMs) { return rebase ? (timeMs <= base ? 0u : timeMs - base) : timeMs; };
        out.recorded = in.recorded;
        out.camp = in.camp;
        out.chapter = in.chapter;
        out.flags = in.flags;
        out.candidateListHash = in.candidateListHash;
        out.candidateCount = in.candidateCount;
        out.interiors = in.interiors;

        std::map<std::uint32_t, std::size_t> lastIpl;
        for (std::size_t n = 0; n < in.ipls.size(); ++n)
            if (in.ipls[n].timeMs <= base)
                lastIpl[in.ipls[n].hash] = n;
        std::vector<IplChange> ipls;
        for (std::size_t n = 0; n < in.ipls.size(); ++n)
        {
            const IplChange& change = in.ipls[n];
            if (change.timeMs > end)
                break;
            if (change.timeMs <= base)
            {
                if (lastIpl[change.hash] != n || !change.active)
                    continue;
            }
            IplChange copy = change;
            copy.timeMs = change.timeMs <= base ? (rebase ? 0u : change.timeMs) : placed(change.timeMs);
            ipls.push_back(copy);
        }
        std::stable_sort(ipls.begin(), ipls.end(), [](const IplChange& a, const IplChange& b) { return a.timeMs < b.timeMs; });
        out.ipls = std::move(ipls);

        std::map<std::string, std::size_t> lastSet;
        for (std::size_t n = 0; n < in.entitySets.size(); ++n)
            if (in.entitySets[n].timeMs <= base)
                lastSet[SetKey(in.entitySets[n].interior, in.entitySets[n].name)] = n;
        std::vector<EntitySetChange> sets;
        for (std::size_t n = 0; n < in.entitySets.size(); ++n)
        {
            const EntitySetChange& change = in.entitySets[n];
            if (change.timeMs > end)
                break;
            if (change.timeMs <= base && lastSet[SetKey(change.interior, change.name)] != n)
                continue;
            EntitySetChange copy = change;
            copy.timeMs = change.timeMs <= base ? (rebase ? 0u : change.timeMs) : placed(change.timeMs);
            sets.push_back(copy);
        }
        std::stable_sort(sets.begin(), sets.end(),
            [](const EntitySetChange& a, const EntitySetChange& b) { return a.timeMs < b.timeMs; });
        out.entitySets = std::move(sets);

        out.spheres.clear();
        for (const IplSphere& sphere : in.spheres)
        {
            const bool used = std::any_of(out.ipls.begin(), out.ipls.end(),
                [&](const IplChange& c) { return c.hash == sphere.hash; });
            if (used)
                out.spheres.push_back(sphere);
        }
    }

    // recording_window::Cut: the state at base folded to the clip start.
    inline void CutWorld(const WorldState& in, std::uint32_t base, std::uint32_t end, WorldState& out)
    {
        FoldWorld(in, base, end, true, out);
    }

    // replay_buffer::TrimBefore: the events before the ring's head go, the
    // state in force at the head stays (at its own time: the recording
    // clock keeps running). Returns how many events went.
    inline std::size_t TrimWorldBefore(WorldState& world, std::uint32_t headMs)
    {
        const std::size_t before = world.ipls.size() + world.entitySets.size();
        WorldState trimmed;
        FoldWorld(world, headMs, 0xFFFFFFFFu, false, trimmed);
        world.ipls = std::move(trimmed.ipls);
        world.entitySets = std::move(trimmed.entitySets);
        world.spheres = std::move(trimmed.spheres);
        const std::size_t after = world.ipls.size() + world.entitySets.size();
        return before > after ? before - after : 0;
    }

    // ---------------- Playback backup ----------------

    // What the playback changed, to give back (and to persist for a crash
    // or a reload): the state found and the state applied.
    struct IplBackup
    {
        std::uint32_t hash = 0;
        bool was = false;
        bool applied = false;
    };
    struct SetBackup
    {
        std::uint32_t interiorHash = 0;
        float position[3] = {};
        char name[64] = {};
        bool was = false;
        bool applied = false;
    };

    // Restore decision after a reload: only what still stands as the replay
    // left it (a save loaded since then rebuilt the map from its own flags).
    inline bool ShouldRestore(bool current, bool applied, bool was)
    {
        return current == applied && current != was;
    }
}
