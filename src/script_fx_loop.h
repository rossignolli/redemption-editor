#pragma once

// The looped particle effects a script starts (START_PARTICLE_FX_LOOPED_ON_ENTITY_BONE
// and the other looped starts) and the request that keeps them alive every frame.
//
// The engine keeps a script looped effect playing only while the script slot
// walk gets its registered entry again every frame (same owner key 0x10000000
// and id, hash and asset 0). Dropping the calls without an effect hash would
// record only the start (e.g. the dynamite fuse, proj_dynamite_trail). So a
// call without a hash counts for the live key it gets, with that key's effect;
// a key not live yet (started before the recording) takes the effect the
// instance holds (rule +0x200). Offline dump, build 1491.50.
// Research: docs/research/fx-fires-decals-effects.md
//
// Pure (no game).

#include "game_sig_core.h"

#include <cstdint>

namespace replay::scriptfx
{
    constexpr std::uint64_t kScriptOwner = 0x10000000;   // registered owner key of script looped effects
    // The calls whose return addresses are the recorded sites of their start
    // and of the per-frame get (hash 0): fx_observer.h ScriptLoopStartSite and
    // ScriptLoopKeepAliveSite.
    inline const sig::Signature kScriptFxCtor{"fx_script_loop_ctor",
        "48 89 5C 24 10 55 56 57 48 83 EC 60 33 ED 48 8D 05 ?? ?? ?? ?? 66 89 69 08 49 8B F9 48 89 69 0C", 0xEB6704};
    inline const sig::Signature kScriptFxStartCall{"fx_script_loop_start_call", kScriptFxCtor, sig::Offset{0x10D},
        "E8 ?? ?? ?? ?? 89 43 10 48 8B C3", 0xEB6811};
    inline const sig::Signature kScriptFxKeepAlive{"fx_script_loop_keep_alive",
        "40 53 48 83 EC 20 80 B9 F4 6A 00 00 00 48 8B D9 74 1E 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 "
        "0F 10 83 F0 6A 00 00",
        0x1472AD4};
    inline const sig::Signature kScriptFxKeepAliveCall{"fx_script_loop_keep_alive_call", kScriptFxKeepAlive,
        sig::Offset{0x1E}, "E8 ?? ?? ?? ?? F3 0F 10 83 F0 6A 00 00", 0x1472AF2};

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

    // Playback check of the script looped tracks (site kScriptFxStartCall): how long
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
