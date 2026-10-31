#pragma once

// The request fields of a speech line, recorded and asked again.
//
// A line the ped's own AI said when recorded (path=ai, data_type=1) comes back
// on the replay's direct path as data type 2 unless the type is asked, and the
// two types play a different set of sounds. So the recorder reads the line's
// copies of the request (what the engine really used, after its own
// resolution) and the replay writes the line type and the four request fields
// the SpeechParams metadata sets, each only in the range the engine reads (the
// volume vector refuses +0x21C or +0x220 >= 4, and +0x214 >= 5). The other
// fields are only compared: one diff line per line type and a count per field.
//
// Evidence: offline dump of build 1491.50.
// Research: docs/research/speech-line-request.md
//
// Pure: unit tested in ReplayCoreTests (scenery_speech_tests.cpp).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace replay::speech::request
{
    // params +0xF4 -> data +0xE0: the line data class.
    constexpr std::uint32_t kParamsLineType = 0xF4;
    constexpr std::uint32_t kTypeSingle = 1;      // one sound (the AI's ambient lines)
    constexpr std::uint32_t kTypeLayered = 2;     // five layers (the validator's default)
    constexpr std::uint32_t kTypePreResolved = 3; // needs the pre-resolved object +0x1B8: never asked

    enum Field : std::uint8_t
    {
        D8 = 0,
        VolumeType,
        Audibility,
        E8Tables,
        Source,
        P100,
        P104,
        P108,
        PEC,
        PF0,
        PreloadMs,
        PD4,
        P170,
        P178,
        P180,
        LineBits,
        kFieldCount
    };

    struct FieldInfo
    {
        const char* name;
        std::uint16_t lineOffset;   // the line's copy (the line init)
        std::uint16_t paramsOffset; // the request field (0: none)
        std::uint32_t limit;        // asked again only below this (0: compared only)
    };

    // The source (+0xF8) stays with the slot manager (speech_slots.h); the
    // flag bytes hold +0x18C, which the replay always sets (repeat check).
    constexpr FieldInfo kFields[kFieldCount] = {
        {"d8", 0x214, 0xD8, 5},
        {"volume_type", 0x21C, 0xDC, 4},
        {"audibility", 0x220, 0xE0, 4},
        {"e8_tables", 0x224, 0xE8, 5},
        {"source", 0x218, 0xF8, 0},
        {"p100", 0x228, 0x100, 0},
        {"p104", 0x22C, 0x104, 0},
        {"p108", 0x230, 0x108, 0},
        {"pEC", 0x23C, 0xEC, 0},
        {"pF0", 0x240, 0xF0, 0},
        {"preload_ms", 0x27C, 0x168, 0},
        {"pD4", 0x1F8, 0xD4, 0},
        {"p170", 0x290, 0x170, 0},
        {"p178", 0x294, 0x178, 0},
        {"p180", 0x2A4, 0x180, 0},
        {"bits", 0x2BC, 0, 0},
    };

    // One guarded read of the line covers every copy (the line is 0x2C0
    // bytes, its allocator's size).
    constexpr std::size_t kLineBlockFrom = 0x1F8;
    constexpr std::size_t kLineBlockBytes = 0x2C0 - kLineBlockFrom;

    struct Fields
    {
        bool read = false;
        std::uint32_t value[kFieldCount] = {};
        // The line's sound set (speech_conversation.h), data +0xE8..+0xF7 =
        // params +0xC0..+0xCF (copied by the data init): +0xF0 the set's
        // name hash (params +0xC8), +0xF4 its metadata ref (params +0xCC;
        // chunk << 24 | offset, from the ref lookup). Read
        // from the line's data, not the line block; SPRQ v2. The pointer at
        // +0xE8 belongs to the session and is not kept.
        bool setRead = false;
        std::uint32_t setHash = 0;
        std::uint32_t setRef = 0;
    };

    // The data block holding the sound set.
    constexpr std::size_t kDataSetFrom = 0xE8;
    constexpr std::size_t kDataSetBytes = 0x10;
    // `block` holds the data's bytes from kDataSetFrom.
    inline bool SetFromDataBlock(const std::uint8_t* block, std::size_t size, Fields& out)
    {
        if (!block || size < kDataSetBytes)
            return false;
        std::memcpy(&out.setHash, block + 8, sizeof(out.setHash));
        std::memcpy(&out.setRef, block + 0xC, sizeof(out.setRef));
        out.setRead = true;
        return true;
    }

    // `block` holds the line's bytes from kLineBlockFrom.
    // Leaves the sound set as it was (it comes from the data, read apart).
    inline bool FromLineBlock(const std::uint8_t* block, std::size_t size, Fields& out)
    {
        out.read = false;
        for (std::size_t f = 0; f < kFieldCount; ++f)
            out.value[f] = 0;
        if (!block || size < kLineBlockBytes)
            return false;
        for (std::size_t f = 0; f < kFieldCount; ++f)
            std::memcpy(&out.value[f], block + (kFields[f].lineOffset - kLineBlockFrom), sizeof(std::uint32_t));
        out.read = true;
        return true;
    }

    inline bool SameFields(const Fields& a, const Fields& b)
    {
        if (a.read != b.read || a.setRead != b.setRead || a.setHash != b.setHash || a.setRef != b.setRef)
            return false;
        for (std::size_t f = 0; f < kFieldCount; ++f)
            if (a.value[f] != b.value[f])
                return false;
        return true;
    }

    // What the replay writes into the direct entry's params after the ctor.
    struct Write
    {
        std::uint16_t offset = 0;
        std::uint32_t value = 0;
    };
    // A params flag byte the line init turns into one of the line's bits
    // (+0x2BC..+0x2BF; speech_script_path.h).
    struct ByteWrite
    {
        std::uint16_t offset = 0;
        std::uint8_t value = 0;
    };
    constexpr std::size_t kMaxByteWrites = 4;
    // Where the sound set the replay asks comes from
    // (speech_conversation.h).
    enum class SetFrom : std::uint8_t
    {
        None = 0,      // left to the context metadata (the context applier)
        Recorded = 1,  // the line's own set, recorded (SPRQ v2)
        EntitySounds = 2 // an older clip: the set ref of the line's recorded sounds (ESND)
    };
    struct Ask
    {
        std::uint32_t lineType = 0; // 0: +0xF4 left at 0 (the validator writes 2)
        std::uint8_t count = 0;
        Write writes[kFieldCount];
        std::uint8_t byteCount = 0;
        ByteWrite bytes[kMaxByteWrites];
        // The sound set written into params +0xC0/+0xC8/+0xCC the way the
        // SpeechParams applier does (by hash when known, else by the
        // recorded ref).
        SetFrom setFrom = SetFrom::None;
        std::uint32_t setHash = 0;
        std::uint32_t setRef = 0;
        bool Empty() const { return lineType == 0 && count == 0 && byteCount == 0 && setFrom == SetFrom::None; }
        // Writes `value` at `offset`, replacing an earlier write there.
        bool Set(std::uint16_t offset, std::uint32_t value)
        {
            for (std::uint8_t n = 0; n < count; ++n)
                if (writes[n].offset == offset)
                {
                    writes[n].value = value;
                    return true;
                }
            if (count >= kFieldCount)
                return false;
            writes[count].offset = offset;
            writes[count].value = value;
            ++count;
            return true;
        }
        bool SetByte(std::uint16_t offset, std::uint8_t value)
        {
            for (std::uint8_t n = 0; n < byteCount; ++n)
                if (bytes[n].offset == offset)
                {
                    bytes[n].value = value;
                    return true;
                }
            if (byteCount >= kMaxByteWrites)
                return false;
            bytes[byteCount].offset = offset;
            bytes[byteCount].value = value;
            ++byteCount;
            return true;
        }
    };

    // The same writes (the replay asks a refused line again only when the
    // conversation fields changed the Ask).
    inline bool SameAsk(const Ask& a, const Ask& b)
    {
        if (a.lineType != b.lineType || a.count != b.count || a.byteCount != b.byteCount || a.setFrom != b.setFrom ||
            a.setHash != b.setHash || a.setRef != b.setRef)
            return false;
        for (std::uint8_t n = 0; n < a.count && n < kFieldCount; ++n)
        {
            bool found = false;
            for (std::uint8_t m = 0; m < b.count && m < kFieldCount && !found; ++m)
                found = b.writes[m].offset == a.writes[n].offset && b.writes[m].value == a.writes[n].value;
            if (!found)
                return false;
        }
        for (std::uint8_t n = 0; n < a.byteCount && n < kMaxByteWrites; ++n)
        {
            bool found = false;
            for (std::uint8_t m = 0; m < b.byteCount && m < kMaxByteWrites && !found; ++m)
                found = b.bytes[m].offset == a.bytes[n].offset && b.bytes[m].value == a.bytes[n].value;
            if (!found)
                return false;
        }
        return true;
    }

    // `recordedType`: the recorded data +0xE0. Type 1 and 2 are asked as
    // recorded; 3 (pre-resolved object) and 0 (unknown) are not.
    inline Ask BuildAsk(std::uint32_t recordedType, const Fields& recorded, bool typeOn, bool fieldsOn)
    {
        Ask ask;
        if (typeOn && (recordedType == kTypeSingle || recordedType == kTypeLayered))
            ask.lineType = recordedType;
        if (fieldsOn && recorded.read)
            for (std::size_t f = 0; f < kFieldCount; ++f)
                if (kFields[f].paramsOffset && kFields[f].limit && recorded.value[f] < kFields[f].limit)
                {
                    ask.writes[ask.count].offset = kFields[f].paramsOffset;
                    ask.writes[ask.count].value = recorded.value[f];
                    ++ask.count;
                }
        return ask;
    }

    // Bit f set: field f differs (both read).
    inline std::uint32_t DiffMask(const Fields& recorded, const Fields& replayed)
    {
        if (!recorded.read || !replayed.read)
            return 0;
        std::uint32_t mask = 0;
        for (std::size_t f = 0; f < kFieldCount; ++f)
            if (recorded.value[f] != replayed.value[f])
                mask |= 1u << f;
        return mask;
    }

    // "type:1>2|volume_type:0>1|..." (recorded > replayed), "none" when all
    // agree, "unread" when either side was not read (an old clip).
    inline std::string DiffText(std::uint32_t recordedType, const Fields& recorded, std::uint32_t replayedType,
        const Fields& replayed)
    {
        std::string text;
        char item[64]{};
        if (recordedType != replayedType)
        {
            std::snprintf(item, sizeof(item), "type:%u>%u", recordedType, replayedType);
            text += item;
        }
        if (!recorded.read || !replayed.read)
            return text.empty() ? std::string("unread") : text + "|unread";
        for (std::size_t f = 0; f < kFieldCount; ++f)
        {
            if (recorded.value[f] == replayed.value[f])
                continue;
            std::snprintf(item, sizeof(item), "%s%s:0x%X>0x%X", text.empty() ? "" : "|", kFields[f].name,
                recorded.value[f], replayed.value[f]);
            text += item;
        }
        // The sound set's ref, when both read.
        if (recorded.setRead && replayed.setRead && recorded.setRef != replayed.setRef)
        {
            std::snprintf(item, sizeof(item), "%sset:0x%X>0x%X", text.empty() ? "" : "|", recorded.setRef,
                replayed.setRef);
            text += item;
        }
        return text.empty() ? std::string("none") : text;
    }

    // The playback's counts and its once-per-type diff line.
    struct Tally
    {
        std::uint64_t lines = 0;      // replayed lines compared
        std::uint64_t typeAsked = 0;  // +0xF4 written
        std::uint64_t typeKept = 0;   // created with the recorded type
        std::uint64_t typeOther = 0;  // created with another type
        std::uint64_t fieldsAsked = 0; // lines with request fields written
        std::uint64_t unread = 0;     // no recorded fields (an old clip) or the line unread
        std::uint64_t same = 0;       // every compared field equal
        std::uint64_t byField[kFieldCount] = {};
        bool logged[4] = {};

        void Note(std::uint32_t recordedType, const Fields& recorded, std::uint32_t replayedType,
            const Fields& replayed, const Ask& ask)
        {
            ++lines;
            typeAsked += ask.lineType ? 1 : 0;
            fieldsAsked += ask.count ? 1 : 0;
            ++(recordedType == replayedType ? typeKept : typeOther);
            if (!recorded.read || !replayed.read)
            {
                ++unread;
                return;
            }
            const std::uint32_t mask = DiffMask(recorded, replayed);
            if (!mask)
                ++same;
            for (std::size_t f = 0; f < kFieldCount; ++f)
                byField[f] += (mask >> f) & 1u;
        }

        // True once per recorded type (0..2, 3 and above share the last).
        bool TakeLog(std::uint32_t recordedType)
        {
            const std::size_t index = recordedType < 3 ? recordedType : 3;
            if (logged[index])
                return false;
            logged[index] = true;
            return true;
        }

        // "volume_type:3|audibility:1" (fields that differed, how often), "none".
        std::string FieldText() const
        {
            std::string text;
            char item[48]{};
            for (std::size_t f = 0; f < kFieldCount; ++f)
            {
                if (!byField[f])
                    continue;
                std::snprintf(item, sizeof(item), "%s%s:%llu", text.empty() ? "" : "|", kFields[f].name,
                    static_cast<unsigned long long>(byField[f]));
                text += item;
            }
            return text.empty() ? std::string("none") : text;
        }
    };
}
