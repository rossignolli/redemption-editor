#pragma once

// Engine sound census, pure part (unit tested in ReplayCoreTests).
//
// The census logs every sound the game side asks the audio engine for, with no
// knowledge of what the recorder records, so a sound that plays live and not in
// the replay shows up even when it goes through a path no track hooks
// (a developer tool compares the recording and playback slices).
//
// Where it listens (build 1491.50, static analysis): the two sound instance
// creators 0x2516CAC and 0x2516900 are the only callers of the requested-sound
// slot allocator 0x251BAC4, so every top-level sound the game creates passes
// one of them. Both take (sound manager, u64 metadata ref, audEntity*,
// audSound** out or null, audSoundInitParams* or null) and return the sound (0
// when refused). Callers: the persistent cores by ref 0x2589124 (41 sites) and
// by hash 0x2589058, the create-and-play queue consumer 0x2588DD8 (0x258A350
// drains the one-shots 0x2589A40 queued), and the 0x2588D1F/0x2588FA4 entries.
// Child sounds of a sound are made by the mixer from the parent's metadata and
// are not seen here.
//
// This header holds what needs no engine: the row, the fields read from
// the init params, the placement class, the owner table (address ranges to
// replay ids) and the clip clock interpolation between two ticks.

#include "sound_check_core.h" // owner classes, audio bases, the cheap mode

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace replay::census
{
    // audSoundInitParams fields read (ctor 0x487E34 defaults in brackets).
    // +0xA4/+0xA8 are the two dword floats after the four 1.0 scales and
    // before the s16 pair at +0xAC/+0xAE [0, -1], in the usual order of stored
    // init params (Volume, Pitch, Pan), so +0xA4 is taken as the volume in dB
    // and +0xA8 logged beside it until confirmed live.
    // kParamsBytes (0x140): sound_check_core.h (the capture keeps them).
    constexpr std::size_t kParamsPosition = 0x00;  // Vec3 [0]
    constexpr std::size_t kParamsVolume = 0xA4;    // f32 [0]
    constexpr std::size_t kParamsVolume2 = 0xA8;   // f32 [0]
    constexpr std::size_t kParamsPitch = 0xAC;     // s16 [0]
    constexpr std::size_t kParamsPan = 0xAE;       // s16 [-1]
    constexpr std::size_t kParamsTracker = 0xC8;   // entity + offset [0]
    constexpr std::size_t kParamsCategory = 0xD8;  // category object [0]
    constexpr std::size_t kParamsFlags = 0x13A;    // byte [0x20]

    constexpr std::uint32_t kCallerDepth = 4; // printed in the SOUND_CENSUS line
    // Frames kept per row: the census detour, then the kPathDepth return
    // addresses of the caller path (sound_check_core.h).
    constexpr std::uint32_t kRowCallers = 1 + kPathDepth;

    struct Row
    {
        std::uint64_t timeUs = 0;
        std::uint8_t core = 0; // 0: 0x2516CAC, 1: 0x2516900
        std::uint64_t ref = 0;
        std::uint64_t audio = 0; // audEntity*
        bool hasParams = false;
        float position[3] = {};
        float volume = 0.0f;
        float volume2 = 0.0f;
        std::int16_t pitch = 0;
        std::int16_t pan = -1;
        std::uint64_t tracker = 0;
        std::uint64_t category = 0;
        std::uint8_t flags = 0;
        std::uint64_t sound = 0; // the creator's result, 0 = refused
        std::uint32_t callers[kRowCallers] = {};
        // The creator's audSound** (0 for none) and whether the mod itself made
        // the sound (census::ModScope on that thread).
        std::uint64_t out = 0;
        bool mine = false;
        std::uint8_t params[kParamsBytes] = {}; // the init params as passed (hasParams)
    };

    // Fills the params fields of a row from a copy of the init params.
    inline void ReadParams(Row& row, const std::uint8_t* params)
    {
        row.hasParams = params != nullptr;
        if (!params)
            return;
        std::memcpy(row.position, params + kParamsPosition, sizeof(row.position));
        std::memcpy(&row.volume, params + kParamsVolume, 4);
        std::memcpy(&row.volume2, params + kParamsVolume2, 4);
        std::memcpy(&row.pitch, params + kParamsPitch, 2);
        std::memcpy(&row.pan, params + kParamsPan, 2);
        std::memcpy(&row.tracker, params + kParamsTracker, 8);
        std::memcpy(&row.category, params + kParamsCategory, 8);
        row.flags = params[kParamsFlags];
    }

    enum class Placement : std::uint8_t
    {
        NoParams, // no init params: the audEntity's defaults
        Tracker,  // 3D, follows an entity (params +0xC8)
        Position, // 3D at a fixed position
        Panned,   // 2D with a pan
        Entity,   // params without position, tracker or pan
    };

    inline Placement Classify(const Row& row)
    {
        if (!row.hasParams)
            return Placement::NoParams;
        if (row.tracker)
            return Placement::Tracker;
        if (row.position[0] != 0.0f || row.position[1] != 0.0f || row.position[2] != 0.0f)
            return Placement::Position;
        if (row.pan >= 0)
            return Placement::Panned;
        return Placement::Entity;
    }

    inline const char* PlacementName(Placement placement)
    {
        switch (placement)
        {
        case Placement::NoParams: return "np";
        case Placement::Tracker: return "3t";
        case Placement::Position: return "3p";
        case Placement::Panned: return "2d";
        case Placement::Entity: return "ent";
        }
        return "?";
    }

    // ------------------------------------------------------------ owners

    enum class OwnerKind : std::uint8_t
    {
        None,
        Recorded,   // an entity of the running recording
        Puppet,     // a playback puppet (its recorded id)
        LivePlayer, // the player ped while a playback runs (not a puppet)
    };

    inline const char* OwnerKindName(OwnerKind kind)
    {
        switch (kind)
        {
        case OwnerKind::None: return "none";
        case OwnerKind::Recorded: return "rec";
        case OwnerKind::Puppet: return "pup";
        case OwnerKind::LivePlayer: return "live_player";
        }
        return "?";
    }

    struct Owner
    {
        std::uint64_t lo = 0; // [lo, hi)
        std::uint64_t hi = 0;
        OwnerKind kind = OwnerKind::None;
        std::uint32_t id = 0xFFFF0000u; // packed replay id (slot << 16 | instance)
        bool player = false;
        // What the owner is and which of its audio bases this range is (lo is
        // the base address; sound_check_core.h).
        OwnerClass cls = OwnerClass::World;
        AudioBase base = AudioBase::Entity;
        std::uint16_t field = 0;
        std::uint64_t entity = 0; // the owner entity's address
    };

    // How the owner was found: the tracker names the entity the sound
    // follows, which is more specific than the audio entity (a static audio
    // entity such as the collision audio plays for many entities).
    enum class Match : std::uint8_t
    {
        None,
        Tracker,
        Audio,
    };

    struct OwnerTable
    {
        std::vector<Owner> audio;   // ranges an audEntity* falls in
        std::vector<Owner> tracker; // ranges a params tracker falls in

        void Clear()
        {
            audio.clear();
            tracker.clear();
        }

        static const Owner* In(const std::vector<Owner>& ranges, std::uint64_t address)
        {
            if (!address)
                return nullptr;
            // The narrowest range wins (a weapon's audio inside its ped's
            // span is the weapon's).
            const Owner* best = nullptr;
            for (const Owner& owner : ranges)
            {
                if (address >= owner.lo && address < owner.hi &&
                    (!best || owner.hi - owner.lo < best->hi - best->lo))
                {
                    best = &owner;
                }
            }
            return best;
        }

        const Owner* Find(std::uint64_t audioEntity, std::uint64_t trackerAddress, Match& match) const
        {
            // A weapon's own audio names the weapon even when the params track its
            // ped (the replay plays the carried weapon sounds, SND kind 4, with the
            // ped puppet as tracker).
            if (const Owner* owner = In(audio, audioEntity); owner && owner->base == AudioBase::WeaponAudio)
            {
                match = Match::Audio;
                return owner;
            }
            if (const Owner* owner = In(tracker, trackerAddress))
            {
                match = Match::Tracker;
                return owner;
            }
            if (const Owner* owner = In(audio, audioEntity))
            {
                match = Match::Audio;
                return owner;
            }
            match = Match::None;
            return nullptr;
        }
    };

    inline std::string PackedIdText(std::uint32_t packed)
    {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%u:%u", packed >> 16, packed & 0xFFFFu);
        return text;
    }

    // ------------------------------------------------------------ clock

    // Clip time of a row made at `us`, between the previous tick (us0, ms0)
    // and this one (us1, ms1). Rows older than the previous tick (the first
    // tick of a phase) take ms0; a pause (ms1 == ms0) keeps the frozen time.
    inline double ClipMsAt(std::uint64_t us0, double ms0, std::uint64_t us1, double ms1, std::uint64_t us)
    {
        if (us1 <= us0 || us <= us0)
            return us1 <= us0 ? ms1 : ms0;
        if (us >= us1)
            return ms1;
        const double f = static_cast<double>(us - us0) / static_cast<double>(us1 - us0);
        return ms0 + (ms1 - ms0) * f;
    }

    // ------------------------------------------------------------ line

    struct LineContext
    {
        const char* phase = "idle"; // rec, play, idle
        double clipMs = 0.0;
        bool normal = true; // clip time running at the recording's rate
        const Owner* owner = nullptr;
        Match match = Match::None;
    };

    // One SOUND_CENSUS line (named fields, read by the comparing tool).
    inline std::string FormatRow(const Row& row, const LineContext& context)
    {
        const OwnerKind kind = context.owner ? context.owner->kind : OwnerKind::None;
        const std::string id = context.owner ? PackedIdText(context.owner->id) : std::string("-");
        char text[512]{};
        std::snprintf(text, sizeof(text),
            "SOUND_CENSUS,ph=%s,t=%.1f,np=%d,us=%llu,core=%c,ref=0x%llx,ok=%d,own=%s,id=%s,pl=%d,by=%s,"
            "d=%s,vol=%.2f,v2=%.2f,pit=%d,pan=%d,cat=0x%llx,fl=0x%02x,ae=0x%llx,tr=0x%llx,c=%x;%x;%x;%x",
            context.phase, context.clipMs, context.normal ? 1 : 0, static_cast<unsigned long long>(row.timeUs),
            row.core ? 'b' : 'a', static_cast<unsigned long long>(row.ref), row.sound ? 1 : 0,
            OwnerKindName(kind), id.c_str(), context.owner && context.owner->player ? 1 : 0,
            context.match == Match::Tracker ? "tr" : (context.match == Match::Audio ? "ae" : "-"),
            PlacementName(Classify(row)), row.volume, row.volume2, static_cast<int>(row.pitch),
            static_cast<int>(row.pan), static_cast<unsigned long long>(row.category),
            static_cast<unsigned int>(row.flags), static_cast<unsigned long long>(row.audio),
            static_cast<unsigned long long>(row.tracker), row.callers[0], row.callers[1], row.callers[2],
            row.callers[3]);
        return text;
    }
}
