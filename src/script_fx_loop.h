#pragma once

// The looped particle effects a script starts (START_PARTICLE_FX_LOOPED_ON_ENTITY_BONE
// and the other looped starts) and the request that keeps them alive every frame.
//
// Build 1491.50:
//   - the script fx object constructor 0xEB6704 calls the script looped start
//     0x2C4FFD8, which gets or creates the registered entry through the
//     wrapper 0x2C3A1C0 with the owner key 0x10000000 (mov ecx, 0x10000000 at
//     0x2C50048) and the script slot id ([slot+0x20]), the effect hash and the
//     asset; the registered core 0x2C49B08 is the one the fx observer detours,
//     and the site it records for this start is 0xEB6816 (the return into the
//     constructor, two frames above the wrapper);
//   - every frame 0x2C526F4 (called by 0x1472AD4, recorded site 0x1472AF7)
//     walks the active script slots and gets each entry again through the
//     same wrapper with the same owner key and id but a hash and an asset of
//     0 (mov dword [rsp+0x80], 0 at 0x2C5276C): the call only marks the entry
//     as got this frame (+0x28), which keeps it playing (registry update
//     0x2C52554 stops an entry not got). STOP_PARTICLE_FX_LOOPED clears the
//     slot and the entry is not got any more.
// Dropping the calls without an effect hash would record only the start of a
// script looped effect (e.g. the dynamite fuse, proj_dynamite_trail). A call
// without a hash counts for the live key it gets (same owner and id), with
// that key's effect; a key not live yet (a script effect started before the
// recording) takes the effect the instance holds (rule +0x200, the same read
// GuardedClearBit compares with the track's hash).
//
// Pure (no game).

#include <cstdint>

namespace replay::scriptfx
{
    constexpr std::uint64_t kScriptOwner = 0x10000000;   // registered owner key of script looped effects
    constexpr std::uint32_t kStartSite = 0xEB6816;       // recorded site of their start
    constexpr std::uint32_t kKeepAliveSite = 0x1472AF7;  // recorded site of the per-frame get (hash 0)

    // How a registered call without an effect hash was resolved.
    enum class Hashless : std::uint8_t
    {
        NotHashless, // the call names its effect
        LiveKey,     // the live track of the same key: its effect
        Identity,    // no live key: the effect the instance holds
        Mismatch,    // the instance holds another effect than the live key's: dropped
        Unresolved,  // no instance, or nothing readable: dropped
    };

    struct HashlessResult
    {
        Hashless how = Hashless::NotHashless;
        std::uint32_t hash = 0;
        bool Keep() const { return how == Hashless::NotHashless || how == Hashless::LiveKey || how == Hashless::Identity; }
    };

    // callHash: the hash the call passed; inst: the instance it returned;
    // liveFound/liveHash: the live recording key with the call's owner and
    // id and its track's effect; identityRead/identityHash: the instance's
    // rule +0x200 read (guarded).
    inline HashlessResult ResolveHashless(std::uint32_t callHash, std::uint64_t inst, bool liveFound,
        std::uint32_t liveHash, bool identityRead, std::uint32_t identityHash)
    {
        HashlessResult result;
        if (callHash)
        {
            result.hash = callHash;
            return result;
        }
        if (!inst)
        {
            result.how = Hashless::Unresolved;
            return result;
        }
        if (liveFound && liveHash)
        {
            if (identityRead && identityHash && identityHash != liveHash)
            {
                result.how = Hashless::Mismatch;
                return result;
            }
            result.how = Hashless::LiveKey;
            result.hash = liveHash;
            return result;
        }
        if (identityRead && identityHash)
        {
            result.how = Hashless::Identity;
            result.hash = identityHash;
            return result;
        }
        result.how = Hashless::Unresolved;
        return result;
    }

    // Playback check of the script looped tracks (site kStartSite): how long
    // each one was played against how long it was recorded.
    struct LoopTrackPlay
    {
        std::uint32_t recStartMs = 0;
        std::uint32_t recEndMs = 0;
        bool entered = false;       // requested at least once in normal play
        std::uint32_t firstMs = 0;  // first and last playhead it was requested at
        std::uint32_t lastMs = 0;
        bool passedEnd = false;     // normal play went past its recorded end after it was entered
    };

    constexpr std::uint32_t kOneTickMs = 100;    // recorded span under this: the start only (older clips)
    constexpr std::uint32_t kShortSlackMs = 250; // play may end this much before the recorded end

    struct LoopTally
    {
        std::uint32_t tracks = 0;
        std::uint32_t oneTick = 0;  // recorded as a single tick
        std::uint32_t entered = 0;
        std::uint32_t complete = 0; // entered near its start and played past its end
        std::uint32_t shortPlay = 0; // complete, but the requests stopped before the recorded end
        std::uint64_t recMs = 0;
        std::uint64_t playedMs = 0; // of the complete ones

        void Add(const LoopTrackPlay& t)
        {
            ++tracks;
            const std::uint32_t span = t.recEndMs > t.recStartMs ? t.recEndMs - t.recStartMs : 0;
            recMs += span;
            if (span < kOneTickMs)
                ++oneTick;
            if (!t.entered)
                return;
            ++entered;
            if (!t.passedEnd || t.firstMs > t.recStartMs + kShortSlackMs)
                return;
            ++complete;
            playedMs += t.lastMs > t.firstMs ? t.lastMs - t.firstMs : 0;
            if (t.lastMs + kShortSlackMs < t.recEndMs)
                ++shortPlay;
        }

        // "none" (no script looped track), "one_tick" (an older clip: the
        // tracks end at their start), "short" (played less than recorded),
        // "ok".

        const char* Verdict() const
        {
            if (!tracks)
                return "none";
            if (oneTick)
                return "one_tick";
            if (shortPlay)
                return "short";
            return "ok";
        }
    };
}
