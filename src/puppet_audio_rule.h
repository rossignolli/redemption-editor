#pragma once

// Rules that keep a puppet from doubling the sounds the replay already plays
// from the recording. While a clip plays, a replay entity sounds only what its
// recorded tracks carry; its own animation and AI would make the same sounds on top.
//
// One-shots: every create-and-play goes through the core 0x2589A40 (build
// 1491.50; 0x150-byte queue entries, audio entity at +0x140, ref at +0x148),
// drained by 0x258A348 from the audio update 0x2F1CE4C. The recording keeps
// those made on a recorded ped's audio (SND kind 1) and playback replays them.
// A create-and-play on a puppet's audio (or the hidden player's and his mount's)
// is refused unless the replay itself makes it (ReplayEmitScope, or inside a
// footstep or melee call the replay runs). Refusing takes the core's own early
// return (0x2589B6D: al = 0, no queue entry, init params untouched), the path an
// unregistered audio entity takes.
//
// Pure (no game); unit tested in tests/sound_particles_tests.cpp.

#include <cstddef>
#include <cstdint>

namespace replay::puppetaudio
{
    // The audio entity lies in one of the holders' spans [holder, holder + span).
    inline bool InSpans(std::uint64_t audio, const std::uint64_t* holders, std::size_t count, std::uint64_t span)
    {
        if (!audio)
            return false;
        for (std::size_t n = 0; n < count; ++n)
            if (holders[n] && audio >= holders[n] && audio - holders[n] < span)
                return true;
        return false;
    }

    // Whether a create-and-play is the puppet's own duplicate.
    inline bool RefuseOwnOneShot(bool active, bool replayEmitting, bool insideReplayedSystem, bool onPuppetAudio)
    {
        return active && onPuppetAudio && !replayEmitting && !insideReplayedSystem;
    }

    // Same rule for two more tracks.
    //
    // 1. Carried weapon one-shots (SND kind 4). The recorder files every
    //    create-and-play on a recorded weapon's audio (CWeapon at [object + 0x448],
    //    its audio at +0x100, span kWeaponOneShotSpan from the CWeapon). Without the
    //    rule the weapon puppet plays its own copy about 20 ms after the replayed one.
    constexpr std::uint64_t kWeaponOneShotSpan = 0x200;

    // 2. Persistent sounds of the ped's own audio without a tracker (ESND pedAudio):
    //    foley and clothing through the persistent cores 0x2589124 (by ref) and
    //    0x2589058 (by hash). The puppet creates the same entry of another sound set
    //    at the recorded time. Refused when the clip's ESND track carries ped audio
    //    sounds of the same engine call site (the return address into the core's
    //    caller), so a call site the recording never saw is left alone. Refusing is
    //    the cores' own early return for an unregistered audio entity
    //    (0x258922E / 0x2589101: *out = 0, nothing created).
    inline bool CallerCarried(std::uint32_t caller, const std::uint32_t* callers, std::size_t count)
    {
        if (!caller)
            return false;
        for (std::size_t n = 0; n < count; ++n)
            if (callers[n] == caller)
                return true;
        return false;
    }

    inline bool RefuseOwnPersistent(bool active, bool replayEmitting, bool insideReplayedSystem, bool onPuppetAudio,
        bool hasTracker, bool callerCarried)
    {
        return active && onPuppetAudio && !hasTracker && callerCarried && !replayEmitting && !insideReplayedSystem;
    }

    // The ped audio hold (time to see whether the puppet makes the sound itself)
    // is not needed while its own are refused: the recorded sound starts at its time.
    inline std::uint32_t PedAudioHoldMs(bool pedAudio, bool ownAudio, bool ownPersistentRefused, std::uint32_t holdMs)
    {
        if (!ownAudio)
            return 0;
        return pedAudio && ownPersistentRefused ? 0u : holdMs;
    }

