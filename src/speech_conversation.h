#pragma once

// Conversation (scripted) lines asked again the engine's own way: with the
// recorded sound set and the recorded duck byte. Without them a replayed
// scripted line plays in another category and ducks itself, a ramp per line
// deeper the nearer the speaker is. Engine facts (build 1491.50):
//   - the sound set: the line's data +0xE8..+0xF7 is params +0xC0..+0xCF
//     (data init 0x2FA92C0 at 0x2FA9327): +0xE8 the set object, +0xF0 its
//     name hash, +0xF4 its metadata ref. The wave (0x2FA6AEC, name
//     0x75E4276D) and the five type 2 layers (0x2FB3E6C) are built from it
//     (0x252A888: low dword the set's ref, high dword the index | 0x800000),
//     so the set picks the line's sounds and with them their category.
//     Params +0xC0 is written only by the SpeechParams applier 0x2F967E8
//     (meta +0x14: +0xC8 = hash, 0x252AD8C + 0x252B408 give the ref ->
//     +0xCC, 0x252B194 the object -> +0xC0) and by the context applier
//     0x2F965D0 (ctx +0x10, the same three calls), which fills it only while
//     +0xC0 is still 0 (0x2F965F3). The replay's ctor (0x2F91EA4) applies no
//     SpeechParams, so the context's set wins and a replayed scripted line
//     picks the AI lines' set and category;
//   - the duck byte: params +0x19E -> line +0x2BE bit 5 (0x2FAC063). A d8 2
//     line of type 1 or 2 with it raises every frame the speech duck level
//     (0x2FCC224 -> 0x5B5BF24, a curve of the line's distance to the
//     listener) that starts the mix scene 0x576520E1 (0x2F2D1EC). Live the
//     scripted lines had it clear (bits 0x02480000); the ctor sets it
//     (0x2FAEE3B), so every replayed scripted line would duck its own voice
//     (the ambient speech category, where the context's set put it).
// So, with SpeechConversationLive=1, every replayed human line is asked with
// its recorded duck byte and its recorded sound set: by hash with the
// applier's own three calls when the clip has it (SPRQ v2), else (an older
// clip) by the set ref of the line's own recorded layer sounds (ESND rows of
// the speaker made by the layer call 0x2FA0DCC at the line's start; type 2
// lines only, see SetFromSoundsAllowed). The replay writes params
// +0xC0/+0xC8/+0xCC before the direct entry as 0x2F967E8 does, so the
// context applier leaves them.
//
// Pure: unit tested in ReplayCoreTests (speech_conversation_tests.cpp).

