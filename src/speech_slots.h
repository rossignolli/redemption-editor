#pragma once

// The replay's own management of the engine's speech wave slots.
//
// Offline (build 1491.50):
//   - the line's voice source (params +0xF8 -> line +0x218 at 0x2FABD75)
//     comes from the context metadata byte +0xD (0x2F965D0, written only
//     when +0xF8 is still 0, 0x2F9665E); the camp chatter says 3 ("bank");
//   - a source 3 line asks 0x2FBD044 through 0x2FBCE38 with an EMPTY class
//     mask (0x619B04(mask, 3) zeroes it): it only reuses a bank slot that
//     already holds the bank of (voice, context) with a live reference
//     (+0x10 != 0, +0x2C == bank). Nothing is loaded for it; the AI path
//     preloads the bank before it says the line, the replay's direct path
//     does not. No slot: 0x2FB6298 kills the line in the same frame (state 2,
//     stop reason 4, line +0xD0 = 0);
//   - a source 4 line ("ambient") asks the ambient slot getter 0x2FBD440: a
//     free ambient slot (+0x10 == 0, +0x18 != 0: 0x2C32CE0) is loaded for the
//     line's own wave (0x2C2D788), or the slot of the lowest real priority
//     (priority x curve of the distance to the listener, 0x2FBDE48) is taken
//     when the request's is higher. 0x2F986B8 turns a source 4 into 3 when
//     the bank is already resident (0x2FA1CF8), so the reuse then happens in
//     the same call.
// So the replay writes 4 into params +0xF8 before the direct entry (the
// metadata then leaves it), and manages the ambient slots itself: before a
// line is asked there must be a free slot; with none, the line waits or takes
// the slot of the replay's own weakest line (farther from the replay camera,
// off screen); a line nobody would hear is not asked while slots are short;
// the slots a puppet still holds with no line of the replay playing are given
// back through the engine's per-owner release 0x2FB1E94 (what the speech
// entity's destructor calls).
//
// Pure: unit tested in ReplayCoreTests (speech_slots_tests.cpp).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::speech::slots
{
    // params +0xF8: the voice source the line's slot is allocated by.
    constexpr std::uint32_t kParamsSource = 0xF8;
    constexpr std::int32_t kSourceBank = 3;
    constexpr std::int32_t kSourceAmbient = 4;

    // Ambient slot layout (0x2FBD440, 0x2FB1E94, 0x2C32CE0): 0x38 bytes,
    // +0x10 the live reference (0 when free), +0x18 the wave slot, +0x28 the
    // owner (speech entity), +0x30 the owner's priority.
    constexpr std::uint32_t kAmbientRef = 0x10;
    constexpr std::uint32_t kAmbientWave = 0x18;
    constexpr std::uint32_t kAmbientOwner = 0x28;
    constexpr std::uint32_t kAmbientPriority = 0x30;
    // The line's wave slot reference (request +0x10 = line +0xC0 + 0x10),
    // the value an ambient slot's +0x10 holds while the line uses it.
    constexpr std::uint32_t kLineWaveRef = 0xD0;

    // Slots kept for audible lines: a line beyond the audible distance is
    // not asked while fewer than this many slots are free.
    constexpr int kReserveForAudible = 2;
    // A line with less left than this is dropped instead of asked late.
    constexpr std::uint32_t kMinRemainingMs = 250;
    // Only a line asked this late (past its recorded start) can be dropped
    // for what is left of it. A short line (a grunt of 100 ms) asked on time
    // is not late.
    constexpr std::uint32_t kLateGraceMs = 200;
    // A held line is taken out for a due one only when the due one is this
    // much stronger (no ping-pong between two equal lines).
    constexpr float kEvictMargin = 0.25f;
    constexpr float kOnScreenWeight = 2.0f;
    constexpr float kFloor = 0.05f;

    // The replay's priority of a line by its speaker: 0 beyond the audible
    // distance, else a linear curve from 1 at the camera to kFloor at the
    // audible distance, doubled when the speaker is on screen.
    inline float Priority(float distanceM, bool onScreen, float audibleM)
    {
        if (audibleM <= 0.0f || distanceM < 0.0f)
            return onScreen ? kOnScreenWeight : 1.0f;
        if (distanceM >= audibleM)
            return 0.0f;
        float curve = 1.0f - distanceM / audibleM;
        if (curve < kFloor)
            curve = kFloor;
        return curve * (onScreen ? kOnScreenWeight : 1.0f);
    }

    inline bool Audible(float distanceM, float audibleM)
    {
        return audibleM <= 0.0f || distanceM < 0.0f || distanceM < audibleM;
    }

    // A line of the replay that holds an ambient slot now.
    struct Holder
    {
        std::size_t id = 0;     // the caller's index
        float priority = 0.0f;  // Priority() of its speaker now
    };

    enum class Decision : std::uint8_t
    {
        Ask = 0,           // a slot is free: ask the line now
        Evict = 1,         // stop `victim` (a weaker replay line), ask next tick
        Wait = 2,          // no slot: keep it waiting
        DropInaudible = 3, // nobody would hear it and slots are short
        DropLate = 4,      // too little of it left
    };

    inline const char* DecisionName(Decision decision)
    {
        switch (decision)
        {
        case Decision::Ask: return "ask";
        case Decision::Evict: return "evict";
        case Decision::Wait: return "wait";
        case Decision::DropInaudible: return "drop_inaudible";
        case Decision::DropLate: return "drop_late";
        default: return "?";
        }
    }

    struct Verdict
    {
        Decision decision = Decision::Ask;
        std::size_t victim = 0; // Holder::id when Evict
    };

    // `freeSlots` < 0: the census could not be read (the line is asked as
    // before). `remainingMs`: what is left of the recorded line at the
    // playhead (0 when unknown: not dropped for it).
    // `lateMs`: how far past its recorded start the line is asked (the
    // default: always late).
    inline Verdict Decide(int freeSlots, float duePriority, bool dueAudible, std::uint32_t remainingMs,
        bool remainingKnown, const std::vector<Holder>& holders, std::uint32_t lateMs = 0xFFFFFFFFu)
    {
        Verdict verdict;
        if (freeSlots < 0)
            return verdict;
        if (remainingKnown && remainingMs < kMinRemainingMs && lateMs >= kLateGraceMs)
        {
            verdict.decision = Decision::DropLate;
            return verdict;
        }
        if (!dueAudible && freeSlots < kReserveForAudible)
        {
            verdict.decision = Decision::DropInaudible;
            return verdict;
        }
        if (freeSlots > 0)
            return verdict;
        // The weakest replay line holding a slot, taken out when the due
        // line is clearly stronger.
        bool found = false;
        float weakest = 0.0f;
        for (const Holder& holder : holders)
        {
            if (!found || holder.priority < weakest)
            {
                found = true;
                weakest = holder.priority;
                verdict.victim = holder.id;
            }
        }
        if (found && duePriority > weakest * (1.0f + kEvictMargin) && duePriority > 0.0f)
        {
            verdict.decision = Decision::Evict;
            return verdict;
        }
        verdict.decision = Decision::Wait;
        return verdict;
    }

    // Order of the waiting lines each tick: the strongest first, then the
    // oldest wait.
    struct Waiting
    {
        std::size_t id = 0;
        float priority = 0.0f;
        std::uint64_t sinceUs = 0;
    };
    inline bool Before(const Waiting& a, const Waiting& b)
    {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.sinceUs < b.sinceUs;
    }

    // A slot a puppet holds while none of the replay's lines plays on it is
    // given back (0x2FB1E94), only when slots are short.
    inline bool ReleaseIdle(int freeSlots, bool ownerIsPuppet, bool ownerHasReplayLine)
    {
        return freeSlots >= 0 && freeSlots < kReserveForAudible && ownerIsPuppet && !ownerHasReplayLine;
    }

    // The source the request carries: ambient for a human line on the
    // direct path (SpeechAmbientSource=1), unless that voice and context
    // already failed to load in an ambient slot this session (then the
    // metadata's source).
    inline std::int32_t RequestSource(bool on, bool animal, bool ambientFailedBefore)
    {
        return on && !animal && !ambientFailedBefore ? kSourceAmbient : 0;
    }

    // The source of a wave slot retry (try counts from 1). The same voice and
    // context can die loading in the ambient slot early in a playback, go back
    // to the metadata's source and then find no bank slot for the whole 2 s
    // wait, while later lines of that key play. A retry alternates the two
    // sources: odd tries the source of the first request, even tries the other
    // one. Also with the ambient source off for first requests (the default):
    // a line the bank has no slot for gets the ambient slots on its even tries.
    inline std::int32_t RetrySource(bool on, bool animal, bool ambientFailedBefore, std::uint32_t tryIndex)
    {
        const std::int32_t first = RequestSource(on, animal, ambientFailedBefore);
        if (animal || tryIndex % 2 == 1)
            return first;
        return first == kSourceAmbient ? 0 : kSourceAmbient;
    }
}
