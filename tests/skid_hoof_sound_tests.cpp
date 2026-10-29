// A horse's skid with a recorded clip's own data (the horse 0:1 from
// 44.8 s to 47.8 s: its SGEN packets, ESND rows and FOOT events, decoded
// from 036-HennigansStead.rpclip), and the rules it exercises:
//   the sound key (sound_owner_table.h SoundKey): set and entry;
//   the cover and the anti-double rule by that key: the skid's body and
//   grains are not "made" by a hoof step of the same surface set;
//   the step exception only near a recorded foot event (FootWindows);
//   the ports' verdicts (PacketLives) with the clip's loops, and the
//   blocked section with runs;
//   the per-sound tally, its section, its findings and the notice at Stop
//   (a skid created and ended by the engine at once is MISSING);
//   a stolen slot does not answer for its first packet (the engine side's
//   generation, emulated by the fake engine with the slot policy);
//   SoundGenericTrace and SoundGenericBlock.
#include "puppet_audio_rule.h"
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"
#include "sound_slot_policy.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
    using namespace replay::soundgeneric;
    using namespace replay::soundgeneric::player;
    namespace owner = replay::soundowner;
    using replay::census::OwnerClass;

    int g_failuresSkid = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failuresSkid;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint32_t kHorse = 0x00000001u; // 0:1

    // 036-HennigansStead.rpclip, SGEN packets of the horse 0:1 from 44.8 s to 47.8 s (ReplayClipTool generic,
    // decoded with the clip's own reader layout): time, raw ref, family caller, then its ends (ms after the
    // create: forget = stop and forget S14, release = S15).
    struct SkidPacket { std::uint32_t t; std::uint64_t ref; std::uint32_t caller; std::uint8_t source; std::int16_t forgetMs; std::int16_t releaseMs; };
    const SkidPacket kSkid036[] = {
        {44827, 0x800000000e18baull, 0x318cb59u, 1, -1, 191},
        {44827, 0x800000000e18baull, 0x318cb59u, 1, -1, 191},
        {44835, 0x800005002f724dull, 0x318e828u, 1, -1, 926},
        {44835, 0x800005002f724dull, 0x318e828u, 1, -1, 841},
        {45068, 0x800005002f724dull, 0x318e828u, 1, -1, 806},
        {45068, 0x800000000d167cull, 0x318dedeu, 1, -1, 741},
        {45168, 0x800005002f724dull, 0x318e828u, 1, -1, 890},
        {45285, 0x800000000e18baull, 0x318cb59u, 1, -1, 189},
        {45285, 0x800000000e18baull, 0x318cb59u, 1, -1, 189},
        {45292, 0x800005002f4361ull, 0x318e828u, 1, -1, 550},
        {45292, 0x800005002f36cbull, 0x318e828u, 1, -1, 715},
        {45507, 0x800005002f4361ull, 0x318e828u, 1, -1, 784},
        {45507, 0x800000000d167cull, 0x318dedeu, 1, -1, 749},
        {45525, 0x80002b002f36cbull, 0x3197f00u, 2, -1, 2169},
        {45541, 0x80000e002f36cbull, 0x251688du, 0, 17, 52},
        {45558, 0x80000e002f36cbull, 0x251688du, 0, 136, 299},
        {45593, 0x800005002f4361ull, 0x318e828u, 1, -1, 663},
        {45640, 0x80000e002f5642ull, 0x251688du, 0, 19, 55},
        {45659, 0x80000e002f5642ull, 0x251688du, 0, 116, 249},
        {45694, 0x80000e002f5642ull, 0x251688du, 0, 98, 214},
        {45775, 0x80000e002f36cbull, 0x251688du, 0, 51, 99},
        {45792, 0x80000e002f4361ull, 0x251688du, 0, 16, 50},
        {45808, 0x80000e002f4361ull, 0x251688du, 0, 18, 49},
        {45826, 0x80000e002f4361ull, 0x251688du, 0, 15, 82},
        {45841, 0x80000e002f4361ull, 0x251688du, 0, 16, 33},
        {45857, 0x80000e002f4361ull, 0x251688du, 0, 17, 51},
        {45874, 0x80000e002f4361ull, 0x251688du, 0, 18, 50},
        {45892, 0x80000e002f5642ull, 0x251688du, 0, 15, 82},
        {45892, 0x80000e002f4361ull, 0x251688du, 0, 15, 82},
        {45907, 0x80000e002f5642ull, 0x251688du, 0, 34, 67},
        {45907, 0x80000e002f4361ull, 0x251688du, 0, 17, 34},
        {45924, 0x80000e002f4361ull, 0x251688du, 0, 17, 50},
        {45941, 0x80000e002f4361ull, 0x251688du, 0, 16, 33},
        {45957, 0x80000e002f4361ull, 0x251688du, 0, 17, 50},
        {45974, 0x80000e002f4361ull, 0x251688du, 0, 16, 51},
        {45991, 0x80000e002f4361ull, 0x251688du, 0, 15, 34},
        {46006, 0x80000e002f4361ull, 0x251688du, 0, 18, 52},
        {46025, 0x80000e002f4361ull, 0x251688du, 0, 15, 66},
        {46040, 0x80000e002f4361ull, 0x251688du, 0, 17, 51},
        {46057, 0x80000e002f4361ull, 0x251688du, 0, 17, 51},
        {46074, 0x80000e002f724dull, 0x251688du, 0, 16, 84},
        {46074, 0x80000e002f4361ull, 0x251688du, 0, 16, 84},
        {46090, 0x80000e002f5642ull, 0x251688du, 0, 33, 85},
        {46090, 0x80000e002f724dull, 0x251688du, 0, 18, 33},
        {46108, 0x80000e002f724dull, 0x251688du, 0, 233, 499},
        {46123, 0x80000e002f724dull, 0x251688du, 0, 150, 318},
        {46273, 0x80000e002f724dull, 0x251688du, 0, 467, 953},
        {46740, 0x80000e002f4361ull, 0x251688du, 0, 21, 69},
        {46740, 0x80000e002f724dull, 0x251688du, 0, 134, 302},
        {46761, 0x80000e002f4361ull, 0x251688du, 0, 15, 48},
        {46776, 0x80000e002f4361ull, 0x251688du, 0, 14, 66},
        {46790, 0x80000e002f4361ull, 0x251688du, 0, 18, 68},
        {46808, 0x80000e002f4361ull, 0x251688du, 0, 17, 50},
        {46825, 0x80000e002f4361ull, 0x251688du, 0, 17, 66},
        {46842, 0x80000e002f4361ull, 0x251688du, 0, 15, 49},
        {46857, 0x80000e002f4361ull, 0x251688du, 0, 17, 68},
        {46874, 0x80000e002f4361ull, 0x251688du, 0, 16, 67},
        {46890, 0x80000e002f4361ull, 0x251688du, 0, 20, 51},
        {46910, 0x80000e002f4361ull, 0x251688du, 0, 14, 64},
        {46924, 0x80000e002f4361ull, 0x251688du, 0, 17, 67},
        {46941, 0x80000e002f4361ull, 0x251688du, 0, 17, 50},
        {46958, 0x80000e002f4361ull, 0x251688du, 0, 16, 66},
        {46974, 0x80000e002f4361ull, 0x251688du, 0, 17, 68},
        {46991, 0x80000e002f4361ull, 0x251688du, 0, 16, 33},
        {47007, 0x80000e002f4361ull, 0x251688du, 0, 17, 52},
        {47024, 0x80000e002f4361ull, 0x251688du, 0, 17, 35},
        {47041, 0x80000e002f4361ull, 0x251688du, 0, 17, 83},
        {47058, 0x80000e002f4361ull, 0x251688du, 0, 16, 33},
        {47074, 0x80000e002f4361ull, 0x251688du, 0, 17, 67},
        {47091, 0x80000e002f4361ull, 0x251688du, 0, 16, 50},
        {47107, 0x80000e002f4361ull, 0x251688du, 0, 16, 51},
        {47123, 0x80000e002f4361ull, 0x251688du, 0, 16, 70},
        {47139, 0x80000e002f4361ull, 0x251688du, 0, 19, 54},
        {47158, 0x80000e002f4361ull, 0x251688du, 0, 15, 48},
        {47173, 0x80000e002f4361ull, 0x251688du, 0, 19, 53},
        {47192, 0x80000e002f4361ull, 0x251688du, 0, 14, 34},
        {47206, 0x80000e002f4361ull, 0x251688du, 0, 19, 51},
        {47225, 0x80000e002f4361ull, 0x251688du, 0, 15, 66},
        {47240, 0x80000e002f4361ull, 0x251688du, 0, 17, 51},
        {47257, 0x80000e002f4361ull, 0x251688du, 0, 16, 66},
        {47273, 0x80000e002f4361ull, 0x251688du, 0, 17, 50},
        {47290, 0x80000e002f4361ull, 0x251688du, 0, 17, 33},
        {47307, 0x80000e002f4361ull, 0x251688du, 0, 16, 67},
        {47323, 0x80000e002f4361ull, 0x251688du, 0, 17, 68},
        {47340, 0x80000e002f4361ull, 0x251688du, 0, 16, 51},
        {47374, 0x800019002f4361ull, 0x318e828u, 1, -1, 633},
        {47440, 0x800019002f4361ull, 0x318e828u, 1, -1, 585},
        {47557, 0x80000d002f4361ull, 0x318e828u, 1, -1, 618},
        {47591, 0x80000d002f4361ull, 0x318e828u, 1, -1, 718},
        {47601, 0x800007000e18baull, 0x318cb59u, 1, -1, 323},
        {47606, 0x80000d002f4361ull, 0x318e828u, 1, -1, 770},
        {47606, 0x800019002f4361ull, 0x318e828u, 1, -1, 584},
        {47624, 0x80000d002f4361ull, 0x318e828u, 1, -1, 620},
        {47718, 0x800007000e18baull, 0x318cb59u, 1, -1, 339},
        {47724, 0x800019002f4361ull, 0x318e828u, 1, -1, 636},
    };
    // The ESND rows of 0:1 in the same span (start, ref, caller): the skid's 70 grains and 2 sounds of the horse's line.
    struct SkidRow { std::uint32_t start; std::uint64_t ref; std::uint32_t caller; };
    const SkidRow kSkidRows036[] = {
        {44830, 0x800004004366e8ull, 0x2fa10cfu},
        {45288, 0x800004004366e8ull, 0x2fa10cfu},
        {45541, 0x80000e002f36cbull, 0x2588c2fu},
        {45558, 0x80000e002f36cbull, 0x2588c2fu},
        {45640, 0x80000e002f5642ull, 0x2588c2fu},
        {45659, 0x80000e002f5642ull, 0x2588c2fu},
        {45694, 0x80000e002f5642ull, 0x2588c2fu},
        {45775, 0x80000e002f36cbull, 0x2588c2fu},
        {45791, 0x80000e002f4361ull, 0x2588c2fu},
        {45809, 0x80000e002f4361ull, 0x2588c2fu},
        {45826, 0x80000e002f4361ull, 0x2588c2fu},
        {45841, 0x80000e002f4361ull, 0x2588c2fu},
        {45857, 0x80000e002f4361ull, 0x2588c2fu},
        {45874, 0x80000e002f4361ull, 0x2588c2fu},
        {45892, 0x80000e002f5642ull, 0x2588c2fu},
        {45892, 0x80000e002f4361ull, 0x2588c2fu},
        {45907, 0x80000e002f5642ull, 0x2588c2fu},
        {45907, 0x80000e002f4361ull, 0x2588c2fu},
        {45924, 0x80000e002f4361ull, 0x2588c2fu},
        {45941, 0x80000e002f4361ull, 0x2588c2fu},
        {45957, 0x80000e002f4361ull, 0x2588c2fu},
        {45975, 0x80000e002f4361ull, 0x2588c2fu},
        {45990, 0x80000e002f4361ull, 0x2588c2fu},
        {46007, 0x80000e002f4361ull, 0x2588c2fu},
        {46024, 0x80000e002f4361ull, 0x2588c2fu},
        {46040, 0x80000e002f4361ull, 0x2588c2fu},
        {46057, 0x80000e002f4361ull, 0x2588c2fu},
        {46074, 0x80000e002f724dull, 0x2588c2fu},
        {46074, 0x80000e002f4361ull, 0x2588c2fu},
        {46091, 0x80000e002f5642ull, 0x2588c2fu},
        {46091, 0x80000e002f724dull, 0x2588c2fu},
        {46107, 0x80000e002f724dull, 0x2588c2fu},
        {46124, 0x80000e002f724dull, 0x2588c2fu},
        {46137, 0x800004004366e8ull, 0x2fa10cfu},
        {46274, 0x80000e002f724dull, 0x2588c2fu},
        {46574, 0x800004004366e8ull, 0x2fa10cfu},
        {46740, 0x80000e002f4361ull, 0x2588c2fu},
        {46740, 0x80000e002f724dull, 0x2588c2fu},
        {46761, 0x80000e002f4361ull, 0x2588c2fu},
        {46776, 0x80000e002f4361ull, 0x2588c2fu},
        {46790, 0x80000e002f4361ull, 0x2588c2fu},
        {46808, 0x80000e002f4361ull, 0x2588c2fu},
        {46825, 0x80000e002f4361ull, 0x2588c2fu},
        {46842, 0x80000e002f4361ull, 0x2588c2fu},
        {46857, 0x80000e002f4361ull, 0x2588c2fu},
        {46874, 0x80000e002f4361ull, 0x2588c2fu},
        {46890, 0x80000e002f4361ull, 0x2588c2fu},
        {46909, 0x80000e002f4361ull, 0x2588c2fu},
        {46924, 0x80000e002f4361ull, 0x2588c2fu},
        {46942, 0x80000e002f4361ull, 0x2588c2fu},
        {46958, 0x80000e002f4361ull, 0x2588c2fu},
        {46973, 0x80000e002f4361ull, 0x2588c2fu},
        {46991, 0x80000e002f4361ull, 0x2588c2fu},
        {47007, 0x80000e002f4361ull, 0x2588c2fu},
        {47024, 0x80000e002f4361ull, 0x2588c2fu},
        {47041, 0x80000e002f4361ull, 0x2588c2fu},
        {47058, 0x80000e002f4361ull, 0x2588c2fu},
        {47074, 0x80000e002f4361ull, 0x2588c2fu},
        {47091, 0x80000e002f4361ull, 0x2588c2fu},
        {47107, 0x80000e002f4361ull, 0x2588c2fu},
        {47123, 0x80000e002f4361ull, 0x2588c2fu},
        {47140, 0x80000e002f4361ull, 0x2588c2fu},
        {47157, 0x80000e002f4361ull, 0x2588c2fu},
        {47174, 0x80000e002f4361ull, 0x2588c2fu},
        {47192, 0x80000e002f4361ull, 0x2588c2fu},
        {47207, 0x80000e002f4361ull, 0x2588c2fu},
        {47225, 0x80000e002f4361ull, 0x2588c2fu},
        {47240, 0x80000e002f4361ull, 0x2588c2fu},
        {47257, 0x80000e002f4361ull, 0x2588c2fu},
        {47273, 0x80000e002f4361ull, 0x2588c2fu},
        {47290, 0x80000e002f4361ull, 0x2588c2fu},
        {47307, 0x80000e002f4361ull, 0x2588c2fu},
        {47323, 0x80000e002f4361ull, 0x2588c2fu},
        {47340, 0x80000e002f4361ull, 0x2588c2fu},
    };
    // The FOOT events of 0:1 from 44.0 s to 48.0 s (time, arg7): gallop events with their hooves 0, 120, 330 and 430 ms
    // later, none from 45.283 to 47.357, then the stop.
    const std::uint32_t kFeet036[][2] = {
        {44400, 0},
        {44400, 120},
        {44400, 330},
        {44400, 430},
        {44818, 0},
        {44818, 120},
        {44818, 330},
        {44818, 430},
        {45283, 0},
        {45283, 120},
        {45283, 330},
        {45283, 430},
        {47357, 0},
        {47424, 0},
        {47540, 0},
        {47574, 0},
        {47591, 0},
        {47591, 0},
        {47607, 0},
        {47708, 0},
        {47842, 0},
        {47858, 0},
        {47858, 0},
        {47858, 0},
        {47858, 0},
    };

    std::uint32_t EntryOf(std::uint64_t ref) { return static_cast<std::uint32_t>((ref >> 32) & 0xFFu); }
    bool Grain(const SkidPacket& p) { return EntryOf(p.ref) == 0x0E && p.caller == 0x251688Du; }
    constexpr std::uint64_t kSlideRef = 0x80002b002f36cbull; // the skid's body: deferred, 2169 ms

    // The clip's track of the span (owner 0:1, horse).
    Track SkidTrack()
    {
        Track track;
        for (const SkidPacket& p : kSkid036)
        {
            CreatePacket c;
            c.timeMs = p.t;
            c.index = track.nextIndex++;
            const StableId id = StableIdOf(p.ref, false);
            c.stableId = id.id;
            c.idKind = id.kind;
            c.rawRef = p.ref;
            c.owner = kHorse;
            c.ownerClass = OwnerClass::Horse;
            c.source = static_cast<Source>(p.source);
            c.hasParams = true;
            c.caller = p.caller;
            track.creates.push_back(c);
            AdjustPacket a; // the volume set at the create (every skid packet has it)
            a.timeMs = p.t;
            a.index = c.index;
            a.kind = Adjust::Volume;
            track.adjusts.push_back(a);
            if (p.forgetMs >= 0)
            {
                EndPacket e;
                e.timeMs = p.t + static_cast<std::uint32_t>(p.forgetMs);
                e.index = c.index;
                e.reason = EndReason::StopAndForget;
                track.ends.push_back(e);
            }
            if (p.releaseMs >= 0)
            {
                EndPacket e;
                e.timeMs = p.t + static_cast<std::uint32_t>(p.releaseMs);
                e.index = c.index;
                e.reason = EndReason::Release;
                track.ends.push_back(e);
            }
        }
        std::stable_sort(track.adjusts.begin(), track.adjusts.end(),
            [](const AdjustPacket& a, const AdjustPacket& b) { return a.timeMs < b.timeMs; });
        std::stable_sort(track.ends.begin(), track.ends.end(),
            [](const EndPacket& a, const EndPacket& b) { return a.timeMs < b.timeMs; });
        return track;
    }

    owner::Cover SkidCover()
    {
        owner::Cover cover;
        for (const SkidRow& row : kSkidRows036)
            cover.Add(kHorse, row.ref, row.start, owner::Family::EntitySound);
        // The SNDS row of the deferred request (kind 0, one in the clip).
        cover.Add(kHorse, kSlideRef, 45525, owner::Family::SoundRequest);
        return cover;
    }

    owner::FootWindows SkidFeet()
    {
        owner::FootWindows feet;
        for (const auto& foot : kFeet036)
            feet.Add(kHorse, foot[0], foot[1]);
        feet.Seal();
        return feet;
    }

    // The engine side as sound_generic_player.cpp has it: 64 slots with the
    // entity sound track's policy (6 per owner, the owner's oldest gives its
    // slot), a handle = slot and generation, the census rows of the other
    // sources in an OwnCreates keyed by SoundKey (the hoof track remakes the
    // step packets near their foot events 16 ms later). `killEntry`: the
    // engine ends a sound of that entry right after its create (a sound
    // never heard).
    struct SlotEngine : Engine
    {
        std::uint64_t slots[64] = {};
        std::uint32_t gen[64] = {};
        std::uint32_t owners[64] = {};
        std::uint32_t started[64] = {};
        std::uint64_t refs[64] = {};
        int killEntry = -1;
        bool setOnlyKey = false; // the set-only key, for the comparison
        std::uint32_t nowMs = 0;
        std::uint32_t steals = 0;
        std::vector<std::uint64_t> stolenRefs;
        replay::census::OwnCreates own;
        std::uint64_t next = 1;

        std::uint64_t Key(std::uint64_t ref) const { return setOnlyKey ? (ref & 0xFFFFFFFFull) : owner::SoundKey(ref); }
        std::ptrdiff_t Current(std::uint64_t handle) const
        {
            const std::size_t slot = static_cast<std::size_t>(handle & 0xFFFFFFFFull);
            return handle == kNoHandle || slot >= 64 || (handle >> 32) != gen[slot] ? -1 : static_cast<std::ptrdiff_t>(slot);
        }
        bool Create(const CreatePacket& packet, bool, std::uint64_t& handle, std::string& status, bool& retry) override
        {
            retry = false;
            replay::soundslots::SlotView views[64];
            for (std::size_t n = 0; n < 64; ++n)
            {
                views[n].used = slots[n] != 0;
                views[n].owner = owners[n];
                views[n].distance = 10.0f;
                views[n].startedMs = started[n];
            }
            const replay::soundslots::Choice choice = replay::soundslots::Choose(views, 64, packet.owner, 10.0f);
            if (choice.slot < 0)
            {
                status = "full";
                retry = true;
                return false;
            }
            const std::size_t slot = static_cast<std::size_t>(choice.slot);
            if (choice.Steals())
            {
                ++steals;
                stolenRefs.push_back(refs[slot]);
            }
            slots[slot] = next++;
            owners[slot] = packet.owner;
            started[slot] = nowMs;
            refs[slot] = packet.rawRef;
            gen[slot] = (gen[slot] + 1) & 0x7FFFFFFFu;
            handle = static_cast<std::uint64_t>(slot) | (static_cast<std::uint64_t>(gen[slot]) << 32);
            status = "playing";
            return true;
        }
        bool Prepare(std::uint64_t) override { return true; }
        bool Play(std::uint64_t) override { return true; }
        void Stop(std::uint64_t handle, EndReason reason) override
        {
            const std::ptrdiff_t slot = Current(handle);
            if (slot >= 0 && reason != EndReason::Stop)
                slots[slot] = 0;
        }
        bool Alive(std::uint64_t handle) override
        {
            const std::ptrdiff_t slot = Current(handle);
            return slot >= 0 && slots[slot] != 0;
        }
        void Adjust(std::uint64_t, const AdjustPacket&) override {}
        bool MadeByOther(const CreatePacket& packet, std::uint32_t now) override
        {
            return own.Take(packet.owner, Key(packet.stableId ? packet.stableId : packet.rawRef), packet.timeMs, now, 300);
        }
        // The engine's audio update between ticks: the killed entry is gone.
        void Update()
        {
            if (killEntry < 0)
                return;
            for (std::size_t n = 0; n < 64; ++n)
                if (slots[n] && static_cast<int>(EntryOf(refs[n])) == killEntry)
                    slots[n] = 0;
        }
    };

    player::Config PacketsConfig()
    {
        player::Config c;
        c.packets = true;
        c.enabled = true;
        return c;
    }

    owner::Overrides PacketsOverrides()
    {
        owner::Overrides o;
        o.packets = true;
        return o;
    }

    // 44.8 s to 47.8 s at 60 ticks a second, the hoof track remaking the
    // step packets the plans leave to it.
    void PlaySkid(Player& player, SlotEngine& engine, const Track& track)
    {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> remakes; // (time, ref) the hoof track makes
        for (const SoundPlan& plan : player.Plans())
            if (plan.taker == owner::Taker::Specific && owner::StepFamily(plan.family))
                remakes.push_back({plan.startMs + 16, track.creates[plan.create].rawRef});
        std::size_t fed = 0;
        std::sort(remakes.begin(), remakes.end());
        for (std::uint32_t tick = 0; tick <= 181; ++tick)
        {
            const std::uint32_t now = 44800 + (tick * 50) / 3; // 16.7 ms
            engine.nowMs = now;
            engine.Update();
            while (fed < remakes.size() && remakes[fed].first <= now)
            {
                engine.own.Add(kHorse, engine.Key(remakes[fed].second), remakes[fed].first);
                ++fed;
            }
            player.Tick(now, true, engine);
        }
    }

    void TestSoundKey()
    {
        Check(owner::SoundKey(0x0080000e002f4361ull) == 0xe002f4361ull, "key: the skid's grain is its set and entry");
        Check(owner::SoundKey(0x0080000e002f4361ull) != owner::SoundKey(0x00800005002f4361ull),
            "key: a hoof step of the same surface set is another sound");
        Check(owner::SoundKey(kSlideRef) == 0x2b002f36cbull, "key: the skid's body");
        Check(owner::SoundKey(0x0000001000491933ull) == owner::SoundKey(0x0080001000491933ull),
            "key: the flag bit (another bank) is the same sound");
        Check(owner::SoundKey(0x000fffff000d1859ull) == 0x000d1859ull && owner::SoundKey(0xffffffff000cf6aeull) == 0x000cf6aeull,
            "key: a ref of another form keeps its low dword");
        Check(owner::PacketSoundKey(0xe002f4361ull, 0x0080000e002f4361ull) == owner::SoundKey(0x0080000e002f4361ull) &&
                owner::PacketSoundKey(0, 0x000fffff000d1859ull) == 0x000d1859ull,
            "key: a packet's stable id names the same sound as the census row's ref");
        Check(owner::SoundEntry(0xe002f4361ull) == 0x0E && owner::SoundEntry(0x2f4361ull) == -1 && owner::SoundEntry(0x000d1859ull) == -1,
            "key: the entry");
        Check(DoubleDetector::KeyOf(kHorse, 0x0080000e002f4361ull) == DoubleDetector::KeyOf(kHorse, 0xe002f4361ull) &&
                DoubleDetector::KeyOf(kHorse, 0x0080000e002f4361ull) != DoubleDetector::KeyOf(kHorse, 0x00800005002f4361ull) &&
                DoubleDetector::KeyOf(kHorse, 0x0080000e002f4361ull) != DoubleDetector::KeyOf(2, 0x0080000e002f4361ull),
            "key: the double detector keys owner, set and entry");
        // With the set alone as the key, the hoof layer of the 45.283 gallop
        // (0x800005002f36cb at 45.292, remade by the hoof track) would answer
        // for the skid's body at 45.525.
        replay::census::OwnCreates set;
        set.Add(kHorse, 0x00800005002f36cbull & 0xFFFFFFFFull, 45308);
        Check(set.Take(kHorse, kSlideRef & 0xFFFFFFFFull, 45525, 45530, 300), "key: the set alone, the body yields to a hoof step");
        replay::census::OwnCreates exact;
        exact.Add(kHorse, owner::SoundKey(0x00800005002f36cbull), 45308);
        Check(!exact.Take(kHorse, owner::SoundKey(kSlideRef), 45525, 45530, 300), "key: set and entry, it does not");
    }

    const SoundPlan* PlanOf(const Player& player, const Track& track, std::uint64_t ref, std::uint32_t t)
    {
        for (const SoundPlan& plan : player.Plans())
            if (track.creates[plan.create].rawRef == ref && plan.startMs == t)
                return &plan;
        return nullptr;
    }

    void TestSkid036()
    {
        const Track track = SkidTrack();
        Check(track.creates.size() == 95, "036: 95 packets of 0:1 from 44.8 s to 47.8 s");
        std::uint32_t grains = 0;
        for (const SkidPacket& p : kSkid036)
            grains += Grain(p) ? 1 : 0;
        Check(grains == 70, "036: the skid's 70 grains (entry 0x0E of four surface sets, one every 16 ms)");
        const owner::Cover cover = SkidCover();
        const owner::FootWindows feet = SkidFeet();
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 44800, &feet);
        std::uint32_t grainsGeneric = 0, hoofSpecific = 0, stepGeneric = 0;
        for (const SoundPlan& plan : player.Plans())
        {
            const CreatePacket& c = track.creates[plan.create];
            if (EntryOf(c.rawRef) == 0x0E && c.caller == 0x251688Du)
                grainsGeneric += plan.family == owner::Family::EntitySound && plan.taker == owner::Taker::Generic && plan.noHold ? 1 : 0;
            if (owner::StepFamily(plan.family))
            {
                hoofSpecific += plan.taker == owner::Taker::Specific ? 1 : 0;
                stepGeneric += plan.taker == owner::Taker::Generic ? 1 : 0;
            }
        }
        Check(grainsGeneric == 70, "036: the 70 grains are entity sounds the generic player plays at their time (their ESND rows)");
        Check(hoofSpecific == 18 && stepGeneric == 0,
            "036: the 18 hoof layers near the gallop and stop foot events stay with the hoof track (keyed by the set alone, 4 of them became entity sounds)");
        const SoundPlan* slide = PlanOf(player, track, kSlideRef, 45525);
        Check(slide && slide->family == owner::Family::SoundRequest && slide->taker == owner::Taker::Generic,
            "036: the skid's body is the SNDS row's (sound request), the generic player's");
        SlotEngine engine;
        PlaySkid(player, engine, track);
        std::uint32_t played = 0;
        for (const SoundPlan& plan : player.Plans())
        {
            const CreatePacket& c = track.creates[plan.create];
            if (EntryOf(c.rawRef) == 0x0E && c.caller == 0x251688Du)
                played += plan.lastOutcome == Outcome::Played ? 1 : 0;
        }
        Check(played == 70, "036: every grain plays");
        Check(slide && slide->lastOutcome == Outcome::Played, "036: the skid's body plays");
        bool slideStolen = false;
        for (const std::uint64_t ref : engine.stolenRefs)
            slideStolen = slideStolen || ref == kSlideRef;
        Check(!slideStolen, "036: the body keeps its slot to its release (6 per owner is not reached)");
        Check(player.Stats().endedAtOnce == 0, "036: nothing ended at once");
        Check(FormatTallySection(player.Tally()).empty() && TallyFindings(player.Tally()).empty(), "036: no sound fell short");

        // The same span with the set alone as the key: the hoof track's layer
        // of the 45.283 gallop answers for the body.
        Player before;
        before.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 44800, &feet);
        SlotEngine setOnly;
        setOnly.setOnlyKey = true;
        PlaySkid(before, setOnly, track);
        const SoundPlan* slideBefore = PlanOf(before, track, kSlideRef, 45525);
        Check(slideBefore && slideBefore->lastOutcome == Outcome::Yielded, "036: with the set as the key the body yields to a hoof step");
        Check(!FormatTallySection(before.Tally()).empty() || before.Tally().size() > 0, "036: the tally saw the yield");
    }

    void TestSkidNeverHeard()
    {
        // The census counts a create of the generic player as played: a skid
        // created and dropped by the engine at once must not read "ok".
        const Track track = SkidTrack();
        const owner::Cover cover = SkidCover();
        const owner::FootWindows feet = SkidFeet();
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 44800, &feet);
        SlotEngine engine;
        engine.killEntry = 0x0E;
        PlaySkid(player, engine, track);
        Check(player.Stats().endedAtOnce == 70, "never heard: the 70 grains ended by the engine at once");
        const std::string section = FormatTallySection(player.Tally());
        Check(Has(section, "== generic packets that fell short"), "never heard: the section");
        Check(Has(section, "SHORT  owner 0:1  ref 0x80000e002f4361  entity_sound  due 55, played 55, yielded 0, refused 0, missed 0, ended by the engine at once 55; from 45.7 s"),
            "never heard: the grain set 0x2f4361, 55 grains");
        const std::vector<replay::census::Finding> findings = TallyFindings(player.Tally());
        Check(findings.size() == 4, "never heard: one MISSING per grain sound (four sets at entry 0x0E)");
        replay::census::DiffResult diff;
        for (const replay::census::Finding& f : findings)
            diff.findings.push_back(f);
        Check(replay::census::FormatNotice(diff) == "Sound check: 4 missing (horse 0:1 at 0:45.5: 4). Details in ReplayDebug.",
            "never heard: the notice at Stop names the horse and the skid's start");
        Check(Has(FormatPlayLogLine(player.Stats(), "stop", 47800, 0, 0), ",off_step=0,ended_at_once=70,"), "never heard: the log line");
    }

    // State sounds: the engine ends a puppet's one-shot well before the
    // life it had live (a rider foley lived 41 to 51 ms of 820 to 1166).
    // Three grains of the horse, each ended by the engine 50 ms after its
    // create: one released 900 ms after its create (ended early), one
    // released after 150 ms (too short to judge) and one the game stopped
    // (stop and forget first, not a one-shot over by itself).
    struct ShortLifeEngine : SlotEngine
    {
        std::uint32_t lifeMs = 50;
        void Expire()
        {
            for (std::size_t n = 0; n < 64; ++n)
                if (slots[n] && nowMs >= started[n] + lifeMs)
                    slots[n] = 0;
        }
    };

    void TestEndedEarly()
    {
        const SkidPacket rows[] = {
            {1000, 0x80000e002f4361ull, 0x251688du, 0, -1, 900},
            {2000, 0x80000e002f5642ull, 0x251688du, 0, -1, 150},
            {3000, 0x80000e002f36cbull, 0x251688du, 0, 100, 700},
        };
        Track track;
        owner::Cover cover;
        for (const SkidPacket& p : rows)
        {
            CreatePacket c;
            c.timeMs = p.t;
            c.index = track.nextIndex++;
            const StableId id = StableIdOf(p.ref, false);
            c.stableId = id.id;
            c.idKind = id.kind;
            c.rawRef = p.ref;
            c.owner = kHorse;
            c.ownerClass = OwnerClass::Horse;
            c.source = static_cast<Source>(p.source);
            c.hasParams = true;
            c.caller = p.caller;
            track.creates.push_back(c);
            if (p.forgetMs >= 0)
            {
                EndPacket e;
                e.timeMs = p.t + static_cast<std::uint32_t>(p.forgetMs);
                e.index = c.index;
                e.reason = EndReason::StopAndForget;
                track.ends.push_back(e);
            }
            EndPacket e;
            e.timeMs = p.t + static_cast<std::uint32_t>(p.releaseMs);
            e.index = c.index;
            e.reason = EndReason::Release;
            track.ends.push_back(e);
            cover.Add(kHorse, p.ref, p.t, owner::Family::EntitySound);
        }
        std::stable_sort(track.ends.begin(), track.ends.end(),
            [](const EndPacket& a, const EndPacket& b) { return a.timeMs < b.timeMs; });
        owner::FootWindows feet;
        feet.Seal();
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 900, &feet);
        ShortLifeEngine engine;
        for (std::uint32_t tick = 0; tick <= 240; ++tick)
        {
            const std::uint32_t now = 900 + (tick * 50) / 3;
            engine.nowMs = now;
            engine.Expire();
            player.Tick(now, true, engine);
        }
        Check(player.Stats().played == 3, "ended early: the three grains played");
        Check(player.Stats().endedAtOnce == 0, "ended early: none ended before a later tick saw it");
        Check(player.Stats().endedEarly == 1, "ended early: only the one with 900 ms of recorded life");
        const std::string section = FormatTallySection(player.Tally());
        Check(Has(section, "SHORT  owner 0:1  ref 0x80000e002f4361  entity_sound") && Has(section, "; ended early 1\n"),
            "ended early: the tally row of the 900 ms grain");
        Check(!Has(section, "0x80000e002f5642") && !Has(section, "0x80000e002f36cb"),
            "ended early: the 150 ms grain and the stopped one are not rows");
        const std::vector<replay::census::Finding> findings = TallyFindings(player.Tally());
        Check(findings.size() == 1 && findings[0].played == 0.0, "ended early: one MISSING, nothing heard of it");
        Check(Has(FormatPlayLogLine(player.Stats(), "stop", 4900, 0, 0), ",ended_at_once=0,ended_early=1,"),
            "ended early: the log line");

        Player whole;
        whole.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 900, &feet);
        ShortLifeEngine lasting;
        lasting.lifeMs = 2000;
        for (std::uint32_t tick = 0; tick <= 240; ++tick)
        {
            const std::uint32_t now = 900 + (tick * 50) / 3;
            lasting.nowMs = now;
            lasting.Expire();
            whole.Tick(now, true, lasting);
        }
        Check(whole.Stats().endedEarly == 0 && FormatTallySection(whole.Tally()).empty(),
            "ended early: sounds that live their recorded life are not flagged");
        Check(replay::soundgeneric::player::EndedEarly(900, 449) && !replay::soundgeneric::player::EndedEarly(900, 450) &&
                !replay::soundgeneric::player::EndedEarly(199, 10),
            "ended early: under half of a life of 200 ms or more");
    }

    void TestOffStep()
    {
        // A sound of the foot system with no foot event near it (a slide
        // scrape of the step function) is the generic player's in packets
        // mode; legacy and an ini taker keep the track.
        Track track;
        CreatePacket c;
        c.timeMs = 46500;
        c.index = track.nextIndex++;
        c.stableId = 0x2a002f4361ull;
        c.idKind = IdKind::RefSet;
        c.rawRef = 0x80002a002f4361ull;
        c.owner = kHorse;
        c.ownerClass = OwnerClass::Horse;
        c.caller = 0x318E828u; // the step's main sound
        track.creates.push_back(c);
        c.timeMs = 45300; // the hoof of the 45.283 gallop
        c.index = track.nextIndex++;
        track.creates.push_back(c);
        const owner::FootWindows feet = SkidFeet();
        Check(!feet.Covers(kHorse, 46500) && feet.Covers(kHorse, 45300) && feet.Covers(kHorse, 45283 + 430 + 150) &&
                !feet.Covers(kHorse, 45283 + 430 + 151) && feet.Covers(kHorse, 47357 - 100) && !feet.Covers(2, 45300),
            "feet: the window of a foot event, 100 ms before to 150 ms after its delay");
        Player player;
        player.Begin(track, PacketsConfig(), owner::Cover(), PacketsOverrides(), 0, &feet);
        const SoundPlan* away = nullptr;
        const SoundPlan* near = nullptr;
        for (const SoundPlan& plan : player.Plans())
            (plan.startMs == 46500 ? away : near) = &plan;
        Check(away && away->family == owner::Family::Footstep && away->taker == owner::Taker::Generic && away->offStep && away->noHold,
            "off step: away from a foot event, the generic player's");
        Check(near && near->taker == owner::Taker::Specific && !near->offStep, "off step: at a foot event, the hoof track's");
        Check(player.Stats().offStep == 1 && Has(FormatSection(player.Stats(), PacketsOverrides(), ""), "given to this player: 1."),
            "off step: counted in the report");
        Player legacy;
        player::Config lc = PacketsConfig();
        lc.packets = false;
        legacy.Begin(track, lc, owner::Cover(), owner::Overrides(), 0, &feet);
        Check(legacy.Plans()[1].taker == owner::Taker::Specific && legacy.Stats().offStep == 0, "off step: legacy keeps the track");
        owner::Overrides ini = owner::ParseOverrides("footstep:specific");
        ini.packets = true;
        Player pinned;
        pinned.Begin(track, PacketsConfig(), owner::Cover(), ini, 0, &feet);
        Check(pinned.Stats().offStep == 0, "off step: footstep:specific in the ini keeps every step with the track");
        Player noFeet;
        noFeet.Begin(track, PacketsConfig(), owner::Cover(), PacketsOverrides(), 0, nullptr);
        Check(noFeet.Stats().offStep == 2, "off step: with no foot event recorded every step packet is the generic player's");
    }

    void TestPacketLives()
    {
        using owner::LifeVerdict;
        owner::PacketLives lives;
        // 26:209's loop 0x80001d002c0750 from 44.985 to its release at 54.062.
        const std::uint32_t k26 = (26u << 16) | 209u, k23 = (23u << 16) | 203u, k2 = (2u << 16) | 3u;
        lives.Add(k26, 0x1d002c0750ull, 0x80001d002c0750ull, 44985, 54062);
        // 23:203's loop 0x80001d002c0750 from 42.624 to 70.026.
        lives.Add(k23, 0x1d002c0750ull, 0x80001d002c0750ull, 42624, 70026);
        // a sound with no stable id (raw form) and one never released
        lives.Add(k2, 0, 0x000fffff000d4598ull, 1000, 0xFFFFFFFFu);
        lives.Add(replay::census::kNoOwner, 0x30012345ull, 0x80000300012345ull, 100, 200); // no owner: not kept
        lives.Seal();
        Check(lives.Size() == 3, "lives: owned packets");
        Check(lives.Judge(k26, 0x80001d002c0750ull, 50000) == LifeVerdict::Recorded, "lives: 26:209 asking again inside the recorded life");
        Check(lives.Judge(k26, 0x80001d002c0750ull, 43600) == LifeVerdict::Recorded,
            "lives: 26:209 asking 1.4 s before the recorded start (the recording starts it then)");
        Check(lives.Judge(k26, 0x80001d002c0750ull, 41000) == LifeVerdict::Passed, "lives: 4 s before, nothing recorded yet");
        Check(lives.Judge(k26, 0x80001d002c0750ull, 54462) == LifeVerdict::Recorded &&
                lives.Judge(k26, 0x80001d002c0750ull, 54463) == LifeVerdict::Passed,
            "lives: 400 ms after the release");
        Check(lives.Judge(k23, 0x80001d002c363cull, 50000) == LifeVerdict::Alternative,
            "lives: 23:203 asks another set at entry 0x1D while its recorded 0x2c0750 plays (proved double)");
        Check(lives.Judge(k23, 0x80001d002c363cull, 31500) == LifeVerdict::Passed,
            "lives: the same before the recorded loop (31.5 s): no packet, left to the puppet");
        Check(lives.Judge(k23, 0x800003002c363cull, 50000) == LifeVerdict::Passed, "lives: another entry is not the recorded slot");
        Check(lives.Judge(k2, 0x000fffff000d4598ull, 150000) == LifeVerdict::Recorded, "lives: a sound never released lives to the end");
        Check(lives.Judge(k2, 0x800003000da07full, 5000) == LifeVerdict::Passed, "lives: 2:3's rider loop 0xda07f, never recorded");
        Check(lives.Judge(replay::census::kNoOwner, 0x80000300012345ull, 150) == LifeVerdict::Passed, "lives: no owner");
        owner::PacketLives unsealed;
        unsealed.Add(k26, 0x1d002c0750ull, 0x80001d002c0750ull, 44985, 54062);
        Check(unsealed.Judge(k26, 0x80001d002c0750ull, 50000) == LifeVerdict::Passed, "lives: not sealed, nothing judged");
    }

    void TestBlockedRuns()
    {
        using owner::LifeVerdict;
        BlockedTally tally;
        const std::uint32_t k26 = (26u << 16) | 209u, k23 = (23u << 16) | 203u, k2 = (2u << 16) | 3u;
        // 26:209 asks for its loop every frame from 43.6 s to 53.0 s: 565 attempts, one run.
        for (std::uint32_t t = 43600; t <= 53000; t += 17)
            tally.Note(k26, 0x80001d002c0750ull, 0x2588C2F, 5, t, LifeVerdict::Recorded, true);
        // 23:203: another set while its loop plays: refused.
        for (std::uint32_t t = 43400; t <= 44400; t += 17)
            tally.Note(k23, 0x80001d002c40b9ull, 0x2588C2F, 5, t, LifeVerdict::Alternative, true);
        // 2:3: the rider's loop, no packet, passed, two runs.
        for (std::uint32_t t = 0; t <= 1000; t += 17)
            tally.Note(k2, 0x800003000da07full, 0x2588C2F, 5, t, LifeVerdict::Passed, false);
        for (std::uint32_t t = 9000; t <= 9100; t += 17)
            tally.Note(k2, 0x800003000da07full, 0x2588C2F, 5, t, LifeVerdict::Passed, false);
        // an own one-shot at S03 (always refused, whatever the verdict) and the hidden player
        tally.Note(k2, 0x000fffff000d4598ull, 0x2588996, 3, 500, LifeVerdict::Passed, true);
        tally.Note(0xFFFFFFFFu, 0x800005000da07full, 0x2588996, 3, 500, LifeVerdict::Passed, true);
        Check(tally.Total() == 553 + 59 + 65 + 1 + 1 && tally.Hidden() == 1, "blocked: attempts");
        Check(tally.ByVerdict(LifeVerdict::Recorded) == 553 && tally.ByVerdict(LifeVerdict::Alternative) == 59 &&
                tally.WithoutPacket() == 66 && tally.RunsWithoutPacket() == 3,
            "blocked: verdicts and runs (a run is broken by 250 ms without an attempt)");
        Check(tally.Refused() == 553 + 59 + 1 + 1 && tally.Passed() == 65, "blocked: refused and passed");
        const std::string section = FormatBlockedSection(tally, true);
        Check(Has(section, "Attempts 679 (a persistent wrapper asks again every frame while refused): refused 614, passed 65."),
            "blocked: the attempts line");
        Check(Has(section, "without a packet 66 in 3 runs (left to the puppet, except S03)"), "blocked: the runs");
        Check(Has(section, "NO PACKET  owner 2:3  sound 0x3000da07f  attempts 65 in 2 runs, refused 0  (S05, ref 0x800003000da07f at 0x2588c2f)  0.0 s to 9.0 s"),
            "blocked: the passed row");
        Check(Has(section, "ALTERNATE  owner 23:203  sound 0x1d002c40b9  attempts 59 in 1 runs, refused 59"), "blocked: the alternative row");
        Check(Has(section, "recorded   owner 26:209  sound 0x1d002c0750  attempts 553 in 1 runs, refused 553"), "blocked: the recorded row");
        Check(section.find("NO PACKET") < section.find("ALTERNATE") && section.find("ALTERNATE") < section.find("recorded   owner"),
            "blocked: no packet first");
        Check(BlockedLogLine(tally) ==
                "SOUND_GENERIC_BLOCKED_SUMMARY,block=packet,attempts=679,refused=614,passed=65,recorded=553,alternative=59,no_packet=66,"
                "no_packet_runs=3,hidden_player=1,s03=2,s04=0,s05=677,s06=0,step_unmade=0,unrecorded=0,s01=0,s02=0",
            "blocked: the log line");
        Check(Has(FormatBlockedSection(tally, true, 40, true), "SoundGenericBlock=all") &&
                Has(BlockedLogLine(tally, true), "block=all"),
            "blocked: SoundGenericBlock=all is named");
        Check(FormatBlockedSection(tally, false).empty(), "blocked: legacy, no section");
    }

    void TestKeysAndTrace()
    {
        Check(BlockAllOfText("all") && BlockAllOfText(" ALL\r\n") && !BlockAllOfText("packet") && !BlockAllOfText("") &&
                !BlockAllOfText(nullptr),
            "block: all in any case; packet, empty and unknown are packet");
        replay::soundgeneric::Config config;
        Check(!config.blockAll && config.trace.empty(), "block: packet and no trace by default");
        const TraceWindow w = ParseTraceWindow(" 0:1@45000-48000 ");
        Check(w.on && !w.anyOwner && w.owner == kHorse && w.fromMs == 45000 && w.toMs == 48000, "trace: owner and span");
        Check(w.Holds(kHorse, 45000) && w.Holds(kHorse, 48000) && !w.Holds(kHorse, 48001) && !w.Holds(2, 46000), "trace: the window");
        const TraceWindow any = ParseTraceWindow("*@0-100");
        Check(any.on && any.anyOwner && any.Holds(12345, 50), "trace: every owner");
        Check(!ParseTraceWindow("").on && !ParseTraceWindow("0:1").on && !ParseTraceWindow("0:1@48000-45000").on &&
                !ParseTraceWindow("x@1-2").on && !ParseTraceWindow("0:1@1-2x").on,
            "trace: a bad key is off");
        const Track track = SkidTrack();
        const owner::Cover cover = SkidCover();
        const owner::FootWindows feet = SkidFeet();
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 44800, &feet, ParseTraceWindow("0:1@45520-45560"));
        SlotEngine engine;
        PlaySkid(player, engine, track);
        const std::vector<std::string> lines = player.TakeTrace();
        bool slidePlayed = false, grainForget = false;
        for (const std::string& line : lines)
        {
            slidePlayed = slidePlayed || (Has(line, "t=45525,owner=0:1,ref=0x80002b002f36cb,family=sound_request,taker=generic,off_step=0,no_hold=1,event=played"));
            grainForget = grainForget || (Has(line, "t=45541,owner=0:1,ref=0x80000e002f36cb,family=entity_sound") && Has(line, "event=forget"));
        }
        Check(slidePlayed && grainForget && lines.size() >= 4 && Has(lines.front(), "SOUND_GENERIC_TRACE,playhead_ms="),
            "trace: the body played and the first grain's forget, one line per event");
        Check(player.TakeTrace().empty(), "trace: taken once");
    }

    // ---- init variables ----

    // A params copy (0x140 bytes) with the init variables at +0x10 and the count at +0x90.
    std::vector<std::uint8_t> ParamsWith(std::int32_t count, const Variable* vars, std::size_t n)
    {
        std::vector<std::uint8_t> params(kParamsBytes, 0);
        for (std::size_t i = 0; i < n; ++i)
        {
            std::memcpy(params.data() + 0x10 + i * 8, &vars[i].hash, 4);
            std::memcpy(params.data() + 0x14 + i * 8, &vars[i].value, 4);
        }
        std::memcpy(params.data() + 0x90, &count, 4);
        return params;
    }

    void TestInitVariables()
    {
        // The grain of 45.541 s (ESND row of 036): the switch, the speed and the third one.
        const Variable grain[3] = {{0x833ff9c9u, 1.0f}, {0x48fe569du, 4.686f}, {0x41f750d5u, 0.0f}};
        std::vector<std::uint8_t> params = ParamsWith(3, grain, 3);
        const InitParams read = ReadInitParams(params.data());
        Check(read.initVariableCount == 3 && read.initVariables[1].hash == 0x48fe569du && read.initVariables[1].value == 4.686f,
            "init variables: +0x10 pairs, count at +0x90");
        Check(read.variableCount == 0, "init variables: the +0x10C dword list is another thing (count +0x124, zero here)");
        Variable withNan[3] = {{0x833ff9c9u, 1.0f}, {0x48fe569du, std::numeric_limits<float>::quiet_NaN()}, {0x41f750d5u, 0.0f}};
        params = ParamsWith(3, withNan, 3);
        const InitParams nan = ReadInitParams(params.data());
        Check(nan.initVariableCount == 2 && nan.initVariables[1].hash == 0x41f750d5u, "init variables: a value that is not finite is left out");
        params = ParamsWith(40, grain, 3);
        InitParams many;
        ReadInitVariables(params.data(), params.size(), many);
        Check(many.initVariableCount == 3, "init variables: a count past 16 reads 16 slots (zero hashes left out)");
        params = ParamsWith(-1, grain, 3);
        InitParams none;
        ReadInitVariables(params.data(), params.size(), none);
        Check(none.initVariableCount == 0, "init variables: a negative count is none");
        ReadInitVariables(params.data(), 0x90, none);
        Check(none.initVariableCount == 0, "init variables: a copy too short is none");

        // The index of an older clip: the ESND row of the grain and the SNDS row of the body.
        InitVariableIndex index;
        params = ParamsWith(3, grain, 3);
        index.Add(kHorse, 0x80000e002f36cbull, 45541, params.data(), params.size(), 2);
        const Variable body[2] = {{0x833ff9c9u, 1.0f}, {0x41f750d5u, 0.0f}};
        const std::vector<std::uint8_t> bodyParams = ParamsWith(2, body, 2);
        index.Add(kHorse, kSlideRef, 45525, bodyParams.data(), bodyParams.size(), 1);
        const std::vector<std::uint8_t> empty = ParamsWith(0, grain, 0);
        index.Add(kHorse, 0x800005002f36cbull, 45292, empty.data(), empty.size(), 2); // a hoof step without variables: no row
        index.Seal();
        // A row with the value dwords counts too (the hoof's params here are not constructed: they differ).
        Check(index.Size() == 3, "index: rows with variables or value dwords");
        InitParams out;
        std::uint8_t source = 0;
        Check(index.Find(kHorse, kSlideRef, 45525, 25, out, source) && source == 1 && out.initVariableCount == 2 &&
                out.initVariables[0].hash == 0x833ff9c9u && out.initVariables[0].value == 1.0f,
            "index: the body finds its request's variables");
        Check(index.Find(kHorse, 0x80000e002f36cbull, 45560, 25, out, source) && source == 2 && out.initVariables[1].value == 4.686f,
            "index: a grain 19 ms off finds its row");
        Check(!index.Find(kHorse, 0x80000e002f36cbull, 45600, 25, out, source), "index: 59 ms off is too far");
        Check(!index.Find(2, kSlideRef, 45525, 25, out, source) && !index.Find(kHorse, 0x80000e002f4361ull, 45541, 25, out, source),
            "index: another owner or another sound finds nothing");
    }

    // ---- value dwords ----

    void TestValueDwords()
    {
        std::uint8_t ctor[kParamsBytes];
        ConstructorParams(ctor);
        InitParams none;
        ReadValueDwords(ctor, sizeof(ctor), none);
        Check(none.valuesKnown && none.valueCount == 0, "values: constructed params differ in nothing");
        Check(ValueDwordOffset(0) == 0x94 && ValueDwordOffset(12) == 0xC4 && ValueDwordOffset(13) == 0xE8 &&
                ValueDwordOffset(kValueDwords - 1) == 0x13C,
            "values: 35 dwords from 0x94 to 0x13C, the pointers 0xC8 to 0xE8 skipped");
        Check(!IsValueDwordIndex(0xC8 / 4) && !IsValueDwordIndex(0xE0 / 4) && !IsValueDwordIndex(0x90 / 4) &&
                IsValueDwordIndex(0xF8 / 4) && IsValueDwordIndex(0x13C / 4) && !IsValueDwordIndex(0x140 / 4),
            "values: only value offsets are taken");
        // The skid's body (SNDS row of 036 at 45.525 s): the client variable
        // 0xEE56493E (0x3197E3F), the update flag 0x04, the tail, and 0x13C.
        std::uint8_t body[kParamsBytes];
        std::memcpy(body, ctor, sizeof(body));
        const std::uint32_t client = 0xee56493eu, tail = 0x3e8e0000u, word13c = 0x3d23d70au;
        std::memcpy(body + 0xF8, &client, 4);
        std::memcpy(body + 0x12C, &tail, 4);
        std::memcpy(body + 0x13C, &word13c, 4);
        body[0x13A] = 0x24;
        body[0x12C] = 0x34; // the params' handle: not a value
        const std::uint64_t env = 0x1b1811ebe10ull;
        std::memcpy(body + 0xE0, &env, 8);
        const InitParams read = ReadInitParams(body);
        Check(read.valuesKnown && read.valueCount == 4 && ValueAt(read, kParamsClientVariable) == client &&
                ValueAt(read, 0x12C) == tail && read.hasEnvGroup && (read.flags & kParamsFlagUpdateEntity) != 0,
            "values: the body's client variable, tail, 0x13C and flags; the group seen");
        Check(ValueAt(read, 0x104) == 20000u && ValueAt(none, kParamsClientVariable) == 0, "values: an unlisted dword is the constructor's");
        // Written over constructed params: the game's params back, less the handle and the group.
        std::uint8_t replayed[kParamsBytes];
        ConstructorParams(replayed);
        replayed[0x12C] = 0x77;
        ApplyValueDwords(replayed, read);
        Check(std::memcmp(replayed + 0x94, body + 0x94, 0xC8 - 0x94) == 0 && std::memcmp(replayed + 0xE8, body + 0xE8, 0x12C - 0xE8) == 0 &&
                std::memcmp(replayed + 0x12E, body + 0x12E, kParamsBytes - 0x12E) == 0 && replayed[0x12C] == 0x77,
            "values: applied, the value fields are the game's and the handle stays");
        InitParams bad = read;
        bad.values[0].index = 0xE0 / 4;
        std::uint8_t guarded[kParamsBytes];
        ConstructorParams(guarded);
        ApplyValueDwords(guarded, bad);
        std::uint64_t group = 0;
        std::memcpy(&group, guarded + 0xE0, 8);
        Check(group == 0, "values: a pointer offset is never written");
        // An older clip's packet finds them in the request's row.
        InitVariableIndex index;
        index.Add(kHorse, kSlideRef, 45525, body, sizeof(body), 1);
        index.Seal();
        InitParams out;
        std::uint8_t source = 0;
        Check(index.Find(kHorse, kSlideRef, 45526, 25, out, source) && out.valuesKnown && out.valueCount == 4 &&
                ValueAt(out, kParamsClientVariable) == client,
            "values: the row gives an older packet the client variable");
    }

    struct VerdictEngine : SlotEngine
    {
        const char* AdjustResult() const override { return "absent"; }
        std::string CreateDetail(std::uint64_t) const override { return "init_vars=3(entity_sound)"; }
    };

    void TestAdjustTrace()
    {
        Track track = SkidTrack();
        // The grain of 45.558 s: its recorded S38 (speed 5.539 at 45.574 s).
        for (const CreatePacket& c : track.creates)
        {
            if (c.timeMs != 45558 || EntryOf(c.rawRef) != 0x0E)
                continue;
            AdjustPacket a;
            a.timeMs = 45574;
            a.index = c.index;
            a.kind = Adjust::Variable;
            a.hash = 0x48fe569du;
            a.value[0] = 5.539f;
            track.adjusts.push_back(a);
            break;
        }
        std::stable_sort(track.adjusts.begin(), track.adjusts.end(),
            [](const AdjustPacket& a, const AdjustPacket& b) { return a.timeMs < b.timeMs; });
        const owner::Cover cover = SkidCover();
        const owner::FootWindows feet = SkidFeet();
        Player player;
        player.Begin(track, PacketsConfig(), cover, PacketsOverrides(), 44800, &feet, ParseTraceWindow("0:1@45550-45560"));
        VerdictEngine engine;
        PlaySkid(player, engine, track);
        bool variable = false, volume = false, create = false;
        for (const std::string& line : player.TakeTrace())
        {
            variable = variable || (Has(line, "t=45558,owner=0:1,ref=0x80000e002f36cb") &&
                Has(line, "event=adjust,detail=S38:0x48fe569d=5.539@45574:absent"));
            volume = volume || Has(line, "event=adjust,detail=S18:0.000@45558:absent");
            create = create || (Has(line, "t=45558,") && Has(line, "event=create,detail=init_vars=3(entity_sound)"));
        }
        Check(variable, "trace: an S38 adjust with its hash, value, recorded time and the port's verdict");
        Check(volume, "trace: every adjust kind is traced (S18)");
        Check(create, "trace: the create line says what init variables the sound had");
        Check(TraceWindow{}.maxLines == 2000, "trace: 2000 lines (the adjusts count too)");
    }

    // The puppet horses' extra hoof set 0x0d167c inside a replayed step.
    void TestStepUnmadeSets()
    {
        using owner::LifeVerdict;
        owner::PacketLives lives;
        const std::uint32_t k6 = (6u << 16) | 716u, k1 = (1u << 16) | 711u, k99 = (99u << 16) | 999u;
        // 6:716 made its main step 0x2f7034 (entries 0x2a and 0x2b) live, never 0x0d167c.
        lives.Add(k6, 0x2a002f7034ull, 0x80002a002f7034ull, 1000, 1200);
        lives.Add(k6, 0x2b002f7034ull, 0x80002b002f7034ull, 1500, 1700);
        // 1:711 made 0x0d167c at entry 1 once.
        lives.Add(k1, 0x01000d167cull, 0x800001000d167cull, 3000, 3100);
        lives.Seal();
        Check(lives.OwnerHasPackets(k6) && lives.OwnerHasPackets(k1) && !lives.OwnerHasPackets(k99), "unmade: owners with packets");
        Check(lives.OwnerMadeSet(k6, 0x80002c002f7034ull), "unmade: another entry of a set the owner made counts (any entry, any time)");
        Check(!lives.OwnerMadeSet(k6, 0x800001000d167cull) && !lives.OwnerMadeSet(k6, 0x800006000d167cull),
            "unmade: 6:716 never made 0x0d167c");
        Check(lives.OwnerMadeSet(k1, 0x800000000d167cull), "unmade: 1:711 made 0x0d167c (entry 1), entry 0 counts");
        Check(!lives.OwnerMadeSet(replay::census::kNoOwner, 0x800001000d167cull), "unmade: no owner");
        using replay::puppetaudio::RefuseUnmadeReplayedSet;
        const auto refuse = [&](bool gate, bool puppet, bool inside, std::uint32_t who, std::uint64_t ref) {
            return RefuseUnmadeReplayedSet(gate, puppet, inside, lives.OwnerHasPackets(who), lives.OwnerMadeSet(who, ref));
        };
        Check(refuse(true, true, true, k6, 0x800001000d167cull), "unmade: 6:716's 0x0d167c in a replayed step is refused");
        Check(!refuse(true, true, true, k6, 0x80002a002f7034ull), "unmade: its recorded main step passes");
        Check(!refuse(true, true, true, k1, 0x800001000d167cull), "unmade: 1:711 made it live, it passes");
        Check(!refuse(true, true, true, k99, 0x800001000d167cull), "unmade: an owner the clip has no packet for is left alone");
        Check(!refuse(false, true, true, k6, 0x800001000d167cull), "unmade: SoundStepSetGate=0");
        Check(!refuse(true, true, false, k6, 0x800001000d167cull), "unmade: outside a replayed call (other rules decide)");
        Check(!refuse(true, false, true, k6, 0x800001000d167cull), "unmade: not a puppet's audio");
        BlockedTally tally;
        tally.Note(k6, 0x800001000d167cull, 0x318DEDE, 3, 2000, LifeVerdict::Unmade, true);
        tally.Note(k6, 0x800001000d167cull, 0x318DEDE, 3, 2100, LifeVerdict::Unmade, true);
        tally.Note(k6, 0x800003000da07full, 0x2588C2F, 5, 2000, LifeVerdict::Passed, false);
        Check(tally.ByVerdict(LifeVerdict::Unmade) == 2 && tally.WithoutPacket() == 1 && tally.RunsWithoutPacket() == 1,
            "unmade: tally");
        const std::string section = FormatBlockedSection(tally, true);
        Check(Has(section, "a set the owner never made in the clip (refused, SoundStepSetGate) 2."), "unmade: section count");
        Check(Has(section, "STEP UNMADE  owner 6:716  sound 0x1000d167c  attempts 2 in 1 runs, refused 2  (S03"),
            "unmade: section row");
        Check(section.find("NO PACKET") < section.find("STEP UNMADE"), "unmade: after the rows without a packet");
        Check(Has(BlockedLogLine(tally), ",step_unmade=2"), "unmade: log line");
    }
}

int RunSkidHoofSoundTests()
{
    g_failuresSkid = 0;
    TestSoundKey();
    TestSkid036();
    TestSkidNeverHeard();
    TestEndedEarly();
    TestOffStep();
    TestPacketLives();
    TestBlockedRuns();
    TestStepUnmadeSets();
    TestKeysAndTrace();
    TestInitVariables();
    TestAdjustTrace();
    TestValueDwords();
    if (g_failuresSkid == 0)
        std::puts("Skid and hoof (init variables, value fields) tests passed.");
    return g_failuresSkid;
}