#include "speech_request.h"
#include "speech_script_path.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace replay::speech::conversation
{
    // Return addresses of the two sound creations built from the line's set.
    constexpr std::uint32_t kWaveSoundCaller = 0x2FA10CF;  // 0x2FA10A4, the wave (data +0xC0)
    constexpr std::uint32_t kLayerSoundCaller = 0x2FA0DCC; // 0x2FA0D88, a type 2 layer (data +0x198)
    // 0x252A888 (bts r9d, 0x17): a child ref's high dword is the index
    // (20 bits) with bit 23 set.
    constexpr std::uint32_t kChildIndexFlag = 0x800000;
    // The recorded sounds of a line are looked for this far around its
    // start (they share the audible start's millisecond, +-1).
    constexpr std::uint32_t kSoundBeforeMs = 250;
    constexpr std::uint32_t kSoundAfterMs = 2500;

    // params offsets the applier 0x2F967E8 writes.
    constexpr std::uint16_t kParamsSetObject = 0xC0;
    constexpr std::uint16_t kParamsSetHash = 0xC8;
    constexpr std::uint16_t kParamsSetRef = 0xCC;
    // 0x2F967E8 passes the ref to 0x252B194 as {ref, 0xFFFFF}.
    constexpr std::uint32_t kRefHighAny = 0xFFFFF;

    // The set object's table, as
    // the name search 0x49F6E4 reads it: the entry count at +5 (0x252B154)
    // and entry n's name hash at +9 + 12 * n (0x252A8F4), searched in binary
    // (so a real set's hashes ascend). A set found by a recorded ref is
    // checked against it before it goes into params (SetTableAscending).
    constexpr std::size_t kSetCountOffset = 5;
    constexpr std::size_t kSetEntriesOffset = 9;
    constexpr std::size_t kSetEntryBytes = 12;
    constexpr std::uint32_t kSetMaxEntries = 4096;

    // 0x252A888 and 0x2516734 take 0 and 0xFFFFFFFF as "no ref".
    inline bool ValidSetRef(std::uint32_t ref) { return ref != 0 && ref != 0xFFFFFFFFu; }

    // The set ref of a sound ref 0x252A888 built from a set, 0 for any other.
    inline std::uint32_t SetRefOfChild(std::uint64_t soundRef)
    {
        const std::uint32_t high = static_cast<std::uint32_t>(soundRef >> 32);
        const std::uint32_t low = static_cast<std::uint32_t>(soundRef);
        if ((high & 0xFFF00000u) != kChildIndexFlag || !ValidSetRef(low))
            return 0;
        return low;
    }

    // `entries`: the set's bytes from kSetEntriesOffset, kSetEntryBytes per
    // entry. True when the count is plausible and the name hashes ascend as
    // 0x49F6E4's binary search needs. 0x252B194 checks only the type byte
    // (0x29) at the ref's offset; a ref recorded with another metadata
    // layout (another install, a DLC or audio mod added or removed: the ref
    // is chunk << 24 | offset) can land inside another object on a 0x29
    // byte, and its "count" then sends the line's set searches (0x2FA6AEC,
    // 0x2FB3E6C, outside any guard of ours) far out of the object.
    inline bool SetTableAscending(const std::uint8_t* entries, std::uint32_t count)
    {
        if (!entries || count == 0 || count > kSetMaxEntries)
            return false;
        std::uint32_t last = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            std::uint32_t hash = 0;
            std::memcpy(&hash, entries + static_cast<std::size_t>(n) * kSetEntryBytes, sizeof(hash));
            if (n && hash < last)
                return false;
            last = hash;
        }
        return true;
    }

    // Only a type 2 line's layers prove the line's own set: 0x2FB3E6C builds
    // them from data +0xE8 and has no default (a line without a set has no
    // layers). A type 1 line plays only its wave, which 0x2FA6AEC builds
    // from the line's set OR from a default set when the line has none, so
    // its wave's set may not be the line's: a type 1 line gets no set from
    // the sounds (left to the context, as the engine had it). An unknown
    // type (0) is matched by its layers like type 2.
    inline bool SetFromSoundsAllowed(std::uint32_t dataType)
    {
        return dataType != request::kTypeSingle && dataType != request::kTypePreResolved;
    }

    // An older clip (no set in SPRQ): the set ref of the speaker's own
    // recorded layer sounds (0x2FA0DCC) nearest to the line's start.
    // `Sounds` is a range of rows with entity, startMs, soundRef, caller,
    // pedAudio and byHash (replay::EntitySound). 0 when none or not allowed.
    template <typename Sounds, typename Id>
    std::uint32_t SetRefFromSounds(const Sounds& sounds, const Id& speaker, std::uint32_t dataType, std::uint32_t startMs,
        std::uint32_t& foundMs)
    {
        std::uint32_t best = 0;
        std::uint32_t bestGap = 0xFFFFFFFFu;
        if (!SetFromSoundsAllowed(dataType))
            return 0;
        const std::uint32_t caller = kLayerSoundCaller;
        const std::uint32_t from = startMs > kSoundBeforeMs ? startMs - kSoundBeforeMs : 0;
        const std::uint32_t to = startMs + kSoundAfterMs;
        for (const auto& sound : sounds)
        {
            if (!(sound.entity == speaker) || !sound.pedAudio || sound.byHash || sound.caller != caller)
                continue;
            if (sound.startMs < from || sound.startMs > to)
                continue;
            const std::uint32_t ref = SetRefOfChild(sound.soundRef);
            if (!ref)
                continue;
            const std::uint32_t gap = sound.startMs > startMs ? sound.startMs - startMs : startMs - sound.startMs;
            if (gap < bestGap)
            {
                bestGap = gap;
                best = ref;
                foundMs = sound.startMs;
            }
        }
        return best;
    }

    // The recorded duck bit (+0x2BE bit 5), -1 when the fields were not read.
    inline int RecordedDuck(const request::Fields& fields)
    {
        if (!fields.read)
            return -1;
        return (fields.value[request::LineBits] & scriptpath::kLineDuckBit) ? 1 : 0;
    }

    // The duck byte an Ask writes, -1 when none.
    inline int AskedDuck(const request::Ask& ask)
    {
        for (std::uint8_t n = 0; n < ask.byteCount && n < request::kMaxByteWrites; ++n)
            if (ask.bytes[n].offset == scriptpath::kParamsDuckByte)
                return ask.bytes[n].value ? 1 : 0;
        return -1;
    }

    // Adds the duck byte and the sound set to a line's Ask (key on).
    // `soundsSetRef`: SetRefFromSounds for the line, used only when the
    // clip has no set for it (SPRQ v1). A line recorded with no set (both
    // 0) is left to the context, as the live one was.
    inline void Apply(bool on, const request::Fields& recorded, std::uint32_t soundsSetRef, request::Ask& ask)
    {
        if (!on)
            return;
        const int duck = RecordedDuck(recorded);
        if (duck >= 0)
            ask.SetByte(scriptpath::kParamsDuckByte, static_cast<std::uint8_t>(duck));
        if (recorded.setRead)
        {
            if (recorded.setHash != 0 || ValidSetRef(recorded.setRef))
            {
                ask.setFrom = request::SetFrom::Recorded;
                ask.setHash = recorded.setHash;
                ask.setRef = recorded.setRef;
            }
            return;
        }
        if (ValidSetRef(soundsSetRef))
        {
            ask.setFrom = request::SetFrom::EntitySounds;
            ask.setHash = 0;
            ask.setRef = soundsSetRef;
        }
    }

    inline const char* SetFromName(request::SetFrom from)
    {
        switch (from)
        {
        case request::SetFrom::None: return "none";
        case request::SetFrom::Recorded: return "recorded";
        case request::SetFrom::EntitySounds: return "entity_sounds";
        }
        return "unknown";
    }

    // What the replay did with an asked set (audio_observer.cpp).
    enum class SetStatus : std::uint8_t
    {
        NotAsked = 0,
        ByHash,    // hash -> ref (0x252B408) -> object (0x252B194), as 0x2F967E8
        ByRef,     // recorded ref -> object (0x252B194): no hash (an older clip, the sounds' set)
        NoObject,  // no object: +0xC0 left 0, the context applier fills it (as after 0x2F967E8)
        Disabled,  // a signature or call site did not match: nothing written
        Exception, // an engine call faulted: nothing written
        BadObject, // the object of a recorded ref failed SetTableAscending: nothing written
        kCount
    };

    inline const char* SetStatusName(SetStatus status)
    {
        switch (status)
        {
        case SetStatus::NotAsked: return "not_asked";
        case SetStatus::ByHash: return "by_hash";
        case SetStatus::ByRef: return "by_ref";
        case SetStatus::NoObject: return "no_object";
        case SetStatus::Disabled: return "disabled";
        case SetStatus::Exception: return "exception";
        case SetStatus::BadObject: return "bad_object";
        case SetStatus::kCount: break;
        }
        return "unknown";
    }

    // The ref the replay resolves to an object: the engine's lookup of the
    // recorded hash (`hashRef`, what the applier writes to +0xCC) when the
    // clip has the hash, the recorded ref only when it has none (an older
    // clip: the set of the line's sounds).
    // A hash the lookup did not find is a set this session does not have by
    // name (0x252B408 searches every chunk), so the recorded ref names
    // another layout and is not tried: 0, the context chooses (as after
    // 0x2F967E8 with an unknown hash).
    inline std::uint32_t RefToResolve(const request::Ask& ask, bool hashLooked, std::uint32_t hashRef, bool& byHash)
    {
        byHash = false;
        if (ask.setHash && hashLooked)
        {
            if (!ValidSetRef(hashRef))
                return 0;
            byHash = true;
            return hashRef;
        }
        return ValidSetRef(ask.setRef) ? ask.setRef : 0;
    }

    // "1>0>0": recorded > asked > got, "-" for a side not known.
    inline std::string DuckText(const request::Fields& recorded, const request::Ask& ask, const request::Fields& replayed)
    {
        const int values[3] = {RecordedDuck(recorded), AskedDuck(ask), RecordedDuck(replayed)};
        std::string text;
        for (int n = 0; n < 3; ++n)
        {
            if (n)
                text += '>';
            text += values[n] < 0 ? "-" : (values[n] ? "1" : "0");
        }
        return text;
    }

    // The playback's counts (SPEECH_CONVERSATION_TOTAL).
    struct Tally
    {
        std::uint64_t lines = 0;
        std::uint64_t duckAsked = 0;   // the duck byte written
        std::uint64_t duckCleared = 0; // written as 0 (the ctor's 1 undone)
        std::uint64_t duckKept = 0;    // the created line has the asked bit
        std::uint64_t setRecorded = 0; // set from SPRQ v2
        std::uint64_t setSounds = 0;   // set from the line's recorded sounds
        std::uint64_t setNone = 0;     // no set asked (left to the context)
        std::uint64_t setKept = 0;     // the created line has the written set ref (the asked one if none written)
        std::uint64_t setOther = 0;    // another set ref
        std::uint64_t setUnread = 0;   // the created line's set not read
        std::uint64_t byStatus[static_cast<std::size_t>(SetStatus::kCount)] = {};

        // `writtenRef`: the ref the replay wrote into params (by hash it is
        // this session's ref, which may differ from the recorded one), 0
        // when none was written.
        void Note(const request::Ask& ask, SetStatus status, const request::Fields& replayed, std::uint32_t writtenRef)
        {
            ++lines;
            const int duck = AskedDuck(ask);
            if (duck >= 0)
            {
                ++duckAsked;
                duckCleared += duck == 0 ? 1 : 0;
                duckKept += RecordedDuck(replayed) == duck ? 1 : 0;
            }
            if (ask.setFrom == request::SetFrom::Recorded)
                ++setRecorded;
            else if (ask.setFrom == request::SetFrom::EntitySounds)
                ++setSounds;
            else
                ++setNone;
            if (static_cast<std::size_t>(status) < static_cast<std::size_t>(SetStatus::kCount))
                ++byStatus[static_cast<std::size_t>(status)];
            if (ask.setFrom == request::SetFrom::None)
                return;
            if (!replayed.setRead)
                ++setUnread;
            else if (replayed.setRef == (writtenRef ? writtenRef : ask.setRef))
                ++setKept;
            else
                ++setOther;
        }

        // "by_hash:3|by_ref:2" (statuses seen), "none".
        std::string StatusText() const
        {
            std::string text;
            char item[40]{};
            for (std::size_t n = 1; n < static_cast<std::size_t>(SetStatus::kCount); ++n)
            {
                if (!byStatus[n])
                    continue;
                std::snprintf(item, sizeof(item), "%s%s:%llu", text.empty() ? "" : "|",
                    SetStatusName(static_cast<SetStatus>(n)), static_cast<unsigned long long>(byStatus[n]));
                text += item;
            }
            return text.empty() ? std::string("none") : text;
        }
    };
}
