#pragma once

// The secondary steps of a ped's feet (the hoof that lifts or lands in a
// gallop), recorded as events and replayed on the puppet, as a replay editor
// records footstep sounds, applied to the second queue of the foot audio.
// Pure part (no game).
//
// Engine (build 1491.50):
//   - every update of the movement component C (row +0x88; 0x8EF554 runs the
//     per-foot loop 0x8EECE8) measures h, the height of the foot over its
//     ground plane (stored at C +0x164 + 0x90 * foot, 0x8EEEA8), and calls
//     0x8ED350(C, h, foot) at 0x8EEE91; there h is compared with
//     [C +0x4F0] + [C +0x550] * [C +0x5E8] (0x8ED3C9..0x8ED40C): a foot down
//     ([C +0x15C + 0x90 * foot] < 6) that rose above it lifted, a foot up that
//     came under it landed. The event type comes from 0x89B608 (by the gait
//     C +0x630: 3, 4, 5, 6 give 0..3 lifting and 6..9 landing; a standing
//     quadruped 4/10, the leg mask 5/11, gait 9 0/6, other gaits none) and
//     goes to 0x317A1D0(foot audio, type, foot, spread, direct) at 0x8ED457;
//   - 0x317A1D0 refuses a foot audio with +0x5A7 set, type -1 and a foot of 12
//     or more; for a quadruped with direct 0 it parks the event in a delayed
//     table (+0xD8, 16 entries of 12 bytes, the delays of the hoof slots
//     +0x3A0), which the movement update 0x3193D14 drains at its start by
//     calling 0x317A1D0 again with direct 1 (0x3193D96); otherwise it queues
//     {type, foot} at +0xB4 + 8 * n with the count at +0xD4 (at most 4; the
//     store at 0x317A31A is the only increment);
//   - the movement sounds 0x3193F20 (inside 0x3193D14, after the main steps)
//     play that queue (0x3194261..0x31942A2) through 0x318CB98, which plays
//     feet 0..3 only and goes to 0x318D470: the type picks a field of the
//     soundset of the foot's slot (foot audio +0x240 + 0x40 * foot, the same
//     container the main step uses) by the hash table 0x3E683C0
//     (0x3180E88), created at 0x318E6E8, return 0x318D509 (the callers[2] of
//     the POSE_V2_HORSE_STEP_AUDIO lines of a secondary step).
// A frozen puppet's ground plane goes stale (it lifts once and never lands
// again) and its gait stays 0 (C +0x5DC is 0), so its own generator gives the
// standing types. So a step is not recomputed from the puppet's physics:
// each step event is recorded and replayed in normal play.
//
// The mechanism: the recorder keeps every entry the engine put in a recorded
// ped's second queue (type, foot, the slot's container, the foot's rise), as
// it entered it (the drain of a quadruped included); the playback queues the
// same entry on the puppet at the same clip time with direct 1 (straight into
// the queue), after setting the recorded container on the foot's slot. While
// a clip with the section plays, the puppet's own events are refused.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace replay::footevent
{
    // ---- engine layout (build 1491.50) ----
    constexpr std::uintptr_t kEnqueueRva = 0x317A1D0;      // (foot audio, type, foot, spread, direct)
    constexpr std::uintptr_t kEnqueueStoreRva = 0x317A31A; // mov [r10+0D4h],eax; ...; mov [r10+rcx*8+0B4h],rax
    constexpr std::uintptr_t kGeneratorCallRva = 0x8ED457; // call 0x317A1D0 from the lift/land detector 0x8ED350
    constexpr std::uintptr_t kGeneratorReturnRva = 0x8ED45C;
    constexpr std::uintptr_t kDrainCallRva = 0x3193D96;    // call 0x317A1D0 from the movement update's drain
    constexpr std::uintptr_t kDrainReturnRva = 0x3193D9B;
    constexpr std::uintptr_t kQueueOffset = 0xB4;
    constexpr std::uintptr_t kQueueCountOffset = 0xD4;
    constexpr std::uintptr_t kQueueStride = 8;
    constexpr std::int32_t kQueueMax = 4;
    constexpr std::uintptr_t kEnabledOffset = 0x24;  // the foot audio is on (the main entry's own test, 0x8C2B64)
    constexpr std::uintptr_t kBlockedOffset = 0x5A7; // 0x317A1DF: set = no event
    constexpr std::int32_t kMaxType = 11;            // 0x89B608 gives 0..11 or -1
    constexpr std::uint8_t kMaxFoot = 12;            // 0x317A200: a foot of 12 or more is refused
    constexpr std::uint8_t kPlayedFeet = 4;          // 0x318CC04: the consumer plays feet 0..3

    // The entry's prologue up to its first branch (37 bytes, unique in
    // .text): mov rax,rsp; three saves; cmp byte [rcx+5A7h],0; mov sil,r9b;
    // mov r11b,r8b; mov ebx,edx; mov r10,rcx; jne. The fifth argument (direct)
    // is a byte on the stack, read at [rsp+28h] (0x317A279).
    constexpr std::uint8_t kEnqueueSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
        0x48, 0x89, 0x78, 0x20, 0x80, 0xB9, 0xA7, 0x05, 0x00, 0x00, 0x00, 0x41, 0x8A, 0xF1, 0x45, 0x8A, 0xD8, 0x8B,
        0xDA, 0x4C, 0x8B, 0xD1, 0x0F, 0x85, 0x37, 0x01};
    // 0x317A31A: mov [r10+0D4h],eax; mov rax,[rsp+8]; mov [r10+rcx*8+0B4h],rax.
    constexpr std::uint8_t kEnqueueStoreSignature[] = {0x41, 0x89, 0x82, 0xD4, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x44,
        0x24, 0x08, 0x49, 0x89, 0x84, 0xCA, 0xB4, 0x00, 0x00, 0x00};
    // 0x8ED457: call 0x317A1D0; mov [rbx+rdi*8+15Ch],esi.
    constexpr std::uint8_t kGeneratorCallSignature[] = {0xE8, 0x74, 0xCD, 0x88, 0x02, 0x89, 0xB4, 0xFB, 0x5C, 0x01,
        0x00, 0x00};
    // 0x3193D96: call 0x317A1D0; or dword [rdi],-1.
    constexpr std::uint8_t kDrainCallSignature[] = {0xE8, 0x35, 0x64, 0xFE, 0xFF, 0x83, 0x0F, 0xFF};

    // The target of a rel32 call at `rva` whose bytes start with E8.
    inline std::uintptr_t CallTarget(std::uintptr_t rva, const std::uint8_t* bytes)
    {
        const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes[1]) |
            (static_cast<std::uint32_t>(bytes[2]) << 8) | (static_cast<std::uint32_t>(bytes[3]) << 16) |
            (static_cast<std::uint32_t>(bytes[4]) << 24));
        return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva) + 5 + rel);
    }

    enum class Origin : std::uint8_t
    {
        Other = 0,
        Generator = 1, // 0x8ED457 (the movement component's lift or landing)
        Drain = 2,     // 0x3193D96 (the delayed table of a quadruped)
        Replay = 3,    // the replay's own (PlayFootEvent)
    };

    inline const char* OriginName(Origin o)
    {
        switch (o)
        {
        case Origin::Generator: return "gen";
        case Origin::Drain: return "drain";
        case Origin::Replay: return "replay";
        default: return "other";
        }
    }

    inline Origin OriginOf(std::uintptr_t returnRva)
    {
        return returnRva == kGeneratorReturnRva ? Origin::Generator
            : returnRva == kDrainReturnRva     ? Origin::Drain
                                               : Origin::Other;
    }

    // The 8-byte queue entry: the type as a dword, the foot as the next byte
    // (0x317A20A/0x317A20D build it in the shadow space; the last three bytes
    // are whatever was there).
    inline void DecodeQueueEntry(const std::uint8_t (&raw)[kQueueStride], std::int32_t& type, std::uint8_t& foot)
    {
        type = static_cast<std::int32_t>(static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
            (static_cast<std::uint32_t>(raw[2]) << 16) | (static_cast<std::uint32_t>(raw[3]) << 24));
        foot = raw[4];
    }

    inline bool ValidType(std::int32_t type) { return type >= 0 && type <= kMaxType; }
    inline bool PlausibleImpact(float v) { return std::isfinite(v) && v >= -100.0f && v <= 100.0f; }

    // One recorded entry (clip section FSEV).
    struct Event
    {
        std::uint32_t owner = 0; // packed replay id
        std::uint32_t timeMs = 0;
        std::int8_t type = -1;
        std::uint8_t foot = 0;
        std::uint8_t origin = 0; // Origin
        std::uint8_t flags = 0;  // kFlagImpact
        std::uint32_t materialHash = 0; // the foot's slot (+0x8): the container name hash, 0 unknown
        std::uint32_t materialRef = 0;  // the foot's slot (+0xC): the ref low dword, 0 unknown
        float impact = 0.0f;            // the foot's rise C +0x160 + 0x90 * foot (kFlagImpact)
    };
    constexpr std::uint8_t kFlagImpact = 1;
    constexpr std::size_t kEventBytes = 4 + 4 + 1 + 1 + 1 + 1 + 4 + 4 + 4;
    constexpr std::size_t kMaxEvents = 1u << 22; // ~4 million (about 12 h of six galloping horses)

    inline bool ValidEvent(const Event& e)
    {
        if (!ValidType(e.type) || e.foot >= kMaxFoot || e.origin > static_cast<std::uint8_t>(Origin::Replay) ||
            (e.flags & ~kFlagImpact) != 0)
            return false;
        return !(e.flags & kFlagImpact) || PlausibleImpact(e.impact);
    }

    struct Track
    {
        std::vector<Event> events; // time ascending (equal times keep their order)

        void Clear() { events.clear(); }
        bool Empty() const { return events.empty(); }
        std::size_t ApproxBytes() const { return events.capacity() * sizeof(Event); }

        // A late time is clamped to the last one (the clip's events never go
        // back). False: not kept (invalid, or the cap).
        bool Add(const Event& event)
        {
            if (!ValidEvent(event) || events.size() >= kMaxEvents)
                return false;
            Event copy = event;
            if (!events.empty() && copy.timeMs < events.back().timeMs)
                copy.timeMs = events.back().timeMs;
            events.push_back(copy);
            return true;
        }

        // The ring: one-shots before the base go (a step before the window's
        // base is never played). Returns how many.
        std::size_t DropBefore(std::uint32_t keepFromMs)
        {
            const auto first = std::lower_bound(events.begin(), events.end(), keepFromMs,
                [](const Event& e, std::uint32_t t) { return e.timeMs < t; });
            const std::size_t count = static_cast<std::size_t>(first - events.begin());
            events.erase(events.begin(), first);
            return count;
        }

        // A saved window [baseMs, endMs]: the kept owners' events inside it,
        // shifted to the window (one-shots: nothing before the base moves in).
        template <typename Kept>
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs, Kept kept) const
        {
            out.Clear();
            for (const Event& e : events)
            {
                if (e.timeMs > endMs)
                    break;
                if (e.timeMs < baseMs || !kept(e.owner))
                    continue;
                Event copy = e;
                copy.timeMs = e.timeMs - baseMs;
                out.events.push_back(copy);
            }
        }

        // A loaded section: every event valid, times never going back.
        bool Valid() const
        {
            if (events.size() > kMaxEvents)
                return false;
            for (std::size_t n = 0; n < events.size(); ++n)
            {
                if (!ValidEvent(events[n]) || (n && events[n - 1].timeMs > events[n].timeMs))
                    return false;
            }
            return true;
        }

        // Indices of the events with afterMs < timeMs <= untilMs.
        void Range(std::uint32_t afterMs, std::uint32_t untilMs, std::vector<std::size_t>& out) const
        {
            out.clear();
            if (untilMs <= afterMs)
                return;
            const auto first = std::upper_bound(events.begin(), events.end(), afterMs,
                [](std::uint32_t t, const Event& e) { return t < e.timeMs; });
            for (auto it = first; it != events.end() && it->timeMs <= untilMs; ++it)
                out.push_back(static_cast<std::size_t>(it - events.begin()));
        }

        std::size_t Owners() const
        {
            std::vector<std::uint32_t> owners;
            owners.reserve(events.size());
            for (const Event& e : events)
                owners.push_back(e.owner);
            std::sort(owners.begin(), owners.end());
            return static_cast<std::size_t>(std::unique(owners.begin(), owners.end()) - owners.begin());
        }
    };

    // ---- playback rules ----
    // The puppet's own events are refused only while a clip WITH the section
    // plays and the replay of the events is on: a clip without it keeps the
    // puppet's own generator, the only secondary steps it has.
    inline bool RefuseOwn(bool sectionPresent, bool replayOn) { return sectionPresent && replayOn; }

    // An event is replayed in normal forward play only (a pause, scrub or
    // seek plays none).
    inline bool Dispatch(bool replayOn, bool normalPlay, const Event& e)
    {
        return replayOn && normalPlay && ValidEvent(e) && e.foot < kPlayedFeet;
    }

    enum class PlayStatus : std::uint8_t
    {
        Queued = 0,
        QueueFull = 1, // +0xD4 already 4: the engine drops it the same way
        Disabled = 2,  // +0x24 clear: the foot audio is off
        Blocked = 3,   // +0x5A7 set: the entry refuses every event
        NoAudio = 4,   // no foot audio or another vtable
        Signature = 5, // the entry's prologue did not match
        Exception = 6, // a fault inside the call (SEH)
        NotQueued = 7, // the call returned and the count did not move
        BadEvent = 8,
    };

    inline const char* PlayStatusName(PlayStatus s)
    {
        switch (s)
        {
        case PlayStatus::Queued: return "queued";
        case PlayStatus::QueueFull: return "queue_full";
        case PlayStatus::Disabled: return "disabled";
        case PlayStatus::Blocked: return "blocked";
        case PlayStatus::NoAudio: return "no_audio";
        case PlayStatus::Signature: return "signature";
        case PlayStatus::Exception: return "exception";
        case PlayStatus::NotQueued: return "not_queued";
        case PlayStatus::BadEvent: return "bad_event";
        }
        return "?";
    }

    // ---- the evidence: per owner and window of clip time ----
    constexpr std::uint32_t kWindowMs = 5000;
    constexpr std::uint32_t kWindowLineCap = 600; // per phase

    struct WindowCounts
    {
        std::uint32_t calls = 0;   // calls of 0x317A1D0 on this foot audio
        std::uint32_t gen = 0;     // ... from the generator 0x8ED457
        std::uint32_t drain = 0;   // ... from the drain 0x3193D96
        std::uint32_t other = 0;   // ... from anywhere else
        std::uint32_t enq = 0;     // entries the engine put in the queue
        std::uint32_t refused = 0; // the puppet's own, refused (a clip with FSEV)
        std::uint32_t replayed = 0; // the clip's, queued by the replay
        std::uint32_t queueFull = 0;
        std::uint32_t failed = 0;  // the clip's, not queued for another reason
        std::uint32_t types[kMaxType + 1] = {};
        std::uint32_t badType = 0;

        bool Any() const { return calls || enq || refused || replayed || queueFull || failed; }
        void AddType(std::int32_t type)
        {
            if (ValidType(type))
                ++types[type];
            else
                ++badType;
        }
        void AddCall(Origin origin)
        {
            ++calls;
            if (origin == Origin::Generator)
                ++gen;
            else if (origin == Origin::Drain)
                ++drain;
            else
                ++other;
        }
    };

    // "8:12|2:3" (type:count of the entries queued or replayed), "none".
    inline std::string TypesText(const WindowCounts& c)
    {
        std::string text;
        char part[24];
        for (std::int32_t t = 0; t <= kMaxType; ++t)
        {
            if (!c.types[t])
                continue;
            std::snprintf(part, sizeof(part), "%s%d:%u", text.empty() ? "" : "|", t, c.types[t]);
            text += part;
        }
        if (c.badType)
        {
            std::snprintf(part, sizeof(part), "%sbad:%u", text.empty() ? "" : "|", c.badType);
            text += part;
        }
        return text.empty() ? "none" : text;
    }

    inline std::string WindowLine(const char* phase, std::uint32_t windowMs, const std::string& owner, const WindowCounts& c)
    {
        char head[256];
        std::snprintf(head, sizeof(head),
            "FOOT_EVENT_WINDOW,phase=%s,clip_ms=%u,owner=%s,calls=%u,gen=%u,drain=%u,other=%u,enq=%u,refused=%u,replayed=%u,"
            "queue_full=%u,failed=%u,types=",
            phase, windowMs, owner.c_str(), c.calls, c.gen, c.drain, c.other, c.enq, c.refused, c.replayed, c.queueFull,
            c.failed);
        return head + TypesText(c);
    }

    // One window per owner, rolled as the time moves; `emit(windowStartMs,
    // owner, counts)` gets each finished window that saw something.
    struct OwnerWindows
    {
        struct Entry
        {
            std::uint32_t window = 0xFFFFFFFFu;
            WindowCounts counts;
        };
        std::map<std::uint32_t, Entry> owners;

        template <typename Emit>
        WindowCounts& At(std::uint32_t owner, std::uint32_t timeMs, Emit emit)
        {
            Entry& entry = owners[owner];
            const std::uint32_t window = timeMs / kWindowMs;
            if (entry.window != window)
            {
                if (entry.window != 0xFFFFFFFFu && entry.counts.Any())
                    emit(entry.window * kWindowMs, owner, entry.counts);
                entry.window = window;
                entry.counts = WindowCounts{};
            }
            return entry.counts;
        }

        template <typename Emit>
        void Flush(Emit emit)
        {
            for (auto& [owner, entry] : owners)
                if (entry.window != 0xFFFFFFFFu && entry.counts.Any())
                    emit(entry.window * kWindowMs, owner, entry.counts);
            owners.clear();
        }
    };

    struct Totals
    {
        std::uint64_t calls = 0, gen = 0, drain = 0, other = 0, enq = 0;
        std::uint64_t recorded = 0;  // record: entries kept in the clip
        std::uint64_t unmapped = 0;  // record: entries of a foot audio no recorded owner has
        std::uint64_t rejected = 0;  // record: the track refused it (cap or invalid)
        std::uint64_t due = 0;       // play: the clip's events the playhead passed in normal play
        std::uint64_t played = 0;
        std::uint64_t queueFull = 0;
        std::uint64_t failed = 0;
        std::uint64_t noPuppet = 0;
        std::uint64_t refusedOwn = 0;
        std::uint32_t lines = 0;
    };

    inline std::string TotalLine(const char* phase, bool installed, const std::string& error, bool recordOn, bool replayOn,
        bool section, bool refuseOwn, std::size_t clipEvents, std::size_t clipOwners, const Totals& t)
    {
        char text[640];
        std::snprintf(text, sizeof(text),
            "FOOT_EVENT_TOTAL,phase=%s,installed=%d,error=%s,record=%d,replay=%d,section=%s,refuse_own=%d,clip_events=%zu,"
            "clip_owners=%zu,calls=%llu,gen=%llu,drain=%llu,other=%llu,enq=%llu,recorded=%llu,unmapped=%llu,rejected=%llu,"
            "due=%llu,played=%llu,queue_full=%llu,failed=%llu,no_puppet=%llu,refused_own=%llu,lines=%u",
            phase, installed ? 1 : 0, error.empty() ? "none" : error.c_str(), recordOn ? 1 : 0, replayOn ? 1 : 0,
            section ? "FSEV" : "none", refuseOwn ? 1 : 0, clipEvents, clipOwners, static_cast<unsigned long long>(t.calls),
            static_cast<unsigned long long>(t.gen), static_cast<unsigned long long>(t.drain),
            static_cast<unsigned long long>(t.other), static_cast<unsigned long long>(t.enq),
            static_cast<unsigned long long>(t.recorded), static_cast<unsigned long long>(t.unmapped),
            static_cast<unsigned long long>(t.rejected), static_cast<unsigned long long>(t.due),
            static_cast<unsigned long long>(t.played), static_cast<unsigned long long>(t.queueFull),
            static_cast<unsigned long long>(t.failed), static_cast<unsigned long long>(t.noPuppet),
            static_cast<unsigned long long>(t.refusedOwn), t.lines);
        return text;
    }

    // ---- the soundset field of a step's indirect ref ----
    // A step's sound is an indirect ref 0x8000NN00'xxxxxxxx (0x252A888: bit 23
    // of the high dword and the field index NN), xxxxxxxx the container's low
    // dword.
    inline bool IndirectField(std::uint64_t ref, std::uint8_t& field, std::uint32_t& container)
    {
        if ((ref >> 48) != 0x80u)
            return false;
        field = static_cast<std::uint8_t>((ref >> 32) & 0xFFu);
        container = static_cast<std::uint32_t>(ref & 0xFFFFFFFFu);
        return true;
    }

    // ---- the paths of the player's horse's step sounds (HORSE_STEP_PATHS) ----
    // callers[2] of a create-and-play (return addresses, build 1491.50).
    constexpr std::uint32_t kMainStepCaller = 0x318D25B;      // 0x318D1B8, the main step's create (0x318D256)
    constexpr std::uint32_t kSecondaryStepCaller = 0x318D509; // 0x318D470, the second queue's sound (0x318D504)
    constexpr std::uint32_t kWetLayerCaller = 0x318D3A9;      // 0x318D1B8, the wet layer (0x318D3A4 -> 0x318CEB4)
    struct PathCounts
    {
        std::uint32_t requests = 0, main = 0, secondary = 0, wet = 0, other = 0;
        std::map<std::uint8_t, std::uint32_t> secondaryFields; // NN -> count
        bool Any() const { return requests != 0; }
        void Add(std::uint32_t caller2, std::uint64_t ref)
        {
            ++requests;
            if (caller2 == kMainStepCaller)
                ++main;
            else if (caller2 == kWetLayerCaller)
                ++wet;
            else if (caller2 == kSecondaryStepCaller)
            {
                ++secondary;
                std::uint8_t field = 0;
                std::uint32_t container = 0;
                if (IndirectField(ref, field, container))
                    ++secondaryFields[field];
            }
            else
                ++other;
        }
    };

    inline std::string PathLine(const char* phase, std::uint32_t windowMs, const std::string& owner, const PathCounts& c)
    {
        std::string fields;
        char part[32];
        for (const auto& [field, count] : c.secondaryFields)
        {
            std::snprintf(part, sizeof(part), "%s%02x:%u", fields.empty() ? "" : "|", field, count);
            fields += part;
        }
        char head[256];
        std::snprintf(head, sizeof(head),
            "HORSE_STEP_PATHS,phase=%s,clip_ms=%u,owner=%s,requests=%u,main=%u,secondary=%u,wet=%u,other=%u,sec_fields=",
            phase, windowMs, owner.c_str(), c.requests, c.main, c.secondary, c.wet, c.other);
        return head + (fields.empty() ? std::string("none") : fields);
    }

    // ---- the movement component's state (MOTION_STATE) ----
    // Offsets in the row +0x88 component C (build 1491.50):
    constexpr std::uintptr_t kSpeedOffset = 0x5DC;      // the smoothed speed (0x8EC5D1), read by 0x8F162A
    constexpr std::uintptr_t kGaitOffset = 0x630;       // the gait 0x8F14A4 writes
    constexpr std::uintptr_t kGaitLoadedOffset = 0x634; // the gait whose thresholds 0x8ED774 loaded (0x8F1A01)
    constexpr std::uintptr_t kGaitTableOffset = 0x4C0;  // pointer; T1..T4 at +4..+0x10 (0x8F1632..0x8F1672)
    constexpr std::uintptr_t kThresholdBaseOffset = 0x4F0;  // the lift threshold base (0x8ED3DD)
    constexpr std::uintptr_t kThresholdScaleOffset = 0x550; // times ... (0x8ED3C9)
    constexpr std::uintptr_t kThresholdInputOffset = 0x5E8; // ... [row +0xE0 +0x6AC] (0x8EFA3C)
    constexpr std::uintptr_t kFootStateOffset = 0x15C;  // + 0x90 * foot: the last event type (0x8ED45C), < 6 = down
    constexpr std::uintptr_t kFootRiseOffset = 0x160;   // + 0x90 * foot: the highest rise since the contact (0x8EEEC3)
    constexpr std::uintptr_t kFootHeightOffset = 0x164; // + 0x90 * foot: h of the last update (0x8EEEA8)
    constexpr std::uintptr_t kFootStride = 0x90;
    constexpr std::uintptr_t kContactsAOffset = 0x61C;  // incremented at 0x8EF17A (PED_VARS row88_61c)
    constexpr std::uintptr_t kContactsBOffset = 0x620;  // incremented at 0x8EF183, zeroed on a gait change (0x8F1A07)
    constexpr std::uintptr_t kByte63EOffset = 0x63E;    // written at 0x8EF78F/0x8EFC52 (the foot update's state byte)
    // In the row (component table entry): +0xA8 the mount link, +0xD0 the
    // motion state object ([+0x298] state, [+0x2A0] move blend ratio, read by
    // 0x8E53BC and 0x8F16B6).
    constexpr std::uintptr_t kRowMountLinkSlot = 0xA8;
    constexpr std::uintptr_t kRowMotionSlot = 0xD0;
    constexpr std::uintptr_t kMotionStateOffset = 0x298;
    constexpr std::uintptr_t kMotionMbrOffset = 0x2A0;

    struct MotionState
    {
        bool read = false;
        float speed = 0.0f;
        std::int32_t gait = -1;
        std::int32_t gaitLoaded = -1;
        bool tableKnown = false;
        float t[4] = {};
        bool mountLink = false;
        bool d0Known = false;
        std::int32_t d0State = 0;
        float d0Mbr = 0.0f;
        bool thrKnown = false;
        float thr = 0.0f; // [C +0x4F0] + [C +0x550] * [C +0x5E8]
        bool feetKnown = false;
        float height[4] = {};
        float rise[4] = {};
        std::int32_t last[4] = {};
        std::uint32_t contactsA = 0;
        std::uint32_t contactsB = 0;
        std::uint8_t b63e = 0;
    };

    inline std::string MotionLine(const char* phase, std::uint32_t clipMs, const std::string& owner, const char* kind,
        const MotionState& s, const char* src)
    {
        if (!s.read)
            return std::string("MOTION_STATE,phase=") + phase + ",clip_ms=" + std::to_string(clipMs) + ",owner=" + owner +
                ",kind=" + kind + ",read=0";
        char text[640];
        char table[64] = "none";
        if (s.tableKnown)
            std::snprintf(table, sizeof(table), "%.2f/%.2f/%.2f/%.2f", s.t[0], s.t[1], s.t[2], s.t[3]);
        char d0[48] = "none";
        if (s.d0Known)
            std::snprintf(d0, sizeof(d0), "%d:%.2f", s.d0State, s.d0Mbr);
        char thr[24] = "none";
        if (s.thrKnown)
            std::snprintf(thr, sizeof(thr), "%.3f", s.thr);
        char feet[200] = "none";
        if (s.feetKnown)
            std::snprintf(feet, sizeof(feet), "h=%.2f/%.2f/%.2f/%.2f,rise=%.2f/%.2f/%.2f/%.2f,last=%d/%d/%d/%d", s.height[0],
                s.height[1], s.height[2], s.height[3], s.rise[0], s.rise[1], s.rise[2], s.rise[3], s.last[0], s.last[1],
                s.last[2], s.last[3]);
        std::snprintf(text, sizeof(text),
            "MOTION_STATE,phase=%s,clip_ms=%u,owner=%s,kind=%s,speed=%.2f,gait=%d,gait_loaded=%d,tbl=%s,mount_link=%d,d0=%s,"
            "thr=%s,%s%s,contacts=%u/%u,b63e=%u,src=%s",
            phase, clipMs, owner.c_str(), kind, s.speed, s.gait, s.gaitLoaded, table, s.mountLink ? 1 : 0, d0, thr,
            s.feetKnown ? "" : "feet=", feet, s.contactsA, s.contactsB, s.b63e, src);
        return text;
    }

    constexpr std::uint32_t kMotionIntervalMs = 1000;
    constexpr std::uint32_t kMotionLineCap = 900; // per phase

    // ---- the movement volume's inputs (MOVEMENT_TERM_DETAIL) ----
    // 0x31815C4 (foot audio, limb, foot, value, out low-pass) is the volume
    // term every step sound adds (main 0x318D52C and secondary 0x318CB98). It
    // calls 0x317B9D0(holder +0x750, ...) first (the component whose byte
    // +0x24 is holder +0x774), then the player term (-2.0 at 0x3E67E5C). This
    // line gives the inputs and the result per owner and 5 s, recording
    // against replay, to tell which term makes a volume differ.
    constexpr std::uintptr_t kMovementLimbComponentOffset = 0x750; // in the ped audio holder (0x3181603)
    constexpr std::uintptr_t kMovementLimbGateOffset = 0x774;      // holder +0x750 +0x24
    constexpr std::uintptr_t kMovementF504Offset = 0x504;          // foot audio (the step volume scale input)
    struct MovementWindow
    {
        std::uint32_t n = 0;
        double sumValue = 0.0;
        double sumResult = 0.0;
        double minResult = 0.0;
        double maxResult = 0.0;
        std::uint32_t gateKnown = 0;
        std::uint32_t gateSet = 0;
        std::uint32_t f504Known = 0;
        double sumF504 = 0.0;

        bool Any() const { return n != 0; }
        void Add(float value, float result, int gate, bool f504Known_, float f504)
        {
            if (!std::isfinite(value) || !std::isfinite(result))
                return;
            if (!n || result < minResult)
                minResult = result;
            if (!n || result > maxResult)
                maxResult = result;
            ++n;
            sumValue += value;
            sumResult += result;
            if (gate >= 0)
            {
                ++gateKnown;
                gateSet += gate ? 1u : 0u;
            }
            if (f504Known_ && std::isfinite(f504))
            {
                ++f504Known;
                sumF504 += f504;
            }
        }
    };

    inline std::string MovementLine(const char* phase, std::uint32_t windowMs, const std::string& owner, const MovementWindow& w)
    {
        char text[320];
        std::snprintf(text, sizeof(text),
            "MOVEMENT_TERM_DETAIL,phase=%s,clip_ms=%u,owner=%s,n=%u,value_mean=%.3f,result_mean=%.3f,result_min=%.3f,"
            "result_max=%.3f,gate774=%u/%u,f504_mean=%s",
            phase, windowMs, owner.c_str(), w.n, w.n ? w.sumValue / w.n : 0.0, w.n ? w.sumResult / w.n : 0.0, w.minResult,
            w.maxResult, w.gateSet, w.gateKnown, w.f504Known ? std::to_string(w.sumF504 / w.f504Known).c_str() : "none");
        return text;
    }
    constexpr std::uint32_t kMovementLineCap = 400; // per phase
}
