#pragma once

// How long after the fire event the gunshot really starts, per shooter, in
// the recording (the engine's own delay, the reference) and in the replay.
//
// The chain followed here, all of it on the engine's own objects:
//
//   fire event --(fire queue 0x2EE8D9C, consumer 0x2F0DB9C)--> gunshot sound
//   created on the weapon's audio (weapon +0x100; the census sees the
//   creator, row.audio and row.sound) --(the audio thread prepares it, the
//   wave bank loaded or not)--> play state "playing".
//
// Build 1491.50: the top-level sound's play state is the two top bits of
// byte +0xF2: the requested-settings processor 0x2533EF0 reads it
// (0x2533F72: movzx esi,[rbx+0F2h]; shr esi,6) and switches on it; the
// prepare 0x2587B08 writes 1 (0x2587BB5), the play 0x258791C writes 2
// (0x2587976, beside the start time at +0x8C) after the prepare answered 1
// (0x2534188), and 0x25878E4 writes 3. Read as 0 dormant, 1 preparing,
// 2 playing, 3 waiting to be deleted.
//
// The gunshot's init params carry no predelay (+0xEC) and no start offset
// (+0xE8): the play function 0x2EFA7E0 builds them at [rbp] (ctor 0x487E34
// at 0x2EFAB2C) and then writes +0xA4, +0xA8, +0xB0, +0xC8, +0xD0, +0xE0
// (environment group), +0x104, +0x138 and +0x13A, never +0xE8 or +0xEC, and
// recorded gunshot packets list neither among the non-default dwords. So the
// engine does not delay a distant gunshot by distance at its creation; the
// travel time printed (distance / 343 m/s) is a reference only.
//
// Pure: the matching of creates to fires, the start samples, the tallies and
// the log text (unit tested in tests/scenery_speech_tests.cpp). The engine side
// (census rows, the play state read, the bank probe, the camera) is in
// replay_events.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace replay::shotdelay
{
    constexpr std::uint64_t kCreateWindowUs = 1000000; // the fire's sound is created within 1 s, else unmatched
    constexpr std::uint64_t kStartWindowUs = 2000000;  // and starts within 2 s, else never started
    constexpr std::uint64_t kClockSlackUs = 2000;      // the fire's and the create's clock reads (two threads)
    constexpr float kSpeedOfSoundMps = 343.0f;         // the reference travel time only
    constexpr std::size_t kMaxOpen = 256;              // fires followed at once (older ones close as unmatched)
    // Clip reference (the SGEN packets of a clip, no live measure): the
    // first weapon packet created by the gunshot's play function (its SGEN
    // caller in kFirePlayBegin..kFirePlayEnd) from kRefBeforeMs before the
    // shot's recorded tick to kRefAfterMs after it. Other named weapon
    // callers are left out: 0x3125951 can come 100 to 130 ms after each shot
    // and 0x310F629 even before the shooter's first shot, so a missing
    // fire packet would otherwise read as a delay.
    constexpr std::uint32_t kRefBeforeMs = 200;
    constexpr std::uint32_t kRefAfterMs = 500;
    // The gunshot's play function (build 1491.50 .pdata extent): 0x2EFA7E0
    // (called by the fire event's processing 0x2F0B6B8) creates the fire
    // sound at 0x2EFAEAA (return 0x2EFAEAF, the gunshots' SGEN caller). A
    // create with a frame in it is the fire sound for sure; one
    // without (another weapon sound on the same audio) is counted apart.
    constexpr std::uint32_t kFirePlayBegin = 0x2EFA7E0u;
    constexpr std::uint32_t kFirePlayEnd = 0x2EFB1F0u;

    inline bool InFirePlay(const std::uint32_t* callers, std::size_t count)
    {
        for (std::size_t n = 0; n < count; ++n)
            if (callers[n] >= kFirePlayBegin && callers[n] < kFirePlayEnd)
                return true;
        return false;
    }

    // The wave bank state of the gunshot's ref when it was created
    // (audio::BankStateName).
    enum class Bank : std::uint8_t
    {
        Unknown = 0,
        Static,
        Loaded,
        Loading,
        Failed,
        NotRequested,
        Absent,
    };
    constexpr std::size_t kBankCount = 7;

    inline const char* BankName(Bank bank)
    {
        static const char* const kNames[kBankCount] = {"unknown", "static", "loaded", "loading", "failed", "not_requested", "absent"};
        return static_cast<std::size_t>(bank) < kBankCount ? kNames[static_cast<std::size_t>(bank)] : "unknown";
    }

    inline Bank BankOfName(const char* name)
    {
        for (std::size_t n = 1; name && n < kBankCount; ++n)
            if (std::strcmp(name, BankName(static_cast<Bank>(n))) == 0)
                return static_cast<Bank>(n);
        return Bank::Unknown;
    }

    // One fire event handed to the engine: the game's in the recording (the
    // fire observer's ring), the replay's EnqueueFire in the playback.
    struct Fire
    {
        std::uint32_t shooter = 0;   // packed replay id
        std::uint64_t audio = 0;     // the weapon's audio entity (weapon +0x100)
        std::uint64_t fireUs = 0;    // QPC us
        std::uint32_t eventMs = 0;   // the shot's clip time
        float distanceM = -1.0f;     // rendered camera (the listener) to the muzzle; < 0 unknown
        bool player = false;         // the recorded player's shot (the puppet that answers "player")
    };

    // A census row (the creator saw it).
    struct Create
    {
        std::uint64_t timeUs = 0;
        std::uint64_t audio = 0;
        std::uint64_t sound = 0; // 0: the creator refused
        std::uint64_t ref = 0;
        bool firePath = false; // a frame in the gunshot's play function (InFirePlay)
    };

    enum class Outcome : std::uint8_t
    {
        Open = 0,
        Started,      // a sample saw it playing
        EndedUnseen,  // a sample saw it already ended (played between two samples, or stopped)
        Lost,         // the sound was gone (freed or reused) before a sample saw it start
        NeverStarted, // created, not playing kStartWindowUs after the fire
        Refused,      // the creator returned no sound
        Unmatched,    // no create on the weapon's audio within kCreateWindowUs
    };

    inline const char* OutcomeName(Outcome outcome)
    {
        switch (outcome)
        {
        case Outcome::Open: return "open";
        case Outcome::Started: return "started";
        case Outcome::EndedUnseen: return "ended_unseen";
        case Outcome::Lost: return "lost";
        case Outcome::NeverStarted: return "never_started";
        case Outcome::Refused: return "refused";
        case Outcome::Unmatched: return "unmatched";
        }
        return "?";
    }

    // One fire followed to its audible start.
    struct Shot
    {
        Fire fire;
        bool created = false;
        std::uint64_t createUs = 0;
        std::uint64_t sound = 0;
        std::uint64_t identity = 0; // the engine side's identity of the live sound (0: not held)
        std::uint64_t ref = 0;
        bool firePath = false;
        std::uint64_t lowerUs = 0;  // the last sample that saw it not started (or the create)
        std::uint64_t startUs = 0;  // the first sample that saw it started (an upper bound)
        int lastState = -1;
        Bank bank = Bank::Unknown;
        Outcome outcome = Outcome::Open;

        double CreateMs() const { return created && createUs > fire.fireUs ? (createUs - fire.fireUs) / 1000.0 : 0.0; }
        double StartMs() const { return startUs > fire.fireUs ? (startUs - fire.fireUs) / 1000.0 : 0.0; }
        double StartLowerMs() const { return lowerUs > fire.fireUs ? (lowerUs - fire.fireUs) / 1000.0 : 0.0; }
    };

    struct Tally
    {
        std::uint32_t shots = 0;
        std::uint32_t created = 0;
        std::uint32_t refused = 0;
        std::uint32_t unmatched = 0;
        std::uint32_t started = 0;
        std::uint32_t endedUnseen = 0;
        std::uint32_t lost = 0;
        std::uint32_t neverStarted = 0;
        std::uint32_t otherPath = 0; // created by another weapon code than the gunshot's play function
        double createSumMs = 0.0;
        double createMaxMs = 0.0;
        double startSumMs = 0.0;   // started and ended unseen (upper bounds)
        double startMaxMs = 0.0;
        double lowerSumMs = 0.0;
        double distSumM = 0.0;
        double distMaxM = 0.0;
        std::uint32_t distCount = 0;
        std::uint32_t banks[kBankCount] = {};
        // The player's shots and how many of them found their wave bank absent (recorded
        // loaded, played without body; PlayerWeaponBank).
        std::uint32_t playerShots = 0;
        std::uint32_t playerCreated = 0;
        std::uint32_t playerAbsent = 0;

        void Add(const Shot& shot)
        {
            ++shots;
            playerShots += shot.fire.player ? 1 : 0;
            if (shot.fire.distanceM >= 0.0f)
            {
                distSumM += shot.fire.distanceM;
                distMaxM = (std::max)(distMaxM, static_cast<double>(shot.fire.distanceM));
                ++distCount;
            }
            switch (shot.outcome)
            {
            case Outcome::Refused: ++refused; return;
            case Outcome::Unmatched:
            case Outcome::Open: ++unmatched; return;
            default: break;
            }
            ++created;
            if (shot.fire.player)
            {
                ++playerCreated;
                playerAbsent += shot.bank == Bank::Absent ? 1 : 0;
            }
            otherPath += shot.firePath ? 0 : 1;
            createSumMs += shot.CreateMs();
            createMaxMs = (std::max)(createMaxMs, shot.CreateMs());
            ++banks[static_cast<std::size_t>(shot.bank) < kBankCount ? static_cast<std::size_t>(shot.bank) : 0];
            switch (shot.outcome)
            {
            case Outcome::Started:
            case Outcome::EndedUnseen:
                ++(shot.outcome == Outcome::Started ? started : endedUnseen);
                startSumMs += shot.StartMs();
                startMaxMs = (std::max)(startMaxMs, shot.StartMs());
                lowerSumMs += shot.StartLowerMs();
                return;
            case Outcome::Lost: ++lost; return;
            case Outcome::NeverStarted: ++neverStarted; return;
            default: return;
            }
        }
        void Merge(const Tally& o)
        {
            shots += o.shots;
            created += o.created;
            refused += o.refused;
            unmatched += o.unmatched;
            started += o.started;
            endedUnseen += o.endedUnseen;
            lost += o.lost;
            neverStarted += o.neverStarted;
            otherPath += o.otherPath;
            createSumMs += o.createSumMs;
            createMaxMs = (std::max)(createMaxMs, o.createMaxMs);
            startSumMs += o.startSumMs;
            startMaxMs = (std::max)(startMaxMs, o.startMaxMs);
            lowerSumMs += o.lowerSumMs;
            distSumM += o.distSumM;
            distMaxM = (std::max)(distMaxM, o.distMaxM);
            distCount += o.distCount;
            for (std::size_t n = 0; n < kBankCount; ++n)
                banks[n] += o.banks[n];
            playerShots += o.playerShots;
            playerCreated += o.playerCreated;
            playerAbsent += o.playerAbsent;
        }
    };

    // The clip's reference per shooter (signed ms: the create may come in the
    // frame before the shot's recorded tick).
    struct Reference
    {
        std::uint32_t count = 0;
        double sumMs = 0.0;
        double maxMs = 0.0;
        double minMs = 0.0;
        void Add(double ms)
        {
            maxMs = count ? (std::max)(maxMs, ms) : ms;
            minMs = count ? (std::min)(minMs, ms) : ms;
            sumMs += ms;
            ++count;
        }
    };

    struct RefShot
    {
        std::uint32_t shooter = 0;
        std::uint32_t weapon = 0; // packed weapon id (the SGEN owner of the gunshot)
        std::uint32_t timeMs = 0;
    };
    struct RefPacket
    {
        std::uint32_t owner = 0;
        std::uint32_t timeMs = 0;
        bool weaponAudio = false;
        bool firePath = false; // the SGEN caller is in the gunshot's play function (InFirePlay)
    };

    // Per shooter: the first weapon audio packet of the fire path on the
    // shot's weapon near the shot, as its delay. `packets` in time order.
    inline std::map<std::uint32_t, Reference> ClipReference(const std::vector<RefShot>& shots, const std::vector<RefPacket>& packets)
    {
        std::map<std::uint32_t, Reference> out;
        std::vector<bool> used(packets.size(), false);
        for (const RefShot& shot : shots)
        {
            const std::uint32_t from = shot.timeMs > kRefBeforeMs ? shot.timeMs - kRefBeforeMs : 0;
            const auto first = std::lower_bound(packets.begin(), packets.end(), from,
                [](const RefPacket& p, std::uint32_t t) { return p.timeMs < t; });
            for (auto it = first; it != packets.end() && it->timeMs <= shot.timeMs + kRefAfterMs; ++it)
            {
                const std::size_t index = static_cast<std::size_t>(it - packets.begin());
                if (used[index] || !it->weaponAudio || !it->firePath || it->owner != shot.weapon)
                    continue;
                used[index] = true;
                out[shot.shooter].Add(static_cast<double>(it->timeMs) - static_cast<double>(shot.timeMs));
                break;
            }
        }
        return out;
    }

    class Tracker
    {
    public:
        void Reset()
        {
            m_open.clear();
            m_finished.clear();
            m_tallies.clear();
        }

        void AddFire(const Fire& fire)
        {
            if (m_open.size() >= kMaxOpen)
            {
                Close(m_open.front(), m_open.front().created ? Outcome::NeverStarted : Outcome::Unmatched);
                m_open.erase(m_open.begin());
            }
            Shot shot;
            shot.fire = fire;
            m_open.push_back(shot);
        }

        // The census rows of this tick. `matched` gets the indexes of the
        // shots created now (valid until Expire), for the engine side's
        // identity and bank probe. A create goes to the oldest open fire of
        // its audio entity that it does not precede.
        void OnCreates(const std::vector<Create>& rows, std::vector<std::size_t>& matched)
        {
            matched.clear();
            if (m_open.empty())
                return;
            for (const Create& row : rows)
            {
                if (!row.audio)
                    continue;
                for (std::size_t n = 0; n < m_open.size(); ++n)
                {
                    Shot& shot = m_open[n];
                    if (shot.created || shot.outcome != Outcome::Open || shot.fire.audio != row.audio ||
                        row.timeUs + kClockSlackUs < shot.fire.fireUs || row.timeUs > shot.fire.fireUs + kCreateWindowUs)
                    {
                        continue;
                    }
                    shot.created = true;
                    shot.createUs = (std::max)(row.timeUs, shot.fire.fireUs);
                    shot.lowerUs = shot.createUs;
                    shot.sound = row.sound;
                    shot.ref = row.ref;
                    shot.firePath = row.firePath;
                    if (!row.sound)
                        shot.outcome = Outcome::Refused;
                    else
                        matched.push_back(n);
                    break;
                }
            }
        }

        std::size_t Size() const { return m_open.size(); }
        Shot& At(std::size_t index) { return m_open[index]; }
        const Shot& At(std::size_t index) const { return m_open[index]; }

        // The created shots still waiting for a sample to see them start.
        void Waiting(std::vector<std::size_t>& out) const
        {
            out.clear();
            for (std::size_t n = 0; n < m_open.size(); ++n)
                if (m_open[n].created && m_open[n].outcome == Outcome::Open)
                    out.push_back(n);
        }

        // A sample of a waiting shot: `alive` false when the engine side no
        // longer holds the sound; `state` its play state (0 to 3).
        void Sample(std::size_t index, bool alive, int state, std::uint64_t nowUs)
        {
            Shot& shot = m_open[index];
            if (shot.outcome != Outcome::Open)
                return;
            if (!alive)
            {
                shot.outcome = Outcome::Lost;
                return;
            }
            shot.lastState = state;
            if (state >= 2)
            {
                shot.startUs = (std::max)(nowUs, shot.createUs);
                shot.outcome = state == 2 ? Outcome::Started : Outcome::EndedUnseen;
            }
            else
            {
                shot.lowerUs = (std::max)(shot.lowerUs, nowUs);
            }
        }

        // Closes what has an outcome or ran out of time (`all`: everything,
        // the phase's end), folds it into the tallies and the finished list.
        void Expire(std::uint64_t nowUs, bool all = false)
        {
            std::size_t kept = 0;
            for (std::size_t n = 0; n < m_open.size(); ++n)
            {
                Shot& shot = m_open[n];
                if (shot.outcome == Outcome::Open)
                {
                    if (!shot.created && (all || nowUs > shot.fire.fireUs + kCreateWindowUs))
                        shot.outcome = Outcome::Unmatched;
                    else if (shot.created && (all || nowUs > shot.fire.fireUs + kStartWindowUs))
                        shot.outcome = Outcome::NeverStarted;
                }
                if (shot.outcome != Outcome::Open)
                {
                    Close(shot, shot.outcome);
                    continue;
                }
                if (kept != n)
                    m_open[kept] = m_open[n];
                ++kept;
            }
            m_open.resize(kept);
        }

        // The shots closed since the last call (the per-shot log lines).
        std::vector<Shot> TakeFinished()
        {
            std::vector<Shot> out;
            out.swap(m_finished);
            return out;
        }

        const std::map<std::uint32_t, Tally>& Tallies() const { return m_tallies; }

    private:
        void Close(Shot& shot, Outcome outcome)
        {
            shot.outcome = outcome;
            m_tallies[shot.fire.shooter].Add(shot);
            if (m_finished.size() < kMaxOpen)
                m_finished.push_back(shot);
        }

        std::vector<Shot> m_open;
        std::vector<Shot> m_finished;
        std::map<std::uint32_t, Tally> m_tallies;
    };

    // ------------------------------------------------------------ log text

    inline std::string Num(double value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", value);
        return text;
    }

    inline std::string BanksText(const Tally& t)
    {
        std::string out;
        for (std::size_t n = 0; n < kBankCount; ++n)
            if (t.banks[n])
                out += (out.empty() ? "" : ";") + std::string(BankName(static_cast<Bank>(n))) + ":" + std::to_string(t.banks[n]);
        return out.empty() ? "none" : out;
    }

    // The metrics of a tally (one shooter or the total), without the tag.
    inline std::string TallyText(const Tally& t)
    {
        const std::uint32_t starts = t.started + t.endedUnseen;
        const double distAvg = t.distCount ? t.distSumM / t.distCount : -1.0;
        std::string out;
        out += "shots=" + std::to_string(t.shots) + ",created=" + std::to_string(t.created) + ",refused=" +
            std::to_string(t.refused) + ",unmatched=" + std::to_string(t.unmatched) + ",started=" + std::to_string(t.started) +
            ",ended_unseen=" + std::to_string(t.endedUnseen) + ",lost=" + std::to_string(t.lost) + ",never_started=" +
            std::to_string(t.neverStarted) + ",other_path=" + std::to_string(t.otherPath);
        out += ",create_avg_ms=" + Num(t.created ? t.createSumMs / t.created : 0.0) + ",create_max_ms=" + Num(t.createMaxMs);
        out += ",start_avg_ms=" + Num(starts ? t.startSumMs / starts : 0.0) + ",start_max_ms=" + Num(t.startMaxMs) +
            ",start_lower_avg_ms=" + Num(starts ? t.lowerSumMs / starts : 0.0);
        out += ",dist_avg_m=" + Num(distAvg) + ",dist_max_m=" + Num(t.distCount ? t.distMaxM : -1.0) + ",travel_avg_ms=" +
            Num(distAvg >= 0.0 ? distAvg * 1000.0 / kSpeedOfSoundMps : -1.0) + ",bank=" + BanksText(t);
        return out;
    }

    inline std::string ReferenceText(const Reference* ref)
    {
        if (!ref || !ref->count)
            return "clip_ref_n=0";
        return "clip_ref_n=" + std::to_string(ref->count) + ",clip_ref_avg_ms=" + Num(ref->sumMs / ref->count) +
            ",clip_ref_min_ms=" + Num(ref->minMs) + ",clip_ref_max_ms=" + Num(ref->maxMs);
    }

    // The player's shots without their bank. The recording has it loaded
    // (the live player holds the weapon); a replay shot of the player with
    // bank=absent plays without body (about -11.8 dB). verdict=bank_absent
    // flags it.
    inline const char* PlayerBankVerdict(const Tally& t)
    {
        if (!t.playerShots)
            return "no_player_shots";
        return t.playerAbsent ? "bank_absent" : "ok";
    }

    inline std::string PlayerBankLine(const char* phase, const Tally& t)
    {
        return std::string("POSE_V2_SHOT_PLAYER_BANK,phase=") + phase + ",player_shots=" + std::to_string(t.playerShots) +
            ",created=" + std::to_string(t.playerCreated) + ",bank_absent=" + std::to_string(t.playerAbsent) +
            ",verdict=" + PlayerBankVerdict(t) + "\n";
    }

    // POSE_V2_SHOT_AUDIO_DELAY per shooter and POSE_V2_SHOT_AUDIO_DELAY_TOTAL;
    // `idText` names a packed id; `refs` (playback) the clip's reference.
    template <typename IdText>
    inline std::string SummaryLines(const char* phase, const std::map<std::uint32_t, Tally>& tallies,
        const std::map<std::uint32_t, Reference>* refs, IdText idText)
    {
        std::string out;
        Tally total;
        Reference totalRef;
        for (const auto& entry : tallies)
        {
            total.Merge(entry.second);
            const Reference* ref = nullptr;
            if (refs)
            {
                const auto found = refs->find(entry.first);
                if (found != refs->end())
                    ref = &found->second;
            }
            out += std::string("POSE_V2_SHOT_AUDIO_DELAY,phase=") + phase + ",shooter=" + idText(entry.first) + "," +
                TallyText(entry.second) + (refs ? "," + ReferenceText(ref) : std::string()) + "\n";
        }
        if (refs)
            for (const auto& entry : *refs)
            {
                const Reference& r = entry.second;
                if (!r.count)
                    continue;
                totalRef.maxMs = totalRef.count ? (std::max)(totalRef.maxMs, r.maxMs) : r.maxMs;
                totalRef.minMs = totalRef.count ? (std::min)(totalRef.minMs, r.minMs) : r.minMs;
                totalRef.count += r.count;
                totalRef.sumMs += r.sumMs;
            }
        out += std::string("POSE_V2_SHOT_AUDIO_DELAY_TOTAL,phase=") + phase + ",shooters=" + std::to_string(tallies.size()) + "," +
            TallyText(total) + (refs ? "," + ReferenceText(&totalRef) : std::string()) + "\n";
        out += PlayerBankLine(phase, total);
        return out;
    }

    // POSE_V2_SHOT_AUDIO_DELAY_SHOT (one closed shot).
    inline std::string ShotLine(const char* phase, const Shot& shot, const std::string& shooterText)
    {
        char text[400];
        std::snprintf(text, sizeof(text),
            "POSE_V2_SHOT_AUDIO_DELAY_SHOT,phase=%s,shooter=%s,event_ms=%u,outcome=%s,create_ms=%.1f,start_ms=%.1f,"
            "start_lower_ms=%.1f,state=%d,bank=%s,fire_path=%d,ref=0x%llx,dist_m=%.1f,travel_ms=%.1f\n",
            phase, shooterText.c_str(), shot.fire.eventMs, OutcomeName(shot.outcome), shot.CreateMs(),
            shot.startUs ? shot.StartMs() : -1.0, shot.StartLowerMs(), shot.lastState, BankName(shot.bank), shot.firePath ? 1 : 0,
            static_cast<unsigned long long>(shot.ref), shot.fire.distanceM,
            shot.fire.distanceM >= 0.0f ? shot.fire.distanceM * 1000.0f / kSpeedOfSoundMps : -1.0f);
        return text;
    }
}
