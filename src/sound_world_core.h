#pragma once

// Pure parts of the world sound diagnostics (unit tested in
// tests/sound_world_tests.cpp):
//
// - SOUND_GENERIC_STEAL: one line per slot the generic player takes from a
//   playing sound (FormatSteal), so a stolen loop is named, not inferred;
// - YieldWatch: a long world packet that yielded to a live sound of the same
//   id nearby (the anti-double rule) and whose live twin then ends while the
//   recorded copy still had more than kLostMarginMs to live
//   (SOUND_GENERIC_YIELD_LOST);
// - PtfxWatch: the particle audio slots of the engine (manager 0x5C9A660,
//   128 slots of 0x1A0 from +0x1D0, ticked by 0x320E568): which slot made its
//   sound and which one the engine marked done without one because the
//   listener was farther than the settings' distance (PTFX_AUDIO_SLOT);
// - the horse group audio (manager [0x5BF7EA8], 10 groups of 0x390 from
//   +0x10, updated by 0x31401A0): members, movers by gait, mean speed and
//   the group's loop (GROUP_AUDIO), recording against replay.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::soundworld
{
    constexpr std::uint32_t kNeverMs = 0xFFFFFFFFu;

    // ------------------------------------------------------------ steals

    struct StealView
    {
        std::uint32_t playheadMs = 0;
        const char* reason = "?";      // soundslots::ReasonName
        std::uint64_t capKey = 0;
        std::uint32_t victimOwner = 0xFFFF0000u;
        std::uint64_t victimRef = 0;   // the packet's stable id, else its raw ref
        const char* victimFamily = "?";
        bool victimLong = false;
        std::uint32_t victimStartedMs = 0;  // playhead at its create
        std::uint32_t victimLifeMs = kNeverMs; // recorded life (kNeverMs: a loop or no end in the clip)
        float victimDistanceM = 0.0f;
        std::uint32_t newOwner = 0xFFFF0000u;
        std::uint64_t newRef = 0;
        const char* newFamily = "?";
        float newDistanceM = 0.0f;
    };

    // The recorded life the victim still had at the steal (-1: unknown or a loop).
    inline long long VictimLeftMs(const StealView& s)
    {
        if (s.victimLifeMs == kNeverMs)
            return -1;
        const long long end = static_cast<long long>(s.victimStartedMs) + s.victimLifeMs;
        const long long left = end - static_cast<long long>(s.playheadMs);
        return left > 0 ? left : 0;
    }

    inline std::string OwnerText(std::uint32_t packed)
    {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%u:%u", packed >> 16, packed & 0xFFFFu);
        return text;
    }

    inline std::string FormatSteal(const StealView& s)
    {
        char line[512]{};
        const std::uint32_t age = s.playheadMs >= s.victimStartedMs ? s.playheadMs - s.victimStartedMs : 0;
        std::snprintf(line, sizeof(line),
            "SOUND_GENERIC_STEAL,playhead_ms=%u,reason=%s,cap_key=0x%llx,victim_owner=%s,victim_ref=0x%llx,victim_family=%s,"
            "victim_long=%d,victim_age_ms=%u,victim_left_ms=%lld,victim_distance_m=%.1f,new_owner=%s,new_ref=0x%llx,"
            "new_family=%s,new_distance_m=%.1f",
            s.playheadMs, s.reason, static_cast<unsigned long long>(s.capKey), OwnerText(s.victimOwner).c_str(),
            static_cast<unsigned long long>(s.victimRef), s.victimFamily, s.victimLong ? 1 : 0, age, VictimLeftMs(s),
            s.victimDistanceM, OwnerText(s.newOwner).c_str(), static_cast<unsigned long long>(s.newRef), s.newFamily,
            s.newDistanceM);
        return line;
    }

    // ------------------------------------------------------------ yields

    constexpr std::uint32_t kLostMarginMs = 1000;
    constexpr std::size_t kYieldWatchMax = 64;

    struct LostYield
    {
        std::uint32_t index = 0;   // the packet's number in the recording
        std::uint64_t ref = 0;
        std::uint64_t sound = 0;   // the live sound it yielded to
        std::uint32_t yieldMs = 0;
        std::uint32_t endMs = kNeverMs;
        std::uint32_t lostMs = 0;  // playhead when the live sound was seen gone
    };

    struct YieldCounters
    {
        std::uint64_t tracked = 0;   // a long world yield whose live twin is a persistent sound we can watch
        std::uint64_t untracked = 0; // ... whose twin is not (no census sound, or not in the live list)
        std::uint64_t dropped = 0;   // the watch was full
        std::uint64_t lost = 0;      // the twin ended with more than kLostMarginMs of recorded life left
        std::uint64_t outlived = 0;  // the twin lived to the copy's recorded end (or ended within the margin)
        std::uint64_t cleared = 0;   // forgotten at a jump or a reset
    };

    // The long world packets that yielded, watched until their recorded end.
    class YieldWatch
    {
    public:
        void Clear()
        {
            m_counters.cleared += m_list.size();
            m_list.clear();
        }
        void Reset()
        {
            m_list.clear();
            m_counters = YieldCounters{};
        }
        // `alive`: the live sound was in the engine's live list at the yield.
        void Note(std::uint32_t index, std::uint64_t ref, std::uint64_t sound, bool alive, std::uint32_t yieldMs,
            std::uint32_t endMs)
        {
            if (!sound || !alive)
            {
                ++m_counters.untracked;
                return;
            }
            for (const Entry& e : m_list)
                if (e.index == index)
                    return; // the same packet asked again (a retry)
            if (m_list.size() >= kYieldWatchMax)
            {
                ++m_counters.dropped;
                return;
            }
            ++m_counters.tracked;
            m_list.push_back(Entry{index, ref, sound, yieldMs, endMs});
        }
        // `isAlive(sound)`: the live sound is still the engine's. Returns the
        // copies that lost their twin early (each once); the rest stay
        // watched until their recorded end.
        template <typename IsAlive>
        void Check(std::uint32_t nowMs, IsAlive isAlive, std::vector<LostYield>& out)
        {
            std::size_t kept = 0;
            for (std::size_t n = 0; n < m_list.size(); ++n)
            {
                const Entry& e = m_list[n];
                if (e.endMs != kNeverMs && nowMs >= e.endMs)
                {
                    ++m_counters.outlived;
                    continue;
                }
                if (isAlive(e.sound))
                {
                    m_list[kept++] = e;
                    continue;
                }
                const bool early = e.endMs == kNeverMs || e.endMs - nowMs > kLostMarginMs;
                if (early)
                {
                    ++m_counters.lost;
                    out.push_back(LostYield{e.index, e.ref, e.sound, e.yieldMs, e.endMs, nowMs});
                }
                else
                    ++m_counters.outlived;
            }
            m_list.resize(kept);
        }
        std::size_t Size() const { return m_list.size(); }
        const YieldCounters& Counters() const { return m_counters; }

    private:
        struct Entry
        {
            std::uint32_t index;
            std::uint64_t ref;
            std::uint64_t sound;
            std::uint32_t yieldMs;
            std::uint32_t endMs;
        };
        std::vector<Entry> m_list;
        YieldCounters m_counters;
    };

    inline std::string FormatLostYield(const LostYield& l)
    {
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "SOUND_GENERIC_YIELD_LOST,playhead_ms=%u,index=%u,ref=0x%llx,live_sound=0x%llx,yield_ms=%u,recorded_end_ms=%lld,"
            "left_ms=%lld",
            l.lostMs, l.index, static_cast<unsigned long long>(l.ref), static_cast<unsigned long long>(l.sound), l.yieldMs,
            l.endMs == kNeverMs ? -1ll : static_cast<long long>(l.endMs),
            l.endMs == kNeverMs ? -1ll : static_cast<long long>(l.endMs) - static_cast<long long>(l.lostMs));
        return line;
    }

    // ------------------------------------------------------------ particle audio slots

    // Layout (build 1491.50): the particle audio manager is a global
    // object at RVA 0x5C9A660 (lea rcx at 0x2F1DABD before the call to the
    // slot loop 0x32148CC); its slots start at +0x1D0, 0x1A0 bytes each, up
    // to +0x1D0+0xD000 (128 slots: lea rbx,[rcx+0x1D0]; lea rbp,[rbx+0xD000];
    // add rbx,0x1A0); the loop passes (byte [mgr+0xD1F8] - 1) & 1 as the
    // buffer index of the slot's position (slot + index*0x10, 0x320E5D6).
    // Per slot (0x320E568): +0x175 byte skip (stops the slot instead),
    // +0x190 byte active, +0x160 settings (max distance float at +0x10;
    // over 0 the slot is tested), +0x177 byte done (set after the create at
    // 0x320E73C, or at 0x320E615 when the listener is farther than the max),
    // +0x180 the sound. A done slot whose settings' mode ([settings+4] & 3)
    // is not 1 makes no sound again.
    constexpr std::uint32_t kPtfxManagerRva = 0x5C9A660;
    constexpr std::uint32_t kPtfxSlotsOffset = 0x1D0;
    constexpr std::uint32_t kPtfxSlotStride = 0x1A0;
    constexpr std::uint32_t kPtfxSlotCount = 0xD000 / kPtfxSlotStride; // 128
    constexpr std::uint32_t kPtfxBufferIndex = 0xD1F8;
    constexpr std::uint32_t kPtfxSettings = 0x160;
    constexpr std::uint32_t kPtfxSettingsMaxM = 0x10;
    constexpr std::uint32_t kPtfxSettingsMode = 0x04;
    constexpr std::uint32_t kPtfxSkip = 0x175;
    constexpr std::uint32_t kPtfxDone = 0x177;
    constexpr std::uint32_t kPtfxSound = 0x180;
    constexpr std::uint32_t kPtfxActive = 0x190;
    static_assert(kPtfxSlotCount == 128, "particle audio slots");

    struct PtfxSlot
    {
        bool active = false;
        bool skip = false;
        bool done = false;
        std::uint64_t sound = 0;
        std::uint64_t settings = 0;
    };

    enum class PtfxEvent : std::uint8_t
    {
        Start,   // the slot became active (or took other settings)
        Created, // its sound appeared
        Far,     // done without a sound: the listener farther than the max (0x320E615; with had_sound=1 the far
                 // path stopped a sound it had), or the create refused
        Ended,   // the slot went inactive
    };
    inline const char* PtfxEventName(PtfxEvent e)
    {
        switch (e)
        {
        case PtfxEvent::Start: return "start";
        case PtfxEvent::Created: return "created";
        case PtfxEvent::Far: return "done_no_sound";
        case PtfxEvent::Ended: return "ended";
        }
        return "?";
    }

    struct PtfxChange
    {
        std::uint32_t slot = 0;
        PtfxEvent event = PtfxEvent::Start;
        bool hadSound = false; // the previous snapshot held a sound (a far stop rather than a refused create)
    };

    // The slots' transitions between two snapshots (the first snapshot of a
    // pass is the baseline: no events).
    class PtfxWatch
    {
    public:
        void Reset() { m_have = false; }
        void Diff(const PtfxSlot* now, std::size_t count, std::vector<PtfxChange>& out)
        {
            if (count > kPtfxSlotCount)
                count = kPtfxSlotCount;
            if (!m_have)
            {
                for (std::size_t n = 0; n < count; ++n)
                    m_prev[n] = now[n];
                m_have = true;
                return;
            }
            for (std::size_t n = 0; n < count; ++n)
            {
                const PtfxSlot& a = m_prev[n];
                const PtfxSlot& b = now[n];
                const std::uint32_t slot = static_cast<std::uint32_t>(n);
                const bool had = a.sound != 0;
                if (b.active && (!a.active || a.settings != b.settings))
                    out.push_back({slot, PtfxEvent::Start, had});
                if (b.sound && !a.sound)
                    out.push_back({slot, PtfxEvent::Created, had});
                if (b.active && b.done && !b.sound && (!a.done || !a.active))
                    out.push_back({slot, PtfxEvent::Far, had});
                if (a.active && !b.active)
                    out.push_back({slot, PtfxEvent::Ended, had});
                m_prev[n] = b;
            }
        }

    private:
        PtfxSlot m_prev[kPtfxSlotCount];
        bool m_have = false;
    };

    // ------------------------------------------------------------ horse group audio

    // Layout (build 1491.50): the manager pointer is the global at RVA
    // 0x5BF7EA8 (mov rcx,[rip+...] at 0x2EF20C8, then call 0x3140498); the
    // update walks 10 groups of 0x390 bytes, group k at manager + 0x10 +
    // k*0x390 (imul rax,rdx,0x390 at 0x31404AF; lea rdi,[rsi+0x10] at
    // 0x31404C1; cmp bl,0xA at 0x31404E7), active when +0x1C8 (its metadata)
    // is set (0x31401C8). Per group (0x31401A0): member pointers from +0x1D8
    // (add r15,0x3B at 0x314021D), member count dword +0x2F8 (0x314020B),
    // the movers' count per gait at +0x300 (4 dwords, 0x314032F), the movers'
    // mean position +0x310 and the members' mean speed +0x320 (0x31402D9,
    // 0x314033E, divided at 0x31403CB/0x31403DE; both zero when nobody
    // moves: the update then leaves before making the sound), the leader
    // +0x1D0 (0x312331E), the group's loop +0x328 (0x314D5F4, created by
    // 0x314D5B4 through 0x25889A8 with the metadata's +0x47F1).
    constexpr std::uint32_t kGroupManagerPtrRva = 0x5BF7EA8;
    constexpr std::uint32_t kGroupFirst = 0x10;
    constexpr std::uint32_t kGroupStride = 0x390;
    constexpr std::uint32_t kGroupCount = 10;
    constexpr std::uint32_t kGroupMeta = 0x1C8;
    constexpr std::uint32_t kGroupLeader = 0x1D0;
    constexpr std::uint32_t kGroupMembers = 0x1D8;
    constexpr std::uint32_t kGroupMemberCount = 0x2F8;
    constexpr std::uint32_t kGroupMemberMax = (kGroupMemberCount - kGroupMembers) / 8; // 36
    constexpr std::uint32_t kGroupGaitCounts = 0x300;
    constexpr std::uint32_t kGroupMeanPos = 0x310;
    constexpr std::uint32_t kGroupMeanSpeed = 0x320;
    constexpr std::uint32_t kGroupSound = 0x328;
    static_assert(kGroupMemberMax == 36, "group members");

    struct GroupView
    {
        std::uint32_t group = 0;
        std::uint32_t members = 0;   // the count field
        std::uint32_t nonNull = 0;   // member pointers set
        std::uint32_t replay = 0;    // members that are replay entities (puppets in play, recorded entities in rec)
        std::uint32_t livePlayer = 0; // members that are the live player (play)
        std::uint32_t gaits[4] = {};
        float speed = 0.0f;
        float pos[3] = {};
        bool sound = false;
        std::uint32_t leaderId = 0xFFFF0000u; // packed replay id of the leader (none: 65535:0)
        std::string memberIds;      // "a:b;c:d" of the replay members
    };

    inline std::uint32_t Movers(const GroupView& g)
    {
        return g.gaits[0] + g.gaits[1] + g.gaits[2] + g.gaits[3];
    }

    inline std::string FormatGroup(const char* phase, std::uint32_t clipMs, const char* why, const GroupView& g,
        float distM)
    {
        char line[512]{};
        std::snprintf(line, sizeof(line),
            "GROUP_AUDIO,phase=%s,clip_ms=%u,why=%s,group=%u,members=%u,non_null=%u,replay=%u,live_player=%u,"
            "movers=%u,gaits=%u/%u/%u/%u,speed=%.2f,pos=%.1f;%.1f;%.1f,listener_m=%.1f,sound=%d,leader=%s,ids=%s",
            phase, clipMs, why, g.group, g.members, g.nonNull, g.replay, g.livePlayer, Movers(g), g.gaits[0], g.gaits[1],
            g.gaits[2], g.gaits[3], g.speed, g.pos[0], g.pos[1], g.pos[2], distM, g.sound ? 1 : 0,
            OwnerText(g.leaderId).c_str(), g.memberIds.empty() ? "none" : g.memberIds.c_str());
        return line;
    }

    // Sanity of a read group (a torn or stale one is not logged).
    inline bool GroupSane(const GroupView& g)
    {
        if (g.members > kGroupMemberMax)
            return false;
        if (!(std::fabs(g.speed) < 1000.0f))
            return false;
        for (float p : g.pos)
            if (!(std::fabs(p) < 100000.0f))
                return false;
        return Movers(g) <= kGroupMemberMax;
    }

    // ------------------------------------------------------------ code sites

    // The world sound probe (world_sound_probe.cpp) reads the fields above
    // only after every site of its part matched byte for byte (build 1491.50;
    // a mismatch turns the part off and logs WORLD_SOUND_PROBE_CHECK,ok=0). The
    // listener part also needs the game listener's own sites
    // (game_listener_core.h).
    enum class ProbePart : std::uint8_t
    {
        Ptfx = 0,
        Group = 1,
        Listener = 2,
    };
    inline const char* ProbePartName(ProbePart part)
    {
        switch (part)
        {
        case ProbePart::Ptfx: return "ptfx";
        case ProbePart::Group: return "group";
        case ProbePart::Listener: return "listener";
        }
        return "?";
    }

    struct ProbeSite
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[8];
        ProbePart part;
    };
    constexpr ProbeSite kProbeSites[] = {
        // particle audio slots
        {"ptfx_mgr_lea", 0x2F1DABD, 7, {0x48, 0x8D, 0x0D, 0x9C, 0xCB, 0xD7, 0x02}, ProbePart::Ptfx},
        {"ptfx_slot_call", 0x2F1DAC4, 5, {0xE8, 0x03, 0x6E, 0x2F, 0x00}, ProbePart::Ptfx},
        {"ptfx_buffer", 0x32148E0, 7, {0x40, 0x8A, 0xB9, 0xF8, 0xD1, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_slots_first", 0x32148E7, 7, {0x48, 0x8D, 0x99, 0xD0, 0x01, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_slots_end", 0x32148F1, 7, {0x48, 0x8D, 0xAB, 0x00, 0xD0, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_slot_stride", 0x321490F, 7, {0x48, 0x81, 0xC3, 0xA0, 0x01, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_skip", 0x320E58A, 7, {0x80, 0xBA, 0x75, 0x01, 0x00, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_active", 0x320E5AA, 7, {0x80, 0xBA, 0x90, 0x01, 0x00, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_settings", 0x320E5B7, 7, {0x48, 0x8B, 0x82, 0x60, 0x01, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_max", 0x320E5C8, 5, {0xF3, 0x0F, 0x10, 0x70, 0x10}, ProbePart::Ptfx},
        {"ptfx_pos", 0x320E5D6, 4, {0x0F, 0x10, 0x04, 0xFA}, ProbePart::Ptfx},
        {"ptfx_far_sound", 0x320E602, 7, {0x48, 0x8B, 0x8B, 0x80, 0x01, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_far_done", 0x320E615, 7, {0xC6, 0x83, 0x77, 0x01, 0x00, 0x00, 0x01}, ProbePart::Ptfx},
        {"ptfx_done_test", 0x320E6EE, 6, {0x38, 0x83, 0x77, 0x01, 0x00, 0x00}, ProbePart::Ptfx},
        {"ptfx_mode", 0x320E6FD, 3, {0x8B, 0x48, 0x04}, ProbePart::Ptfx},
        {"ptfx_create", 0x320E737, 5, {0xE8, 0xF4, 0xA3, 0x37, 0xFF}, ProbePart::Ptfx},
        {"ptfx_created_done", 0x320E73C, 7, {0xC6, 0x83, 0x77, 0x01, 0x00, 0x00, 0x01}, ProbePart::Ptfx},
        // horse group audio
        {"group_mgr_load", 0x2EF20C8, 7, {0x48, 0x8B, 0x0D, 0xD9, 0x5D, 0xD0, 0x02}, ProbePart::Group},
        {"group_update_call", 0x2EF20CF, 5, {0xE8, 0xC4, 0xE3, 0x24, 0x00}, ProbePart::Group},
        {"group_stride", 0x31404AF, 7, {0x48, 0x69, 0xC2, 0x90, 0x03, 0x00, 0x00}, ProbePart::Group},
        {"group_first", 0x31404C1, 4, {0x48, 0x8D, 0x7E, 0x10}, ProbePart::Group},
        {"group_count", 0x31404E7, 3, {0x80, 0xFB, 0x0A}, ProbePart::Group},
        {"group_meta", 0x31401C8, 7, {0x48, 0x8B, 0x81, 0xC8, 0x01, 0x00, 0x00}, ProbePart::Group},
        {"group_member_count", 0x314020B, 6, {0x8B, 0xB9, 0xF8, 0x02, 0x00, 0x00}, ProbePart::Group},
        {"group_members", 0x314021D, 4, {0x49, 0x83, 0xC7, 0x3B}, ProbePart::Group},
        {"group_gait_count", 0x314032F, 7, {0xFF, 0x84, 0x83, 0x00, 0x03, 0x00, 0x00}, ProbePart::Group},
        {"group_pos_mean", 0x31403CB, 7, {0x0F, 0x11, 0x8B, 0x10, 0x03, 0x00, 0x00}, ProbePart::Group},
        {"group_speed_mean", 0x31403DE, 8, {0xF3, 0x0F, 0x11, 0x93, 0x20, 0x03, 0x00, 0x00}, ProbePart::Group},
        {"group_leader", 0x312331E, 7, {0x48, 0x39, 0x91, 0xD0, 0x01, 0x00, 0x00}, ProbePart::Group},
        {"group_sound", 0x314D5F4, 7, {0x48, 0x8D, 0xB9, 0x28, 0x03, 0x00, 0x00}, ProbePart::Group},
        // the particle slot's listener test 0x31DBE60: both listener
        // descriptions (lea rdi,[rcx+0x20]), a description counts while a
        // lane of its +0x2D0 weights is above 1e-5 (cmpltps at 0x31DBEB5)
        {"listener_first", 0x31DBE7B, 4, {0x48, 0x8D, 0x79, 0x20}, ProbePart::Listener},
        {"listener_weight", 0x31DBEB5, 8, {0x0F, 0xC2, 0x81, 0xD0, 0x02, 0x00, 0x00, 0x01}, ProbePart::Listener},
    };
    constexpr std::size_t kProbeSiteCount = sizeof(kProbeSites) / sizeof(kProbeSites[0]);
    constexpr std::uint32_t kListenerWeight = 0x2D0;
    constexpr float kListenerWeightMin = 1e-5f;

    // The target of a rip-relative lea/mov of 7 bytes (rip = the next instruction).
    constexpr std::uint32_t RipTarget7(const ProbeSite& site)
    {
        return site.rva + 7u +
            (static_cast<std::uint32_t>(site.bytes[3]) | (static_cast<std::uint32_t>(site.bytes[4]) << 8) |
                (static_cast<std::uint32_t>(site.bytes[5]) << 16) | (static_cast<std::uint32_t>(site.bytes[6]) << 24));
    }
    static_assert(RipTarget7(kProbeSites[0]) == kPtfxManagerRva, "ptfx_mgr_lea must point at the particle audio manager");
    static_assert(RipTarget7(kProbeSites[17]) == kGroupManagerPtrRva, "group_mgr_load must read the group manager pointer");

    inline bool ProbeSiteMatches(const ProbeSite& site, const std::uint8_t* code)
    {
        if (!code)
            return false;
        for (std::uint8_t n = 0; n < site.size; ++n)
            if (code[n] != site.bytes[n])
                return false;
        return true;
    }

    // The nearest of the listener points to `pos` (-1: none).
    inline float NearestListener(const float (*points)[3], std::size_t count, const float pos[3])
    {
        float best = -1.0f;
        for (std::size_t j = 0; j < count; ++j)
        {
            const float dx = points[j][0] - pos[0], dy = points[j][1] - pos[1], dz = points[j][2] - pos[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best;
    }

    // ------------------------------------------------------------ budgets

    // A per-pass line budget: Take() is true while lines remain; the dropped
    // ones are counted.
    class Budget
    {
    public:
        explicit Budget(std::uint32_t lines = 0) : m_left(lines) {}
        void Reset(std::uint32_t lines)
        {
            m_left = lines;
            m_dropped = 0;
        }
        // A new period: the lines come back, the dropped count goes on.
        void Refill(std::uint32_t lines) { m_left = lines; }
        bool Take()
        {
            if (m_left)
            {
                --m_left;
                return true;
            }
            ++m_dropped;
            return false;
        }
        std::uint64_t Dropped() const { return m_dropped; }

    private:
        std::uint32_t m_left = 0;
        std::uint64_t m_dropped = 0;
    };
}
