#pragma once

// Identity of a speech line (pure, unit tested).
//
// A line is the triple the engine resolved in the speech core (voice +0x110,
// partial context +0x114, variation +0x118 of the line data) plus the voice
// object it used (line +0x1E8: type byte [obj+0], partial name hash
// [obj+8]). Build 1491.50: the line data is written only at line creation
// (0x2FA92C0), so the same identity recorded and played means the same
// phrase on the same voice.

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

namespace replay::speech
{
    struct Identity
    {
        std::uint32_t voice = 0;          // final hash
        std::uint32_t context = 0;        // partial hash
        std::int32_t variation = 0;       // 1-based
        std::uint32_t voiceObjectHash = 0; // finalized [obj+8], 0 when unknown
        std::uint8_t voiceObjectType = 0;  // [obj+0], 0 when unknown
    };

    enum Mismatch : std::uint32_t
    {
        kMismatchVoice = 1u << 0,
        kMismatchContext = 1u << 1,
        kMismatchVariation = 1u << 2,
        kMismatchVoiceObject = 1u << 3,
    };

    // joaat finalization (x*9; ^ x>>11; * 0x8001), as the engine applies it
    // to partial hashes.
    inline std::uint32_t Finalize(std::uint32_t hash)
    {
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }

    // Partial joaat of a text, lower-cased (the engine's name hash).
    inline std::uint32_t PartialHash(const char* text)
    {
        std::uint32_t hash = 0;
        for (; text && *text; ++text)
        {
            char c = *text;
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            hash += static_cast<std::uint8_t>(c);
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        return hash;
    }

    // Mod-side key of a line: joaat("%X_%02d", voice ^ finalize(ctx),
    // variation), the text the subtitle step (0x2F9E444) builds before its
    // table remap. Only compared with itself (record against playback).
    inline std::uint32_t Label(std::uint32_t voice, std::uint32_t contextPartial, std::int32_t variation)
    {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%X_%02d", voice ^ Finalize(contextPartial), variation);
        return Finalize(PartialHash(text));
    }

    // Fields where the played line differs from the recorded one. A field
    // the recording does not know (0) is not compared.
    inline std::uint32_t Compare(const Identity& recorded, const Identity& played)
    {
        std::uint32_t mask = 0;
        if (recorded.voice && recorded.voice != played.voice)
            mask |= kMismatchVoice;
        if (recorded.context != played.context)
            mask |= kMismatchContext;
        if (recorded.variation != played.variation)
            mask |= kMismatchVariation;
        if (recorded.voiceObjectHash &&
            (recorded.voiceObjectHash != played.voiceObjectHash ||
                (recorded.voiceObjectType && recorded.voiceObjectType != played.voiceObjectType)))
        {
            mask |= kMismatchVoiceObject;
        }
        return mask;
    }

    inline std::string MismatchText(std::uint32_t mask)
    {
        if (!mask)
            return "none";
        std::string text;
        const auto add = [&](std::uint32_t bit, const char* name) {
            if (!(mask & bit))
                return;
            if (!text.empty())
                text += '+';
            text += name;
        };
        add(kMismatchVoice, "voice");
        add(kMismatchContext, "context");
        add(kMismatchVariation, "variation");
        add(kMismatchVoiceObject, "voice_obj");
        return text;
    }

    // Replay timing. A recorded line is timed by its audible start; the
    // replay requests it earlier by the load time the engine needs between
    // the request and the start. `typicalLoadMs` is that load time as the
    // recording measured it on AI lines (requested and started by the
    // engine at once); a line's own request-to-start gap bounds it (a
    // scripted or conversation line waits longer than its load).
    inline std::uint32_t StartLead(std::uint32_t startMs, std::uint32_t coreMs, bool timedByStart,
        std::uint32_t typicalLoadMs)
    {
        if (!timedByStart || startMs < coreMs)
            return 0;
        return std::min(startMs - coreMs, typicalLoadMs);
    }

    // Median of the measured load times (0 when none).
    inline std::uint32_t MedianLoad(std::vector<std::uint32_t> loads)
    {
        if (loads.empty())
            return 0;
        std::sort(loads.begin(), loads.end());
        return loads[loads.size() / 2];
    }

    // An animal line the replay made through the direct entry that the engine
    // killed while loading: state 1 -> 2, then stopped one frame later with
    // reason 4 by the state-2 update (0x2FB6298 -> 0x2FBCCD4(line, 4) at
    // 0x2FB6440: no speech slot, or the prepare of the line data refused), while
    // the same line plays live through the animal's own vocalization path (the
    // AI step 0x2F9FB46, asked by the animation's speech tag). Such a line is
    // asked again once through that path (PLAY_ANIMAL_VOCALIZATION: speech entity
    // slot +0x290, 0x2FB2B4C -> Say wrapper 0x2F9FC84) with the context the
    // recording asked for. Only a line that never started, was stopped with that
    // reason, soon after the request, and not retried yet.
    constexpr std::uint32_t kAnimalRetryStopReason = 4;
    constexpr std::uint64_t kAnimalRetryWindowMs = 400;
    inline bool RetryAnimalLine(bool animal, bool started, bool retried, bool stopped, std::uint32_t stopReason,
        std::uint64_t ageMs)
    {
        return animal && !started && !retried && stopped && stopReason == kAnimalRetryStopReason &&
            ageMs <= kAnimalRetryWindowMs;
    }

    // The entry of that retry (build 1491.50): the animal speech entity's
    // constructor 0x2F90FCC calls the base speech constructor 0x2F910F8 (vtable
    // 0x37FD5F8, the line list at +0x78/+0x80) and then writes the vtable
    // 0x37FE430 (also written by 0x2F92F8E); 0x37FE428 is the vtable of a
    // 0x38-byte object (0x2FA0528), not the animal one. In 0x37FE430, slot +0x290
    // is 0x2FB2B70, the native's entry: it hashes the name (0x25FAAE8) and
    // jumps to slot +0x288, 0x2FB2B4C, which takes the context hash itself.
    // The only .rdata qword holding 0x2FB2B4C is 0x37FE6B8 = 0x37FE430 + 0x288.
    constexpr std::uint32_t kAnimalSpeechVtableRva = 0x37FE430;
    constexpr std::uint32_t kAnimalVocalHashSlot = 0x288;
    constexpr std::uint32_t kAnimalVocalHashRva = 0x2FB2B4C;
    enum class AnimalVocalEntry : std::uint8_t
    {
        Ok,
        NotAnimalSpeech, // the speech entity's vtable is not the animal one
        SlotOther,       // the hash slot is not the expected function
    };
    inline AnimalVocalEntry CheckAnimalVocalEntry(std::uint64_t vtableRva, std::uint64_t hashSlotRva)
    {
        if (vtableRva != kAnimalSpeechVtableRva)
            return AnimalVocalEntry::NotAnimalSpeech;
        return hashSlotRva == kAnimalVocalHashRva ? AnimalVocalEntry::Ok : AnimalVocalEntry::SlotOther;
    }

    // A line the replay created (native, direct) that never became audible.
    // The audible start is the only write of state 5 in the speech module
    // (0x2FB9DE9 inside 0x2FB99E8, the start detour; build 1491.50), so a
    // line is heard when the detour saw its start, or when the poll reads
    // it in state 5. It is silent when it left the ped's line list without
    // that (the engine killed it while loading: stop reason 4 in state 2,
    // 6 in state 3), or when the window went by without it. The window is
    // the load the recording measured on its AI lines plus a margin, at
    // least kSilentWindowMinMs, counted on both clocks (audio and playhead)
    // so a pause or a slowed playback does not judge a line early. A line
    // that left the list while the playback was paused or scrubbing, or
    // across a jump of the playhead, is not judged (the replay itself may
    // have stopped it). Without the start detour nothing is judged.
    constexpr std::uint32_t kSilentWindowMinMs = 1500;
    constexpr std::uint32_t kSilentLoadMarginMs = 500;
    constexpr std::int32_t kLineStateAudible = 5;

    inline std::uint32_t SilentWindowMs(std::uint32_t measuredLoadMs)
    {
        const std::uint64_t window = static_cast<std::uint64_t>(measuredLoadMs) + kSilentLoadMarginMs;
        return window < kSilentWindowMinMs ? kSilentWindowMinMs
            : window > 0xFFFFFFFFull     ? 0xFFFFFFFFu
                                         : static_cast<std::uint32_t>(window);
    }

    enum class LineOutcome : std::uint8_t
    {
        Pending = 0,  // not judged yet
        Started = 1,  // the audible start came
        Died = 2,     // left the line list without a start: silent
        TimedOut = 3, // still listed, no start within the window: silent
        Abandoned = 4, // left the list paused, scrubbing or across a jump: not judged
    };

    inline bool Silent(LineOutcome outcome) { return outcome == LineOutcome::Died || outcome == LineOutcome::TimedOut; }

    inline const char* OutcomeName(LineOutcome outcome)
    {
        switch (outcome)
        {
        case LineOutcome::Started: return "started";
        case LineOutcome::Died: return "died";
        case LineOutcome::TimedOut: return "timeout";
        case LineOutcome::Abandoned: return "abandoned";
        default: return "pending";
        }
    }

    struct LineWatch
    {
        bool startObserved = true; // the start detour is installed
        bool started = false;      // the detour saw this line's start
        bool listed = true;        // still in the ped's line list
        std::int32_t state = -1;   // its state when listed
        bool normalPlay = true;    // forward play, not paused nor scrubbing
        bool jumped = false;       // the playhead moved back, or far past the audio clock, since the request
        std::uint64_t audioAgeMs = 0; // since the request, audio clock
        std::uint64_t playAgeMs = 0;  // since the request, playhead
        std::uint32_t windowMs = kSilentWindowMinMs;
    };

    inline LineOutcome JudgeLine(const LineWatch& watch)
    {
        if (!watch.startObserved)
            return LineOutcome::Pending;
        if (watch.started || (watch.listed && watch.state == kLineStateAudible))
            return LineOutcome::Started;
        if (!watch.listed)
            return watch.normalPlay && !watch.jumped ? LineOutcome::Died : LineOutcome::Abandoned;
        if (watch.normalPlay && !watch.jumped && watch.audioAgeMs >= watch.windowMs && watch.playAgeMs >= watch.windowMs)
            return LineOutcome::TimedOut;
        return LineOutcome::Pending;
    }

    // The playhead jumped since the request: it went back, or it advanced
    // more than the audio clock allows (a seek forward). kJumpSlackMs absorbs
    // the tick granularity and a playback faster than real time.
    constexpr std::uint64_t kJumpSlackMs = 1000;
    inline bool PlayheadJumped(std::uint32_t requestPlayMs, std::uint32_t nowPlayMs, std::uint64_t audioAgeMs,
        double rate = 1.0)
    {
        if (nowPlayMs < requestPlayMs)
            return true;
        const double r = rate < 1.0 ? 1.0 : rate;
        return static_cast<double>(nowPlayMs - requestPlayMs) > static_cast<double>(audioAgeMs) * r + kJumpSlackMs;
    }

    // The playback's speech counters (POSE_V2_PLAYBACK_SPEECH_TOTAL, an
    // essential line: written with Debug=0).
    struct LineCounts
    {
        std::uint64_t created[2] = {};  // [0] human, [1] animal
        std::uint64_t started[2] = {};
        std::uint64_t died[2] = {};
        std::uint64_t timedOut[2] = {};
        std::uint64_t remade = 0;     // silent animal lines the retry asked again (counted silent too)
        std::uint64_t abandoned = 0;
        // Animal lines that started on the line the retry made after the first
        // one died (counted in started too).
        std::uint64_t startedRemade = 0;

        void Created(bool animal) { ++created[animal ? 1 : 0]; }
        void Judged(bool animal, LineOutcome outcome)
        {
            const int k = animal ? 1 : 0;
            if (outcome == LineOutcome::Started)
                ++started[k];
            else if (outcome == LineOutcome::Died)
                ++died[k];
            else if (outcome == LineOutcome::TimedOut)
                ++timedOut[k];
            else if (outcome == LineOutcome::Abandoned)
                ++abandoned;
        }
        std::uint64_t Silent(bool animal) const { return died[animal ? 1 : 0] + timedOut[animal ? 1 : 0]; }

        // ",speech_created=..,speech_started=..,speech_silent=..,...", pending
        // being the lines still unjudged when the playback ended.
        std::string Text(std::uint64_t pending, bool startObserved) const
        {
            const auto sum = [](const std::uint64_t (&v)[2]) { return v[0] + v[1]; };
            std::string text;
            const auto add = [&text](const char* key, std::uint64_t value) {
                text += ',';
                text += key;
                text += '=';
                text += std::to_string(value);
            };
            add("speech_created", sum(created));
            add("speech_started", sum(started));
            add("speech_silent", Silent(false) + Silent(true));
            add("created_human", created[0]);
            add("started_human", started[0]);
            add("silent_human", Silent(false));
            add("killed_loading_human", died[0]);
            add("created_animal", created[1]);
            add("started_animal", started[1]);
            add("silent_animal", Silent(true));
            add("killed_loading_animal", died[1]);
            add("silent_remade", remade);
            add("started_remade", startedRemade);
            add("speech_unjudged", abandoned + pending);
            add("start_observer", startObserved ? 1 : 0);
            return text;
        }
    };

    // Engine path that created the line, from the core's return address.
    inline const char* PathName(std::uint32_t caller)
    {
        switch (caller)
        {
        case 0x2F9FB46: return "ai";
        case 0x2FA12B0: return "direct";
        case 0x2FA130E: return "direct_ctx";
        default: return "other";
        }
    }
}
