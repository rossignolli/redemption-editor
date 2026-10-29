#pragma once

// What each recorded melee hit came to in the replay (pure part).
//
// The melee track (who hit and the move's hash) replays a recorded hit through
// the engine's melee audio entry 0x318A28C on the puppet (replay_events.cpp
// PlayPoseV2MeleeHits); the sounds the engine makes inside that call are the
// hit's. A hit is:
// - played: the call ran and made one sound or more;
// - silent: the call ran and made no sound (the hit plays mute);

// - refused: the call did not run (signature, melee audio not ready, a
//   fault), with the status;
// - no puppet: the hitting ped's puppet was not ready.
// A silent or refused hit is a finding of the sound check (MISSING, the
// notice at Stop) and a MELEE_SOUND_CHECK_FLAG line. A clip with no hit says
// so: the engine's melee audio entry was never called in the recording (a
// shove, a tackle, a body collision are contacts whose sounds are the ped
// impact and collision rows of the generic player, not hits).

#include "sound_check_core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace replay::meleecheck
{
    enum class Outcome : std::uint8_t
    {
        Played = 0,
        Silent = 1,
        Refused = 2,
        NoPuppet = 3,
    };

    inline const char* OutcomeToken(Outcome outcome)
    {
        switch (outcome)
        {
        case Outcome::Played: return "played";
        case Outcome::Silent: return "silent";
        case Outcome::Refused: return "refused";
        case Outcome::NoPuppet: return "no_puppet";
        }
        return "?";
    }

    // The sounds the call made, from PoseV2SoundsDuring's text ("4|0x..|..":
    // the count first). -1 when not measured ("observer_off", "none").
    inline int SoundCount(const std::string& text)
    {
        if (text.empty() || text[0] < '0' || text[0] > '9')
            return -1;
        return std::atoi(text.c_str());
    }

    // A hit's outcome: `ran` the call ran (PlayMeleeImpact true), `sounds`
    // the count (-1 not measured: a call that ran counts as played).
    inline Outcome Judge(bool puppetReady, bool ran, int sounds)
    {
        if (!puppetReady)
            return Outcome::NoPuppet;
        if (!ran)
            return Outcome::Refused;
        return sounds == 0 ? Outcome::Silent : Outcome::Played;
    }

    struct Row
    {
        std::uint32_t owner = census::kNoOwner; // the hitting ped (packed id)
        std::uint32_t eventMs = 0;
        std::uint32_t hash = 0; // the move's hash
        Outcome outcome = Outcome::Played;
        int sounds = -1;
        std::string status;
    };

    class Tally
    {
    public:
        static constexpr std::size_t kMaxRows = 64;

        void Reset()
        {
            m_rows.clear();
            m_dropped = 0;
            for (std::uint32_t& c : m_counts)
                c = 0;
        }

        void Note(std::uint32_t owner, std::uint32_t eventMs, std::uint32_t hash, Outcome outcome, int sounds,
            const std::string& status)
        {
            ++m_counts[static_cast<std::uint8_t>(outcome)];
            if (m_rows.size() >= kMaxRows)
            {
                ++m_dropped;
                return;
            }
            Row row;
            row.owner = owner;
            row.eventMs = eventMs;
            row.hash = hash;
            row.outcome = outcome;
            row.sounds = sounds;
            row.status = status;
            m_rows.push_back(row);
        }

        std::uint32_t Count(Outcome outcome) const { return m_counts[static_cast<std::uint8_t>(outcome)]; }
        std::uint32_t Due() const { return m_counts[0] + m_counts[1] + m_counts[2] + m_counts[3]; }
        // Hits that played no sound: silent, refused or without a puppet.
        std::uint32_t Mute() const { return m_counts[1] + m_counts[2] + m_counts[3]; }
        const std::vector<Row>& Rows() const { return m_rows; }

        // The fields of POSE_V2_PLAYBACK_MELEE_TOTAL after recorded=.
        std::string TotalFields() const
        {
            return ",due=" + std::to_string(Due()) + ",with_sound=" + std::to_string(Count(Outcome::Played)) +
                ",silent=" + std::to_string(Count(Outcome::Silent)) + ",call_refused=" + std::to_string(Count(Outcome::Refused)) +
                ",no_puppet=" + std::to_string(Count(Outcome::NoPuppet));
        }

        // The MELEE_SOUND_CHECK_FLAG line of a hit that played no sound
        // (empty for a played one).
        static std::string FlagLine(const Row& row)
        {
            if (row.outcome == Outcome::Played)
                return std::string();
            char text[200]{};
            std::snprintf(text, sizeof(text), "MELEE_SOUND_CHECK_FLAG,kind=%s,ped=%s,hash=0x%x,event_ms=%u,sounds=%d,status=%s",
                OutcomeToken(row.outcome), census::IdText(row.owner).c_str(), row.hash, row.eventMs, row.sounds,
                row.status.empty() ? "none" : row.status.c_str());
            return text;
        }

        // The section of ReplayDebug\<clip>.sound-check.txt. `recorded` the
        // clip's hits, `complements` the melee packets the generic player
        // played because the call did not make them again.
        std::string Section(std::size_t recorded, std::uint32_t complements) const
        {
            std::string out = "== melee hits (melee track: the recorded hit replayed through the engine's melee audio entry) ==\n";
            if (!recorded)
            {
                out += "No melee hit in the recording: the engine's melee audio entry was never called while recording. "
                       "A shove, a tackle or a body collision is a contact, not a hit: its sounds are the ped impact and "
                       "collision rows of the generic sound packets above.\n";
                return out;
            }
            out += "Recorded hits " + std::to_string(recorded) + ". Due in this playback " + std::to_string(Due()) +
                ": with sound " + std::to_string(Count(Outcome::Played)) + ", silent (the call made no sound) " +
                std::to_string(Count(Outcome::Silent)) + ", call refused " + std::to_string(Count(Outcome::Refused)) +
                ", without a puppet " + std::to_string(Count(Outcome::NoPuppet)) + ".\n";
            if (complements)
                out += "Melee sounds the call did not make again, played from the recording by the generic player: " +
                    std::to_string(complements) + ".\n";
            for (const Row& row : m_rows)
            {
                if (row.outcome == Outcome::Played)
                    continue;
                char line[200]{};
                std::snprintf(line, sizeof(line), "%-8s owner %s  at %.1f s  move 0x%08x  sounds %d  (%s)\n",
                    row.outcome == Outcome::Silent ? "SILENT" : row.outcome == Outcome::Refused ? "REFUSED" : "NOPUPPET",
                    census::IdText(row.owner).c_str(), row.eventMs / 1000.0, row.hash, row.sounds,
                    row.status.empty() ? "none" : row.status.c_str());
                out += line;
            }
            if (m_dropped)
                out += "(" + std::to_string(m_dropped) + " more hits not listed)\n";
            out += "How to read: a hit is played when the engine's melee entry, called on the puppet with the recorded move, "
                   "made a sound; SILENT, REFUSED and NOPUPPET are hits recorded with a sound that the replay played mute.\n";
            return out;
        }

        // MISSING findings for the notice at Stop (one per mute hit).
        std::vector<census::Finding> Findings(std::uint32_t playerId) const
        {
            std::vector<census::Finding> out;
            for (const Row& row : m_rows)
            {
                if (row.outcome == Outcome::Played)
                    continue;
                census::Finding f;
                f.kind = census::FindingKind::Missing;
                f.owner = row.owner;
                f.cls = row.owner == playerId ? census::OwnerClass::Player : census::OwnerClass::Ped;
                f.ref = row.hash;
                f.recorded = 1.0;
                f.spans.push_back({row.eventMs, row.eventMs + 1});
                out.push_back(f);
            }
            return out;
        }

    private:
        std::vector<Row> m_rows;
        std::uint32_t m_counts[4] = {};
        std::uint32_t m_dropped = 0;
    };
}
