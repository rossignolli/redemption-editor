// Missing world sounds (sound_world_core.h): the steal line, the yield
// watch, the particle audio slots' transitions, the horse group line, the
// budgets, the probe's code sites and the anti-double table's live twin.
#include "sound_world_core.h"
#include "sound_check_core.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace replay::soundworld;

    int g_worldFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_worldFailures;
    }

    bool Has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

    void TestSteal()
    {
        StealView s;
        s.playheadMs = 10300;
        s.reason = "world_cap";
        s.capKey = (1ull << 63) | 0x4c4a0c;
        s.victimOwner = 0xFFFF0000u;
        s.victimRef = 0xd9388;
        s.victimFamily = "world";
        s.victimLong = true;
        s.victimStartedMs = 5314;
        s.victimLifeMs = 22095; // recorded 5314 -> 27409
        s.victimDistanceM = 3.2f;
        s.newOwner = 0xFFFF0000u;
        s.newRef = 0x4c4a0c;
        s.newFamily = "world";
        s.newDistanceM = 8.0f;
        const std::string line = FormatSteal(s);
        Check(Has(line, "SOUND_GENERIC_STEAL,playhead_ms=10300,reason=world_cap,cap_key=0x80000000004c4a0c,"),
            "steal: head and cap key");
        Check(Has(line, "victim_owner=65535:0,victim_ref=0xd9388,victim_family=world,victim_long=1,victim_age_ms=4986,"
                        "victim_left_ms=17109,"),
            "steal: the victim, its age and the recorded life it still had");
        Check(Has(line, "new_owner=65535:0,new_ref=0x4c4a0c,new_family=world,new_distance_m=8.0"), "steal: the newcomer");
        s.victimLifeMs = kNeverMs;
        Check(VictimLeftMs(s) == -1, "steal: a loop has no recorded end");
        s.victimLifeMs = 1000;
        Check(VictimLeftMs(s) == 0, "steal: a sound past its recorded end has nothing left");
    }

    void TestYieldWatch()
    {
        YieldWatch w;
        w.Note(1, 0x47fd3f, 0, true, 30000, 60000);
        w.Note(2, 0x47fd3f, 0x1111, false, 30000, 60000);
        Check(w.Size() == 0 && w.Counters().untracked == 2, "yield: no twin or a twin out of the live list is untracked");
        w.Note(3, 0x47fd3f, 0xAAAA, true, 30000, 60000);
        w.Note(3, 0x47fd3f, 0xAAAA, true, 30050, 60000); // the same packet again
        w.Note(4, 0x480219, 0xBBBB, true, 30000, 37500);
        w.Note(5, 0x47e61f, 0xCCCC, true, 30000, kNeverMs);
        Check(w.Size() == 3 && w.Counters().tracked == 3, "yield: tracked once per packet");
        std::set<std::uint64_t> alive{0xAAAA, 0xBBBB, 0xCCCC};
        auto isAlive = [&alive](std::uint64_t sound) { return alive.count(sound) != 0; };
        std::vector<LostYield> lost;
        w.Check(36000, isAlive, lost);
        Check(lost.empty() && w.Size() == 3, "yield: twins alive, nothing lost");
        // The 37 s cut: every live twin ends.
        alive.clear();
        w.Check(37100, isAlive, lost);
        Check(lost.size() == 2 && w.Counters().lost == 2 && w.Counters().outlived == 1,
            "yield: two twins ended early, one within the margin of its copy's end");
        Check(w.Size() == 0, "yield: each reported once");
        bool loop = false, zone = false;
        for (const LostYield& l : lost)
        {
            loop = loop || (l.index == 5 && l.endMs == kNeverMs);
            zone = zone || (l.index == 3 && l.endMs == 60000 && l.lostMs == 37100);
        }
        Check(loop && zone, "yield: the lost copies are named");
        const std::string line = FormatLostYield(lost[0]);
        Check(Has(line, "SOUND_GENERIC_YIELD_LOST,playhead_ms=37100,index=3,ref=0x47fd3f,live_sound=0xaaaa,yield_ms=30000,"
                        "recorded_end_ms=60000,left_ms=22900"),
            "yield: the lost line");
        // The copy's own end comes first: outlived, not lost.
        YieldWatch v;
        v.Note(7, 0x1, 0xDDDD, true, 1000, 5000);
        std::vector<LostYield> none;
        v.Check(5200, [](std::uint64_t) { return true; }, none);
        Check(none.empty() && v.Size() == 0 && v.Counters().outlived == 1, "yield: past the copy's end it is let go");
        // Full and cleared.
        YieldWatch f;
        for (std::uint32_t n = 0; n < kYieldWatchMax + 3; ++n)
            f.Note(n, 0x2, 0x100 + n, true, 0, 9000);
        Check(f.Size() == kYieldWatchMax && f.Counters().dropped == 3, "yield: a full watch drops and counts");
        f.Clear();
        Check(f.Size() == 0 && f.Counters().cleared == kYieldWatchMax, "yield: a jump clears and counts");
    }

    void TestPtfxWatch()
    {
        PtfxWatch w;
        std::vector<PtfxSlot> slots(kPtfxSlotCount);
        std::vector<PtfxChange> out;
        slots[5].active = true; // already playing at the pass start
        slots[5].settings = 0x1000;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.empty(), "ptfx: the first snapshot is the baseline");
        // The 33 s one-shot: start, then its sound.
        slots[9].active = true;
        slots[9].settings = 0x2000;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.size() == 1 && out[0].slot == 9 && out[0].event == PtfxEvent::Start, "ptfx: a slot starts");
        out.clear();
        slots[9].sound = 0xABC;
        slots[9].done = true;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.size() == 1 && out[0].event == PtfxEvent::Created, "ptfx: its sound appears (done set with it: not far)");
        out.clear();
        slots[9].sound = 0; // the one-shot ends: done stays, no far
        w.Diff(slots.data(), slots.size(), out);
        Check(out.empty(), "ptfx: a created sound that ends is not a far refusal");
        // The 37 s one: started and marked done with no sound (the listener far).
        slots[12].active = true;
        slots[12].settings = 0x2000;
        slots[12].done = true;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.size() == 2 && out[0].event == PtfxEvent::Start && out[1].event == PtfxEvent::Far && !out[1].hadSound,
            "ptfx: done without a sound is the far path");
        out.clear();
        // A far stop: a sound it had, gone, done set.
        slots[20].active = true;
        slots[20].settings = 0x3000;
        slots[20].sound = 0x777;
        w.Diff(slots.data(), slots.size(), out);
        out.clear();
        slots[20].sound = 0;
        slots[20].done = true;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.size() == 1 && out[0].event == PtfxEvent::Far && out[0].hadSound, "ptfx: a far stop says it had a sound");
        out.clear();
        // Other settings in a busy slot, then the slot ends.
        slots[5].settings = 0x1100;
        slots[9].active = false;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.size() == 2 && out[0].slot == 5 && out[0].event == PtfxEvent::Start && out[1].slot == 9 &&
                out[1].event == PtfxEvent::Ended,
            "ptfx: new settings start again, an inactive slot ends");
        out.clear();
        w.Reset();
        slots[30].active = true;
        w.Diff(slots.data(), slots.size(), out);
        Check(out.empty(), "ptfx: a reset is a new baseline");
        Check(std::string(PtfxEventName(PtfxEvent::Far)) == "done_no_sound", "ptfx: event names");
    }

    void TestGroup()
    {
        GroupView g;
        g.group = 0;
        g.members = 6;
        g.nonNull = 6;
        g.replay = 6;
        g.gaits[2] = 3;
        g.gaits[3] = 3;
        g.speed = 7.8f;
        g.pos[0] = -1984.5f;
        g.pos[1] = 1911.3f;
        g.pos[2] = 261.4f;
        g.sound = true;
        g.leaderId = (7u << 16) | 8u;
        g.memberIds = "0:1;7:8";
        Check(Movers(g) == 6 && GroupSane(g), "group: movers by gait, sane");
        const std::string line = FormatGroup("rec", 15000, "sample", g, 3.4f);
        Check(Has(line, "GROUP_AUDIO,phase=rec,clip_ms=15000,why=sample,group=0,members=6,non_null=6,replay=6,live_player=0,"
                        "movers=6,gaits=0/0/3/3,speed=7.80,pos=-1984.5;1911.3;261.4,listener_m=3.4,sound=1,leader=7:8,ids=0:1;7:8"),
            "group: the line");
        g.members = kGroupMemberMax + 1;
        Check(!GroupSane(g), "group: more members than the array holds is torn");
        g.members = 2;
        g.speed = NAN;
        Check(!GroupSane(g), "group: a NaN speed is torn");
        GroupView empty;
        Check(Has(FormatGroup("play", 1, "inactive", empty, -1.0f), "leader=65535:0,ids=none"), "group: an empty group");
    }

    void TestBudget()
    {
        Budget b(2);
        Check(b.Take() && b.Take() && !b.Take() && b.Dropped() == 1, "budget: lines then dropped");
        b.Refill(1);
        Check(b.Take() && !b.Take() && b.Dropped() == 2, "budget: a refill keeps the dropped count");
        b.Reset(1);
        Check(b.Dropped() == 0 && b.Take(), "budget: a reset starts over");
    }

    void TestSites()
    {
        std::set<std::string> names;
        bool sizes = true, parts = true;
        int ptfx = 0, group = 0, listener = 0;
        for (const ProbeSite& site : kProbeSites)
        {
            names.insert(site.name);
            sizes = sizes && site.size > 0 && site.size <= sizeof(site.bytes);
            parts = parts && std::string(ProbePartName(site.part)) != "?";
            ptfx += site.part == ProbePart::Ptfx;
            group += site.part == ProbePart::Group;
            listener += site.part == ProbePart::Listener;
        }
        Check(names.size() == kProbeSiteCount && sizes && parts, "sites: unique names, sizes, parts");
        Check(ptfx == 17 && group == 13 && listener == 2, "sites: every part has its sites");
        Check(RipTarget7(kProbeSites[0]) == 0x5C9A660 && RipTarget7(kProbeSites[17]) == 0x5BF7EA8,
            "sites: the managers' globals");
        std::uint8_t code[8] = {0x48, 0x8D, 0x0D, 0x9C, 0xCB, 0xD7, 0x02, 0x00};
        Check(ProbeSiteMatches(kProbeSites[0], code), "sites: a match");
        code[3] = 0x9D;
        Check(!ProbeSiteMatches(kProbeSites[0], code) && !ProbeSiteMatches(kProbeSites[0], nullptr), "sites: a mismatch");
        // Field offsets the sites prove (the bytes hold the displacement).
        Check(kProbeSites[6].bytes[2] == (kPtfxSkip & 0xFF) && kProbeSites[7].bytes[2] == (kPtfxActive & 0xFF) &&
                kProbeSites[8].bytes[3] == (kPtfxSettings & 0xFF) && kProbeSites[9].bytes[4] == kPtfxSettingsMaxM &&
                kProbeSites[11].bytes[3] == (kPtfxSound & 0xFF) && kProbeSites[12].bytes[2] == (kPtfxDone & 0xFF) &&
                kProbeSites[14].bytes[2] == kPtfxSettingsMode,
            "sites: the particle slot's fields");
        Check(kProbeSites[19].bytes[3] == (kGroupStride & 0xFF) && kProbeSites[19].bytes[4] == (kGroupStride >> 8) &&
                kProbeSites[20].bytes[3] == kGroupFirst && kProbeSites[21].bytes[2] == kGroupCount &&
                kProbeSites[22].bytes[3] == (kGroupMeta & 0xFF) && kProbeSites[23].bytes[2] == (kGroupMemberCount & 0xFF) &&
                kProbeSites[24].bytes[3] * 8u == kGroupMembers && kProbeSites[26].bytes[3] == (kGroupMeanPos & 0xFF) &&
                kProbeSites[27].bytes[4] == (kGroupMeanSpeed & 0xFF) && kProbeSites[28].bytes[3] == (kGroupLeader & 0xFF) &&
                kProbeSites[29].bytes[3] == (kGroupSound & 0xFF),
            "sites: the group's fields");
        const float points[2][3] = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};
        const float pos[3] = {7.0f, 0.0f, 0.0f};
        Check(std::fabs(NearestListener(points, 2, pos) - 3.0f) < 1e-4f && NearestListener(points, 0, pos) < 0.0f,
            "listener: the nearest point");
    }

    void TestOwnCreatesTwin()
    {
        replay::census::OwnCreates own;
        const float here[3] = {10.0f, 0.0f, 0.0f};
        const float there[3] = {40.0f, 0.0f, 0.0f};
        own.Add(0xFFFF0000u, 0x47fd3f, 30000, here, 0xAAAA);
        own.Add(0xFFFF0000u, 0x47fd3f, 30500, there, 0xBBBB); // too far from the packet
        own.Add(0xFFFF0000u, 0x47fd3f, 31000, here, 0xCCCC);  // the newest near one
        std::uint64_t twin = 0;
        Check(own.Recent(0xFFFF0000u, 0x47fd3f, 31200, 2500, here, 10.0f, &twin) && twin == 0xCCCC,
            "twin: the newest near create is the live twin");
        Check(own.Recent(0xFFFF0000u, 0x47fd3f, 31200, 2500, here, 10.0f), "twin: without the out parameter as before");
        twin = 0;
        Check(!own.Recent(0xFFFF0000u, 0x480219, 31200, 2500, here, 10.0f, &twin) && twin == 0, "twin: another sound");
        own.Add(0xFFFF0000u, 0x1, 100, nullptr); // no sound given
        Check(own.Recent(0xFFFF0000u, 0x1, 200, 2500, nullptr, 10.0f, &twin) && twin == 0, "twin: unknown sound is 0");
    }
}

int RunSoundWorldTests()
{
    g_worldFailures = 0;
    TestSteal();
    TestYieldWatch();
    TestPtfxWatch();
    TestGroup();
    TestBudget();
    TestSites();
    TestOwnCreatesTwin();
    std::printf("  sound world: %d failed\n", g_worldFailures);
    if (g_worldFailures == 0)
        std::printf("Sound world tests passed.\n");
    return g_worldFailures;
}
