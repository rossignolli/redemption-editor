#pragma once

// Identity of a speech line (pure, unit tested).
//
// A line is the triple the engine resolved in the speech core (voice +0x110,
// partial context +0x114, variation +0x118 of the line data) plus the voice
// object it used (line +0x1E8: type byte [obj+0], partial name hash
// [obj+8]). The line data is written only at line creation (the line data
// init), so the same identity recorded and played means the same phrase on
// the same voice.

#include "game_sig_core.h"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

namespace replay::speech
{
    namespace sig = replay::sig;

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
    // variation), the text the subtitle step builds before its table remap.
    // Only compared with itself (record against playback).
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
    // reason 4 by the state-2 update (the line stop: no speech slot, or the
    // prepare of the line data refused), while the same line plays live
    // through the animal's own vocalization path (the AI step, asked by the
    // animation's speech tag). Such a line is asked again once through that
    // path (PLAY_ANIMAL_VOCALIZATION: the speech entity's slot +0x290, the
    // animal vocal entry, then the Say wrapper) with the context the recording
    // asked for. Only a line that never started, was stopped with that reason,
    // soon after the request, and not retried yet.
    constexpr std::uint32_t kAnimalRetryStopReason = 4;
    constexpr std::uint64_t kAnimalRetryWindowMs = 400;
    inline bool RetryAnimalLine(bool animal, bool started, bool retried, bool stopped, std::uint32_t stopReason,
        std::uint64_t ageMs)
    {
        return animal && !started && !retried && stopped && stopReason == kAnimalRetryStopReason &&
            ageMs <= kAnimalRetryWindowMs;
    }

