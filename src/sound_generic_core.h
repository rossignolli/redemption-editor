#pragma once

// The generic sound effect recorder, pure part (unit tested in
// tests/sound_generic_tests.cpp).
//
// Every top-level sound effect the game creates while recording becomes
// packets in the clip (create, update, stop), as a replay editor's sound
// recorder usually works, through the detours the mod already has on the
// sound ports.
//
// Packets (section SGEN of the clip, Track below):
// - CREATE: born at the enqueue (S03, S04) or at the creator for a direct
//   create; keeps a stable id of the sound (not the raw ref: the ref is not
//   proved stable between sessions, StableIdOf), the owner (a recorded
//   entity, else the position of the init params), the init params that
//   matter, the clip time of the request, the delay until the engine made
//   it and the caller path (family filter).
// - ADJUST: a requested setting of a live sound (position, volume, pitch,
//   the two filters, post submix, curve scale, a sound variable), sampled
//   per sound and per setting: only a changed value, at most UpdateHz
//   times a second, the last value of a burst kept (trailing sample).
// - END: the slot's release (S15, the real end: 68% to 85% of the sounds
//   end without a stop) and a stop (S13) or stop-and-forget (S14) with its
//   reason.
//
// What is left out: child sounds (they never pass the hooked ports), the
// speech's sounds (made inside the speech's persistent-by-ref calls), the
// mod's own sounds, sounds alive before the recording started.
//
// Threads: the detours only push fixed-size events into two single
// consumer rings (EventRing: no lock, no allocation); the script thread
// drains them every recording tick into the Recorder, which resolves the
// owner, samples, keeps the budget and writes the packets.

