// The pure parts of the voice rules. The mixer scene track (kinds by caller,
// the recorder's live set, trim and cut of a ring window with a scene active
// across it, the mix groups in force, the playback plan and the group
// decisions, the conversation's participants), the scripted line path (source,
// pD4, duck byte, the Ask writes) and the player's scripted line route, and
// the speech level probe's helpers.
#include "mix_scene_core.h"
#include "speech_level_core.h"
#include "speech_script_path.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_voicesFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_voicesFailures;
    }

    void TestKinds()
    {
        using namespace replay::mixscene;
        // The two start sites' returns as the game has them (mix_scene.cpp finds them).
        const Callers callers{0x2FC2335, 0x31C88EB};
        Check(KindOfCaller(0x2FC2335, callers) == Kind::Conversation, "kind: the conversation duck's start site");
        Check(KindOfCaller(0x31C88EB, callers) == Kind::Script, "kind: START_AUDIO_SCENE's start site");
        Check(KindOfCaller(0x3211FE7, callers) == Kind::Code, "kind: the engine's own code");
        Check(KindOfCaller(0, Callers{}) == Kind::Code, "kind: start sites not found, every start is code");
    }

    void TestLiveAndTrack()
    {
        using namespace replay::mixscene;
        Live live;
        live.callers = Callers{0x2FC2335, 0x31C88EB};
        Track track;
        const std::uint32_t tags[2] = {0x1111u, 0x2222u};
        const Instance& convo = live.Start(0xA000, kConversationScene, 0x2FC2335, 1000, tags, 2);
        TrackStart(track, convo);
        const Instance& weather = live.Start(0xB000, 0x6BF72A2u, 0x3211FE7u, 1200, nullptr, 0);
        TrackStart(track, weather);
        Check(track.instances.size() == 2 && track.instances[0].kind == Kind::Conversation && track.instances[0].tagCount == 2 &&
                  track.instances[0].tags[1] == 0x2222u,
            "live: a conversation start keeps its tags");
        Check(TrackStop(track, live.Stop(0xB000), 3000) && track.instances[1].stopMs == 3000, "live: a stop closes its instance");
        Check(!TrackStop(track, live.Stop(0xC000), 3100) && track.lostStops == 1, "live: an unknown stop is counted");
        Check(live.open.size() == 1, "live: one scene still open");
        // A fresh recording (the track cleared) adopts the open scene.
        Track fresh;
        Check(TrackEnsureOpen(fresh, live, 5000) == 1 && fresh.instances[0].startMs == 5000 &&
                  fresh.instances[0].stopMs == kOpen && fresh.instances[0].id == convo.id,
            "live: a fresh recording starts with the scenes still active");
        Check(TrackEnsureOpen(fresh, live, 6000) == 0, "live: adopted once");
    }

    void TestTrimAndCut()
    {
        using namespace replay::mixscene;
        Track track;
        Instance early;
        early.id = 1;
        early.kind = Kind::Script;
        early.startMs = 100;
        early.stopMs = 900;
        Instance across;
        across.id = 2;
        across.kind = Kind::Conversation;
        across.startMs = 500;
        across.stopMs = kOpen;
        across.tagCount = 1;
        across.tags[0] = 0x77u;
        Instance late;
        late.id = 3;
        late.kind = Kind::Script;
        late.startMs = 4000;
        late.stopMs = 4500;
        track.instances = {early, across, late};
        track.groups = {{200, 10, 0x77u}, {300, 11, 0x88u}, {1500, 11, 0}, {2500, 10, 0x99u}};
        track.DropBefore(1000);
        Check(track.instances.size() == 2 && track.instances[0].id == 2 && track.instances[1].id == 3,
            "trim: an ended scene goes, an active one stays");
        Check(track.groups.size() == 4 && track.groups[0].timeMs == 1000 && track.groups[0].entity == 10 &&
                  track.groups[0].group == 0x77u && track.groups[1].timeMs == 1000 && track.groups[1].group == 0x88u,
            "trim: the groups in force move to the window start");
        Check(track.GroupAt(11, 1200) == 0x88u && track.GroupAt(11, 1600) == 0 && track.GroupAt(10, 1200) == 0x77u &&
                  track.GroupAt(10, 2600) == 0x99u,
            "trim: groups at a time");
        Track cut;
        track.CutInto(cut, 2000, 4200);
        Check(cut.instances.size() == 2 && cut.instances[0].id == 2 && cut.instances[0].startMs == 0 &&
                  cut.instances[0].stopMs == kOpen,
            "cut: a scene active across the window starts at 0");
        Check(cut.instances[1].id == 3 && cut.instances[1].startMs == 2000 && cut.instances[1].stopMs == kOpen,
            "cut: a scene still active at the window end stays open");
        Check(cut.GroupAt(10, 0) == 0x77u && cut.GroupAt(10, 600) == 0x99u && cut.GroupAt(11, 0) == 0,
            "cut: the groups at the window start, then the changes inside it");
    }

    void TestParticipant()
    {
        using namespace replay::mixscene;
        Instance convo;
        convo.kind = Kind::Conversation;
        convo.tagCount = 2;
        convo.tags[0] = 0x10u;
        convo.tags[1] = 0x20u;
        Check(Participant(convo, 0x20u) && !Participant(convo, 0x30u) && !Participant(convo, 0),
            "conversation: the speakers are its participants, everyone else is ducked");
    }

    // Hash lists, stale stops, the variables through trim and cut.
    void TestHashesAndVars()
    {
        using namespace replay::mixscene;
        Instance script;
        script.id = 5;
        script.hash = 0x914C7229u;
        script.kind = Kind::Script;
        Check(ParseHashList("0x1bfc4208, 24bfc688;0X24BFC688 123 0x 0x1FFFFFFFF") ==
                  std::vector<std::uint32_t>({0x1BFC4208u, 24u, 0x24BFC688u, 123u}),
            "hash list: hex and decimal, malformed and too big items dropped");
        Check(ParseHashList(nullptr).empty() && ParseHashList("").empty(), "hash list: empty");
        Check(StaleStop(script, 0) && !StaleStop(script, 0x914C7229u), "stop: hash 0 at the stop is a stale reference");
        Check(SceneAlive(0x914C7229u, 0x1000, 0x914C7229u) && !SceneAlive(0x914C7229u, 0, 0x914C7229u) &&
                  !SceneAlive(0, 0x1000, 0x914C7229u),
            "poll: alive while referenced with the instance's hash");
        Check(VarChanged(false, 0.0f, 0.0f) && !VarChanged(true, 0.5f, 0.50001f) && VarChanged(true, 0.5f, 0.6f),
            "vars: first sight and real changes are stored");
        // Variables through trim and cut.
        Track vt;
        Instance a;
        a.id = 1;
        a.kind = Kind::Script;
        a.startMs = 0;
        a.stopMs = kOpen;
        Instance gone;
        gone.id = 2;
        gone.kind = Kind::Code;
        gone.startMs = 0;
        gone.stopMs = 500;
        vt.instances = {a, gone};
        vt.vars = {{100, 1, 0xAAu, 0.1f}, {200, 2, 0xBBu, 1.0f}, {300, 1, 0xAAu, 0.3f}, {400, 1, 0xCCu, 2.0f}, {1500, 1, 0xAAu, 0.9f}};
        Check(vt.VarsAt(1, 350).size() == 1 && vt.VarsAt(1, 350)[0].second == 0.3f && vt.VarsAt(1, 450).size() == 2,
            "vars: values in force at a time");
        vt.DropBefore(1000);
        Check(vt.instances.size() == 1 && vt.vars.size() == 3 && vt.vars[0].timeMs == 1000 && vt.vars[1].timeMs == 1000 &&
                  vt.vars[2].timeMs == 1500,
            "vars trim: the values in force move to the window start, an ended scene's go");
        Check(vt.VarsAt(1, 1000).size() == 2 && vt.VarsAt(1, 1200)[0].second == 0.3f && vt.VarsAt(1, 1600)[0].second == 0.9f,
            "vars trim: values kept");
        Track cut;
        vt.CutInto(cut, 1200, 2000);
        Check(cut.vars.size() == 3 && cut.vars[0].timeMs == 0 && cut.vars[2].timeMs == 300 && cut.VarsAt(1, 0)[0].second == 0.3f &&
                  cut.VarsAt(1, 400)[0].second == 0.9f,
            "vars cut: the values at the window start at 0, then the changes");
    }

    void TestAsk()
    {
        using namespace replay::speech;
        request::Fields script;
        script.read = true;
        script.value[request::Source] = 2;
        script.value[request::PD4] = 0xF;
        script.value[request::LineBits] = 0x02480000u; // +0x2BE = 0x48: bit 5 clear
        request::Fields camp;
        camp.read = true;
        camp.value[request::Source] = 3;
        camp.value[request::LineBits] = 0x02680000u;
        // Set replaces an earlier write at the same offset.
        request::Ask set;
        set.Set(0xDC, 1);
        set.Set(0xDC, 2);
        Check(set.count == 1 && set.writes[0].value == 2, "ask: Set replaces");
        Check(!set.Empty(), "ask: not empty with a write");
        request::Ask bytes;
        bytes.SetByte(0x19E, 1);
        Check(!bytes.Empty() && bytes.byteCount == 1, "ask: a byte write is not empty");
    }

    void TestLevel()
    {
        using namespace replay::speech::level;
        Check(std::fabs(LinearToDb(1.0f)) < 0.001f && std::fabs(LinearToDb(0.1f) + 20.0f) < 0.01f, "level: dB of a linear volume");
        Check(LinearToDb(0.0f) == kSilentDb && LinearToDb(-1.0f) == kSilentDb, "level: silent");
        Schedule schedule;
        Check(schedule.Due(1, 0) && !schedule.Due(1, 500) && schedule.Due(1, 1000), "level: first sight, then every second");
        Check(schedule.Due(1, 2000) && schedule.Due(1, 3000) && !schedule.Due(1, 4000), "level: four samples per line at most");
        Tally tally;
        tally.Note(3, true, -40.0f, true);
        tally.Note(3, true, -20.0f, false);
        tally.Note(2, false, 0.0f, false);
        const std::string text = tally.Text();
        Check(text.find("src2:n=1,mean_db=na") != std::string::npos && text.find("src3:n=2,mean_db=-30.0,ducked=1") != std::string::npos,
            "level: the per-source totals");
    }
}

int RunVoicesTests()
{
    g_voicesFailures = 0;
    TestKinds();
    TestLiveAndTrack();
    TestTrimAndCut();
    TestParticipant();
    TestHashesAndVars();
    TestAsk();
    TestLevel();
    if (!g_voicesFailures)
        std::printf("Voices tests passed.\n");
    return g_voicesFailures;
}