    // Rule 2 matches on the sound entry, not only the call site: sounds made through
    // the generic persistent create-and-play wrapper 0x2588BE0 (it builds default
    // params, calls the core 0x2589124 and plays) all return to 0x2588C2F, whichever
    // of its 140 call sites asked, so the call site alone would refuse loops the clip
    // never carried. The clip must carry the same entity, call site and entry.
    //
    // A sound set entry ref is built by 0x252A888: the set's metadata offset in the
    // low dword, (index | 0x800000) in the high dword, the index being the entry's
    // place among the set's names sorted by hash (0x49F6E4, binary search through
    // 0x252A8F4). Sets of one kind (the outfit or tack picks the set) list the same
    // names, so the same name has the same index. A set entry ref keys by its index;
    // any other ref (a direct metadata ref, high dword 0xFFFFF, built by 0x252A888
    // from the set's own table or by 0x252B408) and a hash key by their whole value.
    constexpr std::uint32_t kSetEntryFlag = 0x800000;
    constexpr std::uint32_t kSetEntryIndexMask = 0xFFFFF;
    // The hidden player and his mount: no clip entity stands for them, so they
    // match by call site alone.
    constexpr std::uint32_t kNoOwner = 0xFFFFFFFFu;

    inline bool IsSetEntryRef(std::uint64_t ref, bool byHash)
    {
        if (byHash)
            return false;
        const std::uint32_t high = static_cast<std::uint32_t>(ref >> 32);
        return (high & kSetEntryFlag) != 0 && (high & ~(kSetEntryFlag | kSetEntryIndexMask)) == 0;
    }

    // Tag in the top two bits: 1 hash, 2 set entry index, 0 whole ref.
    inline std::uint64_t SoundEntryKey(std::uint64_t ref, bool byHash)
    {
        if (byHash)
            return (1ull << 62) | (ref & 0xFFFFFFFFull);
        if (IsSetEntryRef(ref, false))
            return (2ull << 62) | ((ref >> 32) & kSetEntryIndexMask);
        return ref & 0x3FFFFFFFFFFFFFFFull;
    }

    // One key per (entity, call site, entry): FNV-1a over the three.
    inline std::uint64_t OwnPersistentKey(std::uint32_t owner, std::uint32_t caller, std::uint64_t entryKey)
    {
        std::uint64_t hash = 14695981039346656037ull;
        const auto mix = [&hash](std::uint64_t value, int bytes) {
            for (int n = 0; n < bytes; ++n)
            {
                hash ^= (value >> (8 * n)) & 0xFFu;
                hash *= 1099511628211ull;
            }
        };
        mix(owner, 4);
        mix(caller, 4);
        mix(entryKey, 8);
        return hash;
    }

