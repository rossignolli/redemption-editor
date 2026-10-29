#pragma once

// Horse and character sound outcomes of one playback, so a refused
// vocalization or a missing wet step shows in the log and in
// ReplayDebug\<clip>.sound-check.txt without anyone having to hear it.
//
// Why a tally of its own: the sound census compares the sounds the engine
// created in the recording with the ones it created in the replay. A line of
// speech that the engine refused (result 64) or killed while loading never
// creates a sound, so the census cannot see it; the speech track can, line
// by line. The same holds for a step whose wet layer the puppet's foot never
// asked for.
//
// Also the choice of the path an animal line is replayed with (the native's
// path, audio::PlayAnimalVocal):
//   1. Pinned: the native path with the recorded context, voice and
//      variation (the animal pipeline keeps a set voice and a positive
//      variation);
//   2. Context: the native path with the context only (the pipeline picks
//      voice and variation, the phrase may differ; a vocalization of the
//      right kind over silence);
//   3. Direct: PlaySpeechDirect, as the last resort.
// A person's line keeps the direct path alone.
//
// Pure (no game); unit tested.

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace replay::horsesound
{
    enum class AnimalAttempt : std::uint8_t
    {
        Pinned,
        Context,
        Direct,
        Done,
    };

    inline const char* AttemptName(AnimalAttempt attempt)
    {
        switch (attempt)
        {
        case AnimalAttempt::Pinned: return "native_pinned";
        case AnimalAttempt::Context: return "native_context";
        case AnimalAttempt::Direct: return "direct";
        case AnimalAttempt::Done: return "done";
        }
        return "?";
    }

    // The first path of a line: animals take the native path first when it
    // is on (ini AnimalVocalNative, default 1), everyone else the direct one.
    inline AnimalAttempt FirstAttempt(bool animal, bool nativeOn)
    {
        return animal && nativeOn ? AnimalAttempt::Pinned : AnimalAttempt::Direct;
    }

    // The next path after one that did (created) or did not create a line.
    // Pinned without a recorded variation is the same request as Context,
    // so it is not asked twice.
    inline AnimalAttempt NextAttempt(AnimalAttempt tried, bool created, bool hasVariation = true)
    {
        if (created)
            return AnimalAttempt::Done;
        switch (tried)
        {
        case AnimalAttempt::Pinned: return hasVariation ? AnimalAttempt::Context : AnimalAttempt::Direct;
        case AnimalAttempt::Context: return AnimalAttempt::Direct;
        default: return AnimalAttempt::Done;
        }
    }

    // Whether a line goes on to the direct path once the native attempts are
    // over. Only Direct does: a line a native attempt created (Done) is
    // played, and an attempt left at Pinned or Context means the loop stopped
    // on a created line too, so it must not be asked again through the
    // direct path.
    inline bool RunsDirect(AnimalAttempt afterNative)
    {
        return afterNative == AnimalAttempt::Direct;
    }

    // ------------------------------------------------------------ tallies

    enum class SpeakerClass : std::uint8_t
    {
        Horse,
        Animal,
        Person,
    };

    inline const char* ClassToken(SpeakerClass cls)
    {
        switch (cls)
        {
        case SpeakerClass::Horse: return "horse";
        case SpeakerClass::Animal: return "animal";
        case SpeakerClass::Person: return "person";
        }
        return "?";
    }

    // One speaker's lines in the compared span (normal forward play).
    struct SpeechTally
    {
        SpeakerClass cls = SpeakerClass::Person;
        std::uint32_t recorded = 0;  // lines due
        std::uint32_t played = 0;    // a line was created for it (any path)
        std::uint32_t refused = 0;   // no path created a line
        std::uint32_t noPuppet = 0;  // the speaker had no ready puppet
        std::uint32_t killed = 0;    // created, then stopped while loading and not made again
        // A person's line created and dead before its audible start
        // (POSE_V2_SPEECH_SILENT: revalidation with the rules' code,
        // e1_wave_missing, killed_other...).
        std::uint32_t died = 0;
        std::uint32_t pinned = 0;    // played through the native path, recorded identity asked
        std::uint32_t context = 0;   // played through the native path, context only
        std::uint32_t direct = 0;    // played through the direct path
        std::uint32_t mismatch = 0;  // played, but the created line is another phrase
        std::map<std::string, std::uint32_t> reasons; // refusal reason -> count
        // When the replayed lines were heard against their recorded times:
        // the start delay, and the worst hole (a gap between two heard starts
        // longer than the recorded gap).
        std::uint64_t delaySumMs = 0;
        std::uint32_t delayCount = 0;
        std::uint32_t delayMaxMs = 0;
        std::uint32_t worstHoleMs = 0;
        std::uint32_t worstHoleAtMs = 0;
        std::uint32_t lastEventMs = 0;
        std::uint32_t lastStartMs = 0;
        bool hasLast = false;

        // One heard start (normal play only: a jump forgets the previous one).
        void NoteStart(std::uint32_t eventMs, std::uint32_t startMs, bool normalPlay)
        {
            const std::uint32_t delay = startMs > eventMs ? startMs - eventMs : 0;
            delaySumMs += delay;
            ++delayCount;
            if (delay > delayMaxMs)
                delayMaxMs = delay;
            if (normalPlay && hasLast && eventMs > lastEventMs && startMs > lastStartMs)
            {
                const std::uint32_t recordedGap = eventMs - lastEventMs;
                const std::uint32_t heardGap = startMs - lastStartMs;
                if (heardGap > recordedGap && heardGap - recordedGap > worstHoleMs)
                {
                    worstHoleMs = heardGap - recordedGap;
                    worstHoleAtMs = startMs;
                }
            }
            hasLast = normalPlay;
            lastEventMs = eventMs;
            lastStartMs = startMs;
        }
        std::uint32_t DelayAvgMs() const { return delayCount ? static_cast<std::uint32_t>(delaySumMs / delayCount) : 0; }
    };

    // One entity's footsteps in the compared span.
    struct FootTally
    {
        bool horse = false;
        std::uint32_t recorded = 0;  // steps due
        std::uint32_t played = 0;    // the step entry ran on the puppet
        std::uint32_t refused = 0;   // no puppet, foot audio disabled, ...
        std::uint32_t wetKnown = 0;  // steps with a recorded wetness (FOOT v3)
        std::uint32_t wet = 0;       // ... above 0: the wet layer should play
        std::uint32_t wetPlayed = 0; // wet steps played with the wetness set
        // Wet steps played while the ped's +0xB0 bit 7 was set: the step
        // skips the wet layer then (0x318D346), whatever the wetness.
        std::uint32_t wetGateClosed = 0;
        std::map<std::string, std::uint32_t> reasons;
    };

    struct Tallies
    {
        std::map<std::string, SpeechTally> speech; // key: entity id text ("0:2172")
        std::map<std::string, FootTally> feet;

        void Clear()
        {
            speech.clear();
            feet.clear();
        }

        struct Totals
        {
            std::uint32_t recorded = 0;
            std::uint32_t played = 0;
            std::uint32_t refused = 0;
            std::uint32_t killed = 0;
            std::uint32_t noPuppet = 0;
            std::uint32_t died = 0;
        };

        Totals SpeechTotals(bool animalsOnly) const
        {
            Totals totals;
            for (const auto& entry : speech)
            {
                if (animalsOnly && entry.second.cls == SpeakerClass::Person)
                    continue;
                totals.recorded += entry.second.recorded;
                totals.played += entry.second.played;
                totals.refused += entry.second.refused;
                totals.killed += entry.second.killed;
                totals.noPuppet += entry.second.noPuppet;
                totals.died += entry.second.died;
            }
            return totals;
        }
    };

    // Percent of the lines that had a puppet and were not heard (refused or
    // killed while loading); the in-game acceptance is a few percent.
    inline double SilentPercent(const Tallies::Totals& totals)
    {
        const std::uint32_t asked = totals.recorded > totals.noPuppet ? totals.recorded - totals.noPuppet : 0;
        if (!asked)
            return 0.0;
        return 100.0 * static_cast<double>(totals.refused + totals.killed + totals.died) / static_cast<double>(asked);
    }

    // A steps line of a horse is a problem when it played fewer steps than
    // due or its wet steps were not played wet.
    inline bool FootProblem(const FootTally& tally)
    {
        return tally.played < tally.recorded || tally.wetPlayed < tally.wet || tally.wetGateClosed;
    }

    inline bool SpeechProblem(const SpeechTally& tally)
    {
        return tally.refused || tally.killed || tally.died;
    }

    // One dead line of a person (the line counted as played when it was
    // created): `cause` as POSE_V2_SPEECH_SILENT names it, the state 3
    // handler's code when one was seen (-1 none).
    inline void NoteDied(SpeechTally& tally, const char* cause, long long state3Code)
    {
        ++tally.died;
        if (tally.played)
            --tally.played;
        std::string reason = std::string("died_") + (cause ? cause : "unknown");
        if (state3Code >= 0)
            reason += "_code_" + std::to_string(state3Code);
        ++tally.reasons[reason];
    }

    inline std::string ReasonsText(const std::map<std::string, std::uint32_t>& reasons)
    {
        std::string out;
        for (const auto& reason : reasons)
        {
            if (!out.empty())
                out += "; ";
            out += reason.first + " " + std::to_string(reason.second);
        }
        return out;
    }

    // ------------------------------------------------------------ report

    // The section of the report (no accents, as the rest of the file). Lists
    // every horse and every speaker with a problem first, then the others in
    // one line each. Empty when there is nothing to tell.
    inline std::string FormatSection(const Tallies& tallies)
    {
        if (tallies.speech.empty() && tallies.feet.empty())
            return std::string();
        std::string out;
        char line[512];
        const Tallies::Totals animals = tallies.SpeechTotals(true);
        const Tallies::Totals all = tallies.SpeechTotals(false);
        out += "== speech and vocalizations (speech track; the census does not see refused speech) ==\n";
        std::snprintf(line, sizeof(line),
            "Animal vocalizations: %u due, %u played, %u refused, %u killed while loading, %u without a puppet (%.1f%% silent).\n",
            animals.recorded, animals.played, animals.refused, animals.killed, animals.noPuppet, SilentPercent(animals));
        out += line;
        std::snprintf(line, sizeof(line), "All speech: %u due, %u played, %u refused, %u killed while loading, %u died before their start.\n",
            all.recorded, all.played, all.refused, all.killed, all.died);
        out += line;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const auto& entry : tallies.speech)
            {
                const SpeechTally& t = entry.second;
                if ((pass == 0) != SpeechProblem(t))
                    continue;
                std::snprintf(line, sizeof(line),
                    "%s  owner %s (%s)  due %u, played %u (native with identity %u, native context only %u, direct %u, other line %u), refused %u, killed while loading %u, died before the start %u%s\n",
                    SpeechProblem(t) ? "SILENT" : "ok    ", entry.first.c_str(), ClassToken(t.cls), t.recorded, t.played,
                    t.pinned, t.context, t.direct, t.mismatch, t.refused, t.killed, t.died, t.noPuppet ? ", no puppet" : "");
                out += line;
                if (!t.reasons.empty())
                    out += "    reasons: " + ReasonsText(t.reasons) + "\n";
            }
        }
        if (!tallies.feet.empty())
        {
            out += "\n== footsteps per entity (footstep track) ==\n";
            for (int pass = 0; pass < 2; ++pass)
            {
                for (const auto& entry : tallies.feet)
                {
                    const FootTally& t = entry.second;
                    if (!t.horse && pass == 0)
                        continue; // people: only the second pass, and only with a problem
                    if (pass == 1 && (t.horse || !FootProblem(t)))
                        continue;
                    const bool wetUnknown = t.recorded && !t.wetKnown;
                    std::snprintf(line, sizeof(line),
                        "%s  owner %s (%s)  steps due %u, played %u, refused %u; wet %u, played wet %u%s%s\n",
                        FootProblem(t) ? "MISSED" : "ok    ", entry.first.c_str(), t.horse ? "horse" : "person/animal",
                        t.recorded, t.played, t.refused, t.wet, t.wetPlayed,
                        t.wetGateClosed ? " (bit 7 of +0xB0 set on the puppet: the engine skips the wet layer)" : "",
                        wetUnknown ? " (foot wetness not recorded: a clip from before the wetness track, the wet layer does not play)" : "");
                    out += line;
                    if (!t.reasons.empty())
                        out += "    reasons: " + ReasonsText(t.reasons) + "\n";
                }
            }
        }
        out += "\n";
        return out;
    }

    // One summary line for the top of the report.
    inline std::string SummaryLine(const Tallies& tallies)
    {
        const Tallies::Totals animals = tallies.SpeechTotals(true);
        std::uint32_t wet = 0;
        std::uint32_t wetPlayed = 0;
        std::uint32_t horseSteps = 0;
        std::uint32_t horsePlayed = 0;
        for (const auto& entry : tallies.feet)
        {
            if (!entry.second.horse)
                continue;
            wet += entry.second.wet;
            wetPlayed += entry.second.wetPlayed;
            horseSteps += entry.second.recorded;
            horsePlayed += entry.second.played;
        }
        char line[400];
        std::snprintf(line, sizeof(line),
            "SPEECH AND HORSES: animal vocalizations %u due, %u silent (%.1f%%); horse steps %u due, %u played; wet steps %u, played wet %u.\n",
            animals.recorded, animals.refused + animals.killed, SilentPercent(animals), horseSteps, horsePlayed, wet, wetPlayed);
        std::string out = line;
        // People's lines the replay created that died before their start
        // (the census cannot see a line that never sounded).
        const Tallies::Totals all = tallies.SpeechTotals(false);
        if (all.died)
        {
            std::snprintf(line, sizeof(line), "SPEECH OF PEOPLE: %u lines created by the replay died before their start (see the speech section).\n",
                all.died);
            out += line;
        }
        return out;
    }

    // Puts the summary line under the report's RESUMO line and the section
    // before its first "== " section (or at the end).
    inline std::string InsertIntoReport(const std::string& report, const std::string& summary, const std::string& section)
    {
        std::string out = report;
        if (!section.empty())
        {
            const std::size_t at = out.find("\n== ");
            if (at == std::string::npos)
            {
                const std::size_t how = out.find("\nHow to read");
                if (how == std::string::npos)
                    out += "\n" + section;
                else
                    out.insert(how + 1, section);
            }
            else
            {
                out.insert(at + 1, section);
            }
        }
        if (!summary.empty())
        {
            const std::size_t resumo = out.find("SUMMARY:");
            const std::size_t end = resumo == std::string::npos ? std::string::npos : out.find('\n', resumo);
            if (end == std::string::npos)
                out = summary + out;
            else
                out.insert(end + 1, summary);
        }
        return out;
    }
}
