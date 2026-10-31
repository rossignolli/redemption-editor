#pragma once

// Conversation (scripted) lines asked again the engine's own way: with the
// recorded sound set and the recorded duck byte. Without them a replayed
// scripted line plays in another category and ducks itself, a ramp per line
// deeper the nearer the speaker is: the replay's params ctor applies no
// SpeechParams, so the context's sound set wins (the AI lines' set and
// category), and the ctor sets the duck byte the live scripted lines had
// clear.
//
// So, with SpeechConversationLive=1, every replayed human line is asked with
// its recorded duck byte and its recorded sound set: by hash with the
// applier's own three calls when the clip has it (SPRQ v2), else (an older
// clip) by the set ref of the line's own recorded layer sounds (ESND rows of
// the speaker made by the layer call, kLayerSoundCall, at the line's start;
// type 2 lines only, see SetFromSoundsAllowed). The replay writes params
// +0xC0/+0xC8/+0xCC before the direct entry as the applier does, so the
// context applier leaves them.
//
// Evidence: offline dump of build 1491.50.
// Research: docs/research/speech-duck-and-slots.md
//
// Pure: unit tested in ReplayCoreTests (speech_conversation_tests.cpp).

#include "game_sig_core.h"
#include "speech_request.h"
#include "speech_script_path.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace replay::speech::conversation
{
    namespace sig = replay::sig;

    // The two sound creations the speech controller builds from the line's
    // set: the wave (data +0xC0) and a type 2 layer (data +0x198). The return
    // addresses of these calls are the callers a recorded sound keeps
    // (puppet_audio_rule.h SpeechControllerCallers).
    inline const sig::Signature kWaveSound{"speech_wave_sound",
        "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 89 B8 00 00 00", 0x2FA10A4};
    inline const sig::Signature kWaveSoundCall{"speech_wave_sound_call", kWaveSound, sig::Offset{0x26},
        "E8 ?? ?? ?? ??", 0x2FA10CA};
    inline const sig::Signature kLayerSound{"speech_layer_sound",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 48 8B E9 33 DB", 0x2FA0D88};
    inline const sig::Signature kLayerSoundCall{"speech_layer_sound_call", kLayerSound, sig::Offset{0x3F},
        "E8 ?? ?? ?? ??", 0x2FA0DC7};
    // The child ref builder (bts r9d, 0x17): a child ref's high dword is the
    // index (20 bits) with bit 23 set.
    constexpr std::uint32_t kChildIndexFlag = 0x800000;
    // The recorded sounds of a line are looked for this far around its
    // start (they share the audible start's millisecond, +-1).
    constexpr std::uint32_t kSoundBeforeMs = 250;
    constexpr std::uint32_t kSoundAfterMs = 2500;

    // params offsets the SpeechParams applier writes.
    constexpr std::uint16_t kParamsSetObject = 0xC0;
    constexpr std::uint16_t kParamsSetHash = 0xC8;
    constexpr std::uint16_t kParamsSetRef = 0xCC;
    // The applier passes the ref to the set object lookup as {ref, 0xFFFFF}.
    constexpr std::uint32_t kRefHighAny = 0xFFFFF;

    // The set object's table, as the name search reads it: the entry count
    // at +5 and entry n's name hash at +9 + 12 * n, searched in binary (so a
    // real set's hashes ascend). A set found by a recorded ref is
    // checked against it before it goes into params (SetTableAscending).
    constexpr std::size_t kSetCountOffset = 5;
    constexpr std::size_t kSetEntriesOffset = 9;
    constexpr std::size_t kSetEntryBytes = 12;
    constexpr std::uint32_t kSetMaxEntries = 4096;

    // The child ref builder and the chunk lookup take 0 and 0xFFFFFFFF as
    // "no ref".
    inline bool ValidSetRef(std::uint32_t ref) { return ref != 0 && ref != 0xFFFFFFFFu; }

    // The set ref of a sound ref the child ref builder made from a set, 0 for
    // any other.
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
    // the name search's binary search needs. The set object lookup checks
    // only the type byte (0x29) at the ref's offset; a ref recorded with
    // another metadata layout (another install, a DLC or audio mod added or
    // removed: the ref is chunk << 24 | offset) can land inside another
    // object on a 0x29 byte, and its "count" then sends the line's set
    // searches (the wave and layer builders, outside any guard of ours) far
    // out of the object.
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

    // Only a type 2 line's layers prove the line's own set: the layer builder
    // builds them from data +0xE8 and has no default (a line without a set
    // has no layers). A type 1 line plays only its wave, which the wave
    // builder builds from the line's set OR from a default set when the line
    // has none, so its wave's set may not be the line's: a type 1 line gets
    // no set from the sounds (left to the context, as the engine had it). An
    // unknown type (0) is matched by its layers like type 2.
    inline bool SetFromSoundsAllowed(std::uint32_t dataType)
    {
        return dataType != request::kTypeSingle && dataType != request::kTypePreResolved;
    }

    // An older clip (no set in SPRQ): the set ref of the speaker's own
    // recorded layer sounds (kLayerSoundCall) nearest to the line's start.
    // `layerCaller`: that call's return RVA in this game (0: not found, no
    // set). `Sounds` is a range of rows with entity, startMs, soundRef,
    // caller, pedAudio and byHash (replay::EntitySound). 0 when none or not
    // allowed.
    template <typename Sounds, typename Id>
    std::uint32_t SetRefFromSounds(std::uint32_t layerCaller, const Sounds& sounds, const Id& speaker,
        std::uint32_t dataType, std::uint32_t startMs, std::uint32_t& foundMs)
    {
        std::uint32_t best = 0;
        std::uint32_t bestGap = 0xFFFFFFFFu;
        if (!SetFromSoundsAllowed(dataType) || !layerCaller)
            return 0;
        const std::uint32_t caller = layerCaller;
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
        ByHash,    // hash -> ref (the ref lookup) -> object (the set object lookup), as the applier does
        ByRef,     // recorded ref -> object (the set object lookup): no hash (an older clip, the sounds' set)
        NoObject,  // no object: +0xC0 left 0, the context applier fills it (as after the applier)
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
    // name (the ref lookup searches every chunk), so the recorded ref names
    // another layout and is not tried: 0, the context chooses (as after the
    // applier with an unknown hash).
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