    // The entry of that retry: the animal speech entity's constructor calls
    // the base speech constructor (the line list at +0x78/+0x80) and then
    // writes the animal speech vtable (also written by a second constructor;
    // the vtable just before it is a 0x38-byte object's, not the animal one).
    // In that vtable slot +0x290 is the native's entry: it hashes the name
    // and jumps to slot +0x288, the animal vocal entry, which takes the
    // context hash itself (writes it into params +0x150 and reads +0x18D,
    // its first bytes). The only .rdata qword holding the vocal entry is that
    // slot.
    inline const sig::Signature kAnimalSpeechCtor{"speech_animal_ctor",
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 A3 D0 00 00 00 00 48 8D 05 ?? ?? ?? ??", 0x2F90FCC};
    inline const sig::Signature kAnimalSpeechVtable{"speech_animal_vtable", kAnimalSpeechCtor, sig::Rel32{0x19, 0x1D},
        "", 0x37FE430};
    // The person speech entity's vtable, written by its constructor (the
    // census sizes a ped's speech entity by it, sound_census.cpp).
    inline const sig::Signature kPersonSpeechCtor{"speech_person_ctor",
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 8B D8 00 00 00 48 89 03", 0x2F91040};
    inline const sig::Signature kPersonSpeechVtable{"speech_person_vtable", kPersonSpeechCtor, sig::Rel32{0x11, 0x15},
        "", 0x37FD948};
    inline const sig::Signature kAnimalVocal{"speech_animal_vocal",
        "33 C0 41 89 90 50 01 00 00 41 38 80 8D 01 00 00 49 8B D0 0F 95 C0 41 89 80 30 01 00 00 E9 ?? ?? ?? ??",
        0x2FB2B4C};
    constexpr std::uint32_t kAnimalVocalHashSlot = 0x288;
    enum class AnimalVocalEntry : std::uint8_t
    {
        Ok,
        NotAnimalSpeech, // the speech entity's vtable is not the animal one
        SlotOther,       // the hash slot is not the expected function
    };
    // `vtable` and `hashSlot` as read from the speech entity; `animalVtable`
    // and `vocal` where the game has them (0: not found, nothing passes).
    inline AnimalVocalEntry CheckAnimalVocalEntry(std::uint64_t vtable, std::uint64_t hashSlot,
        std::uint64_t animalVtable, std::uint64_t vocal)
    {
        if (!animalVtable || vtable != animalVtable)
            return AnimalVocalEntry::NotAnimalSpeech;
        return vocal && hashSlot == vocal ? AnimalVocalEntry::Ok : AnimalVocalEntry::SlotOther;
    }

    // A line the replay created (native, direct) that never became audible.
    // The audible start is the only write of state 5 in the speech module
    // (inside the line's audible start, the start detour), so a
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

    // The engine paths into the speech core: the AI Say step, the direct
    // entry and the direct entry with a context; each calls the core once
    // (the core has no other caller). The direct entries have twins that call
    // the AI step instead, so each is reached from a caller of its own.
    inline const sig::Signature kAiSayStep{"speech_ai_say_step",
        "40 53 48 83 EC 20 48 8B DA 41 C6 80 9D 01 00 00 01", 0x2F9FB30};
    inline const sig::Signature kAiCoreCall{"speech_ai_core_call", kAiSayStep, sig::Offset{0x11},
        "E8 ?? ?? ?? ??", 0x2F9FB41};
    inline const sig::Signature kConversationCaller{"speech_conversation_caller",
        "48 89 5C 24 10 57 48 81 EC D0 01 00 00 48 8B 41 60", 0x2FA05E4};
    inline const sig::Signature kDirectEntry{"speech_direct_entry", kConversationCaller, sig::Rel32{0xF9, 0xFD},
        "48 89 5C 24 08 57 48 81 EC 10 02 00 00 48 8B DA 48 8B F9 49 8B D0 48 8D 4C 24 20 E8 ?? ?? ?? ?? 4C 8D 44 24 "
        "20 48 8B D3 48 8B CF E8 ?? ?? ?? ?? 48 8D 4C 24 20 8B D8 E8 ?? ?? ?? ?? 8B C3 48 8B 9C 24 20 02 00 00 48 81 "
        "C4 10 02 00 00 5F C3", 0x2FA1280};
    inline const sig::Signature kDirectCoreCall{"speech_direct_core_call", kDirectEntry, sig::Offset{0x2B},
        "E8 ?? ?? ?? ??", 0x2FA12AB};
    inline const sig::Signature kDirectCtxCaller{"speech_direct_ctx_caller",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B 79 08", 0x2F9FE3C};
    inline const sig::Signature kDirectCtxEntry{"speech_direct_ctx_entry", kDirectCtxCaller, sig::Rel32{0x161, 0x165},
        "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 10 02 00 00 8B DA 48 8B F9 49 8B D1 48 8D 4C 24 20 49 8B F0 E8 ?? "
        "?? ?? ?? 4C 8D 44 24 20 89 9C 24 70 01 00 00 48 8B D6 48 8B CF E8 ?? ?? ?? ?? 48 8D 4C 24 20 8B D8 E8 ?? ?? "
        "?? ?? 4C 8D 9C 24 10 02 00 00 8B C3 49 8B 5B 10 49 8B 73 18 49 8B E3", 0x2FA12D0};
    inline const sig::Signature kDirectCtxCoreCall{"speech_direct_ctx_core_call", kDirectCtxEntry, sig::Offset{0x39},
        "E8 ?? ?? ?? ??", 0x2FA1309};

    // The core's return addresses (call + 5) as RVAs, the caller a recorded
    // line keeps; 0 for a path not found (nothing matches it).
    struct CorePaths
    {
        std::uint32_t ai = 0;
        std::uint32_t direct = 0;
        std::uint32_t directCtx = 0;
    };

    // The return RVA of a 5-byte call at `site` (0 when not found).
    inline std::uint32_t ReturnRva(std::uintptr_t site, std::uintptr_t imageBase)
    {
        return site > imageBase ? static_cast<std::uint32_t>(site + 5 - imageBase) : 0;
    }

    // The paths as the 1491.50 exe has them, for the offline tools (no game
    // to resolve them in).
    inline CorePaths KnownCorePaths()
    {
        return CorePaths{kAiCoreCall.rva + 5, kDirectCoreCall.rva + 5, kDirectCtxCoreCall.rva + 5};
    }

    // Engine path that created the line, from the core's return address.
    inline const char* PathName(std::uint32_t caller, const CorePaths& paths)
    {
        if (!caller)
            return "other";
        if (caller == paths.ai)
            return "ai";
        if (caller == paths.direct)
            return "direct";
        if (caller == paths.directCtx)
            return "direct_ctx";
        return "other";
    }
}
