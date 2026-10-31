#pragma once

// The secondary steps of a ped's feet (the hoof that lifts or lands in a
// gallop), recorded as events and replayed on the puppet, as a replay editor
// records footstep sounds, applied to the second queue of the foot audio.
// Pure part (no game).
//
// Why events and not physics: a frozen puppet's ground plane goes stale (it
// lifts once and never lands again) and its gait stays 0 (C +0x5DC is 0), so
// its own lift/land detector gives the standing types. So each step event is
// recorded and replayed in normal play.
//
// The mechanism: the recorder keeps every entry the engine put in a recorded
// ped's second queue (type, foot, the slot's container, the foot's rise), as
// it entered it (the drain of a quadruped included); the playback queues the
// same entry on the puppet at the same clip time with direct 1 (straight into
// the queue), after setting the recorded container on the foot's slot. While
// a clip with the section plays, the puppet's own events are refused.
//
// Evidence: offline dump of build 1491.50; the code sites below hold the
// layout and are checked by signature.
// Research: docs/research/footsteps.md

#include "game_sig_core.h"

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
    namespace sig = replay::sig;

    // ---- engine layout ----
    constexpr std::uintptr_t kQueueOffset = 0xB4;
    constexpr std::uintptr_t kQueueCountOffset = 0xD4;
    constexpr std::uintptr_t kQueueStride = 8;
    constexpr std::int32_t kQueueMax = 4;
    constexpr std::uintptr_t kEnabledOffset = 0x24;  // the foot audio is on (the main entry's own test)
    constexpr std::uintptr_t kBlockedOffset = 0x5A7; // the entry's first test: set = no event
    constexpr std::int32_t kMaxType = 11;            // the gait table gives 0..11 or -1
    constexpr std::uint8_t kMaxFoot = 12;            // the entry refuses a foot of 12 or more
    constexpr std::uint8_t kPlayedFeet = 4;          // the secondary step plays feet 0..3

    // The entry's prologue up to its first branch (37 bytes): mov rax,rsp;
    // three saves; cmp byte [rcx+5A7h],0; mov sil,r9b; mov r11b,r8b; mov
    // ebx,edx; mov r10,rcx; jne. The fifth argument (direct) is a byte on the
    // stack, read at [rsp+28h].
    inline const sig::Signature kEnqueue{"foot_event_enqueue",
        "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 80 B9 A7 05 00 00 00 41 8A F1 45 8A D8 8B DA 4C 8B D1 0F 85 ?? ?? "
        "?? ?? 83 FA FF 0F 84 ?? ?? ?? ?? 41 80 F8 0C 0F 83 ?? ?? ?? ?? 89 50 08 44 88 40 0C 48 8B 41 18 8B 80 9C 00 00 "
        "00 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 94 01 E0 00 00 00 48 8B C2 "
        "48 83 E0 FE 48 F7 DA 48 1B FF 48 23 F8 48 8B 87 60 06 00 00 48 85 C0 74 0A 80 78 0C 04 75 04 B0 01 EB 02 32 C0 "
        "80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 80 7C 24 28 00",
        0x317A1D0};
    // The store: mov [r10+0D4h],eax; mov rax,[rsp+8]; mov [r10+rcx*8+0B4h],rax.
    inline const sig::Signature kEnqueueStore{"foot_event_enqueue_store", kEnqueue, sig::Offset{0x14A},
        "41 89 82 D4 00 00 00 48 8B 44 24 08 49 89 84 CA B4 00 00 00", 0x317A31A};
    // The lift/land detector's call of the entry; mov [rbx+rdi*8+15Ch],esi.
    inline const sig::Signature kGenerator{"foot_event_generator",
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 30 48 8B 41 08", 0x8ED350};
    inline const sig::Signature kGeneratorCall{"foot_event_generator_call", kGenerator, sig::Offset{0x107},
        "E8 ?? ?? ?? ?? 89 B4 FB 5C 01 00 00", 0x8ED457};
    // The movement update's drain of the delayed table: the call; or dword
    // [rdi],-1.
    inline const sig::Signature kMovementUpdate{"foot_event_movement_update",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 50 0F 29 70 E8 48 8D B9 E0 00 00 00", 0x3193D14};
    inline const sig::Signature kDrainCall{"foot_event_drain_call", kMovementUpdate, sig::Offset{0x82},
        "E8 ?? ?? ?? ?? 83 0F FF", 0x3193D96};

    // The return addresses of the two calls (call + 5) as RVAs in this game,
    // 0 for a call not found (nothing matches it).
    struct CallReturns
    {
        std::uint32_t generator = 0;
        std::uint32_t drain = 0;
    };

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
        Generator = 1, // kGeneratorCall (the movement component's lift or landing)
        Drain = 2,     // kDrainCall (the delayed table of a quadruped)
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

    inline Origin OriginOf(std::uintptr_t returnRva, const CallReturns& calls)
    {
        if (!returnRva)
            return Origin::Other;
        return returnRva == calls.generator ? Origin::Generator
            : returnRva == calls.drain     ? Origin::Drain
                                           : Origin::Other;
    }

    // The 8-byte queue entry: the type as a dword, the foot as the next byte
    // (the entry builds it in the shadow space; the last three bytes are
    // whatever was there).
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
        std::uint32_t calls = 0;   // calls of the entry on this foot audio
        std::uint32_t gen = 0;     // ... from the generator (kGeneratorCall)
        std::uint32_t drain = 0;   // ... from the drain (kDrainCall)
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
    // A step's sound is an indirect ref 0x8000NN00'xxxxxxxx (the child ref
    // builder: bit 23 of the high dword and the field index NN), xxxxxxxx the
    // container's low dword.
    inline bool IndirectField(std::uint64_t ref, std::uint8_t& field, std::uint32_t& container)
    {
        if ((ref >> 48) != 0x80u)
            return false;
        field = static_cast<std::uint8_t>((ref >> 32) & 0xFFu);
        container = static_cast<std::uint32_t>(ref & 0xFFFFFFFFu);
        return true;
    }

    // ---- the paths of the player's horse's step sounds (HORSE_STEP_PATHS) ----
    // callers[2] of a create-and-play: the main step's create and its wet
    // layer, and the secondary step's sound (the second queue's).
    inline const sig::Signature kMainStep{"foot_event_main_step",
        "48 8B C4 48 89 58 18 55 56 57 41 56 41 57 48 8D A8 48 FF FF FF 48 81 EC 90 01 00 00 44 0F B6 4A 0C",
        0x318D1B8};
    inline const sig::Signature kMainStepCall{"foot_event_main_step_call", kMainStep, sig::Offset{0x9E},
        "E8 ?? ?? ?? ??", 0x318D256};
    inline const sig::Signature kWetLayerCall{"foot_event_wet_layer_call", kMainStep, sig::Offset{0x1EC},
        "E8 ?? ?? ?? ??", 0x318D3A4};
    inline const sig::Signature kSecondaryStep{"foot_event_secondary_step",
        "4C 8B DC 49 89 5B 10 49 89 73 18 57 48 81 EC 70 01 00 00", 0x318D470};
    inline const sig::Signature kSecondaryStepCall{"foot_event_secondary_step_call", kSecondaryStep, sig::Offset{0x94},
        "E8 ?? ?? ?? ??", 0x318D504};
    // Their return addresses (call + 5) as RVAs in this game, 0 for a call
    // not found (nothing matches it).
    struct StepCallers
    {
        std::uint32_t main = 0;
        std::uint32_t secondary = 0;
        std::uint32_t wet = 0;
    };
    struct PathCounts
    {
        std::uint32_t requests = 0, main = 0, secondary = 0, wet = 0, other = 0;
        std::map<std::uint8_t, std::uint32_t> secondaryFields; // NN -> count
        bool Any() const { return requests != 0; }
        void Add(const StepCallers& paths, std::uint32_t caller2, std::uint64_t ref)
        {
            ++requests;
            if (caller2 && caller2 == paths.main)
                ++main;
            else if (caller2 && caller2 == paths.wet)
                ++wet;
            else if (caller2 && caller2 == paths.secondary)
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
    // Offsets in the row +0x88 component C (read only, for the log):
    constexpr std::uintptr_t kSpeedOffset = 0x5DC;      // the smoothed speed, read by the gait choice
    constexpr std::uintptr_t kGaitOffset = 0x630;       // the gait the gait choice writes
    constexpr std::uintptr_t kGaitLoadedOffset = 0x634; // the gait whose thresholds the threshold load took
    constexpr std::uintptr_t kGaitTableOffset = 0x4C0;  // pointer; T1..T4 at +4..+0x10 (the gait choice)
    constexpr std::uintptr_t kThresholdBaseOffset = 0x4F0;  // the lift threshold base (the generator)
    constexpr std::uintptr_t kThresholdScaleOffset = 0x550; // times ... (the generator)
    constexpr std::uintptr_t kThresholdInputOffset = 0x5E8; // ... [row +0xE0 +0x6AC] (the foot update)
    constexpr std::uintptr_t kFootStateOffset = 0x15C;  // + 0x90 * foot: the last event type (generator), < 6 = down
    constexpr std::uintptr_t kFootRiseOffset = 0x160;   // + 0x90 * foot: the highest rise since the contact
    constexpr std::uintptr_t kFootHeightOffset = 0x164; // + 0x90 * foot: h of the last update (the foot loop)
    constexpr std::uintptr_t kFootStride = 0x90;
    constexpr std::uintptr_t kContactsAOffset = 0x61C;  // incremented by the foot update (PED_VARS row88_61c)
    constexpr std::uintptr_t kContactsBOffset = 0x620;  // incremented by the foot update, zeroed on a gait change
    constexpr std::uintptr_t kByte63EOffset = 0x63E;    // written by the foot update (its state byte)
    // In the row (component table entry): +0xA8 the mount link, +0xD0 the
    // motion state object ([+0x298] state, [+0x2A0] move blend ratio, read by
    // the motion state getters).
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
    // The movement volume (foot audio, limb, foot, value, out low-pass) is the
    // volume term every step sound adds (the main and the secondary step). It
    // calls the limb gate (holder +0x750, ...) first (the component whose byte
    // +0x24 is holder +0x774), then the player term (-2.0, a constant of the
    // exe). This line gives the inputs and the result per owner and 5 s,
    // recording against replay, to tell which term makes a volume differ.
    constexpr std::uintptr_t kMovementLimbComponentOffset = 0x750; // in the ped audio holder (the movement volume)
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
