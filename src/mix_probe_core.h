#pragma once

// The mix probe, pure part (unit tested in ReplayCoreTests,
// tests/mix_probe_tests.cpp).
//
// Why the replay can sound weaker than live ("no base", gunshots and horse
// hooves weaker, most in gunfights). Six suspects, each answered from what the
// probe records in both passes (record and play):
// 1. listener: the distance from the sound's owner to the rendered camera
//    (and to the live player ped, and the engine's own distance metric of
//    the owner's ped audio) differs between the passes;
// 2. player treatment: the engine treats the player's sounds apart (the
//    player bit of the component row slot +0xB8, byte +0x40; the weapon
//    audio predicate 0x2F02F8C and the mount predicate 0x8B69B0, which the
//    replay's detours answer for the player's puppet and its horse; the
//    movement audio 0x31815C4 reads the bit directly, not covered);
// 3. variant: the engine picks another sound for the same owner and event
//    (the NPC weapon sound chosen by distance);
// 4. audio LOD: the ped audio holder's LOD (+0x45E8) drops components
//    farther from the listener;
// 5. music: the game's score plays live and is stopped in the replay
//    (MusicStop=1, as a replay editor does);
// 6. count: the replay makes far fewer sounds of a role and kind (hooves
//    never created with the free camera): the five above only look at sounds
//    that exist.
//
// Variants are matched by sound set (the ref's low dword), the ped steps
// queued through S03 are steps (RequestFamilies), the LOD verdict looks both
// ways, refused requests stay out of the counts and rates, and in the replay a
// weapon's holder comes from the clip's WeaponPoint attachment (mix_probe.cpp).
//
// Each sound the census sees with a recorded owner is one Sample (owner,
// role, kind, ref, distances, LOD, player flags); once a second a Listener
// (camera, live player and main character positions, music). The recording
// keeps its samples in the clip (section MIXP, optional, written only when
// the probe ran), cut and trimmed with the clip. At the end of a playback
// Compare puts both side by side over the clip time the playback covered.
// Read only: nothing here changes what the game plays.