#include "sound_check_core.h" // OwnerClass, kNoOwner, kOwnerClassCount
#include "event_ring.h"       // the ring protocol

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::soundgeneric
{
    // ------------------------------------------------------------ stable id

    // A metadata ref: 0x0080_0004_0029_934a = flag bit 55 | variant byte 32..39 |
    // set in the low 32 bits. The same set shows with another variant byte in
    // another session (0x0c, 0x03, 0x04 for the wet hoof set), and only 83% to 87%
    // of the refs recur across sessions: the ref is not proved stable, the set is
    // the part that names the sound. The clip keeps set + variant (40 bits) as the
    // id, the raw ref beside it for diagnosis, and counts the refs that do not
    // have this form.
    constexpr std::uint64_t kRefSetMask = 0xFFFFFFFFull;
    constexpr std::uint64_t kRefVariantMask = 0xFFull << 32;
    constexpr std::uint64_t kRefFlagMask = 1ull << 55;
    constexpr std::uint64_t kRefKnownMask = kRefSetMask | kRefVariantMask | kRefFlagMask;

    enum class IdKind : std::uint8_t
    {
        RefSet = 0, // set + variant of a ref of the known form
        Hash = 1,   // creator B: the sound's name hash
        None = 2,   // a ref of another form: the raw ref is all there is
    };
    constexpr std::uint8_t kIdKindCount = 3;

    struct StableId
    {
        std::uint64_t id = 0;
        IdKind kind = IdKind::None;
    };

    inline StableId StableIdOf(std::uint64_t ref, bool byHash)
    {
        StableId out;
        if (byHash)
        {
            out.id = ref & kRefSetMask;
            out.kind = out.id ? IdKind::Hash : IdKind::None;
            return out;
        }
        if ((ref & ~kRefKnownMask) != 0 || (ref & kRefSetMask) == 0)
        {
            out.id = 0;
            out.kind = IdKind::None;
            return out;
        }
        out.id = ref & (kRefSetMask | kRefVariantMask);
        out.kind = IdKind::RefSet;
        return out;
    }

    // The ref a stable id of kind RefSet stands for (the session's own refs carry
    // the flag bit).
    inline std::uint64_t RefOfStableId(std::uint64_t id) { return id | kRefFlagMask; }

    // The ref and the entry a packet's sound is created again with.
    // Creator B (0x2516900, build 1491.50) is called only at 0x25167EC (inside
    // 0x25167D0, called by 0x2516774) and at 0x25168E9 (inside 0x25168A4), both
    // right after 0x252B408 turned a NAME HASH into the metadata ref
    // (`mov edx, dword [rax]` at 0x25167B7 and 0x25168E7). So a packet recorded at
    // creator B (IdKind::Hash, or None with core 1) carries the resolved ref, not a
    // hash: created again through the by-hash port it is looked up as a hash, not
    // found, and creator B gets 0xFFFFFFFF and refuses. The by-ref port
    // (0x2589124 -> 0x2517CB0: a ref whose high dword & 0xF00000 is not 0x800000 is
    // direct and passes whole) takes it as it is, as the GSND track plays creator
    // B's refs. coreBByRef false: by hash.
    inline std::uint64_t CreateRefOf(IdKind kind, std::uint64_t stableId, std::uint64_t rawRef, std::uint8_t core,
        bool coreBByRef, bool& byHash)
    {
        switch (kind)
        {
        case IdKind::RefSet:
            byHash = false;
            return RefOfStableId(stableId);
        case IdKind::Hash:
            byHash = !coreBByRef;
            return stableId & kRefSetMask;
        case IdKind::None:
            break;
        }
        byHash = core != 0 && !coreBByRef;
        return core != 0 && coreBByRef ? (rawRef & kRefSetMask) : rawRef;
    }

    // ------------------------------------------------------------ events

    enum class LifeKind : std::uint8_t
    {
        Enqueue = 0,       // S03 or S04 accepted a request
        Create = 1,        // creator A or B returned (sound 0: refused)
        Stop = 2,          // S13
        StopAndForget = 3, // S14
        Release = 4,       // S15 (not from a creator's failure path)
    };
    constexpr std::uint8_t kLifeKindCount = 5;

    enum class Source : std::uint8_t
    {
        Direct = 0,   // the creator, not through a queue
        Queue = 1,    // S03 create-and-play queue
        Deferred = 2, // S04 deferred request queue
    };
    constexpr std::uint8_t kSourceCount = 3;

    // The init params +0x10C is not a variable list. The creators copy it as
    // dwords (count at +0x124, up to 6: 0x2516AED, 0x25882E9), and the
    // constructor leaves it as it was. The packet keeps its first 6 dwords as
    // three pairs (the dwords go back as they came).
    constexpr std::size_t kVariables = 3; // init params +0x10C: the dword list, as three pairs
    // The sound's init variables are at +0x10: 16 (hash, value) pairs, count at
    // +0x90. The constructor 0x487E34 zeroes them; the params' SetVariableValue
    // (S39, 0x2573638) adds one; the creators pass params +0x10 to the
    // construction (0x2516F99), and a variable block sound's Init (0x257A240)
    // takes the value of each variable it exports from that list (else its
    // metadata's random initial value).
    constexpr std::size_t kInitVariables = 16;

    struct Variable
    {
        std::uint32_t hash = 0;
        float value = 0.0f;
    };

    // A value dword of the init params: its index (offset / 4) and raw bits.
    struct ValueDword
    {
        std::uint8_t index = 0;
        std::uint32_t raw = 0;
    };
    // The value dwords: [0x94, 0x140) less the engine's pointers [0xC8, 0xE8)
    // (tracker, wave slot, category, environment group) = 35. The dword at
    // 0x12C carries the params' u16 handle in its low half (never copied).
    constexpr std::size_t kValueDwords = 35;

    // What the init params say that the packet keeps (the full 0x140 bytes
    // are the sound capture's business; phase 2 may add them).
    struct InitParams
    {
        float position[3] = {};
        float volume = 0.0f;
        float volume2 = 0.0f;
        std::int16_t pitch = 0;
        std::int16_t pan = -1;
        std::uint8_t flags = 0;
        std::uint8_t variableCount = 0;
        bool hasTracker = false;
        Variable variables[kVariables];
        // The init variables (+0x10, count +0x90).
        std::uint8_t initVariableCount = 0;
        Variable initVariables[kInitVariables];
        // Every value dword of the params the game set away from the constructor's
        // default (ValueDwordOffset), so the replayed create gets the params the game
        // passed, as the specific tracks do (CopyRecordedSoundParams); valuesKnown:
        // read from live params or a SGEN v4 packet (an older packet takes them from
        // another track's row).
        bool valuesKnown = false;
        std::uint8_t valueCount = 0;
        ValueDword values[kValueDwords];
        // The game gave the sound an environment group (params +0xE0); the player
        // gives it the owner's.
        bool hasEnvGroup = false;
    };

    // Init params offsets (sound_census_core.h has the same).
    constexpr std::size_t kParamsBytes = 0x140;
    constexpr std::size_t kParamsPosition = 0x00;
    constexpr std::size_t kParamsVolume = 0xA4;
    constexpr std::size_t kParamsVolume2 = 0xA8;
    constexpr std::size_t kParamsPitch = 0xAC;
    constexpr std::size_t kParamsPan = 0xAE;
    constexpr std::size_t kParamsTracker = 0xC8;
    constexpr std::size_t kParamsVariables = 0x10C;
    constexpr std::size_t kParamsVariableCount = 0x124;
    constexpr std::size_t kParamsFlags = 0x13A;
    constexpr std::size_t kParamsInitVariables = 0x10;
    constexpr std::size_t kParamsInitVariableCount = 0x90;

    // The init variables of a params copy (`size` bytes): the count clamped to
    // 16, a pair with a value that is not finite left out.
    inline void ReadInitVariables(const std::uint8_t* params, std::size_t size, InitParams& out)
    {
        out.initVariableCount = 0;
        if (!params || size < kParamsInitVariableCount + 4)
            return;
        std::int32_t count = 0;
        std::memcpy(&count, params + kParamsInitVariableCount, 4);
        if (count <= 0)
            return;
        if (count > static_cast<std::int32_t>(kInitVariables))
            count = static_cast<std::int32_t>(kInitVariables);
        for (std::int32_t n = 0; n < count; ++n)
        {
            Variable v;
            std::memcpy(&v.hash, params + kParamsInitVariables + n * 8, 4);
            std::memcpy(&v.value, params + kParamsInitVariables + n * 8 + 4, 4);
            if (!v.hash || !std::isfinite(v.value))
                continue;
            out.initVariables[out.initVariableCount++] = v;
        }
    }

    // What the constructor 0x487E34 leaves in the params (build 1491.50): 1.0 at
    // +0x94, +0x98, +0x9C, +0xA0 and +0xB0, pan -1 (+0xAE), 0xFF at +0xC4, -1 at
    // +0xF0 (the queue's prepare time limit), 9 at +0x100, 20000 at +0x104 (low
    // pass), -1 at +0x130 and +0x134, 0xFF at +0x136, flags 0x20 at +0x13A; the
    // rest zero (+0xFC and +0x10C..+0x123 it leaves as they were: zero here).
    // +0x13B keeps its high bits: zero here.
    inline void ConstructorParams(std::uint8_t (&out)[kParamsBytes])
    {
        std::memset(out, 0, sizeof(out));
        const float one = 1.0f;
        for (std::size_t o : {std::size_t{0x94}, std::size_t{0x98}, std::size_t{0x9C}, std::size_t{0xA0}, std::size_t{0xB0}})
            std::memcpy(out + o, &one, 4);
        out[0xAE] = 0xFF;
        out[0xAF] = 0xFF;
        out[0xC4] = 0xFF;
        std::memset(out + 0xF0, 0xFF, 4);
        out[0x100] = 9;
        const std::uint32_t lowPass = 20000;
        std::memcpy(out + 0x104, &lowPass, 4);
        std::memset(out + 0x130, 0xFF, 6);
        out[0x136] = 0xFF;
        out[0x13A] = 0x20;
    }

    // The offset of value dword `n` (0 to kValueDwords - 1).
    inline std::size_t ValueDwordOffset(std::size_t n)
    {
        const std::size_t offset = 0x94 + n * 4;
        return offset < 0xC8 ? offset : offset + 0x20;
    }
    inline bool IsValueDwordIndex(std::uint8_t index)
    {
        const std::size_t offset = static_cast<std::size_t>(index) * 4;
        return offset >= 0x94 && offset < kParamsBytes && (offset < 0xC8 || offset >= 0xE8);
    }

    // The value dwords of a params copy (`size` bytes) that differ from the
    // constructor's (the handle half of +0x12C left out).
    inline void ReadValueDwords(const std::uint8_t* params, std::size_t size, InitParams& out)
    {
        out.valueCount = 0;
        out.valuesKnown = false;
        if (!params || size < kParamsBytes)
            return;
        std::uint8_t ctor[kParamsBytes];
        ConstructorParams(ctor);
        for (std::size_t n = 0; n < kValueDwords; ++n)
        {
            const std::size_t offset = ValueDwordOffset(n);
            std::uint32_t raw = 0, base = 0;
            std::memcpy(&raw, params + offset, 4);
            std::memcpy(&base, ctor + offset, 4);
            if (offset == 0x12C)
            {
                raw &= 0xFFFF0000u;
                base &= 0xFFFF0000u;
            }
            if (raw == base)
                continue;
            out.values[out.valueCount].index = static_cast<std::uint8_t>(offset / 4);
            out.values[out.valueCount].raw = raw;
            ++out.valueCount;
        }
        out.valuesKnown = true;
    }

    // Writes the value dwords over constructed params (0x140 bytes).
    inline void ApplyValueDwords(std::uint8_t* params, const InitParams& in)
    {
        for (std::size_t n = 0; n < in.valueCount && n < kValueDwords; ++n)
        {
            const ValueDword& v = in.values[n];
            if (!IsValueDwordIndex(v.index))
                continue;
            const std::size_t offset = static_cast<std::size_t>(v.index) * 4;
            if (offset == 0x12C)
                std::memcpy(params + 0x12E, reinterpret_cast<const std::uint8_t*>(&v.raw) + 2, 2);
            else
                std::memcpy(params + offset, &v.raw, 4);
        }
    }

    // The value dword at `offset` the params would carry (the constructor's when not listed).
    inline std::uint32_t ValueAt(const InitParams& in, std::size_t offset)
    {
        for (std::size_t n = 0; n < in.valueCount && n < kValueDwords; ++n)
            if (static_cast<std::size_t>(in.values[n].index) * 4 == offset)
                return in.values[n].raw;
        std::uint8_t ctor[kParamsBytes];
        ConstructorParams(ctor);
        std::uint32_t raw = 0;
        if (offset + 4 <= kParamsBytes)
            std::memcpy(&raw, ctor + offset, 4);
        return raw;
    }

    // Params +0xF8, the client variable: the creator copies it to the requested
    // settings +0x22C (0x258A9E8 calls 0x25363F8), and the owner's audio entity
    // reads it there every frame when the sound updates with its entity (flags
    // 0x04; e.g. the horse's 0x31A56F8 places a skid's body by it, 0xEE56493E).
    constexpr std::size_t kParamsClientVariable = 0xF8;
    constexpr std::size_t kParamsEnvGroup = 0xE0;
    constexpr std::uint8_t kParamsFlagUpdateEntity = 0x04;

    inline InitParams ReadInitParams(const std::uint8_t* params)
    {
        InitParams out;
        if (!params)
            return out;
        std::memcpy(out.position, params + kParamsPosition, sizeof(out.position));
        std::memcpy(&out.volume, params + kParamsVolume, 4);
        std::memcpy(&out.volume2, params + kParamsVolume2, 4);
        std::memcpy(&out.pitch, params + kParamsPitch, 2);
        std::memcpy(&out.pan, params + kParamsPan, 2);
        std::uint64_t tracker = 0;
        std::memcpy(&tracker, params + kParamsTracker, 8);
        out.hasTracker = tracker != 0;
        out.flags = params[kParamsFlags];
        std::int32_t count = 0;
        std::memcpy(&count, params + kParamsVariableCount, 4);
        out.variableCount = static_cast<std::uint8_t>(count < 0 ? 0 : count > static_cast<int>(kVariables) ? kVariables : count);
        std::uint32_t words[kVariables * 2] = {};
        std::memcpy(words, params + kParamsVariables, sizeof(words));
        // Only the slots the count names are taken; the game leaves the others as
        // they were (e.g. 0xFFFFFFFF, a NaN as a float) and the clip's reader checks
        // every value it reads.
        for (std::size_t n = 0; n < kVariables && n < out.variableCount; ++n)
        {
            out.variables[n].hash = words[n * 2];
            std::memcpy(&out.variables[n].value, &words[n * 2 + 1], 4);
        }
        ReadInitVariables(params, kParamsBytes, out);
        ReadValueDwords(params, kParamsBytes, out);
        std::uint64_t envGroup = 0;
        std::memcpy(&envGroup, params + kParamsEnvGroup, 8);
        out.hasEnvGroup = envGroup != 0;
        return out;
    }

    constexpr std::size_t kCallerFrames = 4;

    // What a life port's detour writes (one slot of the life ring).
    struct LifeEvent
    {
        std::uint64_t timeUs = 0;
        LifeKind kind = LifeKind::Create;
        std::uint8_t core = 0; // Create: 0 creator A (by ref), 1 creator B (by hash)
        Source source = Source::Direct;
        bool refused = false; // Create: the creator returned no sound
        bool speech = false;  // Create: made inside the speech's persistent-by-ref call
        bool mine = false;    // Create: made by the mod itself
        bool hasParams = false;
        std::uint64_t ref = 0;
        std::uint64_t audio = 0;   // the audio entity
        std::uint64_t tracker = 0; // params +0xC8 (Create)
        std::uint64_t sound = 0;   // Create, Stop, StopAndForget, Release
        InitParams params;
        std::uint32_t callers[kCallerFrames] = {}; // RVAs, nearest first (0: none)
    };

    enum class Adjust : std::uint8_t
    {
        Position = 0,          // S16
        PositionNoDoppler = 1, // S17
        Volume = 2,            // S18
        Pitch = 3,             // S19
        LowPass = 4,           // S21
        HighPass = 5,          // S22
        PostSubmix = 6,        // S23
        VolumeCurveScale = 7,  // S24
        Variable = 8,          // S38 (hash, value)
    };
    constexpr std::uint8_t kAdjustKinds = 9;

    inline const char* AdjustName(Adjust kind)
    {
        static const char* const kNames[kAdjustKinds] = {"position", "position_no_doppler", "volume", "pitch", "low_pass",
            "high_pass", "post_submix", "volume_curve_scale", "variable"};
        return static_cast<std::uint8_t>(kind) < kAdjustKinds ? kNames[static_cast<std::uint8_t>(kind)] : "?";
    }
    inline const char* AdjustPort(Adjust kind)
    {
        static const char* const kPorts[kAdjustKinds] = {"S16", "S17", "S18", "S19", "S21", "S22", "S23", "S24", "S38"};
        return static_cast<std::uint8_t>(kind) < kAdjustKinds ? kPorts[static_cast<std::uint8_t>(kind)] : "?";
    }
    inline bool IsVector(Adjust kind) { return kind == Adjust::Position || kind == Adjust::PositionNoDoppler; }

    // What a setting port's detour writes (one slot of the adjust ring).
    struct AdjustEvent
    {
        std::uint64_t timeUs = 0;
        std::uint64_t sound = 0; // [block]: the sound the settings block belongs to
        std::uint32_t hash = 0;  // Variable
        Adjust kind = Adjust::Volume;
        std::uint8_t pad[3] = {};
        float value[3] = {};
    };

    // ------------------------------------------------------------ ring

    // Single consumer ring written by any thread without a lock, with the
    // protocol of event_ring.h: a writer takes a ticket, claims its slot and
    // publishes it; the reader stops at a ticket not yet published (the next
    // drain reads it), checks the sequence after its copy and counts what
    // the writers overwrote or dropped. Fixed size, no allocation after the
    // constructor.
    template <typename T, std::uint32_t N>
    class EventRing
    {
    public:
        static_assert((N & (N - 1)) == 0, "a power of two");

        EventRing() : m_slots(new Slot[N]) {}
        ~EventRing() { delete[] m_slots; }
        EventRing(const EventRing&) = delete;
        EventRing& operator=(const EventRing&) = delete;

        static constexpr std::uint32_t Capacity() { return N; }

        void Push(const T& value) noexcept
        {
            replay::ring::Writer<T> slot(m_slots, N, m_next);
            slot.value = value;
            slot.Publish();
        }

        std::uint64_t Pushed() const noexcept { return m_next.load(std::memory_order_acquire); }
        std::uint64_t Cursor() const noexcept { return m_cursor; }
        std::uint64_t Pending() const noexcept
        {
            const std::uint64_t end = Pushed();
            return end > m_cursor ? end - m_cursor : 0;
        }

        // Reader: everything since the cursor, in order, up to the first
        // event not yet published (the next drain reads it); `dropped`
        // counts the events overwritten, torn or dropped by their writer.
        template <typename Fn>
        std::uint64_t Drain(Fn&& each, std::uint64_t& dropped)
        {
            const replay::ring::Drained drained = replay::ring::ForEachSince(m_slots, N, m_next, m_cursor, each);
            dropped += drained.overflowed + drained.lost;
            return drained.read;
        }

        // Reader: forgets what was pushed so far.
        void Skip() { m_cursor = Pushed(); }

    private:
        using Slot = replay::ring::Slot<T>;

        Slot* m_slots;
        std::atomic<std::uint64_t> m_next{0};
        std::uint64_t m_cursor = 0;
    };

    // ------------------------------------------------------------ packets

    enum class EndReason : std::uint8_t
    {
        Release = 0,       // the slot went back (the sound is over)
        Stop = 1,          // S13: the game stopped it
        StopAndForget = 2, // S14: the game dropped its reference (the sound may play on)
    };
    constexpr std::uint8_t kEndReasonCount = 3;

    struct CreatePacket
    {
        std::uint32_t timeMs = 0; // the request's clip time (enqueue, or the creator for a direct one)
        std::uint32_t index = 0;  // the sound's number in the recording (adjust and end packets name it)
        std::uint64_t stableId = 0;
        IdKind idKind = IdKind::None;
        std::uint64_t rawRef = 0; // diagnosis only
        std::uint32_t owner = census::kNoOwner; // packed recorded id
        census::OwnerClass ownerClass = census::OwnerClass::World;
        Source source = Source::Direct;
        std::uint8_t core = 0;
        bool hasPosition = false; // the params carried a position (owner or not)
        bool hasParams = false;
        bool preexisting = false; // a cut found it alive at the window's base
        std::uint16_t createDelayMs = 0; // creator time minus request time
        InitParams params;
        std::uint32_t caller = 0; // the family caller (FamilyCaller)
        std::uint32_t path[3] = {}; // the caller path as seen
        // Where the sound's audio entity was from its owner (the capture's rule,
        // sound_capture.h ResolveAudioBase), and the audio entity's vtable, so the
        // player recreates the sound on the same audio entity of the owner's puppet.
        // audioKnown false: a v1 packet, or an audio entity no base of the owner
        // reaches (the player falls back to the static audio placed on the owner).
        bool audioKnown = false;
        census::AudioBase audioBase = census::AudioBase::Image;
        std::uint16_t audioField = 0;
        std::int32_t audioOffset = 0;
        std::uint32_t vtableRva = 0;
    };

    struct AdjustPacket
    {
        std::uint32_t timeMs = 0;
        std::uint32_t index = 0;
        Adjust kind = Adjust::Volume;
        std::uint32_t hash = 0; // Variable
        float value[3] = {};
    };

    struct EndPacket
    {
        std::uint32_t timeMs = 0;
        std::uint32_t index = 0;
        EndReason reason = EndReason::Release;
    };

    // Bytes a packet takes in the clip (clip_file.cpp writes exactly these).
    // SGEN v2 adds the audio base (1), field (2), offset (4) and vtable RVA (4) to
    // the create packet: 113 bytes (v1: 102).
    constexpr std::size_t kCreatePacketBytesV1 = 4 + 4 + 8 + 1 + 8 + 4 + 1 + 1 + 1 + 1 + 2 + 12 + 4 + 4 + 2 + 2 + 1 + 1 + 1 +
        kVariables * 8 + 4 + 12;
    constexpr std::size_t kCreatePacketBytes = kCreatePacketBytesV1 + 1 + 2 + 4 + 4;
    // SGEN v3 adds the init variables: a count byte and the pairs.
    constexpr std::size_t kCreatePacketBytesV3Min = kCreatePacketBytes + 1;
    // SGEN v4 adds the value dwords: a count byte and (index, raw) pairs of 5 bytes.
    constexpr std::size_t kCreatePacketBytesV4Min = kCreatePacketBytesV3Min + 1;
    inline std::size_t CreatePacketBytes(const CreatePacket& packet)
    {
        const std::size_t vars = packet.params.initVariableCount > kInitVariables ? kInitVariables : packet.params.initVariableCount;
        const std::size_t values = packet.params.valueCount > kValueDwords ? kValueDwords : packet.params.valueCount;
        return kCreatePacketBytesV4Min + vars * 8 + values * 5;
    }
    constexpr std::size_t kAdjustPacketHeadBytes = 4 + 4 + 1;
    constexpr std::size_t kEndPacketBytes = 4 + 4 + 1;
    inline std::size_t AdjustPacketBytes(Adjust kind)
    {
        return kAdjustPacketHeadBytes + (IsVector(kind) ? 12 : kind == Adjust::Variable ? 8 : 4);
    }

    // The most sounds one recording numbers (nextIndex). The indices are the
    // whole recording session's, so a trimmed clip can hold a few packets
    // with a large nextIndex (76726 for 1067 creates in a QA clip of 38 s):
    // the bound is absolute, far above any session (2^24 is 46 hours at
    // 100 sounds a second), and keeps the tables sized by it (ClipTool,
    // TrimBefore, CutInto) to tens of MB. The clip reader refuses a larger
    // one (the section is left out); the recorder stops numbering there.
    constexpr std::uint32_t kMaxNextIndex = 1u << 24;

    struct Track
    {
        std::vector<CreatePacket> creates;
        std::vector<AdjustPacket> adjusts;
        std::vector<EndPacket> ends;
        std::uint32_t nextIndex = 0; // the next sound's index (every packet index is below it, at most kMaxNextIndex)

        void Clear()
        {
            creates.clear();
            adjusts.clear();
            ends.clear();
            nextIndex = 0;
        }
        bool Empty() const { return creates.empty() && adjusts.empty() && ends.empty(); }
        std::size_t Packets() const { return creates.size() + adjusts.size() + ends.size(); }

        // Memory (the buffer's estimate).
        std::size_t ApproxBytes() const
        {
            return creates.capacity() * sizeof(CreatePacket) + adjusts.capacity() * sizeof(AdjustPacket) +
                ends.capacity() * sizeof(EndPacket);
        }
        // Bytes in the clip file.
        std::size_t FileBytes() const
        {
            std::size_t bytes = ends.size() * kEndPacketBytes;
            for (const CreatePacket& packet : creates)
                bytes += CreatePacketBytes(packet); // SGEN v3
            for (const AdjustPacket& packet : adjusts)
                bytes += AdjustPacketBytes(packet.kind);
            return bytes;
        }
    };

    // ------------------------------------------------------------ callers

    // The caller that names a family: the first frame outside the audio entity's
    // own methods (the create-and-play wrappers 0x2588794 to 0x258A55C, the
    // creators and their entries 0x2516000 to 0x2517000 and the queue drains):
    // those ranges are the same for every sound and say nothing of who asked.
    inline bool InAudioEntityWrappers(std::uint32_t rva)
    {
        return (rva >= 0x2516000u && rva < 0x2517000u) || (rva >= 0x2588794u && rva < 0x258A560u) ||
            (rva >= 0x312D8BCu && rva < 0x312DC00u);
    }

    inline std::uint32_t FamilyCaller(const std::uint32_t* frames, std::size_t count)
    {
        std::uint32_t first = 0;
        for (std::size_t n = 0; n < count; ++n)
        {
            if (!frames[n])
                continue;
            if (!first)
                first = frames[n];
            if (!InAudioEntityWrappers(frames[n]))
                return frames[n];
        }
        return first;
    }

    // ------------------------------------------------------------ budget

    // The player's skip list for the packets whose sound updates with its entity
    // (flag 0x04). The recorded stop and stop-and-forget apply: the puppets' own
    // creates are refused, so the packets are the only copy and nothing else ends
    // them (a sound stopped and forgotten 15 to 20 ms after its create would live
    // on until the owner cap steals it, dozens of voices at once). The recorded
    // volume (S18) applies too: an owner can set a sound to -70 to -90 dB within
    // 60 ms of its create, and the puppet's audio entity does not do it for a copy
    // it did not make. The release stays skipped (the engine's own end, not a
    // request of the owner).
    inline constexpr const char* kEntityDrivenSkipDefault = "release";

    struct Config
    {
        std::uint32_t updateHz = 20;   // SoundGenericUpdateHz: samples per second per sound and setting
        double maxMBPerMin = 8.0;      // SoundGenericMaxMBPerMin: the clip bytes the track may add per minute
        // The player's keys.
        bool play = true;                // SoundGenericPlay: 0 records only (the packets are counted, nothing plays)
        // SoundGenericExport, 0 by default: the player stays off while an export owns
        // the transport clock (the packets due then are counted as disabled).
        bool playInExport = false;
        std::uint32_t preloadMs = 500;   // SoundGenericPreloadMs: create and prepare a loading sound this early (0: off)
        std::uint32_t preloadDelayMs = 100; // SoundGenericPreloadDelayMs: packets that waited this long in the recording
        std::uint32_t holdMs = 150;      // SoundGenericHoldMs: the anti-double wait after a packet's time
        std::uint32_t slots = 64;        // SoundGenericSlots: sounds the player holds at once (8 to 256)
        std::string take;                // SoundGenericTake: family:taker;... (sound_owner_table.h)
        // SoundGenericMode, packets (default): the puppets' own sounds are refused at
        // the creation ports and every recorded packet of a replay entity plays from
        // the clip (sound_owner_table.h); legacy: the owner table and per-track refusals.
        bool packets = true;
        // SoundGenericBlock, packet (default): the ports refuse a puppet's own create
        // only when the recording plays it or it is a proved double; all: everything.
        bool blockAll = false;
        // SoundStepSetGate (default 1): inside a replayed step or melee call, a
        // one-shot of a set the owner never made in the clip is refused
        // (puppet_audio_rule.h).
        bool stepSetGate = true;
        // SoundGenericTrace=owner@fromMs-toMs ("0:1@45000-48000", "*@..." every
        // owner): one log line per packet event in the window.
        std::string trace;
        // SoundGenericEnvGroup=1 (default): a replayed create of an owner gets the
        // puppet's environment group where the game gave one.
        bool envGroups = true;
        // SoundQueueEnvGroup=1: the mode c queue path gets the same group
        // (QueueGenericSound leaves +0xE0 clear otherwise); ShotEnvGroupReplay=1: a
        // weapon puppet's packet asks its holder's group (the live weapon rattle
        // 0x31906D4 puts the holder's at +0xE0, 0x31907CE..0x31907EA; a weapon object
        // answers none).
        bool queueEnvGroups = true;
        bool weaponHolderEnv = true;
        // SoundGenericEntityMode (a, b, c; read at the module's load): how a replayed
        // entity_sound or sound_request packet of an owner makes its sound (EntityMode).
        std::uint8_t entityMode = 0;
        // SoundGenericSkip (read at the module's load, empty by default): what the
        // player does not apply, items by ';'
        // (S16;S18;S19;S21;S22;S23;S24;S38;stop;forget;release).
        std::string skip;
        // SoundClock=1 (default): SOUND_CLOCK lines (clip time against the QPC clock)
        // while recording and playing, so a game audio capture on the same QPC clock
        // maps to clip time exactly.
        bool clockLog = true;
        // SoundGenericDirectParams=0 (default): the direct create builds its params
        // from the packet's fields; 1: from the track row that kept them whole when
        // there is one (the category).
        int directParams = 0;
        // SoundGenericMute=owner@fromMs-toMs (the SoundGenericTrace format): those
        // packets are not created (ablation).
        std::string mute;
        // SoundGenericEntityDrivenSkip (the SoundGenericSkip items, only for the
        // packets whose sound updates with its entity).
        std::string entityDrivenSkip = kEntityDrivenSkipDefault;
        // SoundGenericPlayerRoute=1: the recorded player's packets on the hidden live
        // player's audio entity.
        bool playerRoute = false;
        // SoundGenericWeaponOffShot=1: a weapon object's packet no weapon code asked
        // for and away from every recorded shot of that weapon is the generic
        // player's (soundowner::OffShot; 0: every weapon audio packet is the shot's).
        bool weaponOffShot = true;
        // SoundGenericCoreBByRef=1 (default): a packet recorded at creator B created
        // again by ref (CreateRefOf); SoundGenericKeepLoops=1 (default): the live loops
        // that cover the playhead go on across a jump in play
        // (player::Config::keepLoops).
        bool coreBByRef = true;
        bool keepLoops = true;
        // SoundGenericWorldCap=1 (default): a world packet (no owner) counts the slot
        // cap with the instances of its own sound (stable id, else raw ref), not with
        // every ownerless packet (0, for A/B); SoundGenericWorldCapCount=6: how many
        // instances of one world sound play at once (0: no cap per sound, the slots'
        // distance rule still bounds them).
        bool worldCapPerSound = true;
        std::uint32_t worldCapCount = 6;
        // SoundGenericVehicleBlock=1 (default): in packets mode a puppet vehicle's or
        // train car's own create of a sound the recording plays then is refused at the
        // census creators (vehicle_sound_rule.h; 0: the vehicles make their own copies
        // over the packets).
        bool vehicleBlock = true;
    };

    // The budget's levels: the adjust rate drops first (halved per level),
    // then the adjusts stop, then everything. The level rises when the last
    // ten seconds, scaled to a minute, pass the cap; it falls after ten
    // seconds under half the cap.
    constexpr int kGovernorLevels = 6;
    constexpr int kGovernorAdjustsOff = 4;
    constexpr int kGovernorAllOff = 5;
    constexpr std::uint32_t kGovernorWindowS = 10;
    constexpr std::uint32_t kGovernorRecoverS = 10;

    // SoundGenericMode: "legacy" (any case, spaces around ignored) turns the
    // packets model off; anything else, empty included, is packets (the default).
    // SoundGenericBlock: "all" (any case, spaces around ignored) refuses every own
    // create of the puppets; anything else, empty included, is "packet" (the default).
    inline bool BlockAllOfText(const char* text)
    {
        std::string word;
        for (const char* c = text; c && *c; ++c)
            if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n')
                word += static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
        return word == "all";
    }

    // The ways a replayed sound bound to an entity (flag 0x04: the owner's audio
    // entity updates it every frame, e.g. a skid's body) is made.
    // Detached (a): created without the flag 0x04, the recorded packets (S16
    // position, S18, S38...) drive it alone. Override (b): with the flag, and the
    // player writes the latest recorded variables, volume and pitch back every
    // tick over what the entity computes. GameQueue (c): through the game's own
    // request queue (S03, the recorded params whole), the recorded adjusts on the
    // sound the queue makes.
    enum class EntityMode : std::uint8_t
    {
        Detached = 0,
        Override = 1,
        GameQueue = 2,
        // d: created with the flag the game gave and nothing written back: the
        // puppet's audio entity drives the sound (with SoundGenericEntityDrivenSkip,
        // its recorded volume and ends are left to it too).
        Entity = 3,
    };

    inline EntityMode EntityModeOfText(const char* text)
    {
        std::string word;
        for (const char* c = text; c && *c; ++c)
            if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n')
                word += static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
        if (word == "b" || word == "override")
            return EntityMode::Override;
        if (word == "c" || word == "queue" || word == "game_queue")
            return EntityMode::GameQueue;
        if (word == "d" || word == "entity")
            return EntityMode::Entity;
        return EntityMode::Detached; // "a", empty, anything else
    }

    inline const char* EntityModeLetter(EntityMode mode)
    {
        return mode == EntityMode::Override ? "b" : mode == EntityMode::GameQueue ? "c" : mode == EntityMode::Entity ? "d" : "a";
    }

    inline const char* EntityModeName(EntityMode mode)
    {
        return mode == EntityMode::Override ? "override" : mode == EntityMode::GameQueue ? "game_queue"
            : mode == EntityMode::Entity ? "entity" : "detached";
    }

    inline bool PacketsModeOfText(const char* text)
    {
        std::string word;
        for (const char* c = text; c && *c; ++c)
            if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n')
                word += static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
        return word != "legacy";
    }

    inline std::uint32_t EffectiveHz(std::uint32_t hz, int level)
    {
        if (level >= kGovernorAdjustsOff)
            return 0;
        for (int n = 0; n < level; ++n)
            hz /= 2;
        return hz < 1 ? 1 : hz;
    }

    class Governor
    {
    public:
        void Begin(double maxMBPerMin, std::uint64_t nowUs)
        {
            m_cap = maxMBPerMin;
            m_level = 0;
            m_changes = 0;
            m_calm = 0;
            m_lastBucketUs = nowUs;
            m_bucket = 0;
            for (std::uint64_t& bytes : m_buckets)
                bytes = 0;
            for (std::uint64_t& seconds : m_secondsAtLevel)
                seconds = 0;
            m_rate = 0.0;
        }

        void Note(std::size_t bytes) { m_buckets[m_bucket] += bytes; }

        int Level() const { return m_level; }
        std::uint64_t Changes() const { return m_changes; }
        double RateMBPerMin() const { return m_rate; }
        std::uint64_t SecondsAtLevel(int level) const
        {
            return level >= 0 && level < kGovernorLevels ? m_secondsAtLevel[level] : 0;
        }
        bool AdjustsOff() const { return m_level >= kGovernorAdjustsOff; }
        bool AllOff() const { return m_level >= kGovernorAllOff; }

        // Once a second (any call: it checks the clock). True when the level changed.
        bool Tick(std::uint64_t nowUs)
        {
            if (nowUs < m_lastBucketUs + 1000000ull)
                return false;
            // Close the second (or the seconds a stall skipped).
            while (nowUs >= m_lastBucketUs + 1000000ull)
            {
                m_lastBucketUs += 1000000ull;
                ++m_secondsAtLevel[m_level];
                m_bucket = (m_bucket + 1) % kGovernorWindowS;
                m_buckets[m_bucket] = 0;
            }
            std::uint64_t window = 0;
            for (const std::uint64_t bytes : m_buckets)
                window += bytes;
            m_rate = static_cast<double>(window) * (60.0 / static_cast<double>(kGovernorWindowS)) / (1024.0 * 1024.0);
            const int before = m_level;
            if (m_cap > 0.0 && m_rate > m_cap)
            {
                if (m_level < kGovernorLevels - 1)
                    ++m_level;
                m_calm = 0;
            }
            else if (m_level > 0 && (m_cap <= 0.0 || m_rate < m_cap * 0.5))
            {
                if (++m_calm >= kGovernorRecoverS)
                {
                    --m_level;
                    m_calm = 0;
                }
            }
            else
                m_calm = 0;
            if (m_level != before)
                ++m_changes;
            return m_level != before;
        }

    private:
        double m_cap = 8.0;
        int m_level = 0;
        std::uint64_t m_changes = 0;
        std::uint32_t m_calm = 0;
        std::uint64_t m_lastBucketUs = 0;
        std::uint32_t m_bucket = 0;
        std::uint64_t m_buckets[kGovernorWindowS] = {};
        std::uint64_t m_secondsAtLevel[kGovernorLevels] = {};
        double m_rate = 0.0;
    };

    // ------------------------------------------------------------ recorder

    struct OwnerInfo
    {
        bool found = false;
        std::uint32_t id = census::kNoOwner;
        census::OwnerClass cls = census::OwnerClass::World;
        // The audio entity from the owner (or a static one of the image for a sound
        // without owner), resolved at the drain.
        bool audioKnown = false;
        census::AudioBase audioBase = census::AudioBase::Image;
        std::uint16_t audioField = 0;
        std::int32_t audioOffset = 0;
        std::uint32_t vtableRva = 0;
    };

    struct Stats
    {
        // Life events.
        std::uint64_t enqueues = 0;
        std::uint64_t createsSeen = 0;     // creator calls while recording (refused and skipped included)
        std::uint64_t createsRefused = 0;
        std::uint64_t createsSpeech = 0;
        std::uint64_t createsMine = 0;
        std::uint64_t createsBudget = 0;   // dropped by the budget's last level
        std::uint64_t createsRecorded = 0;
        std::uint64_t createsFromQueue = 0;    // matched an S03 request
        std::uint64_t createsFromDeferred = 0; // matched an S04 request
        std::uint64_t createsDirect = 0;
        std::uint64_t createsNoStableId = 0;
        std::uint64_t createsByHash = 0;
        std::uint64_t ownerEntity = 0;
        std::uint64_t ownerPosition = 0;
        std::uint64_t ownerNone = 0;
        std::uint64_t audioResolved = 0; // creates whose audio entity base the packet carries
        std::uint64_t delayMsTotal = 0;
        std::uint64_t delayMsMax = 0;
        std::uint64_t pendingExpired = 0;
        std::uint64_t pendingOverflow = 0;
        std::uint64_t pendingUnmatchedRefusals = 0;
        // Adjust calls.
        std::uint64_t adjustCalls = 0;
        std::uint64_t adjustUnknown = 0;    // a sound without a create packet (made before the recording)
        std::uint64_t adjustUnchanged = 0;
        std::uint64_t adjustThrottled = 0;  // changed, waited for the interval (trailing sample)
        std::uint64_t adjustFlushedAtEnd = 0; // a waiting value recorded at its sound's end
        std::uint64_t adjustBudget = 0;     // dropped by the budget
        std::uint64_t adjustRecorded = 0;
        std::uint64_t adjustTrailing = 0;   // recorded at a tick, not at the call
        std::uint64_t adjustVariableOverflow = 0; // more hashes on one sound than the sampler keeps
        std::uint64_t adjustCallsByKind[kAdjustKinds] = {};
        std::uint64_t adjustRecordedByKind[kAdjustKinds] = {};
        // Ends.
        std::uint64_t endsStop = 0;
        std::uint64_t endsStopAndForget = 0;
        std::uint64_t endsRelease = 0;
        std::uint64_t endsReleaseAfterStop = 0;
        std::uint64_t endsUnknown = 0;
        std::uint64_t endsBudget = 0;
        // Live table.
        std::uint64_t livePeak = 0;
        std::uint64_t liveAtStop = 0;
        // Rings.
        std::uint64_t lifePushed = 0;
        std::uint64_t adjustPushed = 0;
        std::uint64_t lifeDropped = 0;
        std::uint64_t adjustDropped = 0;
        std::uint64_t discardedPaused = 0;
        // Drain cost (script thread).
        std::uint64_t drainFrames = 0;
        double drainUsTotal = 0.0;
        double drainUsMax = 0.0;
        std::uint64_t drainEventsMax = 0;
        // Budget.
        std::uint64_t bytes = 0;
        double rateMBPerMin = 0.0;
        int governorLevel = 0;
        std::uint64_t governorChanges = 0;
        std::uint64_t secondsAtLevel[kGovernorLevels] = {};
        std::uint32_t updateHz = 0;
        double maxMBPerMin = 0.0;
        std::uint32_t recordedMs = 0;
    };

    class Recorder
    {
    public:
        void Begin(const Config& config, Track* track, std::uint64_t nowUs)
        {
            m_config = config;
            m_track = track;
            m_stats = Stats{};
            m_stats.updateHz = config.updateHz;
            m_stats.maxMBPerMin = config.maxMBPerMin;
            m_pending.clear();
            m_live.clear();
            m_lastCreateMs = m_lastAdjustMs = m_lastEndMs = 0;
            m_governor.Begin(config.maxMBPerMin, nowUs);
        }

        bool Active() const { return m_track != nullptr; }
        const Stats& StatsNow() const { return m_stats; }
        Stats& StatsMutable() { return m_stats; }
        std::size_t Live() const { return m_live.size(); }
        std::size_t Pending() const { return m_pending.size(); }
        const Governor& Budget() const { return m_governor; }

        // A life event with its clip time; `owner` is read for a create only.
        void Life(const LifeEvent& event, std::uint32_t clipMs, const OwnerInfo& owner)
        {
            if (!m_track)
                return;
            switch (event.kind)
            {
            case LifeKind::Enqueue: Enqueue(event, clipMs); break;
            case LifeKind::Create: Create(event, clipMs, owner); break;
            case LifeKind::Stop: End(event, clipMs, EndReason::Stop); break;
            case LifeKind::StopAndForget: End(event, clipMs, EndReason::StopAndForget); break;
            case LifeKind::Release: End(event, clipMs, EndReason::Release); break;
            }
        }

        void AdjustCall(const AdjustEvent& event, std::uint32_t clipMs)
        {
            if (!m_track)
                return;
            const std::uint8_t kindIndex = static_cast<std::uint8_t>(event.kind);
            if (kindIndex >= kAdjustKinds)
                return;
            ++m_stats.adjustCalls;
            ++m_stats.adjustCallsByKind[kindIndex];
            const auto found = m_live.find(event.sound);
            if (found == m_live.end())
            {
                ++m_stats.adjustUnknown;
                return;
            }
            if (m_governor.AdjustsOff())
            {
                ++m_stats.adjustBudget;
                return;
            }
            LiveSound& live = found->second;
            Sampler* sampler = SamplerOf(live, event.kind, event.hash);
            if (!sampler)
            {
                // Beyond the hashes the sampler keeps: recorded as it comes.
                ++m_stats.adjustVariableOverflow;
                Emit(live, event.kind, event.hash, event.value, clipMs, false);
                return;
            }
            if (sampler->emitted && SameValue(sampler->value, event.value))
            {
                sampler->dirty = false;
                ++m_stats.adjustUnchanged;
                return;
            }
            const std::uint64_t intervalUs = IntervalUs();
            // The values set in the create's own frame are the sound's start values,
            // never throttled. The creator applies the params' defaults (volume 0 dB) and
            // the owner sets the real one microseconds later (e.g. a wagon's loop helper
            // 0x30FEB40: create, play, pitch, volume); throttled, that value would wait
            // 50 ms and the replay would play the start at 0 dB.
            const bool createFrame = event.timeUs >= live.createUs && event.timeUs - live.createUs <= kCreateFrameUs;
            if (sampler->emitted && event.timeUs < sampler->lastUs + intervalUs && !createFrame)
            {
                // Within the interval: the value waits for the next tick.
                std::memcpy(sampler->pending, event.value, sizeof(sampler->pending));
                sampler->dirty = true;
                live.dirty = true;
                ++m_stats.adjustThrottled;
                return;
            }
            sampler->lastUs = event.timeUs;
            sampler->emitted = true;
            sampler->dirty = false;
            std::memcpy(sampler->value, event.value, sizeof(sampler->value));
            Emit(live, event.kind, event.hash, event.value, clipMs, false);
        }

        // Every recording tick: trailing samples due, expired requests, the budget.
        void Tick(std::uint64_t nowUs, std::uint32_t clipMs)
        {
            if (!m_track)
                return;
            const std::uint64_t intervalUs = IntervalUs();
            if (!m_governor.AdjustsOff())
            {
                for (auto& entry : m_live)
                {
                    LiveSound& live = entry.second;
                    if (!live.dirty)
                        continue;
                    bool still = false;
                    for (Sampler& sampler : live.samplers)
                    {
                        if (!sampler.dirty)
                            continue;
                        if (nowUs < sampler.lastUs + intervalUs)
                        {
                            still = true;
                            continue;
                        }
                        sampler.lastUs = nowUs;
                        sampler.dirty = false;
                        std::memcpy(sampler.value, sampler.pending, sizeof(sampler.value));
                        Emit(live, sampler.kind, sampler.hash, sampler.value, clipMs, true);
                    }
                    live.dirty = still;
                }
            }
            // Requests the creator never answered (5 s: the queue's longest wait was 5.8 s, a rare tail).
            for (std::size_t n = 0; n < m_pending.size();)
            {
                if (nowUs >= m_pending[n].timeUs + kPendingExpireUs)
                {
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
                    ++m_stats.pendingExpired;
                }
                else
                    ++n;
            }
            m_governor.Tick(nowUs);
            m_stats.governorLevel = m_governor.Level();
            m_stats.governorChanges = m_governor.Changes();
            m_stats.rateMBPerMin = m_governor.RateMBPerMin();
            for (int level = 0; level < kGovernorLevels; ++level)
                m_stats.secondsAtLevel[level] = m_governor.SecondsAtLevel(level);
            m_stats.recordedMs = clipMs;
        }

        // The recording stops: what was still alive.
        void End()
        {
            m_stats.liveAtStop = m_live.size();
            m_track = nullptr;
        }

        static constexpr std::uint64_t kPendingExpireUs = 5000000;
        static constexpr std::uint64_t kCreateFrameUs = 4000; // the create's own frame
        static constexpr std::size_t kPendingMax = 4096;
        static constexpr std::size_t kSamplersPerSound = kAdjustKinds - 1 + 8; // the plain kinds and 8 variable hashes

    private:
        struct PendingRequest
        {
            std::uint64_t audio = 0;
            std::uint64_t ref = 0;
            std::uint64_t timeUs = 0;
            std::uint32_t clipMs = 0;
            Source source = Source::Queue;
            std::uint32_t callers[kCallerFrames] = {};
        };

        struct Sampler
        {
            Adjust kind = Adjust::Volume;
            std::uint32_t hash = 0;
            bool used = false;
            bool emitted = false;
            bool dirty = false;
            std::uint64_t lastUs = 0;
            float value[3] = {};
            float pending[3] = {};
        };

        struct LiveSound
        {
            std::uint32_t index = 0;
            std::uint32_t caller = 0;
            bool stopped = false;
            bool dirty = false;
            std::uint64_t createUs = 0;
            Sampler samplers[kSamplersPerSound];
        };

        std::uint64_t IntervalUs() const
        {
            const std::uint32_t hz = EffectiveHz(m_config.updateHz, m_governor.Level());
            return hz ? 1000000ull / hz : 1000000ull;
        }

        static bool SameValue(const float a[3], const float b[3])
        {
            return std::memcmp(a, b, sizeof(float) * 3) == 0;
        }

        Sampler* SamplerOf(LiveSound& live, Adjust kind, std::uint32_t hash)
        {
            const std::uint8_t kindIndex = static_cast<std::uint8_t>(kind);
            if (kind != Adjust::Variable)
            {
                Sampler& sampler = live.samplers[kindIndex];
                if (!sampler.used)
                {
                    sampler.used = true;
                    sampler.kind = kind;
                }
                return &sampler;
            }
            Sampler* free = nullptr;
            for (std::size_t n = kAdjustKinds - 1; n < kSamplersPerSound; ++n)
            {
                Sampler& sampler = live.samplers[n];
                if (sampler.used && sampler.hash == hash)
                    return &sampler;
                if (!sampler.used && !free)
                    free = &sampler;
            }
            if (!free)
                return nullptr;
            free->used = true;
            free->kind = Adjust::Variable;
            free->hash = hash;
            return free;
        }

        void Emit(LiveSound& live, Adjust kind, std::uint32_t hash, const float value[3], std::uint32_t clipMs, bool trailing)
        {
            AdjustPacket packet;
            packet.timeMs = clipMs < m_lastAdjustMs ? m_lastAdjustMs : clipMs;
            packet.index = live.index;
            packet.kind = kind;
            packet.hash = kind == Adjust::Variable ? hash : 0;
            std::memcpy(packet.value, value, sizeof(packet.value));
            if (!IsVector(kind))
                packet.value[1] = packet.value[2] = 0.0f;
            m_lastAdjustMs = packet.timeMs;
            m_track->adjusts.push_back(packet);
            const std::size_t bytes = AdjustPacketBytes(kind);
            m_stats.bytes += bytes;
            m_governor.Note(bytes);
            ++m_stats.adjustRecorded;
            ++m_stats.adjustRecordedByKind[static_cast<std::uint8_t>(kind)];
            if (trailing)
                ++m_stats.adjustTrailing;
        }

        void Enqueue(const LifeEvent& event, std::uint32_t clipMs)
        {
            ++m_stats.enqueues;
            if (m_pending.size() >= kPendingMax)
            {
                m_pending.erase(m_pending.begin());
                ++m_stats.pendingOverflow;
            }
            PendingRequest request;
            request.audio = event.audio;
            request.ref = event.ref;
            request.timeUs = event.timeUs;
            request.clipMs = clipMs;
            request.source = event.source;
            std::memcpy(request.callers, event.callers, sizeof(request.callers));
            m_pending.push_back(request);
        }

        // The oldest request of this entity and ref from `source`.
        bool TakePending(std::uint64_t audio, std::uint64_t ref, Source source, PendingRequest& out)
        {
            for (std::size_t n = 0; n < m_pending.size(); ++n)
            {
                const PendingRequest& request = m_pending[n];
                if (request.audio != audio || request.ref != ref || request.source != source)
                    continue;
                out = request;
                m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(n));
                return true;
            }
            return false;
        }

        static bool ThroughQueueDrain(const std::uint32_t* frames, std::size_t count)
        {
            for (std::size_t n = 0; n < count; ++n)
                if (frames[n] >= 0x258A348u && frames[n] < 0x258A546u)
                    return true;
            return false;
        }

        void Create(const LifeEvent& event, std::uint32_t clipMs, const OwnerInfo& owner)
        {
            ++m_stats.createsSeen;
            // The request this create answers (a refused one takes its request away too).
            PendingRequest request;
            bool matched = false;
            Source source = Source::Direct;
            if (event.core == 0 && ThroughQueueDrain(event.callers, kCallerFrames))
            {
                matched = TakePending(event.audio, event.ref, Source::Queue, request);
                source = Source::Queue;
            }
            else if (TakePending(event.audio, event.ref, Source::Deferred, request))
            {
                matched = true;
                source = Source::Deferred;
            }
            if (event.refused)
            {
                ++m_stats.createsRefused;
                if (source == Source::Queue && !matched)
                    ++m_stats.pendingUnmatchedRefusals;
                return;
            }
            if (event.speech)
            {
                ++m_stats.createsSpeech;
                return;
            }
            if (event.mine)
            {
                ++m_stats.createsMine;
                return;
            }
            if (m_governor.AllOff())
            {
                ++m_stats.createsBudget;
                return;
            }
            if (m_track->nextIndex >= kMaxNextIndex)
            {
                ++m_stats.createsBudget; // a session past kMaxNextIndex sounds: no more numbers
                return;
            }
            CreatePacket packet;
            packet.index = m_track->nextIndex++;
            if (matched)
            {
                packet.timeMs = request.clipMs;
                const std::uint64_t delayMs = event.timeUs > request.timeUs ? (event.timeUs - request.timeUs) / 1000 : 0;
                packet.createDelayMs = static_cast<std::uint16_t>(delayMs > 0xFFFF ? 0xFFFF : delayMs);
                packet.source = source;
                packet.caller = FamilyCaller(request.callers, kCallerFrames);
                for (std::size_t n = 0; n < 3; ++n)
                    packet.path[n] = request.callers[n];
                m_stats.delayMsTotal += delayMs;
                m_stats.delayMsMax = (std::max)(m_stats.delayMsMax, delayMs);
                if (source == Source::Queue)
                    ++m_stats.createsFromQueue;
                else
                    ++m_stats.createsFromDeferred;
            }
            else
            {
                packet.timeMs = clipMs;
                packet.source = Source::Direct;
                // The census frames: 0 is the census detour, 1 the creator's
                // entry; the family is above them.
                packet.caller = FamilyCaller(event.callers, kCallerFrames);
                for (std::size_t n = 0; n < 3; ++n)
                    packet.path[n] = event.callers[n];
                ++m_stats.createsDirect;
            }
            if (packet.timeMs < m_lastCreateMs)
                packet.timeMs = m_lastCreateMs;
            m_lastCreateMs = packet.timeMs;
            const StableId id = StableIdOf(event.ref, event.core != 0);
            packet.stableId = id.id;
            packet.idKind = id.kind;
            packet.rawRef = event.ref;
            packet.core = event.core;
            packet.hasParams = event.hasParams;
            packet.params = event.params;
            packet.hasPosition = event.hasParams &&
                (event.params.position[0] != 0.0f || event.params.position[1] != 0.0f || event.params.position[2] != 0.0f);
            if (id.kind == IdKind::None)
                ++m_stats.createsNoStableId;
            else if (id.kind == IdKind::Hash)
                ++m_stats.createsByHash;
            if (owner.found)
            {
                packet.owner = owner.id;
                packet.ownerClass = owner.cls;
                ++m_stats.ownerEntity;
            }
            else if (packet.hasPosition)
                ++m_stats.ownerPosition;
            else
                ++m_stats.ownerNone;
            packet.audioKnown = owner.audioKnown;
            packet.audioBase = owner.audioBase;
            packet.audioField = owner.audioField;
            packet.audioOffset = owner.audioOffset;
            packet.vtableRva = owner.vtableRva;
            if (owner.audioKnown)
                ++m_stats.audioResolved;
            m_track->creates.push_back(packet);
            m_stats.bytes += CreatePacketBytes(packet); // SGEN v3
            m_governor.Note(CreatePacketBytes(packet));
            ++m_stats.createsRecorded;
            LiveSound live;
            live.index = packet.index;
            live.caller = packet.caller;
            live.createUs = event.timeUs; // the create frame's values are not throttled
            m_live[event.sound] = live;
            m_stats.livePeak = (std::max)(m_stats.livePeak, static_cast<std::uint64_t>(m_live.size()));
        }

        void End(const LifeEvent& event, std::uint32_t clipMs, EndReason reason)
        {
            const auto found = m_live.find(event.sound);
            if (found == m_live.end())
            {
                ++m_stats.endsUnknown;
                return;
            }
            LiveSound& live = found->second;
            // A value still waiting for its interval was heard until this end: recorded
            // now, before the end (it would be lost when the release comes before the
            // next tick).
            if (live.dirty && !m_governor.AdjustsOff())
            {
                for (Sampler& sampler : live.samplers)
                {
                    if (!sampler.dirty)
                        continue;
                    sampler.lastUs = event.timeUs;
                    sampler.dirty = false;
                    std::memcpy(sampler.value, sampler.pending, sizeof(sampler.value));
                    Emit(live, sampler.kind, sampler.hash, sampler.value, clipMs, true);
                    ++m_stats.adjustFlushedAtEnd;
                }
                live.dirty = false;
            }
            if (reason != EndReason::Release)
            {
                if (live.stopped)
                    return; // one stop packet per sound (the first of S13 and S14)
                live.stopped = true;
                if (reason == EndReason::Stop)
                    ++m_stats.endsStop;
                else
                    ++m_stats.endsStopAndForget;
            }
            else
            {
                ++m_stats.endsRelease;
                if (live.stopped)
                    ++m_stats.endsReleaseAfterStop;
            }
            if (m_governor.AllOff())
                ++m_stats.endsBudget;
            else
            {
                EndPacket packet;
                packet.timeMs = clipMs < m_lastEndMs ? m_lastEndMs : clipMs;
                packet.index = live.index;
                packet.reason = reason;
                m_lastEndMs = packet.timeMs;
                m_track->ends.push_back(packet);
                m_stats.bytes += kEndPacketBytes;
                m_governor.Note(kEndPacketBytes);
            }
            if (reason == EndReason::Release)
                m_live.erase(found);
        }

        Config m_config;
        Track* m_track = nullptr;
        Stats m_stats;
        std::vector<PendingRequest> m_pending;
        std::unordered_map<std::uint64_t, LiveSound> m_live;
        std::uint32_t m_lastCreateMs = 0;
        std::uint32_t m_lastAdjustMs = 0;
        std::uint32_t m_lastEndMs = 0;
        Governor m_governor;
    };

    // ------------------------------------------------------------ trim and cut

    // The buffer's ring (replay_buffer.h): packets of sounds whose release
    // is before keepFrom go, with the sound's create; a sound still alive
    // keeps its create and its adjusts (its state), as the entity sounds
    // do. A create older than keepFrom by more than kUnendedGraceMs whose
    // end was never seen goes too. Returns the packets dropped.
    constexpr std::uint32_t kUnendedGraceMs = 120000;

    inline std::size_t TrimBefore(Track& track, std::uint32_t keepFrom)
    {
        std::vector<std::uint8_t> ended(track.nextIndex, std::uint8_t{0});
        for (const EndPacket& end : track.ends)
            if (end.reason == EndReason::Release && end.timeMs < keepFrom && end.index < ended.size())
                ended[end.index] = 1;
        std::vector<std::uint8_t> drop(track.nextIndex, std::uint8_t{0});
        for (const CreatePacket& create : track.creates)
        {
            if (create.index >= drop.size())
                continue;
            const bool stale = keepFrom > kUnendedGraceMs && create.timeMs < keepFrom - kUnendedGraceMs;
            if (ended[create.index] || stale)
                drop[create.index] = 1;
        }
        const std::size_t before = track.Packets();
        const auto dropped = [&](std::uint32_t index) { return index >= drop.size() || drop[index]; };
        track.creates.erase(std::remove_if(track.creates.begin(), track.creates.end(),
            [&](const CreatePacket& p) { return dropped(p.index); }), track.creates.end());
        track.adjusts.erase(std::remove_if(track.adjusts.begin(), track.adjusts.end(),
            [&](const AdjustPacket& p) { return dropped(p.index); }), track.adjusts.end());
        track.ends.erase(std::remove_if(track.ends.begin(), track.ends.end(),
            [&](const EndPacket& p) { return dropped(p.index); }), track.ends.end());
        return before - track.Packets();
    }

    // A window [base, end] of the recording (recording_window.cpp): creates in the
    // window shifted; a sound created before base and not released by base is
    // kept at 0 (preexisting) with the latest adjust of each setting before base
    // at 0, so the player can start it in its state; ends in the window shifted;
    // everything after end dropped. Indices are kept (unique within the recording).
    inline void CutInto(const Track& in, Track& out, std::uint32_t base, std::uint32_t end)
    {
        out.Clear();
        out.nextIndex = in.nextIndex;
        std::vector<std::uint8_t> releasedBefore(in.nextIndex, std::uint8_t{0});
        for (const EndPacket& e : in.ends)
            if (e.reason == EndReason::Release && e.timeMs < base && e.index < releasedBefore.size())
                releasedBefore[e.index] = 1;
        std::vector<std::uint8_t> kept(in.nextIndex, std::uint8_t{0});
        for (const CreatePacket& c : in.creates)
        {
            if (c.index >= kept.size() || c.timeMs > end)
                continue;
            if (c.timeMs < base && releasedBefore[c.index])
                continue;
            CreatePacket copy = c;
            if (c.timeMs < base)
            {
                copy.timeMs = 0;
                copy.preexisting = true;
            }
            else
                copy.timeMs = c.timeMs - base;
            kept[c.index] = 1;
            out.creates.push_back(copy);
        }
        // The latest adjust per (index, kind, hash) before base, in input order.
        std::map<std::uint64_t, AdjustPacket> latest;
        for (const AdjustPacket& a : in.adjusts)
        {
            if (a.index >= kept.size() || !kept[a.index] || a.timeMs > end)
                continue;
            if (a.timeMs < base)
            {
                const std::uint64_t key = (static_cast<std::uint64_t>(a.index) << 32) |
                    (static_cast<std::uint64_t>(static_cast<std::uint8_t>(a.kind)) << 24) | (a.hash & 0xFFFFFFu);
                AdjustPacket copy = a;
                copy.timeMs = 0;
                latest[key] = copy;
            }
        }
        for (const auto& entry : latest)
            out.adjusts.push_back(entry.second);
        for (const AdjustPacket& a : in.adjusts)
        {
            if (a.index >= kept.size() || !kept[a.index] || a.timeMs < base || a.timeMs > end)
                continue;
            AdjustPacket copy = a;
            copy.timeMs = a.timeMs - base;
            out.adjusts.push_back(copy);
        }
        for (const EndPacket& e : in.ends)
        {
            if (e.index >= kept.size() || !kept[e.index] || e.timeMs < base || e.timeMs > end)
                continue;
            EndPacket copy = e;
            copy.timeMs = e.timeMs - base;
            out.ends.push_back(copy);
        }
    }

    // ------------------------------------------------------------ report

    struct ReportHeader
    {
        std::string clip;
        std::string version;
        std::string written; // local time
        std::uint32_t durationMs = 0;
        std::uint64_t censusSounds = 0;  // the clip's sound census count (0 when the census was off)
        std::uint64_t censusRefused = 0;
    };

    namespace detail
    {
        inline std::string Num(double value)
        {
            char text[40];
            std::snprintf(text, sizeof(text), "%.6g", value);
            return text;
        }
        inline std::string Hex(std::uint64_t value)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
            return text;
        }
        inline std::string Str(std::uint64_t value) { return std::to_string(value); }

        struct Family
        {
            std::uint64_t creates = 0;
            std::uint64_t adjusts = 0;
            std::uint64_t ends = 0;
            std::uint64_t bytes = 0;
            std::uint64_t noStableId = 0;
            std::uint64_t unowned = 0;
            std::uint64_t classes[census::kOwnerClassCount] = {};
        };
    }

    // One record per line, TAG,key=value,... (tools/sound_generic_report.py reads it).
    inline std::string FormatReport(const Track& track, const Stats& stats, const ReportHeader& header)
    {
        using namespace detail;
        std::string out;
        out.reserve(1 << 14);
        const double seconds = static_cast<double>(stats.recordedMs ? stats.recordedMs : header.durationMs) / 1000.0;
        const std::uint64_t fileBytes = track.FileBytes();
        out += "SOUND_GENERIC_REPORT,format=1,version=" + header.version + ",clip=" + header.clip + ",written=" +
            header.written + ",duration_ms=" + Str(header.durationMs) + ",recorded_ms=" + Str(stats.recordedMs) +
            ",update_hz=" + Str(stats.updateHz) + ",max_mb_per_min=" + Num(stats.maxMBPerMin) + '\n';
        out += "# Generic sound effect packets of this clip: what the recorder kept, what it\n";
        out += "# skipped and what it cost. tools/sound_generic_report.py reads it.\n";
        out += "PACKETS,create=" + Str(track.creates.size()) + ",adjust=" + Str(track.adjusts.size()) + ",end=" +
            Str(track.ends.size()) + ",bytes=" + Str(fileBytes) + ",bytes_per_min=" +
            Num(seconds > 0.0 ? static_cast<double>(fileBytes) * 60.0 / seconds : 0.0) + ",mb_per_min=" +
            Num(seconds > 0.0 ? static_cast<double>(fileBytes) * 60.0 / seconds / (1024.0 * 1024.0) : 0.0) + '\n';
        out += "CREATES,seen=" + Str(stats.createsSeen) + ",recorded=" + Str(stats.createsRecorded) + ",refused=" +
            Str(stats.createsRefused) + ",speech=" + Str(stats.createsSpeech) + ",mine=" + Str(stats.createsMine) +
            ",budget=" + Str(stats.createsBudget) + ",from_queue=" + Str(stats.createsFromQueue) + ",from_deferred=" +
            Str(stats.createsFromDeferred) + ",direct=" + Str(stats.createsDirect) + ",enqueues=" + Str(stats.enqueues) +
            ",pending_expired=" + Str(stats.pendingExpired) + ",pending_overflow=" + Str(stats.pendingOverflow) +
            ",unmatched_refusals=" + Str(stats.pendingUnmatchedRefusals) + ",delay_ms_mean=" +
            Num(stats.createsFromQueue + stats.createsFromDeferred
                    ? static_cast<double>(stats.delayMsTotal) / static_cast<double>(stats.createsFromQueue + stats.createsFromDeferred)
                    : 0.0) +
            ",delay_ms_max=" + Str(stats.delayMsMax) + '\n';
        out += "CENSUS,sounds=" + Str(header.censusSounds) + ",refused=" + Str(header.censusRefused) + '\n';
        out += "ID,stable=" + Str(stats.createsRecorded - stats.createsNoStableId - stats.createsByHash) + ",hash=" +
            Str(stats.createsByHash) + ",none=" + Str(stats.createsNoStableId) + '\n';
        out += "OWNER,entity=" + Str(stats.ownerEntity) + ",position=" + Str(stats.ownerPosition) + ",none=" +
            Str(stats.ownerNone) + ",audio_resolved=" + Str(stats.audioResolved) + '\n';
        out += "ADJUSTS,calls=" + Str(stats.adjustCalls) + ",recorded=" + Str(stats.adjustRecorded) + ",unchanged=" +
            Str(stats.adjustUnchanged) + ",throttled=" + Str(stats.adjustThrottled) + ",trailing=" +
            Str(stats.adjustTrailing) + ",flushed_at_end=" + Str(stats.adjustFlushedAtEnd) + ",unknown_sound=" + Str(stats.adjustUnknown) + ",budget=" +
            Str(stats.adjustBudget) + ",variable_overflow=" + Str(stats.adjustVariableOverflow) + ",calls_per_s=" +
            Num(seconds > 0.0 ? static_cast<double>(stats.adjustCalls) / seconds : 0.0) + ",recorded_per_s=" +
            Num(seconds > 0.0 ? static_cast<double>(stats.adjustRecorded) / seconds : 0.0) + '\n';
        for (std::uint8_t kind = 0; kind < kAdjustKinds; ++kind)
        {
            out += std::string("ADJUST_KIND,kind=") + AdjustName(static_cast<Adjust>(kind)) + ",port=" +
                AdjustPort(static_cast<Adjust>(kind)) + ",calls=" + Str(stats.adjustCallsByKind[kind]) + ",recorded=" +
                Str(stats.adjustRecordedByKind[kind]) + '\n';
        }
        out += "ENDS,release=" + Str(stats.endsRelease) + ",stop=" + Str(stats.endsStop) + ",stop_and_forget=" +
            Str(stats.endsStopAndForget) + ",release_after_stop=" + Str(stats.endsReleaseAfterStop) + ",unknown=" +
            Str(stats.endsUnknown) + ",budget=" + Str(stats.endsBudget) + ",live_peak=" + Str(stats.livePeak) +
            ",live_at_stop=" + Str(stats.liveAtStop) + '\n';
        out += "RINGS,life_pushed=" + Str(stats.lifePushed) + ",adjust_pushed=" + Str(stats.adjustPushed) +
            ",life_dropped=" + Str(stats.lifeDropped) + ",adjust_dropped=" + Str(stats.adjustDropped) +
            ",discarded_paused=" + Str(stats.discardedPaused) + '\n';
        out += "COST,drain_frames=" + Str(stats.drainFrames) + ",drain_us_mean=" +
            Num(stats.drainFrames ? stats.drainUsTotal / static_cast<double>(stats.drainFrames) : 0.0) +
            ",drain_us_max=" + Num(stats.drainUsMax) + ",drain_events_max=" + Str(stats.drainEventsMax) +
            ",detour_pushes_per_s=" +
            Num(seconds > 0.0 ? static_cast<double>(stats.lifePushed + stats.adjustPushed) / seconds : 0.0) + '\n';
        out += "BUDGET,level=" + Str(static_cast<std::uint64_t>(stats.governorLevel)) + ",changes=" +
            Str(stats.governorChanges) + ",rate_mb_per_min=" + Num(stats.rateMBPerMin) + ",seconds_at_level=";
        for (int level = 0; level < kGovernorLevels; ++level)
            out += (level ? ";" : "") + Str(stats.secondsAtLevel[level]);
        out += '\n';
        // Families: the create's caller, with its adjusts and ends.
        std::vector<std::uint32_t> callerOf(track.nextIndex, std::uint8_t{0});
        std::map<std::uint32_t, Family> families;
        for (const CreatePacket& c : track.creates)
        {
            if (c.index < callerOf.size())
                callerOf[c.index] = c.caller;
            Family& family = families[c.caller];
            ++family.creates;
            family.bytes += CreatePacketBytes(c);
            if (c.idKind == IdKind::None)
                ++family.noStableId;
            if (c.owner == census::kNoOwner)
                ++family.unowned;
            if (static_cast<std::uint8_t>(c.ownerClass) < census::kOwnerClassCount)
                ++family.classes[static_cast<std::uint8_t>(c.ownerClass)];
        }
        for (const AdjustPacket& a : track.adjusts)
        {
            const std::uint32_t caller = a.index < callerOf.size() ? callerOf[a.index] : 0;
            Family& family = families[caller];
            ++family.adjusts;
            family.bytes += AdjustPacketBytes(a.kind);
        }
        for (const EndPacket& e : track.ends)
        {
            const std::uint32_t caller = e.index < callerOf.size() ? callerOf[e.index] : 0;
            Family& family = families[caller];
            ++family.ends;
            family.bytes += kEndPacketBytes;
        }
        std::vector<std::pair<std::uint32_t, Family>> rows(families.begin(), families.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.bytes != b.second.bytes ? a.second.bytes > b.second.bytes : a.first < b.first;
        });
        out += "FAMILIES,count=" + Str(rows.size()) + '\n';
        for (const auto& row : rows)
        {
            const Family& f = row.second;
            out += "FAMILY,caller=" + Hex(row.first) + ",creates=" + Str(f.creates) + ",adjusts=" + Str(f.adjusts) +
                ",ends=" + Str(f.ends) + ",bytes=" + Str(f.bytes) + ",no_stable_id=" + Str(f.noStableId) + ",unowned=" +
                Str(f.unowned) + ",classes=";
            bool first = true;
            for (std::uint8_t cls = 0; cls < census::kOwnerClassCount; ++cls)
            {
                if (!f.classes[cls])
                    continue;
                out += (first ? "" : ";") + std::string(census::ClassToken(static_cast<census::OwnerClass>(cls))) + ":" +
                    Str(f.classes[cls]);
                first = false;
            }
            out += '\n';
        }
        out += "END\n";
        return out;
    }

    // The SOUND_GENERIC log line (every 60 s while recording, and at the stop).
    inline std::string FormatLogLine(const Stats& stats, const char* phase, std::size_t live, std::size_t pending)
    {
        using namespace detail;
        return std::string("SOUND_GENERIC,phase=") + phase + ",recorded_ms=" + Str(stats.recordedMs) + ",creates=" +
            Str(stats.createsRecorded) + ",adjusts=" + Str(stats.adjustRecorded) + ",adjust_calls=" + Str(stats.adjustCalls) +
            ",ends=" + Str(stats.endsRelease + stats.endsStop + stats.endsStopAndForget) + ",bytes=" + Str(stats.bytes) +
            ",rate_mb_per_min=" + Num(stats.rateMBPerMin) + ",level=" + Str(static_cast<std::uint64_t>(stats.governorLevel)) +
            ",live=" + Str(live) + ",pending=" + Str(pending) + ",no_stable_id=" + Str(stats.createsNoStableId) +
            ",unowned=" + Str(stats.ownerNone) + ",adjust_unknown=" + Str(stats.adjustUnknown) + ",ring_dropped=" +
            Str(stats.lifeDropped + stats.adjustDropped) + ",drain_us_mean=" +
            Num(stats.drainFrames ? stats.drainUsTotal / static_cast<double>(stats.drainFrames) : 0.0) + ",drain_us_max=" +
            Num(stats.drainUsMax);
    }

    // When a SOUND_CLOCK sample is due. Clip time and the QPC clock go together
    // between samples (a reader interpolates linearly), so a sample is taken every
    // `everyMs` of QPC time and around every discontinuity (pause, scrub, slow
    // motion, a record clock pause): the tick before it and the tick of it. Pure.
    struct ClockSample
    {
        std::uint32_t clipMs = 0;
        std::uint64_t qpcUs = 0;
        bool normal = true;
    };

    class ClockSampler
    {
    public:
        explicit ClockSampler(std::uint32_t everyMs = 100, std::uint32_t jumpMs = 25)
            : m_everyUs(static_cast<std::uint64_t>(everyMs) * 1000u), m_jumpMs(jumpMs) {}

        void Reset() { m_have = false; m_prevLogged = false; m_count = 0; }

        // The samples to log for this tick (0, 1 or 2: the previous tick
        // first when this one breaks the line).
        std::size_t Tick(std::uint32_t clipMs, std::uint64_t qpcUs, bool normal, ClockSample out[2])
        {
            const ClockSample now{clipMs, qpcUs, normal};
            std::size_t n = 0;
            if (!m_have)
            {
                out[n++] = now;
                m_lastLoggedUs = qpcUs;
                m_prevLogged = true;
            }
            else
            {
                const double clipDelta = static_cast<double>(clipMs) - static_cast<double>(m_prev.clipMs);
                const double qpcDelta = (static_cast<double>(qpcUs) - static_cast<double>(m_prev.qpcUs)) / 1000.0;
                const bool jump = clipDelta - qpcDelta > m_jumpMs || qpcDelta - clipDelta > m_jumpMs ||
                    normal != m_prev.normal;
                if (jump)
                {
                    if (!m_prevLogged)
                        out[n++] = m_prev;
                    out[n++] = now;
                    m_lastLoggedUs = qpcUs;
                    m_prevLogged = true;
                }
                else if (qpcUs >= m_lastLoggedUs + m_everyUs)
                {
                    out[n++] = now;
                    m_lastLoggedUs = qpcUs;
                    m_prevLogged = true;
                }
                else
                {
                    m_prevLogged = false;
                }
            }
            m_prev = now;
            m_have = true;
            m_count += n;
            return n;
        }

        std::uint64_t Count() const { return m_count; }

    private:
        std::uint64_t m_everyUs;
        std::uint32_t m_jumpMs;
        bool m_have = false;
        bool m_prevLogged = false;
        ClockSample m_prev;
        std::uint64_t m_lastLoggedUs = 0;
        std::uint64_t m_count = 0;
    };

    inline std::string FormatClockLine(const char* phase, const ClockSample& sample)
    {
        return std::string("SOUND_CLOCK,phase=") + phase + ",clip_ms=" + std::to_string(sample.clipMs) +
            ",qpc_us=" + std::to_string(sample.qpcUs) + ",normal=" + (sample.normal ? "1" : "0");
    }
}
