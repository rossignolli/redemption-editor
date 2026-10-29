#pragma once

// What the entity sound track (ESND: persistent sounds of an entity's own
// audio, such as a horse's skid, the tack loops, foley) did with its rows in a
// playback, per family, for ReplayDebug\<clip>.sound-check.txt and the log.
// The per row lines (POSE_V2_ENTITY_SOUND) stop after 40 rows, and the census
// sees a created sound but not whether the track started it, left it to the
// puppet, cut it at its recorded length or never tried it.
//
// A family is (owner, call site, sound set): the set is the low 32 bits of
// the ref (sound_generic_core.h kRefSetMask: the set's metadata offset), so
// a skid's entry 0x0E of 0x2f4361 and the step entries of the same set at
// the same call site share a row, and the sets stay apart. Each row of the
// track counts once per pass (a pause, scrub or jump starts a new one), with
// its outcome and whether the playhead ran slower than x1 at that moment.
//
// Pure (no game); unit tested in tests/sound_check_036_tests.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace replay::esndtally
{
    enum class Outcome : std::uint8_t
    {
        Played = 0,   // the track started it on the puppet
        ByPuppet = 1, // the puppet made the same sound itself (not started again)
        Refused = 2,  // the engine refused the start (status in reasons)
        TooFar = 3,   // the slot policy left it out (too far from the camera)
        Passed = 4,   // its span went by on this pass without a start (no puppet, no slot)
        // SoundGenericMode=packets: a packet of the generic player carries the row:
        // played there with its recorded adjusts, stop and release, not by this track.
        ToGeneric = 5,
    };
    constexpr std::uint8_t kOutcomeCount = 6;

    struct FamilyKey
    {
        std::uint32_t owner = 0;  // packed recorded id
        std::uint32_t caller = 0; // the call site recorded with the row (RVA)
        std::uint32_t set = 0;    // low 32 bits of the ref (or the hash)

        bool operator<(const FamilyKey& other) const
        {
            return std::tie(owner, caller, set) < std::tie(other.owner, other.caller, other.set);
        }
    };

    inline FamilyKey KeyOf(std::uint32_t owner, std::uint32_t caller, std::uint64_t ref)
    {
        FamilyKey key;
        key.owner = owner;
        key.caller = caller;
        key.set = static_cast<std::uint32_t>(ref & 0xFFFFFFFFull);
        return key;
    }

    struct FamilyTally
    {
        std::uint32_t due = 0;                     // rows reached on a pass (each outcome below is one)
        std::uint32_t outcomes[kOutcomeCount] = {};
        std::uint32_t slow = 0;                    // of the due, reached while the playhead ran slower than x1
        std::uint32_t lengthStops = 0;             // started ones stopped at their recorded length (StopAtRecordedLength)
        std::uint32_t endedByEngine = 0;           // started ones the engine ended inside their span
        std::uint32_t firstMs = 0xFFFFFFFFu;       // clip time of the first row due
        std::uint32_t lastMs = 0;
        std::map<std::string, std::uint32_t> reasons; // refusal status -> count
    };

    class Tally
    {
    public:
        void Clear() { m_families.clear(); }
        bool Empty() const { return m_families.empty(); }
        const std::map<FamilyKey, FamilyTally>& Families() const { return m_families; }

        void Note(const FamilyKey& key, Outcome outcome, std::uint32_t rowMs, bool slow, const std::string& status = {})
        {
            FamilyTally& t = m_families[key];
            ++t.due;
            ++t.outcomes[static_cast<std::uint8_t>(outcome)];
            if (slow)
                ++t.slow;
            t.firstMs = (std::min)(t.firstMs, rowMs);
            t.lastMs = (std::max)(t.lastMs, rowMs);
            if (outcome == Outcome::Refused)
                ++t.reasons[status.empty() ? std::string("unknown") : status];
        }
        void LengthStop(const FamilyKey& key) { ++m_families[key].lengthStops; }
        void EngineEnded(const FamilyKey& key) { ++m_families[key].endedByEngine; }

        struct Totals
        {
            std::uint32_t due = 0;
            std::uint32_t outcomes[kOutcomeCount] = {};
            std::uint32_t slow = 0;
            std::uint32_t lengthStops = 0;
            std::uint32_t endedByEngine = 0;
        };
        Totals Sum() const
        {
            Totals out;
            for (const auto& entry : m_families)
            {
                out.due += entry.second.due;
                for (std::uint8_t n = 0; n < kOutcomeCount; ++n)
                    out.outcomes[n] += entry.second.outcomes[n];
                out.slow += entry.second.slow;
                out.lengthStops += entry.second.lengthStops;
                out.endedByEngine += entry.second.endedByEngine;
            }
            return out;
        }

    private:
        std::map<FamilyKey, FamilyTally> m_families;
    };

    inline std::string IdText(std::uint32_t packed)
    {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%u:%u", packed >> 16, packed & 0xFFFFu);
        return text;
    }

    // A family has a problem when some row due was neither played nor left
    // to the puppet (refused, too far, passed).
    inline bool Problem(const FamilyTally& t)
    {
        return t.outcomes[static_cast<std::uint8_t>(Outcome::Refused)] ||
            t.outcomes[static_cast<std::uint8_t>(Outcome::TooFar)] ||
            t.outcomes[static_cast<std::uint8_t>(Outcome::Passed)];
    }

    inline std::string FamilyLine(const FamilyKey& key, const FamilyTally& t)
    {
        char line[400];
        std::snprintf(line, sizeof(line),
            "%s  owner %s  caller 0x%x set 0x%x  due %u (%u in slow motion), played %u, by the puppet %u, refused %u, "
            "too far %u, passed %u, to the generic player %u; cut at recorded length %u, ended by the engine %u; %u.%u s to %u.%u s\n",
            Problem(t) ? "SHORT " : "ok    ", IdText(key.owner).c_str(), key.caller, key.set, t.due, t.slow,
            t.outcomes[0], t.outcomes[1], t.outcomes[2], t.outcomes[3], t.outcomes[4], t.outcomes[5], t.lengthStops, t.endedByEngine,
            t.firstMs / 1000, (t.firstMs % 1000) / 100, t.lastMs / 1000, (t.lastMs % 1000) / 100);
        std::string out = line;
        if (!t.reasons.empty())
        {
            out += "    reasons:";
            bool first = true;
            for (const auto& reason : t.reasons)
            {
                out += std::string(first ? " " : "; ") + reason.first + " " + std::to_string(reason.second);
                first = false;
            }
            out += "\n";
        }
        return out;
    }

    // The report section: totals, then every family with a problem, then
    // the others (at most `limit` lines in all, the busiest first).
    inline std::string FormatSection(const Tally& tally, std::size_t limit = 80)
    {
        if (tally.Empty())
            return std::string();
        const Tally::Totals sum = tally.Sum();
        std::string out = "== entity sounds per family (ESND track: persistent sounds of an entity's own audio) ==\n";
        char line[400];
        std::snprintf(line, sizeof(line),
            "Rows due %u (%u in slow motion): played %u, left to the puppet %u, refused %u, too far %u, passed without a start %u, "
            "to the generic player %u; cut at their recorded length %u, ended by the engine %u.\n",
            sum.due, sum.slow, sum.outcomes[0], sum.outcomes[1], sum.outcomes[2], sum.outcomes[3], sum.outcomes[4], sum.outcomes[5],
            sum.lengthStops, sum.endedByEngine);
        out += line;
        std::vector<std::pair<FamilyKey, const FamilyTally*>> rows;
        for (const auto& entry : tally.Families())
            rows.push_back({entry.first, &entry.second});
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (Problem(*a.second) != Problem(*b.second))
                return Problem(*a.second);
            return a.second->due > b.second->due;
        });
        std::size_t shown = 0;
        for (const auto& row : rows)
        {
            if (shown++ >= limit)
            {
                out += "... and " + std::to_string(rows.size() - limit) + " more families (all in the log, POSE_V2_ENTITY_SOUND_FAMILY).\n";
                break;
            }
            out += FamilyLine(row.first, *row.second);
        }
        out += "How to read: one row per (owner, call site, sound set: the low 32 bits of the ref). \"by the puppet\" is a sound the\n"
               "puppet made itself (the census counts it on the puppet); \"to the generic player\" is a row a recorded packet carries\n"
               "(SoundGenericMode=packets: played there with its adjusts); \"cut at recorded length\" is a short own-audio sound the\n"
               "replay stopped when it had played as long as in the recording (clip time: in slow motion it lives longer on the wall clock).\n\n";
        return out;
    }

    // One log line per family (no limit; the end of a playback).
    inline std::string LogLine(const FamilyKey& key, const FamilyTally& t)
    {
        char line[400];
        std::snprintf(line, sizeof(line),
            "POSE_V2_ENTITY_SOUND_FAMILY,id=%s,caller=0x%x,set=0x%x,due=%u,slow=%u,played=%u,by_puppet=%u,refused=%u,too_far=%u,"
            "passed=%u,to_generic=%u,length_stops=%u,ended_by_engine=%u,first_ms=%u,last_ms=%u",
            IdText(key.owner).c_str(), key.caller, key.set, t.due, t.slow, t.outcomes[0], t.outcomes[1], t.outcomes[2],
            t.outcomes[3], t.outcomes[4], t.outcomes[5], t.lengthStops, t.endedByEngine, t.firstMs == 0xFFFFFFFFu ? 0u : t.firstMs, t.lastMs);
        return line;
    }
}
