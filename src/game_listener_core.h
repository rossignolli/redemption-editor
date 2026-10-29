#pragma once
// The engine's volume listener of the live game, recorded and put back when
// the replay plays the recorded game camera.
//
// With the same game camera the replay was 7 dB lower without it: the ped
// audio distance metric (+0x4640, the minimum squared distance to the
// listeners' volume points) gives 1.0 m for the player's own lines live
// (camera 2 to 10 m away) and the camera distance in the replay. The live
// listener sits near the player, the replay's at its script camera.
//
// Engine layout (RDR2 1491.50):
// - the audio environment singleton (RVA 0x5B9C970, `lea rcx` site 0x3193BC3)
//   holds two listener descriptions of 0x310 bytes at +0x20 (current and
//   next of a microphone crossfade; current index byte +0x784);
// - each description has volume points: count dword +0x2B0, point j position
//   at +0x1D0 + j*0x90 (0x2525308 takes the minimum squared distance over
//   them, 0x252BFA4 returns the slot);
// - the microphone settings are chosen per frame from the dominant camera
//   (0x31D8AE4: the override hash +0x780 a camera's metadata sets, else a map
//   from the camera's metadata name), and each volume microphone places its
//   point on an entity (type 2 the camera's attach entity, 3/4/8 globals that
//   hold the player, 0 the camera frame) plus micLength toward or away from
//   the camera (0x31D8C34);
// - the engine's own volume listener override: bit 8 (0x100) of +0x650 with
//   the matrix +0x6B0..+0x6E0 (position +0x6E0) replaces the computed point
//   (0x31EB61D..0x31EB6C0); the animated scene handlers 0x324C5D8/0x324C728
//   set it, 0x31E824D resets the matrix once the bit is clear.
//
// The replay records the live volume point (absolute position) and, while it
// renders the recorded game camera, puts it back through that override, the
// way the engine's scenes do (a write to a world system: only while the
// replay owns the world, cleared on exit).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::gamelistener
{
    // Layout (RDR2 1491.50).
    constexpr std::uint32_t kEnvironmentRva = 0x5B9C970;
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
    // The microphone:
    // - per point j the slot is desc+0x190+j*0x90 (0x252BFA4); 0x31EB188
    //   writes its weight 1/count at slot+0x50 (0x31EB7CD) and the microphone
    //   metadata's float +0xC at slot+0x70 (0x31EB7B2);
    // - the microphone in use is the metadata pointer at desc+0x300
    //   (0x31D8C06 compares [env+0x320+0x310*cur]);
    // - 0x31D8AE4 takes the override hash +0x780 with the crossfade +0x778
    //   (0x31D8B37); without the override it asks the camera map 0x31DFCDC
    //   (writes only its out dword and its frame; callees are getters and a
    //   binary search), whose answer is a metadata reference looked up by
    //   0x252B5AC (not a hash), then a second map at
    //   env+0x10, else the default hash 0x2A7C18A2 by 0x252B584 (table
    //   0x5B87450, type 0xA9); the replay records the hash of the metadata in
    //   use (desc+0x300) through the table's (offset, hash) entries;
    // - the engine writes +0x780 only at 0x32764C8, 0x3277E8E (an animated
    //   scene's camera metadata, crossfade +0x778 first) and zeroes it at
    //   0x3279896, 0x327ABBD (rip relative stores; the only other access to
    //   +0x780 in the code that names the environment is the read 0x31D8B17).
    constexpr std::uint32_t kMicCrossfade = 0x778;     // u32 ms, read with the override (0x31D8B37)
    constexpr std::uint32_t kSlotBase = 0x190;         // per description, slot j = +0x190 + j*0x90
    constexpr std::uint32_t kSlotWeight = 0x50;        // f32 (vec4 broadcast), 1/count
    constexpr std::uint32_t kSlotF70 = 0x70;           // f32 (vec4 broadcast), the microphone metadata's +0xC
    constexpr std::uint32_t kMicInUse = 0x300;         // per description, the microphone metadata pointer
    constexpr std::uint32_t kMicTableRva = 0x5B87450;  // the audio metadata table 0x31D8AE4 searches
    constexpr std::uint8_t kMicMetadataType = 0xA9;
    constexpr std::uint32_t kMicDefaultHash = 0x2A7C18A2u;
    constexpr std::uint32_t kMicMapRva = 0x31DFCDC;    // (env, u32* out, map) -> out (a metadata reference); not called
    constexpr std::uint8_t kMicMapSignature[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20};
    constexpr std::uint32_t kMetadataLookupRva = 0x252B584; // (table, hash, type) -> metadata or null
    constexpr std::uint8_t kMetadataLookupSignature[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xD8, 0x45,
        0x33, 0xC9, 0x44, 0x8B, 0xC3, 0xE8};
    constexpr std::uint8_t kMaxRecordedPoints = 4;     // LSTN v2 keeps up to this many points per sample
    // The metadata table (0x2516734,
    // 0x2535F8C, 0x2536130): [table] chunks, word [table+8] chunk count,
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

    // Code sites checked at runtime before any read or write: the bytes prove
    // the global (env_lea) and every field offset this module touches.
    struct Site
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[16];
    };
    constexpr Site kSites[] = {
        {"env_lea", 0x3193BC3, 7, {0x48, 0x8D, 0x0D, 0xA6, 0x8D, 0xA0, 0x02}},
        {"vol_override_read", 0x31EB61D, 9, {0x8B, 0x87, 0x50, 0x06, 0x00, 0x00, 0xC1, 0xE8, 0x08}},
        {"vol_matrix_pos_test", 0x31EB636, 8, {0xF3, 0x0F, 0x6F, 0x97, 0xE0, 0x06, 0x00, 0x00}},
        {"vol_matrix_read", 0x31EB693, 14, {0x0F, 0x10, 0x87, 0xB0, 0x06, 0x00, 0x00, 0x0F, 0x10, 0xAF, 0xC0, 0x06, 0x00, 0x00}},
        {"vol_override_reset", 0x31E824D, 12, {0x8B, 0x83, 0x50, 0x06, 0x00, 0x00, 0xC1, 0xE8, 0x08, 0x40, 0x84, 0xC7}},
        {"vol_metric", 0x2525312, 7, {0x44, 0x8B, 0x99, 0xB0, 0x02, 0x00, 0x00}},
        {"point_slot", 0x252BFA4, 9, {0x0F, 0xB6, 0xC2, 0x3B, 0x81, 0xB0, 0x02, 0x00, 0x00}},
        {"listener_index", 0x31EDD73, 16, {0x83, 0xBB, 0x40, 0x06, 0x00, 0x00, 0xFF, 0x75, 0x0F, 0x0F, 0xBE, 0x8B, 0x84, 0x07, 0x00, 0x00}},
        {"mic_override_read", 0x31D8B17, 6, {0x8B, 0x81, 0x80, 0x07, 0x00, 0x00}},
        // The microphone fields.
        {"mic_crossfade_read", 0x31D8B37, 6, {0x8B, 0xBB, 0x78, 0x07, 0x00, 0x00}},
        {"mic_in_use_cmp", 0x31D8C06, 8, {0x48, 0x3B, 0x94, 0x18, 0x20, 0x03, 0x00, 0x00}},
        {"mic_table_lea", 0x31D8B29, 7, {0x48, 0x8D, 0x0D, 0x20, 0xE9, 0x9A, 0x02}},
        {"mic_map_call", 0x31D8B8D, 5, {0xE8, 0x4A, 0x71, 0x00, 0x00}},
        {"slot_f70_write", 0x31EB7B2, 4, {0x0F, 0x11, 0x43, 0x70}},
        {"slot_weight_write", 0x31EB7CD, 4, {0x0F, 0x11, 0x53, 0x50}},
        // The camera map's answer is a metadata REFERENCE (chunk << 24 | offset),
        // looked up by 0x252B5AC -> 0x2516734, not a hash (0x252B584 ->
        // 0x25166EC -> 0x2535F8C). The hash of the microphone in use comes from
        // the chunk's (offset, hash) list instead.
        {"mic_map_ref_lookup", 0x31D8BA0, 5, {0xE8, 0x07, 0x2A, 0x35, 0xFF}},
        {"meta_chunk_count", 0x251673C, 4, {0x0F, 0xB7, 0x41, 0x08}},
        {"meta_chunks", 0x251674C, 3, {0x4C, 0x8B, 0x09}},
        {"meta_chunk_stride", 0x2516758, 7, {0x4C, 0x69, 0xC0, 0xD0, 0x00, 0x00, 0x00}},
        {"meta_chunk_size", 0x251675F, 5, {0x43, 0x3B, 0x54, 0x08, 0x18}},
        {"meta_chunk_data", 0x2516768, 5, {0x4B, 0x03, 0x44, 0x08, 0x10}},
        {"meta_entry_count", 0x2536135, 4, {0x45, 0x8B, 0x50, 0x1C}},
        {"meta_chunk_loaded", 0x2536144, 7, {0x45, 0x38, 0x88, 0xCD, 0x00, 0x00, 0x00}},
        {"meta_entries", 0x2536174, 4, {0x4D, 0x8B, 0x40, 0x20}},
        {"meta_entry_hash", 0x2536184, 5, {0x45, 0x3B, 0x5C, 0xC0, 0x04}},
        {"meta_entry_offset", 0x2536087, 3, {0x8B, 0x0C, 0xC8}},
    };

    // The lea's target (rip = the next instruction).
    constexpr std::uint32_t EnvLeaTarget(const Site& lea)
    {
        const std::uint32_t d = static_cast<std::uint32_t>(lea.bytes[3]) | (static_cast<std::uint32_t>(lea.bytes[4]) << 8) |
            (static_cast<std::uint32_t>(lea.bytes[5]) << 16) | (static_cast<std::uint32_t>(lea.bytes[6]) << 24);
        return lea.rva + 7u + d;
    }
    static_assert(EnvLeaTarget(kSites[0]) == kEnvironmentRva, "env_lea must point at the environment");
    static_assert(EnvLeaTarget(kSites[11]) == kMicTableRva, "mic_table_lea must point at the metadata table");
    constexpr std::uint32_t CallTarget(const Site& call)
    {
        const std::uint32_t d = static_cast<std::uint32_t>(call.bytes[1]) | (static_cast<std::uint32_t>(call.bytes[2]) << 8) |
            (static_cast<std::uint32_t>(call.bytes[3]) << 16) | (static_cast<std::uint32_t>(call.bytes[4]) << 24);
        return call.rva + 5u + d;
    }
    static_assert(CallTarget(kSites[12]) == kMicMapRva, "mic_map_call must call the camera map");

    inline bool SiteMatches(const Site& site, const std::uint8_t* code)
    {
        if (!code)
            return false;
        for (std::uint8_t n = 0; n < site.size; ++n)
            if (code[n] != site.bytes[n])
                return false;
        return true;
    }

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

    // Whether the playback puts the recorded listener back this tick.
    enum class Why : std::uint8_t
    {
        Apply = 0,
        Off,             // ReplayGameListener=0
        NotRecordedCam,  // another camera renders (markers, free camera, lent, live game camera)
        NoTrack,         // the clip has no LSTN samples near the playhead
        Unproven,        // a code site did not match: feature disabled
        EngineOverride,  // the engine's own override is on (a scene) and is not ours
    };

    inline const char* WhyName(Why w)
    {
        switch (w)
        {
        case Why::Apply: return "apply";
        case Why::Off: return "off";
        case Why::NotRecordedCam: return "not_recorded_camera";
        case Why::NoTrack: return "no_track";
        case Why::Unproven: return "unproven";
        case Why::EngineOverride: return "engine_override";
        default: return "?";
        }
    }

    struct Inputs
    {
        bool enabled = false;
        bool proven = false;
        bool recordedGameCamera = false;
        bool haveTarget = false;
        bool overrideBitSet = false; // bit 8 of +0x650 now
        bool overrideIsOurs = false; // we set it and nobody cleared it
    };

    inline Why Decide(const Inputs& in)
    {
        if (!in.enabled)
            return Why::Off;
        if (!in.proven)
            return Why::Unproven;
        if (!in.recordedGameCamera)
            return Why::NotRecordedCam;
        if (!in.haveTarget)
            return Why::NoTrack;
        if (in.overrideBitSet && !in.overrideIsOurs)
            return Why::EngineOverride;
        return Why::Apply;
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
