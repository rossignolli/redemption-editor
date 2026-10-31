#pragma once
// The engine's volume listener of the live game, recorded and put back when
// the replay plays the recorded game camera.
//
// Why: with the same game camera the replay was 7 dB lower. The ped audio
// distance metric measures to the listener's volume points; live they sit
// near the player, in the replay at the script camera.
//
// The replay records the live volume point (absolute position) and, while it
// renders the recorded game camera, puts it back through the engine's own
// volume listener override (bit 0x100 of +0x650, matrix +0x6B0), the way the
// engine's scenes do. A write to a world system: only while the replay owns
// the world, cleared on exit.
// Evidence: offline dump; the code sites (kSites) hold every offset.
// Research: docs/research/audio-listener.md
#include "game_sig_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::gamelistener
{
    namespace sig = replay::sig;

    // Layout (proven by the code sites below).
    constexpr std::uint32_t kListenerBase = 0x20;      // first description
    constexpr std::uint32_t kListenerStride = 0x310;   // two descriptions
    constexpr std::uint32_t kListenerCount = 2;
    constexpr std::uint32_t kCurrentIndex = 0x784;     // byte, 0 or 1
    constexpr std::uint32_t kMicOverrideHash = 0x780;  // u32, a camera's metadata microphone (evidence)
    constexpr std::uint32_t kVolumePointCount = 0x2B0; // per description, dword
    constexpr std::uint32_t kVolumePointPos = 0x1D0;   // per description, point 0 position (vec4)
    constexpr std::uint32_t kVolumePointStride = 0x90;
    constexpr std::uint32_t kFlags = 0x650;            // dword
    constexpr std::uint32_t kVolumeOverrideBit = 0x100;
    constexpr std::uint32_t kVolumeOverrideMatrix = 0x6B0; // 4 rows of 16 bytes, position last (+0x6E0)
    constexpr std::uint32_t kMaxVolumePoints = 8;      // sanity bound for the probe
    // The microphone: per point slot, the microphone in use (desc+0x300) and
    // the override hash +0x780 with its crossfade +0x778, which the engine
    // writes only from animated scene handlers. The replay records the hash of
    // the microphone in use. Research: docs/research/audio-listener.md
    constexpr std::uint32_t kMicCrossfade = 0x778;     // u32 ms, read with the override (mic_crossfade_read)
    constexpr std::uint32_t kSlotBase = 0x190;         // per description, slot j = +0x190 + j*0x90
    constexpr std::uint32_t kSlotWeight = 0x50;        // f32 (vec4 broadcast), 1/count
    constexpr std::uint32_t kSlotF70 = 0x70;           // f32 (vec4 broadcast), the microphone metadata's +0xC
    constexpr std::uint32_t kMicInUse = 0x300;         // per description, the microphone metadata pointer
    constexpr std::uint8_t kMicMetadataType = 0xA9;
    constexpr std::uint32_t kMicDefaultHash = 0x2A7C18A2u;
    constexpr std::uint8_t kMaxRecordedPoints = 4;     // LSTN v2 keeps up to this many points per sample
    // The metadata table (as the chunk find, the entry search and the entry
    // test read it): [table] chunks, word [table+8] chunk count,
    // chunk stride 0xD0; per chunk +0x10 data, dword +0x18 data size, dword
    // +0x1C entry count, +0x20 entries {u32 offset, u32 hash} sorted by hash,
    // byte +0xCD loaded. An object's pointer = data + offset.
    constexpr std::uint32_t kMetaChunkStride = 0xD0;
    constexpr std::uint32_t kMetaChunkData = 0x10;
    constexpr std::uint32_t kMetaChunkSize = 0x18;
    constexpr std::uint32_t kMetaEntryCount = 0x1C;
    constexpr std::uint32_t kMetaEntries = 0x20;
    constexpr std::uint32_t kMetaLoaded = 0xCD;
    constexpr std::uint32_t kMetaMaxChunks = 64;      // sanity bounds of the scan
    constexpr std::uint32_t kMetaMaxEntries = 1u << 21;
    struct MetaEntry
    {
        std::uint32_t offset;
        std::uint32_t hash;
    };
    // The hash whose entry names this offset (first match), 0 none. Pure.
    inline std::uint32_t HashOfOffset(const MetaEntry* entries, std::size_t count, std::uint32_t offset)
    {
        for (std::size_t n = 0; n < count; ++n)
            if (entries[n].offset == offset)
                return entries[n].hash;
        return 0;
    }

    // Code sites, found by signature (game_sig_core.h) before any read or
    // write: their bytes prove the globals and every field offset this module
    // touches.
    //
    // The ped audio update loads the environment for the listener test.
    inline const sig::Signature kPedEnvUpdate{"listener_ped_env_update",
        "48 89 5C 24 08 57 48 83 EC 50 48 8B 01 48 8B F9 FF 90 F0 00 00 00", 0x3193B88};
    inline const sig::Signature kEnvLea{"env_lea", kPedEnvUpdate, sig::Offset{0x3B}, "48 8D 0D ?? ?? ?? ??", 0x3193BC3};
    inline const sig::Signature kEnvironment{"listener_environment", kEnvLea, sig::Rel32{0x3, 0x7}, "", 0x5B9C970};
    // The volume builder: the engine's override (flags +0x650, matrix +0x6B0..
    // +0x6E0) and the slots of the points (+0x50 weight, +0x70), through the
    // point slot getter.
    inline const sig::Signature kVolumeBuilder{"listener_volume_builder",
        "48 8B C4 88 50 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC A8 01 00 00 0F 29 70 A8",
        0x31EB188};
    inline const sig::Signature kVolOverrideRead{"vol_override_read", kVolumeBuilder, sig::Offset{0x495},
        "8B 87 50 06 00 00 C1 E8 08", 0x31EB61D};
    inline const sig::Signature kVolMatrixPosTest{"vol_matrix_pos_test", kVolumeBuilder, sig::Offset{0x4AE},
        "F3 0F 6F 97 E0 06 00 00", 0x31EB636};
    inline const sig::Signature kVolMatrixRead{"vol_matrix_read", kVolumeBuilder, sig::Offset{0x50B},
        "0F 10 87 B0 06 00 00 0F 10 AF C0 06 00 00", 0x31EB693};
    inline const sig::Signature kSlotF70Write{"slot_f70_write", kVolumeBuilder, sig::Offset{0x62A},
        "0F 11 43 70", 0x31EB7B2};
    inline const sig::Signature kSlotWeightWrite{"slot_weight_write", kVolumeBuilder, sig::Offset{0x645},
        "0F 11 53 50", 0x31EB7CD};
    inline const sig::Signature kPointSlot{"point_slot", kVolumeBuilder, sig::Rel32{0x435, 0x439},
        "0F B6 C2 3B 81 B0 02 00 00", 0x252BFA4};
    inline const sig::Signature kVolumeReset{"listener_volume_reset",
        "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 83 EC 40 48 8D B1 60 06 00 00", 0x31E8144};
    inline const sig::Signature kVolOverrideReset{"vol_override_reset", kVolumeReset, sig::Offset{0x109},
        "8B 83 50 06 00 00 C1 E8 08 40 84 C7", 0x31E824D};
    // The volume metric (the minimum squared distance over the points).
    inline const sig::Signature kVolumeMetric{"listener_volume_metric",
        "0F 28 05 ?? ?? ?? ?? 45 33 C9 44 8B 99 B0 02 00 00", 0x2525308};
    inline const sig::Signature kVolMetric{"vol_metric", kVolumeMetric, sig::Offset{0xA},
        "44 8B 99 B0 02 00 00", 0x2525312};
    inline const sig::Signature kListenerUpdate{"listener_index_update",
        "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00", 0x31EDCCC};
    inline const sig::Signature kListenerIndex{"listener_index", kListenerUpdate, sig::Offset{0xA7},
        "83 BB 40 06 00 00 FF 75 0F 0F BE 8B 84 07 00 00", 0x31EDD73};
    // The microphone chooser: the override +0x780 and its crossfade +0x778,
    // the microphone table and its lookup by hash (table, hash, type) ->
    // metadata or null, the camera map (env, u32* out, map) -> out, a
    // metadata reference (never called by the replay), and the microphone in
    // use (+0x320 + 0x310 * current).
    inline const sig::Signature kMicChooser{"listener_mic_chooser",
        "48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 20 0F B6 05 ?? ?? ?? ??", 0x31D8AE4};
    inline const sig::Signature kMicOverrideRead{"mic_override_read", kMicChooser, sig::Offset{0x33},
        "8B 81 80 07 00 00", 0x31D8B17};
    inline const sig::Signature kMicTableLea{"mic_table_lea", kMicChooser, sig::Offset{0x45},
        "48 8D 0D ?? ?? ?? ??", 0x31D8B29};
    inline const sig::Signature kMicTable{"listener_mic_table", kMicTableLea, sig::Rel32{0x3, 0x7}, "", 0x5B87450};
    inline const sig::Signature kMetadataLookup{"listener_metadata_lookup", kMicChooser, sig::Rel32{0x4F, 0x53},
        "40 53 48 83 EC 20 41 0F B6 D8 45 33 C9 44 8B C3 E8 ?? ?? ?? ??",
        0x252B584};
    inline const sig::Signature kMicCrossfadeRead{"mic_crossfade_read", kMicChooser, sig::Offset{0x53},
        "8B BB 78 07 00 00", 0x31D8B37};
    inline const sig::Signature kMicMapCall{"mic_map_call", kMicChooser, sig::Offset{0xA9},
        "E8 ?? ?? ?? ??", 0x31D8B8D};
    inline const sig::Signature kMicMap{"listener_mic_map", kMicMapCall, sig::Rel32{0x1, 0x5},
        "48 89 5C 24 08 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20", 0x31DFCDC};
    inline const sig::Signature kMicMapRefLookup{"mic_map_ref_lookup", kMicChooser, sig::Offset{0xBC},
        "E8 ?? ?? ?? ??", 0x31D8BA0};
    // The camera map's answer is a metadata REFERENCE (chunk << 24 | offset),
    // looked up by the reference lookup through the chunk find, not a hash
    // (the metadata lookup, through the entry search). The hash of the
    // microphone in use comes from the chunk's (offset, hash) list instead.
    inline const sig::Signature kMetaRefLookup{"listener_meta_ref_lookup", kMicMapRefLookup, sig::Rel32{0x1, 0x5},
        "40 53 48 83 EC 20 41 0F B6 D8 45 33 C9 44 8B C3 E8 ?? ?? ?? ?? 33 C9 48 85 C0 74 06 3A 18 48 0F 45 C1 48 83 "
        "C4 20 5B C3", 0x252B5AC};
    inline const sig::Signature kMicInUseCmp{"mic_in_use_cmp", kMicChooser, sig::Offset{0x122},
        "48 3B 94 18 20 03 00 00", 0x31D8C06};
    // The metadata table's layout: the chunks (the chunk find), the entries
    // (the entry test and the entry search).
    inline const sig::Signature kMetaChunkFind{"listener_meta_chunk_find",
        "8D 42 FF 83 F8 FD 77 32 0F B7 41 08", 0x2516734};
    inline const sig::Signature kMetaChunkCountSite{"meta_chunk_count", kMetaChunkFind, sig::Offset{0x8},
        "0F B7 41 08", 0x251673C};
    inline const sig::Signature kMetaChunksSite{"meta_chunks", kMetaChunkFind, sig::Offset{0x18},
        "4C 8B 09", 0x251674C};
    inline const sig::Signature kMetaChunkStrideSite{"meta_chunk_stride", kMetaChunkFind, sig::Offset{0x24},
        "4C 69 C0 D0 00 00 00", 0x2516758};
    inline const sig::Signature kMetaChunkSizeSite{"meta_chunk_size", kMetaChunkFind, sig::Offset{0x2B},
        "43 3B 54 08 18", 0x251675F};
    inline const sig::Signature kMetaChunkDataSite{"meta_chunk_data", kMetaChunkFind, sig::Offset{0x34},
        "4B 03 44 08 10", 0x2516768};
    inline const sig::Signature kMetaEntryTest{"listener_meta_entry_test",
        "48 89 5C 24 08 45 8B 50 1C 45 33 C9", 0x2536130};
    inline const sig::Signature kMetaEntryCountSite{"meta_entry_count", kMetaEntryTest, sig::Offset{0x5},
        "45 8B 50 1C", 0x2536135};
    inline const sig::Signature kMetaChunkLoadedSite{"meta_chunk_loaded", kMetaEntryTest, sig::Offset{0x14},
        "45 38 88 CD 00 00 00", 0x2536144};
    inline const sig::Signature kMetaEntriesSite{"meta_entries", kMetaEntryTest, sig::Offset{0x44},
        "4D 8B 40 20", 0x2536174};
    inline const sig::Signature kMetaEntryHashSite{"meta_entry_hash", kMetaEntryTest, sig::Offset{0x54},
        "45 3B 5C C0 04", 0x2536184};
    inline const sig::Signature kMetaEntrySearch{"listener_meta_entry_search",
        "48 89 5C 24 08 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 83 C8 FF", 0x2535F8C};
    inline const sig::Signature kMetaEntryOffsetSite{"meta_entry_offset", kMetaEntrySearch, sig::Offset{0xFB},
        "8B 0C C8", 0x2536087};

    // Every site the module needs, in the order it checks them (the first
    // one missing is logged).
    inline const sig::Signature* const kSites[] = {&kEnvLea, &kVolOverrideRead, &kVolMatrixPosTest, &kVolMatrixRead,
        &kVolOverrideReset, &kVolMetric, &kPointSlot, &kListenerIndex, &kMicOverrideRead, &kMicCrossfadeRead,
        &kMicInUseCmp, &kMicTableLea, &kMicMapCall, &kSlotF70Write, &kSlotWeightWrite, &kMicMapRefLookup,
        &kMetaChunkCountSite, &kMetaChunksSite, &kMetaChunkStrideSite, &kMetaChunkSizeSite, &kMetaChunkDataSite,
        &kMetaEntryCountSite, &kMetaChunkLoadedSite, &kMetaEntriesSite, &kMetaEntryHashSite, &kMetaEntryOffsetSite,
        &kEnvironment, &kMicTable, &kMicMap, &kMetaRefLookup};

    constexpr std::uint32_t ListenerOffset(std::uint32_t index) { return kListenerBase + index * kListenerStride; }

    constexpr std::uint32_t SlotOffset(std::uint32_t point) { return kSlotBase + point * kVolumePointStride; }
    static_assert(SlotOffset(0) + 0x40 == kVolumePointPos, "the point position is slot +0x40");

    // Where the recorded microphone hash came from.
    enum class MicSource : std::uint8_t
    {
        None = 0,      // not read (a v1 clip, or the read failed)
        Override = 1,  // +0x780 was set (an animated scene's camera metadata)
        CameraMap = 2, // the metadata in use (camera map path), its hash from the table entries
        Default = 3,   // the metadata in use is the default 0x2A7C18A2
    };
    inline const char* MicSourceName(MicSource s)
    {
        switch (s)
        {
        case MicSource::None: return "none";
        case MicSource::Override: return "override";
        case MicSource::CameraMap: return "camera_map";
        case MicSource::Default: return "default";
        default: return "?";
        }
    }

    // One volume point of a sample (LSTN v2).
    struct Point
    {
        float pos[3] = {};
        float weight = 0.0f; // slot +0x50
        float f70 = 0.0f;    // slot +0x70
    };

    // One recorded sample of the live volume listener.
    struct Sample
    {
        std::uint32_t timeMs = 0;
        float pos[3] = {};
        std::uint8_t points = 0; // volume points of the current description (live)
        std::uint8_t flags = 0;  // kSampleOverride: the engine's own override was on (a scene)
        // v2: the microphone and every point (up to kMaxRecordedPoints).
        std::uint32_t micHash = 0;
        MicSource micSource = MicSource::None;
        std::uint8_t stored = 0;
        Point point[kMaxRecordedPoints];
    };
    constexpr std::uint8_t kSampleOverride = 1;

    inline bool HasV2(const Sample& s) { return s.micSource != MicSource::None || s.stored != 0; }

    // The recording's part (Recording::gameListener, clip section LSTN).
    struct Track
    {
        std::vector<Sample> samples; // time ascending

        void Clear() { samples.clear(); }
        bool AnyV2() const
        {
            for (const Sample& s : samples)
                if (HasV2(s))
                    return true;
            return false;
        }
        bool Empty() const { return samples.empty(); }
        std::size_t ApproxBytes() const { return samples.capacity() * sizeof(Sample); }
        void DropBefore(std::uint32_t keepFromMs)
        {
            samples.erase(std::remove_if(samples.begin(), samples.end(),
                              [keepFromMs](const Sample& s) { return s.timeMs < keepFromMs; }),
                samples.end());
        }
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs) const
        {
            out.Clear();
            for (const Sample& s : samples)
            {
                if (s.timeMs < baseMs || s.timeMs > endMs)
                    continue;
                Sample copy = s;
                copy.timeMs -= baseMs;
                out.samples.push_back(copy);
            }
        }
    };

    // Recording rate: one sample per kSamplePeriodMs of clip time (the game
    // camera is sampled every tick; a cut moves the listener at once).
    constexpr std::uint32_t kSamplePeriodMs = 33;
    // Playback: interpolate only inside a short, small step; a longer gap or
    // a jump (a camera cut) holds the earlier sample until the next.
    constexpr std::uint32_t kInterpolateMaxGapMs = 120;
    constexpr float kInterpolateMaxMeters = 3.0f;
    // A sample further than this from the playhead is not used.
    constexpr std::uint32_t kStaleMs = 500;

    inline bool SampleDue(const Track& track, std::uint32_t timeMs)
    {
        return track.samples.empty() || timeMs < track.samples.back().timeMs ||
            timeMs - track.samples.back().timeMs >= kSamplePeriodMs;
    }

    // Appends in time order (a clock that went back drops the newer tail,
    // the way a rewound recording overwrites).
    inline void Append(Track& track, const Sample& s)
    {
        while (!track.samples.empty() && track.samples.back().timeMs >= s.timeMs)
            track.samples.pop_back();
        track.samples.push_back(s);
    }

    inline float Distance(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The recorded listener at timeMs. False when the track has nothing near.
    inline bool At(const Track& track, std::uint32_t timeMs, float out[3])
    {
        const std::vector<Sample>& s = track.samples;
        if (s.empty())
            return false;
        auto it = std::upper_bound(s.begin(), s.end(), timeMs, [](std::uint32_t t, const Sample& x) { return t < x.timeMs; });
        if (it == s.begin())
        {
            if (it->timeMs - timeMs > kStaleMs)
                return false;
            std::copy(it->pos, it->pos + 3, out);
            return true;
        }
        const Sample& a = *(it - 1);
        if (it == s.end())
        {
            if (timeMs - a.timeMs > kStaleMs)
                return false;
            std::copy(a.pos, a.pos + 3, out);
            return true;
        }
        const Sample& b = *it;
        const std::uint32_t gap = b.timeMs - a.timeMs;
        if (gap == 0 || gap > kInterpolateMaxGapMs || Distance(a.pos, b.pos) > kInterpolateMaxMeters)
        {
            if (timeMs - a.timeMs > kStaleMs)
                return false;
            std::copy(a.pos, a.pos + 3, out);
            return true;
        }
        const float t = static_cast<float>(timeMs - a.timeMs) / static_cast<float>(gap);
        for (int n = 0; n < 3; ++n)
            out[n] = a.pos[n] + (b.pos[n] - a.pos[n]) * t;
        return true;
    }

    // ---- The microphone put back ----

    // The sample in force at timeMs (the last one at or before it; the first
    // one when the playhead is just before it), none when stale.
    inline const Sample* SampleAt(const Track& track, std::uint32_t timeMs)
    {
        const std::vector<Sample>& s = track.samples;
        if (s.empty())
            return nullptr;
        auto it = std::upper_bound(s.begin(), s.end(), timeMs, [](std::uint32_t t, const Sample& x) { return t < x.timeMs; });
        if (it == s.begin())
            return it->timeMs - timeMs <= kStaleMs ? &*it : nullptr;
        const Sample& a = *(it - 1);
        return timeMs - a.timeMs <= kStaleMs ? &a : nullptr;
    }

    enum class MicWho : std::uint8_t
    {
        None = 0,
        Recorded, // the clip's own hash (LSTN v2)
        Derived,  // a clip without it: the fallback hash on a sample the live game had 2 points in
    };
    inline const char* MicWhoName(MicWho w)
    {
        return w == MicWho::Recorded ? "recorded" : w == MicWho::Derived ? "derived" : "none";
    }

    struct MicPlan
    {
        std::uint32_t hash = 0;
        MicWho who = MicWho::None;
        std::uint8_t pointsRec = 0;
        bool haveSample = false;
    };

    // Which microphone the playback asks for at timeMs: the recorded one; for
    // a clip without it (v1) the fallback only where the live game
    // had 2 volume points (with 1 point the replay already matches the live
    // listener, d_dist -0.3 to -1.7 m).
    inline MicPlan MicAt(const Track& track, std::uint32_t timeMs, std::uint32_t fallbackHash)
    {
        MicPlan plan;
        const Sample* s = SampleAt(track, timeMs);
        if (!s)
            return plan;
        plan.haveSample = true;
        plan.pointsRec = s->points;
        if (s->micSource != MicSource::None && s->micHash)
        {
            plan.hash = s->micHash;
            plan.who = MicWho::Recorded;
        }
        else if (s->micSource == MicSource::None && s->points >= 2 && fallbackHash)
        {
            plan.hash = fallbackHash;
            plan.who = MicWho::Derived;
        }
        return plan;
    }

    // The replay camera follows the recorded game camera: the replay source,
    // or a marker camera within limitM of the recorded camera at the playhead.
    inline bool FollowsRecordedCamera(bool replaySource, bool markerSource, float camErrM, float limitM)
    {
        if (replaySource)
            return true;
        return markerSource && camErrM >= 0.0f && camErrM <= limitM;
    }

    enum class MicAction : std::uint8_t
    {
        Keep = 0,  // nothing to write
        Write,     // put the planned hash in +0x780
        Restore,   // give +0x780 back (0) because the replay's value is no longer wanted
        Forget,    // someone else wrote +0x780 over ours: stop owning it, write nothing
    };
    inline const char* MicActionName(MicAction a)
    {
        switch (a)
        {
        case MicAction::Keep: return "keep";
        case MicAction::Write: return "write";
        case MicAction::Restore: return "restore";
        case MicAction::Forget: return "forget";
        default: return "?";
        }
    }

    struct MicInputs
    {
        bool enabled = false;
        bool proven = false;
        bool follows = false;      // FollowsRecordedCamera
        std::uint32_t plan = 0;    // MicAt hash, 0 none
        std::uint32_t current = 0; // +0x780 now
        std::uint32_t ours = 0;    // what the replay wrote and still owns, 0 none
    };

    // The rule of the writes: the replay writes +0x780
    // only over 0 or over its own value, and gives back only its own value.
    inline MicAction DecideMic(const MicInputs& in, const char*& why)
    {
        if (in.ours && in.current != in.ours)
        {
            why = "taken_by_engine";
            return MicAction::Forget;
        }
        if (!in.enabled || !in.proven)
        {
            why = !in.enabled ? "off" : "unproven";
            return in.ours ? MicAction::Restore : MicAction::Keep;
        }
        if (in.current && !in.ours)
        {
            why = "engine_override";
            return MicAction::Keep;
        }
        if (!in.follows || !in.plan)
        {
            why = !in.follows ? "not_recorded_camera" : "no_hash";
            return in.ours ? MicAction::Restore : MicAction::Keep;
        }
        if (in.ours == in.plan && in.current == in.plan)
        {
            why = "applied";
            return MicAction::Keep;
        }
        why = "apply";
        return MicAction::Write;
    }
}