    // The clip's keys sorted ascending (a big fight has hundreds: binary
    // search, a dozen reads on the detour's path).
    inline bool SortedKeyCarried(std::uint64_t key, const std::uint64_t* keys, std::size_t count)
    {
        std::size_t low = 0;
        std::size_t high = count;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            if (keys[middle] == key)
                return true;
            if (keys[middle] < key)
                low = middle + 1;
            else
                high = middle;
        }
        return false;
    }

    // Whether the clip carries a puppet's own persistent create: by entity, call
    // site and entry (byEntry, a puppet with a clip entity; `keys` sorted ascending),
    // or by the call site alone (byEntry off: the hidden player and his mount).
    inline bool OwnPersistentCarried(bool byEntry, std::uint32_t owner, std::uint32_t caller, std::uint64_t ref,
        bool byHash, const std::uint64_t* keys, std::size_t keyCount, const std::uint32_t* callers, std::size_t callerCount)
    {
        if (!caller)
            return false;
        if (byEntry && owner != kNoOwner)
            return SortedKeyCarried(OwnPersistentKey(owner, caller, SoundEntryKey(ref, byHash)), keys, keyCount);
        return CallerCarried(caller, callers, callerCount);
    }

    // Where a replayed ped audio sound plays. Recorded params with a position keep
    // it for a short sound (a shell lands where it fell). A loop (alive
    // kPoseV2EntitySoundLoopMs or more, or never ended in the recording) follows its
    // puppet when loopsFollow is on: the engine moves these every frame (the loop
    // helper 0x319D96C and the tack loops in 0x319F8A4 set the position with
    // 0x2536E48 on each update), so a replayed copy would stay where it was created.
    inline bool PedAudioFollowsPuppet(bool pedAudio, bool positioned, bool loop, bool loopsFollow)
    {
        if (!pedAudio || !positioned)
            return true;
        return loopsFollow && loop;
    }

    // How long a replayed short ped audio (or train car audio) persistent sound
    // lives. The game stops some of these itself after a frame or two (a horse's
    // braking slide makes a new surface sound every frame, each stopped 15 to 31 ms
    // later); played whole they sound like a hard skid. The ESND track carries the
    // stop as endMs, so a short own-audio sound stops when it has played its recorded
    // length, counted from the playhead it started at (a late start keeps the
    // length). A sound that ended by itself was measured on the script poll after it
    // ended, so the stop comes after its natural end. One-shots are never cut.
    inline bool StopAtRecordedLength(bool enabled, bool ownAudio, bool oneShot, bool ended, std::uint32_t startMs,
        std::uint32_t endMs, std::uint32_t loopMs, std::uint32_t playedFromMs, std::uint32_t nowMs)
    {
        if (!enabled || !ownAudio || oneShot || !ended || endMs < startMs)
            return false;
        const std::uint32_t length = endMs - startMs;
        if (length >= loopMs || nowMs < playedFromMs)
            return false;
        return nowMs - playedFromMs >= length;
    }
    // An ESND row made by the ped's sound controller for a line of speech (call
    // sites 0x2FA10CF and 0x2FA0DCC, inside 0x2FA10A4 and 0x2FA0D88) is the line's
    // own sound: the speech track replays the line, whose controller makes that sound
    // again. The row is left to the line when the line of the same owner at that time
    // was played; played by the ESND track when the line was refused (the recorded
    // sound over silence) or when no outcome came within maxWaitMs of the row's start.
    inline bool SpeechControllerCaller(std::uint32_t caller)
    {
        return caller == 0x2FA10CFu || caller == 0x2FA0DCCu;
    }

    // SoundGenericMode=packets: a replay entity's sound plays from its recording, so
    // the engine's own create on a puppet's audio is refused unless the replay makes
    // it (ReplayEmitScope, a replayed footstep or melee call). The speech controller
    // is left alone: speech keeps its own track (the recorder skips the line's
    // sounds) and refusing the line's own sound kills the line (stop reason 6 in
    // state 3, none of its sounds played).
    inline bool BlockPuppetCreate(bool block, bool onPuppetAudio, bool replayEmitting, bool insideReplayedSystem,
        std::uint32_t caller)
    {
        return block && onPuppetAudio && !replayEmitting && !insideReplayedSystem && !SpeechControllerCaller(caller);
    }

    // Rule 2 in SoundGenericMode=legacy, with the same speech exemption. The
    // replayed line prepares its own sound through the controller (0x2FA10CF, return
    // of 0x2FA10A4 into 0x2FA0100 from the line data's prepare 0x2FB6000) outside any
    // replay call, and the clip carries that call site and entry for every ped that
    // spoke: refusing it leaves the line data without a sound, the prepare returns 2
    // and the state-2 update kills the line with stop reason 4 (0x2FA0DCC: reason 6 in
    // state 3). The speech track owns the line; the controller's ESND row is left to
    // the line or played over its refusal by SpeechRowRule, as in packets mode.
    inline bool LegacyRefuseOwnPersistent(std::uint32_t caller, bool active, bool replayEmitting,
        bool insideReplayedSystem, bool onPuppetAudio, bool hasTracker, bool callerCarried)
    {
        return !SpeechControllerCaller(caller) &&
            RefuseOwnPersistent(active, replayEmitting, insideReplayedSystem, onPuppetAudio, hasTracker, callerCarried);
    }

    // The puppet's own speech. A replay ped has no speech AI: only the replay makes
    // lines on it. Here the puppet keeps its AI and asks lines of its own through the
    // speech core (0x2FA1114); a replayed line of the same context is then refused by
    // the core's check that the speaker's newest active line is another line of that
    // context (code 18, 0x2F974A4) and dies in state 3 with reason 6. A core call on a
    // puppet's speech entity outside the replay's own calls (ReplayEmitScope) is
    // refused the way the core refuses (scope released, code 0x3F, nothing created).
    inline constexpr std::uint32_t kPuppetOwnSpeechRefusal = 0x3F;
    inline bool RefusePuppetOwnSpeech(bool active, bool onPuppet, bool replayEmitting)
    {
        return active && onPuppet && !replayEmitting;
    }

    // A replayed step (or melee) call runs the puppet's own code, and its one-shots
    // pass (insideReplayedSystem) because the recorder leaves them to that track. The
    // step code branches on the puppet's state (e.g. the extra hoof set 0x0d167c at
    // 0x318DED9, gated by the mount link row +0xA8 and component +0x50 byte +0x22 bit
    // 0x08), so a puppet can make sounds the live entity never did. Inside a replayed
    // call, a one-shot of a set the owner never made anywhere in the clip is refused
    // when the clip has packets for that owner (a clip without the generic track, or
    // an owner it never saw, is left alone).
    inline bool RefuseUnmadeReplayedSet(bool gate, bool onPuppetAudio, bool insideReplayedSystem, bool ownerHasPackets,
        bool ownerMadeSet)
    {
        return gate && onPuppetAudio && insideReplayedSystem && ownerHasPackets && !ownerMadeSet;
    }

    // A persistent sound the engine creates by itself on a puppet (the persistent
    // wrapper 0x2588BE0, return 0x2588C2F into the by-ref port 0x2589124) with no
    // counterpart in the clip, e.g. an animal loop that keeps playing with the replay
    // paused. Refused: a create through the persistent by-ref port of a clip owner
    // (not the hidden player) whose set the owner never made anywhere in the clip (no
    // generic packet of that owner and set) and that the ESND track does not carry
    // (entity, call site, entry). The by-hash port is left out: its hash is the name,
    // while the generic packets keep the creator's resolved ref, so a set cannot be
    // compared there.
    inline bool RefuseUnrecordedOwnPersistent(bool gate, bool byRefPort, bool clipOwner, bool ownerMadeSet,
        bool esndCarried)
    {
        return gate && byRefPort && clipOwner && !ownerMadeSet && !esndCarried;
    }

    enum class LineState : std::uint8_t
    {
        Unknown = 0, // the line was not asked yet on this pass
        Played = 1,  // a path created the line
        Refused = 2, // no path created it
    };

    enum class SpeechRow : std::uint8_t
    {
        NotSpeech = 0,  // not a line's sound: the ESND rules apply
        Wait = 1,       // the line's outcome is not known yet
        LeaveToLine = 2, // the line plays it
        Play = 3,       // the line was refused (or never came): the ESND row plays
    };

    inline SpeechRow SpeechRowRule(bool controllerCaller, bool lineAtRow, LineState line, std::uint32_t waitedMs,
        std::uint32_t maxWaitMs)
    {
        if (!controllerCaller || !lineAtRow)
            return SpeechRow::NotSpeech;
        if (line == LineState::Played)
            return SpeechRow::LeaveToLine;
        if (line == LineState::Refused)
            return SpeechRow::Play;
        return waitedMs >= maxWaitMs ? SpeechRow::Play : SpeechRow::Wait;
    }
}