#include "sound_owner_table.h" // Family, FamilyOfCaller, kWetHoofSet, census::kNoOwner

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace replay::mixprobe
{
    // ------------------------------------------------------------ roles and kinds

    enum class Role : std::uint8_t
    {
        Main = 0,       // the recording's player (live) / its puppet (play)
        MainHorse = 1,  // the horse the main character rides
        Npc = 2,        // a person
        Animal = 3,
        Horse = 4,      // any other horse
        Vehicle = 5,    // vehicles and train cars
        Object = 6,     // props
        LivePlayer = 7, // the hidden live player during a playback
        Other = 8,      // a weapon whose holder is not known
    };
    constexpr std::uint8_t kRoleCount = 9;

    inline const char* RoleName(Role role, bool play)
    {
        switch (role)
        {
        case Role::Main: return play ? "puppet_player" : "player";
        case Role::MainHorse: return "horse_player";
        case Role::Npc: return "npc";
        case Role::Animal: return "animal";
        case Role::Horse: return "horse";
        case Role::Vehicle: return "vehicle";
        case Role::Object: return "object";
        case Role::LivePlayer: return "live_player";
        case Role::Other: return "other";
        }
        return "?";
    }

    // Report heading of a role (both passes).
    inline const char* RoleTitle(Role role)
    {
        switch (role)
        {
        case Role::Main: return "player (live) / puppet_player (replay)";
        default: return RoleName(role, false);
        }
    }

    enum class Kind : std::uint8_t
    {
        Shot = 0,      // the weapon's audio entity (fire, tail, reload: the gunshot family's rule)
        Footstep = 1,  // a person's or animal's step layers
        HorseStep = 2, // a horse's step layers and the wet hoof set
        Movement = 3,  // the ped movement audio (clothing, tack, hoof foley)
        Speech = 4,
        Melee = 5,
        Generic = 6,   // any other owned sound
    };
    constexpr std::uint8_t kKindCount = 7;

    inline const char* KindName(Kind kind)
    {
        static const char* const kNames[kKindCount] = {"shot", "footstep", "horse_step", "movement", "speech", "melee",
            "generic"};
        return static_cast<std::uint8_t>(kind) < kKindCount ? kNames[static_cast<std::uint8_t>(kind)] : "?";
    }

    // The kind a caller family names (false: none of the probe's kinds).
    inline bool KindOfFamily(soundowner::Family family, bool horseOwner, Kind& out)
    {
        switch (family)
        {
        case soundowner::Family::Footstep: out = horseOwner ? Kind::HorseStep : Kind::Footstep; return true;
        case soundowner::Family::HorseStep: out = horseOwner ? Kind::HorseStep : Kind::Footstep; return true;
        case soundowner::Family::Movement: out = Kind::Movement; return true;
        case soundowner::Family::Speech: out = Kind::Speech; return true;
        case soundowner::Family::Melee: out = Kind::Melee; return true;
        default: return false;
        }
    }

    // The kind of a caller path: the innermost frame inside a function of a
    // known system (sound_owner_table.h kCallerRanges, build 1491.50 .pdata
    // extents).
    inline bool KindOfCallers(const std::uint32_t* callers, std::size_t count, bool horseOwner, Kind& out)
    {
        for (std::size_t n = 0; callers && n < count; ++n)
        {
            soundowner::Family family = soundowner::Family::Owned;
            if (soundowner::FamilyOfCaller(callers[n], family) && KindOfFamily(family, horseOwner, out))
                return true;
        }
        return false;
    }

    // The kind of a sound: the weapon's audio is a shot (the owner table's
    // rule 4); the wet hoof set is a step (a horse's, or a person's: peds make
    // 0x29934a too); else the census frames at the creator; else `queued` (the
    // family of the create-and-play request that queued this sound,
    // RequestFamilies); else generic.
    //
    // The ped steps reach the creator through the S03 create-and-play queue,
    // so the census frames are the queue's drain (0x258a469/0x2f1d23e/
    // 0x2ee86b1) and never the step's 0x318D0A0/0x318DAFE/0x318E828; the
    // generic recorder finds the family on the queued request's frames
    // (sound_generic_core.h FamilyCaller of request.callers). The probe does
    // the same through `queued`.
    inline Kind KindOf(bool weaponOwner, bool horseOwner, std::uint64_t ref, const std::uint32_t* callers,
        std::size_t count, const Kind* queued = nullptr)
    {
        if (weaponOwner)
            return Kind::Shot;
        if ((ref & 0xFFFFFFFFull) == soundowner::kWetHoofSet)
            return horseOwner ? Kind::HorseStep : Kind::Footstep;
        Kind kind = Kind::Generic;
        if (KindOfCallers(callers, count, horseOwner, kind))
            return kind;
        if (queued)
            return *queued;
        return Kind::Generic;
    }

    // The create-and-play requests (audio_observer.h CollectPlayRequestsSince:
    // the audio entity, the ref and the frames above the queue call) whose
    // frames name a known system, until the creator makes their sound (the
    // census row with the same audio entity and ref). `timeUs` is when the
    // probe collected the request (one clock for Note and Prune: the census
    // and the request ring stamp time with two different formulas); a
    // request kept longer than kRequestKeepUs is dropped unmatched.
    class RequestFamilies
    {
    public:
        static constexpr std::uint64_t kRequestKeepUs = 3000000;
        static constexpr std::size_t kMaxPending = 4096;

        void Clear()
        {
            m_pending.clear();
            noted = matched = expired = 0;
        }
        // The caller frames are kept raw: the horse rule is the owner's,
        // known only at the creator.
        void Note(std::uint64_t audio, std::uint64_t ref, std::uint64_t timeUs, const std::uint32_t* callers,
            std::size_t count)
        {
            soundowner::Family family = soundowner::Family::Owned;
            bool found = false;
            for (std::size_t n = 0; callers && n < count && !found; ++n)
            {
                Kind probe = Kind::Generic;
                found = soundowner::FamilyOfCaller(callers[n], family) && KindOfFamily(family, false, probe);
            }
            if (!found)
                return;
            if (m_pending.size() >= kMaxPending)
                m_pending.erase(m_pending.begin());
            m_pending.push_back({audio, ref, timeUs, family});
            ++noted;
        }
        // The oldest pending request of this audio entity and ref: its
        // family (consumed).
        bool Take(std::uint64_t audio, std::uint64_t ref, soundowner::Family& out)
        {
            for (auto at = m_pending.begin(); at != m_pending.end(); ++at)
            {
                if (at->audio != audio || at->ref != ref)
                    continue;
                out = at->family;
                m_pending.erase(at);
                ++matched;
                return true;
            }
            return false;
        }
        void Prune(std::uint64_t nowUs)
        {
            const auto old = std::remove_if(m_pending.begin(), m_pending.end(),
                [nowUs](const Request& p) { return nowUs > p.timeUs && nowUs - p.timeUs > kRequestKeepUs; });
            expired += static_cast<std::uint64_t>(m_pending.end() - old);
            m_pending.erase(old, m_pending.end());
        }
        std::size_t Pending() const { return m_pending.size(); }
        std::uint64_t noted = 0;
        std::uint64_t matched = 0;
        std::uint64_t expired = 0;

    private:
        struct Request
        {
            std::uint64_t audio;
            std::uint64_t ref;
            std::uint64_t timeUs;
            soundowner::Family family;
        };
        std::vector<Request> m_pending;
    };

    // ------------------------------------------------------------ samples

    constexpr std::uint8_t kNoLod = 0xFF;
    constexpr std::uint16_t kNoDistance = 0xFFFF;
    constexpr std::uint16_t kMaxDistanceDm = 0xFFFE; // 6553.4 m

    // Sample::flags.
    constexpr std::uint8_t kFlagPlayerBit = 0x01;   // the engine's player bit of the owner (shooter; a horse: its rider)
    constexpr std::uint8_t kFlagPlayerAudio = 0x02; // the weapon (mount) predicate answers "player" (engine or the replay's detour)
    constexpr std::uint8_t kFlagRefused = 0x04;     // the creator returned no sound
    constexpr std::uint8_t kFlagMine = 0x08;        // made by the mod's own call (the replay's tracks)
    constexpr std::uint8_t kFlagBitKnown = 0x10;    // the player bit was readable

    struct Sample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t owner = census::kNoOwner;  // the sound's owner (a shot: the weapon)
        std::uint32_t source = census::kNoOwner; // who makes it (a shot: the holder; else the owner)
        std::uint64_t ref = 0;
        std::uint8_t role = 0;
        std::uint8_t kind = 0;
        std::uint8_t lod = kNoLod;
        std::uint8_t flags = 0;
        std::uint16_t dCamDm = kNoDistance;      // to the rendered camera
        std::uint16_t dPlayerDm = kNoDistance;   // to the live local player ped
        std::uint16_t dListenerDm = kNoDistance; // sqrt of the ped audio's distance metric (holder +0x4640, hypothesis)

        Role GetRole() const { return static_cast<Role>(role < kRoleCount ? role : static_cast<std::uint8_t>(Role::Other)); }
        Kind GetKind() const { return static_cast<Kind>(kind < kKindCount ? kind : static_cast<std::uint8_t>(Kind::Generic)); }
    };

    inline std::uint16_t ToDm(double meters)
    {
        if (!(meters >= 0.0) || !std::isfinite(meters))
            return kNoDistance;
        const double dm = std::round(meters * 10.0);
        return dm >= kMaxDistanceDm ? kMaxDistanceDm : static_cast<std::uint16_t>(dm);
    }

    inline double FromDm(std::uint16_t dm) { return dm == kNoDistance ? -1.0 : dm / 10.0; }

    inline double Distance(const float a[3], const float b[3])
    {
        const double x = static_cast<double>(a[0]) - b[0];
        const double y = static_cast<double>(a[1]) - b[1];
        const double z = static_cast<double>(a[2]) - b[2];
        return std::sqrt(x * x + y * y + z * z);
    }

    // The ped audio holder's distance metric (+0x4640): a ped ~30 m from the
    // camera had 922 and one a bit closer 871, so it reads as the squared
    // distance to the engine's listener (hypothesis). The probe logs the raw
    // value and its square root.
    inline std::uint16_t ListenerDm(float metric)
    {
        if (!std::isfinite(metric) || metric < 0.0f)
            return kNoDistance;
        return ToDm(std::sqrt(static_cast<double>(metric)));
    }

    struct Listener
    {
        std::uint32_t timeMs = 0;
        float cam[3] = {};
        float player[3] = {};
        float main[3] = {};
        std::uint8_t hasMain = 0;
        std::uint8_t music = 0xFF; // AUDIO_IS_MUSIC_PLAYING: 0, 1, 0xFF unknown
    };

    // The recording's part (Recording::mixProbe, section MIXP).
    struct Track
    {
        std::uint32_t mainId = census::kNoOwner; // the recording's player
        std::vector<Sample> samples;     // time ascending
        std::vector<Listener> listeners; // time ascending
        std::uint32_t dropped = 0;       // samples over the per-second cap

        void Clear()
        {
            mainId = census::kNoOwner;
            samples.clear();
            listeners.clear();
            dropped = 0;
        }
        bool Empty() const { return samples.empty() && listeners.empty(); }
        std::size_t ApproxBytes() const
        {
            return samples.capacity() * sizeof(Sample) + listeners.capacity() * sizeof(Listener);
        }
        // The ring buffer drops what is older than keepFromMs.
        void DropBefore(std::uint32_t keepFromMs)
        {
            samples.erase(std::remove_if(samples.begin(), samples.end(),
                              [keepFromMs](const Sample& s) { return s.timeMs < keepFromMs; }),
                samples.end());
            listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                [keepFromMs](const Listener& l) { return l.timeMs < keepFromMs; }),
                listeners.end());
        }
        // The window [baseMs, endMs] as a clip of its own: times move by baseMs.
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs) const
        {
            out.Clear();
            out.mainId = mainId;
            out.dropped = dropped;
            for (const Sample& s : samples)
            {
                if (s.timeMs < baseMs || s.timeMs > endMs)
                    continue;
                Sample copy = s;
                copy.timeMs -= baseMs;
                out.samples.push_back(copy);
            }
            for (const Listener& l : listeners)
            {
                if (l.timeMs < baseMs || l.timeMs > endMs)
                    continue;
                Listener copy = l;
                copy.timeMs -= baseMs;
                out.listeners.push_back(copy);
            }
        }
    };

    // Samples kept per second of clip time (the rest counted, not kept):
    // a gunfight makes a few hundred sounds a second at most.
    constexpr std::uint32_t kMaxSamplesPerSecond = 160;

    class SampleBudget
    {
    public:
        void Reset()
        {
            m_second = UINT32_MAX;
            m_used = 0;
        }
        bool Take(std::uint32_t timeMs)
        {
            const std::uint32_t second = timeMs / 1000;
            if (second != m_second)
            {
                m_second = second;
                m_used = 0;
            }
            if (m_used >= kMaxSamplesPerSecond)
                return false;
            ++m_used;
            return true;
        }

    private:
        std::uint32_t m_second = UINT32_MAX;
        std::uint32_t m_used = 0;
    };

    // ------------------------------------------------------------ log lines budget

    // MIX_SOUND lines per wall second, per kind, so a FullLog file stays
    // usable (the buffer records all session long): shots first, then the
    // steps and hooves, then the rest. Every sound still counts in the
    // totals and the clip; only the line is skipped.
    class LineBudget
    {
    public:
        explicit LineBudget(bool play = false) { SetPlay(play); }
        void SetPlay(bool play)
        {
            m_play = play;
            Reset();
        }
        void Reset()
        {
            m_second = UINT64_MAX;
            for (std::uint32_t& used : m_used)
                used = 0;
            m_lines = 0;
            m_suppressed = 0;
            m_secondSuppressed = 0;
        }
        static std::uint32_t PerSecond(Kind kind, bool play)
        {
            static constexpr std::uint32_t kRecord[kKindCount] = {6, 2, 3, 1, 1, 1, 1};
            static constexpr std::uint32_t kPlay[kKindCount] = {12, 4, 6, 2, 2, 2, 2};
            const std::uint8_t n = static_cast<std::uint8_t>(kind);
            return n < kKindCount ? (play ? kPlay[n] : kRecord[n]) : 0;
        }
        // Whole pass cap: past it, one line in kLateEvery.
        static constexpr std::uint64_t kPassCap = 30000;
        static constexpr std::uint64_t kLateEvery = 50;

        bool Take(Kind kind, std::uint64_t wallMs)
        {
            const std::uint64_t second = wallMs / 1000;
            if (second != m_second)
            {
                m_second = second;
                for (std::uint32_t& used : m_used)
                    used = 0;
                m_secondSuppressed = 0;
            }
            const std::uint8_t n = static_cast<std::uint8_t>(kind) < kKindCount ? static_cast<std::uint8_t>(kind) : 0;
            bool ok = m_used[n] < PerSecond(kind, m_play);
            if (ok && m_lines >= kPassCap)
                ok = (m_suppressed % kLateEvery) == 0;
            if (!ok)
            {
                ++m_suppressed;
                ++m_secondSuppressed;
                return false;
            }
            ++m_used[n];
            ++m_lines;
            return true;
        }
        std::uint64_t Lines() const { return m_lines; }
        std::uint64_t Suppressed() const { return m_suppressed; }

    private:
        bool m_play = false;
        std::uint64_t m_second = UINT64_MAX;
        std::uint32_t m_used[kKindCount] = {};
        std::uint64_t m_lines = 0;
        std::uint64_t m_suppressed = 0;
        std::uint64_t m_secondSuppressed = 0;
    };

    // ------------------------------------------------------------ pass totals

    // Distances in 1 m bins up to 400 m (the last bin: farther).
    constexpr std::uint32_t kDistanceBins = 401;

    struct DistanceHistogram
    {
        std::vector<std::uint32_t> bins;
        std::uint64_t n = 0;
        double sum = 0.0;

        void Add(std::uint16_t dm)
        {
            if (dm == kNoDistance)
                return;
            if (bins.empty())
                bins.assign(kDistanceBins, 0);
            const std::uint32_t bin = (std::min)(static_cast<std::uint32_t>(dm / 10), kDistanceBins - 1);
            ++bins[bin];
            ++n;
            sum += dm / 10.0;
        }
        // The middle bin's centre (1 m resolution); -1 when empty.
        double Median() const
        {
            if (!n)
                return -1.0;
            const std::uint64_t half = (n + 1) / 2;
            std::uint64_t seen = 0;
            for (std::uint32_t bin = 0; bin < bins.size(); ++bin)
            {
                seen += bins[bin];
                if (seen >= half)
                    return bin + 0.5;
            }
            return kDistanceBins - 0.5;
        }
        double Mean() const { return n ? sum / static_cast<double>(n) : -1.0; }
    };

    struct Cell
    {
        std::uint64_t count = 0;
        std::uint64_t refused = 0;
        std::uint64_t playerBit = 0;
        std::uint64_t playerAudio = 0;
        std::uint64_t lod[6] = {}; // 0..4, [5] unknown
        DistanceHistogram cam;
        DistanceHistogram player;
        DistanceHistogram listener;
    };

    inline std::string LodText(const std::uint64_t (&lod)[6])
    {
        std::string text;
        char item[32];
        for (int n = 0; n < 6; ++n)
        {
            if (n < 5)
                std::snprintf(item, sizeof(item), "%s%d:%llu", n ? "|" : "", n, static_cast<unsigned long long>(lod[n]));
            else
                std::snprintf(item, sizeof(item), "|?:%llu", static_cast<unsigned long long>(lod[n]));
            text += item;
        }
        return text;
    }

    class Totals
    {
    public:
        void Clear()
        {
            m_cells.clear();
            world = 0;
            samples = 0;
        }
        void Add(const Sample& s)
        {
            Cell& cell = m_cells[Key(s.GetRole(), s.GetKind())];
            ++cell.count;
            ++samples;
            cell.refused += (s.flags & kFlagRefused) ? 1 : 0;
            cell.playerBit += (s.flags & kFlagPlayerBit) ? 1 : 0;
            cell.playerAudio += (s.flags & kFlagPlayerAudio) ? 1 : 0;
            ++cell.lod[s.lod <= 4 ? s.lod : 5];
            cell.cam.Add(s.dCamDm);
            cell.player.Add(s.dPlayerDm);
            cell.listener.Add(s.dListenerDm);
        }
        // MIX_TOTAL lines, one per role and kind with a sound.
        std::vector<std::string> Lines(const char* phase) const
        {
            std::vector<std::string> out;
            const bool play = std::string(phase) == "play";
            for (const auto& item : m_cells)
            {
                const Cell& c = item.second;
                char line[512];
                std::snprintf(line, sizeof(line),
                    "MIX_TOTAL,phase=%s,role=%s,kind=%s,count=%llu,refused=%llu,d_cam_med=%.1f,d_cam_mean=%.1f,"
                    "d_player_med=%.1f,d_lst_med=%.1f,lod=%s,pbit=%llu,player_audio=%llu",
                    phase, RoleName(static_cast<Role>(item.first / kKindCount), play),
                    KindName(static_cast<Kind>(item.first % kKindCount)), static_cast<unsigned long long>(c.count),
                    static_cast<unsigned long long>(c.refused), c.cam.Median(), c.cam.Mean(), c.player.Median(),
                    c.listener.Median(), LodText(c.lod).c_str(), static_cast<unsigned long long>(c.playerBit),
                    static_cast<unsigned long long>(c.playerAudio));
                out.push_back(line);
            }
            return out;
        }
        const Cell* Find(Role role, Kind kind) const
        {
            const auto found = m_cells.find(Key(role, kind));
            return found == m_cells.end() ? nullptr : &found->second;
        }
        std::uint64_t world = 0;   // sounds without a recorded owner (not sampled)
        std::uint64_t samples = 0; // sounds with an owner

    private:
        static std::uint32_t Key(Role role, Kind kind)
        {
            return static_cast<std::uint32_t>(role) * kKindCount + static_cast<std::uint32_t>(kind);
        }
        std::map<std::uint32_t, Cell> m_cells;
    };

    // ------------------------------------------------------------ comparison

    constexpr std::uint32_t kCoverWindowMs = 250;
    constexpr std::uint32_t kMatchToleranceMs = 150; // same event: same owner and kind within this
    constexpr std::uint32_t kMinRowSamples = 5;      // a row's medians count from this many samples per side

    // What the playback covered at normal play (windows of the clip time)
    // and how much clip time it went through (a loop counts twice).
    class Coverage
    {
    public:
        void Clear()
        {
            m_windows.clear();
            m_playMs = 0.0;
        }
        void Note(double fromMs, double toMs)
        {
            if (!(toMs >= fromMs) || fromMs < 0.0)
                return;
            const std::uint32_t first = static_cast<std::uint32_t>(fromMs) / kCoverWindowMs;
            const std::uint32_t last = static_cast<std::uint32_t>(toMs) / kCoverWindowMs;
            if (toMs - fromMs >= 1000.0)
            {
                // A jump (seek): only its ends.
                Add(first);
                Add(last);
                return;
            }
            m_playMs += toMs - fromMs;
            for (std::uint32_t w = first; w <= last; ++w)
                Add(w);
        }
        bool Contains(std::uint32_t timeMs) const
        {
            return std::binary_search(m_windows.begin(), m_windows.end(), timeMs / kCoverWindowMs);
        }
        double CoveredSeconds() const { return m_windows.size() * (kCoverWindowMs / 1000.0); }
        double PlaySeconds() const { return m_playMs / 1000.0; }
        bool Empty() const { return m_windows.empty(); }

    private:
        void Add(std::uint32_t w)
        {
            const auto at = std::lower_bound(m_windows.begin(), m_windows.end(), w);
            if (at == m_windows.end() || *at != w)
                m_windows.insert(at, w);
        }
        std::vector<std::uint32_t> m_windows; // ascending, unique
        double m_playMs = 0.0;
    };

    struct SideStats
    {
        std::uint32_t count = 0;   // sounds made (refused requests left out: they never sounded)
        std::uint32_t refused = 0; // requests the creator refused (not in the count nor the rates)
        double dCamMed = -1.0;
        double dCamMean = -1.0;
        double dPlayerMed = -1.0;
        double dListenerMed = -1.0;
        std::uint32_t lod[6] = {};
        double meanLod = -1.0;     // over known LODs
        double playerBitShare = -1.0;
        double playerAudioShare = -1.0;
        double perSecond = 0.0;
    };

    inline double MedianOf(std::vector<double>& values)
    {
        if (values.empty())
            return -1.0;
        const std::size_t middle = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
        double value = values[middle];
        if (values.size() % 2 == 0)
        {
            const double lower = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle));
            value = (value + lower) / 2.0;
        }
        return value;
    }

    template <typename Pick>
    SideStats Summarize(const std::vector<const Sample*>& samples, double seconds, Pick pick)
    {
        SideStats out;
        std::vector<double> cam, player, listener;
        double camSum = 0.0, lodSum = 0.0;
        std::uint32_t lodKnown = 0, bitKnown = 0, bits = 0, audio = 0;
        for (const Sample* s : samples)
        {
            if (!pick(*s))
                continue;
            if (s->flags & kFlagRefused)
            {
                ++out.refused;
                continue;
            }
            ++out.count;
            if (s->dCamDm != kNoDistance)
            {
                cam.push_back(FromDm(s->dCamDm));
                camSum += FromDm(s->dCamDm);
            }
            if (s->dPlayerDm != kNoDistance)
                player.push_back(FromDm(s->dPlayerDm));
            if (s->dListenerDm != kNoDistance)
                listener.push_back(FromDm(s->dListenerDm));
            ++out.lod[s->lod <= 4 ? s->lod : 5];
            if (s->lod <= 4)
            {
                lodSum += s->lod;
                ++lodKnown;
            }
            if (s->flags & kFlagBitKnown)
            {
                ++bitKnown;
                bits += (s->flags & kFlagPlayerBit) ? 1 : 0;
            }
            audio += (s->flags & kFlagPlayerAudio) ? 1 : 0;
        }
        out.dCamMean = cam.empty() ? -1.0 : camSum / static_cast<double>(cam.size());
        out.dCamMed = MedianOf(cam);
        out.dPlayerMed = MedianOf(player);
        out.dListenerMed = MedianOf(listener);
        out.meanLod = lodKnown ? lodSum / lodKnown : -1.0;
        out.playerBitShare = bitKnown ? static_cast<double>(bits) / bitKnown : -1.0;
        out.playerAudioShare = out.count ? static_cast<double>(audio) / out.count : -1.0;
        out.perSecond = seconds > 0.0 ? out.count / seconds : 0.0;
        return out;
    }

    struct Row
    {
        Role role = Role::Other;
        Kind kind = Kind::Generic;
        SideStats rec;
        SideStats play;
        // Matched by sound set (the ref's low dword), not by the whole ref:
        // the byte 32..39 (the set's entry) alone changing is counted apart
        // (entry), never as a variant.
        std::uint32_t same = 0;      // play sounds with a recorded one of the same owner, kind and set nearby
        std::uint32_t entry = 0;     // ... of which the entry (byte 32..39) differs
        std::uint32_t swapped = 0;   // ... with only another set nearby (a variant)
        std::uint32_t unmatched = 0; // ... with none
    };

    // The sound set a ref names (sound_owner_table.h kSoundSetMask).
    inline std::uint32_t SetOf(std::uint64_t ref) { return static_cast<std::uint32_t>(ref & 0xFFFFFFFFull); }

    struct SwapPair
    {
        Role role = Role::Other;
        Kind kind = Kind::Generic;
        std::uint32_t owner = census::kNoOwner;
        std::uint64_t recRef = 0;
        std::uint64_t playRef = 0;
        std::uint32_t count = 0;
    };

    enum class Level : std::uint8_t
    {
        Unknown = 0,
        No = 1,
        Possible = 2,
        Likely = 3,
    };

    inline const char* LevelName(Level level)
    {
        switch (level)
        {
        case Level::Unknown: return "unknown";
        case Level::No: return "no";
        case Level::Possible: return "possible";
        case Level::Likely: return "likely";
        }
        return "?";
    }

    enum class Cause : std::uint8_t
    {
        Listener = 0,
        PlayerTreatment = 1,
        Variant = 2,
        Lod = 3,
        Music = 4,
        // The replay makes far fewer sounds of a role and kind than the
        // recording (hooves that are never created). The other verdicts only
        // look at sounds that exist.
        Count = 5,
    };
    constexpr std::uint8_t kCauseCount = 6;

    inline const char* CauseName(Cause cause)
    {
        static const char* const kNames[kCauseCount] = {"listener", "player_treatment", "variant", "lod", "music", "count"};
        return static_cast<std::uint8_t>(cause) < kCauseCount ? kNames[static_cast<std::uint8_t>(cause)] : "?";
    }

    struct Verdict
    {
        Level level = Level::Unknown;
        std::string why;
    };

    struct Comparison
    {
        bool recorded = false;       // the clip has the recording's samples
        std::uint32_t fromMs = 0;    // the clip time compared (the whole clip: 0 to UINT32_MAX)
        std::uint32_t toMs = UINT32_MAX;
        double coveredSeconds = 0.0; // clip seconds the playback went through at normal play
        double playSeconds = 0.0;    // clip seconds played (a loop counts twice)
        std::uint32_t recSamples = 0;  // recording samples inside the covered span
        std::uint32_t playSamples = 0;
        std::uint32_t recDropped = 0;
        double recCamMain = -1.0;    // median camera to main character, from the listener samples
        double playCamMain = -1.0;
        double recMusic = -1.0;      // share of listener samples with the game's music on
        double playMusic = -1.0;
        std::string recListenerFollows;  // the engine's own distance (d_lst) matches: camera, player, unclear
        std::string playListenerFollows;
        std::vector<Row> rows;
        std::vector<SwapPair> swaps; // most first
        Verdict verdicts[kCauseCount];
    };

    namespace detail
    {
        inline bool Inside(std::uint32_t t, std::uint32_t from, std::uint32_t to) { return t >= from && t <= to; }

        // Which of camera or live player the engine's own distance follows:
        // the median of |d_lst - d_cam| against |d_lst - d_player|.
        inline std::string ListenerFollows(const std::vector<const Sample*>& samples)
        {
            std::vector<double> toCam, toPlayer;
            for (const Sample* s : samples)
            {
                if (s->dListenerDm == kNoDistance || s->dCamDm == kNoDistance || s->dPlayerDm == kNoDistance)
                    continue;
                toCam.push_back(std::fabs(FromDm(s->dListenerDm) - FromDm(s->dCamDm)));
                toPlayer.push_back(std::fabs(FromDm(s->dListenerDm) - FromDm(s->dPlayerDm)));
            }
            if (toCam.size() < kMinRowSamples)
                return "unclear (too few samples with the engine distance)";
            const double cam = MedianOf(toCam);
            const double player = MedianOf(toPlayer);
            char text[160];
            const char* who = cam + 1.0 < player * 0.5 ? "camera" : player + 1.0 < cam * 0.5 ? "live player" : "unclear";
            std::snprintf(text, sizeof(text), "%s (median gap to camera %.1f m, to live player %.1f m, n=%zu)", who, cam,
                player, toCam.size());
            return text;
        }

        inline double MedianCamMain(const std::vector<const Listener*>& listeners)
        {
            std::vector<double> values;
            for (const Listener* l : listeners)
                if (l->hasMain)
                    values.push_back(Distance(l->cam, l->main));
            return MedianOf(values);
        }

        inline double MusicShare(const std::vector<const Listener*>& listeners)
        {
            std::uint32_t known = 0, on = 0;
            for (const Listener* l : listeners)
            {
                if (l->music > 1)
                    continue;
                ++known;
                on += l->music;
            }
            return known ? static_cast<double>(on) / known : -1.0;
        }

        inline bool Enough(const Row& row)
        {
            return row.rec.count >= kMinRowSamples && row.play.count >= kMinRowSamples;
        }

        inline std::string Fixed(double value)
        {
            char text[32];
            if (value < 0.0)
                return "-";
            std::snprintf(text, sizeof(text), "%.1f", value);
            return text;
        }

        inline std::string Percent(double share)
        {
            char text[32];
            if (share < 0.0)
                return "-";
            std::snprintf(text, sizeof(text), "%.0f%%", share * 100.0);
            return text;
        }
    }

    // The count verdict (Cause::Count): the role and kind whose replayed
    // rate falls furthest under the recorded one. Likely: under
    // kCountLikelyRatio with the whole role also under kCountRoleRatio (a
    // kind classified apart on one side keeps the role's rate); Possible:
    // under kCountLikelyRatio alone, or under kCountPossibleRatio.
    constexpr std::uint32_t kCountMinRecorded = 10;
    constexpr double kCountLikelyRatio = 0.5;
    constexpr double kCountPossibleRatio = 0.7;
    constexpr double kCountRoleRatio = 0.75;

    struct CountFinding
    {
        const Row* row = nullptr;
        double ratio = 1.0;
        double roleRec = 0.0;
        double roleRay = 0.0; // the role's replayed rate
        double roleRatio = 1.0;
        Level level = Level::Unknown;
    };

    inline CountFinding WorstCount(const std::vector<Row>& rows)
    {
        CountFinding out;
        double roleRec[kRoleCount] = {};
        double rolePlay[kRoleCount] = {};
        for (const Row& row : rows)
        {
            roleRec[static_cast<std::uint8_t>(row.role)] += row.rec.perSecond;
            rolePlay[static_cast<std::uint8_t>(row.role)] += row.play.perSecond;
        }
        // The row with the highest level, then the lowest ratio.
        for (const Row& row : rows)
        {
            if (row.rec.count < kCountMinRecorded || row.rec.perSecond <= 0.0)
                continue;
            const std::uint8_t r = static_cast<std::uint8_t>(row.role);
            const double ratio = row.play.perSecond / row.rec.perSecond;
            const double roleRatio = roleRec[r] > 0.0 ? rolePlay[r] / roleRec[r] : 1.0;
            const Level level = ratio < kCountLikelyRatio && roleRatio < kCountRoleRatio ? Level::Likely
                : ratio < kCountPossibleRatio                                         ? Level::Possible
                                                                                      : Level::No;
            if (out.row && (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(out.level) ||
                               (level == out.level && ratio >= out.ratio)))
                continue;
            out.row = &row;
            out.ratio = ratio;
            out.roleRec = roleRec[r];
            out.roleRay = rolePlay[r];
            out.roleRatio = roleRatio;
            out.level = level;
        }
        return out;
    }

    // The recording's samples (`recorded`) against the playback's, over the
    // clip time the playback covered (`cover`) and inside [fromMs, toMs]
    // (the whole clip by default; a QA mark uses the 3 s before it).
    inline Comparison Compare(const Track& recorded, const std::vector<Sample>& play,
        const std::vector<Listener>& playListeners, const Coverage& cover, std::uint32_t fromMs = 0,
        std::uint32_t toMs = UINT32_MAX)
    {
        using detail::Inside;
        Comparison out;
        out.fromMs = fromMs;
        out.toMs = toMs;
        out.recorded = !recorded.Empty();
        out.recDropped = recorded.dropped;
        const bool whole = fromMs == 0 && toMs == UINT32_MAX;
        out.coveredSeconds = cover.CoveredSeconds();
        out.playSeconds = cover.PlaySeconds();
        if (!whole)
        {
            const double span = (static_cast<double>(toMs) - fromMs) / 1000.0;
            out.coveredSeconds = (std::min)(out.coveredSeconds, span);
            out.playSeconds = span;
        }

        std::vector<const Sample*> rec, ply;
        for (const Sample& s : recorded.samples)
            if (Inside(s.timeMs, fromMs, toMs) && cover.Contains(s.timeMs))
                rec.push_back(&s);
        for (const Sample& s : play)
            if (Inside(s.timeMs, fromMs, toMs))
                ply.push_back(&s);
        out.recSamples = static_cast<std::uint32_t>(rec.size());
        out.playSamples = static_cast<std::uint32_t>(ply.size());

        std::vector<const Listener*> recL, plyL;
        for (const Listener& l : recorded.listeners)
            if (Inside(l.timeMs, fromMs, toMs) && cover.Contains(l.timeMs))
                recL.push_back(&l);
        for (const Listener& l : playListeners)
            if (Inside(l.timeMs, fromMs, toMs))
                plyL.push_back(&l);
        out.recCamMain = detail::MedianCamMain(recL);
        out.playCamMain = detail::MedianCamMain(plyL);
        out.recMusic = detail::MusicShare(recL);
        out.playMusic = detail::MusicShare(plyL);
        out.recListenerFollows = detail::ListenerFollows(rec);
        out.playListenerFollows = detail::ListenerFollows(ply);

        // Rows: every role and kind with a sound on either side.
        bool used[kRoleCount][kKindCount] = {};
        for (const Sample* s : rec)
            used[static_cast<std::uint8_t>(s->GetRole())][static_cast<std::uint8_t>(s->GetKind())] = true;
        for (const Sample* s : ply)
            used[static_cast<std::uint8_t>(s->GetRole())][static_cast<std::uint8_t>(s->GetKind())] = true;
        const double recSeconds = out.coveredSeconds;
        const double playSeconds = out.playSeconds > 0.0 ? out.playSeconds : out.coveredSeconds;
        for (std::uint8_t r = 0; r < kRoleCount; ++r)
        {
            for (std::uint8_t k = 0; k < kKindCount; ++k)
            {
                if (!used[r][k])
                    continue;
                Row row;
                row.role = static_cast<Role>(r);
                row.kind = static_cast<Kind>(k);
                const auto pick = [&](const Sample& s) { return s.GetRole() == row.role && s.GetKind() == row.kind; };
                row.rec = Summarize(rec, recSeconds, pick);
                row.play = Summarize(ply, playSeconds, pick);
                out.rows.push_back(row);
            }
        }

        // Variants: each play sound against the recorded sounds of the same
        // owner and kind within kMatchToleranceMs (a loop matches again).
        std::map<std::pair<std::uint32_t, std::uint8_t>, std::vector<const Sample*>> byOwner;
        for (const Sample* s : rec)
            if (!(s->flags & kFlagRefused))
                byOwner[{s->owner, s->kind}].push_back(s);
        std::map<std::tuple<std::uint8_t, std::uint8_t, std::uint32_t, std::uint64_t, std::uint64_t>, std::uint32_t> pairs;
        for (const Sample* s : ply)
        {
            if (s->flags & kFlagRefused)
                continue;
            Row* row = nullptr;
            for (Row& candidate : out.rows)
                if (candidate.role == s->GetRole() && candidate.kind == s->GetKind())
                    row = &candidate;
            if (!row)
                continue;
            const auto found = byOwner.find({s->owner, s->kind});
            if (found == byOwner.end())
            {
                ++row->unmatched;
                continue;
            }
            const std::vector<const Sample*>& list = found->second;
            const auto first = std::lower_bound(list.begin(), list.end(), s->timeMs > kMatchToleranceMs ? s->timeMs - kMatchToleranceMs : 0u,
                [](const Sample* a, std::uint32_t t) { return a->timeMs < t; });
            bool same = false;
            bool sameRef = false;
            const Sample* other = nullptr;
            std::uint32_t otherGap = UINT32_MAX;
            for (auto at = first; at != list.end() && (*at)->timeMs <= s->timeMs + kMatchToleranceMs; ++at)
            {
                if (SetOf((*at)->ref) == SetOf(s->ref))
                {
                    same = true;
                    if ((*at)->ref == s->ref)
                    {
                        sameRef = true;
                        break;
                    }
                    continue;
                }
                const std::uint32_t gap = (*at)->timeMs > s->timeMs ? (*at)->timeMs - s->timeMs : s->timeMs - (*at)->timeMs;
                if (gap < otherGap)
                {
                    otherGap = gap;
                    other = *at;
                }
            }
            if (same)
            {
                ++row->same;
                row->entry += sameRef ? 0 : 1;
            }
            else if (other)
            {
                ++row->swapped;
                ++pairs[std::make_tuple(s->role, s->kind, s->owner, other->ref, s->ref)];
            }
            else
                ++row->unmatched;
        }
        for (const auto& item : pairs)
        {
            SwapPair pair;
            pair.role = static_cast<Role>(std::get<0>(item.first));
            pair.kind = static_cast<Kind>(std::get<1>(item.first));
            pair.owner = std::get<2>(item.first);
            pair.recRef = std::get<3>(item.first);
            pair.playRef = std::get<4>(item.first);
            pair.count = item.second;
            out.swaps.push_back(pair);
        }
        std::stable_sort(out.swaps.begin(), out.swaps.end(),
            [](const SwapPair& a, const SwapPair& b) { return a.count > b.count; });

        // ---- verdicts
        char why[512];
        const auto mainRole = [](Role role) { return role == Role::Main || role == Role::MainHorse; };

        // 1. Listener: the owner's distance to the camera, replay against recording.
        {
            Verdict& v = out.verdicts[static_cast<std::uint8_t>(Cause::Listener)];
            const Row* worst = nullptr;
            double worstRatio = 0.0;
            bool anyMain = false;
            for (const Row& row : out.rows)
                anyMain = anyMain || (mainRole(row.role) && detail::Enough(row) && row.rec.dCamMed >= 0.0 && row.play.dCamMed >= 0.0);
            for (const Row& row : out.rows)
            {
                if (!detail::Enough(row) || row.rec.dCamMed < 0.0 || row.play.dCamMed < 0.0)
                    continue;
                if (anyMain && !mainRole(row.role) && !(row.role == Role::Npc && row.kind == Kind::Shot))
                    continue;
                const double ratio = row.play.dCamMed / (std::max)(row.rec.dCamMed, 1.0);
                if (!worst || ratio > worstRatio)
                {
                    worst = &row;
                    worstRatio = ratio;
                }
            }
            if (!worst)
            {
                v.level = Level::Unknown;
                v.why = out.recorded ? "too few sounds with an owner on both sides" : "the clip has no recording-side probe data";
            }
            else
            {
                const double diff = worst->play.dCamMed - worst->rec.dCamMed;
                v.level = worstRatio >= 2.0 && diff >= 5.0 ? Level::Likely
                    : worstRatio >= 1.4 && diff >= 2.0     ? Level::Possible
                                                           : Level::No;
                std::snprintf(why, sizeof(why),
                    "%s %s: median distance to the camera %.1f m recorded, %.1f m replayed (x%.1f); camera to "
                    "main character %s m recorded, %s m replayed; engine distance follows: recorded %s; replayed %s",
                    RoleName(worst->role, true), KindName(worst->kind), worst->rec.dCamMed, worst->play.dCamMed,
                    worstRatio, detail::Fixed(out.recCamMain).c_str(), detail::Fixed(out.playCamMain).c_str(),
                    out.recListenerFollows.c_str(), out.playListenerFollows.c_str());
                v.why = why;
            }
        }

        // 2. Player treatment: the share of the main character's sounds the
        // engine treats as the player's, replay against recording.
        {
            Verdict& v = out.verdicts[static_cast<std::uint8_t>(Cause::PlayerTreatment)];
            std::string text;
            Level level = Level::Unknown;
            for (const Row& row : out.rows)
            {
                if (!mainRole(row.role) || row.rec.count < kMinRowSamples || row.play.count < 1)
                    continue;
                // Shots and the main horse: the predicate the replay's detours
                // answer; movement and steps of the main character: the bit
                // itself (the movement audio reads it directly).
                const bool predicate = row.kind == Kind::Shot || row.role == Role::MainHorse;
                const double recShare = predicate ? row.rec.playerAudioShare : row.rec.playerBitShare;
                const double playShare = predicate ? row.play.playerAudioShare : row.play.playerBitShare;
                if (recShare < 0.0 || playShare < 0.0)
                    continue;
                const bool lost = recShare >= 0.5 && playShare < 0.5;
                const bool relevant = row.kind == Kind::Shot || row.kind == Kind::Movement || row.kind == Kind::Footstep ||
                    row.kind == Kind::HorseStep;
                Level here = lost ? (relevant ? Level::Likely : Level::Possible) : Level::No;
                if (static_cast<std::uint8_t>(here) > static_cast<std::uint8_t>(level))
                    level = here;
                if (lost || level == Level::No)
                {
                    char item[200];
                    std::snprintf(item, sizeof(item), "%s%s %s %s %s recorded, %s replayed", text.empty() ? "" : "; ",
                        RoleName(row.role, true), KindName(row.kind), predicate ? "player_audio" : "player bit",
                        detail::Percent(recShare).c_str(), detail::Percent(playShare).c_str());
                    if (lost || text.size() < 300)
                        text += item;
                }
            }
            v.level = level;
            v.why = level == Level::Unknown ? (out.recorded ? "no sound of the main character on both sides"
                                                            : "the clip has no recording-side probe data")
                                            : text;
            if (level == Level::Likely || level == Level::Possible)
                v.why += " (the movement audio 0x31815C4 reads the player bit directly: the replay's detours cover "
                         "only the weapon audio 0x2F02F8C and the mount predicate 0x8B69B0)";
        }

        // 3. Variant: another sound set for the same owner, kind and moment.
        {
            Verdict& v = out.verdicts[static_cast<std::uint8_t>(Cause::Variant)];
            std::uint32_t same = 0, swapped = 0, shotSwapped = 0, entry = 0;
            for (const Row& row : out.rows)
            {
                same += row.same;
                entry += row.entry;
                swapped += row.swapped;
                shotSwapped += row.kind == Kind::Shot ? row.swapped : 0;
            }
            const std::uint32_t matched = same + swapped;
            if (!matched)
            {
                v.level = Level::Unknown;
                v.why = out.recorded ? "no replayed sound met a recorded one of the same owner" : "the clip has no recording-side probe data";
            }
            else
            {
                const double share = static_cast<double>(swapped) / matched;
                v.level = share >= 0.10 && swapped >= 3 ? Level::Likely : swapped > 0 ? Level::Possible : Level::No;
                std::snprintf(why, sizeof(why),
                    "%u of %u matched sounds took another sound set (%.0f%%), %u of them shots; %u same set with "
                    "another entry (byte 32..39; not counted)%s",
                    swapped, matched, share * 100.0, shotSwapped, entry,
                    out.swaps.empty() ? "" : "; see the variant pairs below");
                v.why = why;
            }
        }

        // 4. Audio LOD: the owner's ped audio LOD, replay against recording,
        // in both directions (a replay MORE detailed than live makes layers
        // the live ped had cut).
        {
            Verdict& v = out.verdicts[static_cast<std::uint8_t>(Cause::Lod)];
            const Row* worst = nullptr;
            double worstDiff = 0.0;
            for (const Row& row : out.rows)
            {
                if (!detail::Enough(row) || row.rec.meanLod < 0.0 || row.play.meanLod < 0.0)
                    continue;
                const double diff = row.play.meanLod - row.rec.meanLod;
                if (!worst || std::fabs(diff) > std::fabs(worstDiff))
                {
                    worst = &row;
                    worstDiff = diff;
                }
            }
            if (!worst)
            {
                v.level = Level::Unknown;
                v.why = out.recorded ? "no readable ped audio LOD on both sides" : "the clip has no recording-side probe data";
            }
            else
            {
                const double size = std::fabs(worstDiff);
                v.level = size >= 1.0 ? Level::Likely : size >= 0.5 ? Level::Possible : Level::No;
                std::snprintf(why, sizeof(why),
                    "%s %s: mean LOD %.2f recorded, %.2f replayed (0 = full, 4 = off: the replay %s); %.1f vs %.1f per second",
                    RoleName(worst->role, true), KindName(worst->kind), worst->rec.meanLod, worst->play.meanLod,
                    size < 0.5 ? "matches" : worstDiff > 0.0 ? "is less detailed" : "is more detailed",
                    worst->rec.perSecond, worst->play.perSecond);
                v.why = why;
            }
        }

        // 5. Music: the game's score live, not in the replay.
        {
            Verdict& v = out.verdicts[static_cast<std::uint8_t>(Cause::Music)];
            if (out.recMusic < 0.0 || out.playMusic < 0.0)
            {
                v.level = Level::Unknown;
                v.why = out.recorded ? "no music samples on both sides" : "the clip has no recording-side probe data";
            }
            else
            {
                v.level = out.recMusic >= 0.25 && out.playMusic < 0.05 ? Level::Likely
                    : out.recMusic > 0.0 && out.playMusic < out.recMusic / 2.0 ? Level::Possible
                                                                              : Level::No;
                std::snprintf(why, sizeof(why),
                    "the game's music played in %s of the recorded seconds and %s of the replayed ones%s",
                    detail::Percent(out.recMusic).c_str(), detail::Percent(out.playMusic).c_str(),
                    v.level == Level::No ? "" : " (MusicStop=1 stops the score while the replay owns the world, by design)");
                v.why = why;
            }
        }

        // 6. Count: sounds made per second, replay against recording.
        {
            Verdict& v = out.verdicts[static_cast<std::uint8_t>(Cause::Count)];
            const CountFinding f = WorstCount(out.rows);
            if (!f.row)
            {
                v.level = Level::Unknown;
                v.why = out.recorded ? "no role and kind with enough recorded sounds" : "the clip has no recording-side probe data";
            }
            else
            {
                v.level = f.level;
                std::snprintf(why, sizeof(why),
                    "%s %s: %.1f per second recorded, %.1f replayed (x%.2f; %u against %u sounds); the whole role %.1f "
                    "against %.1f per second (x%.2f)%s%s",
                    RoleName(f.row->role, true), KindName(f.row->kind), f.row->rec.perSecond, f.row->play.perSecond,
                    f.ratio, f.row->rec.count, f.row->play.count, f.roleRec, f.roleRay, f.roleRatio,
                    f.level == Level::Possible && f.ratio < kCountLikelyRatio
                        ? " (the role keeps its rate: the kind may be classified apart on one side, e.g. a clip "
                          "recorded before the queued step kind)"
                        : "",
                    out.recDropped ? " (the recording dropped samples over its cap: its rate is a floor)" : "");
                v.why = why;
            }
        }
        return out;
    }

    // ------------------------------------------------------------ text

    inline std::string RefText(std::uint64_t ref)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(ref));
        return text;
    }

    inline std::string IdText(std::uint32_t packed)
    {
        if (packed == census::kNoOwner)
            return "none";
        char text[24];
        std::snprintf(text, sizeof(text), "%u:%u", packed >> 16, packed & 0xFFFFu);
        return text;
    }

    // One MIX_VERDICT line per cause (`tag`: MIX_VERDICT or MIX_MARK_VERDICT).
    inline std::vector<std::string> VerdictLines(const Comparison& c, const char* tag = "MIX_VERDICT",
        const std::string& extra = std::string())
    {
        std::vector<std::string> out;
        for (std::uint8_t n = 0; n < kCauseCount; ++n)
        {
            std::string why = c.verdicts[n].why;
            for (char& ch : why)
                if (ch == ',' || ch == '\n')
                    ch = ';';
            out.push_back(std::string(tag) + ",cause=" + CauseName(static_cast<Cause>(n)) + ",level=" +
                LevelName(c.verdicts[n].level) + extra + ",why=" + why);
        }
        return out;
    }

    // Compact rows for a log line (MIX_COMPARE / MIX_MARK_ROW).
    inline std::vector<std::string> RowLines(const Comparison& c, const char* tag, const std::string& extra = std::string())
    {
        std::vector<std::string> out;
        for (const Row& row : c.rows)
        {
            char line[768];
            std::snprintf(line, sizeof(line),
                "%s%s,role=%s,kind=%s,rec_n=%u,play_n=%u,rec_d_cam=%s,play_d_cam=%s,rec_d_lst=%s,play_d_lst=%s,"
                "rec_lod=%s,play_lod=%s,rec_pbit=%s,play_pbit=%s,rec_player_audio=%s,play_player_audio=%s,same=%u,swapped=%u,unmatched=%u,"
                "entry=%u,rec_refused=%u,play_refused=%u,rec_per_s=%.1f,play_per_s=%.1f",
                tag, extra.c_str(), RoleName(row.role, true), KindName(row.kind), row.rec.count, row.play.count,
                detail::Fixed(row.rec.dCamMed).c_str(), detail::Fixed(row.play.dCamMed).c_str(),
                detail::Fixed(row.rec.dListenerMed).c_str(), detail::Fixed(row.play.dListenerMed).c_str(),
                row.rec.meanLod < 0.0 ? "-" : (detail::Fixed(row.rec.meanLod)).c_str(),
                row.play.meanLod < 0.0 ? "-" : (detail::Fixed(row.play.meanLod)).c_str(),
                detail::Percent(row.rec.playerBitShare).c_str(), detail::Percent(row.play.playerBitShare).c_str(),
                detail::Percent(row.rec.playerAudioShare).c_str(), detail::Percent(row.play.playerAudioShare).c_str(),
                row.same, row.swapped, row.unmatched, row.entry, row.rec.refused, row.play.refused, row.rec.perSecond,
                row.play.perSecond);
            out.push_back(line);
        }
        return out;
    }

    // The report section (ReplayDebug\<clip>.sound-mix.txt, and inside the
    // sound-check report when that one is written).
    inline std::string FormatSection(const Comparison& c)
    {
        std::string out = "== mix probe: where each sound is heard from, recording against replay ==\n";
        char line[640];
        if (!c.recorded)
            out += "The clip has no recording-side probe data (recorded without MixProbe=1): only the replay side below.\n";
        std::snprintf(line, sizeof(line),
            "Compared: %.1f s of clip time at normal play (%.1f s played); %u recorded and %u replayed sounds with an owner%s.\n",
            c.coveredSeconds, c.playSeconds, c.recSamples, c.playSamples,
            c.recDropped ? " (the recording dropped some over its cap)" : "");
        out += line;
        std::snprintf(line, sizeof(line),
            "Camera to main character (median): %s m recorded, %s m replayed. Game music on: %s recorded, %s replayed.\n",
            detail::Fixed(c.recCamMain).c_str(), detail::Fixed(c.playCamMain).c_str(), detail::Percent(c.recMusic).c_str(),
            detail::Percent(c.playMusic).c_str());
        out += line;
        out += "Engine distance (d_lst, sqrt of the ped audio metric, hypothesis) follows: recorded " + c.recListenerFollows +
            "; replayed " + c.playListenerFollows + ".\n\n";
        out += "Verdicts (one per suspect cause):\n";
        static const char* const kTitles[kCauseCount] = {"1. listener distance", "2. player treatment", "3. sound variant",
            "4. audio LOD", "5. game music", "6. sound count"};
        for (std::uint8_t n = 0; n < kCauseCount; ++n)
        {
            std::snprintf(line, sizeof(line), "  %-22s %-8s %s\n", kTitles[n], LevelName(c.verdicts[n].level),
                c.verdicts[n].why.c_str());
            out += line;
        }
        out += "\nPer role and kind (rec = recording, play = replay; d_cam = median metres to the rendered camera,\n"
               "d_lst = engine distance, lod = mean ped audio LOD 0 full .. 4 off, pbit = engine player bit,\n"
               "p_audio = the weapon/mount predicate says player, /s = sounds per clip second; n and /s leave out\n"
               "refused requests, counted in \"refused\"; same = same owner, kind and sound set nearby, entry = of those\n"
               "with another entry byte, swap = another set only):\n";
        std::snprintf(line, sizeof(line), "  %-14s %-10s %7s %7s %7s %7s %6s %6s %6s %6s %7s %7s %6s %6s %5s %5s %5s %5s %7s\n", "role",
            "kind", "rec n", "play n", "rec/s", "play/s", "d_cam", "d_cam", "d_lst", "d_lst", "lod", "lod", "pbit", "pbit",
            "same", "entry", "swap", "none", "refused");
        out += line;
        std::snprintf(line, sizeof(line), "  %-14s %-10s %7s %7s %7s %7s %6s %6s %6s %6s %7s %7s %6s %6s %5s %5s %5s %5s %7s\n", "", "",
            "", "", "", "", "rec", "play", "rec", "play", "rec", "play", "rec", "play", "", "", "", "", "rec/play");
        out += line;
        for (const Row& row : c.rows)
        {
            char rs[16], ps[16];
            std::snprintf(rs, sizeof(rs), "%.1f", row.rec.perSecond);
            std::snprintf(ps, sizeof(ps), "%.1f", row.play.perSecond);
            const bool predicate = row.kind == Kind::Shot || row.role == Role::MainHorse;
            char refusedText[24];
            std::snprintf(refusedText, sizeof(refusedText), "%u/%u", row.rec.refused, row.play.refused);
            std::snprintf(line, sizeof(line), "  %-14s %-10s %7u %7u %7s %7s %6s %6s %6s %6s %7s %7s %6s %6s %5u %5u %5u %5u %7s%s\n",
                RoleName(row.role, true), KindName(row.kind), row.rec.count, row.play.count, rs, ps,
                detail::Fixed(row.rec.dCamMed).c_str(), detail::Fixed(row.play.dCamMed).c_str(),
                detail::Fixed(row.rec.dListenerMed).c_str(), detail::Fixed(row.play.dListenerMed).c_str(),
                row.rec.meanLod < 0.0 ? "-" : detail::Fixed(row.rec.meanLod).c_str(),
                row.play.meanLod < 0.0 ? "-" : detail::Fixed(row.play.meanLod).c_str(),
                detail::Percent(predicate ? row.rec.playerAudioShare : row.rec.playerBitShare).c_str(),
                detail::Percent(predicate ? row.play.playerAudioShare : row.play.playerBitShare).c_str(), row.same,
                row.entry, row.swapped, row.unmatched, refusedText, predicate ? " (p_audio)" : "");
            out += line;
        }
        if (!c.swaps.empty())
        {
            out += "\nVariant pairs (same owner and kind within 150 ms, another sound set; most first):\n";
            std::size_t shown = 0;
            for (const SwapPair& pair : c.swaps)
            {
                if (++shown > 20)
                    break;
                std::snprintf(line, sizeof(line), "  %-14s %-10s owner %-9s recorded %s replayed %s x%u\n",
                    RoleName(pair.role, true), KindName(pair.kind), IdText(pair.owner).c_str(), RefText(pair.recRef).c_str(),
                    RefText(pair.playRef).c_str(), pair.count);
                out += line;
            }
        }
        out += "\n";
        return out;
    }
}
